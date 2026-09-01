#ifndef _VID_MAIN_H_
#define _VID_MAIN_H_

#include "SYS_Defs.h"
#include "CRenderBackend.h"
#include "VID_Blits.h"
#include "VID_ImageBinding.h"
#include <SDL3/SDL.h>

extern CRenderBackend *gRenderBackend;
class CGuiView;

extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;

#define FRAMES_PER_SECOND 60
#define LOADING_SCREEN_FPS 5

#define SCREEN_SCALE 1.0
#define VIEW_START_X 0.0
#define VIEW_START_Y 0.0

#define SCREEN_EDGE_HEIGHT 2.0
#define SCREEN_EDGE_WIDTH  2.0

//extern float SCREEN_SCALE;
//extern float SCREEN_ASPECT_RATIO;

void VID_Init();
SDL_Window *VID_GetMainSDLWindow();
void VID_PostInit();
void VID_RenderLoop();

// ---------------------------------------------------------------------------
// Render-thread identity (S-5)
//
// Some conversions are O(pixels) bulk work: right on a decode worker, a
// frame-time stall proportional to megapixels in the render loop. "Be careful"
// does not survive six months, so the ones that matter assert instead.
//
// VID_MarkRenderThread() is called once at the top of VID_RenderLoop();
// VID_IsRenderThread() compares std::thread::id, so it is portable and works
// for any thread that asks -- unlike pthread_main_np(), which only answers
// "am I the process main thread" and only on Apple platforms.
// ---------------------------------------------------------------------------
void VID_MarkRenderThread();
bool VID_IsRenderThread();

// Debug-only, no release cost. Fires at the moment the mistake is made rather
// than as a mysterious hitch during culling.
#if defined(DEBUG) || defined(_DEBUG)
	#define MT_ASSERT_NOT_RENDER_THREAD(what) \
		do { \
			if (VID_IsRenderThread()) \
				LOGError("%s: bulk O(pixels) conversion on the RENDER THREAD -- " \
						 "this belongs on a decode worker", (what)); \
		} while (0)
#else
	#define MT_ASSERT_NOT_RENDER_THREAD(what) do {} while (0)
#endif
void VID_StopEventsLoop();
void VID_Shutdown();

void VID_ResetLogicClock();
u64  VID_GetCurrentFrameNumber();

// True while VID_Render() is between ImGui::NewFrame() and ImGui::Render().
// Code that mutates OS window/menu state can check this to defer work that
// would otherwise re-enter the renderer (see the guard in VID_Render()).
bool VID_IsRenderingFrame();

unsigned long VID_GetTickCount();
extern u64 gCurrentFrameTime;
extern bool gViewportsEnableInitAtStartup;
extern bool gHeadlessMode;
extern bool gServiceMode;	// skip VID/GUI/SND init entirely (pure headless service)

enum ImGuiStyleType : int
{
	IMGUI_STYLE_DARK_ALTERNATIVE = 0,
	IMGUI_STYLE_DARK,
	IMGUI_STYLE_LIGHT,
	IMGUI_STYLE_CLASSIC,
	IMGUI_STYLE_INTELIJ,
	IMGUI_STYLE_PHOTOSHOP,
	IMGUI_STYLE_CORPORATE_GREY,
	IMGUI_STYLE_CORPORATE_GREY_3D,
	IMGUI_STYLE_NICE,
	IMGUI_STYLE_SYSTEM,
	IMGUI_STYLE_CUSTOM
};

enum VID_SystemAppearance : int
{
	VID_SYSTEM_APPEARANCE_UNKNOWN = 0,
	VID_SYSTEM_APPEARANCE_LIGHT,
	VID_SYSTEM_APPEARANCE_DARK
};

enum VID_DisplayColorGamut : int
{
	VID_DISPLAY_COLOR_GAMUT_UNKNOWN = 0,
	VID_DISPLAY_COLOR_GAMUT_SRGB,
	VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3
};

