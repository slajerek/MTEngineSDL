#include "CRenderBackendOpenGL4.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "CSlrImage.h"
#include "CImageData.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <GL/gl3w.h>
#include <vector>

// Compressed-texture internal formats. These enum tokens come from the
// GL_ARB_texture_compression_bptc and GL_KHR_texture_compression_astc_ldr
// extensions and may be absent from the GL headers in this tree. Define them
// to their standard values when missing (design note §8.5).
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif

CRenderBackendOpenGL4::CRenderBackendOpenGL4()
: CRenderBackend("OpenGL4")
, cachedCompressedFormat(-1)
{
}

SDL_Window *CRenderBackendOpenGL4::CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized)
{
	LOGD("CRenderBackendOpenGL4::CreateSDLWindow");
	
	SetupGlSlVersion();
	LOGM("GlSl version is %s", glslVersionString);
	
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	SDL_WindowFlags windowFlags = (SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
//	if (maximized)
//		windowFlags |= SDL_WINDOW_MAXIMIZED;

	// SDL3 dropped x/y from SDL_CreateWindow -- position is set afterwards.
	// The window is created HIDDEN (see the flags), so setting the position
	// before it is shown is exactly equivalent to the old behaviour and there
	// is no visible jump.
	//
	// SDL_WINDOW_HIGH_PIXEL_DENSITY is SDL2's SDL_WINDOW_ALLOW_HIGHDPI, renamed.
	// It is still OPT-IN: dropping it here would quietly ship a non-Retina
	// backbuffer, and the symptom would look like a UI-scale bug rather than a
	// missing flag.
	mainWindow = SDL_CreateWindow(title, w, h, windowFlags);
	if (mainWindow != NULL)
	{
		SDL_SetWindowPosition(mainWindow, x, y);
	}
	return mainWindow;
}

void CRenderBackendOpenGL4::CreateRenderContext()
{
	LOGM("OpenGL Init");

	glContext = SDL_GL_CreateContext(mainWindow);
	if (glContext == NULL)
	{
		LOGError("CRenderBackendOpenGL4: Failed to create SDL GL Context. This is fatal! Error=%s\n", SDL_GetError());
		return;
	}

	LOGD("CRenderBackendOpenGL4: glContext is %x", glContext);
	
	// Initialize OpenGL loader. NOTE: ImGui now has own loader too
	int err = gl3wInit();
	
	if (err != 0)
	{
		LOGError("CRenderBackendOpenGL4: Failed to initialize OpenGL loader gl3w, error code=%d", err);
		return;
	}

	const GLubyte* vendor = glGetString(GL_VENDOR);
	const GLubyte* renderer = glGetString(GL_RENDERER);
	const GLubyte* version = glGetString(GL_VERSION);
	
	LOGM("OpenGL vendor=%s, renderer=%s, version=%s", vendor, renderer, version);

	SDL_GL_MakeCurrent(mainWindow, glContext);
	ASSERT_OPENGL();
	
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	ASSERT_OPENGL();

	// HEADLESS: never present. SDL_GL_SwapWindow goes through
	// Cocoa_GL_SwapWindow, which waits on the window server for the display to
	// take the frame. With no display attached, or the display asleep, that wait
	// does not return -- a headless run then blocks forever instead of failing,
	// which is worse than a crash because CI and overnight runs hang silently.
	// Nothing is on screen in headless mode, so there is nothing to present.
	if (!gHeadlessMode)
	{
		SDL_GL_SwapWindow(mainWindow);
		ASSERT_OPENGL();
	}

	// vsync throttles the render loop -- but it is also what makes the swap wait
	// on the display. A headless run has no display to sync to, so request 0.
	SDL_GL_SetSwapInterval(gHeadlessMode ? 0 : 1);
	ASSERT_OPENGL();

}

void CRenderBackendOpenGL4::InitRenderPipeline()
{
	ImGui_ImplSDL3_InitForOpenGL(mainWindow, glContext);
	ImGui_ImplOpenGL3_Init(glslVersionString);
}

void CRenderBackendOpenGL4::SetupGlSlVersion()
{
	LOGD("CRenderBackendOpenGL4::SetupGlSlVersion");
	
	// Decide GL+GLSL versions
#if __APPLE__
	LOGD("CRenderBackendOpenGL4::GetGlSlVersion: Apple: version 410");
	// GL 4.1 + GLSL 410
	glslVersionString = "#version 410";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#elif defined(LINUX)
	LOGD("CRenderBackendOpenGL4::GetGlSlVersion: Linux: version 400");
	// GL 4.0 + GLSL 400
	glslVersionString = "#version 400";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
	LOGD("CRenderBackendOpenGL4::GetGlSlVersion: other: version 410");
	
	// GL 4.1 + GLSL 410
	glslVersionString = "#version 410";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
}

const char *CRenderBackendOpenGL4::GetGlSlVersion()
{
	return glslVersionString;
}

void CRenderBackendOpenGL4::CreateFontsTexture()
{
	// No longer needed: new ImGui backend handles texture creation/updates automatically via ImGui_ImplOpenGL3_UpdateTexture()
}

void CRenderBackendOpenGL4::NewFrame(ImVec4 clearColor)
{
	ImGui_ImplOpenGL3_NewFrame();
}

void CRenderBackendOpenGL4::PresentFrameBuffer(ImVec4 clearColor)
{
	ImGuiIO& io = ImGui::GetIO();
//	SDL_GL_MakeCurrent(gMainWindow, glContext);
	
	//		LOGD("io.DisplaySize.x=%5.2f io.DisplaySize.y=%5.2f", io.DisplaySize.x, io.DisplaySize.y);
	glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
	ASSERT_OPENGL();
	glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
	ASSERT_OPENGL();
	glClear(GL_COLOR_BUFFER_BIT);
	ASSERT_OPENGL();

	///
//	LOGD("CRenderBackendOpenGL4::PresentFrameBuffer: ImGui_ImplOpenGL3_RenderDrawData");
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	
	// Update and Render additional Platform Windows
	// (Platform functions may change the current OpenGL context, so we save/restore it to make it easier to paste this code elsewhere.
	//  For this specific demo app we could also call glfwMakeContextCurrent(window) directly)
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
//		//			GLFWwindow* backup_current_context = glfwGetCurrentContext();
//		ImGui::UpdatePlatformWindows();
//		ImGui::RenderPlatformWindowsDefault();
//		//			glfwMakeContextCurrent(backup_current_context);
//		SDL_GL_MakeCurrent(gMainWindow, glContext);
		
		SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
		SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();

		SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
	}


//	LOGD("CRenderBackendOpenGL4::PresentFrameBuffer: SDL_GL_SwapWindow");
	// HEADLESS: skip the present, not the draw. Everything above -- the clear,
	// ImGui_ImplOpenGL3_RenderDrawData, the platform windows -- has already run,
	// so the back buffer holds the finished frame and imgui_test_engine's
	// screen-capture readback still sees it. Only the blocking hand-off to the
	// window server is skipped. See the comment in InitBackend.
	if (!gHeadlessMode)
	{
		SDL_GL_SwapWindow(mainWindow);
	}
}

