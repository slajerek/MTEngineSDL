/*
 *  CGuiButtonMenu.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-01.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_BUTTON_MENU_
#define _GUI_BUTTON_MENU_

#include "CGuiButton.h"
#include <list>

class CGuiButtonMenu : public CGuiButton
{
public:
	CGuiButtonMenu(CSlrImage *image, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment,
				   CSlrImage *backgroundImage, 
				   float backgroundPosX, float backgroundPosY, float backgroundSizeX, float backgroundSizeY,
				   CGuiButtonCallback *callback);
	CGuiButtonMenu(CSlrImage *image, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment,
				   CGuiButtonCallback *callback);
	
	CGuiButtonMenu(char *text, bool blah, float posX, float posY, float posZ, float sizeX, float sizeY, u8 alignment,
				   CGuiButtonCallback *callback);
	virtual void Render();
	void RenderElements();
	void RenderButton();
	
	/*
	bool IsInsideButton(float posX, float posY);
	*/

//	void SetMenu(std::list<CGuiElement *> *menuElements);
	
	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoFinishDoubleTap(float x, float y);
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);	
	virtual void FinishTouches();
	virtual void DoLogic();
	
	void HideSubMenu(bool immediately);
	void HideSubMenuNoCallback(bool immediately);
	
	CSlrImage *backgroundImage;
	float backgroundPosX;
	float backgroundPosY;
	float backgroundSizeX;
	float backgroundSizeY;
	
	bool manualRendering;

	// overwrite this
	//bool Clicked(float posX, float posY);
	//bool Pressed(float posX, float posY);
	
	//std::list<CGuiElement *> *menuElements;
	
	float previousZ;
	
	void AddMenuSubItem(CGuiElement *guiElement, float z);
	void AddMenuSubItem(CGuiElement *guiElement);
	
	volatile bool finishTapConsumed;	
	
	void SetExpanded(bool expanded);
};

#endif //_GUI_BUTTON_
