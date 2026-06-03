#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include "CCameraMacOS.h"
#include "DBG_Log.h"
#include <mutex>
#include <atomic>
#include <cstring>

@interface CCameraMacOSHelper : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
	AVCaptureSession *session;
	AVCaptureDeviceInput *deviceInput;
	AVCaptureVideoDataOutput *videoOutput;
	dispatch_queue_t captureQueue;

	std::mutex frameMutex;
	u8 *internalBuffer;
	int captureWidth;
	int captureHeight;
	std::atomic<bool> newFrame;
	std::atomic<bool> capturing;
	std::atomic<bool> authorizationPending;
}

- (instancetype)init;
- (void)dealloc;
- (NSArray<AVCaptureDevice *> *)enumerateDevices;
- (BOOL)startCaptureWithDevice:(AVCaptureDevice *)device width:(int)w height:(int)h;
- (BOOL)startSessionWithDevice:(AVCaptureDevice *)device width:(int)w height:(int)h;
- (void)stopCapture;
- (BOOL)isCapturing;
- (BOOL)isNewFrameReady;
- (BOOL)getFrameRGBA:(u8 *)destBuffer width:(int)bufW height:(int)bufH;
- (int)frameWidth;
- (int)frameHeight;

@end

@implementation CCameraMacOSHelper

- (instancetype)init
{
	self = [super init];
	if (self)
	{
		session = nil;
		deviceInput = nil;
		videoOutput = nil;
		captureQueue = dispatch_queue_create("com.retrodebugger.camera", DISPATCH_QUEUE_SERIAL);
		internalBuffer = NULL;
		captureWidth = 0;
		captureHeight = 0;
		newFrame = false;
		capturing = false;
		authorizationPending = false;
	}
	return self;
}

- (void)dealloc
{
	[self stopCapture];
	if (internalBuffer)
	{
		delete[] internalBuffer;
		internalBuffer = NULL;
	}
}

- (NSArray<AVCaptureDevice *> *)enumerateDevices
{
	AVCaptureDeviceDiscoverySession *discovery =
		[AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[
			AVCaptureDeviceTypeBuiltInWideAngleCamera,
			AVCaptureDeviceTypeExternal
		]
		mediaType:AVMediaTypeVideo
		position:AVCaptureDevicePositionUnspecified];

	return discovery.devices;
}

- (BOOL)startCaptureWithDevice:(AVCaptureDevice *)device width:(int)w height:(int)h
{
	[self stopCapture];

	// Check camera authorization
	AVAuthorizationStatus authStatus = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
	if (authStatus == AVAuthorizationStatusNotDetermined)
	{
		// Request access asynchronously — cannot block main thread or the dialog won't appear
		authorizationPending = true;
		LOGD("CCameraMacOS: requesting camera authorization...");
		[AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
			authorizationPending = false;
			if (granted)
			{
				LOGD("CCameraMacOS: camera access granted, starting capture");
				dispatch_async(dispatch_get_main_queue(), ^{
					[self startSessionWithDevice:device width:w height:h];
				});
			}
			else
			{
				LOGD("CCameraMacOS: camera access denied by user");
			}
		}];
		// Return YES to indicate "in progress" — capturing will be set when session starts
		return YES;
	}
	else if (authStatus != AVAuthorizationStatusAuthorized)
	{
		LOGD("CCameraMacOS: camera access not authorized (status=%d)", (int)authStatus);
		return NO;
	}

	return [self startSessionWithDevice:device width:w height:h];
}

