#include "CRenderBackendMetal.h"
#include "SYS_Main.h"
#include "CSlrImage.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_metal.h"
#import <Metal/Metal.h>
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

// Fix 3: triple-buffering semaphore — caps CPU ahead-of-GPU to 3 frames
static dispatch_semaphore_t kInFlightFrames;
static dispatch_once_t kInFlightFramesOnce;

CRenderBackendMetal::CRenderBackendMetal()
: CRenderBackend("Metal")
{
	textures = [[NSMutableArray alloc] init];
}

SDL_Window *CRenderBackendMetal::CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized)
{
	// Inform SDL that we will be using metal for rendering. Without this hint initialization of metal renderer may fail.
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");

	int windowFlags = (SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
//	if (maximized)
//		windowFlags |= SDL_WINDOW_MAXIMIZED;
	mainWindow = SDL_CreateWindow(title, x, y, w, h, (SDL_WindowFlags)windowFlags);

	if (mainWindow == NULL)
	{
		SYS_FatalExit("Error creating window: %s\n", SDL_GetError());
	}

	renderer = SDL_CreateRenderer(mainWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (renderer == NULL)
	{
		SYS_FatalExit("Error creating renderer: %s\n", SDL_GetError());
	}

	return mainWindow;
}

void CRenderBackendMetal::CreateRenderContext()
{

}

void CRenderBackendMetal::InitRenderPipeline()
{
	// Setup Platform/Renderer backends
	layer = (__bridge CAMetalLayer*)SDL_RenderGetMetalLayer(renderer);
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	ImGui_ImplMetal_Init(layer.device);
	ImGui_ImplSDL2_InitForMetal(mainWindow);

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
		SDL_GetRendererOutputSize(renderer, &width, &height);
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

		// Fix 1: nextDrawable can return nil (occluded, throttled, resize race)
		drawable = [layer nextDrawable];
		if (drawable == nil)
		{
			LOGError("CRenderBackendMetal::NewFrame: nextDrawable returned nil — skipping frame");
			// Balance the semaphore so the completion handler never fires for this frame
			dispatch_semaphore_signal(kInFlightFrames);
			return;
		}

		commandBuffer = [commandQueue commandBuffer];

		renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(clearColor.x * clearColor.w, clearColor.y * clearColor.w, clearColor.z * clearColor.w, clearColor.w);

		renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
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
		// Fix 1: if NewFrame aborted (nil drawable, 0×0 size, etc.) nothing to present
		if (drawable == nil || commandBuffer == nil || renderEncoder == nil)
			return;

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

	MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																								 width:(NSUInteger)image->rasterWidth
																								height:(NSUInteger)image->rasterHeight
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

	[texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image->rasterWidth, (NSUInteger)image->rasterHeight) mipmapLevel:0 withBytes:image->loadImageData->getRGBAResultData() bytesPerRow:(NSUInteger)image->rasterWidth * 4];

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

//	LOGD("ReBindTexture image->texturePtr=%x", image->texturePtr);
	id<MTLTexture> texture = (__bridge id<MTLTexture>)(image->texturePtr.load(std::memory_order_acquire));
	[texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image->rasterWidth, (NSUInteger)image->rasterHeight) mipmapLevel:0 withBytes:image->loadImageData->getRGBAResultData() bytesPerRow:(NSUInteger)image->rasterWidth * 4];
}

void CRenderBackendMetal::DeleteTexture(CSlrImage *image)
{
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
