#include "SYS_Platform.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include "win-registry.h"
#include "MT_API.h"
#include "VID_Main.h"
#include "SYS_DefaultConfig.h"

void SYS_PlatformInit()
{
}

void SYS_PlatformShutdown()
{
	SYS_DetachWindowsConsole();
}

void SYS_RestartApplication()
{
	LOGM("SYS_RestartApplication");
	SYS_PlatformShutdown();

	exit(0);
}

void SYS_AttachConsole()
{
	SYS_AttachWindowsConsoleToStdOutIfNotRedirected();
}

static bool isWin32ConsoleAttached = false;

void SYS_AttachWindowsConsoleToStdOutIfNotRedirected()
{
	// check if stdout is already redirected
	bool isRedirected = false;
	HANDLE handleStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD fileTypeStdOut = GetFileType(handleStdOut);
	if (fileTypeStdOut == FILE_TYPE_DISK || fileTypeStdOut == FILE_TYPE_PIPE)
	{
		isRedirected = true;
	}

	if (isRedirected == FALSE)
	{
		if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
		{
			//printf("isRedirected=%d fileTypeStdOut=%d\n", isRedirected, fileTypeStdOut);

			isWin32ConsoleAttached = true;
			freopen("CONOUT$", "w", stdout);
			freopen("CONOUT$", "w", stderr);

			fprintf(stdout, "\n");
		}
	}

}

void SYS_DetachWindowsConsole()
{
	if (isWin32ConsoleAttached) FreeConsole();
}

void SYS_SetMainProcessPriorityBoostDisabled(bool isPriorityBoostDisabled)
{
    LOGD("SYS_SetMainProcessPriorityBoostDisabled: isPriorityBoostDisabled=%s", STRBOOL(isPriorityBoostDisabled));

    BOOL bDisablePriorityBoost = isPriorityBoostDisabled ? TRUE:FALSE;
    
    if (SetProcessPriorityBoost(GetCurrentProcess(), bDisablePriorityBoost) == 0)
    {
	LOGError("SetProcessPriorityBoost failed");
	//MessageBox(NULL, "SetProcessPriorityBoost failed", "Error", MB_OK);
    }
    else
    {
	//MessageBox(NULL, "SetProcessPriorityBoost OK", "OK", MB_OK);
    }

}

void SYS_SetMainProcessPriority(int priority)
{
    LOGD("SYS_SetMainProcessPriority: priority=%d", priority);

    DWORD dwPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    switch(priority)
    {
	case MT_PRIORITY_IDLE:
	    dwPriorityClass = IDLE_PRIORITY_CLASS;
	    break;
	case MT_PRIORITY_BELOW_NORMAL:
	    dwPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
	    break;
	case MT_PRIORITY_NORMAL:
	    dwPriorityClass = NORMAL_PRIORITY_CLASS;
	    break;
	case MT_PRIORITY_ABOVE_NORMAL:
	    dwPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
	    break;
	case MT_PRIORITY_HIGH_PRIORITY:
	    dwPriorityClass = HIGH_PRIORITY_CLASS;
	    break;
    }

    if (SetPriorityClass(GetCurrentProcess(), dwPriorityClass) == 0)
    {
	LOGError("SetPriorityClass to 0x%x failed", dwPriorityClass);
	//MessageBox(NULL, "SetPriorityClass failed", "Error", MB_OK);
    }
    else
    {
	//MessageBox(NULL, "SetPriorityClass OK", "OK", MB_OK);
    }
}

void VID_StoreMainWindowPosition()
{
	//LOGD("VID_StoreMainWindowPosition");
	SDL_Window* sdlMainWindow = VID_GetMainSDLWindow();

	int x, y, width, height;
	SDL_GetWindowPosition(sdlMainWindow, &x, &y);
	SDL_GetWindowSize(sdlMainWindow, &width, &height);

	int hb = GetSystemMetrics(SM_CYCAPTION);
	y -= hb;

	width = SDL_GetWindowSurface(sdlMainWindow)->w;
	height = SDL_GetWindowSurface(sdlMainWindow)->h + hb;

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
	// this does not work on Windows
	return;
}

