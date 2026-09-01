#include "SYS_Defs.h"
#include "Tests/MT_ShaderProbe.h"
#include "VID_Main.h"
#include <thread>
#include "CMTThemeRegistry.h"
#include "MT_UiScale.h"
#include "CIccProfileCodec.h"
#include "ICC_SRGBProfile.h"
#include <atomic>
#if defined(WIN32)
#include <windows.h>   // WM_DISPLAYCHANGE / WM_SETTINGCHANGE for the display-profile hook
#endif
#include "GUI_Main.h"
#include "SYS_Main.h"
#include "SYS_DefaultConfig.h"
#include "SYS_CommandLine.h"   // sysCommandLineArguments (--render-backend, --hdr)
#include <cstring>
#include "MT_VERSION.h"
#include "VID_ImageBinding.h"
#include <SDL3/SDL_keyboard.h>
#include "SYS_FileSystem.h"
#include "SYS_Platform.h"
#include "SYS_SharedMemory.h"
#include "CRenderBackendOpenGL4.h"
#include "CGuiView.h"
#include "CLayoutManager.h"
#include "CMTNativeMenuBar.h"
#include "implot.h"

#if defined(MACOS)
#include "SYS_MacOSWrapper.h"
#include "CRenderBackendMetal.h"
#include <pthread.h>
#endif

#if defined(WIN32)
#include <SDL3/SDL_system.h>
#include "CMTNativeMenuBar.h"
#if defined(MT_RENDER_BACKEND_D3D11)
// S-6. Behind the macro with everything else D3D: with it undefined the
// Windows tree behaves exactly as it did at S-5, which is what lets Task B1
// take an OpenGL baseline of this same tree.
#include "CRenderBackendD3D11.h"
#endif

// The Windows half of VID_GetMaxPotentialHdrHeadroom(). DEFINED (returning
// 1.0) in SYS_WindowsHdr.cpp from Task A2 on, and deliberately OUTSIDE
// MT_RENDER_BACKEND_D3D11: a declaration without a definition does not link,
// and the OpenGL-only Windows tree has to link. Task B4 gives it a real body.
float WINDOWS_GetMaxPotentialHdrHeadroom();

// The Windows half of VID_IsAnyDisplayHdrCapable() -- see that function's
// own comment. Also deliberately outside MT_RENDER_BACKEND_D3D11 and
// defined unconditionally in SYS_WindowsHdr.cpp, same reasoning as above.
bool WINDOWS_IsAnyDisplayHdrCapable();

// Forward-declared: both are called from VID_Init(), defined later in this
// file alongside the rest of the display-profile-change machinery.
static bool SDLCALL VID_WindowsMessageHook(void *userdata, MSG *msg);
static void VID_SeedDisplayProfileDigest();
#endif

#include <vector>

#if !defined(WIN32)
#include <sys/time.h>
#endif

#include "MT_API.h"
#include "GUI_Main.h"
#include "GAM_GamePads.h"

#if MT_ENABLE_IMGUI_TEST_ENGINE
#include "imgui_te_engine.h"
#include "CImGuiTestEngine.h"
#endif

CRenderBackend *gRenderBackend = NULL;

// default screen width and height
int SCREEN_WIDTH;
int SCREEN_HEIGHT;

SDL_GLContext glContext = NULL;
SDL_Window* gMainWindow = NULL;
u64 gCurrentFrameTime = 0;
u64 gCurrentFrameNumber = 0;

bool gViewportsEnableInitAtStartup = false;
bool gHeadlessMode = false;
bool gServiceMode = false;

static ImGuiStyleType gRequestedImGuiStyle = IMGUI_STYLE_DARK;
static ImGuiStyleType gAppDefaultImGuiStyle = IMGUI_STYLE_SYSTEM;
static VID_SystemAppearance gLastSystemAppearance = VID_SYSTEM_APPEARANCE_UNKNOWN;
static VID_DisplayColorGamut gMainWindowRenderColorGamut = VID_DISPLAY_COLOR_GAMUT_SRGB;
static VID_DisplayColorGamut gAppliedDisplayColorGamut = VID_DISPLAY_COLOR_GAMUT_UNKNOWN;
static VID_DisplayColorGamut gAppliedRenderColorGamut = VID_DISPLAY_COLOR_GAMUT_UNKNOWN;
static bool gRenderPipelineInitialized = false;

static void VID_RefreshSystemVisualState();

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
	{
		const char *preferredBackend = VID_GetPreferredRenderBackend();
		if (strcmp(preferredBackend, "metal") == 0)
		{
			LOGM("VID_Init: using Metal render backend");
			gRenderBackend = new CRenderBackendMetal();
		}
		else
		{
			LOGM("VID_Init: using OpenGL4 render backend");
			gRenderBackend = new CRenderBackendOpenGL4();
		}
	}
#elif defined(WIN32) && defined(MT_RENDER_BACKEND_D3D11)
	{
		// D3D11 is now the Windows DEFAULT in a build that has it -- OpenGL is
		// the FALLBACK, taken whenever the device probe fails or the user chose
		// it. VID_GetPreferredRenderBackend() can only return "d3d11" where
		// VID_IsRenderBackendAvailable() says a real device exists.
		//
		// BOTH BACKENDS PROBE BEFORE THEY COMMIT. That used to be true of this
		// one only, on the argument that Metal always exists on a Mac -- fair
		// while Metal was opt-in, wrong once it became the default, because the
		// failure mode is SYS_FatalExit at window creation. See
		// CRenderBackendMetal::IsAvailable().
		//
		// THE PROBE IS NOT REPEATED HERE. It is inside
		// VID_IsRenderBackendAvailable(), which VID_GetPreferredRenderBackend()
		// already consulted -- so a machine with no D3D11 device cannot get
		// "d3d11" out of that call at all, and every UI that asks the same
		// query agrees with what actually happens. Probing again here would be
		// a second answer to the same question, which is exactly how the two
		// could drift.
		const char *preferredBackend = VID_GetPreferredRenderBackend();
		if (strcmp(preferredBackend, "d3d11") == 0)
		{
			LOGM("VID_Init: using Direct3D 11 render backend");
			gRenderBackend = new CRenderBackendD3D11();
		}
		else
		{
			LOGM("VID_Init: using OpenGL4 render backend");
			gRenderBackend = new CRenderBackendOpenGL4();
		}
	}
#else
	LOGM("VID_Init: using OpenGL4 render backend");
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
	VID_ApplyViewportStyleOverrides();
	
	gRenderBackend->InitRenderPipeline();
	gRenderPipelineInitialized = true;
	VID_ApplyMainWindowColorGamut();
	
	// Allow drag'n'drop
	// SDL3: SDL_EventState(type, SDL_ENABLE) -> SDL_SetEventEnabled(type, true).
	SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

#if defined(WIN32)
	// SDL3 REMOVED SDL_SYSWMEVENT along with the whole SDL_syswm.h header --
	// there is no "give me the raw platform message" event any more. The
	// Windows message hook below is now the ONLY way in, and it is enough:
	// SDL_SetWindowsMessageHook receives every message before SDL handles it,
	// which is what VID_WindowsMessageHook wanted the SYSWM event for. So this
	// is a removal with no behaviour change, not a degradation.
	// See VID_WindowsMessageHook: SDL cannot see a recalibration that swaps the
	// profile file while our window keeps focus.
	SDL_SetWindowsMessageHook(VID_WindowsMessageHook, NULL);
	VID_SeedDisplayProfileDigest();
#endif
	
	// Note, SDL_CaptureMouse breaks ImGui starting from SDL 2.0.22. We actually needed to capture mouse to have a DoNotTouchedMove event to GUIs
//	SDL_CaptureMouse(true);
//	SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");

	//
	// SDL3: text input is per-window now (it has to be -- with viewports there
	// is more than one window), so SDL_StartTextInput takes the window.
	SDL_StartTextInput(gMainWindow);
	
	//
	gCurrentFrameNumber = 0;
}

