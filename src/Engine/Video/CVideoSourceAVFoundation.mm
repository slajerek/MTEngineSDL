#include "CVideoSourceAVFoundation.h"

// Guarded on the CAPABILITY as well as the platform: an app that resolves
// MT_CAP_VIDEO_PLAYBACK=0 (c64d) must not be made to link AVFoundation for a
// source it never constructs.
#if defined(__APPLE__) && (!defined(MT_CAP_VIDEO_PLAYBACK) || MT_CAP_VIDEO_PLAYBACK)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include "DBG_Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

struct CVideoSourceAVFoundation::SImpl
{
	AVURLAsset *asset = nil;
	AVAssetTrack *videoTrack = nil;
	AVAssetTrack *audioTrack = nil;
	AVAssetReader *reader = nil;
	AVAssetReaderTrackOutput *videoOut = nil;
	AVAssetReaderAudioMixOutput *audioOut = nil;
	bool use10Bit = false;
};

// ============================================================================
// Extension probe
//
// The set AVFoundation demuxes, deliberately NOT the set the apps accept.
// One app also accepts .mkv/.avi/.mts/.m2ts/.ts/.mpg/.mpeg/.wmv/.asf and
// AVFoundation opens none of them -- those route elsewhere. Answering "yes"
// here for a container AVFoundation cannot read would turn a routable file
// into a hard failure.
// ============================================================================
bool CVideoSourceAVFoundation::CanProbablyOpen(const char *filePathIn)
{
	if (filePathIn == NULL || filePathIn[0] == '\0')
		return false;

	std::string p(filePathIn);
	size_t dot = p.find_last_of('.');
	if (dot == std::string::npos)
		return false;

	std::string ext = p.substr(dot);
	for (char &c : ext)
		c = (char)tolower((unsigned char)c);

	return ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".qt" ||
		   ext == ".3gp" || ext == ".3g2" || ext == ".m4a";
}

CVideoSourceAVFoundation::CVideoSourceAVFoundation()
{
	impl = new SImpl();
}

CVideoSourceAVFoundation::~CVideoSourceAVFoundation()
{
	Close();
	delete impl;
	impl = nullptr;
}

// ============================================================================
// Codec name
//
// The engine's contract is FFmpeg's spelling ("h264", "hevc", "prores") --
// SVideoInfo::videoCodecName is user-visible in apps' info overlays and
// several tests assert it literally, so a source that reported FourCCs would
// change shipped strings. AVFoundation gives the FourCC; this is the mapping.
// ============================================================================
static std::string CodecNameFromFourCC(FourCharCode c)
{
	switch (c)
	{
		case kCMVideoCodecType_H264:             return "h264";
		case kCMVideoCodecType_HEVC:             return "hevc";
		case kCMVideoCodecType_MPEG4Video:       return "mpeg4";
		case kCMVideoCodecType_MPEG2Video:       return "mpeg2video";
		case kCMVideoCodecType_JPEG:             return "mjpeg";
		case kCMVideoCodecType_AppleProRes422:
		case kCMVideoCodecType_AppleProRes422HQ:
		case kCMVideoCodecType_AppleProRes422LT:
		case kCMVideoCodecType_AppleProRes422Proxy:
		case kCMVideoCodecType_AppleProRes4444:  return "prores";
		default: break;
	}
	// 'hvc1' and 'hev1' both mean HEVC; the constant above covers 'hvc1'.
	if (c == 'hev1') return "hevc";
	if (c == 'avc1') return "h264";
	char s[5] = { (char)((c >> 24) & 0xFF), (char)((c >> 16) & 0xFF),
				  (char)((c >> 8) & 0xFF),  (char)(c & 0xFF), 0 };
	return std::string(s);
}

