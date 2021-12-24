#include <SDL.h>
#include "DBG_Log.h"
#include "SYS_FileSystem.h"
#include "SYS_Startup.h"
#include "SYS_CommandLine.h"
#include "SYS_Platform.h"
#include "MT_VERSION.h"

/*
To add files to project:

Open cmd prompt as administrator
mklink /d [current project directory name] [directory in other project it should point to]
This has it's drawbacks and pitfalls, but I use it on occasion for duplicate libraries that need different names.

Steps to add to Visual Studio:

Create link in the project folder using the steps above.
In Visual Studio... select project in Solution Explorer.
At the top of Solution Explorer... click the Show All Files button (may need to click it twice if already active).
The link will now show in your project... right-click and choose Include In Project.
*/

int main(int argc, char* argv[])
{
	LOG_Init();
	LOGM("MTEngineSDL v" MT_VERSION_STRING " compiled on " __DATE__ " " __TIME__);

	SYS_SetCommandLineArguments(argc, (const char**) argv);

	SYS_MTEngineStartup();

	return 0;
}


