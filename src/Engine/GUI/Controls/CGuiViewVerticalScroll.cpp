/*
 *  CGuiViewVerticalScroll.cpp
 *
 *  Created by Marcin Skoczylas on 10-01-07.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CGuiViewVerticalScroll.h"
#include "VID_Main.h"
#include "CGuiMain.h"


void CGuiViewVerticalScrollCallback::VerticalScrollElementSelected(CGuiViewVerticalScroll *listBox)
{
	return;
}


CGuiViewVerticalScroll::CGuiViewVerticalScroll(float posX, float posY, float posZ, float sizeX, float sizeY,
						   CGuiElement **elements, int numElements, bool deleteElements, CGuiViewVerticalScrollCallback *callback)
: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiViewVerticalScroll";

	this->callback = callback;

	this->listElements = NULL;
	this->Init(elements, numElements, deleteElements);
}

void CGuiViewVerticalScroll::Init(CGuiElement **elements, int numElements, bool deleteElements)
{
	if (listElements != NULL && this->deleteElements)
	{
		// TODO:
		LOGTODO("CGuiViewVerticalScroll::Init: listElements != NULL, delete elements");
		/*

		 crashes:
		for (int i = 0; i < numElements; i++)
		{
			delete [] listElements[i];
		}
		delete listElements;*/
	}

	this->deleteElements = deleteElements;

	this->numElements = numElements;
	this->listElements = elements;

	selectedElement = 0;
	drawPosY = 0;

	state = STATE_SHOW;
}

bool CGuiViewVerticalScroll::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	if (!visible)
		return false;

	if (!IsInside(x, y))
		return false;

	if (state == STATE_SHOW)
	{
		if (selectedElement == 0)
		{
			if (diffY > 0)
			{
				drawPosY += diffY/2.0;
			}
			else
			{
				drawPosY += diffY;
			}
		}
		else if (selectedElement == this->numElements-1)
		{
			if (diffY < 0)
			{
				drawPosY += diffY/2.0;
			}
			else
			{
				drawPosY += diffY;
			}

		}
		else
		{
			drawPosY += diffY;
		}

		if (drawPosY > SCREEN_WIDTH)
		{
			selectedElement -= 1;
			drawPosY = 0;
		}
		else if (drawPosY < -SCREEN_WIDTH)
		{
			selectedElement += 1;
			drawPosY = 0;
		}
	}

	return true;
}

bool CGuiViewVerticalScroll::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	LOGD("CGuiViewVerticalScroll::FinishMove");
	if (!visible)
		return false;

	if (!IsInside(x, y))
		return false;

	accelerationY = -accelerationY;

	LOGD("CGuiViewVerticalScroll::FinishMove (2)");
	if (selectedElement == 0 && drawPosY > 0.0)	//SCREEN_WIDTH/2
	{
		float drawY = drawPosY;
		float stepY = ( drawPosY / VERTICAL_SCROLL_ACCEL_NUM_FRAMES );
		for (int i = 0; i < VERTICAL_SCROLL_ACCEL_NUM_FRAMES; i++)
		{
			animDrawPosY[i] = drawY;
			drawY -= stepY;
		}

		state = STATE_MOVE_ANIM;
		moveAnimFrame = 0;
		nextCurrentTlo = selectedElement;
	}
	else if (selectedElement == (this->numElements-1) && drawPosY < 0.0)
	{
		float drawY = drawPosY;
		float stepY = ( drawPosY / VERTICAL_SCROLL_ACCEL_NUM_FRAMES );
		for (int i = 0; i < VERTICAL_SCROLL_ACCEL_NUM_FRAMES; i++)
		{
			animDrawPosY[i] = drawY;
			drawY -= stepY;
		}

		state = STATE_MOVE_ANIM;
		moveAnimFrame = 0;
		nextCurrentTlo = selectedElement;

	}
	else if (accelerationY > 0)
	{
		if (drawPosY < 0.0)
		{
			// -SCRW|  posX
			// -320 | -240		| 0.0  |  240.0  | 320 |
			float distance = SCREEN_WIDTH + drawPosY;
			LOGD("distance=%f", distance);

			float stepY = distance/VERTICAL_SCROLL_ACCEL_NUM_FRAMES;

			float drawY = drawPosY - stepY;
			for (int i = 0; i < VERTICAL_SCROLL_ACCEL_NUM_FRAMES; i++)
			{
				if ((i-1) < 0)
					continue;

				animDrawPosY[i-1] = drawY;
				drawY -= stepY;
			}

			state = STATE_MOVE_ANIM;
			moveAnimFrame = 0;
			nextCurrentTlo = selectedElement + 1;
		}
	}
	else if (accelerationY < 0)
	{
		if (drawPosY > 0.0)
		{
			//
			float distance = SCREEN_WIDTH - drawPosY;
			LOGD("distance=%f", distance);

			float stepY = distance/VERTICAL_SCROLL_ACCEL_NUM_FRAMES;
			float drawY = drawPosY + stepY;
			for (int i = 0; i < VERTICAL_SCROLL_ACCEL_NUM_FRAMES; i++)
			{
				if ((i-1) < 0)
					continue;

				animDrawPosY[i-1] = drawY;
				drawY += stepY;
			}

			state = STATE_MOVE_ANIM;
			moveAnimFrame = 0;
			nextCurrentTlo = selectedElement - 1;
		}
	}

	return true;
}

