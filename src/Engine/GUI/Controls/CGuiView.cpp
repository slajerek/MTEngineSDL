/*
 *  CGuiView.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CGuiView.h"
#include "VID_Main.h"
#include "CSlrString.h"
#include "CGuiMain.h"
#include "CGuiWindow.h"
#include "CLayoutParameter.h"
#include "CByteBuffer.h"

CGuiView::CGuiView(const char *name, float posX, float posY, float sizeX, float sizeY)
	: CGuiElement(posX, posY, -1, sizeX, sizeY)
{
	this->Init(name, posX, posY, 0, sizeX, sizeY);
}

CGuiView::CGuiView(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
	: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
	this->Init(name, posX, posY, posZ, sizeX, sizeY);
}

CGuiView::CGuiView(float posX, float posY, float posZ, float sizeX, float sizeY)
	: CGuiElement(posX, posY, posZ, sizeX, sizeY)
{
//	LOGD("Init generic view: %f %f %f %f", posX, posY, sizeX, sizeY);
	this->Init("CGuiView", posX, posY, posZ, sizeX, sizeY);
}

void CGuiView::Init(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
{
	this->name = name;
	this->previousZ = posZ;
	this->previousFrontZ = posZ;
		
	focusElement = NULL;
	
	consumeTapBackground = true;
	positionElementsOnFrameMove = true;
	
	imGuiWindow = NULL;
	imGuiWindowSkipFocusCheck = false;
	imGuiNoWindowPadding = false;
	imGuiNoScrollbar = false;
	imGuiSkipKeyPressWhenIoWantsTextInput = true;
	imGuiWindowKeepAspectRatio = false;
	imGuiWindowAspectRatio = 1.0f;

	isShowingContextMenu = false;
	
	imGuiForceThisFrameNewPosition = false;
	imGuiForceThisFrameNewSize = false;
	thisFrameNewPosX = -1; thisFrameNewPosY = -1;
	thisFrameNewSizeX = -1; thisFrameNewSizeY = -1;

	previousPosX = -1;
	previousPosY = -1;
	previousViewPosX = -1;
	previousViewPosY = -1;
	previousViewSizeX = -1;
	previousViewSizeY = -1;
	previousViewKeepAspectRatio = false;
	previousViewAspectRatio = 0.0f;
	previousViewIsDocked = false;
	
	SetWindowPositionAndSize(posX, posY, sizeX, sizeY);
	windowInnerRectPosX = posX;
	windowInnerRectPosY = posY;
	windowInnerRectSizeX = sizeX;
	windowInnterRectSizeY = sizeY;
	windowInnerRectPosEndX = posX + sizeX;
	windowInnterRectPosEndY = posY + sizeY;

	numNotHiddenLayoutParameters = 0;

	fullScreenSizeX = -1;
	fullScreenSizeY = -1;
	
	// this is to determine if layout parameters were rendered this frame, if yes then we'll skip render
	previousRenderedFrameWithLayoutParameters = 0;
}

bool CGuiView::IsInsideWindow(float x, float y)
{
	if (!this->visible)
		return false;
	
	if (x >= this->windowPosX && x <= this->windowPosEndX
		&& y >= this->windowPosY && y <= this->windowPosEndY)
	{
		return true;
	}
	
	return false;
}

bool CGuiView::IsResizingOrOutsideWindow(float x, float y)
{
	if (imGuiWindow)
	{
//		// resize from l/r/t/b borders, there's no corner
//		if (imGuiWindow->ResizeBorderHeld != -1)
//		{
//			LOGD("skip IsInside held %d", imGuiWindow->ResizeBorderHeld);
//			return false;
//		}
		// skip when resize l/r/t/b and from corners is active. TODO: this is a hacky way, can we do better?
		ImGuiContext *context = ImGui::GetCurrentContext();
		if (context->MouseCursor == ImGuiMouseCursor_ResizeNESW
			|| context->MouseCursor == ImGuiMouseCursor_ResizeNWSE
			|| context->MouseCursor == ImGuiMouseCursor_ResizeNS
			|| context->MouseCursor == ImGuiMouseCursor_ResizeEW
			|| context->MouseCursor == ImGuiMouseCursor_NotAllowed)
		{
//			LOGI("IsResizingOrOutsideWindow: skip, IsInside corner");
			return true;
		}
		
		if (guiMain->FindTopWindow(x, y) != this)
			return true;
	}

	return false;
}

bool CGuiView::IsInsideWindowInnerRect(float x, float y)
{
//	LOGD("IsInsideWindowInnerRect: visible");
	if (!this->visible)
		return false;

//	LOGD("IsInsideWindowInnerRect: IsResizingOrOutsideWindow");
	if (IsResizingOrOutsideWindow(x, y))
		return false;

//	LOGD("IsInsideWindowInnerRect: check position");
	if (x >= this->windowInnerRectPosX && x <= this->windowInnerRectPosEndX
		&& y >= this->windowInnerRectPosY && y <= this->windowInnterRectPosEndY)
	{
		return true;
	}
	
	return false;
}

bool CGuiView::IsInside(float x, float y)
{
	if (!this->visible)
		return false;
	
	if (x >= this->posX && x <= this->posEndX
		&& y >= this->posY && y <= this->posEndY)
	{
		return true;
	}
	
	return false;
}

bool CGuiView::IsInsideView(float x, float y)
{
//	LOGI("IsInsideView: visible");
	if (!this->visible)
		return false;
	
//	LOGI("IsInsideView: resizing");
	if (IsResizingOrOutsideWindow(x, y))
		return false;

//	LOGI("IsInsideView: IsInsideViewNonVisible");
	return this->IsInsideViewNonVisible(x, y);
}


bool CGuiView::IsInsideViewNonVisible(float x, float y)
{
	float o = 2.0f;
		
	if (x >= this->posX+o && x <= this->posEndX-o
		&& y >= this->posY+o && y <= this->posEndY-o)
	{
		return true;
	}
	
	return false;
}

void CGuiView::SetPosition(float posX, float posY)
{
	CGuiElement::SetPosition(posX, posY);
}
void CGuiView::SetPosition(float posX, float posY, float posZ)
{
	CGuiElement::SetPosition(posX, posY, posZ);
}

void CGuiView::SetSize(float sizeX, float sizeY)
{
	CGuiElement::SetSize(sizeX, sizeY);
}

void CGuiView::SetPosition(float posX, float posY, float sizeX, float sizeY)
{
	CGuiElement::SetPosition(posX, posY, posZ, sizeX, sizeY);
	
	if (positionElementsOnFrameMove)
	{
		this->SetPositionElements(posX, posY);
	}
}

void CGuiView::SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY)
{
	CGuiElement::SetPosition(posX, posY, posZ, sizeX, sizeY);
	
	if (positionElementsOnFrameMove)
	{
		this->SetPositionElements(posX, posY);
	}
}

void CGuiView::SetWindowPositionAndSize(float posX, float posY, float sizeX, float sizeY)
{
	this->windowPosX = posX;
	this->windowPosY = posY;
	this->windowSizeX = sizeX;
	this->windowSizeY = sizeY;
	this->windowPosEndX = windowPosX + windowSizeX;
	this->windowPosEndY = windowPosY + windowSizeY;
}

// iterate over all elements and move them accordingly, to be called by CGuiViewFrame when window is moved
// TODO: change window frame movements to use CGuiAnimation and glTranslate
void CGuiView::SetPositionElements(float posX, float posY)
{
//	LOGG("CGuiView::SetPositionElements: px=%f py=%f", posX, posY);
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *pElement = (*enumGuiElems).second;
		
//		LOGG("pElement '%s' offset=%f %f", pElement->name, pElement->offsetPosX, pElement->offsetPosY);
		pElement->SetPosition(this->posX + pElement->offsetPosX, this->posY + pElement->offsetPosY);
	}
	
}

void CGuiView::PositionCenterOnParentView()
{
	float parentX, parentY;
	if (this->parent)
	{
		parentX = this->parent->posX;
		parentY = this->parent->posY;
	}
	else
	{
		parentX = SCREEN_WIDTH/2.0f;
		parentY = SCREEN_HEIGHT/2.0f;
		
	}
	
	this->SetPosition(parentX - this->sizeX/2.0f, parentY - this->sizeY/2.0f);
}

void CGuiView::SetNewImGuiWindowPosition(float newPosX, float newPosY)
{
	this->thisFrameNewPosX = newPosX;
	this->thisFrameNewPosY = newPosY;
	this->imGuiForceThisFrameNewPosition = true;
}

void CGuiView::SetNewImGuiWindowSize(float newSizeX, float newSizeY)
{
	LOGD("CGuiView::SetNewImGuiWindowSize: %f %f", newSizeX, newSizeY);
	this->thisFrameNewSizeX = newSizeX;
	this->thisFrameNewSizeY = newSizeY;
	this->imGuiForceThisFrameNewSize = true;
}

void CGuiView::CenterImGuiWindowPosition()
{
	CenterImGuiWindowPosition(0.5f, 0.5f);
}

void CGuiView::CenterImGuiWindowPosition(float offsetFactorX, float offsetFactorY)
{
	ImVec2 vc;
	if (VID_IsViewportsEnable())
	{
		ImGuiPlatformIO platformIO = ImGui::GetPlatformIO();
		vc = platformIO.Monitors[0].MainSize;
		vc.x = vc.x * offsetFactorX;
		vc.y = vc.y * offsetFactorY;
	}
	else
	{
		vc = ImGui::GetMainViewport()->Pos;
		vc.x += ImGui::GetMainViewport()->Size.x * offsetFactorX;
		vc.y += ImGui::GetMainViewport()->Size.y * offsetFactorY;
	}
	
	vc.x -= this->sizeX*0.5f;
	vc.y -= this->sizeY*0.5f;
	SetNewImGuiWindowPosition(vc.x, vc.y);
}

void CGuiView::RemoveGuiElements()
{
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *pElement = (*enumGuiElems).second;
		pElement->parent = NULL;
	}

	guiElementsUpwards.clear();
	guiElementsDownwards.clear();
}

void CGuiView::RemoveGuiElement(CGuiElement *guiElement)
{
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *pElement = (*enumGuiElems).second;
		
		if (pElement == guiElement)
		{
			this->guiElementsUpwards.erase(enumGuiElems);
			break;
		}
	}

	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *pElement = (*enumGuiElems).second;
		
		if (pElement == guiElement)
		{
			this->guiElementsDownwards.erase(enumGuiElems);
			break;
		}
	}
	
	guiElement->parent = NULL;
}


void CGuiView::AddGuiElement(CGuiElement *guiElement, float z)
{
#ifndef RELEASE
	// sanity check
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *pElement = (*enumGuiElems).second;

		if (pElement == guiElement)
		{
			SYS_FatalExit("'%s' is already in the view '%s'", guiElement->name, this->name);
		}
	}
#endif

	//map<int, CObjectInfo *>::iterator objDataIt = detectedObjects.find(val);
	this->guiElementsUpwards[z] = guiElement;
	this->guiElementsDownwards[z] = guiElement;
//	this->previousZ = z;
	
	if (previousFrontZ < z)
		previousFrontZ = z;
	
	guiElement->parent = this;
}

void CGuiView::AddGuiElement(CGuiElement *guiElement)
{
#ifndef RELEASE
	// sanity check
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *pElement = (*enumGuiElems).second;

		if (pElement == guiElement)
		{
			SYS_FatalExit("'%s' is already in the view '%s'", guiElement->name, this->name);
		}
	}
#endif

	//map<int, CObjectInfo *>::iterator objDataIt = detectedObjects.find(val);

	this->previousZ += 0.001;
	this->guiElementsUpwards[previousZ] = guiElement;
	this->guiElementsDownwards[previousZ] = guiElement;
	
	guiElement->parent = this;

}

void CGuiView::BringToFront(CGuiElement *guiElement)
{
	RemoveGuiElement(guiElement);	
	AddGuiElement(guiElement, previousFrontZ + 0.01f);
}

void CGuiView::DoLogic()
{
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;

		if (!guiElement->visible)
			continue;

		guiElement->DoLogic();
	}
}


//@returns is consumed
bool CGuiView::DoTap(float x, float y)
{
	//LOGG("CGuiView::DoTap: '%s' x=%f y=%f", this->name, x, y);
	if (CGuiView::DoTapNoBackground(x, y))
		return true;

	//LOGG("done");
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

bool CGuiView::DoTapNoBackground(float x, float y)
{
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		LOGG("CGuiView::DoTap: %s", guiElement->name);
		if (!guiElement->visible)
			continue;

		if (guiElement->IsFocusableElement() && guiElement->IsInside(x, y))
		{
			SetFocusElement(guiElement);
		}
		
		if (guiElement->DoTap(x, y))
		{
			if (focusElement != guiElement)
			{
				ClearFocusElement();
			}
			
			if (guiElement->bringToFrontOnTap)
			{
				guiMain->LockMutex(); //"CGuiView::DoTapNoBackground");
				this->BringToFront(guiElement);
				guiMain->UnlockMutex(); //"CGuiView::DoTapNoBackground");
			}
			return true;
		}
	}

	if (focusElement && !focusElement->IsInside(x, y))
	{
		ClearFocusElement();
	}
	
	return false;
}


bool CGuiView::DoFinishTap(float x, float y)
{
	LOGG("CGuiView::DoFinishTap: %s %f %f", name, x, y);

	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		LOGG("CGuiView::DoFinishTap: guiElement=%s", guiElement->name);

		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoFinishTap(x, y))
			return true;
	}

	if (consumeTapBackground)
	{
		if (IsInsideView(x, y))
			return true;
	}

	return false;
}

//@returns is consumed
bool CGuiView::DoDoubleTap(float x, float y)
{
	LOGG("CGuiView::DoDoubleTap:  x=%f y=%f", x, y);

	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;

		if (guiElement->DoDoubleTap(x, y))
			return true;
	}

	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

bool CGuiView::DoFinishDoubleTap(float x, float y)
{
	LOGG("CGuiView::DoFinishTap: %f %f", x, y);

	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;

		if (guiElement->DoFinishDoubleTap(x, y))
			return true;
	}

	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}


bool CGuiView::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	LOGG("CGuiView::DoMove: this is '%s'", this->name);
	
	//	LOGG("--- DoMove ---");
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;

		LOGG("DoMove %f: %s", (*enumGuiElems).first, guiElement->name);
		
		if (!guiElement->visible)
			continue;
		
		volatile bool consumed = guiElement->DoMove(x, y, distX, distY, diffX, diffY);
		
		LOGG("   consumed=%d", consumed);
		if (consumed)
			return true;
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

bool CGuiView::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	//viewScoreTracks->FinishMove(x, y, distX, distY, accelerationX, accelerationY);
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->FinishMove(x, y, distX, distY, accelerationX, accelerationY))
			return true;
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

//@returns is consumed
bool CGuiView::DoRightClick(float x, float y)
{
	LOGG("CGuiView::DoRightClick: '%s' x=%f y=%f", this->name, x, y);
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		
		LOGG("CGuiView::DoRightClick: %s visible=%d", guiElement->name, guiElement->visible);
		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoRightClick(x, y))
		{
			if (guiElement->bringToFrontOnTap)
			{
				guiMain->LockMutex(); //"CGuiView::DoRightClick");
				this->BringToFront(guiElement);
				guiMain->UnlockMutex(); //"CGuiView::DoRightClick");
			}
			
			return true;
		}
			
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}


bool CGuiView::DoFinishRightClick(float x, float y)
{
	//LOGG("CGuiView::DoFinishRightClick: %f %f", x, y);
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoFinishRightClick(x, y))
			return true;
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

bool CGuiView::DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
//	LOGD("--- DoRightClickMove ---: %s", this->name);

	/*
	if (this->IsInside(x, y))
	{
		mousePosX = x;
		mousePosY = y;
	}
	else
	{
		mousePosX = -1;
		mousePosY = -1;
	}*/
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		//LOGG("DoRightClickMove %f: %s", (*enumGuiElems).first, guiElement->name);
		
		bool consumed = guiElement->DoRightClickMove(x, y, distX, distY, diffX, diffY);
		//LOGG("   consumed=%d", consumed);
		if (consumed)
			return true;
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

