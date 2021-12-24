/*
 *  CGuiListElements.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-01-07.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_VIEW_LIST_
#define _GUI_VIEW_LIST_

#include "CSlrImage.h"
#include "CGuiElement.h"
#include "CGuiView.h"

class CGuiViewListCallback2;

class CGuiViewList : public CGuiElement
{
public:
	CGuiViewList(float posX, float posY, float posZ, float sizeX, float sizeY,
				 CGuiElement **elements, int numElements, bool deleteElements, CGuiViewListCallback2 *callback);
	
	//float fontWidth, fontHeight;
	
	void Init(CGuiElement **elements, int numElements, bool deleteElements);
	
	virtual void Render();
	
	//byte selectedInstrumentNum;
	
	volatile int selectedElement;
	volatile int firstShowElement;
	
	//char *listTextHeader;
	
	int numElements;
	//std::vector<char *> listElements;
	CGuiElement **listElements;
	
	bool deleteElements;
	
	//char drawBuf[64];
	
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
	void MoveView(float diffX, float diffY);

	bool clickConsumed;
	
	void UpdateRollUp();
	void SetElement(int elementNum, bool updatePosition);
	
	CGuiElement *GetSelectedElement();

	//float fontSize;// = 11;
	//float leftMenuGap = 32;
	//float startElementsY;// = 17;
	
	float ySpeed;// = 0.0;	
	//float zoomFontSize;// = fontSize;
	
	int numRollUp;
	bool selectionHighlight;
	bool moving;
	
	virtual void ElementSelected();
	CGuiViewListCallback2 *callback;
	
	int backupSelectedElement;
	int backupFirstShowElement;
	float backupStartDrawY;			
	void BackupViewListPosition();
	void RestoreViewListPosition();

	CSlrImage *imgBackground;

	float colorSelectionR;
	float colorSelectionG;
	float colorSelectionB;
	float colorSelectionA;

	void LockRenderMutex();
	void UnlockRenderMutex();
};

class CGuiViewListCallback2
{
public:
	virtual void ViewListElementSelected(CGuiViewList *listBox);
};


#endif //_GUI_VIEW_LIST_
