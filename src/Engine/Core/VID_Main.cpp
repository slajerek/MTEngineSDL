#include "SYS_Defs.h"
#include "GUI_Main.h"
#include "SYS_Main.h"
#include "MT_VERSION.h"
#include "VID_ImageBinding.h"
#include <SDL_keyboard.h>
#include "SYS_FileSystem.h"
#include "SYS_Platform.h"
#include "SYS_SharedMemory.h"

#if defined(MACOS)
#include "SYS_MacOSWrapper.h"
#endif

#if defined(WIN32)
#include "SDL_syswm.h"
#endif

#include <vector>

#if !defined(WIN32)
#include <sys/time.h>
#endif

#include "MT_API.h"
#include "GUI_Main.h"
#include "GAM_GamePads.h"

int SCREEN_WIDTH;
int SCREEN_HEIGHT;
int SCREEN_FPS = 50;
int SCREEN_TICK_PER_FRAME = 1000 / SCREEN_FPS;

SDL_GLContext glContext;
SDL_Window* gMainWindow = NULL;

SDL_Window *VID_GetMainSDLWindow()
{
	return gMainWindow;
}

//
//https://en.wikibooks.org/wiki/OpenGL_Programming/Modern_OpenGL_Introduction
//https://en.wikibooks.org/wiki/OpenGL_Programming/Modern_OpenGL_Tutorial_02
//https://ourmachinery.com/post/dpi-aware-imgui/

// TODO: gCurrentFrameTime
u64 gCurrentFrameTime;

void VID_SetupShaders();

void VID_Init()
{
//	SCREEN_FPS = MT_GetDefaultFPS();

	const char *glslVersion = VID_GetGlSlVersion();
	
	LOGD("request %s", glslVersion);
	
	// Create window with graphics context
	const char *windowTitle = MT_GetMainWindowTitle();

	int x = SDL_WINDOWPOS_CENTERED;
	int y = SDL_WINDOWPOS_CENTERED;
	int width = 640;
	int height = 35;
	VID_GetStartupMainWindowPosition(&x, &y, &width, &height);

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	gMainWindow = SDL_CreateWindow(windowTitle, x, y, width, height, window_flags);
	
	LOGD("gMainWindow is %x", gMainWindow);
	
	if (gMainWindow == NULL)
	{
		LOGError( "Failed to create SDL Window. This is fatal! Error=%s\n", SDL_GetError() );
		return;
	}

	//	SDL_DisplayMode current;
	//	SDL_GetCurrentDisplayMode(0, &current);

	glContext = SDL_GL_CreateContext(gMainWindow);
	if (glContext == NULL)
	{
		LOGError( "Failed to create SDL GL Context. This is fatal! Error=%s\n", SDL_GetError() );
		return;
	}
	SDL_GL_MakeCurrent(gMainWindow, glContext);
	SDL_GL_SetSwapInterval(1); // Enable vsync

	LOGD("glContext is %x", glContext);
	
	// Initialize OpenGL loader. NOTE: ImGui now has own loader too
	int err = gl3wInit();
	
	if (err != 0)
	{
		LOGError("Failed to initialize OpenGL loader gl3w, error code=%d", err);
		return;
	}
	
	VID_InitImageBindings();
	
	// Setup Dear ImGui binding
	IMGUI_CHECKVERSION();
	ImGuiContext *imGuiContext = ImGui::CreateContext();
	if (imGuiContext == NULL)
	{
		LOGError("ImGui context is NULL");
		return;
	}
	
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	
	char *iniFileName = new char[PATH_MAX];
	sprintf(iniFileName, "%s%s", gCPathToSettings, "imgui.ini");
	
	LOGM("ImGui ini settings path=%s", iniFileName);
	io.IniFilename = (const char*)iniFileName;
	
//	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
	//io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoTaskBarIcons;
	//io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoMerge;
//	io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;
//	io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
	io.ConfigWindowsMoveFromTitleBarOnly = true;
	
	// Setup style
	//	ImGui::StyleColorsClassic();
	ImGui::StyleColorsDark();
	
	
	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
	ImGuiStyle& style = ImGui::GetStyle();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}
	
	ImVec4 colorWindow = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = colorWindow;
	
	ImGui_ImplSDL2_InitForOpenGL(gMainWindow, glContext);
	ImGui_ImplOpenGL3_Init(glslVersion);

	// Setup shaders
	VID_SetupShaders();
	
	// Allow drag'n'drop
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

