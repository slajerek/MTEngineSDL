#include "CSlrFileZlib.h"
#include "RES_ResourceManager.h"
#include "SYS_FileSystem.h"
#include "GAM_GamePads.h"
#include "SYS_Threading.h"
#include "CSlrString.h"
#include "CGuiMain.h"
#include "CGuiView.h"
#include "gamecontrollerdb_txt_zlib.h"

// https://cpp.hotexamples.com/examples/-/-/SDL_GameControllerAddMappingsFromFile/cpp-sdl_gamecontrolleraddmappingsfromfile-function-examples.html
// https://gist.github.com/urkle/6701236

CGamePad *mtGamePads[MAX_GAMEPADS];
CSlrMutex *mutexGamePads;

void GAM_InitGamePads()
{
	LOGM("GAM_InitGamepads");
	
	// controllers data
	RES_AddEmbeddedDataToDeploy("/gamecontrollerdb", DEPLOY_FILE_TYPE_DATA, gamecontrollerdb_txt_zlib, gamecontrollerdb_txt_zlib_length);

	mutexGamePads = new CSlrMutex("mutexGamePads");
	for (int i = 0; i < MAX_GAMEPADS; i++)
	{
		mtGamePads[i] = new CGamePad(i);
	}
	
	CSlrFileZlib *file = RES_GetFileZlib("/gamecontrollerdb");
	int len = file->GetFileSize();
	u8 *mappingTextData = new u8[len];
	file->Read(mappingTextData, len);
	delete file;
	
//	LOGD("mappingText=%s", mappingTextData);
	
	SDL_RWops *sdlStream = SDL_RWFromConstMem(mappingTextData, len);
	int numMappings = SDL_GameControllerAddMappingsFromRW(sdlStream, 1);
	if (numMappings > 0)
	{
		LOGM("Added %d default gamepad mappings", numMappings);
	}
	
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "%s%cgamecontrollerdb.txt", gCPathToCurrentDirectory, SYS_FILE_SYSTEM_PATH_SEPARATOR);
	numMappings = SDL_GameControllerAddMappingsFromFile(buf);
	if (numMappings > 0)
	{
		LOGM("Loaded %d gamepad mappings from %s", numMappings, buf);
	}
	SYS_ReleaseCharBuf(buf);
	
	GAM_RefreshGamePads();
}

void GAM_RefreshGamePads()
{
	LOGD("GAM_RefreshGamePads");
	for (int i = 0; i < MAX_GAMEPADS; i++)
	{
		if (SDL_IsGameController(i))
		{
			mtGamePads[i]->Open(i);
		}
		else
		{
			mtGamePads[i]->Close();
		}
	}
}

CGamePad **GAM_EnumerateGamepads(int *numGamepads)
{
	*numGamepads = MAX_GAMEPADS;
	return mtGamePads;
}

CGamePad *GAM_GetGamePadFromJoystickId(SDL_JoystickID joystickId)
{
	for (int i = 0; i < MAX_GAMEPADS; i++)
	{
		if (mtGamePads[i]->isActive && mtGamePads[i]->sdlJoystickId == joystickId)
		{
			return mtGamePads[i];
		}
	}
	
	return NULL;
}

void GAM_GamePadsEvent(const SDL_Event& event)
{
//	LOGD("GAM_GamepadsEvent: event=%d", event.type);
	switch(event.type)
	{
		// event.cdevice.which:  The joystick device index for the ADDED event, instance id for the REMOVED or REMAPPED even
		case SDL_CONTROLLERDEVICEADDED:
		{
			if (event.cdevice.which < MAX_GAMEPADS)
			{
				CGamePad *gamePad = mtGamePads[event.cdevice.which];
				gamePad->Open(event.cdevice.which);
			}
			break;
		}
		case SDL_CONTROLLERDEVICEREMOVED:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.cdevice.which);
			if (gamePad != NULL)
			{
				gamePad->Close();
			}
			break;
		}
		
		case SDL_CONTROLLERAXISMOTION:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.caxis.which);
			if (gamePad != NULL)
			{
//				LOGD("SDL_CONTROLLERAXISMOTION: axis=%d value=%d", event.caxis.axis, event.caxis.value);
				guiMain->DoGamePadAxisMotion(gamePad, event.caxis.axis, event.caxis.value);
			}
			break;
		}
		
		case SDL_CONTROLLERBUTTONDOWN:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.cbutton.which);
			if (gamePad != NULL)
			{
				LOGD("SDL_CONTROLLERBUTTONDOWN: button=%d value=%d", event.cbutton.button, event.cbutton.state);
				guiMain->DoGamePadButtonDown(gamePad, event.cbutton.button);
			}
			break;
		}
			
		case SDL_CONTROLLERBUTTONUP:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.cbutton.which);
			if (gamePad != NULL)
			{
				LOGD("SDL_CONTROLLERBUTTONUP: button=%d value=%d", event.cbutton.button, event.cbutton.state);
				guiMain->DoGamePadButtonUp(gamePad, event.cbutton.button);
			}
			break;
		}
	}
}

