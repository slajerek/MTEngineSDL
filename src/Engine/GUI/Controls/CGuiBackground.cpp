/*
 *  CGuiBackground.mm
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-01-07.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#define DEFAULT_LABEL_ZOOM 1.0

#include "CGuiBackground.h"
#include "VID_Main.h"
#include "CGuiMain.h"

CGuiBackground::CGuiBackground(CSlrImage *image, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiBackground";

	this->image = image;

	this->posX = posX;
	this->posY = posY;
	this->sizeX = sizeX;
	this->sizeY = sizeY;
	
	this->colorR = 0.0f;
	this->colorG = 0.0f;
	this->colorB = 1.0f;
	this->colorA = 0.75f;
}

CGuiBackground::CGuiBackground(float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiBackground:Text";

	this->image = guiMain->theme->imgBackground;

	this->posX = posX;
	this->posY = posY;
	this->sizeX = sizeX;
	this->sizeY = sizeY;
}

CGuiBackground::CGuiBackground(float posX, float posY, float posZ, float sizeX, float sizeY, float colorR, float colorG, float colorB, float alpha)
: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiBackground:Text";
	
	this->image = NULL;
	
	this->posX = posX;
	this->posY = posY;
	this->sizeX = sizeX;
	this->sizeY = sizeY;
	
	this->colorR = colorR;
	this->colorG = colorG;
	this->colorB = colorB;
	this->colorA = alpha;
	
}

void CGuiBackground::Render()
{
	if (this->visible)
	{
		if (this->image != NULL)
		{
			//LOGD("CGuiBackground::Render: %s posX=%f posY=%f sizeX=%f sizeY=%f", name, posX, posY, sizeX, sizeY);
			image->Render(posX, posY, posZ, this->sizeX, this->sizeY);
		}
		else
		{
			BlitFilledRectangle(posX, posY, posZ, sizeX, sizeY, colorR, colorG, colorB, colorA);
		}
	}
}

void CGuiBackground::Render(float posX, float posY)
{
	if (this->visible)
	{
		if (this->image != NULL)
			image->Render(posX, posY, posZ, this->sizeX, this->sizeY);
	}
}

// @returns is consumed
bool CGuiBackground::DoTap(float x, float y)
{
	if (IsInside(x, y))
		return true;
	return false;
}

bool CGuiBackground::DoFinishTap(float x, float y)
{
	if (IsInside(x, y))
		return true;
	return false;
}

// @returns is consumed
bool CGuiBackground::DoDoubleTap(float x, float y)
{
	if (IsInside(x, y))
		return true;
	return false;
}

bool CGuiBackground::DoFinishDoubleTap(float x, float y)
{
	if (IsInside(x, y))
		return true;
	return false;
}

bool CGuiBackground::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	if (IsInside(x, y))
		return true;
	return false;
}

bool CGuiBackground::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	if (IsInside(x, y))
		return true;
	return false;
}

void CGuiBackground::DoLogic()
{
	return;
}

