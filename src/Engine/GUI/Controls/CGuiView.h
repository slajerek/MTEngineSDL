/*
 *  CGuiView.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_VIEW_
#define _GUI_VIEW_

#include "CGuiElement.h"
#include "CSlrImage.h"
#include "SYS_Main.h"
#include "GUI_Main.h"

class CLayoutParameter;

class CGuiView : public CGuiElement
{
public:
	CGuiView(const char *name, float posX, float posY, float sizeX, float sizeY);
	CGuiView(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	CGuiView(float posX, float posY, float posZ, float sizeX, float sizeY);
	
	void Init(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);

	virtual void SetPosition(float posX, float posY);
	virtual void SetPosition(float posX, float posY, float sizeX, float sizeY);
	virtual void SetPosition(float posX, float posY, float posZ);
	virtual void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual void SetPositionElements(float posX, float posY);
	virtual void SetSize(float sizeX, float sizeY);

	// is inside including frame (with title bar, etc)?
	virtual bool IsInside(float x, float y);
	
	// is inside view interior area (without title bar, etc)?
	virtual bool IsInsideView(float x, float y);
	virtual bool IsInsideViewNonVisible(float x, float y);

	virtual void RenderImGui();
	virtual void PreRenderImGui();
	virtual void PostRenderImGui();

	virtual void Render();
	virtual void Render(float posX, float posY);
	//virtual void Render(float posX, float posY, float sizeX, float sizeY);
	virtual void DoLogic();

	virtual bool DoTap(float x, float y);
	virtual bool DoTapNoBackground(float x, float y);
	virtual bool DoFinishTap(float x, float y);

	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float posX, float posY);

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool DoRightClick(float x, float y);
	virtual bool DoFinishRightClick(float x, float y);
	
	virtual bool DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	
	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);		// fired always even on not focused, not visible
	virtual bool KeyDownRepeat(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUpOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUpGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats

	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);

	virtual bool DoScrollWheel(float deltaX, float deltaY);

	virtual bool DoNotTouchedMove(float x, float y);

	virtual bool DoGamePadButtonDown(CGamePad *gamePad, u8 button);
	virtual bool DoGamePadButtonUp(CGamePad *gamePad, u8 button);
	virtual bool DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value);
	// emulated button presses (analog to dpad), normally routed to DoGamePadButtonDown/Up
	virtual bool DoGamePadAxisMotionButtonDown(CGamePad *gamePad, u8 button);
	virtual bool DoGamePadAxisMotionButtonUp(CGamePad *gamePad, u8 button);

//	virtual void FinishTouches();

	// multi touch
	virtual bool DoMultiTap(COneTouchData *touch, float x, float y);
	virtual bool DoMultiTapNoBackground(COneTouchData *touch, float x, float y);
	virtual bool DoMultiMove(COneTouchData *touch, float x, float y);
	virtual bool DoMultiFinishTap(COneTouchData *touch, float x, float y);

	virtual bool DoDropFile(char *filePath);
	
	void PositionCenterOnParentView();
	
	virtual void ActivateView();
	virtual void DeactivateView();

	bool positionElementsOnFrameMove;
	bool consumeTapBackground;
	
	CSlrImage *imgBackground;

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

	// TODO: volatile bool isActiveView;

	std::map<float, CGuiElement *, compareZupwards> guiElementsUpwards;
	std::map<float, CGuiElement *, compareZdownwards> guiElementsDownwards;

	void AddGuiElement(CGuiElement *guiElement, float z);
	void AddGuiElement(CGuiElement *guiElement);

	void RemoveGuiElements();
	void RemoveGuiElement(CGuiElement *guiElement);
	
	void BringToFront(CGuiElement *guiElement);
	
	// focus
	virtual void RenderFocusBorder();
	virtual void ClearFocus();
	virtual bool SetFocus(CGuiElement *view);
	CGuiElement *focusElement;
		
	virtual void InitImGuiView(const char *name);
	
	ImGuiWindow *imGuiWindow;
	
	bool imGuiForceThisFrameNewPosition;
	float thisFrameNewPosX, thisFrameNewPosY;
	virtual void SetNewImGuiWindowPosition(float newPosX, float newPosY);
	
	bool imGuiForceThisFrameNewSize;
	float thisFrameNewSizeX, thisFrameNewSizeY;
	virtual void SetNewImGuiWindowSize(float newSizeX, float newSizeY);

	bool imGuiWindowSkipFocusCheck;
	bool imGuiNoWindowPadding;
	bool imGuiNoScrollbar;
	bool imGuiWindowKeepAspectRatio;
	float imGuiWindowAspectRatio;
	void SetKeepAspectRatio(bool shouldKeepAspectRatio, float aspectRatio);
	
	// Resource Manager
	// this method should prepare all resources, refresh resources
	virtual void ResourcesPrepare();

	// returns if succeeded
	virtual bool StartAnimationEditorDebug();
	virtual void ReturnFromAnimationEditorDebug();
	
//	float mousePosX, mousePosY;
	
	virtual void UpdateTheme();

	// Context menu
	virtual bool HasContextMenuItems();
	virtual void RenderContextMenuItems();

	// Layout, order to display in popup
	std::list<CLayoutParameter *> layoutParameters;
	// Layout, hash of name
	std::map<u64, CLayoutParameter *> layoutParametersByHash;
	virtual void AddLayoutParameter(CLayoutParameter *layoutParameter);
	virtual void RenderContextMenuLayoutParameters();

	// called when layout parameter is changed by UI
	virtual void LayoutParameterChanged(CLayoutParameter *layoutParameter);

	virtual void SerializeLayout(CByteBuffer *byteBuffer);
	virtual bool DeserializeLayout(CByteBuffer *byteBuffer);

	// custom
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual void Deserialize(CByteBuffer *byteBuffer);

	//
	void SetWindowPositionAndSize(float posX, float posY, float sizeX, float sizeY);
	float windowPosX, windowPosY;
	float windowSizeX, windowSizeY;
	float windowPosEndX, windowPosEndY;
	
	float fullScreenSizeX, fullScreenSizeY;
	void SetFullScreenViewSize(float sx, float sy);
	
private:
	float previousZ;
	float previousFrontZ;
	
	float previousPosX, previousPosY;
};

#endif
//_GUI_VIEW_

