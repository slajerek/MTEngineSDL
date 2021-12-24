/*
 *  CGuiSlider.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CGuiSlider.h"
#include "CGuiMain.h"

#define GAUGE_SIZE 23
#define GAUGE_SIZE_HALF 11

void CGuiSliderCallback::SliderValueChanged(CGuiSlider *owner, float value)
{
}

void CGuiSlider::SliderValueChanged(float value)
{
}


CGuiSlider::CGuiSlider(float posX, float posY, float posZ, float sizeX, float sizeY, CGuiSliderCallback *callback)
:CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiSlider";
	this->posX = posX;
	this->posY = posY;
	this->sizeX = sizeX;
	this->sizeY = sizeY;
	this->value = 0.5;
	this->changing = false;
	this->callback = callback;

	this->readOnly = false;

	this->alpha = 1.0;
}

bool CGuiSlider::DoTap(float x, float y)
{
	if (!this->visible)
		return false;

//	if (x >= (posX-GAUGE_SIZE_HALF) && x <= (posX+sizeX+GAUGE_SIZE_HALF)
//		&& (y >= (posY-GAUGE_SIZE_HALF)) && y <= (posY+sizeY+GAUGE_SIZE_HALF))

	if (x >= (posX) && x <= (posX+sizeX)
		&& (y >= (posY)) && y <= (posY+sizeY))
	{
		if (!this->readOnly)
		{
			this->value = (x - posX)/sizeX;
			changing = true;

			if (callback != NULL)
			{
				callback->SliderValueChanged(this, this->value);
			}
			this->SliderValueChanged(this->value);
		}
		return true;
	}

	return false;
}

bool CGuiSlider::DoDoubleTap(float x, float y)
{
	if (!this->visible)
		return false;

	//GAUGE_SIZE_HALF
	if (x >= (posX) && x <= (posX+sizeX)
		&& (y >= (posY)) && y <= (posY+sizeY))
	{
		if (!this->readOnly)
		{
			this->value = (x - posX)/sizeX;
			changing = true;

			if (callback != NULL)
			{
				callback->SliderValueChanged(this, this->value);
			}
			this->SliderValueChanged(this->value);
		}
		return true;
	}
	return false;
}

//bool DoFinishTap(float x, float y);
bool CGuiSlider::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	if (!this->visible)
		return false;

	if (x >= (posX) && x <= (posX+sizeX)
		&& (y >= (posY)) && y <= (posY+sizeY))
	{
		if (!this->readOnly)
		{
			this->value = (x - posX)/sizeX;
			changing = true;

			if (callback != NULL)
			{
				callback->SliderValueChanged(this, this->value);
			}
			this->SliderValueChanged(this->value);
		}
		return true;
	}
	else if (changing == true)
	{
		if (!this->readOnly)
		{
			if (x < posX)
			{
				this->value = 0.0;
			}
			else if (x > (posX+sizeX))
			{
				this->value = 1.0;
			}
			else
			{
				this->value = (x - posX)/sizeX;
			}				

			if (callback != NULL)
			{
				callback->SliderValueChanged(this, this->value);
			}
			this->SliderValueChanged(this->value);
		}
		return true;
	}

	return false;
}

bool CGuiSlider::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	if (!this->visible)
		return false;

	if (changing)
	{
		this->changing = false;
		return true;
	}
	return false;
}

void CGuiSlider::DoLogic()
{
}


void CGuiSlider::Render()
{
	this->Render(this->posX, this->posY, this->sizeX, this->sizeY);
}

void CGuiSlider::Render(float posX, float posY)
{
	this->Render(posX, posY, this->sizeX, this->sizeY);
}

void CGuiSlider::Render(float posX, float posY, float sizeX, float sizeY)
{
	if (!this->visible)
		return;

	guiMain->theme->imgSliderFull->RenderAlpha(
		 posX, posY, posZ,
		 (sizeX * this->value), sizeY,
		 0.0, 0.0, this->value, 1.0, alpha);

	guiMain->theme->imgSliderEmpty->RenderAlpha(
		 (posX + (sizeX * this->value)), posY, posZ,
		 (sizeX - (sizeX * this->value)), sizeY,
		 this->value, 0.0, 1.0, 1.0, alpha);

	guiMain->theme->imgSliderGauge->RenderAlpha(
		 (posX + (sizeX*this->value) - GAUGE_SIZE_HALF),
		 (posY), posZ+0.01,	//(sizeX/100)*4      (sizeY*0.3)	// - (sizeY*0.36)
		 GAUGE_SIZE, sizeY,
		 0.0, 0.0, 1.0, 1.0, alpha);
}

void CGuiSlider::SetValue(float value)
{
	this->value = value;
}


