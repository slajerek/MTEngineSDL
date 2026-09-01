#include "CRenderShaderMetal.h"
#include "CRenderShaderMaskedTileMetal.h"
#include "CRenderBackendMetal.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "CSlrImage.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_metal.h"
#import <Metal/Metal.h>
#import <AppKit/AppKit.h>   // NSScreen, for the EDR headroom poll
#import <QuartzCore/QuartzCore.h>
#include <vector>

static NSMutableArray *textures;
static CAMetalLayer *layer;
static id<MTLCommandQueue> commandQueue;
static MTLRenderPassDescriptor *renderPassDescriptor;
static id<MTLCommandBuffer> commandBuffer;
static id<MTLRenderCommandEncoder> renderEncoder;
static id<CAMetalDrawable> drawable;

// Fix 2 & 6: cache last drawable size to skip redundant layer property writes
static int lastDrawableW = 0;
static int lastDrawableH = 0;

// HEADLESS RENDERS TO AN OFFSCREEN TEXTURE, NOT A DRAWABLE.
//
// Without this, headless Metal renders NOTHING: the window is created
// SDL_WINDOW_HIDDEN, VID_PostInit() only shows it when not headless, and a
// hidden window yields either a 0x0 output size or a nil drawable -- both of
// which abort the frame below. Every capture-based test would then measure an
// empty buffer and pass. Showing the window is not an option; it would put a
// window on the maintainer's desktop on every automated run.
//
// ImGui_ImplMetal draws into whatever encoder it is given and the CAMetalDrawable
// is only needed to PRESENT, so headless allocates its own colour texture,
// encodes the frame into that, and skips presentation entirely.
static id<MTLTexture> headlessColorTexture = nil;
static int headlessW = 0, headlessH = 0;
static bool frameIsOffscreen = false;

// Staging copy for ReadFramebufferPixels(). The frame's colour attachment is
// MTLStorageModePrivate (and a drawable is write-only unless framebufferOnly is
// cleared), so capture blits into shared storage on the FRAME's command buffer
// -- ordered before the present -- and reads it back after that buffer completes.
// DEFERRED TEXTURE RELEASE.
//
// Releasing a texture the moment DeleteTexture() is called is not safe: ImGui
// draw commands already recorded this frame -- and the up-to-three frames still
// in flight -- can still reference it, and Metal's
// setFragmentTexture: on a released texture crashes rather than no-ops. The GL
// backend has VID_PostDeleteGLTexture for exactly this hazard; this is Metal's
// equivalent. Textures are parked here and released once more frames have been
// submitted than can possibly still be referencing them.
static NSMutableArray *pendingReleaseTextures = nil;
static NSMutableArray *pendingReleaseFrames = nil;   // parallel array of frame numbers
static unsigned long long metalFrameCounter = 0;
static const unsigned long long kFramesInFlight = 3;

// Park a texture for deferred release. Used by DeleteTexture() and by
// CMetalRenderTarget::Destroy() -- see each for why immediate release is unsafe.
static void MetalParkTextureForRelease(id<MTLTexture> texture);

static id<MTLTexture> captureStagingTexture = nil;
static id<MTLCommandBuffer> lastFrameCommandBuffer = nil;
static int captureW = 0, captureH = 0;

// Fix 3: triple-buffering semaphore — caps CPU ahead-of-GPU to 3 frames
static dispatch_semaphore_t kInFlightFrames;
static dispatch_once_t kInFlightFramesOnce;

CRenderBackendMetal::CRenderBackendMetal()
: CRenderBackend("Metal")
{
	textures = [[NSMutableArray alloc] init];
}

// See the header for WHY this exists. The check itself is the smallest thing
// that answers the question honestly: MTLCreateSystemDefaultDevice() is what
// SDL's Metal renderer will call, so if it returns nil there is nothing to
// build a layer on and CreateSDLWindow would SYS_FatalExit a few frames later.
//
// The device is created and released rather than cached. Holding one alive from
// a capability query would keep the GPU resident for the whole session on a
// machine that may have chosen OpenGL, and the real device is created by SDL
// anyway -- this is a question, not an allocation.
bool CRenderBackendMetal::IsAvailable()
{
	@autoreleasepool
	{
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil)
		{
			LOGError("CRenderBackendMetal::IsAvailable: MTLCreateSystemDefaultDevice() returned nil; "
					 "Metal is unavailable here. Falling back to OpenGL.");
			return false;
		}
		LOGM("CRenderBackendMetal::IsAvailable: Metal device '%s' available",
			 [[device name] UTF8String]);
	}
	return true;
}

SDL_Window *CRenderBackendMetal::CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized)
{
	// Inform SDL that we will be using metal for rendering. Without this hint initialization of metal renderer may fail.
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");

	SDL_WindowFlags windowFlags = (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
//	if (maximized)
//		windowFlags |= SDL_WINDOW_MAXIMIZED;

	// SDL3: no x/y on SDL_CreateWindow; the window is created HIDDEN, so
	// positioning it immediately afterwards is equivalent and invisible.
	mainWindow = SDL_CreateWindow(title, w, h, windowFlags);

	if (mainWindow == NULL)
	{
		SYS_FatalExit("Error creating window: %s\n", SDL_GetError());
	}
	SDL_SetWindowPosition(mainWindow, x, y);

	// SDL3 reshaped SDL_CreateRenderer completely: the driver INDEX is gone
	// (it takes a driver NAME, or NULL for "let SDL choose" -- and the
	// SDL_HINT_RENDER_DRIVER above already says metal), and both flags are
	// gone with it:
	//   SDL_RENDERER_ACCELERATED  -- removed; acceleration is the default and
	//                                software is now requested by NAME.
	//   SDL_RENDERER_PRESENTVSYNC -- replaced by SDL_SetRenderVSync(), which
	//                                is a real improvement: vsync can now be
	//                                changed after creation instead of being
	//                                baked in.
	renderer = SDL_CreateRenderer(mainWindow, NULL);
	if (renderer == NULL)
	{
		SYS_FatalExit("Error creating renderer: %s\n", SDL_GetError());
	}
	SDL_SetRenderVSync(renderer, 1);

	return mainWindow;
}

void CRenderBackendMetal::CreateRenderContext()
{

}

void CRenderBackendMetal::InitRenderPipeline()
{
	// Setup Platform/Renderer backends
	layer = (__bridge CAMetalLayer*)SDL_GetRenderMetalLayer(renderer);

	if (VID_IsHdrRequested())
	{
		// ALL THREE settings are required. wantsExtendedDynamicRangeContent on
		// its own changes nothing at all, which is the easiest way to conclude
		// the hardware cannot do HDR when it can.
		//
		// EXTENDED SRGB, NOT EXTENDED LINEAR, and this is not a compromise.
		//
		// Extended-linear declares the framebuffer values to be LINEAR LIGHT.
		// Everything this engine produces is sRGB-ENCODED: textures are
		// RGBA8Unorm rather than the _sRGB variant, so sampling returns them
		// undecoded, ImGui's fragment shader is `color * texel`, and no shader
		// converts anywhere. Declaring those numbers linear made the compositor
		// display mid-grey 0.5 as ~0.74 -- black and white are fixed points and
		// everything between lifts -- so the WHOLE application, Settings window
		// included, came out washed out and far too bright. A gamma/encoding
		// mismatch, not tone-mapping.
		//
		// Extended sRGB is the truthful description of an sRGB-encoded pipeline,
		// and it costs NOTHING: spikes/edr/edr_colorspace.mm measured both on a
		// Studio Display (potential 2.000) from a verified 1.000 baseline, and
		// they ramp identically to the same 2.000 ceiling in the same 1.75 s.
		// The colourspace does not affect the grant at all.
		//
		// Extended LINEAR remains the right long-term target -- but only once the
		// pipeline is linear end to end, which is S-5's work and which must
		// include linearising the UI chrome, not just the image data. Flipping
		// back is these two constants.
		layer.pixelFormat = MTLPixelFormatRGBA16Float;
		layer.wantsExtendedDynamicRangeContent = YES;
		CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB);
		layer.colorspace = colorSpace;
		if (colorSpace != NULL)
			CGColorSpaceRelease(colorSpace);

		LOGM("CRenderBackendMetal: HDR surface requested (RGBA16Float, extended sRGB)");
	}
	else
	{
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.wantsExtendedDynamicRangeContent = NO;
	}
	ImGui_ImplMetal_Init(layer.device);
	ImGui_ImplSDL3_InitForMetal(mainWindow);

	commandQueue = [layer.device newCommandQueue];
	renderPassDescriptor = [MTLRenderPassDescriptor new];
}

