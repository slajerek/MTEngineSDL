/*
 *  CGuiEditBoxHex.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_EDITBOX_HEX_
#define _GUI_EDITBOX_HEX_
#include "CGuiView.h"
#include "CGuiButton.h"

class CGuiEditBoxHex;

class CGuiEditBoxHexCallback
{
public:
	virtual void EditBoxValueChanged(CGuiEditBoxHex *editBox, u8 value);
	virtual void EditBoxValueSet(CGuiEditBoxHex *editBox, u8 value);
};

class CGuiEditBoxHex : public CGuiView, CGuiButtonCallback
{
public:
	CGuiEditBoxHex(float posX, float posY, float posZ, float fontWidth, float fontHeight,
			  int value, u8 numDigits, bool showHexValues, bool readOnly, CGuiEditBoxHexCallback *callback);

	bool editable;

	void Render();
	volatile float alpha;

	float fontWidth;
	float fontHeight;

	float cursorAlpha;
	float cursorAlphaSpeed;

	volatile u32 value;
	u8 numDigits;
	u8 itemVal;
	u8 minVal;
	u8 maxVal;

	bool showHexValues;
	volatile bool readOnly;
	volatile bool editing;
	volatile u8 editingDigitNum;

	bool DoTap(float x, float y);
	bool DoFinishTap(float x, float y);
	bool DoDoubleTap(float x, float y);
	bool DoFinishDoubleTap(float x, float y);
	bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	void DoLogic();
	bool InitZoom();
	bool DoZoomBy(float x, float y, float zoomValue, float difference);
	bool ButtonClicked(CGuiButton *button);
	bool ButtonPressed(CGuiButton *button);

	void DrawDigits(float posX, float posY, float fontWidth);

	char *drawBuf;
	CGuiEditBoxHexCallback *callback;

	CGuiButton *btn0;
	CGuiButton *btn1;
	CGuiButton *btn2;
	CGuiButton *btn3;
	CGuiButton *btn4;
	CGuiButton *btn5;
	CGuiButton *btn6;
	CGuiButton *btn7;
	CGuiButton *btn8;
	CGuiButton *btn9;
	CGuiButton *btnA;
	CGuiButton *btnB;
	CGuiButton *btnC;
	CGuiButton *btnD;
	CGuiButton *btnE;
	CGuiButton *btnF;
	CGuiButton *btnDone;

	void StartEditing();
};

#endif
//_GUI_EDITBOX_HEX_