static std::string AudioCodecNameFromFourCC(FourCharCode c)
{
	switch (c)
	{
		case kAudioFormatMPEG4AAC:
		case kAudioFormatMPEG4AAC_HE:
		case kAudioFormatMPEG4AAC_HE_V2:  return "aac";
		case kAudioFormatMPEGLayer3:      return "mp3";
		case kAudioFormatLinearPCM:       return "pcm";
		case kAudioFormatAppleLossless:   return "alac";
		case kAudioFormatAC3:             return "ac3";
		default: break;
	}
	char s[5] = { (char)((c >> 24) & 0xFF), (char)((c >> 16) & 0xFF),
				  (char)((c >> 8) & 0xFF),  (char)(c & 0xFF), 0 };
	return std::string(s);
}

// ============================================================================
// Colour
//
// SVideoInfo carries FFmpeg's numbering (raw AVCOL_PRI_*/AVCOL_TRC_*, and
// colorSpace normalized to the VPX_CS_* convention). AVFoundation reports the
// same information as CFString constants, so these are the two mapping tables
// Phase 1 flagged as owed -- without them an HDR clip would arrive as
// "unspecified" and the apps' LUT gate, which keys on colorTrc 16/18,
// would silently treat a PQ or HLG clip as SDR.
// ============================================================================
static int TrcFromCFString(CFStringRef s)
{
	if (s == NULL) return 2;                                    // unspecified
	if (CFEqual(s, kCVImageBufferTransferFunction_ITU_R_709_2))  return 1;
	if (CFEqual(s, kCVImageBufferTransferFunction_SMPTE_240M_1995)) return 7;
	if (CFEqual(s, kCVImageBufferTransferFunction_sRGB))         return 13;
	if (CFEqual(s, kCVImageBufferTransferFunction_ITU_R_2100_HLG)) return 18;  // ARIB STD-B67
	if (CFEqual(s, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ)) return 16; // SMPTE 2084
	if (CFEqual(s, kCVImageBufferTransferFunction_Linear))       return 8;
	return 2;
}

static int PrimariesFromCFString(CFStringRef s)
{
	if (s == NULL) return 2;
	if (CFEqual(s, kCVImageBufferColorPrimaries_ITU_R_709_2)) return 1;
	if (CFEqual(s, kCVImageBufferColorPrimaries_EBU_3213))    return 5;
	if (CFEqual(s, kCVImageBufferColorPrimaries_SMPTE_C))     return 6;
	if (CFEqual(s, kCVImageBufferColorPrimaries_ITU_R_2020))  return 9;
	return 2;
}

// Returns an AVCOL_SPC_* value, because VideoColor_NormalizeMatrix() takes the
// FFmpeg convention and the two numberings disagree (AVCOL_SPC_BT709 == 1 but
// VPX_CS_BT_709 == 2 -- an inversion that shipped for months once already).
static int AvcolMatrixFromCFString(CFStringRef s)
{
	if (s == NULL) return 2;                                      // AVCOL_SPC_UNSPECIFIED
	if (CFEqual(s, kCVImageBufferYCbCrMatrix_ITU_R_709_2))   return 1;   // BT709
	if (CFEqual(s, kCVImageBufferYCbCrMatrix_ITU_R_601_4))   return 6;   // BT470BG/SMPTE170M family
	if (CFEqual(s, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995)) return 7;
	if (CFEqual(s, kCVImageBufferYCbCrMatrix_ITU_R_2020))    return 9;   // BT2020_NCL
	return 2;
}

// ============================================================================

