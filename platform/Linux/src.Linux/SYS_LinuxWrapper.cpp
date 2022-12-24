#include "SYS_LinuxWrapper.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "MT_API.h"
#include "SYS_DefaultConfig.h"
#include <SDL.h>

void VID_GetStartupMainWindowPosition(int *x, int *y, int *width, int *height, bool *maximized)
{
	// say hello to microsoft for blocking the "Exists" method name
	if (!gApplicationDefaultConfig->E_x_i_s_t_s("MainWindowX"))
	{
		// default values
		MT_GetDefaultWindowPositionAndSize(x, y, width, height, maximized);
		return;
	}

	int storedX, storedY, storedWidth, storedHeight;

	//
	int defaultX, defaultY, defaultWidth, defaultHeight;
	bool defaultMaximized;
	MT_GetDefaultWindowPositionAndSize(&defaultX, &defaultY, &defaultWidth, &defaultHeight, &defaultMaximized);

	gApplicationDefaultConfig->GetBool("MainWindowMaximized", maximized, defaultMaximized);
	gApplicationDefaultConfig->GetInt("MainWindowX", &storedX, defaultX);
	gApplicationDefaultConfig->GetInt("MainWindowY", &storedY, defaultY);
	gApplicationDefaultConfig->GetInt("MainWindowWidth", &storedWidth, defaultWidth);
	gApplicationDefaultConfig->GetInt("MainWindowHeight", &storedHeight, defaultHeight);

	*x = storedX;
	*y = storedY;
	*width = storedWidth;
	*height = storedHeight;
}

void VID_StoreMainWindowPosition()
{
    	SDL_Window* sdlMainWindow = VID_GetMainSDLWindow();
    	
    	if (!sdlMainWindow)
    		return;

	int x, y, width, height;
	SDL_GetWindowPosition(sdlMainWindow, &x, &y);
	SDL_GetWindowSize(sdlMainWindow, &width, &height);

	bool maximized = (SDL_GetWindowFlags(sdlMainWindow) & SDL_WINDOW_MAXIMIZED);
	//LOGD("VID_StoreMainWindowPosition: maximized=%s", STRBOOL(maximized));

	//
	if (!maximized)
	{
		gApplicationDefaultConfig->SetInt("MainWindowX", &x);
		gApplicationDefaultConfig->SetInt("MainWindowY", &y);
		gApplicationDefaultConfig->SetInt("MainWindowWidth", &width);
		gApplicationDefaultConfig->SetInt("MainWindowHeight", &height);
	}
	gApplicationDefaultConfig->SetBool("MainWindowMaximized", &maximized);
}

void VID_RestoreMainWindowPosition()
{
}