bool CGuiView::FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->FinishRightClickMove(x, y, distX, distY, accelerationX, accelerationY))
			return true;
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}


bool CGuiView::DoNotTouchedMove(float x, float y)
{
	//	LOGG("--- DoNotTouchedMove ---");
	/*
	if (this->IsInside(x, y))
	{
		mousePosX = x;
		mousePosY = y;
	}
	else
	{
		mousePosX = -1;
		mousePosY = -1;
	}
	 */
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		//LOGG("DoNotTouchedMove %f: %s", (*enumGuiElems).first, guiElement->name);
		
		//bool consumed =
		guiElement->DoNotTouchedMove(x, y);
		
		//LOGG("   consumed=%d", consumed);
		// arrrgh! this is a bug... not touched move event does not need to be 'consumed'. it is for tracking mouse!
		// TODO: refactor DoNotTouchedMove to not return anything (void).
//		if (consumed)
//			return true;
	}
	
	return false;
}

bool CGuiView::InitZoom()
{
	//viewScoreTracks->InitZoom();

	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;

		if (guiElement->InitZoom())
			return true;
	}

	if (consumeTapBackground)
	{
		if (IsInside(guiMain->mousePosX, guiMain->mousePosY))
			return true;
	}

	return false;
}

bool CGuiView::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;

		if (guiElement->DoZoomBy(x, y, zoomValue, difference))
			return true;
	}

	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

