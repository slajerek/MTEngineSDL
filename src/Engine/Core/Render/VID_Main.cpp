#include "SYS_Defs.h"
#include "GUI_Main.h"
#include "SYS_Main.h"
#include "SYS_DefaultConfig.h"
#include "MT_VERSION.h"
#include "VID_ImageBinding.h"
#include <SDL_keyboard.h>
#include "SYS_FileSystem.h"
#include "SYS_Platform.h"
#include "SYS_SharedMemory.h"
#include "CRenderBackendOpenGL4.h"
#include "CGuiView.h"
#include "CLayoutManager.h"
#include "implot.h"

#if defined(MACOS)
#include "SYS_MacOSWrapper.h"
#include "CRenderBackendMetal.h"
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

CRenderBackend *gRenderBackend = NULL;

// default screen width and height
int SCREEN_WIDTH;
int SCREEN_HEIGHT;

SDL_GLContext glContext = NULL;
SDL_Window* gMainWindow = NULL;
u64 gCurrentFrameTime = 0;
u64 gCurrentFrameNumber = 0;

bool gViewportsEnableInitAtStartup = false;

SDL_Window *VID_GetMainSDLWindow()
{
	return gMainWindow;
}

//https://ourmachinery.com/post/dpi-aware-imgui/

bool initWindowMaxmized = false;

void VID_SetupShaders();

void VID_Init()
{
//	SCREEN_FPS = MT_GetDefaultFPS();
	
#if defined(MACOS)
//	gRenderBackend = new CRenderBackendMetal();
	gRenderBackend = new CRenderBackendOpenGL4();
#else
	gRenderBackend = new CRenderBackendOpenGL4();
#endif
	
	// Create window with graphics context
	const char *windowTitle = MT_GetMainWindowTitle();

	int x = SDL_WINDOWPOS_CENTERED;
	int y = SDL_WINDOWPOS_CENTERED;
	int width = 800;
	int height = 450;	// floating windows: 35;
	VID_GetStartupMainWindowPosition(&x, &y, &width, &height, &initWindowMaxmized);
	
	gMainWindow = gRenderBackend->CreateSDLWindow(windowTitle, x, y, width, height, initWindowMaxmized);
	LOGD("gMainWindow is %x", gMainWindow);
	
	if (gMainWindow == NULL)
	{
		LOGError( "Failed to create SDL Window. This is fatal! Error=%s\n", SDL_GetError() );
		return;
	}
		
	//	SDL_DisplayMode current;
	//	SDL_GetCurrentDisplayMode(0, &current);
	
	gRenderBackend->CreateRenderContext();
	
	VID_InitImageBindings();
	
	// Setup Dear ImGui binding
	IMGUI_CHECKVERSION();
	ImGuiContext *imGuiContext = ImGui::CreateContext();
	if (imGuiContext == NULL)
	{
		LOGError("ImGui context is NULL");
		return;
	}
	
	ImPlot::CreateContext();
	
	ImGuiIO& io = ImGui::GetIO();
	
	char *iniFileName = new char[PATH_MAX];
	sprintf(iniFileName, "%s%s", gCPathToSettings, "imgui.ini");
	
	LOGM("ImGui ini settings path=%s", iniFileName);
	io.IniFilename = (const char*)iniFileName;
	
	
//	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
	
	io.ConfigDockingWithShift = gApplicationDefaultConfig->GetBool("uiDockingWithShift", true);
	io.ConfigIsTabBarTriangleHidden = gApplicationDefaultConfig->GetBool("ConfigIsTabBarTriangleHidden", false);
	
	if (VID_IsViewportsEnable())
	{
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
		gViewportsEnableInitAtStartup = true;
	}
	else
	{
		io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;         // Disable Multi-Viewport / Platform Windows
		gViewportsEnableInitAtStartup = false;
	}
	//io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoTaskBarIcons;
//	io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoMerge;
//	io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;
//	io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
	io.ConfigWindowsMoveFromTitleBarOnly = true;
	
	// Setup default style
	int defaultStyle = VID_GetDefaultImGuiStyle();
	VID_SetImGuiStyle((ImGuiStyleType)defaultStyle);
	
	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
	ImGuiStyle& style = ImGui::GetStyle();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}
	
	gRenderBackend->InitRenderPipeline();
	
	// Allow drag'n'drop
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

#if defined(WIN32)
	SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
	
	// Note, SDL_CaptureMouse breaks ImGui starting from SDL 2.0.22. We actually needed to capture mouse to have a DoNotTouchedMove event to GUIs
//	SDL_CaptureMouse(SDL_TRUE);
//	SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");

	//
	SDL_StartTextInput();
	
	//
	gCurrentFrameNumber = 0;
}

void VID_PostInit()
{
	LOGM("VID_PostInit: show window, restore position");
	
	if (initWindowMaxmized)
	{
		SDL_MaximizeWindow(gMainWindow);
	}
	SDL_ShowWindow(gMainWindow);
	VID_RestoreMainWindowPosition();

	LOGD("VID_PostInit: completed");
}

