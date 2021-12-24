/*
 *  CGuiList.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-16.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_LIST_
#define _GUI_LIST_

#include "CSlrFont.h"
#include "CSlrImage.h"
#include "CGuiElement.h"
#include "CGuiView.h"
#include "SYS_Threading.h"

#define GUI_LIST_ELEMTYPE_CHARS 1
#define GUI_LIST_ELEMTYPE_NSSTRINGS 2

class CGuiListCallback;

class CGuiList : public CGuiElement
{
public:
	CGuiList(float posX, float posY, float posZ, float sizeX, float sizeY, float fontSize, //float fontWidth, float fontHeight,
			 char **elements, int numElements, bool deleteElements, CSlrFont *font, 
			 CSlrImage *imgBackground, float backgroundAlpha, CGuiListCallback *callback);
	~CGuiList();
	
	void Init(char **elements, int numElements, bool deleteElements);
	virtual void Render();
	
	//byte selectedInstrumentNum;
	
	volatile int selectedElement;
	volatile int firstShowElement;
	
	volatile bool readOnly;
	
	//char *listTextHeader;
	
	CSlrImage *imgBackground;
	float backgroundAlpha;
	
	CSlrFont *font;
	CSlrFont *fontSelected;
	
	u8 typeOfElements;
	
	int numElements;
	//std::vector<char *> listElements;
	void **listElements;
	bool deleteElements;
	
	char drawBuf[64];
	
	//float fontWidth, fontHeight;
	
	float startDrawX;
	float startDrawY;		
	
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	virtual void MoveView(float diffX, float diffY);
	virtual bool DoScrollWheel(float deltaX, float deltaY);

	virtual void DoLogic();
	
	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoDoubleTap(float x, float y);
	
	void UpdateRollUp();
	void SetElement(int elementNum, bool updatePosition);
	
	volatile bool scrollModified;
	
	float fontScale;
	
	float fontSize;// = 11;
	//float leftMenuGap = 32;
	float startElementsY;// = 17;
	
	float ySpeed;// = 0.0;	
	float zoomFontSize;// = fontSize;
	
	int numRollUp;
	
	bool moving;
	
	float listUpGap;
	float elementsGap;
	
	float textOffsetX;
	float textOffsetY;

	
	virtual void ElementSelected();
	CGuiListCallback *callback;
	
	void SetGaps(float listUpGap, float elementsGap);
	
	int backupSelectedElement;
	int backupFirstShowElement;
	float backupStartDrawY;			
	void BackupListPosition();
	void RestoreListPosition();
	
	void LockRenderMutex();
	void UnlockRenderMutex();
	
	virtual void SetFont(CSlrFont *font, float fontSize);
	
};

class CGuiListCallback
{
public:
	// called a while before selection after user tap: return true=yes, do select; false=no, cancel select
	virtual bool ListElementPreSelect(CGuiList *listBox, int elementNum);
	virtual void ListElementSelected(CGuiList *listBox);
};


#endif //_GUI_LIST_
