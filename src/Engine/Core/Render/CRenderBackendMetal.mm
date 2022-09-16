#include "CRenderBackendMetal.h"
#include "SYS_Main.h"
#include "CSlrImage.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_metal.h"
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <vector>

NSMutableArray *textures;

CRenderBackendMetal::CRenderBackendMetal()
: CRenderBackend("Metal")
{
	textures = [[NSMutableArray alloc] init];
}

SDL_Window *CRenderBackendMetal::CreateSDLWindow(const char *title, int x, int y, int w, int h)
{
	// Inform SDL that we will be using metal for rendering. Without this hint initialization of metal renderer may fail.
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");

	mainWindow = SDL_CreateWindow(title, x, y, w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
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

CAMetalLayer* layer;
id<MTLCommandQueue> commandQueue;
MTLRenderPassDescriptor* renderPassDescriptor;

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

bool ImGui_ImplMetal_CreateFontsTexture();

void CRenderBackendMetal::CreateFontsTexture()
{
	ImGui_ImplMetal_CreateFontsTexture(layer.device);
}

id<MTLCommandBuffer> commandBuffer;
id <MTLRenderCommandEncoder> renderEncoder;
id<CAMetalDrawable> drawable;

void CRenderBackendMetal::NewFrame(ImVec4 clearColor)
{
	int width, height;
	SDL_GetRendererOutputSize(renderer, &width, &height);
	layer.drawableSize = CGSizeMake(width, height);
	drawable = [layer nextDrawable];

	commandBuffer = [commandQueue commandBuffer];
//	renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(clearColor[0] * clear_color[3], clear_color[1] * clear_color[3], clear_color[2] * clear_color[3], clear_color[3]);
	
	renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(clearColor.x * clearColor.w, clearColor.y * clearColor.w, clearColor.z * clearColor.w, clearColor.w);

	renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
	renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
	renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
	renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
	[renderEncoder pushDebugGroup:@"CRenderBackendMetal"];

	// Start the Dear ImGui frame
	ImGui_ImplMetal_NewFrame(renderPassDescriptor);
}

void CRenderBackendMetal::PresentFrameBuffer(ImVec4 clearColor)
{
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);

	[renderEncoder popDebugGroup];
	[renderEncoder endEncoding];

	[commandBuffer presentDrawable:drawable];
	[commandBuffer commit];

	ImGuiIO& io = ImGui::GetIO();

	// Update and Render additional Platform Windows
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}
}

void CRenderBackendMetal::Shutdown()
{
	ImGui_ImplMetal_Shutdown();
}

void CRenderBackendMetal::CreateTexture(CSlrImage *image)
{
	id<MTLDevice> device = layer.device;
	
	MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																								 width:(NSUInteger)image->rasterWidth
																								height:(NSUInteger)image->rasterHeight
																							 mipmapped:NO];
	textureDescriptor.usage = MTLTextureUsageShaderRead;
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
	textureDescriptor.storageMode = MTLStorageModeManaged;
#else
	textureDescriptor.storageMode = MTLStorageModeShared;
#endif
	id <MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
	[texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image->rasterWidth, (NSUInteger)image->rasterHeight) mipmapLevel:0 withBytes:image->loadImageData->getRGBAResultData() bytesPerRow:(NSUInteger)image->rasterWidth * 4];

	[textures addObject:texture];
	image->texturePtr = (__bridge void*)texture;

//	LOGD("image->texturePtr=%x %3.2f x %3.2f", image->texturePtr, image->rasterWidth, image->rasterHeight);
}

void CRenderBackendMetal::UpdateTextureLinearScaling(CSlrImage *image)
{
//	if (image->linearScaling)
}

void CRenderBackendMetal::ReBindTexture(CSlrImage *image)
{
//	LOGD("ReBindTexture image->texturePtr=%x", image->texturePtr);
	id<MTLTexture> texture = (__bridge id<MTLTexture>)(image->texturePtr);
	[texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image->rasterWidth, (NSUInteger)image->rasterHeight) mipmapLevel:0 withBytes:image->loadImageData->getRGBAResultData() bytesPerRow:(NSUInteger)image->rasterWidth * 4];
}

void CRenderBackendMetal::DeleteTexture(CSlrImage *image)
{
	LOGTODO("CRenderBackendMetal::DeleteTexture");
//	GLuint textureId = (intptr_t)image->texturePtr;
//	glDeleteTextures(1, &textureId);
}

CRenderBackendMetal::~CRenderBackendMetal()
{
	SDL_DestroyRenderer(renderer);
}