void CRenderBackendMetal::CreateFontsTexture()
{
	// No longer needed: new ImGui backend handles texture creation/updates automatically via ImGui_ImplMetal_UpdateTexture()
}

void CRenderBackendMetal::NewFrame(ImVec4 clearColor)
{
	@autoreleasepool
	{
		// Initialise the frame-pacing semaphore once (Fix 3)
		dispatch_once(&kInFlightFramesOnce, ^{
			kInFlightFrames = dispatch_semaphore_create(3);
		});

		// Null out per-frame objects so callers see a clean empty state if we abort
		commandBuffer  = nil;
		renderEncoder  = nil;
		drawable       = nil;

		// Fix 2 & 6: guard against 0×0 (hidden window / early frames / resize)
		int width, height;
		SDL_GetCurrentRenderOutputSize(renderer, &width, &height);
		if (width <= 0 || height <= 0)
		{
			// Nothing useful to render this frame; skip silently
			return;
		}

		// Only update the layer property when the size actually changed (Fix 6)
		if (lastDrawableW != width || lastDrawableH != height)
		{
			layer.drawableSize = CGSizeMake(width, height);
			lastDrawableW = width;
			lastDrawableH = height;
		}

		// Fix 3: wait for a free in-flight slot AFTER the early-abort checks so
		// we never acquire the semaphore on an aborted frame
		dispatch_semaphore_wait(kInFlightFrames, DISPATCH_TIME_FOREVER);

		// Drain textures parked by DeleteTexture() once no in-flight frame can
		// still reference them.
		metalFrameCounter++;
		if (pendingReleaseTextures != nil)
		{
			while (pendingReleaseFrames.count > 0 &&
				   metalFrameCounter - [pendingReleaseFrames[0] unsignedLongLongValue] > kFramesInFlight)
			{
				[pendingReleaseTextures removeObjectAtIndex:0];
				[pendingReleaseFrames removeObjectAtIndex:0];
			}
		}

		frameIsOffscreen = gHeadlessMode;
		if (frameIsOffscreen)
		{
			// Headless: render into our own texture. No drawable is requested,
			// so the hidden window never matters and nothing is presented.
			if (headlessColorTexture == nil || headlessW != width || headlessH != height)
			{
				MTLTextureDescriptor *d = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:layer.pixelFormat
					width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
				d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				d.storageMode = MTLStorageModePrivate;
				headlessColorTexture = [layer.device newTextureWithDescriptor:d];
				headlessW = width;
				headlessH = height;
			}
			if (headlessColorTexture == nil)
			{
				LOGError("CRenderBackendMetal::NewFrame: headless colour texture allocation failed");
				dispatch_semaphore_signal(kInFlightFrames);
				return;
			}
		}
		else
		{
			// Fix 1: nextDrawable can return nil (occluded, throttled, resize race)
			drawable = [layer nextDrawable];
			if (drawable == nil)
			{
				LOGError("CRenderBackendMetal::NewFrame: nextDrawable returned nil — skipping frame");
				// Balance the semaphore so the completion handler never fires for this frame
				dispatch_semaphore_signal(kInFlightFrames);
				return;
			}
		}

		commandBuffer = [commandQueue commandBuffer];

		renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(clearColor.x * clearColor.w, clearColor.y * clearColor.w, clearColor.z * clearColor.w, clearColor.w);

		renderPassDescriptor.colorAttachments[0].texture = frameIsOffscreen ? headlessColorTexture : drawable.texture;
		renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
		renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
		renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
		[renderEncoder pushDebugGroup:@"CRenderBackendMetal"];

		// Start the Dear ImGui frame
		ImGui_ImplMetal_NewFrame(renderPassDescriptor);
	}
}

void CRenderBackendMetal::PresentFrameBuffer(ImVec4 clearColor)
{
	@autoreleasepool
	{
		// Fix 1: if NewFrame aborted (nil drawable, 0×0 size, etc.) nothing to present.
		// Offscreen frames legitimately have no drawable.
		if (commandBuffer == nil || renderEncoder == nil)
			return;
		if (!frameIsOffscreen && drawable == nil)
			return;

		// THE SCISSOR MUST FIT THE ATTACHMENT, and only we can guarantee it.
		//
		// ImGui_ImplMetal_RenderDrawData clamps every clip rect to
		// DisplaySize * FramebufferScale -- ImGui's OWN framebuffer, taken from
		// the SDL window in POINTS. The attachment it encodes into is sized in
		// PIXELS from SDL_GetCurrentRenderOutputSize in NewFrame. Two sources,
		// two moments, and they diverge during a LIVE RESIZE, because
		// VID_Render is re-entered from inside AppKit's windowDidResize:
		// notification while the window is mid-change.
		//
		// The arithmetic lives in VID_ScissorForClipRect / the clamp below
		// rather than inline here, so a test can drive the exact conditions a
		// resize creates -- a crash that only reproduces while dragging a
		// window corner is otherwise untestable, and this one aborts inside
		// vendored ImGui code with no numbers in it.
		{
			ImDrawData *dd = ImGui::GetDrawData();
			id<MTLTexture> attachment = frameIsOffscreen ? headlessColorTexture : drawable.texture;
			if (dd != nullptr && attachment != nil)
			{
				const int attW = (int)attachment.width;
				const int attH = (int)attachment.height;

				// DIAGNOSE BEFORE fixing, and report the numbers. If a rect is
				// still bad after the clamp, the clamp was the wrong theory --
				// which is exactly what happened the first time this was
				// "fixed" from a stack trace alone.
#if defined(DEBUG) || defined(_DEBUG)
				VID_ReportBadScissors(dd, attW, attH, "pre-clamp");
#endif
				VID_ClampDrawDataToAttachment(dd, attW, attH);
#if defined(DEBUG) || defined(_DEBUG)
				const int stillBad = VID_ReportBadScissors(dd, attW, attH, "POST-clamp");
				if (stillBad > 0)
				{
					LOGError("VID scissor: %d rect(s) still bad AFTER clamping to the "
							 "attachment -- the framebuffer/attachment mismatch is NOT "
							 "the whole story here", stillBad);
				}
#endif
			}
		}

		ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);

		ImGuiIO& io = ImGui::GetIO();

		// Update and Render additional Platform Windows
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}

		[renderEncoder popDebugGroup];
		[renderEncoder endEncoding];