bool CGuiView::DoScrollWheel(float deltaX, float deltaY)
{
	LOGG("CGuiView::DoScrollWheel: deltaX=%f deltaY=%f", deltaX, deltaY);

	if (!this->IsInsideView(guiMain->mousePosX, guiMain->mousePosY))
	{
		if (consumeTapBackground)
		{
			if (IsInside(guiMain->mousePosX, guiMain->mousePosY))
				return true;
		}
		return false;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->IsInside(guiMain->mousePosX, guiMain->mousePosY))
		{
			if (guiElement->DoScrollWheel(deltaX, deltaY))
				return true;
		}
		
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(guiMain->mousePosX, guiMain->mousePosY))
			return true;
	}

	return false;
}

bool CGuiView::DoMultiTapNoBackground(COneTouchData *touch, float x, float y)
{
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		//LOGG("CGuiView::DoTap: %s", guiElement->name);
		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoMultiTap(touch, x, y))
			return true;
	}
	
	return false;
}

//@returns is consumed
bool CGuiView::DoMultiTap(COneTouchData *touch, float x, float y)
{
	//LOGD("CGuiView::DoMultiTap: '%s' x=%f y=%f", this->name, x, y);
	if (CGuiView::DoMultiTapNoBackground(touch, x, y))
		return true;
	
	//LOGD("done");
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

bool CGuiView::DoMultiMove(COneTouchData *touch, float x, float y)
{
	//viewScoreTracks->DoMove(x, y, distX, distY, diffX, diffY);
	
	//	LOGG("--- DoMove ---");
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		//LOGG("DoMove %f: %s", (*enumGuiElems).first, guiElement->name);
		
		bool consumed = guiElement->DoMultiMove(touch, x, y); //, distX, distY, diffX, diffY);
		//LOGG("   consumed=%d", consumed);
		if (consumed)
			return true;
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;

}

bool CGuiView::DoMultiFinishTap(COneTouchData *touch, float x, float y)
{
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoMultiFinishTap(touch, x, y))
			return true;
	}
	
	if (consumeTapBackground)
	{
		if (IsInside(x, y))
			return true;
	}

	return false;
}

