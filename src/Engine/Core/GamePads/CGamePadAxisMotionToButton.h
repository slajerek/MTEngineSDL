#ifndef _CGamePadAxisMotionToButton_h_
#define _CGamePadAxisMotionToButton_h_

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