#if !defined(MT_ENABLE_IMGUI_TEST_ENGINE) || (MT_ENABLE_IMGUI_TEST_ENGINE)
		// Capture staging blit, encoded on the FRAME's command buffer so it is
		// ordered before the present rather than racing it. ReadFramebufferPixels()
		// then waits on this buffer. Doing it on a separate command buffer would
		// reintroduce exactly the in-flight read this exists to avoid.
		{
			id<MTLTexture> src = frameIsOffscreen ? headlessColorTexture : drawable.texture;
			if (src != nil)
			{
				if (captureStagingTexture == nil ||
					captureW != (int)src.width || captureH != (int)src.height)
				{
					MTLTextureDescriptor *d = [MTLTextureDescriptor
						texture2DDescriptorWithPixelFormat:src.pixelFormat
						width:src.width height:src.height mipmapped:NO];
					d.usage = MTLTextureUsageShaderRead;
					d.storageMode = MTLStorageModeShared;   // CPU-readable
					captureStagingTexture = [layer.device newTextureWithDescriptor:d];
					captureW = (int)src.width;
					captureH = (int)src.height;
				}
				if (captureStagingTexture != nil)
				{
					id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
					[blit copyFromTexture:src sourceSlice:0 sourceLevel:0
							 sourceOrigin:MTLOriginMake(0, 0, 0)
							   sourceSize:MTLSizeMake(src.width, src.height, 1)
								toTexture:captureStagingTexture
						 destinationSlice:0 destinationLevel:0
						destinationOrigin:MTLOriginMake(0, 0, 0)];
					[blit endEncoding];
				}
			}
		}
#endif

		if (!frameIsOffscreen)
			[commandBuffer presentDrawable:drawable];

		// Fix 3 & 4: combined completion handler — logs GPU errors and signals the
		// frame-pacing semaphore. Runs on a Metal-owned thread; both calls are safe.
		[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
			if (cb.error)
				LOGError("Metal command buffer error: %s",
						 cb.error.localizedDescription.UTF8String);
			dispatch_semaphore_signal(kInFlightFrames);
		}];

		[commandBuffer commit];
		lastFrameCommandBuffer = commandBuffer;
	}

}

void CRenderBackendMetal::ApplyDisplayColorGamut(VID_DisplayColorGamut gamut)
{
	@autoreleasepool
	{
		if (layer == nil)
		{
			return;
		}

		// HDR FIRST, and this ordering is the whole point of the branch.
		//
		// VID_Init() calls VID_ApplyMainWindowColorGamut() on the line right
		// after InitRenderPipeline(), and the SDR path below assigns
		// layer.colorspace unconditionally -- to Display P3, to sRGB, or to
		// NULL. Whichever it picked, the extended-linear colourspace set during
		// init was gone microseconds later and HDR silently never happened:
		// every call reported success and the screen was simply never brighter.
		//
		// A plain P3 or sRGB space clamps at 1.0 and discards exactly the
		// headroom this exists to reach, so the user's gamut preference is
		// MAPPED onto its extended counterpart rather than ignored. Reordering
		// the two init calls instead would have been wrong: the gamut setting is
		// a real user preference that has to keep working.
		if (VID_IsHdrRequested())
		{
			// The EXTENDED (sRGB-encoded) counterparts, matching
			// InitRenderPipeline -- see the long note there for why these are not
			// the extended-LINEAR ones. A plain P3 or sRGB space would clamp at
			// 1.0 and discard exactly the headroom this exists to reach, so the
			// user's gamut preference is still MAPPED rather than ignored.
			CGColorSpaceRef hdrColorSpace = CGColorSpaceCreateWithName(
				gamut == VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3
					? kCGColorSpaceExtendedDisplayP3
					: kCGColorSpaceExtendedSRGB);
			layer.colorspace = hdrColorSpace;
			if (hdrColorSpace != NULL)
				CGColorSpaceRelease(hdrColorSpace);
			return;
		}

		CGColorSpaceRef colorSpace = NULL;
		if (gamut == VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3)
		{
			colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
		}
		else if (gamut == VID_DISPLAY_COLOR_GAMUT_SRGB)
		{
			colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		}

		layer.colorspace = colorSpace;
		if (colorSpace != NULL)
		{
			CGColorSpaceRelease(colorSpace);
		}
	}
}

void CRenderBackendMetal::Shutdown()
{
	ImGui_ImplMetal_Shutdown();
}

// Compressed (KTX2/UASTC-transcoded) + mipmapped upload path. Fully separate
// from the RGBA path below; an RGBA image never reaches this function.
// Design note §6 / §8: logical mip dims drive the MTLRegion, physical (block-
// padded) dims drive bytesPerRow; BC7 and ASTC-4x4 are both 16 bytes / 4x4 block.
static bool MetalCreateCompressedTexture(id<MTLDevice> device,
										 NSMutableArray *texturesArray,
										 CSlrImage *image)
{
	CImageData *cd = image->loadImageData;
	if (cd == NULL || cd->compressedMips == NULL || cd->compressedMipCount <= 0)
	{
		LOGError("CRenderBackendMetal::CreateTexture: no compressed mip data");
		return false;
	}

	// EImageGpuFormat -> MTLPixelFormat. BC7 and ASTC-4x4 pixel formats were
	// introduced in macOS 11.0 / iOS 8.0; guard for the (rare) older deployment
	// target. GetPreferredCompressedFormat performs the matching capability
	// check, so this branch only fails on a genuinely incapable OS.
	MTLPixelFormat pf;
	if (@available(macOS 11.0, iOS 8.0, *))
	{
		switch (cd->compressedGpuFormat)
		{
			case IMG_GPU_ASTC_4x4:
				pf = MTLPixelFormatASTC_4x4_LDR;
				break;
			case IMG_GPU_BC7:
			default:
				pf = MTLPixelFormatBC7_RGBAUnorm;
				break;
		}
	}
	else
	{
		LOGError("CRenderBackendMetal::CreateTexture: compressed pixel formats require macOS 11.0+");
		return false;
	}

	const int mipCount = cd->compressedMipCount;

	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
																				   width:(NSUInteger)cd->width
																				  height:(NSUInteger)cd->height
																			   mipmapped:(mipCount > 1)];
	desc.mipmapLevelCount = (NSUInteger)mipCount;
	desc.usage = MTLTextureUsageShaderRead;
	// Fix 5: use Shared on Apple Silicon (unified memory) — Managed wastes double RAM there
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
	desc.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
#else
	desc.storageMode = MTLStorageModeShared;