void VID_PostInit()
{
	LOGM("VID_PostInit: show window, restore position");
	
	if (!gHeadlessMode)
	{
		if (initWindowMaxmized)
		{
			SDL_MaximizeWindow(gMainWindow);
		}
		SDL_ShowWindow(gMainWindow);
		VID_RestoreMainWindowPosition();
	}

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

// SDL3: SDL_GetTicks() is Uint64. An int truncates it and goes NEGATIVE at
// 2^31 ms (~24.8 days of uptime), after which the avgFPS division below reads
// as a huge elapsed time and the FPS figure collapses to nonsense. Same class
// as the two fixed in c64d.
u64 startTicks = SDL_GetTicks();

// Start counting frames per second
int countedRenderFrames = 0;
int countedLogicFrames = 0;

#if MT_ENABLE_IMGUI_TEST_ENGINE
static float teInputPrevMouseX = -FLT_MAX;
static float teInputPrevMouseY = -FLT_MAX;

static void VID_ForwardTestEngineInputToGuiMain()
{
	ImGuiTestEngine *te = CImGuiTestEngine::GetEngine();
	if (!te || !ImGuiTestEngine_IsUsingSimulatedInputs(te))
		return;

	ImGuiIO &io = ImGui::GetIO();
	float mx = io.MousePos.x;
	float my = io.MousePos.y;

	// Update modifier state on guiMain
	guiMain->isShiftPressed = io.KeyShift;
	guiMain->isControlPressed = io.KeyCtrl;
	guiMain->isAltPressed = io.KeyAlt;
	guiMain->isSuperPressed = io.KeySuper;

	// Mouse movement
	if (mx != teInputPrevMouseX || my != teInputPrevMouseY)
	{
		guiMain->DoNotTouchedMove((int)mx, (int)my);
		if (io.MouseDown[0])
			guiMain->DoMove((int)mx, (int)my);
		if (io.MouseDown[1])
			guiMain->DoRightClickMove((int)mx, (int)my);
	}

	// Left mouse button transitions
	if (io.MouseClicked[0])
		guiMain->DoTap((int)mx, (int)my);
	if (io.MouseReleased[0])
		guiMain->DoFinishTap((int)mx, (int)my);

	// Right mouse button transitions
	if (io.MouseClicked[1])
		guiMain->DoRightClick((int)mx, (int)my);
	if (io.MouseReleased[1])
		guiMain->DoFinishRightClick((int)mx, (int)my);

	// Scroll wheel
	if (io.MouseWheelH != 0.0f || io.MouseWheel != 0.0f)
		guiMain->DoScrollWheel(io.MouseWheelH, io.MouseWheel);

	teInputPrevMouseX = mx;
	teInputPrevMouseY = my;
}
#endif

// True between ImGui::NewFrame() and ImGui::Render() below. See the guard in
// VID_Render() for why this exists.
static bool gVidRenderInProgress = false;

bool VID_IsRenderingFrame()
{
	return gVidRenderInProgress;
}

void VID_Render()
{
//	SDL_CaptureMouse(true);

	// Re-entrancy guard.
	//
	// VID_Render can be re-entered on the SAME stack, from inside its own
	// GUI_Render(): SDL_filterEventCallback below renders a frame on
	// SDL_EVENT_WINDOW_RESIZED (so the window does not go black during a live
	// drag), and SDL_PushEvent invokes that filter INLINE, not from the event
	// queue. Any code that runs during the frame and makes the OS resize the
	// window synchronously therefore lands back in here mid-frame. The known
	// trigger is mutating a native Win32 menu bar (DrawMenuBar -> non-client
	// recalc -> WM_WINDOWPOSCHANGED), but a SetWindowPos / style change from
	// view code does the same thing.
	//
	// A nested call would issue a second ImGui::NewFrame() while the current
	// frame is still open, and the next frame trips
	//   imgui.cpp: (g.FrameCount == 0 || g.FrameCountEnded == g.FrameCount) &&
	//              "Forgot to call Render() or EndFrame() at the end of the
	//               previous frame?"
	// (an assert in debug builds; undefined ImGui state in release).
	//
	// Skipping the nested render loses nothing: the outer frame is already in
	// flight and will be presented, and the render loop draws again immediately
	// afterwards. Apps should still keep OS-window mutation out of the frame --
	// this is the backstop, not a licence.
	if (gVidRenderInProgress)
	{
		LOGD("VID_Render: re-entrant call skipped (frame already in progress)");
		return;
	}

	struct ScopedRenderFlag
	{
		ScopedRenderFlag()  { gVidRenderInProgress = true;  }
		~ScopedRenderFlag() { gVidRenderInProgress = false; }
	} scopedRenderFlag;

	long t1 = SYS_GetCurrentTimeInMillis();
	VID_RefreshSystemVisualState();
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
	
	ImGui_ImplSDL3_NewFrame();

	ImGui::NewFrame();

#if MT_ENABLE_IMGUI_TEST_ENGINE
	VID_ForwardTestEngineInputToGuiMain();
#endif

	ImGuiIO& io = ImGui::GetIO();

	// Resync modifier state from ImGui's per-frame view of the keyboard.
	// In VID_ProcessEvents we only update guiMain->is*Pressed on SDL
	// KEYDOWN/KEYUP, which fails when SDL silently drops a KEYUP — most
	// commonly when the user holds Shift while drag-docking a window
	// (the macOS+SDL2 drag handover steals the Shift release event), or
	// when the app loses keyboard focus mid-modifier. The stuck modifier
	// then bleeds into every subsequent key: "Q" turns into Shift+Q
	// (transpose-up in the GT2 pattern editor), "F" routes through
	// Shift+F instead of the bare F hex digit, etc.
	//
	// ImGui already does the right thing here. ImGui_ImplSDL2_NewFrame
	// reads SDL_GetKeyboardState directly each frame and writes io.Key*,
	// so io.Key* reflects the live OS state — KEYUP delivery is not on
	// the critical path. Mirroring those into guiMain right after
	// NewFrame keeps the engine's modifier view in lockstep with what
	// ImGui sees, and recovers from any missed KEYUP on the very next
	// frame instead of staying stuck until the user explicitly retaps
	// the modifier.
	guiMain->isShiftPressed   = io.KeyShift;
	guiMain->isControlPressed = io.KeyCtrl;
	guiMain->isAltPressed     = io.KeyAlt;
	guiMain->isSuperPressed   = io.KeySuper;
	
	VID_LockRenderMutex();
	
	// ******************************RENDER
	
	VID_BindImages();
	
	gCurrentFrameTime = VID_GetTickCount();
	gCurrentFrameNumber++;
	
	GUI_Render();

	// Draws only when a test has opened it. Placed after GUI_Render() so the
	// probe window is on top of the app's own views and its capture is not
	// contaminated by whatever the host happened to draw over that region.
	MT_ShaderProbeRender();

	ImGui::EndFrame();
	ImGui::Render();
	
	VID_UnlockRenderMutex();
	
	// present framebuffer
	gRenderBackend->PresentFrameBuffer(clearColor);

	// SERVICE THE SWAPCHAIN-VS-OFFSCREEN READBACK, for every backend, in ONE
	// place.
	//
	// A backend that RESOLVES (D3D11) has already serviced it inside
	// PresentFrameBuffer, where the offscreen and the back buffer hold the same
	// picture and the back buffer has not yet been discarded by Present -- and
	// the request flag is an exchange(false), so this call is a free no-op for
	// it. Every other backend renders straight to the surface, has nothing to
	// compare, and answers UNSUPPORTED here.
	//
	// Here rather than per-backend because a backend that FORGETS leaves the
	// probe in SHADER_PROBE_PENDING forever, and a test written to the
	// documented contract then hangs -- on the DEFAULT backend, which is the
	// baseline every Windows pass starts from.
	MT_ShaderProbeServiceSwapchainReadback();
	
	// TODO: LOGIC & RENDER
	countedRenderFrames += 1;
	
	//		doLogic();
	countedLogicFrames += 1;

	long t2 = SYS_GetCurrentTimeInMillis();
//	LOGD("render took %dms", t2-t1);
	
	// TODO: remove me when confirmed it is working OK
	if (SDL_TextInputActive(gMainWindow) == false)
	{
		LOGError("SDL_TextInputActive returned false");
		SDL_StartTextInput(gMainWindow);
	}
}

// TODO: VID_isChangingFullScreenState is required because SDL_filterEventCallback is called on SDL PUSH event during SDL_SetWindowFullscreen and that causes GUI_PostRenderEndFrame tasks to be run twice. This should be fixed by splitting rendering and UI async tasks loop in guiMain->PostRenderEndFrame
bool VID_isChangingFullScreenState = false;

// This is still not fixed in SDL 2.0.18.
// Workaround for: https://stackoverflow.com/questions/34967628/sdl2-window-turns-black-on-resize
// Note, this is also called during going FULL SCREEN, thus the CGuiView::PostRenderEndFrame is called twice
// SDL3: SDL_EventFilter returns bool, not int -- true keeps the event, false
// drops it. The values happen to line up (1/0), but the type does not, and a
// filter that silently returned the wrong thing would drop every event.
bool SDL_filterEventCallback(void *userdata, SDL_Event * event)
{
	if (VID_isChangingFullScreenState)
		return true;

	// SDL3 flattened window events: there is no SDL_WINDOWEVENT wrapper with a
	// sub-type in event.window.event any more -- each is its own event->type.
	if (event->type == SDL_EVENT_WINDOW_RESIZED)
	{
		// SDL_SetEventFilter docs: "the callback may run in a different thread".
		// On macOS, calling VID_Render() from a non-main thread triggers
		// ImGui::UpdatePlatformWindows() which modifies SDL/Cocoa window
		// properties, corrupting the window manager's XPC transaction state
		// and causing intermittent CFHash crashes at startup.
#if defined(MACOS)
		if (pthread_main_np() == 0)
			return 1;  // not main thread — skip render, let main loop handle it
#endif

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

#if MT_ENABLE_IMGUI_TEST_ENGINE
	bool suppressInput = false;
	if (ImGuiTestEngine *te = CImGuiTestEngine::GetEngine())
		suppressInput = ImGuiTestEngine_IsUsingSimulatedInputs(te);
#endif

	// check MouseMotion event
	int posX, posY;
	u32 button = VID_GetMousePos(&posX, &posY);
	if (posX != prevGlobalMousePosX || posY != prevGlobalMousePosY)
	{
//		LOGI("VID_ProcessEvents: DoNotTouchedMove: (%d %d) left=%d right=%d", posX, posY,
//			 button & SDL_BUTTON_MASK(SDL_BUTTON_LEFT), button & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT));

		prevGlobalMousePosX = posX;
		prevGlobalMousePosY = posY;

#if MT_ENABLE_IMGUI_TEST_ENGINE
		if (!suppressInput)
#endif
		guiMain->DoNotTouchedMove(posX, posY);

		if (button & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))
		{
#if MT_ENABLE_IMGUI_TEST_ENGINE
			if (!suppressInput)
#endif
			guiMain->DoMove(posX, posY);
		}

		if (button & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))
		{
#if MT_ENABLE_IMGUI_TEST_ENGINE
			if (!suppressInput)
#endif
			guiMain->DoRightClickMove(posX, posY);
		}
	}

	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
//		LOGI("SDL_PollEvent: event.type=%d", event.type);
//		LOGI("SDL_TextInputActive=%s", STRBOOL(SDL_TextInputActive()));
		if (event.type == SDL_EVENT_QUIT)
		{
//			LOGM("SDL_EVENT_QUIT");
			mtQuitApplication = true;
		}
		
		else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(gMainWindow))
		{
//			LOGM("SDL_EVENT_WINDOW_CLOSE_REQUESTED");
			mtQuitApplication = true;
		}

		else if (event.type == SDL_EVENT_TEXT_INPUT)
		{
			// https://wiki.libsdl.org/Tutorials-TextInput
			LOGI("VID_ProcessEvents: SDL_EVENT_TEXT_INPUT: %d %x '%s'", event.text.text[0], event.text.text[0], event.text.text);
#if MT_ENABLE_IMGUI_TEST_ENGINE
			if (!suppressInput)
#endif
			guiMain->KeyTextInput(event.text.text);
		}

		else if (event.type == SDL_EVENT_TEXT_EDITING)
		{
			LOGI("VID_ProcessEvents: SDL_EVENT_TEXT_EDITING: %d %x '%s'", event.text.text[0], event.text.text[0], event.text.text);
		}
		
		else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
		{
			LOGI("VID_ProcessEvents: %s: key=%d", (event.type == SDL_EVENT_KEY_DOWN ? "KeyDown" : "KeyUp"), event.key.key);

#if MT_ENABLE_IMGUI_TEST_ENGINE
		  if (!suppressInput)
		  {
#endif
			// BUG: DOES NOT WORK on macOS and SDL 2.0.10. Problem is that CMD KEYUP is received even though the key is still pressed
			// 		https://github.com/libsdl-org/SDL/issues/5090
			guiMain->isShiftPressed = ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
			guiMain->isLeftShiftPressed = ((SDL_GetModState() & SDL_KMOD_LSHIFT) != 0);
			guiMain->isRightShiftPressed = ((SDL_GetModState() & SDL_KMOD_RSHIFT) != 0);

			guiMain->isAltPressed = ((SDL_GetModState() & SDL_KMOD_ALT) != 0);
			guiMain->isLeftAltPressed = ((SDL_GetModState() & SDL_KMOD_LALT) != 0);
			guiMain->isRightAltPressed = ((SDL_GetModState() & SDL_KMOD_LALT) != 0);

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
//			bool isKeyDown = event.type == SDL_EVENT_KEY_DOWN;
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
			
			
			u32 keyCode = event.key.key;   // SDL3: SDL_KeyboardEvent lost .keysym; .key is the keycode, .scancode the scancode
			
			u32 keyValue = SYS_GetShiftedKey(keyCode,
											 guiMain->isShiftPressed,
											 guiMain->isAltPressed,
											 guiMain->isControlPressed,
											 guiMain->isSuperPressed);

			LOGI("keyCode=%d keyValue=%d SDL_GetModState()=%d guiMain->isControlPressed=%d %d", keyCode, keyValue, SDL_GetModState(), guiMain->isControlPressed, ((SDL_GetModState() & SDL_KMOD_GUI) != 0));
			
			if (event.type == SDL_EVENT_KEY_DOWN)
			{
				// On macOS the OS native menu bar fires a menu item's key
				// equivalent directly during the Cocoa event pump, and SDL
				// still delivers the same keystroke here. Drop the duplicate so
				// the bound action does not run twice (a toggle would net to
				// nothing). No-op on menu bars that don't auto-dispatch key
				// equivalents (ImGui / Win32).
				bool handledByNativeMenu =
					guiMain->nativeMenuBar
					&& guiMain->nativeMenuBar->WasKeyHandledByNativeMenu(
						keyCode,
						guiMain->isShiftPressed, guiMain->isAltPressed,
						guiMain->isControlPressed, guiMain->isSuperPressed);

				if (!handledByNativeMenu)
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
			}
			
			if (event.type == SDL_EVENT_KEY_UP)
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
#if MT_ENABLE_IMGUI_TEST_ENGINE
		  } // if (!suppressInput)
#endif
		}

		else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		{
			LOGI("%lu SDL_EVENT_MOUSE_BUTTON_DOWN button=%d", SYS_GetTickCount(), event.button.button);

#if MT_ENABLE_IMGUI_TEST_ENGINE
		  if (!suppressInput)
		  {
#endif
			int posX, posY;
			VID_GetMousePos(&posX, &posY);
//			LOGD("SDL_EVENT_MOUSE_BUTTON_DOWN: %d %d (%d %d)", event.motion.x, event.motion.y, posX, posY);

			if (event.button.button == SDL_BUTTON_LEFT)
			{
				guiMain->DoTap(posX, posY);
			}
			else if (event.button.button == SDL_BUTTON_RIGHT)
			{
				guiMain->DoRightClick(posX, posY);
			}
#if MT_ENABLE_IMGUI_TEST_ENGINE
		  }
#endif
		}
		else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
		{
#if MT_ENABLE_IMGUI_TEST_ENGINE
		  if (!suppressInput)
		  {
#endif
			int posX, posY;
			VID_GetMousePos(&posX, &posY);
			LOGI("SDL_EVENT_MOUSE_BUTTON_UP: %d %d (%d %d)", event.motion.x, event.motion.y, posX, posY);

			if (event.button.button == SDL_BUTTON_LEFT)
			{
				guiMain->DoFinishTap(posX, posY);
			}
			else if (event.button.button == SDL_BUTTON_RIGHT)
			{
				guiMain->DoFinishRightClick(posX, posY);
			}
#if MT_ENABLE_IMGUI_TEST_ENGINE
		  }
#endif
		}
		else if (event.type == SDL_EVENT_MOUSE_MOTION)
		{
			// Oh, why should we use clunky SDL2's events
		}
		else if (event.type == SDL_EVENT_MOUSE_WHEEL)
		{
			LOGI("SDL_EVENT_MOUSE_WHEEL");
#if MT_ENABLE_IMGUI_TEST_ENGINE
			if (!suppressInput)
#endif
			guiMain->DoScrollWheel((float)event.wheel.x, (float)event.wheel.y);
		}
		// SDL3 flattened SDL_WINDOWEVENT into one event type per sub-event, so
		// there is no single "any window event" case to catch. These are the
		// two the engine actually reacted to: a window move/resize (position
		// bookkeeping) and a display/ICC change (colour-management serial).
		else if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
		{
			VID_HandleDisplayProfileEvent(&event);
			if (!gHeadlessMode)
			{
				VID_StoreMainWindowPosition();
			}
		}
		else if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED
				 || event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
				 || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
		{
//			switch(event.type)
//			{
//				case SDL_EVENT_GAMEPAD_ADDED:
//					LOGI("SDL_EVENT_GAMEPAD_ADDED");
//					break;
//				case SDL_EVENT_GAMEPAD_REMOVED:
//					LOGI("SDL_EVENT_GAMEPAD_REMOVED");
//					break;
//				case SDL_EVENT_GAMEPAD_AXIS_MOTION:
//					LOGI("SDL_EVENT_GAMEPAD_AXIS_MOTION");
//					break;
//				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
//					LOGI("SDL_EVENT_GAMEPAD_BUTTON_DOWN");
//					break;
//				case SDL_EVENT_GAMEPAD_BUTTON_UP:
//					LOGI("SDL_EVENT_GAMEPAD_BUTTON_UP");
//					break;
//			}

			GAM_GamePadsEvent(event);
		}
		else if (event.type == SDL_EVENT_DROP_FILE)
		{
			// SDL3: the payload moved from event.drop.file (which WE freed) to
			// event.drop.data, which SDL OWNS -- freeing it is a double free.
			LOGM("SDL_EVENT_DROP_FILE: event.drop.windowId=%d file=%s", event.drop.windowID, event.drop.data);
			// SDL OWNS event.drop.data (SDL2's event.drop.file was ours to
			// SDL_free; freeing this one is a double free), and DoDropFile
			// takes a mutable char* down a chain that reaches app overrides in
			// three repos. So: hand it a copy. Casting the const away would
			// work right up until some view decides to normalise the path in
			// place, and then it would corrupt SDL's event queue.
			char *droppedPath = strdup(event.drop.data);
			guiMain->DoDropFile(event.drop.windowID, droppedPath);
			free(droppedPath);
		}
		
		// TODO: bug? for some reason these do not work on macOS. is this implemented at all?
	// TODO: copy paste old MTEngine code with proper implementation of the events queue
