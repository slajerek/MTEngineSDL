/*
 *  CGuiList.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-16.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_LIST_TREE_
#define _GUI_LIST_TREE_

#include "CSlrFont.h"
#include "CSlrImage.h"
#include "CGuiElement.h"
#include "CGuiView.h"
#include <vector>

#define GUI_LIST_ELEMTYPE_CHARS 1
#define GUI_LIST_ELEMTYPE_NSSTRINGS 2

class CGuiListTreeCallback;

class CGuiListTreeElement
{
public:
	CGuiListTreeElement(CGuiListTreeElement *parent, char *text, void *obj);
	~CGuiListTreeElement();

	CGuiListTreeElement *parent;
	char *text;
	u32 textLen;
	void *obj;

	i64 elementId;

	std::vector<CGuiListTreeElement *> elements;

	bool unfoldable;
	bool unfolded;

	u32 openerSpaces;

	u32 CountUnfolded();
	u32 AddToListElems(char **listElems, CGuiListTreeElement **listTreeElems, u32 elemNum, u32 numSpaces);

	CGuiListTreeElement *FindObj(void *obj);

	void AddToMap(std::map<i64, CGuiListTreeElement *> *map);
	void ReInit(std::map<i64, CGuiListTreeElement *> *map);

	void SetUnfoldedToAllParents(bool unfolded);
};

class CGuiListTree : public CGuiElement
{
public:
	CGuiListTree(float posX, float posY, float posZ, float sizeX, float sizeY, float fontSize,
				 bool deleteElements, CSlrFont *font,
				 CSlrImage *imgBackground, float backgroundAlpha, CGuiListTreeCallback *callback);

	CGuiListTree(float posX, float posY, float posZ, float sizeX, float sizeY, float fontSize,
				 std::vector<CGuiListTreeElement *> *elements, bool deleteElements, CSlrFont *font,
			 CSlrImage *imgBackground, float backgroundAlpha, CGuiListTreeCallback *callback);
	~CGuiListTree();

	void Init(std::vector<CGuiListTreeElement *> *elements, bool deleteElements);
	void ReInit(std::vector<CGuiListTreeElement *> *elements, bool deleteElements);
	void UpdateListElements();

	virtual void Render();

	void DebugPrintTree();

	//byte selectedInstrumentNum;

	void *GetObject(u32 elemNum);

	volatile int selectedElement;
	volatile void *selectedObject;

	volatile int firstShowElement;
	volatile bool readOnly;

	//char *listTextHeader;

	CSlrImage *imgBackground;
	float backgroundAlpha;

	CSlrFont *font;

	u8 typeOfElements;

	int numElements;
	void **listElements;
	//float *elemOpenerStartX;
	//float *elemOpenerEndX;
	CGuiListTreeElement **listTreeElements;
	bool deleteElements;

	CGuiListTreeElement *GetTreeElementByObj(void *obj);

	std::vector<CGuiListTreeElement *> treeElements;
	std::map<i64, CGuiListTreeElement *> mapElements;

	char drawBuf[64];

	//float fontWidth, fontHeight;

	float startDrawX;
	float startDrawY;

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	virtual void MoveView(float diffX, float diffY);
	virtual void DoLogic();

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoDoubleTap(float x, float y);

	void UpdateRollUp();
	void SetElement(int elementNum, bool updatePosition);
	void SetObject(void *obj, bool updatePosition);

	volatile bool scrollModified;

	float fontSize;// = 11;
	//float leftMenuGap = 32;
	float startElementsY;// = 17;

	float ySpeed;// = 0.0;
	float zoomFontSize;// = fontSize;

	int numRollUp;

	bool moving;

	virtual void ElementSelected();
	CGuiListTreeCallback *callback;

	void LockRenderMutex();
	void UnlockRenderMutex();
};

class CGuiListTreeCallback
{
public:
	virtual void ListTreeElementSelected(CGuiListTree *listBox);
};


#endif //_GUI_LIST_TREE_