#if defined(WIN32)
	SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
}

int quitKeyCode = -1;
bool quitIsShift = false;
bool quitIsAlt = false;
bool quitIsControl = false;

void SYS_SetQuitKey(int keyCode, bool isShift, bool isAlt, bool isControl)
{
	quitKeyCode = keyCode;
	quitIsShift = isShift;
	quitIsAlt = isAlt;
	quitIsControl = isControl;
}

const char *VID_GetGlSlVersion()
{
	LOGD("VID_GetGlSlVersion");
	
	// Decide GL+GLSL versions
#if __APPLE__
	LOGD("VID_GetGlSlVersion: Apple: version 150");
	// GL 3.2 Core + GLSL 150
//	const char* glsl_version = MT_GetGLSLVersion();
	const char* glsl_version = "#version 150";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#elif defined(LINUX)
	LOGD("VID_GetGlSlVersion: Linux: version 130");
	// GL 3.0 + GLSL 130
//	const char* glsl_version = MT_GetGLSLVersion();
	const char* glsl_version = "#version 130";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
	LOGD("VID_GetGlSlVersion: other: version 130");
	
	// GL 3.0 + GLSL 130
//	const char* glsl_version = MT_GetGLSLVersion();
	const char* glsl_version = "#version 130";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	
#endif
		
	return glsl_version;
}

void VID_SetupShaders()
{
	GLint compile_ok = GL_FALSE;
	
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	const char *vs_source =
	"#version 100\n"
//	"precision mediump float;	"
	"attribute vec2 coord2d;                  "
	"void main(void) {                        "
	"  gl_Position = vec4(coord2d, 0.0, 1.0); "
	"}";
	glShaderSource(vs, 1, &vs_source, NULL);
	glCompileShader(vs);
	glGetShaderiv(vs, GL_COMPILE_STATUS, &compile_ok);
	
	if (!compile_ok)
	{
		GLint maxLength = 0;
		glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &maxLength);
		
		// The maxLength includes the NULL character
		std::vector<GLchar> errorLog(maxLength);
		glGetShaderInfoLog(vs, maxLength, &maxLength, &errorLog[0]);
		
		char *str = new char[maxLength];
		for (int i = 0; i < maxLength; i++)
		{
			str[i] = errorLog[i];
		}
		
		LOGError("Shader compile ERROR='%s'", str);
		
		SYS_FatalExit("Can't compile shader");
	}
}

int startTicks = SDL_GetTicks();

// Start counting frames per second
int countedRenderFrames = 0;
int countedLogicFrames = 0;

void VID_Render()
{
//	long t1 = SYS_GetCurrentTimeInMillis();
	ImVec4 clearColor;
	
	if (guiMain->IsViewFullScreen() == false)
	{
		clearColor = ImVec4(11.0f/256.0f, 34.0f/256.0f, 44.0f/255.0f, 1.0f); //0.45f, 0.55f, 0.60f, 1.00f);
	}
	else
	{
		clearColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
	}
	
	float avgFPS = countedRenderFrames / ((SDL_GetTicks() - startTicks) / 1000.f);
	if( avgFPS > 2000000 )
		avgFPS = 0;
	
	
	// Start the Dear ImGui frame
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame(gMainWindow);
	ImGui::NewFrame();

	ImGuiIO& io = ImGui::GetIO();
	
	VID_LockRenderMutex();
	
	// ******************************RENDER
	
	VID_BindImages();
	
	gCurrentFrameTime = VID_GetTickCount();
	
	GUI_Render();
	
	ImGui::EndFrame();
	
	ImGui::Render();
	
	VID_UnlockRenderMutex();
	
	// present framebuffer
//	SDL_GL_MakeCurrent(gMainWindow, glContext);
	
	//		LOGD("io.DisplaySize.x=%5.2f io.DisplaySize.y=%5.2f", io.DisplaySize.x, io.DisplaySize.y);
	glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
	glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
	glClear(GL_COLOR_BUFFER_BIT);
	
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

	SDL_GL_SwapWindow(gMainWindow);
		
	// TODO: LOGIC & RENDER
	countedRenderFrames += 1;
	
	//		doLogic();
	countedLogicFrames += 1;
	
	/*
		// If frame finished early
		int frameTicks = SDL_GetTicks() - fpsCapStartTicks;
		if( frameTicks < SCREEN_TICK_PER_FRAME )
		{
	 //Wait remaining time
	 SDL_Delay( SCREEN_TICK_PER_FRAME - frameTicks );
		}
		*/
	
//	long t2 = SYS_GetCurrentTimeInMillis();
//	LOGD("render took %dms", t2-t1);
}

