#include "CRenderBackendOpenGL4.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "CSlrImage.h"
#include "CImageData.h"
#include "imgui_impl_sdl2.h"
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

	int windowFlags = (SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
//	if (maximized)
//		windowFlags |= SDL_WINDOW_MAXIMIZED;
	mainWindow = SDL_CreateWindow(title, x, y, w, h, (SDL_WindowFlags)windowFlags);
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

	SDL_GL_SwapWindow(mainWindow);
	ASSERT_OPENGL();

	SDL_GL_SetSwapInterval(1); // Enable vsync
	ASSERT_OPENGL();

}

void CRenderBackendOpenGL4::InitRenderPipeline()
{
	ImGui_ImplSDL2_InitForOpenGL(mainWindow, glContext);
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
	SDL_GL_SwapWindow(mainWindow);
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

	u8 *data = image->loadImageData->getRGBAResultData();
//	LOGD("CRenderBackendOpenGL4::CreateTexture: width=%d height=%d image=%x image->loadImageData=%x", image->rasterWidth, image->rasterHeight, image, image->loadImageData, data);

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

	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	ASSERT_OPENGL();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->rasterWidth, image->rasterHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->loadImageData->getRGBAResultData());
	ASSERT_OPENGL();

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

	u8 *data = image->loadImageData->getRGBAResultData();
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
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->loadImageData->width, image->loadImageData->height, GL_RGBA, GL_UNSIGNED_BYTE, image->loadImageData->getRGBAResultData());
	ASSERT_OPENGL();
}

void CRenderBackendOpenGL4::DeleteTexture(CSlrImage *image)
{
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
	SDL_GL_DeleteContext(glContext);
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
