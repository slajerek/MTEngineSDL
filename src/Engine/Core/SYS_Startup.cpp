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
#include "MT_VERSION.h"
#include "MT_API.h"


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

	SDL_version compiled;
	SDL_version linked;

	SDL_VERSION(&compiled);
	SDL_GetVersion(&linked);
	LOGM("MTEngineSDL: SDL compiled %d.%d.%d, linked with %d.%d.%d",
		   compiled.major, compiled.minor, compiled.patch,
		   linked.major, linked.minor, linked.patch);
	LOGM("             ImGui version %s (%d)", IMGUI_VERSION, IMGUI_VERSION_NUM);
	
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0)
	{
		LOGError("SDL_Init error: %s", SDL_GetError());
		return;
	}
	
	// this is the order of startup:
	
	RES_Init(2048);

	VID_Init();
	
	IM_ASSERT(ImGui::GetCurrentContext() != NULL && "Dear ImGui context failed to create");

	SND_Init();
	
	// init the application gui
	GUI_Init();
	
	// init Enet
	NET_Initialize();
	
	// init gamepads
	GAM_InitGamePads();

	// update default OS menus (e.g. remove items in macOS)
	PLATFORM_UpdateMenus();
	
	MT_PostInit();
	
	// start sound
	SND_Start();

	VID_PostInit();
	
	// First rendered frame is here and whole Render Loop
	VID_RenderLoop();

	// shutdown
//	MT_Shutdown();

	ImGuiContext& g = *GImGui;
	if (guiMain->IsViewFullScreen() == false && 
		guiMain->layoutManager->currentLayout)
	{
		// store ImGui layout
		ImGui::SaveIniSettingsToDisk(g.IO.IniFilename);

		if (guiMain->layoutManager->currentLayout->doNotUpdateViewsPositions == false)
		{
			// serialize current layout to workspaces
			guiMain->layoutManager->currentLayout->serializedLayoutBuffer->Clear();
			
			// note, we can just serialize layout now because the frame has been rendered, normally we would need to call async serialize
			guiMain->SerializeLayout(guiMain->layoutManager->currentLayout);
		}
		
		// save all layouts
		guiMain->layoutManager->StoreLayouts();
	}
	
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
	VID_StopEventsLoop();
}