CGamePad::CGamePad(int index)
{
	this->index = index;
	isActive = false;
	sdlGamePad = NULL;
	sdlGamePadHaptic = NULL;
	sdlJoystickId = -1;
	name = NULL;
	guid[0] = 0x00;
	mapping = NULL;
	deadZoneMargin = 8000;

	ClearButtonsState();
}

void CGamePad::ClearButtonsState()
{
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
	{
		axisToButtonState[i] = false;
	}
}

void CGamePad::Open(int deviceId)
{
	LOGD("CGamePad::Open: deviceId=%d", deviceId);
	sdlGamePad = SDL_GameControllerOpen(deviceId);
	
	const char *joyName = SDL_JoystickNameForIndex(deviceId);
	if (joyName == NULL)
	{
		LOGError("CGamePad::Open: %s", SDL_GetError());
		joyName = "";
	}
	
	this->name = STRALLOC(joyName);
	LOGD("...name=%s", name);

	SDL_Joystick *j = SDL_GameControllerGetJoystick(sdlGamePad);
	sdlJoystickId = SDL_JoystickInstanceID(j);
	LOGD("...joystickId=%d", sdlJoystickId);
	
	SDL_JoystickGUID guidValue = SDL_JoystickGetGUID(j);
	SDL_JoystickGetGUIDString(guidValue, this->guid, 33);
	LOGD("...GUID=%s", this->guid);

	this->mapping = STRALLOC(SDL_GameControllerMapping(sdlGamePad));
	LOGD("...mapping=%s", this->mapping);
	
	isActive = true;
	if (SDL_JoystickIsHaptic(j))
	{
		LOGD("SDL_JoystickIsHaptic");
		
		sdlGamePadHaptic = SDL_HapticOpenFromJoystick(j);
		LOGD("Haptic Effects: %d", SDL_HapticNumEffects(sdlGamePadHaptic));
		LOGD("Haptic Query: %x", SDL_HapticQuery(sdlGamePadHaptic));
		if (SDL_HapticRumbleSupported(sdlGamePadHaptic))
		{
			if (SDL_HapticRumbleInit(sdlGamePadHaptic) != 0)
			{
				LOGError("Haptic Rumble Init: %s", SDL_GetError());
				SDL_HapticClose(sdlGamePadHaptic);
				sdlGamePadHaptic = NULL;
			}
		}
		else
		{
			SDL_HapticClose(sdlGamePadHaptic);
			sdlGamePadHaptic = NULL;
		}
	}
	
	ClearButtonsState();

	LOGD("CGamePad::Open: done");
}

void CGamePad::Close()
{
	LOGD("CGamePad::Close: joystickId=%d", sdlJoystickId);
	if (isActive)
	{
		isActive = false;
		if (sdlGamePadHaptic)
		{
			SDL_HapticClose(sdlGamePadHaptic);
			sdlGamePadHaptic = NULL;
		}
		SDL_GameControllerClose(sdlGamePad);
	}
	
	if (this->name != NULL)
	{
		STRFREE(this->name);
	}
	
	if (this->mapping != NULL)
	{
		STRFREE(this->mapping);
	}
	
	ClearButtonsState();
}

// fire button event based on axis event
bool CGamePad::GamePadAxisMotionToButtonEvent(u8 axis, int value)
{
//	LOGD("CGamePad::GamePadAxisMotionToButtonEvent: axis=%d value=%d", axis, value);
	if (abs(value) < this->deadZoneMargin)
	{
		if (axis == SDL_CONTROLLER_AXIS_LEFTY)
		{
			if (axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_UP] == true)
			{
				axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_UP] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_CONTROLLER_BUTTON_DPAD_UP);
			}
			
			if (axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_DOWN] == true)
			{
				axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
			}
		}
		else if (axis == SDL_CONTROLLER_AXIS_LEFTX)
		{
			if (axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_LEFT] == true)
			{
				axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
			}
			
			if (axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] == true)
			{
				axisToButtonState[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
			}
		}
		
		return false;
	}
	
	int button = SDL_CONTROLLER_BUTTON_INVALID;
	if (axis == SDL_CONTROLLER_AXIS_LEFTY)
	{
		if (value < 0)
		{
			button = SDL_CONTROLLER_BUTTON_DPAD_UP;
		}
		else
		{
			button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
		}
	}
	else if (axis == SDL_CONTROLLER_AXIS_LEFTX)
	{
		if (value < 0)
		{
			button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
		}
		else
		{
			button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
		}
	}
	
	if (button != SDL_CONTROLLER_BUTTON_INVALID)
	{
		if (axisToButtonState[button] == false)
		{
			axisToButtonState[button] = true;
			
			LOGD("DoGamePadAxisMotionButtonDown: button=%d", button);
			
			return guiMain->DoGamePadAxisMotionButtonDown(this, button);
		}
	}
	
	return false;
}

CGamePad::~CGamePad()
{
	if (isActive)
	{
		Close();
	}
}