//	https://discourse.libsdl.org/t/events-dont-fire-sdl-multigesture-sdl-mousewheel-osx-10-14-6/27900
		else if (event.type == SDL_EVENT_FINGER_DOWN)
		{
			LOGD("SDL_EVENT_FINGER_DOWN X=%g Y=%g ID=%lld dx=%g dy=%g",
				 event.tfinger.x, event.tfinger.y,
				 event.tfinger.fingerID,
				 event.tfinger.dx, event.tfinger.dy);
		}
		else if (event.type == SDL_EVENT_FINGER_UP)
		{
			LOGD("SDL_EVENT_FINGER_UP X=%g Y=%g ID=%lld dx=%g dy=%g",
				 event.tfinger.x, event.tfinger.y,
				 event.tfinger.fingerID,
				 event.tfinger.dx, event.tfinger.dy);
		}
		else if (event.type == SDL_EVENT_FINGER_MOTION)
		{
			LOGD("SDL_EVENT_FINGER_MOTION X=%g Y=%g ID=%lld dx=%g dy=%g",
				 event.tfinger.x, event.tfinger.y,
				 event.tfinger.fingerID,
				 event.tfinger.dx, event.tfinger.dy);
		}
		// SDL3 REMOVED THE GESTURE RECOGNISER ENTIRELY -- there is no
		// SDL_MULTIGESTURE and no replacement event. This handler only logged
		// (its two lines of intent were already commented out), so the
		// degradation is explicit and total: pinch/rotate is gone until
		// somebody rebuilds it from raw SDL_EVENT_FINGER_* above, which is its
		// own piece of work with its own decision, not something to improvise
		// during a port. See the plan's "one genuine capability loss".
		//
		// SDL3 also removed SDL_SYSWMEVENT with the whole SDL_syswm.h header.
		// The Windows WM_USER handling that lived here is not lost: SDL3's
		// SDL_SetWindowsMessageHook (installed in VID_InitWindow) sees every
		// message before SDL does, which is strictly more than the SYSWM event
		// gave us. VID_WindowsMessageHook is where that logic now belongs.
		else
		{
//			LOGWarning("Unknown event.type=%d", event.type);
		}
		
		long t1 = SYS_GetTickCount();
		ImGui_ImplSDL3_ProcessEvent(&event);
		long t2 = SYS_GetTickCount();