- (BOOL)startSessionWithDevice:(AVCaptureDevice *)device width:(int)w height:(int)h
{
	session = [[AVCaptureSession alloc] init];

	if (w <= 640 && h <= 480)
		session.sessionPreset = AVCaptureSessionPreset640x480;
	else if (w <= 1280 && h <= 720)
		session.sessionPreset = AVCaptureSessionPreset1280x720;
	else
		session.sessionPreset = AVCaptureSessionPreset1920x1080;

	NSError *error = nil;
	deviceInput = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
	if (!deviceInput || error)
	{
		LOGD("CCameraMacOS: failed to create device input: %s", [[error localizedDescription] UTF8String]);
		session = nil;
		return NO;
	}

	if (![session canAddInput:deviceInput])
	{
		LOGD("CCameraMacOS: cannot add input to session");
		session = nil;
		deviceInput = nil;
		return NO;
	}
	[session addInput:deviceInput];

	videoOutput = [[AVCaptureVideoDataOutput alloc] init];
	videoOutput.videoSettings = @{
		(NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
	};
	videoOutput.alwaysDiscardsLateVideoFrames = YES;
	[videoOutput setSampleBufferDelegate:self queue:captureQueue];

	if (![session canAddOutput:videoOutput])
	{
		LOGD("CCameraMacOS: cannot add output to session");
		session = nil;
		deviceInput = nil;
		videoOutput = nil;
		return NO;
	}
	[session addOutput:videoOutput];

	[session startRunning];
	capturing = true;
	return YES;
}

- (void)stopCapture
{
	if (session)
	{
		[session stopRunning];
		session = nil;
		deviceInput = nil;
		videoOutput = nil;
	}
	capturing = false;
	newFrame = false;
}

- (BOOL)isCapturing
{
	return capturing.load();
}

- (BOOL)isNewFrameReady
{
	return newFrame.load();
}

- (int)frameWidth
{
	return captureWidth;
}

- (int)frameHeight
{
	return captureHeight;
}

- (BOOL)getFrameRGBA:(u8 *)destBuffer width:(int)bufW height:(int)bufH
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (!internalBuffer || captureWidth != bufW || captureHeight != bufH)
		return NO;

	memcpy(destBuffer, internalBuffer, bufW * bufH * 4);
	newFrame = false;
	return YES;
}

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
	   fromConnection:(AVCaptureConnection *)connection
{
	CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (!imageBuffer)
		return;

	CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

	int w = (int)CVPixelBufferGetWidth(imageBuffer);
	int h = (int)CVPixelBufferGetHeight(imageBuffer);
	size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
	u8 *baseAddress = (u8 *)CVPixelBufferGetBaseAddress(imageBuffer);

	{
		std::lock_guard<std::mutex> lock(frameMutex);

		if (captureWidth != w || captureHeight != h)
		{
			if (internalBuffer)
				delete[] internalBuffer;
			captureWidth = w;
			captureHeight = h;
			internalBuffer = new u8[w * h * 4];
		}

		// Convert BGRA -> RGBA
		for (int y = 0; y < h; y++)
		{
			u8 *srcRow = baseAddress + y * bytesPerRow;
			u8 *dstRow = internalBuffer + y * w * 4;
			for (int x = 0; x < w; x++)
			{
				int si = x * 4;
				int di = x * 4;
				dstRow[di + 0] = srcRow[si + 2]; // R
				dstRow[di + 1] = srcRow[si + 1]; // G
				dstRow[di + 2] = srcRow[si + 0]; // B
				dstRow[di + 3] = srcRow[si + 3]; // A
			}
		}

		newFrame = true;
	}

	CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

// ---- C++ wrapper implementation ----

CCameraMacOS::CCameraMacOS()
{
	captureHelper = (void *)CFBridgingRetain([[CCameraMacOSHelper alloc] init]);
}

CCameraMacOS::~CCameraMacOS()
{
	CCameraMacOSHelper *helper = (__bridge_transfer CCameraMacOSHelper *)captureHelper;
	[helper stopCapture];
}

std::vector<CCameraDevice> CCameraMacOS::EnumerateDevices()
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	NSArray<AVCaptureDevice *> *avDevices = [helper enumerateDevices];

	std::vector<CCameraDevice> result;
	for (int i = 0; i < (int)[avDevices count]; i++)
	{
		AVCaptureDevice *dev = avDevices[i];
		CCameraDevice d;
		d.index = i;
		strncpy(d.name, [[dev localizedName] UTF8String], sizeof(d.name) - 1);
		d.name[sizeof(d.name) - 1] = '\0';
		strncpy(d.uniqueId, [[dev uniqueID] UTF8String], sizeof(d.uniqueId) - 1);
		d.uniqueId[sizeof(d.uniqueId) - 1] = '\0';
		result.push_back(d);
	}
	return result;
}

bool CCameraMacOS::StartCapture(int deviceIndex, int requestedWidth, int requestedHeight)
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	NSArray<AVCaptureDevice *> *avDevices = [helper enumerateDevices];

	if (deviceIndex < 0 || deviceIndex >= (int)[avDevices count])
		return false;

	return [helper startCaptureWithDevice:avDevices[deviceIndex] width:requestedWidth height:requestedHeight];
}

void CCameraMacOS::StopCapture()
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	[helper stopCapture];
}

bool CCameraMacOS::IsCapturing()
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	return [helper isCapturing];
}

bool CCameraMacOS::IsNewFrameReady()
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	return [helper isNewFrameReady];
}

bool CCameraMacOS::GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight)
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	return [helper getFrameRGBA:destBuffer width:bufferWidth height:bufferHeight];
}

int CCameraMacOS::GetFrameWidth()
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	return [helper frameWidth];
}

int CCameraMacOS::GetFrameHeight()
{
	CCameraMacOSHelper *helper = (__bridge CCameraMacOSHelper *)captureHelper;
	return [helper frameHeight];
}

CCameraCapture *SYS_CreatePlatformCameraCapture()
{
	return new CCameraMacOS();
}
