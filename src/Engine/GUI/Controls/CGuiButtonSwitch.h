/*
 *  CGuiButtonSwitch.h
 *  MobiTracker
 *
 *  Created by mars on 4/12/10.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_BUTTON_SWITCH_
#define _GUI_BUTTON_SWITCH_

#include "CGuiButton.h"
#include <list>

class CGuiButtonSwitchCallback;

class CGuiButtonSwitch : public CGuiButton
{
public:
	CGuiButtonSwitch(CSlrImage *imageOn, CSlrImage *imageOff,
			float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment,
			CGuiButtonSwitchCallback *callback);
	CGuiButtonSwitch(char *text,
			float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment,
			CGuiButtonSwitchCallback *callback);
	CGuiButtonSwitch(CSlrImage *bkgImageOn, CSlrImage *bkgImageOff, CSlrImage *bkgImageDisabled,
			   float posX, float posY, float posZ, float sizeX, float sizeY,
			   CSlrString *textUTF,
			   u8 textAlignment, float textOffsetX, float textOffsetY,
			   CSlrFont *font, float fontScale,
			   float textColorOnR, float textColorOnG, float textColorOnB, float textColorOnA,
			   float textColorOffR, float textColorOffG, float textColorOffB, float textColorOffA,
			   float textColorDisabledR, float textColorDisabledG, float textColorDisabledB, float textColorDisabledA,
			   CGuiButtonSwitchCallback *callback);

	virtual void InitBackgroundColors();
	
	float buttonSwitchOnColorR;
	float buttonSwitchOnColorG;
	float buttonSwitchOnColorB;
	float buttonSwitchOnColorA;
	float buttonSwitchOnColor2R;
	float buttonSwitchOnColor2G;
	float buttonSwitchOnColor2B;
	float buttonSwitchOnColor2A;
	
	float buttonSwitchOffColorR;
	float buttonSwitchOffColorG;
	float buttonSwitchOffColorB;
	float buttonSwitchOffColorA;
	float buttonSwitchOffColor2R;
	float buttonSwitchOffColor2G;
	float buttonSwitchOffColor2B;
	float buttonSwitchOffColor2A;

	void InitWithText(char *txt);

	virtual void Render();
	virtual void Render(float posX, float posY);
	virtual void RenderUTFButton(float posX, float posY);

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoFinishDoubleTap(float x, float y);
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	virtual void DoLogic();

	CSlrImage *imageOn;
	CSlrImage *imageOff;

	float textColorOffR;
	float textColorOffG;
	float textColorOffB;
	float textColorOffA;

	CGuiButtonSwitchCallback *switchCallback;

	virtual bool Pressed(float posX, float posY);
	virtual bool DoSwitch();
	virtual void SetOn(bool isOn);
	virtual bool IsOn();

private:
	volatile bool isOn;
};

class CGuiButtonSwitchCallback : public CGuiButtonCallback
{
public:
	virtual bool ButtonSwitchChanged(CGuiButtonSwitch *button);
};


#endif //_GUI_BUTTON_SWITCH_
