/*
 *  CGuiViewMessageBox.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-09-03.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_VIEW_MSGBOX_
#define _GUI_VIEW_MSGBOX_

#include "CGuiView.h"
#include "CGuiButton.h"

class CGuiMessageBoxCallback;

class CGuiViewMessageBox : public CGuiView, CGuiButtonCallback
{
public:
	CGuiViewMessageBox(float posX, float posY, float posZ, float sizeX, float sizeY, CSlrString *message, CGuiMessageBoxCallback *callback);
	~CGuiViewMessageBox();

	void SetText(CSlrString *message);

	virtual void Render();
	virtual void Render(float posX, float posY);
	//virtual void Render(float posX, float posY, float sizeX, float sizeY);
	virtual void DoLogic();

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);

	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float posX, float posY);

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);

	virtual void ActivateView();
	virtual void DeactivateView();

	CSlrString *messageLine1;
	CSlrString *messageLine2;
	CSlrString *messageLine3;
	CSlrString *messageLine4;

	bool ButtonClicked(CGuiButton *button);
	bool ButtonPressed(CGuiButton *button);

	CGuiButton *btnOK;

	float textPosX;
	float textPosY;

	CGuiMessageBoxCallback *callback;
};

class CGuiMessageBoxCallback
{
public:
	virtual bool MessageBoxClickedOK(CGuiViewMessageBox *messageBox);
};

#endif
//_GUI_VIEW_MSGBOX_