void CRenderBackendOpenGL4::Shutdown()
{
	ImGui_ImplOpenGL3_Shutdown();
}

// Compressed (KTX2/UASTC-transcoded) + mipmapped upload path. Fully separate
// from the RGBA path below; an RGBA image never reaches this function.
static void OpenGL4CreateCompressedTexture(CSlrImage *image)
{
	CImageData *cd = image->loadImageData;
	if (cd == NULL || cd->compressedMips == NULL || cd->compressedMipCount <= 0)
	{
		LOGError("CRenderBackendOpenGL4::CreateTexture: no compressed mip data");
		return;
	}

	// EImageGpuFormat -> GL internal format.
	GLenum internalFormat;
	switch (cd->compressedGpuFormat)
	{
		case IMG_GPU_ASTC_4x4:
			internalFormat = GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
			break;
		case IMG_GPU_BC7:
		default:
			internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM;
			break;
	}

	const int mipCount = cd->compressedMipCount;

	GLuint textureId;
	glGenTextures(1, &textureId);
	ASSERT_OPENGL();
	glBindTexture(GL_TEXTURE_2D, textureId);
	ASSERT_OPENGL();

	// Min-filter: mip-aware only when the texture actually has a mip chain;
	// single-level compressed images keep today's non-mip filter.
	if (mipCount > 1)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
						image->linearScaling ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
		ASSERT_OPENGL();
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		ASSERT_OPENGL();
	}
	// Mag-filter never samples mips; identical to the RGBA path.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					image->linearScaling ? GL_LINEAR : GL_NEAREST);
	ASSERT_OPENGL();

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	ASSERT_OPENGL();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	ASSERT_OPENGL();

	// Bound the sampled mip range to what the KTX2 file actually provides.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipCount - 1);
	ASSERT_OPENGL();

	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	ASSERT_OPENGL();

	// Upload every mip level. Logical mip dims (width>>level, clamped >=1) drive
	// the texture extent; the transcoded payload size is the block byte count.
	// Do NOT call glGenerateMipmap — the mips come from the KTX2 file.
	for (int level = 0; level < mipCount; level++)
	{
		const SCompressedMip &mip = cd->compressedMips[level];

		GLsizei mipW = (GLsizei)(cd->width  >> level);
		if (mipW < 1) mipW = 1;
		GLsizei mipH = (GLsizei)(cd->height >> level);
		if (mipH < 1) mipH = 1;

		glCompressedTexImage2D(GL_TEXTURE_2D, level, internalFormat,
							   mipW, mipH, 0,
							   (GLsizei)mip.blockDataSize, mip.blockData);
		ASSERT_OPENGL();
	}

	image->texturePtr.store((void*)(intptr_t)textureId, std::memory_order_release);
}

