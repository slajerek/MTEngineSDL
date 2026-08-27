#include "CSlrFileZlib.h"
#include "RES_ResourceManager.h"
#include "SYS_FileSystem.h"
#include "GAM_GamePads.h"
#include "SYS_Threading.h"
#include "CSlrString.h"
#include "CGuiMain.h"
#include "CGuiView.h"
#include "gamecontrollerdb_txt_zlib.h"

// https://cpp.hotexamples.com/examples/-/-/SDL_AddGamepadMappingsFromFile/cpp-sdl_gamecontrolleraddmappingsfromfile-function-examples.html
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
	
	SDL_IOStream *sdlStream = SDL_IOFromConstMem(mappingTextData, len);
	int numMappings = SDL_AddGamepadMappingsFromIO(sdlStream, 1);
	if (numMappings > 0)
	{
		LOGM("Added %d default gamepad mappings", numMappings);
	}
	
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "%s%cgamecontrollerdb.txt", gCPathToCurrentDirectory, SYS_FILE_SYSTEM_PATH_SEPARATOR);
	numMappings = SDL_AddGamepadMappingsFromFile(buf);
	if (numMappings > 0)
	{
		LOGM("Loaded %d gamepad mappings from %s", numMappings, buf);
	}
	SYS_ReleaseCharBuf(buf);
	
	GAM_RefreshGamePads();
}

// SDL3 REPLACED DEVICE INDICES WITH INSTANCE IDs, and this function is where
// that bites hardest. SDL2 let you walk 0..SDL_NumJoysticks() and treat the
// index as the device; SDL3 has no SDL_NumJoysticks at all -- SDL_GetGamepads()
// returns an array of SDL_JoystickID that the caller must SDL_free.
//
// The difference is not cosmetic: an index silently meant a DIFFERENT device
// the moment somebody unplugged a controller ahead of it in the list, whereas
// an instance ID stays bound to the same physical device for its lifetime. Our
// mtGamePads[] slots stay index-based (they are our own array), but what goes
// INTO a slot is now an ID.
void GAM_RefreshGamePads()
{
	LOGD("GAM_RefreshGamePads");

	int numGamepads = 0;
	SDL_JoystickID *gamepadIds = SDL_GetGamepads(&numGamepads);

	for (int i = 0; i < MAX_GAMEPADS; i++)
	{
		if (gamepadIds != NULL && i < numGamepads)
		{
			mtGamePads[i]->Open(gamepadIds[i]);
		}
		else
		{
			mtGamePads[i]->Close();
		}
	}

	if (gamepadIds != NULL)
		SDL_free(gamepadIds);
}