bool CVideoSourceAVFoundation::FillInfoFromAsset()
{
	AVAssetTrack *vt = impl->videoTrack;
	if (vt == nil)
	{
		errorReason = "no video track";
		return false;
	}

	CGSize natural = vt.naturalSize;
	info.width  = (int)llround(natural.width);
	info.height = (int)llround(natural.height);
	info.duration = CMTimeGetSeconds(impl->asset.duration);
	if (!(info.duration > 0.0) || std::isnan(info.duration))
		info.duration = 0.0;
	info.fps = vt.nominalFrameRate > 0.0f ? (double)vt.nominalFrameRate : 0.0;

	// ROTATION. FFmpeg reports av_display_rotation_get() UNNEGATED, and
	// SVideoInfo's comment pins that convention against
	// tests/fixtures/video/h264_rot90.mp4 -- apps both display this and
	// writes it back into user files, so a sign error here corrupts data rather
	// than merely tilting a preview.
	//
	// preferredTransform is the CGAffineTransform that maps the stored frame to
	// display orientation, and atan2(b, a) is its rotation. That is the same
	// quantity and the same sign as the display matrix FFmpeg reads.
	//
	// NOT the same in one respect, and it is recorded rather than papered over:
	// FFmpeg composes the track matrix with the movie-level `mvhd` matrix,
	// AVFoundation exposes only the track's. The two disagree on clips with a
	// non-identity mvhd -- 138 such captures exist in the app corpora.
	// CTestVideoSourceAVF pins the common case against the rotation fixtures.
	// THE SIGN IS NEGATED, and it is measured rather than reasoned about.
	// preferredTransform maps stored -> display; av_display_rotation_get()
	// reports the opposite direction, and SVideoInfo's contract is FFmpeg's
	// value. Against the four rotation fixtures:
	//
	//     fixture          ffprobe rotation      preferredTransform atan2
	//     h264_rot0.mp4    (absent -> 0)          0
	//     h264_rot90.mp4    90                  -90
	//     h264_rot180.mp4  -180  (== 180)        180
	//     h264_rot270.mp4   -90  (== 270)         90
	//
	// so engine == -atan2, normalised into [0,360). Getting this backwards is
	// not a tilted preview: apps write rotationDegrees back into the
	// user's file through a tkhd matrix patch, so the wrong sign corrupts data.
	// It shipped wrong here once already and two tests caught it --
	// CTestVideoDecode's corner check and CTestVideoOrientationWrite's
	// display-dimension check (48x64 where 64x48 was expected).
	CGAffineTransform t = vt.preferredTransform;
	double angle = -atan2((double)t.b, (double)t.a) * 180.0 / M_PI;
	int rot = (int)llround(angle);
	rot = ((rot % 360) + 360) % 360;
	// Snap to the four legal values; a matrix carrying a flip or a shear has no
	// representation in SVideoInfo, and reporting a near value is better than a
	// wild one.
	static const int kSnap[] = { 0, 90, 180, 270, 360 };
	int best = 0, bestD = 1 << 30;
	for (int cand : kSnap)
	{
		int d = abs(rot - cand);
		if (d < bestD) { bestD = d; best = cand % 360; }
	}
	info.rotationDegrees = best;

	info.videoTrackId = (int)vt.trackID;

	CMFormatDescriptionRef fd = NULL;
	NSArray *fds = vt.formatDescriptions;
	if (fds.count > 0)
		fd = (__bridge CMFormatDescriptionRef)fds[0];

	if (fd != NULL)
	{
		info.videoCodecName = CodecNameFromFourCC(CMFormatDescriptionGetMediaSubType(fd));

		CFStringRef trc  = (CFStringRef)CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_TransferFunction);
		CFStringRef pri  = (CFStringRef)CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_ColorPrimaries);
		CFStringRef mtx  = (CFStringRef)CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_YCbCrMatrix);
		info.colorTrc       = TrcFromCFString(trc);
		info.colorPrimaries = PrimariesFromCFString(pri);
		info.colorSpace     = VideoColor_NormalizeMatrix(AvcolMatrixFromCFString(mtx), info.width, info.height);

		CFBooleanRef fullRange = (CFBooleanRef)CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_FullRangeVideo);
		info.fullRange = (fullRange != NULL && CFBooleanGetValue(fullRange));

		// 10-bit output only when the source is HDR. Asking VideoToolbox for a
		// 10-bit surface on 8-bit content costs bandwidth for no picture, and
		// asking for 8 bits on PQ/HLG content quantises the shadows visibly --
		// the defect the engine elsewhere describes as "actively destroying the
		// picture".
		impl->use10Bit = (info.colorTrc == 16 || info.colorTrc == 18);
	}

	NSDate *created = nil;
	for (AVMetadataItem *item in impl->asset.commonMetadata)
	{
		if ([item.commonKey isEqualToString:AVMetadataCommonKeyCreationDate])
		{
			if ([item.value isKindOfClass:[NSDate class]])
				created = (NSDate *)item.value;
			else if ([item.stringValue length] > 0)
			{
				NSISO8601DateFormatter *f = [[NSISO8601DateFormatter alloc] init];
				created = [f dateFromString:item.stringValue];
			}
			break;
		}
	}
	if (created == nil)
		created = impl->asset.creationDate.dateValue;
	info.creationTime = created ? (time_t)[created timeIntervalSince1970] : 0;

	if (impl->audioTrack != nil)
	{
		info.hasAudio = true;
		NSArray *afds = impl->audioTrack.formatDescriptions;
		if (afds.count > 0)
		{
			CMAudioFormatDescriptionRef afd = (__bridge CMAudioFormatDescriptionRef)afds[0];
			const AudioStreamBasicDescription *asbd = CMAudioFormatDescriptionGetStreamBasicDescription(afd);
			if (asbd != NULL)
			{
				info.audioChannels   = (int)asbd->mChannelsPerFrame;
				info.audioSampleRate = (u32)llround(asbd->mSampleRate);
				info.audioCodecName  = AudioCodecNameFromFourCC(asbd->mFormatID);
			}
		}
		// The reader is asked for interleaved S16 at the source rate, so what
		// ReadAudio hands back matches these fields exactly.
		if (info.audioChannels <= 0) info.audioChannels = 2;
		if (info.audioSampleRate == 0) info.audioSampleRate = 44100;
	}

	return true;
}