//void CGuiView::FinishTouches()
//{
//	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
//		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
//	{
//		CGuiElement *guiElement = (*enumGuiElems).second;
//		if (!guiElement->visible)
//			continue;
//
//		guiElement->FinishTouches();
//	}
//}

bool CGuiView::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CGuiView::KeyDown: %s %d %s %s %s", name ? name : "CGuiView", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	if (focusElement && focusElement->visible)
	{
		LOGI("...%s KeyDown focusElement=%s", name ? name : "CGuiView", focusElement->name);
		if (focusElement->KeyDown(keyCode, isShift, isAlt, isControl, isSuper))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->KeyDown(keyCode, isShift, isAlt, isControl, isSuper))
		{
			return true;
		}
	}
	
	if (this->KeyPressed(keyCode, isShift, isAlt, isControl, isSuper))
	{
		return true;
	}
	
	return false;
}

bool CGuiView::PostKeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CGuiView::PostKeyDown: %s %d %s %s %s", name ? name : "CGuiView", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	if (focusElement && focusElement->visible)
	{
		LOGI("...%s PostKeyDown focusElement=%s", name ? name : "CGuiView", focusElement->name);
		if (focusElement->KeyDown(keyCode, isShift, isAlt, isControl, isSuper))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->PostKeyDown(keyCode, isShift, isAlt, isControl, isSuper))
		{
			return true;
		}
	}

	return false;
}

bool CGuiView::KeyDownOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CGuiView::KeyDownOnMouseHover: %s %d %s %s %s", name ? name : "CGuiView", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
	
		if (guiElement->IsInside(guiMain->mousePosX, guiMain->mousePosY))
		{
			if (guiElement->KeyDownOnMouseHover(keyCode, isShift, isAlt, isControl, isSuper))
			{
				return true;
			}
		}
	}
	
	return false;
}

bool CGuiView::KeyDownGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CGuiView::KeyDownGlobal: %s %d %s %s %s", name ? name : "CGuiView", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		guiElement->KeyDownGlobal(keyCode, isShift, isAlt, isControl, isSuper);
	}
	
	return false;
}

bool CGuiView::KeyDownRepeat(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CGuiView::KeyDownRepeat: %s %d %s %s %s", name ? name : "CGuiView", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	if (focusElement && focusElement->visible)
	{
		LOGI("...%s KeyDownRepeat focusElement=%s", name ? name : "CGuiView", focusElement->name);
		if (focusElement->KeyDownRepeat(keyCode, isShift, isAlt, isControl, isSuper))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->KeyDownRepeat(keyCode, isShift, isAlt, isControl, isSuper))
		{
			return true;
		}
	}
	
	if (this->KeyPressed(keyCode, isShift, isAlt, isControl, isSuper))
	{
		return true;
	}
	
	return false;
}