void CRenderBackendOpenGL4::CreateTexture(CSlrImage *image)
{
	if (image->isCompressed)
	{
		OpenGL4CreateCompressedTexture(image);
		return;
	}

	// getResultDataForUpload, not getRGBAResultData: the latter fatal-exits on
	// any type but RGBA8, and a float image is a legitimate upload (S-5).
	u8 *data = image->loadImageData->getResultDataForUpload();
	if (data == NULL)
	{
		LOGError("CRenderBackendOpenGL4::CreateTexture: image type %2.2x is not uploadable",
				 image->loadImageData->getImageType());
		// No texture exists, so no format is bound. Leaving boundFormat at a
		// stale value would have the next ReBindTexture compare against a
		// format the (absent) texture never had.
		image->boundFormat = RENDER_TEXTURE_RGBA8;
		return;
	}

	GLuint textureId;
	glGenTextures(1, &textureId);
	ASSERT_OPENGL();
	glBindTexture(GL_TEXTURE_2D, textureId);
	ASSERT_OPENGL();

	if (image->linearScaling)
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		ASSERT_OPENGL();
	}

	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	ASSERT_OPENGL();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	ASSERT_OPENGL();

	// Use the actual buffer dimensions. LoadImageForRebinding passes through the
	// raw decoded buffer (original pixel size, not POT-padded); LoadImage/PreloadImage
	// allocates a padded CImageData so loadImageData->width == rasterWidth there.
	// OpenGL 4 supports NPOT textures natively.
	int texW = (int)image->loadImageData->width;
	int texH = (int)image->loadImageData->height;
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	ASSERT_OPENGL();

	// The RESIDENT format decides the internal format and the source type.
	// GL_RGBA16F + GL_HALF_FLOAT needs GL 3.0+, which the 4.1 core context
	// already guarantees, so there is no capability branch to make here --
	// SupportsTextureFormat() is what answers that question, once.
	u8 *pixels = image->loadImageData->getResultDataForUpload();
	if (pixels == NULL)
	{
		LOGError("CRenderBackendOpenGL4::CreateTexture: image type %2.2x is not uploadable",
				 image->loadImageData->getImageType());
		return;
	}
	if (image->residentFormat == RENDER_TEXTURE_RGBA16F)
	{
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, texW, texH, 0, GL_RGBA, GL_HALF_FLOAT, pixels);
	}
	else
	{
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	}
	ASSERT_OPENGL();
	image->boundFormat = image->residentFormat;

	image->texturePtr.store((void*)(intptr_t)textureId, std::memory_order_release);
}

