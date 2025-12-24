#ifndef _VID_MAIN_H_
#define _VID_MAIN_H_

#include "SYS_Defs.h"
#include "CRenderBackend.h"
#include "VID_Blits.h"
#include "VID_ImageBinding.h"
#include <SDL.h>

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
void VID_StopEventsLoop();
void VID_Shutdown();

void VID_ResetLogicClock();
u64  VID_GetCurrentFrameNumber();

unsigned long VID_GetTickCount();
extern u64 gCurrentFrameTime;
extern bool gViewportsEnableInitAtStartup;

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
	IMGUI_STYLE_NICE
};

void VID_SetImGuiStyle(ImGuiStyleType imGuiStyleType);
void VID_SetDefaultImGuiStyle(ImGuiStyleType imGuiStyleType);	// also store as default config
ImGuiStyleType VID_GetDefaultImGuiStyle();
void VID_ResetImGuiStyle();

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

void VID_GetStartupMainWindowPosition(int *x, int *y, int *width, int *height, bool *maximized);
void VID_StoreMainWindowPosition();
void VID_RestoreMainWindowPosition();

void VID_SetMainWindowTitle(const char *title);

void VID_LockRenderMutex();
void VID_UnlockRenderMutex();

#endif
