/*
 *  CGuiListElements.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-01-07.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_VIEW_HORIZONTAL_SCROLL_
#define _GUI_VIEW_HORIZONTAL_SCROLL_

#include "CSlrImage.h"
#include "CGuiElement.h"
#include "CGuiView.h"

class CGuiViewHorizontalScrollCallback;

#define HORIZONTAL_SCROLL_ACCEL_NUM_FRAMES 5

#define STATE_SHOW 0
#define STATE_MOVE_ANIM	1

class CGuiViewHorizontalScroll : public CGuiElement
{
public:
	CGuiViewHorizontalScroll(float posX, float posY, float posZ, float sizeX, float sizeY,
				 CGuiElement **elements, int numElements, bool deleteElements, CGuiViewHorizontalScrollCallback *callback);
	
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
	CGuiViewHorizontalScrollCallback *callback;

	float animDrawPosX[HORIZONTAL_SCROLL_ACCEL_NUM_FRAMES + 5];
	int selectedElement;
	int nextCurrentTlo;

	float drawPosX;
	int moveAnimFrame;

	u8 state;
};

class CGuiViewHorizontalScrollCallback
{
public:
	virtual void HorizontalScrollElementSelected(CGuiViewHorizontalScroll *listBox);
};


#endif //_GUI_VIEW_HORIZONTAL_SCROLL_
