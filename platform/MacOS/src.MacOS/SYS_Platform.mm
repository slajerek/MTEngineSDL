#include "SYS_Platform.h"
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include "DBG_Log.h"

void SYS_PlatformInit()
{
}

void SYS_PlatformShutdown()
{
}

void SYS_AttachConsole()
{
}

bool MACOS_IsApplicationFullScreen()
{
	NSApplicationPresentationOptions opts = [[NSApplication sharedApplication ] presentationOptions];
	if ( opts & NSApplicationPresentationFullScreen)
	{
		LOGD("MACOS_IsApplicationFullScreen: return true");
		return true;
	}
	LOGD("MACOS_IsApplicationFullScreen: return false");
	return false;
	
}

// fix for broken SDL_ShowCursor
static volatile bool VID_isMouseCursorVisible = true;
void VID_ShowMouseCursor()
{
        LOGM("VID_ShowMouseCursor");
        dispatch_async(dispatch_get_main_queue(), ^{
                if (VID_isMouseCursorVisible == false)
                {
					VID_isMouseCursorVisible = true;
					[NSCursor unhide];
                }
        });
}

void VID_HideMouseCursor()
{
        LOGM("VID_HideMouseCursor");
        dispatch_async(dispatch_get_main_queue(), ^{
                if (VID_isMouseCursorVisible == true)
                {
					VID_isMouseCursorVisible = false;
					[NSCursor hide];
                }
        });
}

bool VID_IsMouseCursorVisible()
{
	return VID_isMouseCursorVisible;
}
