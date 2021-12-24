#include "SYS_Platform.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include "win-registry.h"
#include "MT_API.h"
#include "VID_Main.h"

void SYS_PlatformInit()
{
}

void SYS_PlatformShutdown()
{
	SYS_DetachWindowsConsole();
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

DWORD windowShowCmd, windowLeft, windowRight, windowTop, windowBottom;
int windowWidth;
int windowHeight;

SDL_Window* imgui_impl_sdl_get_main_window();

void VID_StoreMainWindowPosition()
{
	//LOGD("VID_StoreMainWindowPosition");
	
	SDL_Window* sdlWindow = imgui_impl_sdl_get_main_window();
	
	char* DEFAULT_REG_KEY = SYS_GetCharBuf();
	sprintf(DEFAULT_REG_KEY, "SOFTWARE\\%s", MT_GetSettingsFolderName());

	int x, y, width, height;
	SDL_GetWindowPosition(sdlWindow, &x, &y);
	SDL_GetWindowSize(sdlWindow, &width, &height);

	CreateRegistryKey(HKEY_CURRENT_USER, DEFAULT_REG_KEY);

	int hb = GetSystemMetrics(SM_CYCAPTION);

	width = SDL_GetWindowSurface(sdlWindow)->w;
	height = SDL_GetWindowSurface(sdlWindow)->h + hb;

	WriteDwordInRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "x", x);
	WriteDwordInRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "y", y);
	WriteDwordInRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "width", width);
	WriteDwordInRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "height", height);

	//LOGD("  showCmd=%d left=%d right=%d top=%d bottom=%d", wp.showCmd, wp.rcNormalPosition.left, wp.rcNormalPosition.right, wp.rcNormalPosition.top, wp.rcNormalPosition.bottom);
	
	SYS_ReleaseCharBuf(DEFAULT_REG_KEY);
}

void VID_RestoreMainWindowPosition()
{
}

void VID_GetStartupMainWindowPosition(int* x, int* y, int* width, int* height)
{
	// default values
	MT_GetDefaultWindowPositionAndSize(x, y, width, height);
	SCREEN_WIDTH = *width;
	SCREEN_HEIGHT = *height;

	SDL_Window* sdlWindow = imgui_impl_sdl_get_main_window();

	char* DEFAULT_REG_KEY = SYS_GetCharBuf();
	sprintf(DEFAULT_REG_KEY, "SOFTWARE\\%s", MT_GetSettingsFolderName());

	DWORD widthStored, heightStored;

	if ( (readDwordValueRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "x", &windowLeft) != TRUE)
		|| (readDwordValueRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "y", &windowTop) != TRUE)
		|| (readDwordValueRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "width", &widthStored) != TRUE)
		|| (readDwordValueRegistry(HKEY_CURRENT_USER, DEFAULT_REG_KEY, "height", &heightStored) != TRUE))
	{
		SYS_ReleaseCharBuf(DEFAULT_REG_KEY);
		return;
	}

	windowRight = windowLeft + widthStored;
	windowBottom = windowTop + heightStored;

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
		SYS_ReleaseCharBuf(DEFAULT_REG_KEY);
		return;
	}

	if (windowLeft > maxX
		|| windowTop > maxY)
	{
		LOGError("Main window outside visible area: left=%d maxX=%d top=%d maxY=%d", windowLeft, maxX, windowTop, maxY);
		SYS_ReleaseCharBuf(DEFAULT_REG_KEY);
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
	SYS_ReleaseCharBuf(DEFAULT_REG_KEY);
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