void CRenderBackendOpenGL4::UpdateTextureLinearScaling(CSlrImage *image)
{
//	LOGD("CRenderBackendOpenGL4::UpdateTextureLinearScaling: image=%x", image);
	if (!image->isBound)
	{
		LOGError("CRenderBackendOpenGL4::UpdateTextureLinearScaling: image is not bound");
		return;
	}
	
	glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)image->texturePtr.load(std::memory_order_acquire));
	if (image->linearScaling)
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		ASSERT_OPENGL();
	}
}

void CRenderBackendOpenGL4::ReBindTexture(CSlrImage *image)
{
//	LOGD("CRenderBackendOpenGL4::ReBindTexture: image=%x");
	if (!image)
	{
		LOGError("CRenderBackendOpenGL4::ReBindTexture: image is NULL");
		return;
	}
	
	if (!image->isBound)
	{
		LOGError("CRenderBackendOpenGL4::ReBindTexture: image is not bound, CreateTexture");
		CreateTexture(image);
		return;
	}

	// Compressed mip chains are not partially re-uploaded in place; delete the
	// old texture object and recreate it from the compressed mip data.
	if (image->isCompressed)
	{
		DeleteTexture(image);
		CreateTexture(image);
		return;
	}

	// Same reasoning for a RESIDENT FORMAT change (S-5): glTexSubImage2D
	// reuses the existing storage, so RGBA8 <-> RGBA16F cannot be written in
	// place -- 8-byte pixels into a 4-byte allocation is an overrun, not a
	// wrong colour.
	if (image->residentFormat != image->boundFormat)
	{
		DeleteTexture(image);
		CreateTexture(image);
		return;
	}

//	LOGD("CRenderBackendOpenGL4::ReBindTexture: width=%d height=%d image=%x image->loadImageData=%x", image->rasterWidth, image->rasterHeight, image, image->loadImageData, data);
	
	glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)image->texturePtr.load(std::memory_order_acquire));
	ASSERT_OPENGL();

	if (image->linearScaling)
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		ASSERT_OPENGL();
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		ASSERT_OPENGL();
	}
	
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	ASSERT_OPENGL();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	ASSERT_OPENGL();

//	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
//	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->rasterWidth, image->rasterHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->loadImageData->getRGBAResultData());
	// glTexSubImage2D does NOT reallocate storage, so a format CHANGE cannot be
	// written -- it has to be recreated, or 8-byte pixels go into a 4-byte
	// allocation. (ReBindImageData and RefreshImageParameters are exactly this
	// shape, and c64d calls both.)
	u8 *pixels = image->loadImageData->getResultDataForUpload();
	if (pixels == NULL)
	{
		LOGError("CRenderBackendOpenGL4::ReBindTexture: image type %2.2x is not uploadable",
				 image->loadImageData->getImageType());
		return;
	}
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->loadImageData->width, image->loadImageData->height,
					GL_RGBA,
					image->boundFormat == RENDER_TEXTURE_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE,
					pixels);
	ASSERT_OPENGL();
}

