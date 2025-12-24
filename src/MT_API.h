#ifndef _MT_API_H_
#define _MT_API_H_

const char *MT_GetMainWindowTitle();
const char *MT_GetSettingsFolderName();
void MT_GetDefaultWindowPositionAndSize(int *defaultWindowPosX, int *defaultWindowPosY, int *defaultWindowWidth, int *defaultWindowHeight, bool *maximized);

// command line starts with
void MT_PreInit();

// then we init drivers and
void MT_GuiPreInit();

// finally when engine init is completed
void MT_PostInit();

// and in the loop
void MT_Render();
void MT_PostRenderEndFrame();

void MT_Shutdown();

// TODO:
//int MT_GetDefaultFPS();

#endif
