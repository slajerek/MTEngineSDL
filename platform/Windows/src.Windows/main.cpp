#include <SDL3/SDL.h>
// MUST be included in the file that defines main(), and SDL3 no longer pulls it
// in from SDL.h -- see the SDL3 migration guide.
//
// This is not cosmetic on Windows. The project links with SubSystem=Windows, so
// the real entry point is WinMain, and SDL2 supplied it from the separate
// SDL2main.lib. SDL3 HAS NO SDL3main.lib: SDL_main.h is header-only (it pulls
// in SDL_main_impl.h) and provides WinMain inline, then #defines main to
// SDL_main so the function below becomes what SDL calls. Without this include
// the link fails with an unresolved WinMain -- which is the whole reason the
// dropped SDL3 branch needed its own commit for the Windows entry point.
//
// Harmless where it is not needed: SDL_main.h only defines SDL_MAIN_AVAILABLE /
// SDL_MAIN_NEEDED on the platforms that want it (Win32, GDK, iOS, Android,
// Emscripten, PSP/PS2/3DS). macOS and Linux are not among them, so their own
// main files deliberately do not include it.
#include <SDL3/SDL_main.h>
#include "DBG_Log.h"
#include "SYS_FileSystem.h"
#include "SYS_Startup.h"
#include "SYS_CommandLine.h"
#include "SYS_Platform.h"
#include "SYS_MiniDump.h"
#include "MT_VERSION.h"
#if !defined(GLOBAL_DEBUG_OFF)
#include "CGuiViewDebugLog.h"
#endif

int main(int argc, char* argv[])
{
	SYS_InstallCrashHandler();
	LOG_Init();
#if !defined(GLOBAL_DEBUG_OFF)
	guiViewDebugLog = new CGuiViewDebugLog("Debug Log", 50, 50, -1, 200, 200);
#endif
	LOGM("MTEngineSDL v" MT_VERSION_STRING " compiled on " __DATE__ " " __TIME__);

	SYS_SetCommandLineArguments(argc, (const char**) argv);

	SYS_MTEngineStartup();

	return 0;
}