void CRenderBackendOpenGL4::DeleteTexture(CSlrImage *image)
{
	// The texture is gone, so nothing is bound in any format.
	if (image) image->boundFormat = RENDER_TEXTURE_RGBA8;
//	LOGD("CRenderBackendOpenGL4::DeleteTexture: %x", image);
	if (!image)
	{
		LOGError("CRenderBackendOpenGL4::DeleteTexture: image is NULL");
		return;
	}

	if (!image->isBound)
	{
		LOGError("CRenderBackendOpenGL4::DeleteTexture: image is not bound");
		return;
	}
	
	GLuint textureId = (intptr_t)image->texturePtr.load(std::memory_order_acquire);
	glDeleteTextures(1, &textureId);
	ASSERT_OPENGL();
	image->texturePtr.store(NULL, std::memory_order_release);
}

EImageGpuFormat CRenderBackendOpenGL4::GetPreferredCompressedFormat()
{
	// Return cached result after the first probe.
	if (cachedCompressedFormat != -1)
	{
		return (EImageGpuFormat)cachedCompressedFormat;
	}

	bool hasBptc  = false;
	bool hasAstc  = false;

	GLint numExtensions = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
	for (GLint i = 0; i < numExtensions; ++i)
	{
		const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (ext == nullptr)	
			continue;
		if (strcmp(ext, "GL_ARB_texture_compression_bptc") == 0)
			hasBptc = true;
		else if (strcmp(ext, "GL_KHR_texture_compression_astc_ldr") == 0)
			hasAstc = true;
	}

	EImageGpuFormat result;
	if (hasBptc)
		result = IMG_GPU_BC7;
	else if (hasAstc)
		result = IMG_GPU_ASTC_4x4;
	else
		result = IMG_GPU_UNCOMPRESSED;

	cachedCompressedFormat = (int)result;
	return result;
}

CRenderBackendOpenGL4::~CRenderBackendOpenGL4()
{
	SDL_GL_DestroyContext(glContext);
}

CRenderBackendOpenGL4 *CRenderBackendOpenGL4::GetRenderBackendOpenGL4()
{
	CRenderBackend *renderBackend = VID_GetRenderBackend();
	if (strcmp(renderBackend->name, "OpenGL4"))
	{
		SYS_FatalExit("CRenderBackendOpenGL4::GetRenderBackendOpenGL4: current renderBackend is %s", renderBackend->name);
		return NULL;
	}
	
	return (CRenderBackendOpenGL4*)renderBackend;
}

bool CRenderBackendOpenGL4::CheckOpenGLError()
{
	GLenum glError = glGetError();
	if (glError != GL_NO_ERROR)
	{
		switch (glError)
		{
			// opengl 2 errors (8)
			case GL_INVALID_ENUM:
				LOGError("glError: GL_INVALID_ENUM");
				return true;
			case GL_INVALID_VALUE:
				LOGError("glError: GL_INVALID_VALUE");
				return true;

			case GL_INVALID_OPERATION:
				LOGError("glError: GL_INVALID_OPERATION");
				return true;

			case GL_STACK_OVERFLOW:
				LOGError("glError: GL_STACK_OVERFLOW");
				return true;

			case GL_STACK_UNDERFLOW:
				LOGError("glError: GL_STACK_UNDERFLOW");
				return true;

			case GL_OUT_OF_MEMORY:
				LOGError("glError: GL_OUT_OF_MEMORY");
				return true;

//			case GL_TABLE_TOO_LARGE:
//				  LOGError("glError: GL_TABLE_TOO_LARGE");
//				  break;
			  default:
				LOGError("glError: GL_INVALID_ENUM");
				return true;
		}
		
		return true;
	}
	return false;
}

// ===========================================================================
// S-4: backend-neutral seams, OpenGL implementations.
// ===========================================================================

#include "CMaskedTileShader.h"
#include "CRenderShaderFlatColorOpenGL4.h"
#include "CRenderShaderMaskedTile.h"
#include "CRenderShaderMaskedTileQueued.h"