// Finds the slot holding a given instance ID, or the first free slot. Needed
// because SDL3's ADDED event carries an instance ID, not the array position
// SDL2's device index effectively gave us.
static int GAM_FindSlotForJoystickId(SDL_JoystickID joystickId)
{
	for (int i = 0; i < MAX_GAMEPADS; i++)
	{
		if (mtGamePads[i]->isActive && mtGamePads[i]->sdlJoystickId == joystickId)
			return i;
	}
	for (int i = 0; i < MAX_GAMEPADS; i++)
	{
		if (!mtGamePads[i]->isActive)
			return i;
	}
	return -1;
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
		// SDL3 renamed the event union members too: cdevice/caxis/cbutton (the
		// "controller" spelling) became gdevice/gaxis/gbutton ("gamepad"), and
		// `which` is now ALWAYS an instance ID -- in SDL2 it was a device INDEX
		// for ADDED and an instance ID for REMOVED/REMAPPED, which is exactly
		// the kind of inconsistency SDL3 set out to remove.
		case SDL_EVENT_GAMEPAD_ADDED:
		{
			int slot = GAM_FindSlotForJoystickId(event.gdevice.which);
			if (slot >= 0)
			{
				mtGamePads[slot]->Open(event.gdevice.which);
			}
			else
			{
				LOGWarning("SDL_EVENT_GAMEPAD_ADDED: no free gamepad slot for joystickId=%d", (int)event.gdevice.which);
			}
			break;
		}
		case SDL_EVENT_GAMEPAD_REMOVED:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.gdevice.which);
			if (gamePad != NULL)
			{
				gamePad->Close();
			}
			break;
		}
		
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.gaxis.which);
			if (gamePad != NULL)
			{
//				LOGD("SDL_EVENT_GAMEPAD_AXIS_MOTION: axis=%d value=%d", event.gaxis.axis, event.gaxis.value);
				guiMain->DoGamePadAxisMotion(gamePad, event.gaxis.axis, event.gaxis.value);
			}
			break;
		}
		
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.gbutton.which);
			if (gamePad != NULL)
			{
				LOGD("SDL_EVENT_GAMEPAD_BUTTON_DOWN: button=%d down=%d", event.gbutton.button, event.gbutton.down);
				guiMain->DoGamePadButtonDown(gamePad, event.gbutton.button);
			}
			break;
		}
			
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
		{
			CGamePad *gamePad = GAM_GetGamePadFromJoystickId(event.gbutton.which);
			if (gamePad != NULL)
			{
				LOGD("SDL_EVENT_GAMEPAD_BUTTON_UP: button=%d down=%d", event.gbutton.button, event.gbutton.down);
				guiMain->DoGamePadButtonUp(gamePad, event.gbutton.button);
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
	for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
	{
		axisToButtonState[i] = false;
	}
}

// deviceId is an SDL3 INSTANCE ID now, not a device index. The parameter kept
// its name and type (int) so no caller outside this file changes, but what it
// means changed completely -- see GAM_RefreshGamePads.
void CGamePad::Open(int deviceId)
{
	LOGD("CGamePad::Open: joystickId=%d", deviceId);
	sdlGamePad = SDL_OpenGamepad((SDL_JoystickID)deviceId);
	if (sdlGamePad == NULL)
	{
		LOGError("CGamePad::Open: SDL_OpenGamepad failed: %s", SDL_GetError());
		return;
	}
	
	const char *joyName = SDL_GetJoystickNameForID((SDL_JoystickID)deviceId);
	if (joyName == NULL)
	{
		LOGError("CGamePad::Open: %s", SDL_GetError());
		joyName = "";
	}
	
	this->name = STRALLOC(joyName);
	LOGD("...name=%s", name);

	SDL_Joystick *j = SDL_GetGamepadJoystick(sdlGamePad);
	sdlJoystickId = SDL_GetJoystickID(j);
	LOGD("...joystickId=%d", sdlJoystickId);
	
	SDL_GUID guidValue = SDL_GetJoystickGUID(j);
	// SDL3: SDL_JoystickGetGUIDString -> SDL_GUIDToString (the GUID type is no
	// longer joystick-specific, so neither is its formatter).
	SDL_GUIDToString(guidValue, this->guid, 33);
	LOGD("...GUID=%s", this->guid);

	// SDL3: SDL_GetGamepadMapping returns an SDL-ALLOCATED string that the
	// caller must SDL_free -- SDL2's returned a caller-owned buffer too, but the
	// old one-liner leaked it either way by copying and never freeing. It can
	// also return NULL (a gamepad with no mapping, or an error), which STRALLOC
	// would dereference.
	char *sdlMapping = SDL_GetGamepadMapping(sdlGamePad);
	if (sdlMapping != NULL)
	{
		this->mapping = STRALLOC(sdlMapping);
		SDL_free(sdlMapping);
	}
	else
	{
		LOGWarning("CGamePad::Open: no gamepad mapping: %s", SDL_GetError());
		this->mapping = STRALLOC("");
	}
	LOGD("...mapping=%s", this->mapping);
	
	isActive = true;
	if (SDL_IsJoystickHaptic(j))
	{
		LOGD("SDL_IsJoystickHaptic");
		
		sdlGamePadHaptic = SDL_OpenHapticFromJoystick(j);
		LOGD("Haptic Effects: %d", SDL_GetMaxHapticEffects(sdlGamePadHaptic));
		LOGD("Haptic Query: %x", SDL_GetHapticFeatures(sdlGamePadHaptic));
		if (SDL_HapticRumbleSupported(sdlGamePadHaptic))
		{
			// SDL3: returns bool. `!= 0` compiles and is true on SUCCESS, so
			// this would have closed the haptic device every time rumble
			// initialised CORRECTLY -- silently disabling rumble on every
			// controller that supports it.
			if (!SDL_InitHapticRumble(sdlGamePadHaptic))
			{
				LOGError("Haptic Rumble Init: %s", SDL_GetError());
				SDL_CloseHaptic(sdlGamePadHaptic);
				sdlGamePadHaptic = NULL;
			}
		}
		else
		{
			SDL_CloseHaptic(sdlGamePadHaptic);
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
			SDL_CloseHaptic(sdlGamePadHaptic);
			sdlGamePadHaptic = NULL;
		}
		SDL_CloseGamepad(sdlGamePad);
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
		if (axis == SDL_GAMEPAD_AXIS_LEFTY)
		{
			if (axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_UP] == true)
			{
				axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_UP] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_GAMEPAD_BUTTON_DPAD_UP);
			}
			
			if (axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_DOWN] == true)
			{
				axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
			}
		}
		else if (axis == SDL_GAMEPAD_AXIS_LEFTX)
		{
			if (axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_LEFT] == true)
			{
				axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
			}
			
			if (axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] == true)
			{
				axisToButtonState[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = false;
				return guiMain->DoGamePadAxisMotionButtonUp(this, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
			}
		}
		
		return false;
	}
	
	int button = SDL_GAMEPAD_BUTTON_INVALID;
	if (axis == SDL_GAMEPAD_AXIS_LEFTY)
	{
		if (value < 0)
		{
			button = SDL_GAMEPAD_BUTTON_DPAD_UP;
		}
		else
		{
			button = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
		}
	}
	else if (axis == SDL_GAMEPAD_AXIS_LEFTX)
	{
		if (value < 0)
		{
			button = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
		}
		else
		{
			button = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
		}
	}
	
	if (button != SDL_GAMEPAD_BUTTON_INVALID)
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
