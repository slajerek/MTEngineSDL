/*
 *  CGuiElement.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-26.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CGuiElement.h"

CGuiElement::CGuiElement(float posX, float posY, float posZ, float sizeX, float sizeY)
{
	this->name = "GuiElement";

	this->parent = NULL;
	
	this->visible = true;
	this->allowFocus = true;
	
	SetPositionOffset(posX, posY, posZ);
	SetPosition(posX, posY, posZ, sizeX, sizeY);

	this->elementAlignment = ELEMENT_ALIGNED_NONE;

	this->locked = false;
	this->fullPath = NULL;

	this->hasFocus = false;

	this->manualRender = false;
	this->bringToFrontOnTap = false;
	
	this->repeatTime = 0;
	this->isKeyDown = false;
	
	this->drawFocusBorder = true;
	
	this->userData = NULL;
}

CGuiElement::~CGuiElement()
{

}

bool CGuiElement::IsVisible()
{
	return this->visible;
}

void CGuiElement::SetVisible(bool isVisible)
{
	this->visible = isVisible;
}

// refresh UI
void CGuiElement::UpdatePosition()
{
	this->SetPosition(this->posX, this->posY, this->posZ, this->sizeX, this->sizeY);
}

///
void CGuiElement::SetSize(float sizeX, float sizeY)
{
	this->sizeX = sizeX;
	this->sizeY = sizeY;
	this->posEndX = posX + sizeX;
	this->posEndY = posY + sizeY;
}

void CGuiElement::SetPosition(float posX, float posY)
{
	this->SetPosition(posX, posY, this->posZ, sizeX, sizeY);
}

void CGuiElement::SetPosition(float posX, float posY, float posZ)
{
	this->SetPosition(posX, posY, posZ, this->sizeX, this->sizeY);
}

void CGuiElement::SetPosition(float posX, float posY, float sizeX, float sizeY)
{
	//LOGD("CGuiElement::SetPosition3, name=%s", this->name);
	
	this->posX = posX;
	this->posY = posY;
	this->SetSize(sizeX, sizeY);
	
	this->gapX = 0;
	this->gapY = 0;
}

void CGuiElement::SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY)
{
	//LOGD("CGuiElement::SetPosition3, name=%s", this->name);

	this->posX = posX;
	this->posY = posY;
	this->posZ = posZ;
	this->SetSize(sizeX, sizeY);
	
	this->gapX = 0;
	this->gapY = 0;
}

void CGuiElement::SetPositionOffset(float offsetPosX, float offsetPosY)
{
	this->SetPositionOffset(offsetPosX, offsetPosY, offsetPosZ);
}

void CGuiElement::SetPositionOffset(float offsetPosX, float offsetPosY, float offsetPosZ)
{
	this->offsetPosX = offsetPosX;
	this->offsetPosY = offsetPosY;
	this->offsetPosZ = offsetPosZ;
	
	this->offsetPosEndX = offsetPosX + sizeX;
	this->offsetPosEndY = offsetPosY + sizeY;
}

bool CGuiElement::IsInside(float x, float y)
{
	if (!this->visible)
	{
//		LOGG("CGuiElement::IsInside: not visible, return false");
		return false;
	}

	return IsInsideNonVisible(x, y);
}

bool CGuiElement::IsInsideNonVisible(float x, float y)
{
//	LOGG("CGuiElement::IsInsideNonVisible: x=%f y=%f posX=%f posEndX=%f posY=%f posEndY=%f", x, y, this->posX, this->posEndX, this->posY, this->posEndY);

	if (x >= this->posX && x <= this->posEndX
		&& y >= this->posY && y <= this->posEndY)
	{
//		LOGG("CGuiElement::IsInsideNonVisible: return true");
		return true;
	}

//	LOGG("CGuiElement::IsInsideNonVisible: return false");
	return false;
}

void CGuiElement::RenderImGui()
{
}

void CGuiElement::Render()
{
}


void CGuiElement::Render(float posX, float posY)
{
	Render();
}

void CGuiElement::Render(float posX, float posY, float sizeX, float sizeY)
{
	Render(posX, posY);
}

void CGuiElement::RenderFocusBorder()
{
	if (drawFocusBorder)
	{
		// TODO: render focus rectangle based on colours from theme!
		const float lineWidth = 0.7f;
		BlitRectangle(this->posX, this->posY, this->posZ, this->sizeX, this->sizeY, 1.0f, 0.0f, 0.0f, 0.5f, lineWidth);		
	}
}

bool CGuiElement::IsFocusable()
{
	return allowFocus;
}

bool CGuiElement::DoTap(float x, float y)
{
	//LOGG("CGuiElement::DoTap");
	return false;
}

bool CGuiElement::DoFinishTap(float x, float y)
{
	LOGG("CGuiElement::DoFinishTap");
	return false;
}

bool CGuiElement::DoDoubleTap(float x, float y)
{
	LOGG("CGuiElement::DoDoubleTap");
	return this->DoTap(x, y);
}

bool CGuiElement::DoFinishDoubleTap(float x, float y)
{
	LOGG("CGuiElement::DoFinishDoubleTap");
	return this->DoFinishTap(x, y);
}

bool CGuiElement::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	LOGG("CGuiElement::DoMove");
	return false;
}

bool CGuiElement::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	LOGG("CGuiElement::FinishMove");
	return false;
}

bool CGuiElement::DoRightClick(float x, float y)
{
	return false;
}

bool CGuiElement::DoFinishRightClick(float x, float y)
{
	return false;
}

bool CGuiElement::DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return false;
}

bool CGuiElement::FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	return false;	
}


bool CGuiElement::DoNotTouchedMove(float x, float y)
{
	//LOGG("CGuiElement::DoNotTouchedMove");
	return false;
}

bool CGuiElement::InitZoom()
{
	LOGG("CGuiElement::InitZoom");
	return false;
}

bool CGuiElement::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	LOGG("CGuiElement::DoZoomBy");
	return false;
}

bool CGuiElement::DoScrollWheel(float deltaX, float deltaY)
{
	LOGG("CGuiElement::DoScrollWheel");
	return false;
}

bool CGuiElement::DoMultiTap(COneTouchData *touch, float x, float y)
{
	LOGG("CGuiElement::DoMultiTap: %s", this->name);
	return false;
}

bool CGuiElement::DoMultiMove(COneTouchData *touch, float x, float y)
{
	LOGG("CGuiElement::DoMultiMove: %s", this->name);
	return false;

}

bool CGuiElement::DoMultiFinishTap(COneTouchData *touch, float x, float y)
{
	LOGG("CGuiElement::DoMultiFinishTap: %s", this->name);
	return false;
}

bool CGuiElement::DoGamePadButtonDown(CGamePad *gamePad, u8 button)
{
	LOGG("CGuiElement::DoGamePadButtonDown: %d", button);
	return false;
}

bool CGuiElement::DoGamePadButtonUp(CGamePad *gamePad, u8 button)
{
	LOGG("CGuiElement::DoGamePadButtonUp: %d", button);
	return false;
}

bool CGuiElement::DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value)
{
//	LOGG("CGuiElement::DoGamePadAxisMotion: %d %d", axis, value);
	return false;
}

void CGuiElement::DoLogic()
{
	//LOGG("CGuiElement::DoLogic");
}

void CGuiElement::AddGuiElement(CGuiElement *guiElement, float z)
{
	//map<int, CObjectInfo *>::iterator objDataIt = detectedObjects.find(val);
	this->guiElementsUpwards[z] = guiElement;
	this->guiElementsDownwards[z] = guiElement;
	
	guiElement->parent = this;
}

float CGuiElement::GetWidth()
{
	// optimize this in setPosition... or better not
	return this->posEndX - this->posX;
}

float CGuiElement::GetHeight()
{
	// optimize this in setPosition... or better not
	return this->posEndY - this->posY;
}

void CGuiElement::FocusReceived()
{
//	LOGG("CGuiElement::FocusReceived: name=%s", name);
	this->hasFocus = true;
}

void CGuiElement::FocusLost()
{
//	LOGG("CGuiElement::FocusLost: name=%s", name);
	this->hasFocus = false;
}

bool CGuiElement::HasFocus()
{
//	LOGD("CGuiElement::HasFocus: element=%s hasFocus=%s", this->name, STRBOOL(this->hasFocus));
	return this->hasFocus;
}

bool CGuiElement::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CGuiElement::KeyDown: %d %s %s %s", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	return false;
}

bool CGuiElement::KeyDownOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
//	LOGI("CGuiElement::KeyDownOnMouseHover: %d %s %s %s", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	return false;
}

// key down event even when not focused and not visible
bool CGuiElement::KeyDownGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
//	LOGI("CGuiElement::KeyDownGlobal: %d %s %s %s", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	return false;
}

bool CGuiElement::KeyDownRepeat(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CGuiElement::KeyDownRepeat: %d %s %s %s", keyCode, STRBOOL(isShift), STRBOOL(isAlt), STRBOOL(isControl));
	return false;
}

bool CGuiElement::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

bool CGuiElement::KeyUpGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

bool CGuiElement::KeyUpOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

bool CGuiElement::KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

void CGuiElement::ResourcesPrepare()
{	
}

void CGuiElement::ResourcesPostLoad()
{
}

void CGuiElement::SetName(const char *name)
{
	this->name = name;
}

void CGuiElement::Serialize(CByteBuffer *byteBuffer)
{
}

void CGuiElement::Deserialize(CByteBuffer *byteBuffer)
{
}

bool CGuiElement::SetFocus()
{
	this->FocusReceived();
	return true;
}
