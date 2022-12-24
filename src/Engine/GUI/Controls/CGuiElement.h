/*
 *  CGuiElement.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-26.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_ELEM_
#define _GUI_ELEM_

#include "SYS_Defs.h"
#if defined(IOS)
#include "OpenGLCommon.h"
#endif
#include "SYS_Main.h"
#include <map>
#include "GuiConsts.h"
#include "COneTouchData.h"
#include "CGuiTheme.h"

#define ELEMENT_ALIGNED_NONE	BV00
#define ELEMENT_ALIGNED_CENTER	BV01
#define ELEMENT_ALIGNED_UP		BV02
#define ELEMENT_ALIGNED_DOWN	BV03
#define ELEMENT_ALIGNED_LEFT	BV04
#define ELEMENT_ALIGNED_RIGHT	BV05

class CGamePad;

class CGuiElement : public CThemeChangeListener
{
public:
	CGuiElement(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiElement();

	// TODO: refactor posX to screenPosX and offsetPosX to posX
	// TODO: use OpenGL's glTranslate to not calculate offsets within a window frame (extend CGuiElement from CGuiAnimation that handles this already)
	
	// real position of the element on screen
	float posX, posY, posZ, sizeX, sizeY, posEndX, posEndY;
	float gapX, gapY; // for GuiViewList
	
	// offset position within a window frame
	float offsetPosX, offsetPosY, offsetPosZ;
	float offsetPosEndX, offsetPosEndY;
	
	virtual bool IsInside(float x, float y);
	virtual bool IsInsideNonVisible(float x, float y);

	virtual void SetVisible(bool isVisible);

	virtual void SetPosition(float posX, float posY);
	virtual void SetPosition(float posX, float posY, float sizeX, float sizeY);
	virtual void SetPosition(float posX, float posY, float posZ);
	virtual void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual void SetPositionOffset(float offsetPosX, float offsetPosY);
	virtual void SetPositionOffset(float offsetPosX, float offsetPosY, float offsetPosZ);
	virtual void SetSize(float sizeX, float sizeY);
	virtual void UpdatePosition();

	virtual void RenderImGui();

	virtual void Render();
	virtual void Render(float posX, float posY);
	virtual void Render(float posX, float posY, float sizeX, float sizeY);

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float x, float y);

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool DoRightClick(float x, float y);
	virtual bool DoFinishRightClick(float x, float y);

	virtual bool DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	// not touched move = mouse move with not clicked button
	virtual bool DoNotTouchedMove(float x, float y);

	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);

	virtual bool DoScrollWheel(float deltaX, float deltaY);

	// multi touch
	virtual bool DoMultiTap(COneTouchData *touch, float x, float y);
	virtual bool DoMultiMove(COneTouchData *touch, float x, float y);
	virtual bool DoMultiFinishTap(COneTouchData *touch, float x, float y);

	virtual bool DoGamePadButtonDown(CGamePad *gamePad, u8 button);
	virtual bool DoGamePadButtonUp(CGamePad *gamePad, u8 button);
	virtual bool DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value);

	virtual void DoLogic();

	virtual bool HasFocus();
	virtual bool IsFocusableElement();
	virtual bool WillReceiveFocus();	// return true: ok, returned false: something went wrong, can't receive focus
	virtual bool WillClearFocus();
	virtual bool FocusReceived();
	virtual bool FocusLost();

	bool drawFocusBorder;
	virtual void RenderFocusBorder();
	volatile bool hasFocus;

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUpOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);		// fired always, even on not focused and not visible
	virtual bool KeyUpGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownRepeat(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats
	virtual bool KeyTextInput(const char *text); // utf text input entered
	virtual bool KeyTextInputOnMouseHover(const char *text); // utf text input entered
	bool isKeyDown;

	virtual float GetHeight();
	virtual float GetWidth();
	
	
	// Resource Manager
	// this method should prepare all resources, refresh resources
	virtual void ResourcesPrepare();
	virtual void ResourcesPostLoad();

	virtual bool IsVisible();
	bool visible;

	// does not render in view->Render method
	bool manualRender;
	
	// can element gain focus?
	bool allowFocus;

	u8 elementAlignment;

	const char *name;
	virtual void SetName(const char *name);
	
	volatile bool locked;

	//NSString *fullPath;
	char *fullPath;

	class compareZupwards
	{
		// simple comparison function
	public:
		bool operator()(const float z1, const float z2) const
		{
			return z1 < z2; //(x-y)>0;
		}
	};

	class compareZdownwards
	{
		// simple comparison function
	public:
		bool operator()(const float z1, const float z2) const
		{
			return z1 > z2; //(x-y)>0;
		}
	};


	std::map<float, CGuiElement *, compareZupwards> guiElementsUpwards;
	std::map<float, CGuiElement *, compareZdownwards> guiElementsDownwards;

	void AddGuiElement(CGuiElement *guiElement, float z);
	//	void RemoveGuiElement(CGuiElement *guiElement);
	
	CGuiElement *parent;
	
	bool bringToFrontOnTap;
	
	void *userData;

	// Layout
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual void Deserialize(CByteBuffer *byteBuffer);
};

#endif //_GUI_ELEM_
