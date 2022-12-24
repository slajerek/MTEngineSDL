#include <SDL.h>
#include "DBG_Log.h"
#include "SYS_FileSystem.h"
#include "SYS_Startup.h"
#include "SYS_CommandLine.h"
#include "SYS_Platform.h"
#include "MT_VERSION.h"
#if !defined(GLOBAL_DEBUG_OFF)
#include "CGuiViewDebugLog.h"
#endif

int main(int argc, char* argv[])
{
	LOG_Init();
#if !defined(GLOBAL_DEBUG_OFF)
	guiViewDebugLog = new CGuiViewDebugLog("Debug Log", 50, 50, -1, 200, 200);
#endif
	LOGM("MTEngineSDL v" MT_VERSION_STRING " compiled on " __DATE__ " " __TIME__);

	SYS_SetCommandLineArguments(argc, (const char**) argv);

	SYS_MTEngineStartup();

	return 0;
}


