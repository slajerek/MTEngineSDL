//
//  main.cpp
//  MTEngineSDL
//
//  Created by Marcin Skoczylas on 26/11/2019.
//  Copyright © 2019 Marcin Skoczylas. All rights reserved.
//

#include "DBG_Log.h"
#include "SYS_Startup.h"
#include "SYS_CommandLine.h"
#include "MT_VERSION.h"
#include "CGuiViewDebugLog.h"

int main(int argc, const char * argv[])
{
	LOG_Init();
	guiViewDebugLog = new CGuiViewDebugLog("Debug Log", 50, 50, -1, 200, 200);

	LOGM("MTEngineSDL v" MT_VERSION_STRING " compiled on " __DATE__ " " __TIME__);
	
	SYS_SetCommandLineArguments(argc, argv);

	SYS_MTEngineStartup();
	
	return 0;
}