//		LOGD("ImGui_ImplSDL3_ProcessEvent took %dms", t2-t1);
		
		long tFrame = SYS_GetTickCount();
		if (tFrame > tFrameMax)
			break;

	}
}

// See VID_Main.h. std::thread::id rather than pthread_main_np(): the render
// thread is not necessarily the process main thread on every platform, and a
// portable answer is what lets Core code ask without an #ifdef.
static std::thread::id gRenderThreadId;
static bool gRenderThreadMarked = false;

void VID_MarkRenderThread()
{
	gRenderThreadId = std::this_thread::get_id();
	gRenderThreadMarked = true;
}

bool VID_IsRenderThread()
{
	// Before the loop starts nothing IS the render thread, so the honest
	// answer is false -- an assert must not fire during init.
	return gRenderThreadMarked && (std::this_thread::get_id() == gRenderThreadId);
}

void VID_RenderLoop()
{
	LOGM("VID_RenderLoop");
	VID_MarkRenderThread();
	mtEventLoopRunning = true;
	
	SDL_SetEventFilter(SDL_filterEventCallback, NULL);
	
	previousFrameStep = SDL_GetPerformanceCounter();
		
	while (mtQuitApplication == false)
	{
		VID_ProcessEvents();

		// Dock-Quit (macOS) posts SDL_EVENT_QUIT mid-iteration while AppKit is already
		// tearing down CVDisplayLink / NSOpenGLContext. Bail out before touching
		// GL again — otherwise we hit glTexSubImage2D crashes or hang in
		// Cocoa_GL_SwapWindow's cond wait.
		if (mtQuitApplication)
			break;

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
			SYS_Sleep(1);
		}
	}

	mtEventLoopRunning = false;
}

