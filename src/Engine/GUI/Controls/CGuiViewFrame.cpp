#include "SYS_Defs.h"
#include "SYS_Main.h"
#include "CGuiViewWindowsManager.h"
#include "CGuiViewFrame.h"
#include "CSlrString.h"
#include "CGuiMain.h"
#include "CGuiWindow.h"
#include "CGuiViewToolBox.h"
#include "RES_ResourceManager.h"

//CGuiViewFrame::CGuiViewFrame(CGuiView *view)
//: CGuiView(view->posX, view->posY, view->posZ, view->sizeX, view->sizeY)
//{
//	this->Initialize(view, NULL, GUI_FRAME_NO_TITLE);
//}
//

CGuiViewFrame::CGuiViewFrame(CGuiView *view, CSlrString *barTitle)
: CGuiView(view->posX, view->posY, view->posZ, view->sizeX, view->sizeY)
{
	this->Initialize(view, barTitle, GUI_FRAME_MODE_WINDOW);
}

CGuiViewFrame::CGuiViewFrame(CGuiView *view, CSlrString *barTitle, u32 mode)
: CGuiView(view->posX, view->posY, view->posZ, view->sizeX, view->sizeY)
{
	this->Initialize(view, barTitle, mode);
}

void CGuiViewFrame::Initialize(CGuiView *view, CSlrString *barTitle, u32 mode)
{
	if (barTitle != NULL)
	{
		// debug set name
		char *buf = SYS_GetCharBuf();
		char *t = barTitle->GetUTF8();
		sprintf(buf, "CGuiViewFrame (%s)", t);
		this->name = STRALLOC(buf);
		SYS_ReleaseCharBuf(buf);
		delete [] t;
	}
	else
	{
		this->name = "CGuiViewFrame";
	}

	//
	this->view = view;
	this->view->bringToFrontOnTap = true;
	this->bringToFrontOnTap = false;
	
	this->barTitle = NULL;
	this->viewFrameToolBox = NULL;
	
	this->SetPositionOffset(0, 0, 0);

	//
	barHeight = 13.0f;
	frameWidth = 5.0f;
	
	barColorR = 58.0f/255.0f;
	barColorG = 57.0f/255.0f;
	barColorB = 57.0f/255.0f;
	barColorA = 1.0f;
	
	barTextColorR = 147.0f/255.0f;
	barTextColorG = 147.0f/255.0f;
	barTextColorB = 147.0f/255.0f;
	barTextColorA = 1.0f;
	
	fontSize = 1.5f;
	
	barFont = guiMain->fntEngineDefault;
	
	
	if (IS_SET(mode, GUI_FRAME_HAS_CLOSE_BUTTON))
	{
		float iconCloseSize = 7.4f;
		imgIconClose = RES_GetImage("/gfx/icon_close", true);
		btnCloseWindow = new CGuiButton(imgIconClose, 0, 0, posZ, iconCloseSize, iconCloseSize, BUTTON_ALIGNED_CENTER, this);
		btnCloseWindow->name = "CGuiViewFrame::btnCloseWindow";
		
		//	pressed background: #23282C
		btnCloseWindow->buttonPressedColorR = 0.1373f;
		btnCloseWindow->buttonPressedColorG = 0.1569f;
		btnCloseWindow->buttonPressedColorB = 0.1725f;

		btnCloseWindow->userData = imgIconClose;
		btnCloseWindow->SetPositionOffset(this->sizeX - 6, (-barHeight+7), 0);
		this->AddGuiElement(btnCloseWindow);
	}
	else
	{
		imgIconClose = NULL;
		btnCloseWindow = NULL;
	}
	
	movingView = false;
	
	this->SetBarTitle(barTitle);

	this->SetPosition(this->posX, this->posY);
	this->view->SetPositionElements(this->posX, this->posY);
}

void CGuiViewFrame::UpdateSize()
{
	this->SetSize(view->sizeX, view->sizeY);
	this->view->SetPositionElements(this->posX, this->posY);
}