void VID_SetViewportsEnable(bool viewportsEnable)
{
	gApplicationDefaultConfig->SetBool("uiViewportsEnable", &viewportsEnable);

	if (gViewportsEnableInitAtStartup == false && viewportsEnable)
	{
		// if viewports were enabled at startup then ImGuiPlatformIO is already setup, if not, we need to restart app to setup SDL's ImGuiPlatformIO
		CUiMessageBoxCallbackRestartApplication *messageBoxCallback = new CUiMessageBoxCallbackRestartApplication();
		guiMain->ShowMessageBox("Application", "Please restart the application for the floating windows setting to take effect.", messageBoxCallback);
		return;
	}
	
	ImGuiIO& io = ImGui::GetIO();
	if (viewportsEnable)
	{
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
		ImGuiContext& g = *GImGui;
		g.FrameCountPlatformEnded = g.FrameCount;
	}
	else
	{
		io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;         // Disable Multi-Viewport / Platform Windows
	}
	
	// refresh style
	VID_ResetImGuiStyle();
}

void VID_ResetImGuiStyle()
{
	ImGuiStyleType style = VID_GetDefaultImGuiStyle();
	VID_SetImGuiStyle(style);
}

bool VID_IsViewportsEnable()
{
	bool viewportsEnable = false;
	gApplicationDefaultConfig->GetBool("uiViewportsEnable", &viewportsEnable, false);
	return viewportsEnable;
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

int startTicks = SDL_GetTicks();

// Start counting frames per second
int countedRenderFrames = 0;
int countedLogicFrames = 0;

void VID_Render()
{
//	SDL_CaptureMouse(SDL_TRUE);

	long t1 = SYS_GetCurrentTimeInMillis();
	ImVec4 clearColor;
	
	if (guiMain->IsViewFullScreen() == false)
	{
		clearColor = ImVec4(11.0f/255.0f, 34.0f/255.0f, 44.0f/255.0f, 1.0f); //0.45f, 0.55f, 0.60f, 1.00f);
	}
	else
	{
		clearColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
	}
	
	float avgFPS = countedRenderFrames / ((SDL_GetTicks() - startTicks) / 1000.f);
	if( avgFPS > 2000000 )
		avgFPS = 0;
	
	// Start the Dear ImGui frame

	// OPENGL3
	gRenderBackend->NewFrame(clearColor);
	
	ImGui_ImplSDL2_NewFrame(gMainWindow);
	
	ImGui::NewFrame();

	ImGuiIO& io = ImGui::GetIO();
	
	VID_LockRenderMutex();
	
	// ******************************RENDER
	
	VID_BindImages();
	
	gCurrentFrameTime = VID_GetTickCount();
	gCurrentFrameNumber++;
	
	GUI_Render();

	ImGui::EndFrame();
	ImGui::Render();
	
	VID_UnlockRenderMutex();
	
	// present framebuffer
	gRenderBackend->PresentFrameBuffer(clearColor);
	
	// TODO: LOGIC & RENDER
	countedRenderFrames += 1;
	
	//		doLogic();
	countedLogicFrames += 1;

	long t2 = SYS_GetCurrentTimeInMillis();
//	LOGD("render took %dms", t2-t1);
	
	// TODO: remove me when confirmed it is working OK
	if (SDL_IsTextInputActive() == SDL_FALSE)
	{
		LOGError("SDL_IsTextInputActive returned false");
		SDL_StartTextInput();
	}
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

// Workaround for a Bug in SDL2, SDL_CaptureMouse does not work since 2.0.22, let's do our own mouse move events
static int prevGlobalMousePosX = 0;
static int prevGlobalMousePosY = 0;

void VID_ProcessEvents()
{
//	LOGD("%d VID_ProcessEvents\n", SYS_GetTickCount());
	
//			SDL_Window *window = SDL_GetWindowFromID(event.motion.windowID);
//			SDL_GetWindowPosition(window, &windowX, &windowX);

//	SDL_StopTextInput();

	// not let event processing starve rendering
	long tFrameMax = SYS_GetTickCount() + frameMaxTimeInMillis;
		
	// check MouseMotion event
	int posX, posY;
	u32 button = VID_GetMousePos(&posX, &posY);
	if (posX != prevGlobalMousePosX || posY != prevGlobalMousePosY)
	{
//		LOGI("VID_ProcessEvents: DoNotTouchedMove: (%d %d) left=%d right=%d", posX, posY,
//			 button & SDL_BUTTON(SDL_BUTTON_LEFT), button & SDL_BUTTON(SDL_BUTTON_RIGHT));
		
		prevGlobalMousePosX = posX;
		prevGlobalMousePosY = posY;
		
		guiMain->DoNotTouchedMove(posX, posY);
		
		if (button & SDL_BUTTON(SDL_BUTTON_LEFT))
		{
			guiMain->DoMove(posX, posY);
		}

		if (button & SDL_BUTTON(SDL_BUTTON_RIGHT))
		{
			guiMain->DoRightClickMove(posX, posY);
		}
	}

	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
//		LOGI("SDL_PollEvent: event.type=%d", event.type);
//		LOGI("SDL_IsTextInputActive=%s", STRBOOL(SDL_IsTextInputActive()));
		if (event.type == SDL_QUIT)
		{
//			LOGM("SDL_QUIT");
			mtQuitApplication = true;
		}
		
		else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(gMainWindow))
		{
//			LOGM("SDL_WINDOWEVENT_CLOSE");
			mtQuitApplication = true;
		}

		else if (event.type == SDL_TEXTINPUT)
		{
			// https://wiki.libsdl.org/Tutorials-TextInput
			LOGI("VID_ProcessEvents: SDL_TEXTINPUT: %d %x '%s'", event.text.text[0], event.text.text[0], event.text.text);
			guiMain->KeyTextInput(event.text.text);
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
			
//			LOGD("shift=%s lshift=%s rshift=%s | alt=%s lalt=%s ralt=%s | ctrl=%s lctrl=%s rctrl=%s | super=%s lsuper=%s rsuper=%s",
//				 STRBOOL(guiMain->isShiftPressed), 	 STRBOOL(guiMain->isLeftShiftPressed),	 STRBOOL(guiMain->isRightShiftPressed),
//				 STRBOOL(guiMain->isAltPressed), 	 STRBOOL(guiMain->isLeftAltPressed), 	 STRBOOL(guiMain->isRightAltPressed),
//				 STRBOOL(guiMain->isControlPressed), STRBOOL(guiMain->isLeftControlPressed), STRBOOL(guiMain->isRightControlPressed),
//				 STRBOOL(guiMain->isSuperPressed), 	 STRBOOL(guiMain->isLeftShiftPressed),   STRBOOL(guiMain->isRightShiftPressed));

			// this code below also bugs in SDL2 and does not work either
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
			LOGI("%lu SDL_MOUSEBUTTONDOWN button=%d", SYS_GetTickCount(), event.button.button);
			
			int posX, posY;
			VID_GetMousePos(&posX, &posY);
//			LOGD("SDL_MOUSEBUTTONDOWN: %d %d (%d %d)", event.motion.x, event.motion.y, posX, posY);
			
			if (event.button.button == SDL_BUTTON_LEFT)
			{
				guiMain->DoTap(posX, posY);
			}
			else if (event.button.button == SDL_BUTTON_RIGHT)
			{
				guiMain->DoRightClick(posX, posY);
			}
		}
		else if (event.type == SDL_MOUSEBUTTONUP)
		{
			int posX, posY;
			VID_GetMousePos(&posX, &posY);
			LOGI("SDL_MOUSEBUTTONUP: %d %d (%d %d)", event.motion.x, event.motion.y, posX, posY);
			
			if (event.button.button == SDL_BUTTON_LEFT)
			{
				guiMain->DoFinishTap(posX, posY);
			}
			else if (event.button.button == SDL_BUTTON_RIGHT)
			{
				guiMain->DoFinishRightClick(posX, posY);
			}
		}
		else if (event.type == SDL_MOUSEMOTION)
		{
			// Oh, why should we use clunky SDL2's events
		}
		else if (event.type == SDL_MOUSEWHEEL)
		{
			LOGI("SDL_MOUSEWHEEL");
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
//			switch(event.type)
//			{
//				case SDL_CONTROLLERDEVICEADDED:
//					LOGI("SDL_CONTROLLERDEVICEADDED");
//					break;
//				case SDL_CONTROLLERDEVICEREMOVED:
//					LOGI("SDL_CONTROLLERDEVICEREMOVED");
//					break;
//				case SDL_CONTROLLERAXISMOTION:
//					LOGI("SDL_CONTROLLERAXISMOTION");
//					break;
//				case SDL_CONTROLLERBUTTONDOWN:
//					LOGI("SDL_CONTROLLERBUTTONDOWN");
//					break;
//				case SDL_CONTROLLERBUTTONUP:
//					LOGI("SDL_CONTROLLERBUTTONUP");
//					break;
//			}

			GAM_GamePadsEvent(event);
		}
		else if (event.type == SDL_DROPFILE)
		{
			LOGM("SDL_DROPFILE: event.drop.windowId=%d file=%s", event.drop.windowID, event.drop.file);
			guiMain->DoDropFile(event.drop.windowID, event.drop.file);
			SDL_free(event.drop.file);
		}
		
		// TODO: bug? for some reason these do not work on macOS. is this implemented at all?
	// TODO: copy paste old MTEngine code with proper implementation of the events queue
//	https://discourse.libsdl.org/t/events-dont-fire-sdl-multigesture-sdl-mousewheel-osx-10-14-6/27900
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
		
		long t1 = SYS_GetTickCount();
		ImGui_ImplSDL2_ProcessEvent(&event);
		long t2 = SYS_GetTickCount();
//		LOGD("ImGui_ImplSDL2_ProcessEvent took %dms", t2-t1);
		
		long tFrame = SYS_GetTickCount();
		if (tFrame > tFrameMax)
			break;

	}
}