// TODO: VID_isChangingFullScreenState is required because SDL_filterEventCallback is called on SDL PUSH event during SDL_SetWindowFullscreen and that causes GUI_PostRenderEndFrame tasks to be run twice. This should be fixed by splitting rendering and UI async tasks loop in guiMain->PostRenderEndFrame
bool VID_isChangingFullScreenState = false;

// This is still not fixed in SDL 2.0.18.
// Workaround for: https://stackoverflow.com/questions/34967628/sdl2-window-turns-black-on-resize
// Note, this is also called during going FULL SCREEN, thus the CGuiView::PostRenderEndFrame is called twice
int SDL_filterEventCallback(void *userdata, SDL_Event * event)
{
	if (VID_isChangingFullScreenState)
		return 1;
	
	if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_RESIZED)
	{
		//convert userdata pointer to yours and trigger your own draw function
		//this is called very often now
		
		// TODO: check this, it seems this is indeed called on other thread which is WRONG
		
		//IMPORTANT: Might be called from a different thread, see SDL_SetEventFilter docs
		//((MyApplicationClass *)userdata)->myDrawFunction();
		
		VID_Render();
		
		//return 0 if you don't want to handle this event twice
		return 0;
	}
	
	//important to allow all events, or your SDL_PollEvent doesn't get any event
	return 1;
}

bool eventsLoopWithFpsCap = true;
double gTargetFPS = 60.0f;

//https://gamedev.stackexchange.com/questions/151877/handling-variable-frame-rate-in-sdl2
//
//Uint32 time_step_ms = 1000 / fps_the_game_was_designed_for;
//Uint32 next_game_step = SDL_GetTicks(); // initial value
//
//while(!quit){
//	Uint32 now = SDL_GetTicks();
//
//	// Check so we don't render for no reason (unless vsync is enabled)
//	if(next_game_step <= now || vsync_enabled){
//
//		int computer_is_too_slow_limit = 10; // max number of advances per render, adjust this according to your minimum playable fps
//
//		// Loop until all steps are executed or computer_is_too_slow_limit is reached
//		while((next_game_step <= now) && (computer_is_too_slow_limit--)){
//			AdvanceGameLogicBy1Step();
//			next_game_step += time_step_ms; // count 1 game tick done
//		}
//
//		RenderGame();
//	} else {
//		// we're too fast, wait a bit.
//		if(be_nice_and_dont_burn_the_cpu){
//			SDL_Delay(next_game_tick - now);
//		}
//	}
//}

double previousFrameStep;
double frameTimeStep;
long frameMaxTimeInMillis = 0;

void VID_SetFPS(float fps)
{
	LOGM("VID_SetFPS: %3.2f", fps);
	gTargetFPS = fps;
	
	// to not let processing events starve rendering, we allow max 2 * frame time for events
	frameMaxTimeInMillis = (long)(1000.0 / gTargetFPS) * 2;

	double countPerSecond = SDL_GetPerformanceFrequency();
	frameTimeStep = countPerSecond / gTargetFPS;
	
	LOGD("frameTimeStep=%f", frameTimeStep);

	if (fps != 60.0f)
	{
		eventsLoopWithFpsCap = false;
	}
	previousFrameStep = (double)SDL_GetPerformanceCounter();
	
	/*
	LOGD("Performance counter frequency: %u", SDL_GetPerformanceFrequency());
	u64 start32 = SDL_GetTicks();
	u64 start = SDL_GetPerformanceCounter();
	SDL_Delay(1000);
	u64 now = SDL_GetPerformanceCounter();
	u64 now32 = SDL_GetTicks();
	LOGM("Delay 1 second = %d ms in ticks, %f ms according to performance counter", (now32-start32), (double)((now - start)*1000) / SDL_GetPerformanceFrequency());

	LOGM("%f performance counter", (double)(now - start));

	
//	double p = (1000.0 / (double)(SDL_GetPerformanceFrequency()) * gTargetFPS);
//	double p = (double)SDL_GetPerformanceFrequency() * gTargetFPS;
	
//	LOGD("SDL_GetPerformanceFrequency=%ld p=%f", SDL_GetPerformanceCounter(), p);
	
	//SDL_GetPerformanceCounter
	
	LOGD("tes");*/
	
	//	//dt = (long)((float)1000.0 / (float)fps);
	//	dtf = 1000.0 / (double)fps;
	//
	//	LOGTODO("VID_SetFPS: MacOS not implemented");
}