#endif
	id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
	if (!texture)
	{
		LOGError("CRenderBackendMetal::CreateTexture: newTextureWithDescriptor returned nil (compressed)");
		return false;
	}

	// Upload every mip level. 4x4 block formats: 16 bytes/block, 4 texels/block.
	// The MTLRegion must cover the texture's own (logical) mip dimensions
	// (textureDim >> level, clamped >=1), NOT the block-padded physWidth.
	for (int level = 0; level < mipCount; level++)
	{
		const SCompressedMip &mip = cd->compressedMips[level];

		NSUInteger mipW = (NSUInteger)cd->width  >> level;
		if (mipW < 1) mipW = 1;
		NSUInteger mipH = (NSUInteger)cd->height >> level;
		if (mipH < 1) mipH = 1;

		NSUInteger blocksX = ((NSUInteger)mip.physWidth + 3) / 4;
		NSUInteger bytesPerRow = blocksX * 16;

		[texture replaceRegion:MTLRegionMake2D(0, 0, mipW, mipH)
				   mipmapLevel:(NSUInteger)level
					 withBytes:mip.blockData
				   bytesPerRow:bytesPerRow];
	}

	[texturesArray addObject:texture];
	image->texturePtr.store((__bridge void*)texture, std::memory_order_release);
	return true;
}

void CRenderBackendMetal::CreateTexture(CSlrImage *image)
{
	if (!image)
	{
		LOGError("CRenderBackendMetal::CreateTexture: image is NULL");
		return;
	}

	id<MTLDevice> device = layer.device;

	if (image->isCompressed)
	{
		MetalCreateCompressedTexture(device, textures, image);
		return;
	}

	// SOURCE DIMENSIONS COME FROM loadImageData, NOT rasterWidth/rasterHeight.
	//
	// This was a real crash: rasterWidth/rasterHeight is the POT-padded TEXTURE
	// size, while the buffer getRGBAResultData() returns is only padded on some
	// paths. LoadImage/PreloadImage allocate a padded CImageData (so the two
	// agree), but LoadImageForRebinding passes the RAW decoded buffer at its
	// original pixel size -- and uploading rasterWidth*rasterHeight from that
	// reads off the end. Metal does not tolerate it the way a driver might:
	// AGXMetal's agxaAssertBufferIsValid dereferences past the allocation and
	// the process dies with EXC_BAD_ACCESS, which is what every image-decoding
	// test did on this backend. The OpenGL path has always used the buffer's own
	// dimensions and documents exactly this (CRenderBackendOpenGL4.cpp:322-327).
	const NSUInteger srcW = (NSUInteger)image->loadImageData->width;
	const NSUInteger srcH = (NSUInteger)image->loadImageData->height;

	// The RESIDENT format decides the pixel format AND the row stride. Getting
	// the stride wrong does not fail: reading RGBA16Float at w*4 returns half a
	// row per row and looks like plausible garbage, which is exactly the shape
	// S-4 hit in the capture path.
	const bool isFloatTex = (image->residentFormat == RENDER_TEXTURE_RGBA16F);
	const NSUInteger bytesPerPixel = isFloatTex ? 8 : 4;

	MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(isFloatTex ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA8Unorm)
																								 width:srcW
																								height:srcH
																							 mipmapped:NO];
	textureDescriptor.usage = MTLTextureUsageShaderRead;
	// Fix 5: use Shared on Apple Silicon (unified memory) — Managed wastes double RAM there
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
	textureDescriptor.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
#else
	textureDescriptor.storageMode = MTLStorageModeShared;
#endif
	id <MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
	if (!texture)
	{
		LOGError("CRenderBackendMetal::CreateTexture: newTextureWithDescriptor returned nil");
		return;
	}

	u8 *uploadPixels = image->loadImageData->getResultDataForUpload();
	if (uploadPixels == NULL)
	{
		LOGError("CRenderBackendMetal::CreateTexture: image type %2.2x is not uploadable",
				 image->loadImageData->getImageType());
		// Nothing bound, so no bound format -- see the OpenGL twin.
		image->boundFormat = RENDER_TEXTURE_RGBA8;
		return;
	}
	[texture replaceRegion:MTLRegionMake2D(0, 0, srcW, srcH)
			   mipmapLevel:0
				 withBytes:uploadPixels
			   bytesPerRow:srcW * bytesPerPixel];
	image->boundFormat = image->residentFormat;

	[textures addObject:texture];
	image->texturePtr.store((__bridge void*)texture, std::memory_order_release);

//	LOGD("image->texturePtr=%x %3.2f x %3.2f", image->texturePtr, image->rasterWidth, image->rasterHeight);
}

void CRenderBackendMetal::UpdateTextureLinearScaling(CSlrImage *image)
{
	// Metal textures don't have per-texture filter state; filtering is set on the sampler in the shader pipeline
}

void CRenderBackendMetal::ReBindTexture(CSlrImage *image)
{
	if (!image)
	{
		LOGError("CRenderBackendMetal::ReBindTexture: image is NULL");
		return;
	}

	if (!image->isBound)
	{
		LOGError("CRenderBackendMetal::ReBindTexture: image is not bound, CreateTexture");
		CreateTexture(image);
		return;
	}

	// Compressed mip chains cannot be partially replaceRegion'd in place;
	// delete the old texture and recreate it from the compressed mip data.
	if (image->isCompressed)
	{
		DeleteTexture(image);
		CreateTexture(image);
		return;
	}

	// Same reasoning for a RESIDENT FORMAT change (S-5): replaceRegion writes
	// into the texture that already exists, so RGBA8 <-> RGBA16F has to be
	// recreated. Writing 8-byte pixels into a 4-byte allocation is an overrun,
	// and AGXMetal does not tolerate those -- it dereferences past the
	// allocation and the process dies.
	if (image->residentFormat != image->boundFormat)
	{
		DeleteTexture(image);
		CreateTexture(image);
		return;
	}

//	LOGD("ReBindTexture image->texturePtr=%x", image->texturePtr);
	id<MTLTexture> texture = (__bridge id<MTLTexture>)(image->texturePtr.load(std::memory_order_acquire));
	// Same buffer-vs-raster distinction as CreateTexture above -- and this is the
	// path LoadImageForRebinding actually takes, so it is the one that crashed.
	const NSUInteger srcW = (NSUInteger)image->loadImageData->width;
	const NSUInteger srcH = (NSUInteger)image->loadImageData->height;
	if (srcW > texture.width || srcH > texture.height)
	{
		// The decoded buffer no longer fits the texture allocated for it (a
		// resize between bind and rebind). Rebuild rather than overrun.
		CreateTexture(image);
		return;
	}
	u8 *rebindPixels = image->loadImageData->getResultDataForUpload();
	if (rebindPixels == NULL)
	{
		LOGError("CRenderBackendMetal::ReBindTexture: image type %2.2x is not uploadable",
				 image->loadImageData->getImageType());
		return;
	}
	[texture replaceRegion:MTLRegionMake2D(0, 0, srcW, srcH)
			   mipmapLevel:0
				 withBytes:rebindPixels
			   bytesPerRow:srcW * (image->boundFormat == RENDER_TEXTURE_RGBA16F ? 8 : 4)];
}

void CRenderBackendMetal::DeleteTexture(CSlrImage *image)
{
	// The texture is gone, so nothing is bound in any format.
	if (image) image->boundFormat = RENDER_TEXTURE_RGBA8;
	if (!image)
	{
		LOGError("CRenderBackendMetal::DeleteTexture: image is NULL");
		return;
	}

	if (!image->isBound)
	{
		LOGError("CRenderBackendMetal::DeleteTexture: image is not bound");
		return;
	}

	id<MTLTexture> texture = (__bridge id<MTLTexture>)(image->texturePtr.load(std::memory_order_acquire));
	// PARK, do not release: draw commands already recorded this frame -- and up
	// to kFramesInFlight submitted frames -- may still reference it, and binding
	// a RELEASED Metal texture crashes inside the driver rather than failing
	// gracefully the way a stale GL name does.
	MetalParkTextureForRelease(texture);
	[textures removeObject:texture];
	image->texturePtr.store(NULL, std::memory_order_release);
}