u32 VID_GetMousePos(int *posX, int *posY)
{
	// SDL3 moved mouse coordinates to float (they always were, on high-DPI and
	// with sub-pixel trackpads; SDL2 just truncated them for us).
	float mouseX = 0.0f, mouseY = 0.0f;
	u32 button = SDL_GetGlobalMouseState(&mouseX, &mouseY);
	*posX = (int)mouseX;
	*posY = (int)mouseY;
	
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

// ---------------------------------------------------------------------------
// Which backends exist HERE
// ---------------------------------------------------------------------------
//
// One list, asked by everything: the factory, the CLI whitelist, the config
// validator, the setter, the Settings pane and both app menus. Before S-6
// there was only a boolean -- VID_IsRenderBackendSwitchable() -- and every
// caller hardcoded WHICH backends that implied. That is exactly how c64d's
// menu came to offer "Metal" on Windows: the guard was true, and the two menu
// items were written out by hand under it.
//
// The names are the SELECTION names (lowercase), not the running backend's
// own capitalised `name`. See VID_Main.h.

// A COPY OF A BACKEND NAME, and it is a fix for a real use-after-free rather
// than defensive habit.
//
// CConfigStorageHjson::GetString hands back `static_cast<const char *>(hValue)`
// -- a pointer into the std::string inside the Hjson value the config map owns.
// CConfigStorageHjson::SetString then does `hjsonRoot[name] = value`, which
// REPLACES that value: the old ValueImpl drops to refcount zero, its string is
// freed, and every pointer previously returned from a Get dangles.
//
// Harmless while callers computed a bool up front and never looked again. It
// stopped being harmless the moment the render-backend pickers began iterating
// a list and calling VID_SetPreferredRenderBackend from INSIDE the loop: click
// OpenGL while "metal" is persisted and the next iteration strcmp's freed heap.
// No test could ever have seen it -- headless runs wipe the settings directory,
// so the getter returns the static default literal and never a heap pointer.
//
// thread_local because the buffer is shared; every caller is on the main thread
// today and a per-thread copy costs nothing to keep that from mattering later.
// Selection names are a closed set of at most eight characters.
static const char *VID_CopyBackendName(const char *name)
{
	static thread_local char sBuf[32];
	if (name == NULL)
		return "opengl";
	snprintf(sBuf, sizeof(sBuf), "%s", name);
	return sBuf;
}

bool VID_IsRenderBackendAvailable(const char *name)
{
	if (name == NULL)
		return false;
	if (strcmp(name, "opengl") == 0)
		return true;   // every platform, always: the default and the fallback
#if defined(MACOS)
	if (strcmp(name, "metal") == 0)
	{
		// PROBED, not assumed -- since Metal became the macOS DEFAULT.
		//
		// This used to `return true` unconditionally, and the comment in
		// VID_Init said so explicitly ("acceptable on a Mac, where Metal always
		// exists"). That was fair while Metal was opt-in: a user who asked for
		// it and hit CreateSDLWindow's SYS_FatalExit had at least asked. As the
		// default it is not, because a Mac that cannot make a device would fail
		// to start the app at all and the only way out would be hand-editing
		// settings.hjson.
		//
		// Probed ONCE per process, for the same reason as the D3D11 arm below:
		// this is called from per-frame UI.
		static const bool sMetalProbed = CRenderBackendMetal::IsAvailable();
		return sMetalProbed;
	}
#elif defined(WIN32) && defined(MT_RENDER_BACKEND_D3D11)
	if (strcmp(name, "d3d11") == 0)
	{
		// THE RUNTIME PROBE LIVES HERE, not only in the factory.
		//
		// "Compiled in" and "this machine can actually do it" are two different
		// questions, and the first draft answered only the first -- so on a
		// Windows box that cannot create a D3D11 device (a bare VM, a remoted
		// session, a driver in a bad state) the Settings pane, both app menus
		// and CTestRenderBackend would all have insisted Direct3D 11 was
		// selected while OpenGL quietly ran, with one log line as the only
		// evidence. A capability query that disagrees with what the factory
		// does is worse than no query.
		//
		// Probed ONCE per process: this is called from per-frame UI, and
		// creating a D3D11 device to answer a menu is not acceptable at that
		// rate. Once is right anyway -- a device that cannot be created at
		// startup will not appear later, and a backend switch needs a restart.
		static const bool sD3D11Probed = CRenderBackendD3D11::IsAvailable();
		return sD3D11Probed;
	}
#endif
	return false;
}

// The SELECTION name that will actually be used at the next launch: the
// persisted choice if this platform can run it, else "opengl".
//
// Exists because THREE user interfaces need exactly this and each wrote its own
// version of it -- the photo app's Settings radios, c64d's menu and the game app's
// menu. Two of the three then got it wrong in the same way: they ticked the
// item whose name equalled the persisted string and had no answer at all when
// the persisted string named a backend this build does not have, so a
// settings.hjson carried from another machine (or hand-edited to "OpenGL4",
// which the menu TITLE shows) left every item unticked while the app plainly
// ran one of them.
const char *VID_GetEffectiveRenderBackendSelection()
{
	// VID_GetPersistedRenderBackend() already returns a COPY, which is what
	// makes this safe to hold across a loop that calls
	// VID_SetPreferredRenderBackend -- and every one of its three callers does
	// exactly that.
	const char *persisted = VID_GetPersistedRenderBackend();
	// Same reasoning as the fallback in VID_GetPreferredRenderBackend(): the
	// recovery from an unrunnable persisted name is the platform default, which
	// is itself "opengl" wherever the preferred backend does not probe.
	return VID_IsRenderBackendAvailable(persisted) ? persisted : VID_GetDefaultRenderBackend();
}

// The SELECTION name of the backend that is RUNNING right now, mapped back from
// its capitalised `name`. The inverse of the vocabulary split described in
// VID_Main.h, and it exists so a menu can title itself in the SAME vocabulary
// as the items underneath it: "Render Backend: OpenGL" over an "OpenGL" item,
// not "Render Backend: OpenGL4" over one.
const char *VID_GetCurrentRenderBackendSelection()
{
	const char *running = VID_GetCurrentRenderBackendName();
	if (strcmp(running, "Metal") == 0)   return "metal";
	if (strcmp(running, "D3D11") == 0)   return "d3d11";
	if (strcmp(running, "OpenGL4") == 0) return "opengl";
	// A name this function does not know -- "none" before VID_Init, or a
	// backend added without updating this map. Falling through to "opengl"
	// SILENTLY would put a menu title of "OpenGL" over a ticked "Direct3D 12",
	// which is the exact two-vocabularies symptom this function exists to
	// remove, reintroduced without a word.
	LOGError("VID_GetCurrentRenderBackendSelection: running backend '%s' has no selection name; "
			 "reporting 'opengl'. Add it here when a backend is added.", running);
	return "opengl";
}

int VID_GetAvailableRenderBackends(const char **outNames, int maxNames)
{
	// OpenGL first: it is the default and the fallback on every platform, and
	// a picker that puts the default first reads the same everywhere.
	static const char *kAll[] = { "opengl", "metal", "d3d11" };
	int count = 0;
	for (int i = 0; i < (int)(sizeof(kAll) / sizeof(kAll[0])); i++)
	{
		if (count >= maxNames)
			break;
		if (VID_IsRenderBackendAvailable(kAll[i]))
			outNames[count++] = kAll[i];
	}
	return count;
}

const char *VID_GetRenderBackendDisplayName(const char *name)
{
	if (name == NULL)
		return "?";
	if (strcmp(name, "metal") == 0)  return "Metal";
	if (strcmp(name, "d3d11") == 0)  return "Direct3D 11";
	if (strcmp(name, "opengl") == 0) return "OpenGL";
	return name;
}

bool VID_IsRenderBackendHdrCapable(const char *name)
{
	if (name == NULL)
		return false;
#if defined(MACOS)
	return strcmp(name, "metal") == 0;
#elif defined(WIN32) && defined(MT_RENDER_BACKEND_D3D11)
	return strcmp(name, "d3d11") == 0;
#else
	(void)name;
	return false;
#endif
}

// The backend a fresh install gets: the best one this platform HAS, not the
// lowest common denominator.
//
// OpenGL was the default everywhere until now, which meant a new user on a Mac
// or a modern Windows box silently ran the slowest path and could not have HDR
// at all -- an extended-range surface needs Metal or D3D11, so the default
// decided a feature the user never knew they had.
//
// FALLBACK IS REAL, NOT NOMINAL. Both preferred names go through
// VID_IsRenderBackendAvailable(), which probes an actual device on both
// platforms, so a machine that cannot run the preferred backend gets "opengl"
// from this function rather than a promise that fails at window creation.
//
// This is a DEFAULT, not a migration: it applies only where the config has no
// `renderBackend` key. An existing install that already persisted "opengl" --
// including one that persisted it merely by visiting the menu -- keeps OpenGL
// until someone changes it. That is the correct behaviour for a default, and it
// is also why "why am I still on OpenGL after the update" has an answer.
const char *VID_GetDefaultRenderBackend()
{
#if defined(MACOS)
	if (VID_IsRenderBackendAvailable("metal"))
		return "metal";
#elif defined(WIN32) && defined(MT_RENDER_BACKEND_D3D11)
	if (VID_IsRenderBackendAvailable("d3d11"))
		return "d3d11";
#endif
	return "opengl";
}

const char *VID_GetPreferredRenderBackend()
{
	// The command line is checked BEFORE the config, for two reasons. One:
	// the photo app's headless runs deliberately wipe the settings directory to
	// start from factory defaults, so a config-only setting can never be seen by
	// an automated test -- which left the Metal backend with no test coverage at
	// all. Two: an operator debugging a rendering problem should be able to
	// switch backends without editing a file the app rewrites underneath them.
	for (int i = 0; i < (int)sysCommandLineArguments.size(); i++)
	{
		const char *arg = sysCommandLineArguments[i];
		if (strncmp(arg, "--render-backend=", 17) != 0)
			continue;
		const char *name = arg + 17;
		// ASK THE AVAILABILITY QUERY, never a hardcoded pair. This line read
		// `strcmp(name,"metal")==0 || strcmp(name,"opengl")==0` until S-6, and
		// that made the entire D3D11 stage unreachable by its own documented
		// switch: --render-backend=d3d11 was rejected HERE, logged as
		// "unknown", and the factory -- which reads this same function -- never
		// saw it. Three review rounds read this function and none noticed.
		// A whitelist that has to be kept in step with a capability list is a
		// whitelist that will drift; there is now one list.
		if (VID_IsRenderBackendAvailable(name))
			return name;   // sysCommandLineArguments entries outlive this call
		LOGError("VID_GetPreferredRenderBackend: backend '%s' is not available on this platform, ignoring --render-backend", name);
	}

	const char *value = nullptr;
	gApplicationDefaultConfig->GetString("renderBackend", &value, VID_GetDefaultRenderBackend());
	if (value == NULL)
		return VID_GetDefaultRenderBackend();
	// VALIDATE THE PERSISTED VALUE TOO, and say so when it loses. This path
	// used to return the config string unchecked, so a settings file naming a
	// backend this build does not have -- the user moved the file between
	// machines, or downgraded from a build with MT_RENDER_BACKEND_D3D11 -- fell
	// through to OpenGL in the factory with no message at all, which is
	// indistinguishable from a typo being ignored.
	if (!VID_IsRenderBackendAvailable(value))
	{
		// Falls back to the platform DEFAULT, not to a hardcoded "opengl": on a
		// Mac the sensible recovery from an unrunnable persisted name is Metal,
		// and VID_GetDefaultRenderBackend() itself degrades to OpenGL when the
		// preferred backend does not probe.
		const char *fallback = VID_GetDefaultRenderBackend();
		LOGError("VID_GetPreferredRenderBackend: persisted backend '%s' is not available on this platform, using %s", value, fallback);
		return fallback;
	}
	// A COPY, for the same reason VID_GetPersistedRenderBackend() returns one:
	// this pointer is into the config's own string and the next SetString on
	// this key frees it.
	return VID_CopyBackendName(value);
}

// Whether HDR was ASKED for. Distinct from whether it was GRANTED, which is
// VID_GetDisplayHdrHeadroom() > 1.0 and is not knowable at init time.
//
// Same command-line-first rule as the backend override, and for the same
// reason: the settings-directory wipe would otherwise make this unreachable
// from any automated test. Accepts on|off|auto; `auto` behaves as `off` for now
// -- the real heuristic is S-5's problem, and the tracker is explicit that
// "HDR display present -> always float" is the WRONG rule because it would
// halve the image cache for a RAW-heavy library.
float VID_GetMaxPotentialHdrHeadroom()
{
#if defined(MACOS)
	return MACOS_GetMaxPotentialHdrHeadroom();
#elif defined(WIN32)
	// S-6 Task A2 wires the seam and Task B4 fills it in. Until then this
	// returns 1.0, which keeps the `auto` gate closed and every current
	// behaviour byte-identical -- the stub exists so the CALL SITE is written
	// and reviewed on the machine that can read it, rather than being invented
	// on the VM under time pressure.
	return WINDOWS_GetMaxPotentialHdrHeadroom();
#else
	// Linux has no probe, so `auto` resolves to off there rather than guessing.
	return 1.0f;
#endif
}

bool VID_IsAnyDisplayHdrCapable()
{
#if defined(WIN32)
	return WINDOWS_IsAnyDisplayHdrCapable();
#elif defined(MACOS)
	// Reuses the existing MACOS_GetMaxPotentialHdrHeadroom() rather than
	// adding a new, untested NSScreen walk from a machine that cannot build
	// or test macOS code. Corrected after review (this comment originally,
	// wrongly, called this path NOT live): MACOS_GetMaxPotentialHdrHeadroom()
	// itself has no internal cache -- it recomputes from [NSScreen screens]
	// on every call (SYS_MacOSWrapper.mm), which Apple documents as updating
	// live on hotplug. The "once-per-session" LATCH this codebase's comments
	// refer to elsewhere lives one level up, in VID_IsHdrRequested()'s own
	// static latch below -- a different call site from this one. So this
	// path is, in fact, already live/hotplug-aware on macOS -- verify that
	// claim on real hardware before relying on it further, since it was
	// derived by reading, not by testing.
	return MACOS_GetMaxPotentialHdrHeadroom() > 1.0f;
#else
	return false;
#endif
}

bool VID_IsHdrRequested()
{
	const char *value = nullptr;
	for (int i = 0; i < (int)sysCommandLineArguments.size(); i++)
	{
		const char *arg = sysCommandLineArguments[i];
		if (strncmp(arg, "--hdr=", 6) != 0)
			continue;
		value = arg + 6;
		if (strcmp(value, "on") == 0 || strcmp(value, "off") == 0 || strcmp(value, "auto") == 0)
			break;
		LOGError("VID_IsHdrRequested: unknown --hdr value '%s' (want on|off|auto), ignoring", value);
		value = nullptr;
	}

	if (value == nullptr)
		gApplicationDefaultConfig->GetString("hdrMode", &value, "auto");

	if (value == nullptr)
		return false;
	if (strcmp(value, "on") == 0)
		return true;
	if (strcmp(value, "off") == 0)
		return false;

	// `auto` (the shipped default): on iff some attached display could
	// actually show the extra range.
	//
	// CACHED after the first call, and that is not an optimisation. This is
	// called at least twice during init -- once for the surface's pixel format
	// and once for its colourspace -- so an unlatched probe would let a display
	// hotplug between the two calls leave the surface FORMAT and the surface
	// COLOURSPACE disagreeing: a half-configured HDR surface, and about the
	// hardest thing there is to diagnose from a bug report.
	static bool sAutoResolved = false;
	static bool sAutoAnswer = false;
	if (!sAutoResolved)
	{
		const float potential = VID_GetMaxPotentialHdrHeadroom();
		sAutoAnswer = (potential > 1.0f);
		sAutoResolved = true;
		LOGM("VID_IsHdrRequested: hdrMode=auto resolved to %s (max potential headroom %.3f)",
			 sAutoAnswer ? "ON" : "off", potential);
	}
	return sAutoAnswer;
}

float VID_GetDisplayHdrHeadroom()
{
	// A POLL, never a cached value: macOS grants headroom lazily (1.0 for the
	// first seconds after the request, then ramping) and keeps moving it with
	// display brightness and ambient light.
	return gRenderBackend ? gRenderBackend->GetDisplayHdrHeadroom() : 1.0f;
}

const char *VID_GetHdrMode()
{
	const char *value = nullptr;
	gApplicationDefaultConfig->GetString("hdrMode", &value, "auto");
	return value ? value : "auto";
}

// RETURNS A COPY -- see VID_CopyBackendName() for the use-after-free this
// avoids. Callers may hold the result across a VID_SetPreferredRenderBackend().
const char *VID_GetPersistedRenderBackend()
{
	// The DEFAULT is the platform's preferred backend, so a settings UI on a
	// fresh install ticks what will actually run rather than ticking OpenGL
	// while Metal starts.
	const char *fallback = VID_GetDefaultRenderBackend();
	const char *value = nullptr;
	gApplicationDefaultConfig->GetString("renderBackend", &value, fallback);
	return VID_CopyBackendName(value ? value : fallback);
}

const char *VID_GetPersistedHdrMode()
{
	return VID_GetHdrMode();
}

static bool VID_HasCommandLineArgPrefix(const char *prefix)
{
	const size_t len = strlen(prefix);
	for (int i = 0; i < (int)sysCommandLineArguments.size(); i++)
	{
		if (strncmp(sysCommandLineArguments[i], prefix, len) == 0)
			return true;
	}
	return false;
}

bool VID_IsRenderBackendOverriddenByCommandLine()
{
	return VID_HasCommandLineArgPrefix("--render-backend=");
}

bool VID_IsHdrOverriddenByCommandLine()
{
	return VID_HasCommandLineArgPrefix("--hdr=");
}

void VID_SetHdrMode(const char *mode)
{
	if (!mode)
		return;
	// Reject anything else rather than persisting it. VID_IsHdrRequested()
	// treats an unknown value as "not on", so a typo would silently disable HDR
	// with no way for the user to see why.
	if (strcmp(mode, "auto") != 0 && strcmp(mode, "on") != 0 && strcmp(mode, "off") != 0)
	{
		LOGError("VID_SetHdrMode: unknown mode '%s' (want auto|on|off), ignoring", mode);
		return;
	}
	gApplicationDefaultConfig->SetString("hdrMode", mode);
}

void VID_SetPreferredRenderBackend(const char *name)
{
	if (!name)
		return;
	// Reject anything this platform cannot run, and SAY SO. The old form
	// accepted "metal" everywhere and dropped unknown names without a word --
	// so c64d on Windows could persist a backend that does not exist there, and
	// a typo was indistinguishable from a setting that had been ignored.
	if (!VID_IsRenderBackendAvailable(name))
	{
		LOGError("VID_SetPreferredRenderBackend: backend '%s' is not available on this platform, not persisting it", name);
		return;
	}
	gApplicationDefaultConfig->SetString("renderBackend", name);
}

const char *VID_GetCurrentRenderBackendName()
{
	if (gRenderBackend)
		return gRenderBackend->name;
	return "none";
}

bool VID_IsRenderBackendSwitchable()
{
	// "More than one backend is available here" -- DERIVED, not a per-platform
	// constant. The old `#if defined(MACOS) return true` was correct only for
	// as long as macOS was the only two-backend platform, and it told callers
	// nothing about WHICH two, which is what they then hardcoded.
	const char *names[8];
	return VID_GetAvailableRenderBackends(names, 8) > 1;
}

void VID_Shutdown()
{
	LOGM("VID_Shutdown");
	guiMain->LockMutex();

	gRenderPipelineInitialized = false;
	gRenderBackend->Shutdown();
	
	ImGui_ImplSDL3_Shutdown();
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
	// QueryPerformanceCounter gives sub-microsecond precision vs GetTickCount's ~15.6ms
	static INT64 qpcFreq = 0;
	if (qpcFreq == 0)
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		qpcFreq = freq.QuadPart;
	}
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (unsigned long)(counter.QuadPart * 1000 / qpcFreq);
#endif
}