volatile bool mtQuitApplication = false;
volatile bool mtEventLoopRunning = false;

void VID_ProcessEvents()
{
//	printf("%d VID_ProcessEvents\n", SYS_GetTickCount());
	
	SDL_CaptureMouse(SDL_TRUE);
//			SDL_Window *window = SDL_GetWindowFromID(event.motion.windowID);
//			SDL_GetWindowPosition(window, &windowX, &windowX);

//	SDL_StopTextInput();
	
	// not let event processing starve rendering
	long tFrameMax = SYS_GetTickCount() + frameMaxTimeInMillis;
	
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
//		LOGI("SDL_PollEvent: event.type=%d", event.type);
//		LOGI("SDL_IsTextInputActive=%s", STRBOOL(SDL_IsTextInputActive()));
		if (event.type == SDL_QUIT)
		{
			mtQuitApplication = true;
		}
		
		else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(gMainWindow))
		{
			mtQuitApplication = true;
		}

		else if (event.type == SDL_TEXTINPUT)
		{
			LOGI("VID_ProcessEvents: SDL_TEXTINPUT: %d %x '%s'", event.text.text[0], event.text.text[0], event.text.text);
		}

		else if (event.type == SDL_TEXTEDITING)
		{
			LOGI("VID_ProcessEvents: SDL_TEXTEDITING: %d %x '%s'", event.text.text[0], event.text.text[0], event.text.text);
		}
		
		else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
		{
			LOGI("VID_ProcessEvents: %s: keysym.sym=%d", (event.type == SDL_KEYDOWN ? "KeyDown" : "KeyUp"), event.key.keysym.sym);
			
			// BUG: DOES NOT WORK on macOS and SDL 2.0.10. Problem is that CMD KEYUP is received even though the key is still pressed
			// 		https://github.com/libsdl-org/SDL/issues/5090
			guiMain->isShiftPressed = ((SDL_GetModState() & KMOD_SHIFT) != 0);
			guiMain->isLeftShiftPressed = ((SDL_GetModState() & KMOD_LSHIFT) != 0);
			guiMain->isRightShiftPressed = ((SDL_GetModState() & KMOD_RSHIFT) != 0);

			guiMain->isAltPressed = ((SDL_GetModState() & KMOD_ALT) != 0);
			guiMain->isLeftAltPressed = ((SDL_GetModState() & KMOD_LALT) != 0);
			guiMain->isRightAltPressed = ((SDL_GetModState() & KMOD_LALT) != 0);

			guiMain->isControlPressed = ((SDL_GetModState() & PLATFORM_KMOD_CTRL) != 0);
			guiMain->isLeftControlPressed = ((SDL_GetModState() & PLATFORM_KMOD_LCTRL) != 0);
			guiMain->isRightControlPressed = ((SDL_GetModState() & PLATFORM_KMOD_RCTRL) != 0);

			guiMain->isSuperPressed = ((SDL_GetModState() & PLATFORM_KMOD_GUI) != 0);
			guiMain->isLeftSuperPressed = ((SDL_GetModState() & PLATFORM_KMOD_LGUI) != 0);
			guiMain->isRightSuperPressed = ((SDL_GetModState() & PLATFORM_KMOD_RGUI) != 0);
			
			// this does not work either
//#if defined(MACOS)
//#define PLATFORM_SCANCODE_LCTRL	SDL_SCANCODE_LGUI
//#define PLATFORM_SCANCODE_RCTRL	SDL_SCANCODE_RGUI
//#define PLATFORM_SCANCODE_LGUI	SDL_SCANCODE_LCTRL
//#define PLATFORM_SCANCODE_RGUI	SDL_SCANCODE_RCTRL
//#else
//#define PLATFORM_SCANCODE_LCTRL	SDL_SCANCODE_LCTRL
//#define PLATFORM_SCANCODE_RCTRL	SDL_SCANCODE_RCTRL
//#define PLATFORM_SCANCODE_LGUI	SDL_SCANCODE_LGUI
//#define PLATFORM_SCANCODE_RGUI	SDL_SCANCODE_RGUI
//#endif
//
//			bool isKeyDown = event.type == SDL_KEYDOWN;
//			switch(event.key.keysym.scancode)
//			{
//				case SDL_SCANCODE_LALT:
//					guiMain->isLeftAltPressed = isKeyDown;
//					break;
//				case SDL_SCANCODE_RALT:
//					guiMain->isRightAltPressed = isKeyDown;
//					break;
//				case SDL_SCANCODE_LSHIFT:
//					guiMain->isLeftShiftPressed = isKeyDown;
//					break;
//				case SDL_SCANCODE_RSHIFT:
//					guiMain->isRightShiftPressed = isKeyDown;
//					break;
//				case PLATFORM_SCANCODE_LCTRL:
//					guiMain->isLeftControlPressed = isKeyDown;
//					LOGD("PLATFORM_SCANCODE_LCTRL=%d", isKeyDown);
//					break;
//				case PLATFORM_SCANCODE_RCTRL:
//					guiMain->isRightControlPressed = isKeyDown;
//					LOGD("PLATFORM_SCANCODE_RCTRL=%d", isKeyDown);
//					break;
//				case PLATFORM_SCANCODE_LGUI:
//					guiMain->isLeftSuperPressed = isKeyDown;
//					break;
//				case PLATFORM_SCANCODE_RGUI:
//					guiMain->isRightSuperPressed = isKeyDown;
//					break;
//			}
//
//			guiMain->isAltPressed = guiMain->isLeftAltPressed | guiMain->isRightAltPressed;
//			guiMain->isShiftPressed = guiMain->isLeftShiftPressed | guiMain->isRightShiftPressed;
//			guiMain->isControlPressed = guiMain->isLeftControlPressed | guiMain->isRightControlPressed;
//			guiMain->isSuperPressed = guiMain->isLeftSuperPressed | guiMain->isRightSuperPressed;
			
//			switch(event.key.keysym.scancode)
//			{
//				case SDL_SCANCODE_LALT:
//					LOGTODO("..SDL_SCANCODE_LALT");
//					break;
//				case SDL_SCANCODE_RALT:
//					LOGTODO("..SDL_SCANCODE_RALT");
//					break;
//				case SDL_SCANCODE_LSHIFT:
//					LOGTODO("..SDL_SCANCODE_LSHIFT");
//					break;
//				case SDL_SCANCODE_RSHIFT:
//					LOGTODO("..SDL_SCANCODE_RSHIFT");
//					break;
//				case SDL_SCANCODE_LGUI:
//					LOGTODO("..SDL_SCANCODE_LGUI");
//					break;
//				case SDL_SCANCODE_RGUI:
//					LOGTODO("..SDL_SCANCODE_RGUI");
//					break;
//				case SDL_SCANCODE_LCTRL:
//					LOGTODO("..SDL_SCANCODE_LCTRL");
//					break;
//				case SDL_SCANCODE_RCTRL:
//					LOGTODO("..SDL_SCANCODE_RCTRL");
//					break;
//			}
			
			
			u32 keyCode = event.key.keysym.sym;
			u32 keyValue = SYS_GetShiftedKey(keyCode,
											 guiMain->isShiftPressed,
											 guiMain->isAltPressed,
											 guiMain->isControlPressed,
											 guiMain->isSuperPressed);

			LOGI("keyCode=%d keyValue=%d SDL_GetModState()=%d guiMain->isControlPressed=%d %d", keyCode, keyValue, SDL_GetModState(), guiMain->isControlPressed, ((SDL_GetModState() & KMOD_GUI) != 0));
			
			if (event.type == SDL_KEYDOWN)
			{
				if (event.key.repeat == 0)
				{
					if (guiMain->KeyDown(keyValue))
					{
	//					continue;
					}
				}
				else if (guiMain->KeyDownRepeat(keyValue))
				{
//						continue;
				}
			}
			
			if (event.type == SDL_KEYUP)
			{
				if (event.key.repeat == 0)
				{
					if (guiMain->KeyUp(keyValue))
					{
	//					continue;
					}
				}

//				if (guiMain->KeyUpWithRepeat(keyCode))
//				{
////						continue;
//				}

			}
			
//			LOGD("VID_ProcessEvents: key consumed");
		}
		else if (event.type == SDL_MOUSEBUTTONDOWN)
		{
			LOGI("%d SDL_MOUSEBUTTONDOWN button=%d", SYS_GetTickCount(), event.button.button);
			
			int xPos, yPos;
			SDL_GetGlobalMouseState(&xPos, &yPos);
//			LOGD("SDL_MOUSEBUTTONDOWN: %d %d (%d %d)", event.motion.x, event.motion.y, xPos, yPos);
			
			if (event.button.button == SDL_BUTTON_LEFT)
			{
				guiMain->DoTap(xPos, yPos);
			}
			else if (event.button.button == SDL_BUTTON_RIGHT)
			{
				guiMain->DoRightClick(xPos, yPos);
			}
		}
		else if (event.type == SDL_MOUSEBUTTONUP)
		{
			int xPos, yPos;
			SDL_GetGlobalMouseState(&xPos, &yPos);
//			LOGD("SDL_MOUSEBUTTONUP: %d %d (%d %d)", event.motion.x, event.motion.y, xPos, yPos);
			
			if (event.button.button == SDL_BUTTON_LEFT)
			{
				guiMain->DoFinishTap(xPos, yPos);
			}
			else if (event.button.button == SDL_BUTTON_RIGHT)
			{
				guiMain->DoFinishRightClick(xPos, yPos);
			}
		}
		else if (event.type == SDL_MOUSEMOTION)
		{
			int xPos, yPos;
			u32 button = SDL_GetGlobalMouseState(&xPos, &yPos);
//			LOGD("SDL_MOUSEMOTION: %d %d (%d %d)", event.motion.x, event.motion.y, xPos, yPos);
			
			guiMain->DoNotTouchedMove(xPos, yPos);
			
			if (button & SDL_BUTTON(SDL_BUTTON_LEFT))
			{
				guiMain->DoMove(xPos, yPos);
			}
		}
		else if (event.type == SDL_MOUSEWHEEL)
		{
			guiMain->DoScrollWheel((float)event.wheel.x, (float)event.wheel.y);
		}
		else if (event.type == SDL_WINDOWEVENT)
		{
			VID_StoreMainWindowPosition();
		}
		else if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED
				 || event.type == SDL_CONTROLLERAXISMOTION
				 || event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP)
		{
			GAM_GamePadsEvent(event);
		}
		else if (event.type == SDL_DROPFILE)
		{
			LOGD("SDL_DROPFILE: event.drop.windowId=%d file=%s", event.drop.windowID, event.drop.file);
			guiMain->DoDropFile(event.drop.windowID, event.drop.file);
			SDL_free(event.drop.file);
		}
		
		// TODO: bug? for some reason these do not work on macOS. is this related? https://discourse.libsdl.org/t/events-dont-fire-sdl-multigesture-sdl-mousewheel-osx-10-14-6/27900
		else if (event.type == SDL_FINGERDOWN)
		{
			LOGD("SDL_FINGERDOWN X=%g Y=%g ID=%lld dx=%g dy=%g",
				 event.tfinger.x, event.tfinger.y,
				 event.tfinger.fingerId,
				 event.tfinger.dx, event.tfinger.dy);
		}
		else if (event.type == SDL_FINGERUP)
		{
			LOGD("SDL_FINGERUP X=%g Y=%g ID=%lld dx=%g dy=%g",
				 event.tfinger.x, event.tfinger.y,
				 event.tfinger.fingerId,
				 event.tfinger.dx, event.tfinger.dy);
		}
		else if (event.type == SDL_FINGERMOTION)
		{
			LOGD("SDL_FINGERMOTION X=%g Y=%g ID=%lld dx=%g dy=%g",
				 event.tfinger.x, event.tfinger.y,
				 event.tfinger.fingerId,
				 event.tfinger.dx, event.tfinger.dy);
		}
		else if (event.type == SDL_MULTIGESTURE)
		{
			LOGD("SDL_MULTIGESTURE: x=%f y=%f numFingers=%d dDist=%f", event.mgesture.x, event.mgesture.y, event.mgesture.numFingers, event.mgesture.dDist);
//			guiMain->Zoo
//			event.mgesture.dDist
		}