// Adapts the existing GL masked-tile shaders to the backend-neutral interface.
//
// An ADAPTER rather than making CRenderShaderMaskedTile inherit the interface
// directly: it already derives from CRenderShaderOpenGL4, which declares
// CompileShaders()/UseShaderProgram()/ResetState() of its own, so adding a
// second base with the same pure-virtual signatures would force a forwarding
// override for each anyway. Doing it here keeps the GL shader classes untouched
// and confines the void*<->GLuint conversion to one place.
class CMaskedTileShaderGL : public CMaskedTileShader
{
public:
	CMaskedTileShaderGL(CRenderShaderMaskedTile *shader, CRenderShaderMaskedTileQueued *queued)
	: shader(shader), queued(queued) {}

	virtual ~CMaskedTileShaderGL() { delete shader; }

	virtual void CompileShaders() override    { shader->CompileShaders(); }
	virtual void UseShaderProgram() override  { shader->UseShaderProgram(); }
	virtual void ResetState() override        { shader->ResetState(); }
	virtual bool IsUsable() const override    { return shader->isCompiled; }

	virtual void SetMaskTexture(void *maskTexture) override
	{
		shader->SetMaskTexture((GLuint)(uintptr_t)maskTexture);
	}

	virtual void SetTileBounds(float px, float py, float sx, float sy) override
	{
		shader->SetTileBounds(px, py, sx, sy);
	}

	virtual void BeginBatch() override
	{
		if (queued) queued->BeginBatch();
	}

	virtual void PushTileBounds(void *maskTexture, float px, float py, float sx, float sy) override
	{
		if (queued) queued->PushTileBounds((GLuint)(uintptr_t)maskTexture, px, py, sx, sy);
	}

private:
	CRenderShaderMaskedTile *shader;
	CRenderShaderMaskedTileQueued *queued;   // same object as `shader` when queued, else NULL
};

CMaskedTileShader *CRenderBackendOpenGL4::CreateMaskedTileShader(bool queued)
{
	if (queued)
	{
		CRenderShaderMaskedTileQueued *q = new CRenderShaderMaskedTileQueued(this);
		return new CMaskedTileShaderGL(q, q);
	}
	return new CMaskedTileShaderGL(new CRenderShaderMaskedTile(this), NULL);
}

CRenderShader *CRenderBackendOpenGL4::CreateFlatColorShader(float r, float g, float b, float a)
{
	return new CRenderShaderFlatColorOpenGL4(this, r, g, b, a);
}

// --- per-draw texture filtering -------------------------------------------
//
// This backend sets min/mag per TEXTURE (CreateTexture and
// UpdateTextureLinearScaling call glTexParameteri on the texture itself), which
// used to be the whole story. It no longer is.
//
// imgui_impl_opengl3 now binds a SAMPLER OBJECT in SetupRenderState whenever
// glBindSampler is available -- desktop GL 3.3+, i.e. always here -- and a bound
// sampler object OVERRIDES every glTexParameter setting on the texture. The
// backend's own changelog calls this out as breaking. So since the ImGui
// upgrade, every glTexParameteri in this file has been silently dead: point-
// magnified images sampled linear, and mipmapped compressed textures lost their
// mips, because ImGui's linear sampler is GL_LINEAR min -- not
// GL_LINEAR_MIPMAP_LINEAR.
//
// The fix is to UNBIND ImGui's sampler around the draw, which restores exactly
// the per-texture semantics this engine has always intended -- rather than
// binding ImGui's own nearest sampler, which is NEAREST on min as well as mag
// and would both change minification and disable mips.
static GLuint gSavedImGuiSampler = 0;

static void GLDrawCallbackUseTextureOwnFilter(const ImDrawList *, const ImDrawCmd *)
{
	ImGui_ImplOpenGL3_RenderState *renderState = ImGui_ImplOpenGL3_GetRenderState();
	if (renderState == NULL || !renderState->UseBindSampler)
		return;   // no sampler object in play; the texture's own state already wins

	gSavedImGuiSampler = (GLuint)renderState->CurrentSampler;
	renderState->CurrentSampler = 0;
	glBindSampler(0, 0);

	// UseTexParameterFilter is deliberately left FALSE. Setting it would make
	// ImGui start writing glTexParameter itself on every draw, clobbering the
	// very per-texture filter this exists to expose.
}