SDL_Window *ImGui_ImplSDL3_ImGuiViewportToSDLWindow(ImGuiViewport *imGuiViewport);
// this needs to be moved to imgui_impl_sdl.cpp
//SDL_Window *ImGui_ImplSDL3_ImGuiViewportToSDLWindow(ImGuiViewport *imGuiViewport)
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

	return ImGui_ImplSDL3_ImGuiViewportToSDLWindow(imGuiViewport);
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
	
	SDL_SetWindowAlwaysOnTop(window, isOnTop ? true:false);
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
	SDL_SetWindowAlwaysOnTop(window, isOnTop ? true:false);
}

void VID_SetMainWindowAlwaysOnTopTemporary(bool isOnTop)
{
	SDL_Window *window = VID_GetMainSDLWindow();
	SDL_SetWindowAlwaysOnTop(window, isOnTop ? true:false);
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
	// SDL3 folded SDL_WINDOW_FULLSCREEN_DESKTOP into SDL_WINDOW_FULLSCREEN --
	// "fullscreen" now means borderless-desktop by default and an exclusive
	// video mode is requested separately via SDL_SetWindowFullscreenMode. So
	// the second test is not merely renamed, it is the SAME test, and keeping
	// both would be checking one flag twice.
	if (SDL_GetWindowFlags(gMainWindow) & SDL_WINDOW_FULLSCREEN)
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
	
	// SDL3: SDL_SetWindowFullscreen takes a bool. Passing
	// SDL_WINDOW_FULLSCREEN_DESKTOP used to mean "borderless fullscreen", which
	// is now simply what `true` does, since the window has no fullscreen VIDEO
	// MODE set (SDL_SetWindowFullscreenMode). Same behaviour, plainer call.
	SDL_SetWindowFullscreen(gMainWindow, isFullScreen);
	
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

// Which display "the screen" means: the one our window is on, falling back to
// the primary one before a window exists.
//
// SDL3 DISPLAY IDS ARE OPAQUE HANDLES, NOT INDICES. Passing a literal 0 -- as
// this code did, because in SDL2 zero meant "the first display" -- is not
// "display zero", it is an INVALID ID. SDL3 reserves 0 as the "no display"
// value, so the call fails, the SDL_Rect is left untouched, and the caller gets
// whatever garbage was on the stack as a screen size. Silently, because the
// bool return was not checked either.
static bool VID_GetUsableScreenBounds(SDL_Rect *outRect)
{
	SDL_DisplayID displayId = 0;

	if (gMainWindow != NULL)
		displayId = SDL_GetDisplayForWindow(gMainWindow);

	if (displayId == 0)
		displayId = SDL_GetPrimaryDisplay();

	if (displayId == 0)
		return false;

	return SDL_GetDisplayUsableBounds(displayId, outRect);
}

float VID_GetScreenWidth()
{
	SDL_Rect r = { 0, 0, 0, 0 };
	if (!VID_GetUsableScreenBounds(&r))
	{
		LOGError("VID_GetScreenWidth: no usable display bounds: %s", SDL_GetError());
		return 0.0f;
	}
	return r.w;
}

float VID_GetScreenHeight()
{
	SDL_Rect r = { 0, 0, 0, 0 };
	if (!VID_GetUsableScreenBounds(&r))
	{
		LOGError("VID_GetScreenHeight: no usable display bounds: %s", SDL_GetError());
		return 0.0f;
	}
	return r.h;
}

void VID_SetMainWindowTitle(const char *title)
{
	SDL_SetWindowTitle(gMainWindow, title);
}

ImGuiStyleType VID_GetDefaultImGuiStyle()
{
	int style = (int)gAppDefaultImGuiStyle;
	gApplicationDefaultConfig->GetInt("uiImGuiStyle", &style, style);
	if (style < IMGUI_STYLE_DARK_ALTERNATIVE || style > IMGUI_STYLE_CUSTOM)
	{
		style = (int)gAppDefaultImGuiStyle;
	}
	return (ImGuiStyleType)style;
}
void VID_SetAppDefaultImGuiStyle(ImGuiStyleType imGuiStyleType)
{
	if (imGuiStyleType < IMGUI_STYLE_DARK_ALTERNATIVE || imGuiStyleType > IMGUI_STYLE_SYSTEM)
	{
		return;
	}
	gAppDefaultImGuiStyle = imGuiStyleType;
}

void VID_SetDefaultImGuiStyle(ImGuiStyleType imGuiStyleType)
{
	if (imGuiStyleType < IMGUI_STYLE_DARK_ALTERNATIVE || imGuiStyleType > IMGUI_STYLE_CUSTOM)
	{
		return;
	}
	int style = imGuiStyleType;
	gApplicationDefaultConfig->SetInt("uiImGuiStyle", &style);
	VID_SetImGuiStyle(imGuiStyleType);
}

VID_SystemAppearance VID_GetSystemAppearance()
{
#if defined(MACOS)
	return MACOS_GetSystemAppearance();
#else
	return VID_SYSTEM_APPEARANCE_UNKNOWN;
#endif
}

const char *VID_GetSystemAppearanceName(VID_SystemAppearance appearance)
{
	switch (appearance)
	{
		case VID_SYSTEM_APPEARANCE_LIGHT:
			return "light";
		case VID_SYSTEM_APPEARANCE_DARK:
			return "dark";
		case VID_SYSTEM_APPEARANCE_UNKNOWN:
		default:
			return "unknown";
	}
}

ImGuiStyleType VID_ResolveImGuiStyle(ImGuiStyleType imGuiStyleType)
{
	if (imGuiStyleType < IMGUI_STYLE_DARK_ALTERNATIVE || imGuiStyleType > IMGUI_STYLE_CUSTOM)
	{
		return IMGUI_STYLE_DARK;
	}

	if (imGuiStyleType == IMGUI_STYLE_CUSTOM)
	{
		return IMGUI_STYLE_CUSTOM;
	}

	if (imGuiStyleType != IMGUI_STYLE_SYSTEM)
	{
		return imGuiStyleType;
	}

	VID_SystemAppearance appearance = VID_GetSystemAppearance();
	if (appearance == VID_SYSTEM_APPEARANCE_LIGHT)
	{
		return IMGUI_STYLE_LIGHT;
	}
	return IMGUI_STYLE_DARK;
}

VID_DisplayColorGamut VID_GetMainDisplayColorGamut()
{
#if defined(MACOS)
	return MACOS_GetMainDisplayColorGamut(gMainWindow);
#else
	return VID_DISPLAY_COLOR_GAMUT_SRGB;
#endif
}

VID_DisplayColorGamut VID_GetMainWindowRenderColorGamut()
{
	return gMainWindowRenderColorGamut;
}

const char *VID_GetDisplayColorGamutName(VID_DisplayColorGamut gamut)
{
	switch (gamut)
	{
		case VID_DISPLAY_COLOR_GAMUT_SRGB:
			return "sRGB";
		case VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3:
			return "Display P3";
		case VID_DISPLAY_COLOR_GAMUT_UNKNOWN:
		default:
			return "unknown";
	}
}

bool VID_IsMainDisplayWideGamut()
{
	return VID_GetMainDisplayColorGamut() == VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3;
}

static void VID_ApplyMainWindowAppearance(ImGuiStyleType resolvedStyle)
{
#if defined(MACOS)
	if (gMainWindow == NULL)
	{
		return;
	}

	if (gRequestedImGuiStyle == IMGUI_STYLE_SYSTEM)
	{
		MACOS_ApplyWindowAppearance(gMainWindow, VID_SYSTEM_APPEARANCE_UNKNOWN);
		return;
	}

	VID_SystemAppearance appearance = resolvedStyle == IMGUI_STYLE_LIGHT ?
		VID_SYSTEM_APPEARANCE_LIGHT : VID_SYSTEM_APPEARANCE_DARK;
	MACOS_ApplyWindowAppearance(gMainWindow, appearance);
#else
	(void)resolvedStyle;
#endif
}

// Bumped by OS-driven events only -- never by polling the profile itself.
static std::atomic<u64> gDisplayProfileSerial(1);

#if defined(WIN32)
// Windows needs a supplement to SDL's own change detection, and the gap is
// precise. SDL_GetWindowICCProfile re-reads the profile FILE on every call, but
// the filename it uses is cached in SDL_WindowData::ICMFileName and refreshed
// only by WIN_UpdateWindowICCProfile -- on window creation, on WM_ACTIVATE, and
// when the window changes display. So a recalibration that associates a
// DIFFERENT profile file while our window keeps focus is invisible to SDL until
// the user next activates the window.
//
// Deliberately NOT re-implementing the fetch here with MonitorFromWindow /
// GetICMProfileW: that would duplicate SDL for a defect SDL does not have, and
// create a second, independently-drifting notion of which monitor we are on.
static std::atomic<bool> gDisplayProfileRecheckPending(false);
static u8   gLastDisplayProfileDigest[16];
static bool gLastDisplayProfileDigestValid = false;

// SDL3 changed this callback's shape: it now takes the whole MSG and returns
// bool ("true to let the event continue on, false to drop it"), where SDL2
// passed hWnd/message/wParam/lParam separately and returned void. ALWAYS
// return true here -- this hook observes, it does not filter, and returning
// false would silently swallow Windows messages from the whole app.
//
// It also absorbed the work that used to arrive as SDL_SYSWMEVENT, which SDL3
// removed along with SDL_syswm.h. That is not a loss: the hook sees every
// message BEFORE SDL processes it, which is strictly more than the event gave
// us, and it removes the need to have SDL_SYSWMEVENT enabled at all.
static bool SDLCALL VID_WindowsMessageHook(void *userdata, MSG *msg)
{
	(void)userdata;
	if (msg == NULL)
		return true;

	// Only raise a flag. WM_SETTINGCHANGE is broadcast for a great many
	// unrelated settings, so bumping the serial here would invalidate the whole
	// colour cache and re-decode every image for a mouse-speed change. The
	// actual comparison happens once per frame, off this callback.
	if (msg->message == WM_DISPLAYCHANGE || msg->message == WM_SETTINGCHANGE)
		gDisplayProfileRecheckPending.store(true);

	// Previously the SDL_SYSWMEVENT branch of VID_ProcessEvents.
	if (msg->message == WM_USER
		&& msg->wParam == WM_USER_TRIGGER_WPARAM
		&& msg->lParam == WM_USER_TRIGGER_LPARAM)
	{
		mtEngineHandleWM_USER();
	}
	if (guiMain && guiMain->nativeMenuBar)
		guiMain->nativeMenuBar->HandlePlatformEvent(msg);

	return true;
}

// Records the current profile's digest without bumping. Seeded at init so the
// first WM_SETTINGCHANGE after startup compares against something real rather
// than reporting a spurious change.
static void VID_SeedDisplayProfileDigest()
{
	u8 *bytes = NULL; u32 size = 0;
	if (!VID_GetMainDisplayICCProfile(&bytes, &size) || bytes == NULL)
		return;
	CIccProfileCodec::GetContentDigest(bytes, size, gLastDisplayProfileDigest);
	gLastDisplayProfileDigestValid = true;
	delete [] bytes;
}
#endif

// Per-frame, and cheap: one atomic read unless Windows actually flagged
// something. Bumps the serial only when the profile's bytes really changed --
// SDL's own ICCPROF_CHANGED may already have covered the same event, and a
// double bump would cost a redundant re-decode of everything.
void VID_PollDisplayProfileChange()
{
#if defined(WIN32)
	if (!gDisplayProfileRecheckPending.exchange(false))
		return;

	u8 *bytes = NULL; u32 size = 0;
	if (!VID_GetMainDisplayICCProfile(&bytes, &size) || bytes == NULL)
		return;

	u8 digest[16];
	CIccProfileCodec::GetContentDigest(bytes, size, digest);
	delete [] bytes;

	if (!gLastDisplayProfileDigestValid)
	{
		memcpy(gLastDisplayProfileDigest, digest, 16);
		gLastDisplayProfileDigestValid = true;
		return;
	}
	if (memcmp(digest, gLastDisplayProfileDigest, 16) != 0)
	{
		memcpy(gLastDisplayProfileDigest, digest, 16);
		gDisplayProfileSerial++;
		LOGD("VID: display profile changed (Windows settings broadcast); serial now %llu",
		     (unsigned long long)gDisplayProfileSerial.load());
	}
#endif
}

bool VID_GetDisplayICCProfileForWindow(SDL_Window *window, u8 **outBytes, u32 *outSize)
{
	if (outBytes == NULL || outSize == NULL)
		return false;
	*outBytes = NULL;
	*outSize = 0;

#if SDL_VERSION_ATLEAST(2, 0, 18)
	if (window != NULL)
	{
		size_t sz = 0;
		void *icc = SDL_GetWindowICCProfile(window, &sz);
		if (icc != NULL)
		{
			if (sz > 0 && CIccProfileCodec::ValidateHeader((const u8 *)icc, (u32)sz))
			{
				*outBytes = new u8[sz];
				memcpy(*outBytes, icc, sz);
				*outSize = (u32)sz;
				SDL_free(icc);
				return true;
			}
			// A profile the OS handed us that we cannot parse is not usable as
			// a conversion target; fall through to sRGB rather than passing it
			// to a CMM.
			LOGD("VID_GetDisplayICCProfileForWindow: display profile failed validation (%d bytes)", (int)sz);
			SDL_free(icc);
		}
	}
#else
	(void)window;
	// SDL_GetWindowICCProfile arrived in SDL 2.0.18. macOS and Windows use the
	// vendored SDL, but Linux builds against the distro's, so this can be an
	// older one -- degrade to the sRGB assumption rather than failing to build.
#endif

	// Fallback: the built-in sRGB profile. Deliberately not "no profile" --
	// see the header.
	const std::vector<uint8_t> srgb = ICC_BuildSRGBProfileV2();
	if (srgb.empty())
		return false;
	*outBytes = new u8[srgb.size()];
	memcpy(*outBytes, &srgb[0], srgb.size());
	*outSize = (u32)srgb.size();
	return true;
}

bool VID_GetMainDisplayICCProfile(u8 **outBytes, u32 *outSize)
{
	return VID_GetDisplayICCProfileForWindow(VID_GetMainSDLWindow(), outBytes, outSize);
}

u64 VID_GetMainDisplayProfileSerial()
{
	return gDisplayProfileSerial.load();
}

void VID_HandleDisplayProfileEvent(const SDL_Event *event)
{
	if (event == NULL || event->type < SDL_EVENT_WINDOW_FIRST || event->type > SDL_EVENT_WINDOW_LAST)
		return;
	// Main window only: another window moving between displays says nothing
	// about the profile our pixels are destined for.
	SDL_Window *mainWindow = VID_GetMainSDLWindow();
	if (mainWindow == NULL || event->window.windowID != SDL_GetWindowID(mainWindow))
		return;

	// SDL3: the sub-type IS the event type. The SDL_VERSION_ATLEAST(2,0,18)
	// guard around ICCPROF_CHANGED is gone with it -- SDL3 has always had it.
	if (event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED
		|| event->type == SDL_EVENT_WINDOW_ICCPROF_CHANGED)
	{
		gDisplayProfileSerial++;
	}
}

void VID_ApplyMainWindowColorGamut()
{
	if (gMainWindow == NULL || gRenderBackend == NULL || !gRenderPipelineInitialized)
	{
		return;
	}

	VID_DisplayColorGamut displayGamut = VID_GetMainDisplayColorGamut();
	VID_DisplayColorGamut renderGamut = VID_GetMainWindowRenderColorGamut();
	if (displayGamut == gAppliedDisplayColorGamut &&
		renderGamut == gAppliedRenderColorGamut)
	{
		return;
	}

#if defined(MACOS)
	MACOS_ApplyWindowColorGamut(gMainWindow, renderGamut);
#endif
	if (gRenderBackend != NULL)
	{
		gRenderBackend->ApplyDisplayColorGamut(renderGamut);
	}
	gAppliedDisplayColorGamut = displayGamut;
	gAppliedRenderColorGamut = renderGamut;
}

static void VID_RefreshSystemVisualState()
{
	// The OS-appearance poll used to be gated on gRequestedImGuiStyle ==
	// IMGUI_STYLE_SYSTEM. That gate breaks the moment a host stops writing
	// uiImGuiStyle (which the photo app does in TS-2): gRequestedImGuiStyle then
	// holds whatever stale value VID_Init read, the poll never fires, and
	// "Follow System" silently stops following. Poll whenever the style is
	// SYSTEM **or** a theme is active -- the second condition is new and is
	// what makes design #6.4 actually work.
	CMTThemeRegistry *reg = CMTThemeRegistry::Instance();
	const bool watchAppearance =
		(gRequestedImGuiStyle == IMGUI_STYLE_SYSTEM) || reg->HasActiveTheme();

	if (watchAppearance)
	{
		VID_SystemAppearance appearance = VID_GetSystemAppearance();
		// First watched poll: latch without notifying. gLastSystemAppearance
		// starts UNKNOWN and, before this change, was only ever latched inside
		// VID_SetImGuiStyle's SYSTEM branch -- so a host with a theme but a
		// non-SYSTEM style would otherwise fire a spurious "appearance
		// changed" on frame 1 and re-apply the theme during startup.
		if (gLastSystemAppearance == VID_SYSTEM_APPEARANCE_UNKNOWN)
		{
			// Latch only a REAL value. Latching UNKNOWN would make the next
			// real reading look like a change and fire a spurious
			// notification -- and on Windows and Linux, where
			// VID_GetSystemAppearance() always returns UNKNOWN today, it would
			// leave the poll permanently comparing UNKNOWN to UNKNOWN.
			if (appearance != VID_SYSTEM_APPEARANCE_UNKNOWN)
				gLastSystemAppearance = appearance;
		}
		else if (appearance == VID_SYSTEM_APPEARANCE_UNKNOWN)
		{
			// UNKNOWN is "the platform could not tell us", not "dark". Folding
			// it into Dark would flip a light-mode user's theme on one
			// transient poll and then latch, so it would not flip back until
			// the OS appearance genuinely changed again. Ignore it and keep
			// the last known good value.
		}
		else if (appearance != gLastSystemAppearance)
		{
			gLastSystemAppearance = appearance;
			// Tell the host FIRST. ReapplyActiveTheme re-resolves the CURRENT
			// selection and therefore cannot switch a host's dark slot for its
			// light slot -- only the host knows it has two slots. The host's
			// callback is expected to call SetActiveTheme with the other slot,
			// which applies and fires onThemeChanged by itself.
			reg->NotifySystemAppearanceChanged(
				appearance == VID_SYSTEM_APPEARANCE_LIGHT ? MTThemeMode_Light
				                                          : MTThemeMode_Dark);
			if (gRequestedImGuiStyle == IMGUI_STYLE_SYSTEM)
				VID_SetImGuiStyle(IMGUI_STYLE_SYSTEM);
		}
	}
	VID_ApplyMainWindowColorGamut();
	VID_PollDisplayProfileChange();
}

// The viewport fix-up used to live inline in VID_Init, which meant every later
// VID_SetImGuiStyle / VID_ResetImGuiStyle silently dropped it: a user who
// switched style with viewports on got rounded, translucent platform windows
// until the next launch. Extracted so both the style path and the theme path
// re-apply it. Pre-existing defect, fixed here because the new apply path
// would otherwise inherit it.
void VID_ApplyViewportStyleOverrides()
{
	if (ImGui::GetCurrentContext() == NULL)
		return;
	ImGuiIO &io = ImGui::GetIO();
	ImGuiStyle &style = ImGui::GetStyle();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}
}