void CGuiViewFrame::SetSize(float sizeX, float sizeY)
{
	LOGG("CGuiViewFrame::SetSize: '%s'", this->view->name);
	CGuiView::SetSize(sizeX, sizeY);
	if (btnCloseWindow != NULL)
	{
		LOGG("CGuiViewFrame::SetSize: '%s' update close button, this->sizeX=%f", this->view->name, this->sizeX);
		btnCloseWindow->SetPositionOffset(this->sizeX - 7, (-barHeight+2), 0);
	}
}

void CGuiViewFrame::SetBarTitle(CSlrString *newBarTitle)
{
	guiMain->LockMutex();
	
	if (this->barTitle)
	{
		delete this->barTitle;
	}
	
	this->barTitle = newBarTitle;
	
	float barTitleHeight;
	barFont->GetTextSize(this->barTitle, fontSize, &barTitleWidth, &barTitleHeight);
	
	guiMain->UnlockMutex();
}

void CGuiViewFrame::AddBarToolBox(CGuiViewToolBoxCallback *callback)
{
	const float iconsGap = 8.0f;
	float f = 2.818f;
	
	float s = 7.2f;
	
	float s2 = s*f+3;
	
	this->viewFrameToolBox = new CGuiViewToolBox(0, 0, posZ,
											  iconsGap, 2.5f,
											  s * f, s,
											  s2, 0,
											  -1, NULL, callback);
	
	this->viewFrameToolBox->SetPositionOffset(barTitleWidth, -barHeight, 0);
	this->AddGuiElement(viewFrameToolBox);
}

void CGuiViewFrame::AddBarIcon(CSlrImage *imageIcon)
{
	viewFrameToolBox->AddIcon(imageIcon);
}

bool CGuiViewFrame::IsInside(float x, float y)
{
	if (x >= (view->posX - frameWidth) && x <= (view->posEndX + frameWidth)
		&& y >= (view->posY - barHeight) && y <= (view->posEndY + frameWidth))
	{
		return true;
	}
	
	return false;
}

bool CGuiViewFrame::DoTap(float x, float y)
{
	if (viewFrameToolBox)
	{
		if (viewFrameToolBox->DoTap(x, y))
		{
//			// bring to front
//			if (bringToFrontOnTap)
//			{
//				CGuiViewWindowsManager *viewManager = ((CGuiViewWindowsManager *)this->view->parent);
//				viewManager->BringToFront(this->view);
//			}
			
			return true;
		}
	}
	
	if (btnCloseWindow)
	{
		if (btnCloseWindow->DoTap(x, y))
		{
			return true;
		}
	}

	if (IsInsideSurroundingFrame(x, y))
	{
		movingView = true;
		
//		// bring to front
//		if (bringToFrontOnTap)
//		{
//			CGuiViewWindowsManager *viewManager = ((CGuiViewWindowsManager *)this->view->parent);
//			viewManager->BringToFront(this->view);
//		}
		
		return true;
	}
	
	return false;
}

bool CGuiViewFrame::DoRightClick(float x, float y)
{
	if (IsInsideSurroundingFrame(x, y))
	{
//		// bring to front
//		if (bringToFrontOnTap)
//		{
//			CGuiViewWindowsManager *viewManager = ((CGuiViewWindowsManager *)this->view->parent);
//			viewManager->BringToFront(this->view);
//		}
		return true;
	}
	
	return false;
}

bool CGuiViewFrame::DoFinishTap(float x, float y)
{
	LOGG("CGuiViewFrame::DoFinishTap()");
	
	if (btnCloseWindow)
	{
		btnCloseWindow->DoFinishTap(x, y);
	}
	
	if (movingView)
	{
		movingView = false;
		return true;
	}
	
	if (this->viewFrameToolBox)
	{
		if (this->viewFrameToolBox->DoFinishTap(x, y))
		{
			return true;
		}
	}
	
	if (IsInsideSurroundingFrame(x, y))
	{
		return true;
	}
	
	return false;
}

