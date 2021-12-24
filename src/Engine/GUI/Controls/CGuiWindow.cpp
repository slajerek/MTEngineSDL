#include "CGuiWindow.h"
#include "CGuiViewWindowsManager.h"
#include "SYS_Main.h"
#include "RES_ResourceManager.h"
#include "CDataAdapter.h"
#include "CSlrString.h"
#include "SYS_KeyCodes.h"
#include "SYS_Threading.h"
#include "VID_ImageBinding.h"
#include <list>

CGuiWindow::CGuiWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
					   CSlrString *windowName)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	if (windowName != NULL)
	{
		this->Initialize(posX, posY, posZ, sizeX, sizeY, windowName, GUI_FRAME_MODE_WINDOW, NULL);
	}
	else
	{
		this->Initialize(posX, posY, posZ, sizeX, sizeY, windowName, GUI_FRAME_NO_FRAME, NULL);
	}
}

CGuiWindow::CGuiWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
					   CSlrString *windowName, CGuiWindowCallback *callback)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	if (windowName != NULL)
	{
		this->Initialize(posX, posY, posZ, sizeX, sizeY, windowName, GUI_FRAME_MODE_WINDOW, callback);
	}
	else
	{
		this->Initialize(posX, posY, posZ, sizeX, sizeY, windowName, GUI_FRAME_NO_FRAME, callback);
	}
}


CGuiWindow::CGuiWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
					   CSlrString *windowName, u32 mode, CGuiWindowCallback *callback)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	this->Initialize(posX, posY, posZ, sizeX, sizeY, windowName, mode, callback);
}

void CGuiWindow::Initialize(float posX, float posY, float posZ, float sizeX, float sizeY,
						CSlrString *windowName, u32 mode, CGuiWindowCallback *callback)
{
//	if (windowName != NULL)
//	{
//		char *buf = SYS_GetCharBuf();
//		char *bufName = windowName->GetStdASCII();
//		sprintf(buf, "CGuiWindow (%s)", bufName);
//		this->name = STRALLOC(buf);
//		delete [] bufName;
//		SYS_ReleaseCharBuf(buf);
//	}
//	else
//	{
//		this->name = STRALLOC("CGuiWindow");
//	}
	
	this->callback = callback;
	
	if (IS_SET(mode, GUI_FRAME_HAS_FRAME))
	{
		viewFrame = new CGuiViewFrame(this, windowName, mode);
		this->AddGuiElement(viewFrame);
	}
	else
	{
		viewFrame = NULL;
	}
}

void CGuiWindow::SetSize(float sizeX, float sizeY)
{
	CGuiView::SetSize(sizeX, sizeY);
	
	if (viewFrame)
	{
		this->viewFrame->SetSize(sizeX, sizeY);
	}
}

void CGuiWindow::SetPosition(float posX, float posY)
{
//	LOGD("CGuiWindow::SetPosition: %f %f (sizeX=%f sizeY=%f)", posX, posY, this->sizeX, this->sizeY);
	
	CGuiView::SetPosition(posX, posY);
}

void CGuiWindow::SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY)
{
//	LOGD("CGuiWindow::SetPosition: %f %f %f %f", posX, posY, sizeX, sizeY);
	
	CGuiView::SetPosition(posX, posY, posZ, sizeX, sizeY);
}

void CGuiWindow::DoLogic()
{
	CGuiView::DoLogic();
}

void CGuiWindow::Render()
{
//	BlitFilledRectangle(posX, posY, posZ, sizeX, sizeY, backgroundColorR, backgroundColorG, backgroundColorB, backgroundColorA);
	
	CGuiView::Render();
	
	if (viewFrame)
	{
		BlitRectangle(posX, posY, posZ, sizeX, sizeY, 0, 0, 0, 1, 1);
	}
}

bool CGuiWindow::DoTap(float x, float y)
{
	LOGG("CGuiWindow::DoTap: %f %f", x, y);
	
	if (viewFrame)
	{
		if (viewFrame->DoTap(x, y) == false)
		{
			if (CGuiView::DoTap(x, y) == false)
			{
				if (this->IsInsideView(x, y))
				{
					return true;
				}
			}
		}
	}
	
	return CGuiView::DoTap(x, y);;
}

bool CGuiWindow::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return CGuiView::DoMove(x, y, distX, distY, diffX, diffY);
}


bool CGuiWindow::DoRightClick(float x, float y)
{
	LOGI("CGuiWindow::DoRightClick: %f %f", x, y);
	
	return CGuiView::DoRightClick(x, y);
}


void CGuiWindow::ActivateView()
{
}

void CGuiWindow::RenderWindowBackground()
{
	BlitFilledRectangle(posX, posY, posZ, sizeX, sizeY, viewFrame->barColorR, viewFrame->barColorG, viewFrame->barColorB, 1);
}

void CGuiWindow::WindowCloseButtonPressed()
{
	LOGD("CGuiWindow::WindowCloseButtonPressed");
	
	if (this->callback)
	{
		if (this->callback->GuiWindowCallbackWindowClose(this))
		{
			return;
		}
	}
	
	if (this->parent)
	{
		CGuiViewWindowsManager *parentView = (CGuiViewWindowsManager*)this->parent;
		parentView->HideWindow(this);
	}
}

bool CGuiWindowCallback::GuiWindowCallbackWindowClose(CGuiWindow *window)
{
	// do not cancel close
	return false;
}

