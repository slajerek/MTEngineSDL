#include "CGuiViewToolBox.h"
#include "SYS_Main.h"
#include "RES_ResourceManager.h"
#include "CGuiMain.h"
#include "CDataAdapter.h"
#include "CSlrString.h"
#include "SYS_KeyCodes.h"
#include "SYS_Threading.h"
#include "CGuiEditHex.h"
#include "VID_ImageBinding.h"
#include "CSlrFileFromOS.h"
#include "CSlrFileFromDocuments.h"
#include <list>

CGuiViewToolBox::CGuiViewToolBox(float posX, float posY, float posZ,
						   float iconGapX, float iconGapY,
						   float iconSizeX, float iconSizeY, float iconStepX, float iconStepY,
						   int numColumns,
						   CSlrString *windowName,
						   CGuiViewToolBoxCallback *callback)
: CGuiWindow("CGuiViewToolBox", posX, posY, posZ, 123, 98, windowName)
{
	this->callback = callback;

	font = guiMain->fntEngineDefault;
	fontScale = 1.5;
	fontHeight = font->GetCharHeight('@', fontScale) + 2;
	
	this->iconGapX = iconGapX;
	this->iconGapY = iconGapY;
	this->iconSizeX = iconSizeX;
	this->iconSizeY = iconSizeY;

	this->iconStepX = iconStepX;
	this->iconStepY = iconStepY;
	
	this->numColumns = numColumns;

	this->nextIconX = iconGapX;
	this->nextIconY = iconGapY;
	this->numIconsInCurrentRow = 0;
	this->numRows = 0;
		
	backgroundColorR = 58.0f/255.0f;
	backgroundColorG = 57.0f/255.0f;
	backgroundColorB = 57.0f/255.0f;
	backgroundColorA = 1.0f;

	
//	if (withWindowFrame)
//	{
//		LOGD("withWindowFrame!!!!!!!!!!!!!!!!");
//		//viewFrame->barColorR, viewFrame->barColorG, viewFrame->barColorB, 1);
//
////		viewFrame = new CGuiViewFrame(this, windowName);
////		this->AddGuiElement(viewFrame);
//	}
//
}

CGuiButton *CGuiViewToolBox::AddIcon(CSlrImage *imgIcon)
{
	return this->AddIcon(imgIcon, nextIconX, nextIconY);
}

CGuiButton *CGuiViewToolBox::AddIcon(CSlrImage *imgIcon, float iconX, float iconY)
{
	CGuiButton *btnIcon = new CGuiButton(imgIcon, 0, 0, posZ, iconSizeX, iconSizeY, BUTTON_ALIGNED_CENTER, this);
	btnIcon->name = "CGuiButton ToolBox icon";
	
	btnIcon->SetPositionOffset(iconX, iconY, 0);

	//	pressed background: #23282C
	btnIcon->buttonPressedColorR = 0.1373f;
	btnIcon->buttonPressedColorG = 0.1569f;
	btnIcon->buttonPressedColorB = 0.1725f;
	this->AddGuiElement(btnIcon);
	this->buttons.push_back(btnIcon);
	
	nextIconX = iconX; nextIconY = iconY;
	
	nextIconX += iconStepX;
	numIconsInCurrentRow++;

	float sx = -1;
	float sy = -1;
	
	if (numIconsInCurrentRow == numColumns)
	{
		sx = nextIconX;
		
		numIconsInCurrentRow = 0;
		nextIconX = iconGapX;
		nextIconY += iconStepY;
		numRows++;
	}

	if (numRows == 0)
	{
		sx = nextIconX;
		sy = nextIconY + iconStepY;
	}
	else
	{
		sy = nextIconY;
	}
	
	this->SetPosition(posX, posY, posZ, sx, sy);
	if (viewFrame)
	{
		viewFrame->SetSize(sx, sy);
	}
	return btnIcon;
}


