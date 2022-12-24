//
//  main.mm
//  MTEngineSDL
//
//  Created by Marcin Skoczylas on 26/11/2019.
//  Copyright © 2019 Marcin Skoczylas. All rights reserved.
//

#import <Foundation/Foundation.h>

#include <iostream>
#include <string>
#include <SDL.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include "SYS_FileSystem.h"
#include "DBG_Log.h"
#include "SYS_Startup.h"
#include "SYS_CommandLine.h"
#include "MT_VERSION.h"

#if !defined(GLOBAL_DEBUG_OFF)
#include "CGuiViewDebugLog.h"
#endif

const char *SYS_ExecSystemCommand(const char *command);

int main(int argc, const char * argv[])
{
	LOG_Init();
#if !defined(GLOBAL_DEBUG_OFF)
	guiViewDebugLog = new CGuiViewDebugLog("Debug Log", 50, 50, -1, 200, 200);
#endif
	
	LOGM("MTEngineSDL v" MT_VERSION_STRING " compiled on " __DATE__ " " __TIME__);
	
	{
		size_t size;
		sysctlbyname("hw.machine", NULL, &size, NULL, 0);
		char *machine = (char *)malloc(size);
		sysctlbyname("hw.machine", machine, &size, NULL, 0);
		NSString *platform = [NSString stringWithCString:machine encoding:NSASCIIStringEncoding];
		free(machine);
		
		LOGM("Machine string=%s", [platform UTF8String]);
	}
	
	SYS_SetCommandLineArguments(argc, argv);

	// allow intertia scrolling
	[[NSUserDefaults standardUserDefaults] setBool: YES
											forKey: @"AppleMomentumScrollSupported"];
	
	SYS_MTEngineStartup();
	
	return 0;
}