void VID_GetStartupMainWindowPosition(int* x, int* y, int* width, int* height, bool *maximized)
{
	DWORD windowLeft, windowRight, windowTop, windowBottom;
	int windowWidth;
	int windowHeight;

	if (!gApplicationDefaultConfig->E_x_i_s_t_s("MainWindowX"))
	{
		// default values
		MT_GetDefaultWindowPositionAndSize(x, y, width, height, maximized);
		return;
	}

	int storedLeft, storedTop, storedWidth, storedHeight;

	//
	int defaultX, defaultY, defaultWidth, defaultHeight;
	bool defaultMaximized;
	MT_GetDefaultWindowPositionAndSize(&defaultX, &defaultY, &defaultWidth, &defaultHeight, &defaultMaximized);

	gApplicationDefaultConfig->GetBool("MainWindowMaximized", maximized, defaultMaximized);
	gApplicationDefaultConfig->GetInt("MainWindowX", &storedLeft, defaultX);
	gApplicationDefaultConfig->GetInt("MainWindowY", &storedTop, defaultY);
	gApplicationDefaultConfig->GetInt("MainWindowWidth", &storedWidth, defaultWidth);
	gApplicationDefaultConfig->GetInt("MainWindowHeight", &storedHeight, defaultHeight);

	windowLeft = storedLeft;
	windowTop = storedTop;

	///
	windowRight = windowLeft + storedWidth;
	windowBottom = windowTop + storedHeight;

	// the following correction is needed when the taskbar is
	// at the left or top and it is not "auto-hidden"
	RECT workArea;
	SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
	windowLeft += workArea.left;
	windowTop += workArea.top;
 
	// make sure the window is not completely out of sight
	int minX = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int minY = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int maxX = GetSystemMetrics(SM_CXVIRTUALSCREEN) - GetSystemMetrics(SM_CXICON);
	int maxY = GetSystemMetrics(SM_CYVIRTUALSCREEN) - GetSystemMetrics(SM_CYICON);
	if (windowLeft < minX
		|| windowTop < minY)
	{
		LOGError("Main window outside visible area: left=%d minX=%d top=%d minY=%d", windowLeft, minX, windowTop, minY);
		return;
	}

	if (windowLeft > maxX
		|| windowTop > maxY)
	{
		LOGError("Main window outside visible area: left=%d maxX=%d top=%d maxY=%d", windowLeft, maxX, windowTop, maxY);
		return;
	}

	// restore the window's width and height
	windowWidth = windowRight - windowLeft;
	windowHeight = windowBottom - windowTop;

	if (windowWidth > maxX)
	{
		windowWidth = SCREEN_WIDTH;
	}

	if (windowHeight > maxY)
	{
		windowHeight = SCREEN_HEIGHT;
	}

	*x = windowLeft;
	*y = windowTop;
	*width = windowWidth;
	*height = windowHeight;

	//LOGD("x=%d y=%d width=%d height=%d", *x, *y, *width, *height);
}

/* note, it seems this below gives virus false positives on windows defender
// Use discrete GPU by default.
extern "C" {
  // http://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
  __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
  
  // http://developer.amd.com/community/blog/2015/10/02/amd-enduro-system-for-developers/
  __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
*/

// fix for broken SDL_ShowCursor
static volatile bool VID_isMouseCursorVisible = true;
void VID_ShowMouseCursor()
{
        ShowCursor(TRUE);
        VID_isMouseCursorVisible = true;
}

void VID_HideMouseCursor()
{
        LOGD("VID_HideMouseCursor");

        // rule #1: don't ever trust microsoft
        //while(ShowCursor(FALSE)>=0);

        for (int i = 0; i < 1000; i++)
        {
                int result = ShowCursor(FALSE);
                LOGD("ShowCursor(FALSE): result=%d", result);
                if (result < 0)
                        break;
        }
        
        VID_isMouseCursorVisible = false;
        LOGD("VID_HideMouseCursor finished");
}

bool VID_IsMouseCursorVisible()
{
	return VID_isMouseCursorVisible;
}

/*
std::string ConvertWideToANSI(const std::wstring& wstr)
{
	int count = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
	std::string str(count, 0);
	WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &str[0], count, NULL, NULL);
	return str;
}

std::wstring ConvertAnsiToWide(const std::string& str)
{
	int count = MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), NULL, 0);
	std::wstring wstr(count, 0);
	MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.length(), &wstr[0], count);
	return wstr;
}

std::string ConvertWideToUtf8(const std::wstring& wstr)
{
	int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
	std::string str(count, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], count, NULL, NULL);
	return str;
}

std::wstring ConvertUtf8ToWide(const std::string& str)
{
	int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), NULL, 0);
	std::wstring wstr(count, 0);
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), &wstr[0], count);
	return wstr;
}
*/

void PLATFORM_SetThreadName(const char *name)
{
/*
	// this is not supported on windows below windows 10, removed for now
	
	WCHAR threadName[256];
	MultiByteToWideChar(0, 0, name, strlen(name), threadName, 256);
	HRESULT hr = SetThreadDescription(GetCurrentThread(), threadName);
	if (FAILED(hr))
	{
		LOGError("PLATFORM_SetThreadName: failed to set name '%s'", name);
	}
*/
}

void PLATFORM_UpdateMenus()
{
}