// ============================================================================
// Reader construction
//
// AVAssetReader is single-pass and cannot seek: seeking means building a new
// one over a timeRange. That is the whole seek implementation, and it is why
// this is a separate function called from both Open() and Seek().
// ============================================================================
bool CVideoSourceAVFoundation::BuildReader(double startSeconds)
{
	TeardownReader();
	// A rebuild is the point at which a requested cancel has been honoured.
	cancelRequested.store(false, std::memory_order_release);

	NSError *err = nil;
	impl->reader = [[AVAssetReader alloc] initWithAsset:impl->asset error:&err];
	if (impl->reader == nil)
	{
		errorReason = std::string("AVAssetReader init failed: ") +
					  (err ? err.localizedDescription.UTF8String : "unknown");
		return false;
	}

	if (startSeconds > 0.0)
	{
		CMTime start = CMTimeMakeWithSeconds(startSeconds, 600);
		impl->reader.timeRange = CMTimeRangeMake(start, kCMTimePositiveInfinity);
	}

	OSType pixFmt = impl->use10Bit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
								   : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	NSDictionary *videoSettings = @{
		(id)kCVPixelBufferPixelFormatTypeKey : @(pixFmt),
		// Ask for CPU-addressable memory: SDecodedVideoFrame is borrowed CPU
		// planes, so an IOSurface-backed buffer would only be copied out anyway.
		(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
	};
	impl->videoOut = [[AVAssetReaderTrackOutput alloc] initWithTrack:impl->videoTrack
													 outputSettings:videoSettings];
	impl->videoOut.alwaysCopiesSampleData = NO;
	if ([impl->reader canAddOutput:impl->videoOut])
		[impl->reader addOutput:impl->videoOut];
	else
	{
		errorReason = "AVAssetReader refused the video output (unsupported codec in this container)";
		TeardownReader();
		return false;
	}

	if (impl->audioTrack != nil && !openVideoOnly)
	{
		// THE OS DECODES THE AUDIO TOO. This is what keeps PCM, MP3, AAC, ALAC
		// and AC-3 working without FFmpeg: LPCM output settings make
		// AVFoundation decode whatever the track holds into the one format the
		// engine's mixer takes.
		NSDictionary *audioSettings = @{
			AVFormatIDKey            : @(kAudioFormatLinearPCM),
			AVLinearPCMBitDepthKey   : @16,
			AVLinearPCMIsFloatKey    : @NO,
			AVLinearPCMIsBigEndianKey: @NO,
			AVLinearPCMIsNonInterleaved : @NO,
			AVSampleRateKey          : @((double)info.audioSampleRate),
			AVNumberOfChannelsKey    : @(info.audioChannels),
		};
		impl->audioOut = [[AVAssetReaderAudioMixOutput alloc]
							initWithAudioTracks:@[impl->audioTrack]
								  audioSettings:audioSettings];
		impl->audioOut.alwaysCopiesSampleData = NO;
		if ([impl->reader canAddOutput:impl->audioOut])
			[impl->reader addOutput:impl->audioOut];
		else
			impl->audioOut = nil;   // video still plays; audio is the optional half
	}

	if (![impl->reader startReading])
	{
		errorReason = std::string("AVAssetReader startReading failed: ") +
					  (impl->reader.error ? impl->reader.error.localizedDescription.UTF8String : "unknown");
		TeardownReader();
		return false;
	}
	return true;
}

void CVideoSourceAVFoundation::TeardownReader()
{
	AVAssetReader *doomed = nil;
	{
		std::lock_guard<std::mutex> lock(readerMutex);
		doomed = impl->reader;
		impl->reader   = nil;
		impl->videoOut = nil;
		impl->audioOut = nil;
	}
	// Cancel OUTSIDE the lock and on a pointer nobody else can still reach:
	// WakeAbort now sees nil and does nothing, and ARC keeps `doomed` alive
	// until this scope ends.
	if (doomed != nil && doomed.status == AVAssetReaderStatusReading)
		[doomed cancelReading];
}

bool CVideoSourceAVFoundation::Open(const char *filePathIn)
{
	Close();
	errorReason.clear();
	info = SVideoInfo();

	if (filePathIn == NULL || filePathIn[0] == '\0')
	{
		errorReason = "empty path";
		return false;
	}
	filePath = filePathIn;

	@autoreleasepool
	{
		NSString *path = [NSString stringWithUTF8String:filePathIn];
		NSURL *url = [NSURL fileURLWithPath:path];
		impl->asset = [AVURLAsset URLAssetWithURL:url options:nil];
		if (impl->asset == nil)
		{
			errorReason = "AVURLAsset could not be created";
			return false;
		}

		NSArray<AVAssetTrack *> *vts = [impl->asset tracksWithMediaType:AVMediaTypeVideo];
		// TRACK SELECTION. FFmpeg uses av_find_best_stream, which prefers the
		// track marked enabled and with the most pixels; AVFoundation's array
		// order is container order. Picking the first ENABLED track matches the
		// common case and the two aux-track fixtures
		// (h264_aux_track.mp4 carries a second `vide` track with track_enabled
		// clear, and the engine must display the same one the rotation writer
		// patches).
		for (AVAssetTrack *t in vts)
		{
			if (t.isEnabled) { impl->videoTrack = t; break; }
		}
		if (impl->videoTrack == nil && vts.count > 0)
			impl->videoTrack = vts[0];

		if (impl->videoTrack == nil)
		{
			errorReason = "no video track in this container";
			return false;
		}

		NSArray<AVAssetTrack *> *ats = [impl->asset tracksWithMediaType:AVMediaTypeAudio];
		for (AVAssetTrack *t in ats)
		{
			if (t.isEnabled) { impl->audioTrack = t; break; }
		}
		if (impl->audioTrack == nil && ats.count > 0)
			impl->audioTrack = ats[0];

		if (!FillInfoFromAsset())
			return false;

		if (openVideoOnly)
			return true;   // metadata only: no reader, no decode

		return BuildReader(0.0);
	}
}

bool CVideoSourceAVFoundation::ReadVideoFrame(SDecodedVideoFrame &out)
{
	errorReason.clear();
	if (impl->reader == nil || impl->videoOut == nil)
	{
		errorReason = "read before open";
		return false;
	}
	if (IsAborted())
		return false;   // abort: false + EMPTY errorReason, per the interface contract

	@autoreleasepool
	{
		AVAssetReaderTrackOutput *vo = nil;
		{
			std::lock_guard<std::mutex> lock(readerMutex);
			vo = impl->videoOut;
		}
		if (vo == nil)
			return false;   // torn down under us: EOS/abort, never an error

		CMSampleBufferRef sb = NULL;
		CVImageBufferRef pb = NULL;
		while (true)
		{
		if (IsAborted())
			return false;
		sb = [vo copyNextSampleBuffer];
		if (sb == NULL)
		{
			// Three outcomes share this exit and the interface distinguishes
			// them by errorReason: abort (empty, caller re-checks the
			// predicate), clean EOS (empty), genuine failure (set).
			if (IsAborted())
				return false;
			AVAssetReader *r = nil;
			{
				std::lock_guard<std::mutex> lock(readerMutex);
				r = impl->reader;
			}
			// A CANCELLED reader is an abort, not a failure: WakeAbort cancels,
			// and reporting that as an error would latch CVideoPlayer into
			// EVideoPlayerState::Error on an ordinary scrub or close.
			if (r != nil && r.status == AVAssetReaderStatusFailed)
			{
				errorReason = std::string("AVAssetReader failed: ") +
							  (r.error ? r.error.localizedDescription.UTF8String : "unknown");
			}
			return false;   // EOS otherwise
		}

		pb = CMSampleBufferGetImageBuffer(sb);
		if (pb == NULL)
		{
			// AVFoundation emits marker sample buffers carrying no image (the
			// passthrough spike measured them). Skip and take the next one.
			//
			// A LOOP, not the recursive call this first was: a run of markers
			// would have recursed once per marker and overflowed the stack, and
			// nothing bounds how many a container may hold.
			CFRelease(sb);
			sb = NULL;
			continue;
		}

		break;   // got a real image buffer
		}

		CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
		// Clip-relative already: AVFoundation reports track time, and these
		// containers start at 0 (the startTime translation the contract
		// describes is the identity here -- see Seek()).
		lastVideoPts = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) : 0.0;

		CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

		int yW = (int)CVPixelBufferGetWidthOfPlane(pb, 0);
		int yH = (int)CVPixelBufferGetHeightOfPlane(pb, 0);
		int ySrcStride = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
		const u8 *ySrc = (const u8 *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);

		int cW = (int)CVPixelBufferGetWidthOfPlane(pb, 1);
		int cH = (int)CVPixelBufferGetHeightOfPlane(pb, 1);
		int cSrcStride = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
		const u8 *cSrc = (const u8 *)CVPixelBufferGetBaseAddressOfPlane(pb, 1);

		frameWidth = yW;
		frameHeight = yH;
		const OSType fmt = CVPixelBufferGetPixelFormatType(pb);
		frameIs10Bit = (fmt == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
						fmt == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange);

		if (!frameIs10Bit)
		{
			// 420YpCbCr8BiPlanar: plane0 8-bit Y, plane1 interleaved Cb/Cr --
			// exactly NV12, which the engine's shader and CPU paths already take.
			strideY = yW;
			planeY.resize((size_t)strideY * yH);
			for (int r = 0; r < yH; r++)
				memcpy(planeY.data() + (size_t)r * strideY, ySrc + (size_t)r * ySrcStride, (size_t)strideY);

			strideUV = cW * 2;
			planeUV.resize((size_t)strideUV * cH);
			for (int r = 0; r < cH; r++)
				memcpy(planeUV.data() + (size_t)r * strideUV, cSrc + (size_t)r * cSrcStride, (size_t)strideUV);
		}
		else
		{
			// 10-bit biplanar is MSB-justified (value << 6) and interleaved;
			// the engine's YUV420P10 is LSB-justified and 3-plane. Same shift
			// and de-interleave CVideoDecoderHEVCVT does -- kept identical on
			// purpose so both Apple paths produce byte-identical frames.
			strideY = yW * 2;
			planeY.resize((size_t)strideY * yH);
			for (int r = 0; r < yH; r++)
			{
				const u16 *srcRow = reinterpret_cast<const u16 *>(ySrc + (size_t)r * ySrcStride);
				u16 *dstRow = reinterpret_cast<u16 *>(planeY.data() + (size_t)r * strideY);
				for (int c = 0; c < yW; c++)
					dstRow[c] = (u16)(srcRow[c] >> 6);
			}

			strideC10 = cW * 2;
			planeU10.resize((size_t)strideC10 * cH);
			planeV10.resize((size_t)strideC10 * cH);
			for (int r = 0; r < cH; r++)
			{
				const u16 *srcRow = reinterpret_cast<const u16 *>(cSrc + (size_t)r * cSrcStride);
				u16 *dstU = reinterpret_cast<u16 *>(planeU10.data() + (size_t)r * strideC10);
				u16 *dstV = reinterpret_cast<u16 *>(planeV10.data() + (size_t)r * strideC10);
				for (int c = 0; c < cW; c++)
				{
					dstU[c] = (u16)(srcRow[c * 2 + 0] >> 6);
					dstV[c] = (u16)(srcRow[c * 2 + 1] >> 6);
				}
			}
		}

		CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
		CFRelease(sb);
	}

	out.width  = frameWidth;
	out.height = frameHeight;
	if (frameIs10Bit)
	{
		out.pixelFormat = EVideoPixelFormat::YUV420P10;
		out.plane[0] = planeY.data();   out.stride[0] = strideY;
		out.plane[1] = planeU10.data(); out.stride[1] = strideC10;
		out.plane[2] = planeV10.data(); out.stride[2] = strideC10;
		out.plane[3] = nullptr;         out.stride[3] = 0;
	}
	else
	{
		out.pixelFormat = EVideoPixelFormat::NV12;
		out.plane[0] = planeY.data();  out.stride[0] = strideY;
		out.plane[1] = planeUV.data(); out.stride[1] = strideUV;
		out.plane[2] = nullptr;        out.stride[2] = 0;
		out.plane[3] = nullptr;        out.stride[3] = 0;
	}
	out.pts = lastVideoPts;
	return true;
}

