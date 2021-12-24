/*
 *  CGuiLabel.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-01-07.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_LABEL_
#define _GUI_LABEL_

#include "CGuiElement.h"
#include "CSlrImage.h"
#include "SYS_Main.h"
class CSlrString;
class CSlrFont;

#define LABEL_ALIGNED_CENTER	BV00
#define LABEL_ALIGNED_UP		BV01
#define LABEL_ALIGNED_DOWN		BV02
#define LABEL_ALIGNED_LEFT		BV03
#define LABEL_ALIGNED_RIGHT		BV04

class CGuiLabelCallback;

class CGuiLabel : public CGuiElement
{
public:
	CGuiLabel(CSlrImage *image, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment, CGuiLabelCallback *callback);
	CGuiLabel(char *text, bool stretched, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment, float fontWidth, float fontHeight, CGuiLabelCallback *callback);
	CGuiLabel(char *text, float posX, float posY, float posZ, CSlrFont *font, float fontSize, CGuiLabelCallback *callback);
	CGuiLabel(CSlrString *textUTF, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment, CSlrFont *font, float fontSize, CGuiLabelCallback *callback);
	CGuiLabel(CSlrString *textUTF, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment, CSlrFont *font, float fontSize,
			  float colorR, float colorG, float colorB, float alpha, CGuiLabelCallback *callback);
	CGuiLabel(CSlrString *textUTF, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment, CSlrFont *font, float fontSize,
			  float bkgColorR, float bkgColorG, float bkgColorB, float blgColorA,
			  float textColorR, float textColorG, float textColorB, float textColorA,
			  float textOffsetX, float textOffsetY,
			  CGuiLabelCallback *callback);
	void Render();
	void Render(float posX, float posY);

	bool DoTap(float x, float y);
	bool DoFinishTap(float x, float y);
	bool DoDoubleTap(float x, float y);
	bool DoFinishDoubleTap(float x, float y);
	bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	void DoLogic();

	void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);
	void UpdateTextSize(bool stretched);

	// overwrite this
	virtual bool Clicked(float posX, float posY);
	virtual bool Pressed(float posX, float posY);

	void InitWithText(char *txt, bool stretched);
	void SetText(char *text, bool stretched);

	void InitWithText(CSlrString *textUTF);
	void SetText(CSlrString *textUTF);

	char *text;
	float fontWidth;
	float fontHeight;

	CSlrString *textUTF;
	CSlrFont *font;
	float fontSize;
	void SetFontSize(float fs);
	
	CSlrImage *image;

	float textPosX;
	float textPosY;

	bool clickConsumed;
	bool beingClicked;

	//bool zoomable;

	u8 alignment;

	bool transparentToTaps;

	float bkgColorR;
	float bkgColorG;
	float bkgColorB;
	float bkgColorA;

	float textColorR;
	float textColorG;
	float textColorB;
	float textColorA;

	CGuiLabelCallback *callback;
};

class CGuiLabelCallback
{
public:
	virtual bool LabelClicked(CGuiLabel *label);
	virtual bool LabelPressed(CGuiLabel *label);
};



#endif
//_GUI_BUTTON_

