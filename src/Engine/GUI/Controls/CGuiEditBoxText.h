/*
 *  CGuiEditBoxText.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_EDITBOX_TEXT_
#define _GUI_EDITBOX_TEXT_

#include "CGuiView.h"
#include "CGuiButton.h"

#define TEXTBOX_TYPE_TEXT	0x00
#define TEXTBOX_TYPE_FLOAT	0x01
#define TEXTBOX_TYPE_INT	0x02

class CGuiEditBoxText;

class CGuiEditBoxTextCallback
{
public:
	virtual void EditBoxTextValueChanged(CGuiEditBoxText *editBox, char *text);
	virtual void EditBoxTextFinished(CGuiEditBoxText *editBox, char *text);
};

class CGuiEditBoxText : public CGuiView
{
public:
	CGuiEditBoxText(float posX, float posY, float posZ, float fontWidth, float fontHeight,
			  char *defaultText, u16 maxNumChars, bool readOnly,
					CGuiEditBoxTextCallback *callback);

	CGuiEditBoxText(float posX, float posY, float posZ,
					float sizeX, float sizeY,
					char *defaultText, u16 maxNumChars,
					CSlrFont *textFont, float fontScale, bool readOnly, CGuiEditBoxTextCallback *callback);

	CGuiEditBoxText(float posX, float posY, float posZ,
					float sizeX,
					char *defaultText, u16 maxNumChars,
					CSlrFont *textFont, float fontScale, bool readOnly, CGuiEditBoxTextCallback *callback);

	void Initialize(char *defaultText, u16 maxNumChars, bool readOnly,
					CGuiEditBoxTextCallback *callback);

	volatile bool enabled;
	bool editable;
	u8 type;

	virtual void Render();
	virtual void Render(float posX, float posY);
	virtual void Render(float posX, float posY, float sizeX, float sizeY);
	volatile float alpha;

	float fontWidth;
	float fontHeight;

	void SetFont(CSlrFont *font, float fontScale);
	CSlrFont *font;
	float fontScale;
	float fontAlpha;

	float textOffsetY;

	float cursorAlpha;
	//float cursorAlphaVal;
	float cursorAlphaSpeed;

	char *textBuffer;
	u16 numChars;
	u16 currentPos;
	u16 maxNumChars;

	bool forceCapitals;

	float gapX, gapY, gapX2, gapY2;
	float cursorGapY;
	float cursorWidth, cursorHeight;

	volatile bool readOnly;
	volatile bool editing;

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float x, float y);
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	virtual void DoLogic();
	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	virtual void FocusReceived();
	virtual void FocusLost();

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats

	virtual void SetText(char *setText);
	virtual char *GetText();

	float colorR;
	float colorG;
	float colorB;
	float colorA;
	float color2R;
	float color2G;
	float color2B;
	float color2A;
	float cursorColorR;
	float cursorColorG;
	float cursorColorB;
	float cursorColorA;

	virtual void SetEnabled(bool setEnabled);
	CGuiEditBoxTextCallback *callback;

};

#endif //_GUI_EDITBOX_TEXT_
