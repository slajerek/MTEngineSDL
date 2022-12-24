/*
 *  CGuiButton.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-26.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_BUTTON_
#define _GUI_BUTTON_

#include "CGuiElement.h"
#include "CSlrImage.h"
#include "SYS_Main.h"
#include "CSlrFont.h"

#define BUTTON_ALIGNED_CENTER	BV00
#define BUTTON_ALIGNED_UP		BV01
#define BUTTON_ALIGNED_DOWN		BV02
#define BUTTON_ALIGNED_LEFT		BV03
#define BUTTON_ALIGNED_RIGHT	BV04

#define DEFAULT_BUTTON_GAPX 5.0f
#define DEFAULT_BUTTON_GAPY 5.0f

#define DEFAULT_BUTTON_SIZE 1.0f
#define DEFAULT_BUTTON_ZOOM 1.5f

//#define DEFAULT_BUTTON_ZOOM_SPEED 6.3f
#define DEFAULT_BUTTON_ZOOM_SPEED ( (60.0f/(float)FRAMES_PER_SECOND) * 3.3f )

class CGuiButtonCallback;

class CGuiButton : public CGuiElement
{
 public:
	CGuiButton(CSlrImage *image, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment, CGuiButtonCallback *callback);
	CGuiButton(CSlrImage *bkgImage, CSlrImage *bkgImageDisabled,
			   float posX, float posY, float posZ, float sizeX, float sizeY,
			   CSlrString *textUTF,
			   u8 textAlignment, float textOffsetX, float textOffsetY,
			   CSlrFont *font, float fontScale,
			   float textColorR, float textColorG, float textColorB, float textColorA,
			   float textColorDisabledR, float textColorDisabledG, float textColorDisabledB, float textColorDisabledA,
			   CGuiButtonCallback *callback);

	CGuiButton(const char *text, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment, CGuiButtonCallback *callback);
	virtual void Render();
	virtual void Render(float posX, float posY);
	virtual void RenderText(float posX, float posY);
	virtual void RenderUTFButton(float posX, float posY);

	bool DoTap(float x, float y);
	bool DoFinishTap(float x, float y);
	bool DoDoubleTap(float x, float y);
	bool DoFinishDoubleTap(float x, float y);
	bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	void DoLogic();

	// overwrite this
	virtual bool Clicked(float posX, float posY);
	virtual bool Pressed(float posX, float posY);

	virtual void ReleaseClick();
	
	float buttonShadeAmount;
	float buttonShadeDistance;
	float buttonShadeDistance2;
	
	float buttonEnabledColorR;
	float buttonEnabledColorG;
	float buttonEnabledColorB;
	float buttonEnabledColorA;
	float buttonEnabledColor2R;
	float buttonEnabledColor2G;
	float buttonEnabledColor2B;
	float buttonEnabledColor2A;
	
	float buttonDisabledColorR;
	float buttonDisabledColorG;
	float buttonDisabledColorB;
	float buttonDisabledColorA;
	float buttonDisabledColor2R;
	float buttonDisabledColor2G;
	float buttonDisabledColor2B;
	float buttonDisabledColor2A;
	
	float buttonPressedColorR;
	float buttonPressedColorG;
	float buttonPressedColorB;
	float buttonPressedColorA;

	virtual void InitBackgroundColors();
	
	virtual void InitWithText(char *txt);
	void SetText(const char *text);
	char *text2;
	float fontWidth;
	float fontHeight;

	void SetFont(CSlrFont *font, float fontScale);
	CSlrFont *font;
	float fontScale;
	
	int textAlignment;
	
	float textColorR;
	float textColorG;
	float textColorB;
	float textColorA;

	float textColorDisabledR;
	float textColorDisabledG;
	float textColorDisabledB;
	float textColorDisabledA;

	void RecalcTextPosition();
	float textDrawPosX;
	float textDrawPosY;
	
	void SetFontScale(float fontScale);

	CSlrImage *image;
	CSlrImage *imageDisabled;
	CSlrImage *imageExpanded;
	
	CSlrImage *bkgImage;
	CSlrString *textUTF;
	void SetText(CSlrString *textUTF);
	
	void SetImage(CSlrImage *image);
	
	float buttonPosX;
	float buttonPosY;
	float buttonSizeX;
	float buttonSizeY;

	float buttonZoom;

	volatile bool clickConsumed;
	bool beingClicked;
	volatile bool pressConsumed;

	volatile bool enabled;
	void SetEnabled(bool enabled);

	bool zoomable;
	bool zoomingLocked;
	float zoomSpeed;
	bool IsZoomingOut();
	bool IsZoomed();

	u8 alignment;

	CGuiButtonCallback *callback;

	volatile bool wasExpanded;
	volatile bool isExpanded;
	bool DoExpandZoom();

	bool imageFlippedX;
	
	bool centerText;
	float textOffsetY;
	virtual void RenderFocusBorder();

	virtual void UpdateTheme();
};

class CGuiButtonCallback
{
public:
	virtual bool ButtonClicked(CGuiButton *button);
	virtual bool ButtonPressed(CGuiButton *button);

	// TODO: move callback from CGuiButtonMenu to CGuiButton to have action when expanded (not only menu show)
	// for CGuiButtonMenu
	virtual void ButtonExpandedChanged(CGuiButton *button);
};

#endif //_GUI_BUTTON_