// Put the tail in a helper, because VID_SetImGuiStyle has TWO exits: the
// IMGUI_STYLE_CUSTOM branch returns long before the end of the function, so a
// hook placed only at the bottom never runs for a host on a custom style --
// and ClearActiveTheme() -> VID_ResetImGuiStyle() goes straight through it.
static void VID_FinishStyleChange()
{
	// Always: this can never recurse, and it is what keeps the viewport
	// override alive across a style switch.
	VID_ApplyViewportStyleOverrides();

	// If the host has an active theme, it owns the style: the engine has just
	// overwritten it. Re-apply. With no active theme -- every host that never
	// registers one -- this is a no-op and behaviour is unchanged.
	//
	// GUARDED, and the guard lives in the REGISTRY: an "am I nested in
	// VID_SetImGuiStyle" flag would be set on entry and therefore always set
	// when this runs, making the whole tail dead code on the outermost
	// legitimate call. The call that must be suppressed is the registry's own
	// VID_SetImGuiStyle for an imported theme, which is the OUTERMOST on its
	// stack.
	if (!CMTThemeRegistry::Instance()->IsApplyingTheme())
	{
		CMTThemeRegistry::Instance()->ReapplyActiveTheme();
	}

	// And the UI scale, for a host with NO theme -- the case the HiDPI notes
	// call out as the real limitation (docs/hidpi-ui-scaling.md #6). Every
	// branch above has just rebuilt geometry from a default ImGuiStyle, which
	// silently drops the scale; putting it back HERE, in the one tail every
	// style change passes through, means a host never has to notice a style
	// change and re-assert it. A themed host is left alone: its theme applied
	// the scale itself, and MT_UiScaleApplyToImGuiStyle() returns immediately.
	MT_UiScaleApplyToImGuiStyle();
}

