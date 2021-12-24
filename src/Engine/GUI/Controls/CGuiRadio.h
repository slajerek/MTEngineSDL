/*
 *  CGuiRadio.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-07-06.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_RADIO_H_
#define _GUI_RADIO_H_

#include "CGuiElement.h"
#include "CSlrImage.h"
#include <list>

class CGuiRadioCallback;

class CGuiRadioElement : public CGuiElement
{
public:
	CGuiRadioElement(CSlrImage *imageNotSelected, CSlrImage *imageSelected);
	CGuiRadioElement(float patosX, float posY, float posZ, float sizeX, float sizeY, CSlrImage *imageNotSelected, CSlrImage *imageSelected);
	CSlrImage *imageNotSelected;
	CSlrImage *imageSelected;
	volatile bool isSelected;	
};

class CGuiRadio : public CGuiElement 
{
public:
	CGuiRadio(float posX, float posY, float posZ, float sizeX, float sizeY, std::list<CGuiRadioElement *> *elements, bool blitScaled);
	
	std::list<CGuiRadioElement *> *elements;
	volatile CGuiRadioElement *selectedElement;

	void Render();
	void Render(float posX, float posY);	
	
	bool DoTap(float x, float y);
//	bool DoFinishTap(float x, float y);
//	bool DoDoubleTap(float x, float y);
//	bool DoFinishDoubleTap(float x, float y);
//	bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
//	bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);	
//	void FinishTouches();
//	void DoLogic();

	void SetElement(CGuiRadioElement *selectedElement);
	void SetElement(int elemNum);
	
	bool blitScaled;
};

class CGuiRadioCallback
{
public:
	virtual void RadioElementSelected(CGuiRadioElement *radioElem);
};


#endif
