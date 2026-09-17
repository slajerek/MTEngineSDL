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
#include <cstring>
#include <SDL3/SDL.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include "SYS_FileSystem.h"
#include "DBG_Log.h"
#include "SYS_Startup.h"
#include "SYS_CommandLine.h"
#include "MT_VERSION.h"

#include "CGuiViewDebugLog.h"

const char *SYS_ExecSystemCommand(const char *command);

int main(int argc, const char * argv[])
{
	LOG_Init();
	guiViewDebugLog = new CGuiViewDebugLog("Debug Log", 50, 50, -1, 200, 200);
	
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

	// An automated run must never be able to stop on a system dialog.
	//
	// After any abnormal termination -- a crash, or a watchdog SIGKILLing a
	// hung run -- AppKit answers the launch 'oapp' event with
	// -[NSPersistentUIManager promptToIgnorePersistentState], the "reopen
	// windows?" prompt. It blocks the main thread waiting for a click that a
	// test run will never produce, and every subsequent run inherits it. It
	// looks exactly like a hung test and is not one: two runs were
	// misdiagnosed that way on 2026-09-08 before a stack sample showed
	// _handleAEOpenEvent -> _reopenWindowsAsNecessary... -> the prompt.
	//
	// Only for runs started with a test switch. Interactive launches keep the
	// stock behaviour, and applications persist their own layouts anyway.
	{
		bool isAutomatedRun = false;
		for (int i = 1; i < argc; i++)
		{
			const char *a = argv[i];
			if (a == NULL) continue;
			if (strcmp(a, "--run-suite") == 0 || strcmp(a, "--run-test") == 0
				|| strcmp(a, "--run-tests") == 0 || strcmp(a, "--run-imgui-test") == 0
				|| strcmp(a, "--exit-after-tests") == 0 || strcmp(a, "--headless") == 0)
			{
				isAutomatedRun = true;
				break;
			}
		}
		if (isAutomatedRun)
		{
			LOGM("main: automated run, disabling AppKit restorable-state prompt");
			[[NSUserDefaults standardUserDefaults] setBool: YES
													forKey: @"ApplePersistenceIgnoreState"];
		}
	}

	SYS_MTEngineStartup();
	
	return 0;
}