EImageGpuFormat CRenderBackendMetal::GetPreferredCompressedFormat()
{
	id<MTLDevice> device = layer.device;
	if (device == nil)
	{
		// Layer not yet initialised; return a safe default.
		return IMG_GPU_ASTC_4x4;
	}

#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
	// On macOS, check for BC (BPTC) support. The property is available on
	// macOS 11+ (Metal feature set macOS-GPU-family2 v1 and later).
	// All Apple Silicon and most Intel Macs since 2012 support BC7.
	if (@available(macOS 11.0, *))
	{
		if ([device supportsBCTextureCompression])
		{
			return IMG_GPU_BC7;
		}
	}
	else
	{
		// macOS < 11 lacks MTLPixelFormatBC7_RGBAUnorm, and Apple GPUs do not
		// expose ASTC on macOS either. Return uncompressed so CImageData::LoadKTX2
		// takes its RGBA32 fallback path on these older systems.
		return IMG_GPU_UNCOMPRESSED;
	}
#endif

	// iOS / tvOS / visionOS: Apple GPUs always support ASTC.
	return IMG_GPU_ASTC_4x4;
}

CRenderBackendMetal::~CRenderBackendMetal()
{
	SDL_DestroyRenderer(renderer);
}

// ===========================================================================
// S-4 seams, Metal implementations.
// ===========================================================================

// Subdirectory-qualified: HEADER_SEARCH_PATHS carries src/Engine but not its
// subdirectories, so a bare basename here depends on Xcode's header map picking
// up a newly added file. Qualifying against src/Engine resolves the same way on
// every platform's build system.
#include "Core/Render/CRenderTarget.h"
#include "Core/Render/CMaskedTileShader.h"
#include "Video/CVideoYUVConverter.h"
#include "CVideoYUVShaderMetal.h"

static void MetalParkTextureForRelease(id<MTLTexture> texture)
{
	if (texture == nil)
		return;
	if (pendingReleaseTextures == nil)
	{
		pendingReleaseTextures = [[NSMutableArray alloc] init];
		pendingReleaseFrames = [[NSMutableArray alloc] init];
	}
	[pendingReleaseTextures addObject:texture];
	[pendingReleaseFrames addObject:@(metalFrameCounter)];
}

void *CRenderBackendMetal::GetCurrentRenderCommandEncoder() { return (__bridge void *)renderEncoder; }
void *CRenderBackendMetal::GetMetalDevice()                 { return (__bridge void *)(layer ? layer.device : nil); }
void *CRenderBackendMetal::GetMetalCommandQueue()           { return (__bridge void *)commandQueue; }

ERenderSurfaceFormat CRenderBackendMetal::GetSurfaceFormat()
{
	if (layer != nil && layer.pixelFormat == MTLPixelFormatRGBA16Float)
		return RENDER_SURFACE_RGBA16F;
	return RENDER_SURFACE_RGBA8;
}

bool CRenderBackendMetal::GetSurfaceIsFloatFormat()
{
	return layer != nil && layer.pixelFormat == MTLPixelFormatRGBA16Float;
}

bool CRenderBackendMetal::GetSurfaceIsLinearColorSpace()
{
	// Distinguishes extended-LINEAR from extended-SRGB, which
	// GetSurfaceIsExtendedRange() cannot -- both contain "Extended".
	//
	// Worth a query of its own because the difference between them is invisible
	// to every test we have: a framebuffer capture reads back the values we
	// WROTE, and the mismatch happens in the compositor afterwards. That is
	// precisely why the whole suite stayed green while the screen was wrong. A
	// human still has to look; this only catches the constant being flipped.
	if (layer == nil || layer.colorspace == NULL)
		return false;

	@autoreleasepool
	{
		NSString *name = (__bridge_transfer NSString *)CGColorSpaceCopyName(layer.colorspace);
		return name != nil && [name containsString:@"Linear"];
	}
}

bool CRenderBackendMetal::GetSurfaceIsExtendedRange()
{
	if (layer == nil || layer.colorspace == NULL)
		return false;

	// Ask Core Graphics rather than remembering what we set: the point of this
	// query is to catch something else having REPLACED the colourspace, which a
	// remembered flag would report as fine.
	@autoreleasepool
	{
		NSString *name = (__bridge_transfer NSString *)CGColorSpaceCopyName(layer.colorspace);
		return name != nil && [name containsString:@"Extended"];
	}
}

float CRenderBackendMetal::GetDisplayHdrHeadroom()
{
	// NEVER cache this. macOS grants headroom lazily -- it reads 1.0 for the
	// first seconds after the request and ramps -- and then keeps moving it with
	// display brightness and ambient light. A value latched at startup is always
	// wrong, and reads as "this display has no HDR".
	@autoreleasepool
	{
		NSWindow *nsWindow = (__bridge NSWindow *)SDL_GetPointerProperty(
			SDL_GetWindowProperties(mainWindow), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
		NSScreen *screen = nsWindow ? nsWindow.screen : [NSScreen mainScreen];
		if (screen == nil)
			return 1.0f;
		return (float)screen.maximumExtendedDynamicRangeColorComponentValue;
	}
}

bool CRenderBackendMetal::ReadFramebufferPixels(int x, int y, int w, int h, unsigned int *outRGBA)
{
	if (w <= 0 || h <= 0 || outRGBA == NULL || captureStagingTexture == nil)
		return false;

	@autoreleasepool
	{
		// The staging blit was encoded on the frame's command buffer, so waiting
		// on that buffer is what makes this read safe. Blocking is fine: this
		// path only exists when the test engine is compiled in.
		if (lastFrameCommandBuffer != nil)
			[lastFrameCommandBuffer waitUntilCompleted];

		if (x < 0 || y < 0 || x + w > (int)captureStagingTexture.width || y + h > (int)captureStagingTexture.height)
			return false;

		// NO VERTICAL FLIP. Metal textures are already top-down, which is the
		// contract here. The GL path flips because ITS framebuffer origin is
		// bottom-left -- doing it on both sides yields a mirrored capture that a
		// "not all zero" assertion happily accepts.
		//
		// UNDER HDR THE SURFACE IS 8 BYTES PER PIXEL, not 4. Reading RGBA16Float
		// with bytesPerRow = w*4 does not fail loudly -- it returns half a row
		// per row, so every capture-based test would compare plausible-looking
		// garbage. The contract of this function is RGBA8 regardless of surface
		// format, so the float case is converted here rather than pushed onto
		// every caller.
		if (captureStagingTexture.pixelFormat == MTLPixelFormatRGBA16Float)
		{
			const size_t count = (size_t)w * (size_t)h;
			std::vector<__fp16> halfPixels(count * 4);
			[captureStagingTexture getBytes:halfPixels.data()
							   bytesPerRow:(NSUInteger)w * 8
								fromRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h)
							   mipmapLevel:0];

			// Clamp to [0,1] and encode as 8-bit. Values ABOVE 1.0 are exactly
			// the extra headroom HDR exists to carry, so this is lossy on
			// purpose: the capture path's job is to answer "did the right thing
			// get drawn", and it must give the same answer on both backends and
			// in both surface formats. A test that needs true HDR values needs a
			// float readback, which is a separate facility.
			for (size_t i = 0; i < count; i++)
			{
				unsigned int rgba = 0;
				for (int c = 0; c < 4; c++)
				{
					float v = (float)halfPixels[i * 4 + c];
					if (v < 0.0f) v = 0.0f;
					if (v > 1.0f) v = 1.0f;
					rgba |= ((unsigned int)(v * 255.0f + 0.5f)) << (c * 8);
				}
				outRGBA[i] = rgba;
			}
			return true;
		}

		[captureStagingTexture getBytes:outRGBA
						   bytesPerRow:(NSUInteger)w * 4
							fromRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h)
						   mipmapLevel:0];

		// The layer is BGRA8 while this function's contract -- and the GL path's
		// glReadPixels(GL_RGBA, ...) -- is RGBA8. Swizzle here so no caller has
		// to know which backend produced the buffer. Invisible to a "not all
		// zero" check, and it would make every cross-backend colour comparison
		// fail in a way that looks like a shader bug.
		if (captureStagingTexture.pixelFormat == MTLPixelFormatBGRA8Unorm)
		{
			const size_t count = (size_t)w * (size_t)h;
			for (size_t i = 0; i < count; i++)
			{
				unsigned int p = outRGBA[i];
				outRGBA[i] = (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
			}
		}
		return true;
	}
}