static void GLDrawCallbackRestoreImGuiSampler(const ImDrawList *, const ImDrawCmd *)
{
	ImGui_ImplOpenGL3_RenderState *renderState = ImGui_ImplOpenGL3_GetRenderState();
	if (renderState == NULL || !renderState->UseBindSampler)
		return;

	renderState->CurrentSampler = gSavedImGuiSampler;
	glBindSampler(0, gSavedImGuiSampler);
}

bool CRenderBackendOpenGL4::ReadTexturePixels(void *texture, int w, int h, unsigned int *outRGBA)
{
	if (texture == NULL || outRGBA == NULL || w <= 0 || h <= 0)
		return false;

	// glGetTexImage returns the texture in its OWN row order, and this engine
	// uploads and renders render targets top-down (CVideoYUVShader inverts its
	// quad precisely so row 0 is the visual top), so no flip here.
	glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)texture);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);
	glBindTexture(GL_TEXTURE_2D, 0);
	return true;
}

bool CRenderBackendOpenGL4::ReadTexturePixelsFloat(void *texture, int w, int h, float *outRGBA)
{
	if (texture == NULL || outRGBA == NULL || w <= 0 || h <= 0)
		return false;

	// GL_FLOAT rather than GL_HALF_FLOAT: the caller's buffer is float, and
	// glGetTexImage converts from the texture's own RGBA16F storage on the way
	// out. Reading an RGBA16F texture as GL_UNSIGNED_BYTE (what the integer
	// sibling above does) would clamp away exactly the above-1.0 values a
	// caller asking for float wants to see -- which is why this is a separate
	// entry point rather than a flag on that one.
	//
	// Same row-order rationale as ReadTexturePixels: no flip.
	glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)texture);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, outRGBA);
	glBindTexture(GL_TEXTURE_2D, 0);
	return true;
}

bool CRenderBackendOpenGL4::ImageNeedsSamplerOverride(CSlrImage *image)
{
	if (image == NULL)
		return false;

	// Point magnification, or mipmapped minification -- ImGui's bound sampler
	// destroys both. compressedMipCount > 1 is the KTX2/BC7/ASTC atlas path,
	// whose whole point is the mip chain.
	return !image->linearScaling || image->compressedMipCount > 1;
}

void CRenderBackendOpenGL4::QueueSamplerForImage(CSlrImage *image)
{
	ImGui::GetWindowDrawList()->AddCallback(GLDrawCallbackUseTextureOwnFilter, NULL);
}

void CRenderBackendOpenGL4::QueueDefaultSampler()
{
	ImGui::GetWindowDrawList()->AddCallback(GLDrawCallbackRestoreImGuiSampler, NULL);
}

