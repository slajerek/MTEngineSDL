#include "SYS_CommandLine.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"

int sysArgc;
const char **sysArgv;

std::vector<const char *> sysCommandLineArguments;

// Warning: any console output here slows down startup by one second

void SYS_SetCommandLineArguments(int argc, const char **argv)
{
	LOGD("SYS_SetCommandLineArguments: argc=%d argv=%x", argc, argv);
	sysArgc = argc;
	sysArgv = argv;
	
	for (int i = 1; i < argc; i++)
	{
		LOGD("sysArgv[%d]=%s", sysArgv[i]);
		sysCommandLineArguments.push_back(argv[i]);
	}
}

int SYS_GetArgc()
{
	return sysArgc;
}

const char **SYS_GetArgv()
{
	return sysArgv;
}
