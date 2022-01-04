#include "DBG_Log.h"
#include "SYS_Main.h"

#include <GL/gl3w.h>
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#include "VID_Main.h"
#include "NET_Main.h"
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
#include "SYS_FileSystem.h"
#include "SYS_PauseResume.h"
#include "CSlrString.h"

void SYS_Shutdown();

// THIS IS THE ENTRY POINT, ENJOY :)
void SYS_MTEngineStartup()
{
	SYS_PlatformInit();

	SYS_InitCharBufPool();
	SYS_InitStrings();
	SYS_InitFileSystem();
	SYS_InitApplicationPauseResume();
	
	MT_PreInit();

	SDL_version compiled;
	SDL_version linked;

	SDL_VERSION(&compiled);
	SDL_GetVersion(&linked);
	LOGM("MTEngineSDL compiled SDL %d.%d.%d, linked with %d.%d.%d",
		   compiled.major, compiled.minor, compiled.patch,
		   linked.major, linked.minor, linked.patch);
	
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0)
	{
		LOGError("SDL_Init error: %s", SDL_GetError());
		return;
	}

	// this is the order of startup:
	
	RES_Init(2048);

	VID_Init();

	
	//getInfo();
	
	IM_ASSERT(ImGui::GetCurrentContext() != NULL && "Missing dear imgui context. Refer to examples app!"); // Exceptionally add an extra assert here for people confused with initial dear imgui setup

	SND_Init();
	
	VID_RestoreMainWindowPosition();

	// init the application
	GUI_Init();
	
#if !defined(WIN32)
	NET_Initialize();
#else
	LOGTODO("NET_Initialize(); windows");
#endif
	
	MT_PostInit();
	
	// first rendered frame is here:
	VID_RenderLoop();

	// shutdown
	ImGuiContext& g = *GImGui;
	if (guiMain->IsViewFullScreen() == false && 
		guiMain->layoutManager->currentLayout)
	{
		// store ImGui layout
		ImGui::SaveIniSettingsToDisk(g.IO.IniFilename);

		// serialize current layout to workspaces
		guiMain->layoutManager->currentLayout->serializedLayoutBuffer->Clear();
		
		// note, we can just serialize layout as the frame has been rendered, normally we would need to call async serialize
		guiMain->SerializeLayout(guiMain->layoutManager->currentLayout->serializedLayoutBuffer);
		
		// save all layouts
		guiMain->layoutManager->StoreLayouts();
	}
	
	SYS_PlatformShutdown();
	
	_exit(0);

	// this below takes ages sometimes (~1-2sec) and is not needed on modern systems
	SND_Shutdown();
	
	VID_Shutdown();
	
	SDL_Quit();
	
	LOG_Shutdown();
}

void SYS_Shutdown()
{
	LOGM("SYS_Shutdown");
	VID_StopEventsLoop();
}
