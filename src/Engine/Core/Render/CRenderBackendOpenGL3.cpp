#include "CRenderBackendOpenGL3.h"
#include "SYS_Main.h"
#include "CSlrImage.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"
#include <GL/gl3w.h>
#include <vector>

CRenderBackendOpenGL3::CRenderBackendOpenGL3()
: CRenderBackend("OpenGL 3")
{
}

SDL_Window *CRenderBackendOpenGL3::CreateSDLWindow(const char *title, int x, int y, int w, int h)
{
	GetGlSlVersion();
	LOGD("GlSl version is %s", glslVersionString);
	
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	SDL_WindowFlags windowFlags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	mainWindow = SDL_CreateWindow(title, x, y, w, h, windowFlags);
	return mainWindow;
}

void CRenderBackendOpenGL3::CreateRenderContext()
{
	glContext = SDL_GL_CreateContext(mainWindow);
	if (glContext == NULL)
	{
		LOGError("CRenderBackendOpenGL3: Failed to create SDL GL Context. This is fatal! Error=%s\n", SDL_GetError());
		return;
	}
	SDL_GL_MakeCurrent(mainWindow, glContext);
	SDL_GL_SetSwapInterval(1); // Enable vsync

	LOGD("CRenderBackendOpenGL3: glContext is %x", glContext);
	
	// Initialize OpenGL loader. NOTE: ImGui now has own loader too
	int err = gl3wInit();
	
	if (err != 0)
	{
		LOGError("CRenderBackendOpenGL3: Failed to initialize OpenGL loader gl3w, error code=%d", err);
		return;
	}
}

void CRenderBackendOpenGL3::InitRenderPipeline()
{
	ImGui_ImplSDL2_InitForOpenGL(mainWindow, glContext);
	
	ImGui_ImplOpenGL3_Init(glslVersionString);
}

void CRenderBackendOpenGL3::GetGlSlVersion()
{
	LOGD("CRenderBackendOpenGL3::GetGlSlVersion");
	
	// Decide GL+GLSL versions
#if __APPLE__
	LOGD("CRenderBackendOpenGL3::GetGlSlVersion: Apple: version 150");
	// GL 3.2 Core + GLSL 150
	glslVersionString = "#version 150";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#elif defined(LINUX)
	LOGD("CRenderBackendOpenGL3::GetGlSlVersion: Linux: version 130");
	// GL 3.0 + GLSL 130
	glslVersionString = "#version 130";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
	LOGD("CRenderBackendOpenGL3::GetGlSlVersion: other: version 130");
	
	// GL 3.0 + GLSL 130
	glslVersionString = "#version 130";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
}

void CRenderBackendOpenGL3::CreateFontsTexture()
{
	ImGui_ImplOpenGL3_CreateFontsTexture();
}

void CRenderBackendOpenGL3::NewFrame(ImVec4 clearColor)
{
	ImGui_ImplOpenGL3_NewFrame();
}

void CRenderBackendOpenGL3::PresentFrameBuffer(ImVec4 clearColor)
{
	ImGuiIO& io = ImGui::GetIO();
//	SDL_GL_MakeCurrent(gMainWindow, glContext);
	
	//		LOGD("io.DisplaySize.x=%5.2f io.DisplaySize.y=%5.2f", io.DisplaySize.x, io.DisplaySize.y);
	glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
	glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
	glClear(GL_COLOR_BUFFER_BIT);

	///
	
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

	SDL_GL_SwapWindow(mainWindow);
}

void CRenderBackendOpenGL3::Shutdown()
{
	ImGui_ImplOpenGL3_Shutdown();
}

void CRenderBackendOpenGL3::CreateTexture(CSlrImage *image)
{
	GLuint textureId;
	glGenTextures(1, &textureId);
	glBindTexture(GL_TEXTURE_2D, textureId);
	
	if (image->linearScaling)
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	}

	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->rasterWidth, image->rasterHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->loadImageData->getRGBAResultData());
	
	image->texturePtr = (void*)(intptr_t)textureId;
}

void CRenderBackendOpenGL3::UpdateTextureLinearScaling(CSlrImage *image)
{
	glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)image->texturePtr);
	if (image->linearScaling)
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	}
}

void CRenderBackendOpenGL3::ReBindTexture(CSlrImage *image)
{
	glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)image->texturePtr);
	
	if (image->linearScaling)
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	}
	
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	
//	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
//	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->rasterWidth, image->rasterHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->loadImageData->getRGBAResultData());
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->loadImageData->width, image->loadImageData->height, GL_RGBA, GL_UNSIGNED_BYTE, image->loadImageData->getRGBAResultData());
}

void CRenderBackendOpenGL3::DeleteTexture(CSlrImage *image)
{
	GLuint textureId = (intptr_t)image->texturePtr;
	glDeleteTextures(1, &textureId);
}

CRenderBackendOpenGL3::~CRenderBackendOpenGL3()
{
	SDL_GL_DeleteContext(glContext);
}