void VID_RenderLoop()
{
	LOGM("VID_RenderLoop");
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
		SYS_Sleep(1);
	}
	
	mtEventLoopRunning = false;
}

u32 VID_GetMousePos(int *posX, int *posY)
{
	u32 button = SDL_GetGlobalMouseState(posX, posY);
	
	ImGuiIO& io = ImGui::GetIO();
	if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == false)
	{
		int windowPosX, windowPosY;
		SDL_GetWindowPosition(gMainWindow, &windowPosX, &windowPosY);
		*posX -= windowPosX;
		*posY -= windowPosY;
	}
	
	return button;
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

CRenderBackend *VID_GetRenderBackend()
{
	return gRenderBackend;
}

void VID_Shutdown()
{
	LOGM("VID_Shutdown");
	guiMain->LockMutex();

	gRenderBackend->Shutdown();
	
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	delete gRenderBackend;
	gRenderBackend = NULL;
	
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

SDL_Window *ImGui_ImplSDL2_ImGuiViewportToSDLWindow(ImGuiViewport *imGuiViewport);
// this needs to be moved to imgui_impl_sdl.cpp
//SDL_Window *ImGui_ImplSDL2_ImGuiViewportToSDLWindow(ImGuiViewport *imGuiViewport)
//{
//	ImGui_ImplSDL2_ViewportData *viewportData = (ImGui_ImplSDL2_ViewportData *)imGuiViewport->PlatformUserData;
//	SDL_Window *sdlWindow = viewportData->Window;
//	return sdlWindow;
//}

SDL_Window *VID_GetSDLViewportWindowFromCGuiView(CGuiView *view)
{
	if (!view->imGuiWindow)
		return NULL;
	
	ImGuiViewport *imGuiViewport = view->imGuiWindow->Viewport;

	if (!imGuiViewport)
		return NULL;

	return ImGui_ImplSDL2_ImGuiViewportToSDLWindow(imGuiViewport);
}

SDL_Window *VID_GetSDLWindowFromCGuiView(CGuiView *view)
{
	SDL_Window *window = NULL;
	if (VID_IsViewportsEnable())
	{
		window = VID_GetSDLViewportWindowFromCGuiView(view);
		if (!window)
			window = VID_GetMainSDLWindow();

	}
	else
	{
		window = VID_GetMainSDLWindow();
	}
	return window;
}

bool VID_IsWindowAlwaysOnTop(CGuiView *view)
{
	SDL_Window *window = VID_GetSDLWindowFromCGuiView(view);
	
	if (!window)
	{
		LOGError("VID_IsWindowAlwaysOnTop: SDL_Window for view=%s is NULL", view->name);
		return false;
	}

	// check main window
	Uint32 flags = SDL_GetWindowFlags(window);
	
	if (flags & SDL_WINDOW_ALWAYS_ON_TOP)
	{
		return true;
	}
	
	return false;
}

void VID_SetWindowAlwaysOnTop(CGuiView *view, bool isOnTop)
{
	SDL_Window *window = VID_GetSDLWindowFromCGuiView(view);
	
	if (!window)
	{
		LOGError("VID_SetWindowAlwaysOnTop: SDL_Window for view=%s is NULL", view->name);
		return;
	}
	
	SDL_SetWindowAlwaysOnTop(window, isOnTop ? SDL_TRUE:SDL_FALSE);
}

bool VID_IsMainWindowAlwaysOnTop()
{
	SDL_Window *window = VID_GetMainSDLWindow();
	
	// check main window
	Uint32 flags = SDL_GetWindowFlags(window);
	
	if (flags & SDL_WINDOW_ALWAYS_ON_TOP)
	{
		return true;
	}
	
	return false;
}

void VID_SetMainWindowAlwaysOnTop(bool isOnTop)
{
	SDL_Window *window = VID_GetMainSDLWindow();
	SDL_SetWindowAlwaysOnTop(window, isOnTop ? SDL_TRUE:SDL_FALSE);
}

void VID_SetMainWindowAlwaysOnTopTemporary(bool isOnTop)
{
	SDL_Window *window = VID_GetMainSDLWindow();
	SDL_SetWindowAlwaysOnTop(window, isOnTop ? SDL_TRUE:SDL_FALSE);
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

u64 VID_GetCurrentFrameNumber()
{
	return gCurrentFrameNumber;
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

void VID_RaiseMainWindow()
{
	SDL_RaiseWindow(gMainWindow);
}

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
	int displayNum = 0;
	
//	SDL_Rect r1;
//	SDL_GetDisplayBounds(n, &r1);
//
	SDL_Rect r2;
	SDL_GetDisplayUsableBounds(displayNum, &r2);
	return r2.w;
}

float VID_GetScreenHeight()
{
	int displayNum = 0;
	SDL_Rect r2;
	SDL_GetDisplayUsableBounds(displayNum, &r2);
	return r2.h;
}

void VID_SetMainWindowTitle(const char *title)
{
	SDL_SetWindowTitle(gMainWindow, title);
}

ImGuiStyleType VID_GetDefaultImGuiStyle()
{
	int style = ImGuiStyleType::IMGUI_STYLE_DARK;
	gApplicationDefaultConfig->GetInt("uiImGuiStyle", &style, style);
	return (ImGuiStyleType)style;
}

void VID_SetDefaultImGuiStyle(ImGuiStyleType imGuiStyleType)
{
	int style = imGuiStyleType;
	gApplicationDefaultConfig->SetInt("uiImGuiStyle", &style);
	VID_SetImGuiStyle(imGuiStyleType);
}

void VID_SetImGuiStyle(ImGuiStyleType imGuiStyleType)
{
	if (imGuiStyleType == IMGUI_STYLE_DARK)
	{
		ImGui::StyleColorsDark();
	}
	else if (imGuiStyleType == IMGUI_STYLE_LIGHT)
	{
		ImGui::StyleColorsLight();
	}
	else if (imGuiStyleType == IMGUI_STYLE_CLASSIC)
	{
		ImGui::StyleColorsClassic();
	}

	// Color scheme
//	 ImGuiStyle& style = ImGui::GetStyle();
//	 style.Colors[ImGuiCol_TitleBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.6f);
//	 style.Colors[ImGuiCol_TitleBgActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
//	 style.Colors[ImGuiCol_MenuBarBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
//	 style.Colors[ImGuiCol_Header] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
//	 style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

	else if (imGuiStyleType == IMGUI_STYLE_INTELIJ)
	{
		//---------------
		//--------------- INTELIJ
		//---------------
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 5.3f;
		style.GrabRounding = style.FrameRounding = 2.3f;
		style.ScrollbarRounding = 5.0f;
		style.FrameBorderSize = 1.0f;
		style.ItemSpacing.y = 6.5f;

		style.Colors[ImGuiCol_Text]                  = {0.73333335f, 0.73333335f, 0.73333335f, 1.00f};
		style.Colors[ImGuiCol_TextDisabled]          = {0.34509805f, 0.34509805f, 0.34509805f, 1.00f};
		style.Colors[ImGuiCol_WindowBg]              = {0.23529413f, 0.24705884f, 0.25490198f, 0.94f};
		style.Colors[ImGuiCol_ChildBg]               = {0.23529413f, 0.24705884f, 0.25490198f, 0.00f};
		style.Colors[ImGuiCol_PopupBg]               = {0.23529413f, 0.24705884f, 0.25490198f, 0.94f};
		style.Colors[ImGuiCol_Border]                = {0.33333334f, 0.33333334f, 0.33333334f, 0.50f};
		style.Colors[ImGuiCol_BorderShadow]          = {0.15686275f, 0.15686275f, 0.15686275f, 0.00f};
		style.Colors[ImGuiCol_FrameBg]               = {0.16862746f, 0.16862746f, 0.16862746f, 0.54f};
		style.Colors[ImGuiCol_FrameBgHovered]        = {0.453125f, 0.67578125f, 0.99609375f, 0.67f};
		style.Colors[ImGuiCol_FrameBgActive]         = {0.47058827f, 0.47058827f, 0.47058827f, 0.67f};
		style.Colors[ImGuiCol_TitleBg]               = {0.04f, 0.04f, 0.04f, 1.00f};
		style.Colors[ImGuiCol_TitleBgCollapsed]      = {0.16f, 0.29f, 0.48f, 1.00f};
		style.Colors[ImGuiCol_TitleBgActive]         = {0.00f, 0.00f, 0.00f, 0.51f};
		style.Colors[ImGuiCol_MenuBarBg]             = {0.27058825f, 0.28627452f, 0.2901961f, 0.80f};
		style.Colors[ImGuiCol_ScrollbarBg]           = {0.27058825f, 0.28627452f, 0.2901961f, 0.60f};
		style.Colors[ImGuiCol_ScrollbarGrab]         = {0.21960786f, 0.30980393f, 0.41960788f, 0.51f};
		style.Colors[ImGuiCol_ScrollbarGrabHovered]  = {0.21960786f, 0.30980393f, 0.41960788f, 1.00f};
		style.Colors[ImGuiCol_ScrollbarGrabActive]   = {0.13725491f, 0.19215688f, 0.2627451f, 0.91f};
		// style.Colors[ImGuiCol_ComboBg]               = {0.1f, 0.1f, 0.1f, 0.99f};
		style.Colors[ImGuiCol_CheckMark]             = {0.90f, 0.90f, 0.90f, 0.83f};
		style.Colors[ImGuiCol_SliderGrab]            = {0.70f, 0.70f, 0.70f, 0.62f};
		style.Colors[ImGuiCol_SliderGrabActive]      = {0.30f, 0.30f, 0.30f, 0.84f};
		style.Colors[ImGuiCol_Button]                = {0.33333334f, 0.3529412f, 0.36078432f, 0.49f};
		style.Colors[ImGuiCol_ButtonHovered]         = {0.21960786f, 0.30980393f, 0.41960788f, 1.00f};
		style.Colors[ImGuiCol_ButtonActive]          = {0.13725491f, 0.19215688f, 0.2627451f, 1.00f};
		style.Colors[ImGuiCol_Header]                = {0.33333334f, 0.3529412f, 0.36078432f, 0.53f};
		style.Colors[ImGuiCol_HeaderHovered]         = {0.453125f, 0.67578125f, 0.99609375f, 0.67f};
		style.Colors[ImGuiCol_HeaderActive]          = {0.47058827f, 0.47058827f, 0.47058827f, 0.67f};
		style.Colors[ImGuiCol_Separator]             = {0.31640625f, 0.31640625f, 0.31640625f, 1.00f};
		style.Colors[ImGuiCol_SeparatorHovered]      = {0.31640625f, 0.31640625f, 0.31640625f, 1.00f};
		style.Colors[ImGuiCol_SeparatorActive]       = {0.31640625f, 0.31640625f, 0.31640625f, 1.00f};
		style.Colors[ImGuiCol_ResizeGrip]            = {1.00f, 1.00f, 1.00f, 0.85f};
		style.Colors[ImGuiCol_ResizeGripHovered]     = {1.00f, 1.00f, 1.00f, 0.60f};
		style.Colors[ImGuiCol_ResizeGripActive]      = {1.00f, 1.00f, 1.00f, 0.90f};
		style.Colors[ImGuiCol_PlotLines]             = {0.61f, 0.61f, 0.61f, 1.00f};
		style.Colors[ImGuiCol_PlotLinesHovered]      = {1.00f, 0.43f, 0.35f, 1.00f};
		style.Colors[ImGuiCol_PlotHistogram]         = {0.90f, 0.70f, 0.00f, 1.00f};
		style.Colors[ImGuiCol_PlotHistogramHovered]  = {1.00f, 0.60f, 0.00f, 1.00f};
		style.Colors[ImGuiCol_TextSelectedBg]        = {0.18431373f, 0.39607847f, 0.79215693f, 0.90f};
		
	}
	else if (imGuiStyleType == IMGUI_STYLE_DARK_ALTERNATIVE)
	{
		//---------------
		// Dark style, nice but to sharp, maybe need some work
		//---------------
		ImGuiStyle &st = ImGui::GetStyle();
		st.FrameBorderSize = 1.0f;
		st.FramePadding = ImVec2(4.0f,2.0f);
		st.ItemSpacing = ImVec2(8.0f,2.0f);
		st.WindowBorderSize = 1.0f;
		st.TabBorderSize = 1.0f;
		st.WindowRounding = 1.0f;
		st.ChildRounding = 1.0f;
		st.FrameRounding = 1.0f;
		st.ScrollbarRounding = 1.0f;
		st.GrabRounding = 1.0f;
		st.TabRounding = 1.0f;

		// Setup style
		ImVec4* colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 0.95f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.12f, 0.12f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.94f);
		colors[ImGuiCol_Border] = ImVec4(0.53f, 0.53f, 0.53f, 0.46f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.85f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.22f, 0.40f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.16f, 0.16f, 0.53f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.48f, 0.48f, 0.48f, 1.00f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.79f, 0.79f, 0.79f, 1.00f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.48f, 0.47f, 0.47f, 0.91f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.56f, 0.55f, 0.55f, 0.62f);
		colors[ImGuiCol_Button] = ImVec4(0.50f, 0.50f, 0.50f, 0.63f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.67f, 0.67f, 0.68f, 0.63f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.26f, 0.26f, 0.63f);
		colors[ImGuiCol_Header] = ImVec4(0.54f, 0.54f, 0.54f, 0.58f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.64f, 0.65f, 0.65f, 0.80f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.25f, 0.25f, 0.80f);
		colors[ImGuiCol_Separator] = ImVec4(0.58f, 0.58f, 0.58f, 0.50f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.81f, 0.81f, 0.81f, 0.64f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.81f, 0.81f, 0.81f, 0.64f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.87f, 0.87f, 0.87f, 0.53f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.87f, 0.87f, 0.87f, 0.74f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.87f, 0.87f, 0.87f, 0.74f);
		colors[ImGuiCol_Tab] = ImVec4(0.01f, 0.01f, 0.01f, 0.86f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
		colors[ImGuiCol_TabActive] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
		// colors[ImGuiCol_DockingPreview] = ImVec4(0.38f, 0.48f, 0.60f, 1.00f);
		// colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.68f, 0.68f, 0.68f, 1.00f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.77f, 0.33f, 1.00f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.87f, 0.55f, 0.08f, 1.00f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.47f, 0.60f, 0.76f, 0.47f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(0.58f, 0.58f, 0.58f, 0.90f);
		colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
		colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
	}
	else if (imGuiStyleType == IMGUI_STYLE_PHOTOSHOP)
	{
		//---------------
		// Photoshop style (the best!)
		//---------------
		ImGuiStyle* style = &ImGui::GetStyle();
		ImVec4* colors = style->Colors;
	
		colors[ImGuiCol_Text]                   = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
		colors[ImGuiCol_TextDisabled]           = ImVec4(0.500f, 0.500f, 0.500f, 1.000f);
		colors[ImGuiCol_WindowBg]               = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
		colors[ImGuiCol_ChildBg]                = ImVec4(0.280f, 0.280f, 0.280f, 0.000f);
		colors[ImGuiCol_PopupBg]                = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
		colors[ImGuiCol_Border]                 = ImVec4(0.266f, 0.266f, 0.266f, 1.000f);
		colors[ImGuiCol_BorderShadow]           = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);
		colors[ImGuiCol_FrameBg]                = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
		colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_FrameBgActive]          = ImVec4(0.280f, 0.280f, 0.280f, 1.000f);
		colors[ImGuiCol_TitleBg]                = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
		colors[ImGuiCol_TitleBgActive]          = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
		colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
		colors[ImGuiCol_MenuBarBg]              = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
		colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
		colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.277f, 0.277f, 0.277f, 1.000f);
		colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
		colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_CheckMark]              = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
		colors[ImGuiCol_SliderGrab]             = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
		colors[ImGuiCol_SliderGrabActive]       = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_Button]                 = ImVec4(1.000f, 1.000f, 1.000f, 0.000f);
		colors[ImGuiCol_ButtonHovered]          = ImVec4(1.000f, 1.000f, 1.000f, 0.156f);
		colors[ImGuiCol_ButtonActive]           = ImVec4(1.000f, 1.000f, 1.000f, 0.391f);
		colors[ImGuiCol_Header]                 = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
		colors[ImGuiCol_HeaderHovered]          = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
		colors[ImGuiCol_HeaderActive]           = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
		colors[ImGuiCol_Separator]              = colors[ImGuiCol_Border];
		colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
		colors[ImGuiCol_SeparatorActive]        = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_ResizeGrip]             = ImVec4(1.000f, 1.000f, 1.000f, 0.250f);
		colors[ImGuiCol_ResizeGripHovered]      = ImVec4(1.000f, 1.000f, 1.000f, 0.670f);
		colors[ImGuiCol_ResizeGripActive]       = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_Tab]                    = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
		colors[ImGuiCol_TabHovered]             = ImVec4(0.352f, 0.352f, 0.352f, 1.000f);
		colors[ImGuiCol_TabActive]              = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
		colors[ImGuiCol_TabUnfocused]           = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
		colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
		// colors[ImGuiCol_DockingPreview]         = ImVec4(1.000f, 0.391f, 0.000f, 0.781f);
		// colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
		colors[ImGuiCol_PlotLines]              = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
		colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_PlotHistogram]          = ImVec4(0.586f, 0.586f, 0.586f, 1.000f);
		colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_TextSelectedBg]         = ImVec4(1.000f, 1.000f, 1.000f, 0.156f);
		colors[ImGuiCol_DragDropTarget]         = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_NavHighlight]           = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.000f, 0.391f, 0.000f, 1.000f);
		colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);
		colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);
	
		style->ChildRounding = 4.0f;
		style->FrameBorderSize = 1.0f;
		style->FrameRounding = 2.0f;
		style->GrabMinSize = 7.0f;
		style->PopupRounding = 2.0f;
		style->ScrollbarRounding = 12.0f;
		style->ScrollbarSize = 13.0f;
		style->TabBorderSize = 1.0f;
		style->TabRounding = 0.0f;
		style->WindowRounding = 4.0f;
	}
	else if (imGuiStyleType == IMGUI_STYLE_CORPORATE_GREY
			 || imGuiStyleType == IMGUI_STYLE_CORPORATE_GREY_3D)
	{
		//---------------
		// CorporateGrey
		//---------------

		 ImGuiStyle & style = ImGui::GetStyle();
		 ImVec4 * colors = style.Colors;
	
		 /// 0 = FLAT APPEARENCE
		 /// 1 = MORE "3D" LOOK
		 int is3D = (imGuiStyleType == IMGUI_STYLE_CORPORATE_GREY) ? 0 : 1;
	
		 colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		 colors[ImGuiCol_TextDisabled]           = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
		 colors[ImGuiCol_ChildBg]                = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		 colors[ImGuiCol_WindowBg]               = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		 colors[ImGuiCol_PopupBg]                = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		 colors[ImGuiCol_Border]                 = ImVec4(0.12f, 0.12f, 0.12f, 0.71f);
		 colors[ImGuiCol_BorderShadow]           = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
		 colors[ImGuiCol_FrameBg]                = ImVec4(0.42f, 0.42f, 0.42f, 0.54f);
		 colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.42f, 0.42f, 0.42f, 0.40f);
		 colors[ImGuiCol_FrameBgActive]          = ImVec4(0.56f, 0.56f, 0.56f, 0.67f);
		 colors[ImGuiCol_TitleBg]                = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
		 colors[ImGuiCol_TitleBgActive]          = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
		 colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.17f, 0.17f, 0.17f, 0.90f);
		 colors[ImGuiCol_MenuBarBg]              = ImVec4(0.335f, 0.335f, 0.335f, 1.000f);
		 colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.24f, 0.24f, 0.24f, 0.53f);
		 colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
		 colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
		 colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
		 colors[ImGuiCol_CheckMark]              = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
		 colors[ImGuiCol_SliderGrab]             = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
		 colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.64f, 0.64f, 0.64f, 1.00f);
		 colors[ImGuiCol_Button]                 = ImVec4(0.54f, 0.54f, 0.54f, 0.35f);
		 colors[ImGuiCol_ButtonHovered]          = ImVec4(0.52f, 0.52f, 0.52f, 0.59f);
		 colors[ImGuiCol_ButtonActive]           = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
		 colors[ImGuiCol_Header]                 = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
		 colors[ImGuiCol_HeaderHovered]          = ImVec4(0.47f, 0.47f, 0.47f, 1.00f);
		 colors[ImGuiCol_HeaderActive]           = ImVec4(0.76f, 0.76f, 0.76f, 0.77f);
		 colors[ImGuiCol_Separator]              = ImVec4(0.000f, 0.000f, 0.000f, 0.137f);
		 colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.700f, 0.671f, 0.600f, 0.290f);
		 colors[ImGuiCol_SeparatorActive]        = ImVec4(0.702f, 0.671f, 0.600f, 0.674f);
		 colors[ImGuiCol_ResizeGrip]             = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
		 colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
		 colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
		 colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
		 colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
		 colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
		 colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
		 colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.73f, 0.73f, 0.73f, 0.35f);
		 colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
		 colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
		 colors[ImGuiCol_NavHighlight]           = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		 colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
		 colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
	
		 style.PopupRounding = 3;
	
		 style.WindowPadding = ImVec2(4, 4);
		 style.FramePadding  = ImVec2(6, 4);
		 style.ItemSpacing   = ImVec2(6, 2);
	
		 style.ScrollbarSize = 18;
	
		 style.WindowBorderSize = 1;
		 style.ChildBorderSize  = 1;
		 style.PopupBorderSize  = 1;
		 style.FrameBorderSize  = is3D;
	
		 style.WindowRounding    = 3;
		 style.ChildRounding     = 3;
		 style.FrameRounding     = 3;
		 style.ScrollbarRounding = 2;
		 style.GrabRounding      = 3;
	
		 #ifdef IMGUI_HAS_DOCK
		 	style.TabBorderSize = is3D;
		 	style.TabRounding   = 3;
	
		 	colors[ImGuiCol_DockingEmptyBg]     = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
		 	colors[ImGuiCol_Tab]                = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		 	colors[ImGuiCol_TabHovered]         = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
		 	colors[ImGuiCol_TabActive]          = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
		 	colors[ImGuiCol_TabUnfocused]       = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		 	colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
		 	colors[ImGuiCol_DockingPreview]     = ImVec4(0.85f, 0.85f, 0.85f, 0.28f);
	
		 	if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		 	{
		 		style.WindowRounding = 0.0f;
		 		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		 	}
		 #endif

		
	}
	else if (imGuiStyleType == IMGUI_STYLE_NICE)
	{
		//---------------
		// Nice, but require Ruda-Bold fonts!
		//---------------
		ImGui::GetStyle().FrameRounding = 4.0f;
		ImGui::GetStyle().GrabRounding = 4.0f;

		ImVec4* colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.36f, 0.42f, 0.47f, 1.00f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
		colors[ImGuiCol_Border] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.20f, 0.28f, 1.00f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.09f, 0.12f, 0.14f, 1.00f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.12f, 0.14f, 0.65f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.39f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.18f, 0.22f, 0.25f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.09f, 0.21f, 0.31f, 1.00f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.37f, 0.61f, 1.00f, 1.00f);
		colors[ImGuiCol_Button] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
		colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.29f, 0.55f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
		colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
		colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
		colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
	}
}
