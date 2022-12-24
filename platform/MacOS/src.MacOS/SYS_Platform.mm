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

void PLATFORM_SetThreadName(const char *name)
{
	[[NSThread currentThread] setName:[NSString stringWithUTF8String:name]];
}

void SYS_RestartApplication()
{
	LOGM("SYS_RestartApplication");
	SYS_PlatformShutdown();
	
	NSString *path = [NSBundle mainBundle].resourcePath;
	NSLog(@"path=%@", path);
	
	NSString *appPath = [[path stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
	NSLog(@"appPath=%@", appPath);
	
	NSArray *arguments = [NSArray arrayWithObjects: appPath, nil];
	NSTask *task = [[NSTask alloc] init];
	[task setLaunchPath: @"/usr/bin/open"];
	[task setArguments:arguments];
	[task launch];
	
	exit(0);
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

void PLATFORM_UpdateMenus()
{
	// remove Close Window (Cmd+W), it will be handled by SDL2/ImGui

	//	NSMenu *menu = [NSApp mainMenu];
	
	NSMenu  *mainMenu = [[NSApplication sharedApplication] mainMenu];
	LOGD("PLATFORM_UpdateMenus: count=%d", [mainMenu numberOfItems]);
	
	NSMenu  *appMenu = [[mainMenu itemAtIndex:1] submenu];

//	[appMenu removeAllItems];
//	for (int i = 0; i < [appMenu numberOfItems]; i++)
//	{
//		NSMenuItem *menuItem = [appMenu itemAtIndex:i];
//		NSLog(@"%d menuItem=%@", i, [menuItem title]);
//	}
	
	// remove Ctrl+W Close Window
	[appMenu removeItemAtIndex:0];

	// remove Close All
	[appMenu removeItemAtIndex:0];

	// remove Minimize Cmd+M (as Cmd+M is switch memory)
	[appMenu removeItemAtIndex:0];

	// example adding menu
//	NSMenuItem *item=[[NSMenuItem alloc]initWithTitle:@"Tutorial" action:@selector(actionTutorial:) keyEquivalent:@"T"];
//
//	[item setTarget:self];
//	[item setTag:0];
//
//	[appMenu addItem:item];
//	[appMenu removeItemAtIndex:2]; // which is the index of above added item
	
	LOGD("PLATFORM_UpdateMenus: DONE");
}
