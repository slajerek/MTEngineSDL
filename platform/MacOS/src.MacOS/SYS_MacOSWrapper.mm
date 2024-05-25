#include "SYS_MacOS.h"
#include "SYS_MacOSWrapper.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "MT_API.h"
#include "SDL_video.h"
#include "SYS_DefaultConfig.h"
#include <SDL.h>

void VID_GetStartupMainWindowPosition(int *x, int *y, int *width, int *height, bool *maximized)
{
	MT_GetDefaultWindowPositionAndSize(x, y, width, height, maximized);
	gApplicationDefaultConfig->GetBool("MainWindowMaximized", maximized, *maximized);
}

void VID_StoreMainWindowPosition()
{
	MACOS_StoreMainWindowPosition();
}

void VID_RestoreMainWindowPosition()
{
	MACOS_RestoreMainWindowPosition();
}

void MACOS_StoreMainWindowPosition()
{
//	LOGG("MACOS_StoreMainWindowPosition");
	
	SDL_Window* sdlMainWindow = VID_GetMainSDLWindow();
	bool maximized = (SDL_GetWindowFlags(sdlMainWindow) & SDL_WINDOW_MAXIMIZED) == 1;
	gApplicationDefaultConfig->SetBool("MainWindowMaximized", &maximized);

	NSWindow *mainWindow = [[[NSApplication sharedApplication] windows] objectAtIndex:0];
	NSRect frame = mainWindow.frame;
//	LOGD("... frame x=%f y=%f w=%f h=%f", frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
	
	int x = (int)frame.origin.x;
	int y = (int)frame.origin.y;
	int width = (int)frame.size.width;
	int height = (int)frame.size.height;

	gApplicationDefaultConfig->SetInt("MainWindowX", &x);
	gApplicationDefaultConfig->SetInt("MainWindowY", &y);
	gApplicationDefaultConfig->SetInt("MainWindowWidth", &width);
	gApplicationDefaultConfig->SetInt("MainWindowHeight", &height);
//	[[NSUserDefaults standardUserDefaults] setObject:NSStringFromRect(frame) forKey:@"MainWindowFrameKey"];
}

void MACOS_RestoreMainWindowPosition()
{
//	LOGG("MACOS_RestoreMainWindowPosition");
	
	NSWindow *mainWindow = [[[NSApplication sharedApplication] windows] objectAtIndex:0];
	
	if (!gApplicationDefaultConfig->E_x_i_s_t_s("MainWindowX"))
	{
		return;
	}
	
	int defaultX, defaultY, defaultWidth, defaultHeight;
	bool defaultMaximized;
	MT_GetDefaultWindowPositionAndSize(&defaultX, &defaultY, &defaultWidth, &defaultHeight, &defaultMaximized);

	bool maximized;
	int x, y, width, height;

	gApplicationDefaultConfig->GetBool("MainWindowMaximized", &maximized, defaultMaximized);
	gApplicationDefaultConfig->GetInt("MainWindowX", &x, defaultX);
	gApplicationDefaultConfig->GetInt("MainWindowY", &y, defaultY);
	gApplicationDefaultConfig->GetInt("MainWindowWidth", &width, defaultWidth);
	gApplicationDefaultConfig->GetInt("MainWindowHeight", &height, defaultHeight);
	
	if (maximized)
	{
		SDL_Window* sdlMainWindow = VID_GetMainSDLWindow();
		SDL_MaximizeWindow(sdlMainWindow);
		return;
	}
	
	NSRect savedRect;
	savedRect.origin.x = (float)x;
	savedRect.origin.y = (float)y;
	savedRect.size.width = (float)width;
	savedRect.size.height = (float)height;
		

//	NSString *winFrameString = [[NSUserDefaults standardUserDefaults] stringForKey:@"MainWindowFrameKey"];
//
//	if (winFrameString != nil)
//	{
//		NSRect savedRect = NSRectFromString(winFrameString);
////		LOGD("... savedRect x=%f y=%f w=%f h=%f", savedRect.origin.x, savedRect.origin.y, savedRect.size.width, savedRect.size.height);

		for (id screen in [NSScreen screens])
		{
			NSRect visibleFrame = ((NSScreen *)screen).visibleFrame;

//			if (CGRectContainsRect(visibleFrame, savedRect))
			if (   savedRect.origin.x >= visibleFrame.origin.x && savedRect.origin.x < visibleFrame.origin.x-10 + visibleFrame.size.width
				&& savedRect.origin.y >= visibleFrame.origin.y && savedRect.origin.y < visibleFrame.origin.y-10 + visibleFrame.size.height)			
			{
				if (savedRect.size.width > 10 && savedRect.size.height > 10)
				{
					[mainWindow setFrame:savedRect display:NO];
					break;
				}
			}
		}
//	}
}

