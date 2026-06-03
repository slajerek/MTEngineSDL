#include "DBG_Log.h"
#include "SYS_Main.h"

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"

#include "VID_Main.h"
#include "NET_Main.h"
#include "SYS_DefaultConfig.h"
#include "RES_ResourceManager.h"
#include "SND_SoundEngine.h"
#include "CLayoutManager.h"

#include "SYS_Platform.h"
#include "SYS_CommandLine.h"
#include "MT_VERSION.h"
#include "MT_API.h"

#include <cstring>


//https://github.com/thennequin/ImWindow/tree/master/ImWindowGLFW

//https://github.com/ocornut/imgui/issues/2109

//https://github.com/ocornut/imgui/wiki

#define IMGUI_IMPL_OPENGL_LOADER_GL3W


#include <iostream>
#include <vector>

#include "GUI_Main.h"
#include "CGuiViewDebugLog.h"
#include "SYS_FileSystem.h"
#include "SYS_PauseResume.h"
#include "CSlrString.h"
#include "GAM_GamePads.h"

void SYS_Shutdown();
extern volatile bool mtQuitApplication;

// THIS IS THE ENTRY POINT, ENJOY :)
void SYS_MTEngineStartup()
{
	SYS_PlatformInit();
	LOGM("Platform: %s Architecture: %s", SYS_GetPlatformNameString(), SYS_GetPlatformArchitectureString());

	SYS_InitCharBufPool();
	SYS_InitStrings();
	SYS_InitFileSystem();
	SYS_InitApplicationDefaultConfig();
	SYS_InitApplicationGuiLogger();
	SYS_InitApplicationPauseResume();
	
	SYS_SetThreadName("Main");

	MT_PreInit();

	// Early headless/service detection: scan argv before SDL_Init so we can
	// skip heavy subsystems for pure services (auth, node-agent, registry, etc.)
	// and set SDL hints for headless mode.
	for (int i = 1; i < SYS_GetArgc(); i++)
	{
		const char *arg = SYS_GetArgv()[i];
		if (strcmp(arg, "--headless") == 0 || strcmp(arg, "--mcp-headless") == 0)
		{
			gHeadlessMode = true;
		}
		else if (strcmp(arg, "--auth-server") == 0
				 || strcmp(arg, "--node-agent") == 0
				 || strcmp(arg, "--registry-server") == 0
				 || strcmp(arg, "--admin-dashboard") == 0
				 || strcmp(arg, "--registry-admin") == 0
				 || strcmp(arg, "--registry-admin-cmd") == 0)
		{
			gServiceMode = true;
			gHeadlessMode = true;
		}
	}

	if (gHeadlessMode)
	{
		LOGM("Headless mode detected (early), setting SDL background app hint");
		SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
	}

	SDL_version compiled;
	SDL_version linked;

	SDL_VERSION(&compiled);
	SDL_GetVersion(&linked);
	LOGM("MTEngineSDL: SDL compiled %d.%d.%d, linked with %d.%d.%d",
		   compiled.major, compiled.minor, compiled.patch,
		   linked.major, linked.minor, linked.patch);
	LOGM("             ImGui version %s (%d)", IMGUI_VERSION, IMGUI_VERSION_NUM);

	if (gServiceMode)
	{
		// Service mode: skip window, OpenGL, audio, gamepads
		// Still init SDL (timer only), resource manager, ImGui context (minimal),
		// and networking — some service code references these globals.
		if (SDL_Init(SDL_INIT_TIMER) != 0)
		{
			LOGError("SDL_Init error: %s", SDL_GetError());
			return;
		}

		RES_Init(2048);

		// Create minimal ImGui context (no window/renderer) — some service
		// code may reference ImGui::GetCurrentContext() or ImGui::GetIO()
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::GetIO().IniFilename = NULL;

		NET_Initialize();

		MT_PostInit();

		// Service mode main loop — MT_PostInit starts the service in a thread,
		// this loop keeps the process alive until SYS_Shutdown() is called
		while (mtQuitApplication == false)
		{
			SYS_Sleep(10);
		}
	}
	else
	{
		// Normal mode: full initialization with rendering, audio, GUI
		if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0)
		{
			LOGError("SDL_Init error: %s", SDL_GetError());
			return;
		}

		RES_Init(2048);

		VID_Init();

		IM_ASSERT(ImGui::GetCurrentContext() != NULL && "Dear ImGui context failed to create");

		SND_Init();

		GUI_Init();

		NET_Initialize();

		GAM_InitGamePads();

		PLATFORM_UpdateMenus();

		MT_PostInit();

		SND_Start();

		VID_PostInit();

		// First rendered frame is here and whole Render Loop
		VID_RenderLoop();
	}

	if (!gServiceMode && !gHeadlessMode)
	{
		// Fix #5b: ensure at least one workspace exists before saving
		if (guiMain->layoutManager->layouts.empty())
		{
			CLayoutData *defaultLayout = new CLayoutData();
			defaultLayout->layoutName = STRALLOC("Default");
			defaultLayout->doNotUpdateViewsPositions = false;
			guiMain->layoutManager->AddLayout(defaultLayout);
			guiMain->layoutManager->currentLayout = defaultLayout;
		}

		// Fix #3: if currentLayout is NULL but layouts exist, pick first one
		if (!guiMain->layoutManager->currentLayout && !guiMain->layoutManager->layouts.empty())
		{
			guiMain->layoutManager->currentLayout = guiMain->layoutManager->layouts.front();
		}

		if (guiMain->layoutManager->currentLayout)
		{
			// Save ImGui ini settings BEFORE MT_Shutdown() so that any test engine
			// settings handlers are still alive when SaveIniSettingsToDisk runs.
			ImGuiContext& g = *GImGui;
			ImGui::SaveIniSettingsToDisk(g.IO.IniFilename);

			// Fix #3: only serialize view state when not in fullscreen
			// (we don't want to capture fullscreen state as the normal layout),
			// but always call StoreLayouts() to persist a valid currentLayoutName
			if (guiMain->IsViewFullScreen() == false &&
				guiMain->layoutManager->currentLayout->doNotUpdateViewsPositions == false)
			{
				// serialize current layout to workspaces
				guiMain->layoutManager->currentLayout->serializedLayoutBuffer->Clear();

				// note, we can just serialize layout now because the frame has been rendered, normally we would need to call async serialize
				guiMain->SerializeLayout(guiMain->layoutManager->currentLayout);
			}

			// always save layouts to persist valid currentLayoutName
			guiMain->layoutManager->StoreLayouts();
		}
	}

	// shutdown
	MT_Shutdown();

	SYS_ApplicationShutdown();
	SYS_PlatformShutdown();
	
#if !defined(MTENGINE_FULL_SHUTDOWN_PROCEDURE)
	LOG_Shutdown();
	_exit(0);
#else

	// this below takes ages sometimes (~1-2sec) and is not needed on modern systems
	SND_Shutdown();
	VID_Shutdown();
	SDL_Quit();
	
	LOG_Shutdown();
	_exit(0);
#endif
	
}

void SYS_Shutdown()
{
	LOGM("SYS_Shutdown");
	if (gServiceMode)
	{
		// Service mode: no VID event loop, just signal the sleep loop to exit
		mtQuitApplication = true;
	}
	else
	{
		VID_StopEventsLoop();
	}
}
