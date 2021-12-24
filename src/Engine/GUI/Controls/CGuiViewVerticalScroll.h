/*
 *  CGuiViewVerticalScroll.h
 *
 *  Created by Marcin Skoczylas on 10-01-07.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_VIEW_VERTICAL_SCROLL_
#define _GUI_VIEW_VERTICAL_SCROLL_

#include "CSlrImage.h"
#include "CGuiElement.h"
#include "CGuiView.h"

class CGuiViewVerticalScrollCallback;

#define VERTICAL_SCROLL_ACCEL_NUM_FRAMES 5

#define STATE_SHOW 0
#define STATE_MOVE_ANIM	1

class CGuiViewVerticalScroll : public CGuiElement
{
public:
	CGuiViewVerticalScroll(float posX, float posY, float posZ, float sizeX, float sizeY,
				 CGuiElement **elements, int numElements, bool deleteElements, CGuiViewVerticalScrollCallback *callback);
	
	void Init(CGuiElement **elements, int numElements, bool deleteElements);
	
	virtual void Render();
	
	int numElements;
	CGuiElement **listElements;
	
	bool deleteElements;
	
	float startDrawX;
	float startDrawY;	
	
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	virtual void DoLogic();
	
	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoDoubleTap(float x, float y);
	
	void ScrollHome();
	void ScrollTo(int newElement);

	virtual void ElementSelected();
	CGuiViewVerticalScrollCallback *callback;

	float animDrawPosY[VERTICAL_SCROLL_ACCEL_NUM_FRAMES + 5];
	int selectedElement;
	int nextCurrentTlo;

	float drawPosY;
	int moveAnimFrame;

	u8 state;
};

class CGuiViewVerticalScrollCallback
{
public:
	virtual void VerticalScrollElementSelected(CGuiViewVerticalScroll *listBox);
};


#endif //_GUI_VIEW_VERTICAL_SCROLL_