bool CGuiViewFrame::IsInsideSurroundingFrame(float x, float y)
{
	if (
			(
				x >= (view->posX - frameWidth) && x < (view->posX)
				&& y >= (view->posY - barHeight) && y < (view->posEndY + frameWidth)
				
			 )
			||
			(
				x >= (view->posEndX)           && x <= (view->posEndX + frameWidth)
				&& y >= (view->posY - barHeight) && y < (view->posEndY + frameWidth)
			 )
			||
			(
				x >= (view->posX - frameWidth) && x <= (view->posEndX + frameWidth)
				&& y >= (view->posEndY)           && y <= (view->posEndY + frameWidth)
			 )
			||
			(
				x >= (view->posX - frameWidth) && x <= (view->posEndX + frameWidth)
				&& y >= (view->posY - barHeight)  && y <= view->posY
				
			 )
		)
	{
		return true;
	}
	
	return false;
}

bool CGuiViewFrame::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	LOGD("CGuiViewFrame::DoMove: movingView=%d diffX=%f diffY=%f", movingView, diffX, diffY);
	
	if (IsInsideSurroundingFrame(x, y))
	{
		movingView = true;
	}
	
	if (movingView)
	{
//		LOGD(".... prev pos=%f %f new pos=%f %f", view->posX, view->posY, view->posX + diffX, view->posY + diffY);
		view->SetPosition(view->posX + diffX, view->posY + diffY);
//		this->viewFrameToolBox->SetPosition(view->posX + barTitleWidth, view->posY - barHeight);
		
//		// bring to front
//		if (bringToFrontOnTap)
//		{
//			CGuiViewWindowsManager *viewManager = ((CGuiViewWindowsManager *)this->view->parent);
//			viewManager->BringToFront(this->view);
//		}

		return true;
	}
	return false;
}

bool CGuiViewFrame::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	if (movingView)
	{
		movingView = false;
		return true;
	}
	
	return false;
}


bool CGuiViewFrame::DoDoubleTap(float x, float y)
{
	return false;
}
bool CGuiViewFrame::DoFinishDoubleTap(float posX, float posY)
{
	return false;	
}

void CGuiViewFrame::Render()
{
//	LOGG("CGuiViewFrame::Render: %s sizeX=%f", this->name, this->sizeX);
	
	BlitRectangle(view->posX, view->posY, view->posZ, view->sizeX, view->sizeY, barColorR, barColorG, barColorB, barColorA, frameWidth);
	
	BlitFilledRectangle(view->posX-frameWidth, view->posY-barHeight, view->posZ, view->sizeX+frameWidth*2.0f, barHeight, barColorR, barColorG, barColorB, barColorA);
	
	barFont->BlitTextColor(this->barTitle, view->posX, view->posY-barHeight+frameWidth*0.75f, view->posZ, fontSize,
						   barTextColorR, barTextColorG, barTextColorB, barTextColorA);

	BlitRectangle(view->posX-frameWidth, view->posY-barHeight, view->posZ, view->sizeX+frameWidth*2.0f, view->sizeY+barHeight+frameWidth, 0, 0, 0, 1, 1);
	
//	if (viewFrameToolBox)
//	{
//		viewFrameToolBox->RenderButtons();
//	}
	
	CGuiView::Render();
	
	//LOGD("movingView=%x", movingView);
}

void CGuiViewFrame::Render(float posX, float posY)
{
	
	LOGD("CGuiViewFrame::Render %f %f", posX, posY);

	BlitRectangle(posX, posY, this->posZ, this->sizeX, this->sizeY, 0.0f, 0.0f, 1.0f, 1.0f);

}

void CGuiViewFrame::DoLogic()
{
	// NO DO LOGIC
	LOGD("CGuiViewFrame::DoLogic: movingView=%x", movingView);
}

bool CGuiViewFrame::ButtonPressed(CGuiButton *button)
{
	if (button == btnCloseWindow)
	{
		LOGD("CGuiViewFrame::ButtonPressed: btnCloseWindow");
		
		CGuiWindow *window = (CGuiWindow *)this->view;
		window->WindowCloseButtonPressed();
		
	}
	
	return false;
}
