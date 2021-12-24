#ifndef _GAM_GamePads_H_
#define _GAM_GamePads_H_

#include <SDL.h>

#define MAX_GAMEPADS 4

class CSlrFile;
class CGuiView;
class CGamePad;

void GAM_InitGamePads();
void GAM_RefreshGamePads();
void GAM_GamePadsEvent(const SDL_Event& event);
CGamePad **GAM_EnumerateGamepads(int *numGamepads);

class CGamePad
{
public:
	CGamePad(int index);
	~CGamePad();
	
	bool isActive;
	SDL_GameController *sdlGamePad;
	SDL_Haptic *sdlGamePadHaptic;
	SDL_JoystickID sdlJoystickId;
	int index;
	char *name;
	char guid[33];
	char *mapping;
	int deadZoneMargin;
		
	virtual void Open(int deviceId);
	virtual void Close();
	virtual void ClearButtonsState();
	virtual bool GamePadAxisMotionToButtonEvent(u8 axis, int value);

private:
	bool axisToButtonState[SDL_CONTROLLER_BUTTON_MAX];
};

class CGamePadAxisMotionToButton
{
public:
	CGamePadAxisMotionToButton(CGuiView *view);
	CGuiView *view;
	
	int deadZoneMargin;
	
	// fire button event based on axis event
	virtual bool GamePadAxisMotionToButtonEvent(CGamePad *gamePad, u8 axis, int value);
};

#endif