bool CGuiView::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	if (focusElement && focusElement->visible)
	{
		if (focusElement->KeyUp(keyCode, isShift, isAlt, isControl, isSuper))
			return true;
	}

	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->KeyUp(keyCode, isShift, isAlt, isControl, isSuper))
		{
			return true;
		}
	}
	
	return false;
}

bool CGuiView::KeyUpOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->IsInside(guiMain->mousePosX, guiMain->mousePosY))
		{
			if (guiElement->KeyUpOnMouseHover(keyCode, isShift, isAlt, isControl, isSuper))
			{
				return true;
			}
		}
	}
	
	return false;
}

bool CGuiView::KeyUpGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		guiElement->KeyUpGlobal(keyCode, isShift, isAlt, isControl, isSuper);
	}
	
	return false;
}

bool CGuiView::KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	if (focusElement && focusElement->visible)
	{
		if (focusElement->KeyPressed(keyCode, isShift, isAlt, isControl, isSuper))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->KeyPressed(keyCode, isShift, isAlt, isControl, isSuper))
		{
			return true;
		}
	}
	
	return false;
}

// utf text input entered
bool CGuiView::KeyTextInput(const char *text)
{
	if (focusElement && focusElement->visible)
	{
		if (focusElement->KeyTextInput(text))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->KeyTextInput(text))
		{
			return true;
		}
	}
	
	return false;
}

// utf text input entered
bool CGuiView::KeyTextInputOnMouseHover(const char *text)
{
	LOGI("CGuiView::KeyTextInputOnMouseHover: %s '%s'", name ? name : "CGuiView", text);
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
	
		if (guiElement->IsInside(guiMain->mousePosX, guiMain->mousePosY))
		{
			if (guiElement->KeyTextInputOnMouseHover(text))
			{
				return true;
			}
		}
	}
	
	return false;
}

//virtual bool ;
//   virtual bool (CGamePad *gamePad, u8 button);
//   virtual bool (CGamePad *gamePad, u8 axis, int value);

bool CGuiView::DoGamePadButtonDown(CGamePad *gamePad, u8 button)
{
	LOGI("CGuiView::DoGamePadButtonDown: %d", button);
	if (focusElement && focusElement->visible)
	{
		LOGI("...focusElement=%s", focusElement->name);
		if (focusElement->DoGamePadButtonDown(gamePad, button))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoGamePadButtonDown(gamePad, button))
		{
			return true;
		}
	}
	
	return false;
}

bool CGuiView::DoGamePadButtonUp(CGamePad *gamePad, u8 button)
{
	LOGI("CGuiView::DoGamePadButtonUp: %d", button);
	if (focusElement && focusElement->visible)
	{
		LOGI("...focusElement=%s", focusElement->name);
		if (focusElement->DoGamePadButtonUp(gamePad, button))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoGamePadButtonUp(gamePad, button))
		{
			return true;
		}
	}
	
	return false;
}

bool CGuiView::DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value)
{
//	LOGI("CGuiView::DoGamePadAxisMotion: %d %d", axis, value);
	if (focusElement && focusElement->visible)
	{
//		LOGI("...focusElement=%s", focusElement->name);
		if (focusElement->DoGamePadAxisMotion(gamePad, axis, value))
			return true;
	}
	
	for (std::map<float, CGuiElement *, compareZdownwards>::iterator enumGuiElems = guiElementsDownwards.begin();
		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		if (!guiElement->visible)
			continue;
		
		if (guiElement->DoGamePadAxisMotion(gamePad, axis, value))
		{
			return true;
		}
	}
	
	return false;
}

bool CGuiView::DoGamePadAxisMotionButtonDown(CGamePad *gamePad, u8 button)
{
	return DoGamePadButtonDown(gamePad, button);
}

bool CGuiView::DoGamePadAxisMotionButtonUp(CGamePad *gamePad, u8 button)
{
	return DoGamePadButtonUp(gamePad, button);
}


void CGuiView::ActivateView()
{
	LOGG("CGuiView::ActivateView()");
}

void CGuiView::DeactivateView()
{
	LOGG("CGuiView::DeactivateView()");
}

void CGuiView::RenderImGui()
{
//#define LOGGUIVIEW
//#define MEASURETIME

#if defined(LOGGUIVIEW)
	LOGD("-------------- CGuiView::RenderImGui() --------------");
	LOGD("view=%s", this->name);
#endif
	
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		
		if ( (!guiElement->visible) || (guiElement->manualRender == true) )
			continue;
		
#if defined(LOGGUIVIEW)
		LOGD("....renderImGui %f: %s", (*enumGuiElems).first, guiElement->name);
#endif
		
		
		guiElement->RenderImGui();
		
		
#if defined(LOGGUIVIEW)
		LOGD("....renderImGui done %f: %s", (*enumGuiElems).first, guiElement->name);
#endif
		
	}
}

void CGuiView::SetFullScreenViewSize(float sx, float sy)
{
	this->fullScreenSizeX = sx;
	this->fullScreenSizeY = sy;
}

void CGuiView::CalcAspectRatio(float aspectRatio, ImVec2 viewportSize, ImVec2 *adjustedPos, ImVec2 *adjustedSize)
{
	float vW = (float) viewportSize.x;
	float vH = (float) viewportSize.y;
	float A = (float) aspectRatio;
	float vA = (vW / vH);

	if (A > vA)
	{
		adjustedPos->x = 0;
		adjustedPos->y = (vH * 0.5) - ((vW / A) * 0.5);
		adjustedSize->x = vW;
		adjustedSize->y = (vW / A);
	}
	else
	{
		if (A < vA)
		{
			adjustedPos->x = (vW * 0.5) - ((vH * A) * 0.5);
			adjustedPos->y = 0;
			adjustedSize->x = (vH * A);
			adjustedSize->y = vH;
		}
		else
		{
			adjustedPos->x = 0;
			adjustedPos->y = 0;
			adjustedSize->x = vW;
			adjustedSize->y = vH;
		}
	}
}

