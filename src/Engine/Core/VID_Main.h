#ifndef _VID_MAIN_H_
#define _VID_MAIN_H_

#include "SYS_Defs.h"
#include "VID_Blits.h"
#include "VID_ImageBinding.h"
#include <GL/gl3w.h>
#include <SDL.h>

extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;
extern int SCREEN_FPS;
extern int SCREEN_TICK_PER_FRAME;

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
const char *VID_GetGlSlVersion();
void VID_SetupShaders();
SDL_Window *VID_GetMainSDLWindow();
void VID_RenderLoop();
void VID_StopEventsLoop();
void VID_Shutdown();

void VID_ResetLogicClock();

unsigned long VID_GetTickCount();
extern u64 gCurrentFrameTime;

bool VID_IsWindowAlwaysOnTop();
void VID_SetWindowAlwaysOnTop(bool isOnTop);
void VID_SetWindowAlwaysOnTopTemporary(bool isOnTop);

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

void VID_StoreMainWindowPosition();

void GUI_GetRealScreenPixelSizes(double *pixelSizeX, double *pixelSizeY);

void SYS_SetQuitKey(int keyCode, bool isShift, bool isAlt, bool isControl);

void VID_SetVSyncScreenRefresh(bool isVSyncRefresh);

void VID_GetStartupMainWindowPosition(int *x, int *y, int *width, int *height);
void VID_StoreMainWindowPosition();
void VID_RestoreMainWindowPosition();

void VID_LockRenderMutex();
void VID_UnlockRenderMutex();


#endif