#if defined(WIN32)
		else if (event.type == SDL_SYSWMEVENT)
		{
			//LOGD("SDL_SYSWMEVENT WPARAM=%x LPARAM=%x", event.syswm.msg->msg.win.wParam, event.syswm.msg->msg.win.lParam);
			if (event.syswm.msg->msg.win.wParam == WM_USER_TRIGGER_WPARAM
				&& event.syswm.msg->msg.win.lParam == WM_USER_TRIGGER_LPARAM)
			{
				mtEngineHandleWM_USER();
			}
		}
#endif
		else
		{
//			LOGWarning("Unknown event.type=%d", event.type);
		}
		
		ImGui_ImplSDL2_ProcessEvent(&event);
		
		long tFrame = SYS_GetTickCount();
		if (tFrame > tFrameMax)
			break;

	}
}

void VID_RenderLoop()
{
	mtEventLoopRunning = true;
	
	SDL_SetEventFilter(SDL_filterEventCallback, NULL);
	
	previousFrameStep = SDL_GetPerformanceCounter();
		
	while (mtQuitApplication == false)
	{
		VID_ProcessEvents();
		
		VID_Render();
		
		GUI_PostRenderEndFrame();
		
		if (eventsLoopWithFpsCap)
		{
			// manual sync
			double now = (double)SDL_GetPerformanceCounter();
			
			int maxLoops = 5;
			int loop = 0;
			
			double currentFrameStep = previousFrameStep;
			
			while (currentFrameStep < now)
			{
				loop++;
				if (loop == maxLoops)
					break;

				currentFrameStep += frameTimeStep;
				
				// DO-LOGIC
				
				VID_ProcessEvents();
			}
		}
	}
	
	mtEventLoopRunning = false;
}