void VID_SetAppDefaultImGuiStyle(ImGuiStyleType imGuiStyleType);
void VID_SetImGuiStyle(ImGuiStyleType imGuiStyleType);
void VID_SetDefaultImGuiStyle(ImGuiStyleType imGuiStyleType);	// also store as default config
ImGuiStyleType VID_GetDefaultImGuiStyle();
ImGuiStyleType VID_ResolveImGuiStyle(ImGuiStyleType imGuiStyleType);
void VID_ResetImGuiStyle();
// Re-applies the multi-viewport style fix-up (square, opaque platform windows).
// Must run after ANY style or theme apply, not only at VID_Init -- see the
// comment on the definition.
void VID_ApplyViewportStyleOverrides();
void VID_SaveCustomImGuiStyle();
bool VID_LoadCustomImGuiStyle();
bool VID_HasCustomImGuiStyle();

VID_SystemAppearance VID_GetSystemAppearance();
const char *VID_GetSystemAppearanceName(VID_SystemAppearance appearance);
VID_DisplayColorGamut VID_GetMainDisplayColorGamut();
VID_DisplayColorGamut VID_GetMainWindowRenderColorGamut();
const char *VID_GetDisplayColorGamutName(VID_DisplayColorGamut gamut);
bool VID_IsMainDisplayWideGamut();
void VID_ApplyMainWindowColorGamut();

// ICC profile of the display the MAIN WINDOW occupies -- not the OS primary
// monitor; the name follows the VID_GetMainDisplayColorGamut convention above.
//
// The colour-gamut enum is a three-value classification and cannot represent a
// custom-calibrated monitor, which is the entire point of colour management: a
// display profiled with a colorimeter is neither "sRGB" nor "Display P3", it
// is a unique measured profile. These return the bytes.
//
// Never returns false with garbage and never yields NULL: when discovery fails
// (headless, Wayland, an unconfigured X11 session) it falls back to the
// built-in sRGB profile, because silently disabling colour management is a
// worse outcome than assuming the near-universal default.
//
// Caller frees *outBytes with delete[].
bool VID_GetMainDisplayICCProfile(u8 **outBytes, u32 *outSize);

// Same for an explicit window. Passing NULL forces the sRGB fallback, which is
// the only way to reach that branch on a machine whose display discovery
// works -- the test seam for it.
bool VID_GetDisplayICCProfileForWindow(SDL_Window *window, u8 **outBytes, u32 *outSize);

// Bumps whenever the profile above may have changed: the window moved to a
// differently-profiled display, the user recalibrated, or the OS display
// arrangement changed. Cheap enough to poll (an atomic read) -- it is driven
// by events, never by re-fetching the profile, because on Windows a fetch is
// a file read.
//
// This is what a colour epoch watches. The existing per-frame gamut check
// cannot serve that purpose: a monitor re-profiled from one sRGB-class profile
// to another never changes gamut classification, yet that recalibration is
// exactly the event this feature exists to honour.
u64 VID_GetMainDisplayProfileSerial();

// Event tap: call for every SDL_WINDOWEVENT. Bumps the serial on
// ICCPROF_CHANGED / DISPLAY_CHANGED, and only for the main window. Public
// precisely so tests can feed it synthetic events -- the bump logic is
// otherwise unobservable headless.
void VID_HandleDisplayProfileEvent(const SDL_Event *event);

// Per-frame supplement, Windows-only in effect and a no-op elsewhere. SDL
// caches the Windows ICM filename and refreshes it only on window creation,
// activation, or a display change, so a recalibration that swaps the profile
// file while the window keeps focus is invisible to it. A message hook flags
// WM_DISPLAYCHANGE / WM_SETTINGCHANGE and this compares the profile's actual
// bytes -- WM_SETTINGCHANGE is broadcast for many unrelated settings, so
// bumping the serial without comparing would re-decode everything for a
// mouse-speed change. Already called from the engine's per-frame refresh.
void VID_PollDisplayProfileChange();

void VID_SetViewportsEnable(bool viewportsEnable);
bool VID_IsViewportsEnable();

SDL_Window *VID_GetSDLViewportWindowFromCGuiView(CGuiView *view);
SDL_Window *VID_GetSDLWindowFromCGuiView(CGuiView *view);

bool VID_IsMainWindowAlwaysOnTop();
void VID_SetMainWindowAlwaysOnTop(bool isOnTop);
void VID_SetMainWindowAlwaysOnTopTemporary(bool isOnTop);