void VID_SetImGuiStyle(ImGuiStyleType imGuiStyleType)
{
	if (imGuiStyleType < IMGUI_STYLE_DARK_ALTERNATIVE || imGuiStyleType > IMGUI_STYLE_CUSTOM)
	{
		imGuiStyleType = IMGUI_STYLE_DARK;
	}

	gRequestedImGuiStyle = imGuiStyleType;
	if (imGuiStyleType == IMGUI_STYLE_CUSTOM)
	{
		if (!VID_LoadCustomImGuiStyle())
		{
			// Custom style file missing or corrupt, fall back to dark
			ImGui::StyleColorsDark();
		}
		VID_ApplyMainWindowAppearance(IMGUI_STYLE_DARK);
		VID_FinishStyleChange();     // the SECOND exit -- see the helper
		return;
	}
	if (imGuiStyleType == IMGUI_STYLE_SYSTEM)
	{
		gLastSystemAppearance = VID_GetSystemAppearance();
	}
	ImGuiStyleType resolvedStyle = VID_ResolveImGuiStyle(imGuiStyleType);
	VID_ApplyMainWindowAppearance(resolvedStyle);
	imGuiStyleType = resolvedStyle;

	// StyleColorsDark/Light/Classic set COLOURS only, while the INTELIJ,
	// PHOTOSHOP, CORPORATE_GREY* and NICE branches below also set rounding,
	// borders and spacing -- and nothing ever put them back. Switching
	// PHOTOSHOP -> DARK therefore kept PHOTOSHOP's geometry. Reset the
	// geometry-carrying fields from a default-constructed style first;
	// colours are overwritten by the branch that follows either way.
	{
		ImGuiStyle defaults;
		ImGuiStyle &s = ImGui::GetStyle();
		s.WindowPadding = defaults.WindowPadding;   s.WindowRounding = defaults.WindowRounding;
		s.WindowBorderSize = defaults.WindowBorderSize;
		s.ChildRounding = defaults.ChildRounding;   s.ChildBorderSize = defaults.ChildBorderSize;
		s.PopupRounding = defaults.PopupRounding;   s.PopupBorderSize = defaults.PopupBorderSize;
		s.FramePadding = defaults.FramePadding;     s.FrameRounding = defaults.FrameRounding;
		s.FrameBorderSize = defaults.FrameBorderSize;
		s.ItemSpacing = defaults.ItemSpacing;       s.ItemInnerSpacing = defaults.ItemInnerSpacing;
		s.CellPadding = defaults.CellPadding;       s.IndentSpacing = defaults.IndentSpacing;
		s.ScrollbarSize = defaults.ScrollbarSize;   s.ScrollbarRounding = defaults.ScrollbarRounding;
		s.GrabMinSize = defaults.GrabMinSize;       s.GrabRounding = defaults.GrabRounding;
		s.TabRounding = defaults.TabRounding;       s.TabBorderSize = defaults.TabBorderSize;
	}

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

	VID_FinishStyleChange();
}

// -- custom ImGui style persistence (binary file next to settings.hjson) --

static const char *VID_GetCustomStyleFilePath()
{
	static char path[PATH_MAX];
	sprintf(path, "%s%s", gCPathToSettings, "custom-style.bin");
	return path;
}

void VID_SaveCustomImGuiStyle()
{
	const ImGuiStyle &style = ImGui::GetStyle();
	const char *filePath = VID_GetCustomStyleFilePath();
	FILE *f = fopen(filePath, "wb");
	if (f == NULL)
	{
		LOGError("VID_SaveCustomImGuiStyle: failed to open %s for writing", filePath);
		return;
	}
	size_t written = fwrite(&style, sizeof(ImGuiStyle), 1, f);
	fclose(f);
	if (written != 1)
	{
		LOGError("VID_SaveCustomImGuiStyle: failed to write style to %s", filePath);
	}
}

bool VID_LoadCustomImGuiStyle()
{
	const char *filePath = VID_GetCustomStyleFilePath();
	FILE *f = fopen(filePath, "rb");
	if (f == NULL)
	{
		return false;
	}
	ImGuiStyle savedStyle;
	size_t read = fread(&savedStyle, sizeof(ImGuiStyle), 1, f);
	fclose(f);
	if (read != 1)
	{
		LOGError("VID_LoadCustomImGuiStyle: failed to read style from %s", filePath);
		return false;
	}
	// Restore the loaded style into the live ImGui style
	ImGui::GetStyle() = savedStyle;
	return true;
}

bool VID_HasCustomImGuiStyle()
{
	const char *filePath = VID_GetCustomStyleFilePath();
	FILE *f = fopen(filePath, "rb");
	if (f == NULL)
	{
		return false;
	}
	fclose(f);
	return true;
}