bool CVideoSourceAVFoundation::ReadAudio(SDecodedAudio &out)
{
	if (impl->reader == nil || impl->audioOut == nil)
		return false;
	if (IsAborted())
		return false;

	@autoreleasepool
	{
		AVAssetReaderAudioMixOutput *ao = nil;
		{
			std::lock_guard<std::mutex> lock(readerMutex);
			ao = impl->audioOut;
		}
		if (ao == nil)
			return false;
		CMSampleBufferRef sb = [ao copyNextSampleBuffer];
		if (sb == NULL)
			return false;   // EOS / abort / no audio pending

		CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
		double ptsSec = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) : 0.0;

		AudioBufferList abl;
		CMBlockBufferRef block = NULL;
		OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
			sb, NULL, &abl, sizeof(abl), NULL, NULL,
			kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
		if (st != noErr || abl.mNumberBuffers == 0)
		{
			if (block) CFRelease(block);
			CFRelease(sb);
			return false;
		}

		const AudioBuffer &buf = abl.mBuffers[0];
		size_t sampleCount = buf.mDataByteSize / sizeof(s16);
		audioPcm.resize(sampleCount);
		if (sampleCount > 0 && buf.mData != NULL)
			memcpy(audioPcm.data(), buf.mData, sampleCount * sizeof(s16));

		CFRelease(block);
		CFRelease(sb);

		out.pcm        = audioPcm;
		out.channels   = info.audioChannels;
		out.sampleRate = info.audioSampleRate;
		out.pts        = ptsSec;
	}
	return true;
}