bool CGuiView::IsDocked()
{
	if (!imGuiWindow)
		return false;
	
	return imGuiWindow->DockNode && imGuiWindow->DockNode->HostWindow;
}

// is view hidden by OS, i.e. minimized?
bool CGuiView::IsHidden()
{
	return guiMain->IsViewHidden(this);
}

bool CGuiView::IsVisible()
{
	if (!IsHidden())
	{
		return this->visible;
	}
	return false;
}

void CGuiView::PreRenderImGui()
{
	LOGG("CGuiView::PreRenderImGui: %x %s", this, this->name);
	ImGuiContext& g = *GImGui;
		
	bool isFullScreen = (guiMain->viewFullScreen == this);
	
	if (isFullScreen)
	{
		// fullscreen
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImVec2 realFullScreenPos;
		ImVec2 realFullScreenSize;
		float aspectRatio = (float) imGuiWindowKeepAspectRatio ? imGuiWindowAspectRatio : this->fullScreenSizeX / (float) this->fullScreenSizeY;
		CalcAspectRatio(aspectRatio, viewport->WorkSize, &realFullScreenPos, &realFullScreenSize);
		
		ImGui::SetNextWindowPos(realFullScreenPos);
		ImGui::SetNextWindowSize(realFullScreenSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	}
	else
	{
		// not fullscreen, setup parameters of this window
		if (imGuiForceThisFrameNewPosition)
		{
			// force new position
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos + ImVec2(this->thisFrameNewPosX, this->thisFrameNewPosY), ImGuiCond_Always);
			imGuiForceThisFrameNewPosition = false;
		}
		else
		{
			// startup window position
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos + ImVec2(this->posX, this->posY), ImGuiCond_FirstUseEver);
		}
		
		if (imGuiForceThisFrameNewSize)
		{
			// force new size
			ImGui::SetNextWindowSize(ImVec2(this->thisFrameNewSizeX, this->thisFrameNewSizeY), ImGuiCond_Always);
			imGuiForceThisFrameNewSize = false;
		}
		else
		{
			// startup window size
			ImGui::SetNextWindowSize(ImVec2(this->sizeX, this->sizeY), ImGuiCond_FirstUseEver);
		}
				
		// should we keep aspect ratio?
		// Note, below is workaround for imGuiWindowKeepAspectRatio bug. without this makes the window resize/wobbly during move
//		LOGD("imGuiWindowKeepAspectRatio: name=%s", this->name);
		
		if (imGuiWindowKeepAspectRatio)
		{
			// window is being moved?
			if (previousPosX != this->posX || previousPosY != this->posY
				|| guiMain->layoutJustRestored)
			{
				// do not set constraint on window move
				previousPosX = this->posX;
				previousPosY = this->posY;
			}
			else
			{
				// Note, when window is docked the constraint does not work
				ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, FLT_MAX),
													ImGuiWindowSizeConstraints::KeepContentAspect, (void*)&imGuiWindowAspectRatio);
			}
		}
	}

	if (imGuiNoWindowPadding)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
	}

	if (imGuiNoScrollbar)
	{
		ImGui::Begin(this->name, &(this->visible), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
					 | (isFullScreen ? ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize : 0));
	}
	else
	{
		ImGui::Begin(this->name, &(this->visible), (isFullScreen ? ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize : 0));
	}

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	
	// remember me
	window->userData = this;
	this->imGuiWindow = window;

	// InnerRect is window size without decorations and scrollbars
	float sx = window->InnerRect.GetSize().x-1;
	float sy = window->InnerRect.GetSize().y-1;
	const float offsetY = 0.0f;	// TODO: take from theme
	
	bool isDocked = IsDocked();
	if (previousViewIsDocked != isDocked
		|| fabs(sx - previousViewSizeX) > 0.0f
		|| fabs(sy - previousViewSizeY) > 0.0f
		|| fabs(window->InnerRect.Min.x - previousViewPosX) > 0.0f
		|| fabs(window->InnerRect.Min.y - previousViewPosY) > 0.0f
		|| previousViewKeepAspectRatio != imGuiWindowKeepAspectRatio
		|| previousViewAspectRatio != imGuiWindowAspectRatio)
	{
		if (imGuiWindowKeepAspectRatio && isDocked)
		{
			// view should keep aspect and is docked, add black bars around
			ImVec2 viewportSize(sx, sy);
			ImVec2 viewScreenPos;
			ImVec2 viewScreenSize;
			CalcAspectRatio(imGuiWindowAspectRatio, viewportSize, &viewScreenPos, &viewScreenSize);

//			LOGD("view %s is docked and should keep aspect", this->name);
			viewScreenPos.x += window->InnerRect.Min.x;
			viewScreenPos.y += window->InnerRect.Min.y+offsetY;
			this->SetPosition(viewScreenPos.x, viewScreenPos.y, -1, viewScreenSize.x, viewScreenSize.y);
		}
		else
		{
			this->SetPosition(window->InnerRect.Min.x, window->InnerRect.Min.y+offsetY, -1, sx, sy);
		}
		this->SetWindowPositionAndSize(window->Pos.x, window->Pos.y, window->Size.x, window->Size.y);
		
		windowInnerRectPosX = window->InnerRect.Min.x;
		windowInnerRectPosY = window->InnerRect.Min.y;
		windowInnerRectSizeX = window->InnerRect.Max.x - window->InnerRect.Min.x;
		windowInnterRectSizeY = window->InnerRect.Max.y - window->InnerRect.Min.y;
		windowInnerRectPosEndX = window->InnerRect.Max.x;
		windowInnterRectPosEndY = window->InnerRect.Max.y;

		previousViewSizeX = sx;
		previousViewSizeY = sy;
		previousViewPosX = window->InnerRect.Min.x;
		previousViewPosY = window->InnerRect.Min.y;
		previousViewKeepAspectRatio = imGuiWindowKeepAspectRatio;
		previousViewAspectRatio = imGuiWindowAspectRatio;
		previousViewIsDocked = isDocked;
	}

