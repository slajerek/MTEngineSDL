#include "SYS_MacOS.h"
#include "SYS_MacOSWrapper.h"
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
	MACOS_StoreMainWindowPosition();
}

void VID_RestoreMainWindowPosition()
{
	MACOS_RestoreMainWindowPosition();
}

void MACOS_StoreMainWindowPosition()
{
//	LOGG("MACOS_StoreMainWindowPosition");
	
	NSWindow *mainWindow = [[[NSApplication sharedApplication] windows] objectAtIndex:0];
	NSRect frame = mainWindow.frame;
//	LOGD("... frame x=%f y=%f w=%f h=%f", frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
	
	[[NSUserDefaults standardUserDefaults] setObject:NSStringFromRect(frame) forKey:@"MainWindowFrameKey"];
}

void MACOS_RestoreMainWindowPosition()
{
//	LOGG("MACOS_RestoreMainWindowPosition");
	
	NSWindow *mainWindow = [[[NSApplication sharedApplication] windows] objectAtIndex:0];
	
	NSString *winFrameString = [[NSUserDefaults standardUserDefaults] stringForKey:@"MainWindowFrameKey"];
	
	if (winFrameString != nil)
	{
		NSRect savedRect = NSRectFromString(winFrameString);
//		LOGD("... savedRect x=%f y=%f w=%f h=%f", savedRect.origin.x, savedRect.origin.y, savedRect.size.width, savedRect.size.height);

		for (id screen in [NSScreen screens])
		{
			if (CGRectContainsRect(((NSScreen *)screen).visibleFrame, savedRect))
			{
				if (savedRect.size.width > 10 && savedRect.size.height > 10)
				{
					[mainWindow setFrame:savedRect display:NO];
					break;
				}
			}
		}
	}
}