bool CVideoSourceAVFoundation::Seek(double seconds)
{
	errorReason.clear();
	if (impl->asset == nil)
	{
		errorReason = "seek before open";
		return false;
	}

	// CLEAR THE CANCEL FLAG FIRST -- a Seek IS the new work.
	//
	// CVideoPlayer submits a seek by publishing the new generation and THEN
	// calling WakeAbort(), so by the time this runs the flag is set on our
	// behalf: it means "abandon the read that was in flight", not "abandon the
	// seek you are about to do". Honouring it here made every seek return false
	// -- "source seek failed for time 3.000" -- and with it every
	// SeekPausedAndDecodeFrame, the swap-sync pump and SmartNav's loop wrap.
	//
	// The abort predicate is the authority on whether THIS seek is already
	// superseded, because unlike the flag it is generation-scoped; it is checked
	// immediately below, after the flag is cleared.
	cancelRequested.store(false, std::memory_order_release);

	if (abortPredicate && abortPredicate())
		return false;

	// Clip-relative clamp FIRST, per the interface's clip-relative contract.
	// AVFoundation containers start at 0, so there is no startTime offset to
	// add afterwards -- the translation point exists but is the identity here.
	double target = seconds;
	if (target < 0.0) target = 0.0;
	if (info.duration > 0.0 && target > info.duration) target = info.duration;

	// STREAM HEAD. Rebuilding with a timeRange starting at 0 is the same as no
	// range at all, and passing a zero-length or zero-start range has bitten
	// other implementations into skipping the opening frame -- so seek-to-0 is
	// an explicit full rebuild.
	if (!BuildReader(target > 0.0 ? target : 0.0))
		return false;

	lastVideoPts = target;
	return true;
}

