/*
 *  CGuiRadio.mm
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-07-06.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CGuiRadio.h"
#include "CGuiMain.h"

void RadioElementSelected(CGuiRadioElement *radioElem)
{
	return;
}

CGuiRadioElement::CGuiRadioElement(float posX, float posY, float posZ, float sizeX, float sizeY, CSlrImage *imageNotSelected, CSlrImage *imageSelected)
: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->imageNotSelected = imageNotSelected;
	this->imageSelected = imageSelected;
	this->isSelected = false;
}

CGuiRadioElement::CGuiRadioElement(CSlrImage *imageNotSelected, CSlrImage *imageSelected)
: CGuiElement(50, 50, -1, 100, 100)
{
	this->imageNotSelected = imageNotSelected;
	this->imageSelected = imageSelected;
	this->isSelected = false;
}

CGuiRadio::CGuiRadio(float posX, float posY, float posZ, float sizeX, float sizeY, std::list<CGuiRadioElement *> *elements, bool blitScaled)
: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiRadio";
	
	this->elements = elements;
	this->selectedElement = NULL;
	this->blitScaled = blitScaled;
}

void CGuiRadio::Render()
{
	this->Render(this->posX, this->posY);
}

void CGuiRadio::Render(float posX, float posY)
{
	if (blitScaled)
	{
		float elemSizeX = this->sizeX / this->elements->size();
		float drawX = posX;
		
		for (std::list<CGuiRadioElement *>::iterator enumElem = this->elements->begin();
			 enumElem != this->elements->end(); enumElem++)
		{
			CGuiRadioElement *radioElem = (*enumElem);
			
			if (radioElem->isSelected)
			{
				//void Render(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
				radioElem->imageSelected->Render(drawX, posY, this->posZ,
					 elemSizeX, this->sizeY);
			}
			else 
			{
				radioElem->imageNotSelected->Render(drawX, posY, this->posZ,
					 elemSizeX, this->sizeY);				
			}
			
			drawX += elemSizeX;

			radioElem->Render();
		}
	}
	else
	{
		for (std::list<CGuiRadioElement *>::iterator enumElem = this->elements->begin();
			 enumElem != this->elements->end(); enumElem++)
		{
			CGuiRadioElement *radioElem = (*enumElem);
			
			if (radioElem->isSelected)
			{
				//void Render(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
				radioElem->imageSelected->Render(radioElem->posX, radioElem->posY, this->posZ,
					 radioElem->sizeX, radioElem->sizeY);
			}
			else 
			{
				//void Render(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
				radioElem->imageNotSelected->Render(radioElem->posX, radioElem->posY, this->posZ,
					 radioElem->sizeX, radioElem->sizeY);				
			}

			radioElem->Render();
		}
	}
	
	
}

void CGuiRadio::SetElement(CGuiRadioElement *selectedElement)
{
	for (std::list<CGuiRadioElement *>::iterator enumElem = this->elements->begin();
		 enumElem != this->elements->end(); enumElem++)
	{
		CGuiRadioElement *radioElem = (*enumElem);
		if (radioElem == selectedElement)
		{
			radioElem->isSelected = true;
		}
		else 
		{
			radioElem->isSelected = false;
		}
	}
	
	this->selectedElement = selectedElement;
}

void CGuiRadio::SetElement(int elemNum)
{
	int i = 0;
	
	CGuiRadioElement *selectElem = NULL;
	
	for (std::list<CGuiRadioElement *>::iterator enumElem = this->elements->begin();
		 enumElem != this->elements->end(); enumElem++)
	{
		CGuiRadioElement *radioElem = (*enumElem);
		if (i == elemNum)
		{
			radioElem->isSelected = true;
			selectElem = radioElem;
		}
		else 
		{
			radioElem->isSelected = false;
		}
		
		i++;
	}
	
	this->selectedElement = selectElem;
}

bool CGuiRadio::DoTap(float x, float y)
{
	LOGG("CGuiRadio::DoTap(%d, %d)", x, y);
	for (std::list<CGuiRadioElement *>::iterator enumElem = this->elements->begin();
		 enumElem != this->elements->end(); enumElem++)
	{
		CGuiRadioElement *radioElem = (*enumElem);
	
		if (radioElem->IsInside(x, y))
		{
			this->SetElement(radioElem);
			return true;
		}
	}
	
	return false;
}

/*
bool CGuiRadio::DoFinishTap(float x, float y);
bool CGuiRadio::DoDoubleTap(float x, float y);
bool CGuiRadio::DoFinishDoubleTap(float x, float y);
bool CGuiRadio::DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
bool CGuiRadio::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);	
void CGuiRadio::FinishTouches();
void CGuiRadio::DoLogic();
*/
