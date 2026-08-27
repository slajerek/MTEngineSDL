#include "SYS_MacOS.h"
#include "SYS_MacOSWrapper.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "MT_API.h"
#include <SDL3/SDL_video.h>
#include "SYS_DefaultConfig.h"
#include <SDL3/SDL.h>

static NSWindow *MACOS_GetNSWindowForSDLWindow(SDL_Window *sdlWindow)
{
	if (sdlWindow != NULL)
	{
		// SDL3 removed SDL_syswm.h entirely. The platform handle now comes out
		// of the window's property bag instead of an SDL_SysWMinfo struct.
		//
		// The failure mode changed with it, and that is the part worth noticing:
		// SDL_GetWindowWMInfo returned a bool you had to test, whereas
		// SDL_GetPointerProperty returns the DEFAULT you passed (NULL here) when
		// the property is absent. So the check moved onto the pointer, and the
		// fallback below still runs when it is missing.
		NSWindow *nsWindow = (__bridge NSWindow *)SDL_GetPointerProperty(
			SDL_GetWindowProperties(sdlWindow),
			SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
		if (nsWindow != nil)
		{
			return nsWindow;
		}
	}

	NSArray *windows = [[NSApplication sharedApplication] windows];
	if ([windows count] > 0)
	{
		return [windows objectAtIndex:0];
	}
	return nil;
}

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

VID_SystemAppearance MACOS_GetSystemAppearance()
{
	@autoreleasepool
	{
		if (@available(macOS 10.14, *))
		{
			NSAppearance *appearance = [[NSApplication sharedApplication] effectiveAppearance];
			if (appearance == nil)
			{
				appearance = [NSAppearance currentAppearance];
			}

			NSString *bestMatch = [appearance bestMatchFromAppearancesWithNames:@[
				NSAppearanceNameAqua,
				NSAppearanceNameDarkAqua
			]];
			if ([bestMatch isEqualToString:NSAppearanceNameDarkAqua])
			{
				return VID_SYSTEM_APPEARANCE_DARK;
			}
			if ([bestMatch isEqualToString:NSAppearanceNameAqua])
			{
				return VID_SYSTEM_APPEARANCE_LIGHT;
			}
			return VID_SYSTEM_APPEARANCE_UNKNOWN;
		}

		return VID_SYSTEM_APPEARANCE_LIGHT;
	}
}

// SETTING -[NSWindow appearance] IS A UI MUTATION AND MUST HAPPEN ON THE MAIN
// THREAD. It is not merely a property write: AppKit reacts by re-running
// -_windowDidChangeAppearance, which reaches _NSWindowSetShadowProperties and,
// on macOS 15 and later, the WindowManagement framework's
// -[NSWMWindowCoordinator performTransactionUsingBlock:]. Off the main thread
// that coordinator has no valid transaction and hard-traps with
// "assertion failure: transaction must be valid" -- an EXC_BREAKPOINT, not an
// exception, so nothing above can catch it.
//
// Found 2026-08-21 by MTEngineSDLDummyApp's `settings_theme_applies` ImGui
// test, which calls VID_SetDefaultImGuiStyle() straight from the
// imgui_test_engine coroutine thread -- a perfectly reasonable thing for a
// test (and for any worker) to do, since nothing in the VID_ API says
// otherwise. The whole call chain VID_SetDefaultImGuiStyle ->
// VID_SetImGuiStyle -> VID_ApplyMainWindowAppearance -> here was
// thread-unsafe on macOS from the day it landed; it only became a CRASH when
// the OS added the coordinator assertion.
//
// So the marshalling belongs HERE, at the one place that touches AppKit,
// rather than in every caller. ASYNC rather than sync, deliberately: this is
// cosmetic, no caller reads anything back, and a dispatch_sync to the main
// queue from a thread the main queue may itself be waiting on is a deadlock.
static void MACOS_ApplyWindowAppearanceOnMainThread(SDL_Window *sdlWindow, VID_SystemAppearance appearance)
{
	@autoreleasepool
	{
		if (@available(macOS 10.14, *))
		{
			NSWindow *window = MACOS_GetNSWindowForSDLWindow(sdlWindow);
			if (window == nil)
			{
				return;
			}

			if (appearance == VID_SYSTEM_APPEARANCE_DARK)
			{
				window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
			}
			else if (appearance == VID_SYSTEM_APPEARANCE_LIGHT)
			{
				window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
			}
			else
			{
				window.appearance = nil;
			}
		}
	}
}

void MACOS_ApplyWindowAppearance(SDL_Window *sdlWindow, VID_SystemAppearance appearance)
{
	if ([NSThread isMainThread])
	{
		MACOS_ApplyWindowAppearanceOnMainThread(sdlWindow, appearance);
		return;
	}

	dispatch_async(dispatch_get_main_queue(), ^{
		MACOS_ApplyWindowAppearanceOnMainThread(sdlWindow, appearance);
	});
}

VID_DisplayColorGamut MACOS_GetMainDisplayColorGamut(SDL_Window *sdlWindow)
{
	@autoreleasepool
	{
		NSWindow *window = MACOS_GetNSWindowForSDLWindow(sdlWindow);
		NSScreen *screen = window != nil ? [window screen] : nil;
		if (screen == nil)
		{
			screen = [NSScreen mainScreen];
		}
		if (screen == nil || [screen colorSpace] == nil)
		{
			return VID_DISPLAY_COLOR_GAMUT_UNKNOWN;
		}

		CGColorSpaceRef colorSpace = [[screen colorSpace] CGColorSpace];
		if (colorSpace == NULL)
		{
			return VID_DISPLAY_COLOR_GAMUT_UNKNOWN;
		}

		CFStringRef colorSpaceName = CGColorSpaceGetName(colorSpace);
		if (colorSpaceName != NULL && CFStringCompare(colorSpaceName, kCGColorSpaceDisplayP3, 0) == kCFCompareEqualTo)
		{
			return VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3;
		}

		if (@available(macOS 10.12, *))
		{
			if (CGColorSpaceIsWideGamutRGB(colorSpace))
			{
				return VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3;
			}
		}

		return VID_DISPLAY_COLOR_GAMUT_SRGB;
	}
}

float MACOS_GetMaxPotentialHdrHeadroom()
{
	@autoreleasepool
	{
		float best = 1.0f;
		// EVERY attached screen, not just the main one: the window can be
		// moved, and the gate's question is "can ANY display show this",
		// deliberately -- re-decoding a cache because a window changed
		// monitors is exactly what the once-per-session rule exists to
		// prevent.
		for (NSScreen *screen in [NSScreen screens])
		{
			const float pot =
				(float)screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
			if (pot > best)
				best = pot;
		}
		return best;
	}
}

void MACOS_ApplyWindowColorGamut(SDL_Window *sdlWindow, VID_DisplayColorGamut gamut)
{
	@autoreleasepool
	{
		NSWindow *window = MACOS_GetNSWindowForSDLWindow(sdlWindow);
		if (window == nil)
		{
			return;
		}

		if (@available(macOS 10.11, *))
		{
			if (gamut == VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3)
			{
				window.colorSpace = [NSColorSpace displayP3ColorSpace];
			}
			else if (gamut == VID_DISPLAY_COLOR_GAMUT_SRGB)
			{
				window.colorSpace = [NSColorSpace sRGBColorSpace];
			}
			else
			{
				window.colorSpace = nil;
			}
		}
	}
}
