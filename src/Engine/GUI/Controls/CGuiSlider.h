/*
 *  CGuiSlider.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_SLIDER_
#define _GUI_SLIDER_
#include "CGuiElement.h"

class CGuiSliderCallback;

class CGuiSlider : public CGuiElement
{
public:
	CGuiSlider(float posX, float posY, float posZ, float sizeX, float sizeY, CGuiSliderCallback *callback);
	void Render();
	void Render(float posX, float posY);
	void Render(float posX, float posY, float sizeX, float sizeY);
	volatile float alpha;

	volatile float value;	// <0.0, 1.0>
	void SetValue(float value);

	volatile bool changing;
	volatile bool readOnly;

	bool DoTap(float x, float y);
	//bool DoFinishTap(float x, float y);
	bool DoDoubleTap(float x, float y);
	bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	void DoLogic();

	CGuiSliderCallback *callback;

	virtual void SliderValueChanged(float value);

};

class CGuiSliderCallback
{
public:
	virtual void SliderValueChanged(CGuiSlider *slider, float value);
};

#endif
//_GUI_SLIDER_