// Metal can upload both resident formats: RGBA16Float is a first-class texture
// format on every device this backend runs on.
// HDR10 mastering metadata describing the content actually on screen.
//
// MEASURED NOT TO APPLY TO THE SURFACE WE SHIP, and this is deliberate rather
// than unfinished. Setting it on our extended-SRGB layer was tested directly
// (spikes/edr/edr_abovewhite.mm, the `M` toggle, 2026-08-20) and the result was
// unambiguous:
//
//   metadata OFF -> the above-white phase is visibly brighter than SDR white,
//                   at every peak from ~2x up to 12x. Exactly what we want.
//   metadata ON  -> the above-white phase and SDR white become EQUAL. The whole
//                   image saturates; the distinction the float pipeline exists
//                   to produce is destroyed.
//
// The reason is a mismatch of contracts. CAEDRMetadata's HDR10 factories
// describe PQ-encoded BT.2020 content -- that is what HDR10 IS -- so handing
// them to a layer whose buffer is extended sRGB tells the compositor to
// interpret every value under a transfer function the pixels do not use, and it
// re-maps the entire range accordingly.
//
// An EXTENDED-RANGE surface needs no such declaration: `wantsExtendedDynamicRangeContent`
// plus an extended colourspace IS the mechanism, and values above 1.0 are
// simply brighter. Metadata belongs with a PQ surface, which is a different
// stage's work.
//
// So this applies metadata ONLY on a PQ layer, and otherwise guarantees the
// layer carries none. The plumbing is kept rather than deleted because the
// engine is a library surface and a PQ path is a plausible future -- but a
// method that silently made every HDR photo worse would be a landmine, so the
// condition is explicit and the measurement is recorded here.
void CRenderBackendMetal::SetSurfaceEdrMetadata(float maxComponent)
{
	@autoreleasepool
	{
		if (layer == nil)
			return;

		// Is this layer actually PQ? Only then does HDR10 metadata describe it.
		bool layerIsPq = false;
		if (layer.colorspace != NULL)
		{
			NSString *name = (__bridge_transfer NSString *)CGColorSpaceCopyName(layer.colorspace);
			layerIsPq = (name != nil) && ([name containsString:@"PQ"] ||
										  [name containsString:@"2100"]);
		}

		if (!layerIsPq || !(maxComponent > 1.0f))
		{
			// The shipping path. SDR content, no measurement, or -- the usual
			// case -- an extended-range surface that must NOT be given HDR10
			// metadata. Clearing rather than leaving whatever was there also
			// stops one HDR photo describing every photo viewed after it.
			if (layer.EDRMetadata != nil)
				layer.EDRMetadata = nil;
			return;
		}

		if (@available(macOS 10.15, *))
		{
			// 1.0 = SDR reference white = 203 nits (BT.2408), the same anchor
			// the video poster lane uses when it converts PQ and HLG -- one
			// reference across the whole pipeline, or the numbers would
			// describe different whites in different lanes.
			const float kSdrWhiteNits = 203.0f;
			layer.EDRMetadata =
				[CAEDRMetadata HDR10MetadataWithMinLuminance:0.0f
												maxLuminance:maxComponent * kSdrWhiteNits
										  opticalOutputScale:kSdrWhiteNits];
		}
	}
}

// Whether the layer currently carries EDR metadata. For tests: the shipping
// answer on an extended-range surface is NO, and that is worth pinning, because
// the failure it guards against looks like "HDR is broken" rather than like a
// metadata bug.
bool CRenderBackendMetal::GetSurfaceHasEdrMetadata()
{
	@autoreleasepool
	{
		return (layer != nil) && (layer.EDRMetadata != nil);
	}
}

bool CRenderBackendMetal::SupportsTextureFormat(ERenderTextureFormat fmt)
{
	return fmt == RENDER_TEXTURE_RGBA8 || fmt == RENDER_TEXTURE_RGBA16F;
}

// The float sibling of ReadTexturePixels. Separate rather than a flag because
// the buffer type differs: reading an RGBA16Float texture into unsigned ints
// would clamp away exactly the above-1.0 values a caller wants to see.
bool CRenderBackendMetal::ReadTexturePixelsFloat(void *texture, int w, int h, float *outRGBA)
{
	id<MTLTexture> src = (__bridge id<MTLTexture>)texture;
	if (src == nil || outRGBA == NULL || w <= 0 || h <= 0)
		return false;
	if (w > (int)src.width || h > (int)src.height)
		return false;
	if (src.pixelFormat != MTLPixelFormatRGBA16Float)
		return false;

	@autoreleasepool
	{
		MTLTextureDescriptor *d = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
			width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModeShared;
		id<MTLTexture> staging = [layer.device newTextureWithDescriptor:d];
		if (staging == nil)
			return false;

		id<MTLCommandBuffer> cb = [commandQueue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:src sourceSlice:0 sourceLevel:0
				 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
					toTexture:staging
			 destinationSlice:0 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];

		// 8 bytes per pixel, NOT 4 -- the stride mistake that returns half a
		// row per row and looks like plausible garbage rather than failing.
		const size_t count = (size_t)w * (size_t)h * 4;
		u16 *halves = new u16[count];
		[staging getBytes:halves
			  bytesPerRow:(NSUInteger)w * 8
			   fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
			  mipmapLevel:0];
		for (size_t i = 0; i < count; i++)
			outRGBA[i] = HalfToFloat(halves[i]);
		delete [] halves;
	}
	return true;
}