bool CRenderBackendOpenGL4::ReadFramebufferPixels(int x, int y, int w, int h, unsigned int *outRGBA)
{
	if (w <= 0 || h <= 0 || outRGBA == NULL)
		return false;

	// The caller's x/y are TOP-DOWN framebuffer coords; GL's origin is
	// BOTTOM-left, so both the read origin and the row order are converted here.
	// The conversion belongs on this side of the seam because it is a GL
	// convention -- Metal textures are already top-down and must NOT be flipped,
	// and doing it on both sides yields a mirrored capture that a "not all zero"
	// assertion happily accepts.
	int fbW = 0, fbH = 0;
	SDL_GetWindowSizeInPixels(mainWindow, &fbW, &fbH);
	int glY = fbH - y - h;
	if (glY < 0) glY = 0;

	std::vector<unsigned int> buf((size_t)w * (size_t)h, 0u);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(x, glY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

	for (int row = 0; row < h; row++)
	{
		const unsigned int *src = &buf[(size_t)(h - 1 - row) * (size_t)w];
		memcpy(&outRGBA[(size_t)row * (size_t)w], src, (size_t)w * sizeof(unsigned int));
	}
	return true;
}

// --- video plane textures -------------------------------------------------

void *CRenderBackendOpenGL4::CreatePlaneTexture(int width, int height, int channels, int bytesPerChannel)
{
	GLuint tex = 0;
	glGenTextures(1, &tex);
	if (tex == 0)
		return NULL;

	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GLint internalFormat = GL_R8;
	GLenum format = GL_RED;
	GLenum type = GL_UNSIGNED_BYTE;
	if (channels == 2)
	{
		internalFormat = (bytesPerChannel == 2) ? GL_RG16 : GL_RG8;
		format = GL_RG;
	}
	else
	{
		internalFormat = (bytesPerChannel == 2) ? GL_R16 : GL_R8;
		format = GL_RED;
	}
	if (bytesPerChannel == 2)
		type = GL_UNSIGNED_SHORT;

	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	return (void *)(uintptr_t)tex;
}

void CRenderBackendOpenGL4::UpdatePlaneTexture(void *tex, const void *data, int width, int height, int stride)
{
	if (tex == NULL || data == NULL)
		return;

	GLuint id = (GLuint)(uintptr_t)tex;
	GLint internalFormat = 0;
	glBindTexture(GL_TEXTURE_2D, id);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);

	GLenum format = (internalFormat == GL_RG8 || internalFormat == GL_RG16) ? GL_RG : GL_RED;
	GLenum type = (internalFormat == GL_R16 || internalFormat == GL_RG16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
	int bytesPerPixel = ((format == GL_RG) ? 2 : 1) * ((type == GL_UNSIGNED_SHORT) ? 2 : 1);

	// Source rows may be padded; GL_UNPACK_ROW_LENGTH is in PIXELS, not bytes.
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (stride > 0) ? (stride / bytesPerPixel) : 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
	// Restore both defaults. Callers used to do this themselves; now that the
	// upload lives behind the seam, leaving global GL state modified would be an
	// action at a distance for whatever draws next.
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void CRenderBackendOpenGL4::DeletePlaneTexture(void *tex)
{
	if (tex == NULL)
		return;
	GLuint id = (GLuint)(uintptr_t)tex;
	glDeleteTextures(1, &id);
}

// --- CM-E display LUT (3D) ------------------------------------------------

void *CRenderBackendOpenGL4::CreateLutTexture3D(int edge)
{
	GLuint tex = 0;
	glGenTextures(1, &tex);
	if (tex == 0)
		return NULL;

	glBindTexture(GL_TEXTURE_3D, tex);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16, edge, edge, edge, 0, GL_RGBA, GL_UNSIGNED_SHORT, NULL);
	glBindTexture(GL_TEXTURE_3D, 0);
	return (void *)(uintptr_t)tex;
}

void CRenderBackendOpenGL4::UpdateLutTexture3D(void *tex, const void *data, int edge)
{
	if (tex == NULL || data == NULL)
		return;
	glBindTexture(GL_TEXTURE_3D, (GLuint)(uintptr_t)tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, edge, edge, edge, GL_RGBA, GL_UNSIGNED_SHORT, data);
	glBindTexture(GL_TEXTURE_3D, 0);
}

void CRenderBackendOpenGL4::DeleteLutTexture3D(void *tex)
{
	if (tex == NULL)
		return;
	GLuint id = (GLuint)(uintptr_t)tex;
	glDeleteTextures(1, &id);
}

#include "../../Video/CGLRenderTarget.h"
#include "../../Video/CVideoYUVShader.h"

CRenderTarget *CRenderBackendOpenGL4::CreateRenderTarget()
{
	return new CGLRenderTarget();
}

CVideoYUVConverter *CRenderBackendOpenGL4::CreateVideoYUVConverter()
{
	return new CVideoYUVShader();
}