//	LOGD("%s window->Pos.x=%5.2f window->Pos.y=%5.2f size=%5.2f %5.2f", this->name, window->Pos.x, window->Pos.y, window->Size.x, window->Size.y);
//	LOGD("%s window->InnerRect.x=%5.2f window->InnerRect.y=%5.2f size=%5.2f %5.2f", this->name, window->InnerRect.Min.x, window->InnerRect.Min.y, sx, sy);

	if (imGuiWindowSkipFocusCheck == false)
	{
		LOGG("check focus %s", this->name);
		if (ImGui::IsWindowFocused())
		{
			LOGG("..ImGui::IsWindowFocused %s", this->name);
			guiMain->SetInternalViewFocus(this);
		}
	}
}

void CGuiView::PostRenderImGui()
{
	if (imGuiNoWindowPadding)
	{
		ImGui::PopStyleVar();
	}

	if (isShowingContextMenu || HasContextMenuItems() || numNotHiddenLayoutParameters > 0)
	{
		if (ImGui::BeginPopupContextWindow("CGuiViewContextMenu"))
		{
			RenderContextMenuItems();
			RenderContextMenuLayoutParameters(false);
			ImGui::EndPopup();
		}
		else
		{
			isShowingContextMenu = false;
		}
	}
		
	ImGui::End();
	
	bool isFullScreen = (guiMain->viewFullScreen != NULL);

	if (isFullScreen)
	{
		ImGui::PopStyleVar(2);
	}
}

bool CGuiView::HasContextMenuItems()
{
	return false;
}

void CGuiView::RenderContextMenuItems()
{
}

// forced will always render, not forced will skip render if parameters were already rendered this frame
void CGuiView::RenderContextMenuLayoutParameters(bool forced)
{
	u64 currentFrame = VID_GetCurrentFrameNumber();
	if (!forced && (previousRenderedFrameWithLayoutParameters == currentFrame))
	{
		// skip rendering if
		return;
	}
	for (std::list<CLayoutParameter *>::iterator it = layoutParameters.begin(); it != layoutParameters.end(); it++)
	{
		CLayoutParameter *param = *it;
		if (param->isHidden)
			continue;
		
		if (param->RenderImGui())
		{
			this->LayoutParameterChanged(param);
		}
	}
	previousRenderedFrameWithLayoutParameters = currentFrame;
}

void CGuiView::Render()
{
//#define LOGGUIVIEW
//#define MEASURETIME
	
	
#if defined(LOGGUIVIEW)
	LOGD("-------------- CGuiView::Render() --------------");
	LOGD("view=%s", this->name);
#endif

	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;

		if ( (!guiElement->visible) || (guiElement->manualRender == true) )
			continue;

#if defined(LOGGUIVIEW)
		LOGD("....render %f: %s", (*enumGuiElems).first, guiElement->name);
#endif

#if defined(MEASURETIME) || defined(LOGGUIVIEW)
		long t1 = SYS_GetCurrentTimeInMillis();
#endif
		
		guiElement->Render();

#if defined(MEASURETIME) || defined(LOGGUIVIEW)
		long t2 = SYS_GetCurrentTimeInMillis();
#endif
		
#if defined(LOGGUIVIEW)
		LOGD("....render done %f: %s time=%d", (*enumGuiElems).first, guiElement->name, t2-t1);
#endif

#if defined(MEASURETIME)
		if (t2-t1 > 10)
		{
			LOGError("rendering %s took %dms", guiElement->name, t2-t1);
		}
#endif
		
	}
}

void CGuiView::Render(float posX, float posY)
{
	//LOGG("-------------- CGuiView::Render(posX=%f, posY=%f) --------------", posX, posY);
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;

		if (!guiElement->visible)
			continue;

		//LOGD("render %f: %s posX=%f posY=%f", (*enumGuiElems).first, guiElement->name, posX, posY);
		guiElement->Render(guiElement->posX + posX, guiElement->posY + posY);
	}
}

/*
void CGuiView::Render(float posX, float posY, float sizeX, float sizeY)
{
	//	LOGG("-------------- CGuiView::Render(posX=%f, posY=%f) --------------", posX, posY);
	for (std::map<float, CGuiElement *>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;

		if (!guiElement->visible)
			continue;

		//LOGG("render %f: %s posX=%f posY=%f", (*enumGuiElems).first, guiElement->name, posX, posY);
		guiElement->Render(guiElement->posX + posX, guiElement->posY + posY, sizeX, sizeY);
	}
}
*/

void CGuiView::RenderFocusBorder()
{
	return;
	
	if (this->focusElement != NULL)
	{
		this->focusElement->RenderFocusBorder();
	}
	else
	{
		BlitRectangle(this->posX, this->posY, this->posZ, this->sizeX, this->sizeY, 1.0f, 0.0f, 0.0f, 0.5f, guiMain->theme->focusBorderLineWidth);
	}
}

void CGuiView::UpdateTheme()
{
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;
		
		guiElement->UpdateTheme();
	}
}


// focus
bool CGuiView::WillClearFocus()
{
	LOGG("CGuiView::WillClearFocus: name='%s'", (name ? name : "NULL"));
	if (CGuiElement::WillClearFocus())
	{
		return ClearFocusElement();
	}
	return false;
}