bool CRenderBackendMetal::ReadTexturePixels(void *texture, int w, int h, unsigned int *outRGBA)
{
	id<MTLTexture> src = (__bridge id<MTLTexture>)texture;
	if (src == nil || outRGBA == NULL || w <= 0 || h <= 0)
		return false;
	if (w > (int)src.width || h > (int)src.height)
		return false;

	@autoreleasepool
	{
		// A private-storage texture cannot be read by the CPU, so blit to a
		// shared-storage staging texture first. Done on its own command buffer
		// and waited on, because this is a test-only path with no frame to
		// piggyback -- unlike the capture path, which must ride the frame's
		// command buffer to stay ordered before the present.
		MTLTextureDescriptor *d = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:src.pixelFormat
			width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModeShared;
		id<MTLTexture> staging = [layer.device newTextureWithDescriptor:d];
		if (staging == nil)
			return false;

		id<MTLCommandBuffer> cb = [commandQueue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:src sourceSlice:0 sourceLevel:0
				 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
					toTexture:staging
			 destinationSlice:0 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];

		// No vertical flip: Metal textures are already top-down, which is this
		// function's contract on both backends.
		[staging getBytes:outRGBA
			  bytesPerRow:(NSUInteger)w * 4
			   fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
			  mipmapLevel:0];

		// Same RGBA contract as ReadFramebufferPixels: swizzle if the source is
		// BGRA so no caller has to know which backend produced the buffer.
		if (staging.pixelFormat == MTLPixelFormatBGRA8Unorm)
		{
			const size_t count = (size_t)w * (size_t)h;
			for (size_t i = 0; i < count; i++)
			{
				unsigned int p = outRGBA[i];
				outRGBA[i] = (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
			}
		}
		return true;
	}
}

// --- render target --------------------------------------------------------

class CMetalRenderTarget : public CRenderTarget
{
public:
	CMetalRenderTarget(id<MTLDevice> device, id<MTLCommandQueue> queue)
	: device(device), queue(queue) {}

	virtual ~CMetalRenderTarget() { Destroy(); }

	virtual bool Create(int w, int h, ERenderTextureFormat fmt = RENDER_TEXTURE_RGBA8) override
	{
		// The FORMAT joins the "already correct" test. A caller switching an
		// existing target from RGBA8 to RGBA16F at the same dimensions must get
		// a new texture -- otherwise it would keep writing above-white values
		// into an 8-bit attachment that clamps them away, which is exactly what
		// this parameter exists to stop.
		if (w == width && h == height && fmt == format && colorTexture != nil)
			return true;
		Destroy();

		// The format is CALLER-CHOSEN (S-5 Phase 5). It used to be hardcoded
		// RGBA8 "to match CGLRenderTarget exactly, so a Metal-vs-GL comparison
		// of the offscreen result is meaningful" -- true when both backends
		// could only ever be 8-bit, and stale now that HDR video playback needs
		// a target able to hold above-white values. Both backends take the same
		// parameter, so the comparison still holds; it is just no longer
		// enforced by neither of them having a choice.
		//
		// It still does NOT follow the LAYER's format: a pipeline must match
		// the pass it is encoded into, and this pass targets THIS texture, not
		// the window.
		const MTLPixelFormat mtlFmt = (fmt == RENDER_TEXTURE_RGBA16F)
			? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA8Unorm;
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:mtlFmt
			width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModePrivate;

		colorTexture = [device newTextureWithDescriptor:desc];
		if (colorTexture == nil)
		{
			LOGError("CMetalRenderTarget::Create: newTextureWithDescriptor failed (%dx%d, %s)",
					 w, h, (fmt == RENDER_TEXTURE_RGBA16F) ? "RGBA16Float" : "RGBA8Unorm");
			return false;
		}
		width = w;
		height = h;
		format = fmt;
		return true;
	}

	virtual ERenderTextureFormat GetFormat() const override { return format; }

	virtual void *BeginPassWithClear(float r, float g, float b, float a) override
	{
		return BeginPassInternal(r, g, b, a);
	}

	virtual void *BeginPass() override
	{
		// Transparent black, which is what this path has always cleared to.
		return BeginPassInternal(0.0f, 0.0f, 0.0f, 0.0f);
	}

private:
	void *BeginPassInternal(float clearR, float clearG, float clearB, float clearA)
	{
		if (colorTexture == nil || queue == nil)
			return NULL;

		// Its OWN command buffer and encoder. An encoder is bound to one render
		// pass, and the frame's pass targets the drawable -- reusing it would
		// draw into the window instead of this target.
		passCommandBuffer = [queue commandBuffer];
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = colorTexture;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		// MTLClearColor takes doubles and an RGBA16Float attachment stores them
		// unclamped, so an above-white clear survives on a float target.
		rp.colorAttachments[0].clearColor = MTLClearColorMake(clearR, clearG, clearB, clearA);
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		passEncoder = [passCommandBuffer renderCommandEncoderWithDescriptor:rp];
		return (__bridge void *)passEncoder;
	}

public:

	virtual void EndPass() override
	{
		if (passEncoder != nil) { [passEncoder endEncoding]; passEncoder = nil; }
		if (passCommandBuffer != nil) { [passCommandBuffer commit]; passCommandBuffer = nil; }
	}

	virtual void *GetTexture() const override { return (__bridge void *)colorTexture; }
	virtual int GetWidth() const override { return width; }
	virtual int GetHeight() const override { return height; }

	virtual void Destroy() override
	{
		// PARK the texture rather than releasing it here.
		//
		// Metal keeping it alive for in-flight COMMAND BUFFERS is not enough:
		// GetTexture() hands the raw pointer out to consumers that cache it --
		// CSlrImageExternalTexture holds it across frames, and the photo app's
		// video controller only re-points when the handle or size CHANGES. So a
		// resize (Create() calls Destroy() first) would release a texture that
		// an adapter is still about to draw with. On OpenGL that was harmless,
		// because a stale texture NAME merely fails to sample; on Metal it is a
		// released object and AGX dereferences it.
		MetalParkTextureForRelease(colorTexture);
		colorTexture = nil;
		width = height = 0;
	}

private:
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLTexture> colorTexture = nil;
	id<MTLCommandBuffer> passCommandBuffer = nil;
	id<MTLRenderCommandEncoder> passEncoder = nil;
	int width = 0, height = 0;
	ERenderTextureFormat format = RENDER_TEXTURE_RGBA8;
};

CRenderTarget *CRenderBackendMetal::CreateRenderTarget()
{
	if (layer == nil || commandQueue == nil)
		return NULL;
	return new CMetalRenderTarget(layer.device, commandQueue);
}

// --- video plane / LUT textures -------------------------------------------

void *CRenderBackendMetal::CreatePlaneTexture(int width, int height, int channels, int bytesPerChannel)
{
	if (layer == nil)
		return NULL;

	MTLPixelFormat fmt;
	if (channels == 2)
		fmt = (bytesPerChannel == 2) ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
	else
		fmt = (bytesPerChannel == 2) ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;

	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
																				 width:(NSUInteger)width
																				height:(NSUInteger)height
																			 mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = [layer.device newTextureWithDescriptor:d];
	if (t == nil)
		return NULL;
	return (__bridge_retained void *)t;
}

void CRenderBackendMetal::UpdatePlaneTexture(void *tex, const void *data, int width, int height, int stride)
{
	if (tex == NULL || data == NULL)
		return;
	id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
	[t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
		 mipmapLevel:0
		   withBytes:data
		 bytesPerRow:(NSUInteger)(stride > 0 ? stride : width)];
}

void CRenderBackendMetal::DeletePlaneTexture(void *tex)
{
	if (tex == NULL)
		return;
	id<MTLTexture> t = (__bridge_transfer id<MTLTexture>)tex;   // balances __bridge_retained
	t = nil;
}

void *CRenderBackendMetal::CreateLutTexture3D(int edge)
{
	if (layer == nil)
		return NULL;
	MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
	d.textureType = MTLTextureType3D;
	d.pixelFormat = MTLPixelFormatRGBA16Unorm;
	d.width = (NSUInteger)edge;
	d.height = (NSUInteger)edge;
	d.depth = (NSUInteger)edge;
	d.usage = MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = [layer.device newTextureWithDescriptor:d];
	if (t == nil)
		return NULL;
	return (__bridge_retained void *)t;
}

void CRenderBackendMetal::UpdateLutTexture3D(void *tex, const void *data, int edge)
{
	if (tex == NULL || data == NULL)
		return;
	id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
	[t replaceRegion:MTLRegionMake3D(0, 0, 0, (NSUInteger)edge, (NSUInteger)edge, (NSUInteger)edge)
		 mipmapLevel:0
			   slice:0
		   withBytes:data
		 bytesPerRow:(NSUInteger)edge * 8            // RGBA16 = 8 bytes/texel
	   bytesPerImage:(NSUInteger)edge * (NSUInteger)edge * 8];
}

void CRenderBackendMetal::DeleteLutTexture3D(void *tex)
{
	if (tex == NULL)
		return;
	id<MTLTexture> t = (__bridge_transfer id<MTLTexture>)tex;
	t = nil;
}

// --- shader / converter factories -----------------------------------------
//
// Filled in by the shader and video tasks. Until then they return NULL, which
// every call site already handles by drawing its plain/unshaded fallback rather
// than dereferencing -- that is what lets the other two apps LAUNCH on Metal
// while their shaders are still being ported.

CMaskedTileShader *CRenderBackendMetal::CreateMaskedTileShader(bool queued)
{
	if (layer == nil)
		return NULL;
	return new CRenderShaderMaskedTileMetal(this, queued);
}

CRenderShader *CRenderBackendMetal::CreateFlatColorShader(float r, float g, float b, float a)
{
	if (layer == nil)
		return NULL;
	return new CRenderShaderFlatColorMetal(this, r, g, b, a);
}

// --- per-draw texture filtering -------------------------------------------
//
// OWN sampler states rather than the two imgui_impl_metal builds, for two
// reasons that both matter:
//
//  1. GL PARITY. UpdateTextureLinearScaling() sets MIN=LINEAR, MAG=NEAREST for a
//     non-linear image -- nearest MAGNIFICATION only. ImGui's nearest sampler is
//     nearest on min, mag AND mip, so using it would change minification too and
//     a downscaled bitmap font would alias on Metal where it does not on OpenGL.
//
//  2. MIPS. imgui_impl_metal.mm says outright not to put a mipped KTX2 texture
//     on its nearest sampler, because that one is MipFilterNearest and the
//     compressed-atlas path (KTX2/UASTC -> BC7/ASTC) depends on MipFilterLinear.
//     Ours keeps MipFilterLinear, so a compressed mipped texture that also wants
//     point magnification stays correct.
static id<MTLSamplerState> samplerMagNearest = nil;
static id<MTLSamplerState> samplerLinear = nil;

static void MetalEnsureSamplers(id<MTLDevice> device)
{
	if (samplerMagNearest != nil || device == nil)
		return;

	@autoreleasepool
	{
		MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
		d.sAddressMode = MTLSamplerAddressModeClampToEdge;
		d.tAddressMode = MTLSamplerAddressModeClampToEdge;
		d.minFilter = MTLSamplerMinMagFilterLinear;
		d.magFilter = MTLSamplerMinMagFilterLinear;
		d.mipFilter = MTLSamplerMipFilterLinear;
		samplerLinear = [device newSamplerStateWithDescriptor:d];

		// The one that matters: point magnification, linear minification and
		// mips -- exactly what glTexParameteri does on the OpenGL side.
		d.magFilter = MTLSamplerMinMagFilterNearest;
		samplerMagNearest = [device newSamplerStateWithDescriptor:d];
	}
}

// The callbacks run at RENDER time, long after the blit that queued them, so
// they must fetch the encoder freshly and tolerate nil -- NewFrame() aborts the
// frame when there is no drawable and the draw lists are still walked.
static void MetalDrawCallbackSamplerMagNearest(const ImDrawList *, const ImDrawCmd *)
{
	CRenderBackendMetal *backend = (CRenderBackendMetal *)VID_GetRenderBackend();
	if (backend == NULL)
		return;
	id<MTLRenderCommandEncoder> encoder =
		(__bridge id<MTLRenderCommandEncoder>)backend->GetCurrentRenderCommandEncoder();
	if (encoder == nil || samplerMagNearest == nil)
		return;
	[encoder setFragmentSamplerState:samplerMagNearest atIndex:0];
}

static void MetalDrawCallbackSamplerLinear(const ImDrawList *, const ImDrawCmd *)
{
	CRenderBackendMetal *backend = (CRenderBackendMetal *)VID_GetRenderBackend();
	if (backend == NULL)
		return;
	id<MTLRenderCommandEncoder> encoder =
		(__bridge id<MTLRenderCommandEncoder>)backend->GetCurrentRenderCommandEncoder();
	if (encoder == nil || samplerLinear == nil)
		return;
	[encoder setFragmentSamplerState:samplerLinear atIndex:0];
}

bool CRenderBackendMetal::ImageNeedsSamplerOverride(CSlrImage *image)
{
	return image != NULL && !image->linearScaling;
}

void CRenderBackendMetal::QueueSamplerForImage(CSlrImage *image)
{
	if (layer == nil || image == NULL)
		return;
	MetalEnsureSamplers(layer.device);
	ImGui::GetWindowDrawList()->AddCallback(
		image->linearScaling ? MetalDrawCallbackSamplerLinear : MetalDrawCallbackSamplerMagNearest,
		NULL);
}

void CRenderBackendMetal::QueueDefaultSampler()
{
	if (layer == nil)
		return;
	MetalEnsureSamplers(layer.device);
	ImGui::GetWindowDrawList()->AddCallback(MetalDrawCallbackSamplerLinear, NULL);
}

unsigned int CRenderBackendMetal::GetColorPixelFormatRaw()
{
	// Raw NSUInteger rather than MTLPixelFormat, because the header is included
	// from plain C++ all over the engine and must not name a Metal type. Callers
	// cast it back. Reading it from the LAYER rather than returning a constant
	// matters from Task 11 on: HDR flips the surface to RGBA16Float, and a
	// pipeline whose attachment format disagrees with the render pass fails at
	// draw time, not at creation.
	if (layer == nil)
		return (unsigned int)MTLPixelFormatBGRA8Unorm;
	return (unsigned int)layer.pixelFormat;
}

CVideoYUVConverter *CRenderBackendMetal::CreateVideoYUVConverter()
{
	if (layer == nil || commandQueue == nil)
		return NULL;
	return new CVideoYUVShaderMetal((__bridge void *)layer.device, (__bridge void *)commandQueue);
}
