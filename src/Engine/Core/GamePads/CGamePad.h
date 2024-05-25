#ifndef _CGamePad_h_
#define _CGamePad_h_

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

#endif