void VID_StopEventsLoop()
{
	mtQuitApplication = true;
}

void VID_SetVSyncScreenRefresh(bool isVSyncRefresh)
{
	// this removes vsync and lowers the frame rate, to pump events to the OS dialogs
	eventsLoopWithFpsCap = !isVSyncRefresh;
}

void VID_Shutdown()
{
	LOGM("VID_Shutdown");
	guiMain->LockMutex();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	
	SDL_GL_DeleteContext(glContext);
	SDL_DestroyWindow(gMainWindow);
}

unsigned long VID_GetTickCount()
{
#if defined(MACOS) || defined(LINUX)
	timeval ts;
	gettimeofday(&ts, 0);
	return (long)(ts.tv_sec * 1000 + (ts.tv_usec / 1000));
#elif defined(WIN32)
	return GetTickCount();
#endif

}

bool VID_IsWindowAlwaysOnTop()
{
	LOGTODO("VID_IsWindowAlwaysOnTop TODO");
	
	/*
	 -// Helper structure we store in the void* PlatformUserData field of each ImGuiViewport to easily retrieve our backend data.
	 -struct ImGui_ImplSDL2_ViewportData
	 -{
	 -       SDL_Window*     Window;
	 -       Uint32          WindowID;
	 -       bool            WindowOwned;
	 -       SDL_GLContext   GLContext;
	 -
	 -       ImGui_ImplSDL2_ViewportData() { Window = NULL; WindowID = 0; WindowOwned = false; GLContext = NULL; }
	 -       ~ImGui_ImplSDL2_ViewportData() { IM_ASSERT(Window == NULL && GLContext == NULL); }
	 -};
	 -
	  //@returns is consumed
	  bool CViewAtariScreen::DoTap(float x, float y)
	  {
			 LOGG("CViewAtariScreen::DoTap:  x=%f y=%f", x, y);
	 -
	 -//     static bool isFullscreen = false;
	 -
	 -       isFullscreen = !isFullscreen;
	 -
	 -//     ImGuiWindow *window = this->imGuiWindow;
	 -//
	 -//     ImGui_ImplSDL2_ViewportData *viewportData = (ImGui_ImplSDL2_ViewportData *)window->Viewport->PlatformUserData;
	 -//
	 -//     SDL_Window *sdlWindow = viewportData->Window;
	 -//
	 -//     if (isFullscreen == false)
	 -//     {
	 -//             SDL_SetWindowFullscreen(sdlWindow, SDL_WINDOW_FULLSCREEN);
	 -//             isFullscreen = true;
	 -//     }
	 -//     else
	 -//     {
	 -//             SDL_SetWindowFullscreen(sdlWindow, 0);
	 -//             isFullscreen = false;
	 -//     }
	 -

	 */
	return false;
}

