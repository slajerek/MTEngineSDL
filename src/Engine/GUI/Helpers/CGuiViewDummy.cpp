#include "CGuiViewDummy.h"
#include "CGuiMain.h"

CGuiViewDummy::CGuiViewDummy(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
//	imGuiNoWindowPadding = true;
//	imGuiNoScrollbar = true;
}

CGuiViewDummy::~CGuiViewDummy()
{
}

void CGuiViewDummy::DoLogic()
{
	CGuiView::DoLogic();
}

void CGuiViewDummy::Render()
{
	guiMain->fntConsole->BlitText("CGuiViewDummy", posX, posY, 0, 11, 1.0);

	CGuiView::Render();
}

void CGuiViewDummy::Render(float posX, float posY)
{
	CGuiView::Render(posX, posY);
}

void CGuiViewDummy::RenderImGui()
{
	PreRenderImGui();
	this->Render();
	PostRenderImGui();
}

//@returns is consumed
bool CGuiViewDummy::DoTap(float x, float y)
{
	LOGG("CGuiViewDummy::DoTap:  x=%f y=%f", x, y);
	return CGuiView::DoTap(x, y);
}

bool CGuiViewDummy::DoFinishTap(float x, float y)
{
	LOGG("CGuiViewDummy::DoFinishTap: %f %f", x, y);
	return CGuiView::DoFinishTap(x, y);
}

//@returns is consumed
bool CGuiViewDummy::DoDoubleTap(float x, float y)
{
	LOGG("CGuiViewDummy::DoDoubleTap:  x=%f y=%f", x, y);
	return CGuiView::DoDoubleTap(x, y);
}

bool CGuiViewDummy::DoFinishDoubleTap(float x, float y)
{
	LOGG("CGuiViewDummy::DoFinishTap: %f %f", x, y);
	return CGuiView::DoFinishDoubleTap(x, y);
}


bool CGuiViewDummy::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return CGuiView::DoMove(x, y, distX, distY, diffX, diffY);
}

bool CGuiViewDummy::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	return CGuiView::FinishMove(x, y, distX, distY, accelerationX, accelerationY);
}

bool CGuiViewDummy::DoRightClick(float x, float y)
{
	return CGuiView::DoRightClick(x, y);
}

bool CGuiViewDummy::DoFinishRightClick(float x, float y)
{
	return CGuiView::CGuiElement::DoFinishRightClick(x, y);
}

bool CGuiViewDummy::DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return CGuiView::DoRightClickMove(x, y, distX, distY, diffX, diffY);
}

bool CGuiViewDummy::FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	return CGuiView::CGuiElement::FinishRightClickMove(x, y, distX, distY, accelerationX, accelerationY);
}

bool CGuiViewDummy::DoNotTouchedMove(float x, float y)
{
	return CGuiView::DoNotTouchedMove(x, y);
}

bool CGuiViewDummy::InitZoom()
{
	return CGuiView::InitZoom();
}

bool CGuiViewDummy::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	return CGuiView::DoZoomBy(x, y, zoomValue, difference);
}

bool CGuiViewDummy::DoScrollWheel(float deltaX, float deltaY)
{
	return CGuiView::DoScrollWheel(deltaX, deltaY);
}

bool CGuiViewDummy::DoMultiTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiTap(touch, x, y);
}

bool CGuiViewDummy::DoMultiMove(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiMove(touch, x, y);
}

bool CGuiViewDummy::DoMultiFinishTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiFinishTap(touch, x, y);
}

bool CGuiViewDummy::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyDown(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewDummy::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyUp(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewDummy::KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyPressed(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewDummy::DoGamePadButtonDown(CGamePad *gamePad, u8 button)
{
	return CGuiView::DoGamePadButtonDown(gamePad, button);
}

bool CGuiViewDummy::DoGamePadButtonUp(CGamePad *gamePad, u8 button)
{
	return CGuiView::DoGamePadButtonUp(gamePad, button);
}

bool CGuiViewDummy::DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value)
{
	return CGuiView::DoGamePadAxisMotion(gamePad, axis, value);
}

bool CGuiViewDummy::HasContextMenuItems()
{
	return false;
}

void CGuiViewDummy::RenderContextMenuItems()
{
}

void CGuiViewDummy::ActivateView()
{
	LOGG("CGuiViewDummy::ActivateView()");
}

void CGuiViewDummy::DeactivateView()
{
	LOGG("CGuiViewDummy::DeactivateView()");
}