bool CGuiView::WillReceiveFocus()
{
	return CGuiElement::WillReceiveFocus();
}

bool CGuiView::FocusReceived()
{
	return CGuiElement::FocusReceived();
}

bool CGuiView::FocusLost()
{
	return CGuiElement::FocusLost();
}

bool CGuiView::ClearFocusElement()
{
	if (focusElement != NULL)
	{
		if (focusElement->WillClearFocus())
		{
			focusElement = NULL;
			
			// this is a sanity check, should not be required
			for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
				 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
			{
				CGuiElement *guiElement = (*enumGuiElems).second;
				guiElement->hasFocus = false;
			}
			return true;
		}
		return false;
	}
	return true;
}

bool CGuiView::SetFocusElement(CGuiElement *element)
{
	LOGD("CGuiView::SetFocusElement: view=%s, element=%s", (name ? name : "NULL"), (element ? element->name : "NULL"));
	if (CGuiElement::WillClearFocus())
	{
		if (element != NULL && element->WillReceiveFocus())
		{
			if (this->focusElement != NULL)
			{
				this->focusElement->WillClearFocus();
			}
			this->focusElement = element;
			LOGG("CGuiView::SetFocusElement: %s is set focus", element->name);
			return true;
		}
	}
	return false;
}

void CGuiView::SetFocus()
{
	SetVisible(true);
	guiMain->SetFocus(this);
}

void CGuiView::SetKeepAspectRatio(bool shouldKeepAspectRatio, float aspectRatio)
{
	this->imGuiWindowKeepAspectRatio = shouldKeepAspectRatio;
	this->imGuiWindowAspectRatio = aspectRatio;
}


void CGuiView::ResourcesPrepare()
{
	for (std::map<float, CGuiElement *, compareZupwards>::iterator enumGuiElems = guiElementsUpwards.begin();
		 enumGuiElems != guiElementsUpwards.end(); enumGuiElems++)
	{
		CGuiElement *guiElement = (*enumGuiElems).second;

		// win32 fuckup:
//	for (std::map<float, CGuiElement *>::iterator enumGuiElems = guiElementsDownwards.begin();
//		 enumGuiElems != guiElementsDownwards.end(); enumGuiElems++)
//	{
//		CGuiElement *guiElement = (*enumGuiElems).second;
		
		
		guiElement->ResourcesPrepare();

	}
}

//


bool CGuiView::StartAnimationEditorDebug()
{
	return false;
}

void CGuiView::ReturnFromAnimationEditorDebug()
{
}

//
void CGuiView::InitImGuiView(const char *name)
{
	this->name = STRALLOC(name);

	// this is real imgui window
	imGuiNoWindowPadding = false;
	imGuiNoScrollbar = false;
	consumeTapBackground = true;
}

// layout
void CGuiView::AddLayoutParameter(CLayoutParameter *layoutParameter)
{
	u64 hash = GetHashCode64(layoutParameter->name);
	layoutParametersByHash[hash] = layoutParameter;
	layoutParameters.push_back(layoutParameter);
	
	if (layoutParameter->isHidden == false)
		numNotHiddenLayoutParameters++;
}

void CGuiView::LayoutParameterChanged(CLayoutParameter *layoutParameter)
{
}

void CGuiView::SerializeLayout(CByteBuffer *byteBuffer)
{
	LOGG("CGuiView::SerializeLayout: %s", name);
	
	byteBuffer->PutBool(IsVisible());
	byteBuffer->PutFloat(posX);
	byteBuffer->PutFloat(posY);
	byteBuffer->PutFloat(sizeX);
	byteBuffer->PutFloat(sizeY);
	
	byteBuffer->PutU32(layoutParameters.size());
	
	for (std::list<CLayoutParameter *>::iterator it = layoutParameters.begin(); it != layoutParameters.end(); it++)
	{
		CLayoutParameter *parameter = *it;

//		LOGD("...parameter=%s", parameter->name);
		byteBuffer->putString(parameter->name);
		parameter->Serialize(byteBuffer);
	}	
}

bool CGuiView::DeserializeLayout(CByteBuffer *byteBuffer, int version)
{
	LOGG("CGuiView::DeserializeLayout: %s version=%d", name, version);
	
	bool isRestoreCorrect = true;
	bool setVisible = byteBuffer->GetBool();
	
	if (version >= 2)
	{
		float px = byteBuffer->GetFloat();
		float py = byteBuffer->GetFloat();
		float sx = byteBuffer->GetFloat();
		float sy = byteBuffer->GetFloat();
		this->SetPosition(px, py, -1, sx, sy);
	}
	
	u32 numParameters = byteBuffer->GetU32();

	for (u32 i = 0; i < numParameters; i++)
	{
		char *paramName = byteBuffer->getString();
		u64 hash = GetHashCode64(paramName);
		
		std::map<u64, CLayoutParameter *>::iterator it = layoutParametersByHash.find(hash);
		
		if (it == layoutParametersByHash.end())
		{
			LOGError("CGuiView::DeserializeLayout: %s layout parameter not found '%s'", this->name, paramName);
			STRFREE(paramName);
			isRestoreCorrect = false;
			break;
		}

		STRFREE(paramName);

		CLayoutParameter *parameter = it->second;
		if (parameter->Deserialize(byteBuffer) == false)
		{
			isRestoreCorrect = false;
			break;
		}
	}
	
	this->LayoutParameterChanged(NULL);
	this->SetVisible(setVisible);
	
	return isRestoreCorrect;
}

bool CGuiView::DoDropFile(char *filePath)
{
	return false;
}

void CGuiView::Serialize(CByteBuffer *byteBuffer)
{
}

void CGuiView::Deserialize(CByteBuffer *byteBuffer)
{
}


