#ifndef _MT_API_H_
#define _MT_API_H_

const char *MT_GetMainWindowTitle();
const char *MT_GetSettingsFolderName();
void MT_GetDefaultWindowPositionAndSize(int *defaultWindowPosX, int *defaultWindowPosY, int *defaultWindowWidth, int *defaultWindowHeight);
void MT_PreInit();
void MT_GuiPreInit();
void MT_PostInit();
void MT_Render();
void MT_PostRenderEndFrame();

//int MT_GetDefaultFPS();

#endif