bool CGuiViewVerticalScroll::InitZoom()
{
	if (!visible)
		return false;

	return true;
}

bool CGuiViewVerticalScroll::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	if (!visible)
		return false;

	return false;
}


bool CGuiViewVerticalScroll::DoTap(float x, float y)
{
	if (numElements < 1)
		return false;

	if (!IsInside(x, y))
		return false;

	return true;
}

bool CGuiViewVerticalScroll::DoFinishTap(float x, float y)
{
	if (!visible)
		return false;

	if (numElements < 1)
		return false;

	if (!IsInside(x, y))
		return false;

	return true;
}

bool CGuiViewVerticalScroll::DoDoubleTap(float x, float y)
{
	if (!visible)
		return false;

	return DoFinishTap(x, y);
}

void CGuiViewVerticalScroll::Render()
{
	if (!visible)
		return;

	float drawY = drawPosY - SCREEN_WIDTH;
	for (int i = selectedElement-1; i < this->numElements; i++)
	{
		if (i < 0 || i > this->numElements)
		{
			drawY += SCREEN_WIDTH;
			continue;
		}

		if (listElements[i] != NULL)
			listElements[i]->Render(posX, posY + drawY);

		drawY += SCREEN_WIDTH;
		if (drawY > SCREEN_WIDTH)
			break;
	}
}

void CGuiViewVerticalScroll::DoLogic()
{
	if (!visible)
		return;

//	LOGD("CGuiViewVerticalScroll::DoLogic");
	if (state == STATE_MOVE_ANIM)
	{
		LOGD("CGuiViewVerticalScroll::DoLogic: state == STATE_MOVE_ANIM");
		drawPosY = animDrawPosY[moveAnimFrame];
		moveAnimFrame++;
		if (moveAnimFrame == VERTICAL_SCROLL_ACCEL_NUM_FRAMES)
		{
			state = STATE_SHOW;
			selectedElement = nextCurrentTlo;
			drawPosY = 0;

			if (this->callback)
				this->callback->VerticalScrollElementSelected(this);
		}
	}
}

void CGuiViewVerticalScroll::ScrollHome()
{
	this->ScrollTo(0);
}

void CGuiViewVerticalScroll::ScrollTo(int newElement)
{
	this->state = STATE_SHOW;
	this->selectedElement = newElement;
}

void CGuiViewVerticalScroll::ElementSelected()
{
	LOGG("CGuiViewVerticalScroll::ElementSelected()");
}

