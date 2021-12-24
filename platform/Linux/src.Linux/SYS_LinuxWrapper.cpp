#include "SYS_LinuxWrapper.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "MT_API.h"
#include <SDL.h>

void VID_GetStartupMainWindowPosition(int *x, int *y, int *width, int *height)
{
    MT_GetDefaultWindowPositionAndSize(x, y, width, height);
    SCREEN_WIDTH = *width;
    SCREEN_HEIGHT = *height;
}

void VID_StoreMainWindowPosition()
{
}

void VID_RestoreMainWindowPosition()
{
}