bool VID_IsWindowAlwaysOnTop(CGuiView *view);
void VID_SetWindowAlwaysOnTop(CGuiView *view, bool isOnTop);

bool VID_IsMainApplicationWindowFullScreen();
void VID_SetMainApplicationWindowFullScreen(bool isFullScreen);

bool VID_IsMouseCursorVisible();
void VID_ShowMouseCursor();
void VID_HideMouseCursor();

void VID_SetClipping(int x, int y, int sizeX, int sizeY);
void VID_ResetClipping();

void VID_ResetLogicClock();
void VID_SetFPS(float fps);

float VID_GetScreenWidth();
float VID_GetScreenHeight();

u32 VID_GetMousePos(int *posX, int *posY);

void VID_StoreMainWindowPosition();

void GUI_GetRealScreenPixelSizes(double *pixelSizeX, double *pixelSizeY);

void SYS_SetQuitKey(int keyCode, bool isShift, bool isAlt, bool isControl);

void VID_SetVSyncScreenRefresh(bool isVSyncRefresh);

void VID_RaiseMainWindow();

CRenderBackend *VID_GetRenderBackend();

// ---------------------------------------------------------------------------
// Render backend selection
// ---------------------------------------------------------------------------
//
// Two vocabularies, and confusing them is the bug this comment exists to stop.
// The SELECTION name is lowercase and is what the config file and
// `--render-backend=` use: "opengl", "metal", "d3d11". The RUNNING name is the
// live object's own `name` and is capitalised: "OpenGL4", "Metal", "D3D11".
// VID_GetCurrentRenderBackendName() returns the second; everything else here
// takes or returns the first.

// LIFETIME, and it applies to the three getters that return a NAME:
// VID_GetPreferredRenderBackend(), VID_GetPersistedRenderBackend() and
// VID_GetEffectiveRenderBackendSelection() share ONE per-thread buffer. A
// result stays valid across VID_SetPreferredRenderBackend() -- that is the
// use-after-free A2 fixed, where the hjson config freed the string a picker was
// still comparing -- but calling a SECOND of these three silently overwrites
// the first. Hold one at a time, or copy into a std::string. (The three UI
// pickers and the ImGui test all do; this note exists so the next caller does
// too.) VID_GetPreferredRenderBackend() additionally returns a
// sysCommandLineArguments pointer or a literal on two of its three paths, so
// never assume WHICH storage you have.
// The backend a fresh install gets on THIS platform: "metal" on macOS, "d3d11"
// on a Windows build that has it, "opengl" everywhere else AND wherever the
// preferred one fails its device probe. A returned literal, so it has none of
// the shared-buffer lifetime problem the three getters below have.
//
// This is what "the default" means throughout this file. It is NOT a migration:
// a config that already persisted a backend keeps it.
const char *VID_GetDefaultRenderBackend();

const char *VID_GetPreferredRenderBackend();          // reads --render-backend, then config; VID_GetDefaultRenderBackend() if unset
void        VID_SetPreferredRenderBackend(const char *name); // writes config; rejects names not available HERE

// Is `name` a backend THIS PLATFORM can actually run?
//
// Replaces the old bare `VID_IsRenderBackendSwitchable()` boolean, which said
// only "more than one exists" and left every caller to hardcode WHICH. That is
// how c64d's menu came to offer "Metal" on Windows -- the guard was true and
// the menu items were written out by hand. Ask this instead, or better,
// enumerate with VID_GetAvailableRenderBackends().
//
// On Windows "d3d11" is available only in a build that defines
// MT_RENDER_BACKEND_D3D11 (S-6), so a plain OpenGL Windows build answers
// exactly as it did before this existed.
bool        VID_IsRenderBackendAvailable(const char *name);

// Every available backend's SELECTION name, in display order, into `outNames`.
// Returns how many were written. The entries are static strings that outlive
// the call. This is what a picker or a menu should iterate: writing the list
// out by hand is precisely how a platform ends up offering a backend it does
// not have.
int         VID_GetAvailableRenderBackends(const char **outNames, int maxNames);

// The human label for a SELECTION name -- "OpenGL", "Metal", "Direct3D 11".
// For UI with no i18n of its own (c64d's and the game app's menus). the photo app
// translates instead, via settings.appearance.renderer.<name>.
const char *VID_GetRenderBackendDisplayName(const char *name);