void CGuiViewToolBox::UpdateSize(int numColumns)
{
	this->numColumns = numColumns;
	
	int cx = 0;
	
	nextIconX = iconGapX;
	nextIconY = iconGapY;
	
	float sx = -1;
	float sy = -1;
	
	for (std::vector<CGuiButton *>::iterator it = buttons.begin(); it != buttons.end(); it++)
	{
		CGuiButton *btn = *it;
		
		btn->SetPositionOffset(nextIconX, nextIconY, 0);
		
		nextIconX += iconStepX;
		
		cx++;
		
		if (cx == numColumns)
		{
			sx = nextIconX;
			cx = 0;
			nextIconX = iconGapX;
			nextIconY += iconStepY;
		}
	}
	
	if (sx < 0)
	{
		sx = nextIconX;
		sy = nextIconY + iconStepY;
	}
	else
	{
		sy = nextIconY;
	}
	
	this->SetPosition(posX, posY, posZ, sx, sy);
	if (viewFrame)
	{
		viewFrame->SetSize(sx, sy);
	}
}


bool CGuiViewToolBox::ButtonPressed(CGuiButton *button)
{
	LOGD("CGuiViewToolBox::ButtonPressed");
	
	CSlrImage *img = (CSlrImage*)button->image;
	
	if (this->callback)
	{
		this->callback->ToolBoxIconPressed(img);
	}
	
	return false;
}


//void CGuiViewToolBox::SetPosition(float posX, float posY)
//{
//	LOGD("CGuiViewToolBox::SetPosition: %f %f (sizeX=%f sizeY=%f)", posX, posY, this->sizeX, this->sizeY);
//	
//	CGuiWindow::SetPosition(posX, posY);
//}
//
//void CGuiViewToolBox::SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY)
//{
//	LOGD("CGuiViewToolBox::SetPosition: %f %f %f %f", posX, posY, sizeX, sizeY);
//	
//	CGuiWindow::SetPosition(posX, posY, posZ, sizeX, sizeY);
//}

void CGuiViewToolBox::DoLogic()
{
	CGuiWindow::DoLogic();
}

void CGuiViewToolBox::Render()
{
	BlitFilledRectangle(posX, posY, posZ, sizeX, sizeY, backgroundColorR, backgroundColorG, backgroundColorB, backgroundColorA);

	CGuiWindow::Render();
	
	if (viewFrame)
	{
		BlitRectangle(posX, posY, posZ, sizeX, sizeY, 0, 0, 0, 1, 1);
	}
}

void CGuiViewToolBox::RenderButtons()
{
	for (std::vector<CGuiButton *>::iterator it = buttons.begin(); it != buttons.end(); it++)
	{
		CGuiButton *btn = *it;
		
		btn->Render();
	}
	
}

bool CGuiViewToolBox::DoTap(float x, float y)
{
	LOGG("CGuiViewToolBox::DoTap: %f %f", x, y);
	
	if (CGuiWindow::DoTap(x, y) == false)
	{
		if (this->IsInsideView(x, y))
		{
			return true;
		}
		
		return false;
	}
	
	return true;
}

//bool CGuiViewToolBox::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
//{
//	if (this->IsInsideView(x, y))
//		return true;
//	
//	return CGuiWindow::DoMove(x, y, distX, distY, diffX, diffY);
//}
//

bool CGuiViewToolBox::DoRightClick(float x, float y)
{
	LOGI("CGuiViewToolBox::DoRightClick: %f %f", x, y);
	
	if (this->IsInsideView(x, y))
		return true;
	
	return CGuiWindow::DoRightClick(x, y);
}


bool CGuiViewToolBox::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGD("CGuiViewToolBox::KeyDown: %d", keyCode);
	
	return false;
}

bool CGuiViewToolBox::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

void CGuiViewToolBox::ActivateView()
{
}

void CGuiViewToolBoxCallback::ToolBoxIconPressed(CSlrImage *imgIcon)
{
	LOGError("CGuiViewToolBoxCallback::ToolBoxIconPressed");
}

