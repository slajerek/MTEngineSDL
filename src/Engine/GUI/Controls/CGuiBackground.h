/*
 *  CGuiBackground.h
 *  MTEngine
 *
 *  Created by Marcin Skoczylas on 10-01-07.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_BACKGROUND_
#define _GUI_BACKGROUND_

#include "CGuiElement.h"
#include "CSlrImage.h"
#include "SYS_Main.h"

class CGuiBackground : public CGuiElement
{
public:
	CGuiBackground(float posX, float posY, float posZ, float sizeX, float sizeY);
	CGuiBackground(CSlrImage *image, float posX, float posY, float posZ, float sizeX, float sizeY);
	CGuiBackground(float posX, float posY, float posZ, float sizeX, float sizeY, float colorR, float colorG, float colorB, float alpha);
	void Render();
	void Render(float posX, float posY);

	bool DoTap(float x, float y);
	bool DoFinishTap(float x, float y);
	bool DoDoubleTap(float x, float y);
	bool DoFinishDoubleTap(float x, float y);
	bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	void DoLogic();

	CSlrImage *image;
	float colorR, colorG, colorB, colorA;
};

#endif
//_GUI_BUTTON_