// The SELECTION name that the NEXT LAUNCH will actually use: the persisted
// choice if this platform can run it, else "opengl". Every picker and menu
// should tick against THIS, not against a raw string compare -- two of the
// three UIs that did the latter had no answer for a persisted value naming a
// backend the build does not have, and left nothing ticked.
const char *VID_GetEffectiveRenderBackendSelection();

// The SELECTION name of the RUNNING backend, mapped back from its capitalised
// `name`. So a menu can title itself in the same vocabulary as its items.
const char *VID_GetCurrentRenderBackendSelection();

// Can `name` drive an HDR surface on THIS platform? True for "metal" on macOS
// and "d3d11" on Windows.
//
// Ask this about the PERSISTED backend, never about the live one: a backend
// switch needs a restart, so a live-backend query would grey the HDR control
// out for a user who has just chosen a capable backend, and leave it enabled
// after they chose an incapable one.
bool        VID_IsRenderBackendHdrCapable(const char *name);

// HDR: requested (a setting or --hdr=on|off|auto) vs granted (a live poll).
bool        VID_IsHdrRequested();
float       VID_GetDisplayHdrHeadroom();
const char *VID_GetCurrentRenderBackendName();        // the RUNNING backend ("Metal"/"OpenGL4"/"D3D11")
bool        VID_IsRenderBackendSwitchable();          // "more than one backend is available here"

// The persisted HDR preference, as one of "auto" | "on" | "off". THREE states,
// not a bool: `auto`'s heuristic is a later stage's problem, and a boolean
// would force a settings migration to add the third value back.
// The largest POTENTIAL EDR headroom any attached display can grant, 1.0 if
// none can. POTENTIAL rather than current: the current value reads 1.0 until
// EDR content is on screen and then ramps over seconds, so it is useless as an
// init-time capability question. Returns 1.0 on platforms without a probe.
float VID_GetMaxPotentialHdrHeadroom();

// LIVE, NEVER CACHED, unlike VID_GetMaxPotentialHdrHeadroom() above (which is
// deliberately latched once per session -- see its own comment and
// VID_IsHdrRequested()'s "poll, never latch... except this one thing").
// This exists for the Settings UI's "On" radio specifically: a user can
// disconnect the display they launched with and connect an HDR-capable one
// mid-session, and the control must notice on its very next redraw, not on
// the next app launch. Cheap enough to call every frame the Settings pane is
// visible (an SDL display-property walk on Windows, no DXGI, no device) --
// do NOT call this from a hot per-frame render-thread path the way the
// stale, uncached call to VID_GetMaxPotentialHdrHeadroom() from
// PC_ResidentFormat.cpp was found doing (S-6 whole-subphase review); this
// function is for UI code only.
bool VID_IsAnyDisplayHdrCapable();

const char *VID_GetHdrMode();
void        VID_SetHdrMode(const char *mode);

// The PERSISTED preferences, ignoring any --render-backend / --hdr override.
//
// Settings UI must read these, not VID_GetPreferredRenderBackend() /
// VID_IsHdrRequested(). Those check the command line FIRST, so under an
// override the UI would show the flag's value rather than the user's saved
// choice -- and every click would appear to do nothing, because the getter
// keeps returning the override no matter what was written. Show the saved
// choice, and use VID_Is*OverriddenByCommandLine() to tell the user a flag is
// winning this run.
// Returns a COPY (a thread-local buffer), NOT a pointer into the config store.
// That matters: the store frees the old string on the next SetString for the
// same key, so the raw pointer would dangle the moment a picker writes a new
// choice -- which every picker does from inside the loop that reads this.
const char *VID_GetPersistedRenderBackend();
const char *VID_GetPersistedHdrMode();
bool        VID_IsRenderBackendOverriddenByCommandLine();
bool        VID_IsHdrOverriddenByCommandLine();

void VID_GetStartupMainWindowPosition(int *x, int *y, int *width, int *height, bool *maximized);
void VID_StoreMainWindowPosition();
void VID_RestoreMainWindowPosition();

void VID_SetMainWindowTitle(const char *title);

void VID_LockRenderMutex();
void VID_UnlockRenderMutex();

#endif