void VID_SetWindowAlwaysOnTop(bool isOnTop)
{
	LOGTODO("VID_SetWindowAlwaysOnTop TODO");
}

void VID_SetWindowAlwaysOnTopTemporary(bool isOnTop)
{
	LOGTODO("VID_SetWindowAlwaysOnTopTemporary TODO");
}

void VID_SetClipping(int x, int y, int sizeX, int sizeY)
{
	ImVec2 p1(x, y);
	ImVec2 p2(x + sizeX, y + sizeY);
	
	ImGui::PushClipRect(p1, p2, true);
	return;
}

void VID_ResetClipping()
{
	ImGui::PopClipRect();
	return;
}

void VID_ResetLogicClock()
{
	LOGTODO("void VID_ResetLogicClock();");
}

void GUI_GetRealScreenPixelSizes(double *pixelSizeX, double *pixelSizeY)
{
	SYS_FatalExit("TODO: GUI_GetRealScreenPixelSizes");
//	LOGD("GUI_GetRealScreenPixelSizes");
//	
//	LOGD("  SCREEN_WIDTH=%f SCREEN_HEIGHT=%f  |  SCREEN_SCALE=%f",
//		 SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_SCALE);
//	LOGD("  viewPortSizeX=%d viewPortSizeY=%d |  viewPortStartX=%d viewPortStartY=%d",
//		 viewPortSizeX, viewPortSizeY, viewPortStartX, viewPortStartY);
//	
//	LOGD("... calc pixel size");
//	
//	*pixelSizeX = (double)SCREEN_WIDTH / (double)viewPortSizeX;
//	*pixelSizeY = (double)SCREEN_HEIGHT / (double)viewPortSizeY;
//	
//	LOGD("  pixelSizeX=%f pixelSizeY=%f", *pixelSizeX, *pixelSizeY);
//	
//	LOGD("GUI_GetRealScreenPixelSizes done");
}