void CVideoSourceAVFoundation::SetAbortPredicate(std::function<bool()> pred)
{
	abortPredicate = std::move(pred);
}

void CVideoSourceAVFoundation::WakeAbort()
{
	// THIS DELIBERATELY DOES NOT CANCEL THE READER, and the reason is a crash.
	//
	// AVAssetReader has no interrupt hook: copyNextSampleBuffer blocks and the
	// only primitive is cancelReading, which is destructive. Calling it from
	// this thread while the decode thread sits inside copyNextSampleBuffer tears
	// down the reader's internals underneath that call, and the read faults --
	// measured, as EXC_BAD_ACCESS in _pthread_cond_wait under
	// CoreMedia`WaitOnCondition, from a VideoPlayback suite case 13, which
	// cancels a seek mid-flight and then closes.
	//
	// So abort is cooperative here: raise the flag, and let the read paths honour
	// it BETWEEN reads, where no cancel can race one. The reader is then torn
	// down by whoever is already on the decode thread (Seek's rebuild, or Close).
	//
	// That is sufficient because of what WakeAbort is FOR. It exists to cut
	// short a decoder that blocks for a long time -- the Windows Media Foundation
	// HEVC pipeline waits up to TEN SECONDS per step, and FFmpeg's network I/O
	// can stall indefinitely. One AVAssetReader read of a local file returns in
	// about a millisecond, so the worst this costs is that a Close waits for a
	// single sample instead of zero. Trading a millisecond of latency for a
	// SIGSEGV is not a close call.
	//
	// The one case this does not cover is a file whose I/O really is slow -- an
	// iCloud-evicted original. The apps already refuse to open those on the
	// scan path (the dataless-file check), which is why that is a recorded
	// limit here
	// rather than a second crash.
	if (impl == nullptr)
		return;
	cancelRequested.store(true, std::memory_order_release);
}

void CVideoSourceAVFoundation::Close()
{
	if (impl != nullptr)
	{
		TeardownReader();
		impl->videoTrack = nil;
		impl->audioTrack = nil;
		impl->asset = nil;
		impl->use10Bit = false;
	}
	planeY.clear(); planeUV.clear(); planeU10.clear(); planeV10.clear();
	audioPcm.clear();
	strideY = strideUV = strideC10 = 0;
	frameWidth = frameHeight = 0;
	frameIs10Bit = false;
	lastVideoPts = 0.0;
	cancelRequested.store(false, std::memory_order_release);
	errorReason.clear();
}

#endif // __APPLE__ && MT_CAP_VIDEO_PLAYBACK