bool VID_IsMainApplicationWindowFullScreen()
{
	if (SDL_GetWindowFlags(gMainWindow) & SDL_WINDOW_FULLSCREEN)
		return true;
	
	if (SDL_GetWindowFlags(gMainWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP)
		return true;
	
#if defined(MACOS)
	if (MACOS_IsApplicationFullScreen())
		return true;
#endif
	
	return false;
}

void VID_SetMainApplicationWindowFullScreen(bool isFullScreen)
{
	VID_isChangingFullScreenState = true;
	
	if (isFullScreen)
	{
		SDL_SetWindowFullscreen(gMainWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	else
	{
		SDL_SetWindowFullscreen(gMainWindow, 0);
	}
	
	VID_isChangingFullScreenState = false;
}

// BUG: SDL_ShowCursor does not work. This is implemented in SYS_Platform
//void VID_ShowMouseCursor()
//{
//	LOGD("VID_ShowMouseCursor");
//	SDL_ShowCursor(SDL_ENABLE);
//}
//
//void VID_HideMouseCursor()
//{
//	LOGD("VID_HideMouseCursor");
//	SDL_ShowCursor(SDL_DISABLE);
//}

//bool VID_IsMouseCursorVisible()
//{
//	if (SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE)
//	{
//		return true;
//	}
//
//	return false;
//}


void VID_LockRenderMutex()
{
	guiMain->LockMutex(); //"SYS_LockRenderMutex");
}

void VID_UnlockRenderMutex()
{
	guiMain->UnlockMutex(); //"SYS_LockRenderMutex");
}

float VID_GetScreenWidth()
{
	return SCREEN_WIDTH;
}

float VID_GetScreenHeight()
{
	return SCREEN_HEIGHT;
}
