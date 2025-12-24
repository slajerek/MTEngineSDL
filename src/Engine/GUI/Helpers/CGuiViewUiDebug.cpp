#include "VID_Main.h"
#include "CGuiViewUiDebug.h"
#include "CGuiMain.h"
#include "SYS_KeyCodes.h"
#include "SYS_Platform.h"
#include "MT_API.h"
#include "MT_VERSION.h"
#include <SDL.h>

CGuiViewUiDebug::CGuiViewUiDebug(float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "MTEngineSDL UI debug test";
	
	imGuiNoWindowPadding = true;
	imGuiNoScrollbar = true;

	tapPosX = -1000;	// outside screen
	tapPosY = -1000;
	upPosX = -1000;		// outside screen
	upPosY = -1000;

	keyCode = 0;
	isShift = isAlt = isCtrl = isSuper = false;
}

CGuiViewUiDebug::~CGuiViewUiDebug()
{
}

void CGuiViewUiDebug::DoLogic()
{
	CGuiView::DoLogic();
}

void CGuiViewUiDebug::Render()
{
	guiMain->fntConsole->BlitText("CGuiViewUiDebug", 0, 0, 0, 11, 1.0);

	CGuiView::Render();
}

void CGuiViewUiDebug::Render(float posX, float posY)
{
	CGuiView::Render(posX, posY);
}

void CGuiViewUiDebug::RenderImGui()
{
	PreRenderImGui();
	
	float px = posX;
	float py = posY;
	float fontSize = 11.0f;
	
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "MTEngineSDL version %s", MT_VERSION_STRING);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize, 1.0);
	py += fontSize;
	
	sprintf(buf, "compiled on " __DATE__ " " __TIME__);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize, 1.0);
	py += fontSize;

	SDL_version compiled;
	SDL_version linked;
	SDL_VERSION(&compiled);
	SDL_GetVersion(&linked);
	sprintf(buf, "SDL compiled %d.%d.%d, linked with %d.%d.%d",
		   compiled.major, compiled.minor, compiled.patch,
		   linked.major, linked.minor, linked.patch);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize, 1.0);
	py += fontSize;

	sprintf(buf, "ImGui version %s (%d)", IMGUI_VERSION, IMGUI_VERSION_NUM);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize, 1.0);
	py += fontSize;

	sprintf(buf, "Application %s", MT_GetMainWindowTitle());
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize, 1.0);
	py += fontSize*2;

	sprintf(buf, "Window Pos: X=%5.2f Y=%5.2f  Size: W=%5.2f H=%5.2f", posX, posY, sizeX, sizeY);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;

	sprintf(buf, "Global Mouse pos: %5.2f %5.2f", guiMain->mousePosX, guiMain->mousePosY);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;

	if (tapPosX < 0)
	{
		sprintf(buf, "Tap Mouse pos:");
	}
	else
	{
		sprintf(buf, "Tap Mouse pos: %5.2f %5.2f", tapPosX, tapPosY);
	}

	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;

	if (upPosX < 0)
	{
		sprintf(buf, "Up Mouse pos:");
	}
	else
	{
		sprintf(buf, "Up Mouse pos: %5.2f %5.2f", upPosX, upPosY);
	}

	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;
	
	sprintf(buf, "Key code: %8x %-4d", keyCode, keyCode);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;

	CSlrString *keyName = SYS_KeyName(keyCode);
	if (keyName != NULL)
	{
		char *k = keyName->GetUTF8();
		sprintf(buf, "Key name: %s", k);
		STRFREE(k);
	}
	else
	{
		sprintf(buf, "Key name: %c", keyCode);
	}
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;

	sprintf(buf, "Modifier keys: ");
	if (isShift)
		strcat(buf, "SHIFT ");
	if (isAlt)
		strcat(buf, "ALT ");
	if (isCtrl)
		strcat(buf, "CTRL ");
	if (isSuper)
		strcat(buf, "SUPER");
	
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;
	
	//
	int numDisplays = SDL_GetNumVideoDisplays();
	sprintf(buf, "Video Displays: %d", numDisplays);
	guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
	py += fontSize;

	for (int n = 0; n < numDisplays; n++)
	{
		SDL_Rect r1;
		SDL_GetDisplayBounds(n, &r1);

		SDL_Rect r2;
		SDL_GetDisplayUsableBounds(n, &r2);

		float ddpi, hdpi, vdpi;
		SDL_GetDisplayDPI(n, &ddpi, &hdpi, &vdpi);
		
		float dpiScale = 0.0f;
#if defined(__APPLE__)
		dpiScale = MACOS_GetBackingScaleFactor(n);
#elif defined(SDL_HAS_PER_MONITOR_DPI)
		float dpi = 0.0f;
		if (!SDL_GetDisplayDPI(n, &dpi, NULL, NULL))
			dpiScale = dpi / 96.0f;
#endif

		sprintf(buf, "Display #%d", n);
		guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
		py += fontSize;
		
		sprintf(buf, "  bounds=%d %d %d %d  usable=%d %d %d %d",
				r1.x, r1.y, r1.w, r1.h, r2.x, r2.y, r2.w, r2.h);
		guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
		py += fontSize;

		sprintf(buf, "  ddpi=%5.2f hdpi=%5.2f vdpi=%5.2f scale=%5.2f", ddpi, hdpi, vdpi, dpiScale);
		guiMain->fntConsole->BlitText(buf, px, py, 0, fontSize);
		py += fontSize;
	}
	
	
	// blit cursor position
	BlitPlus(guiMain->mousePosX, guiMain->mousePosY, -1, 1.0f, 0.0f, 0.0f, 1.0f);

	// blit tap position
	BlitPlus(tapPosX, tapPosY, -1, 0.0f, 0.0f, 1.0f, 1.0f);

	// blit up position
	BlitPlus(upPosX, upPosY, -1, 0.0f, 1.0f, 0.0f, 1.0f);

	PostRenderImGui();
}

bool CGuiViewUiDebug::ButtonClicked(CGuiButton *button)
{
	return false;
}

bool CGuiViewUiDebug::ButtonPressed(CGuiButton *button)
{
	return false;
}

//@returns is consumed
bool CGuiViewUiDebug::DoTap(float x, float y)
{
	LOGG("CGuiViewUiDebug::DoTap:  x=%f y=%f", x, y);
	
	tapPosX = x;
	tapPosY = y;
	
	return CGuiView::DoTap(x, y);
}

bool CGuiViewUiDebug::DoFinishTap(float x, float y)
{
	LOGG("CGuiViewUiDebug::DoFinishTap: %f %f", x, y);
	
	upPosX = x;
	upPosY = y;
	
	return CGuiView::DoFinishTap(x, y);
}

//@returns is consumed
bool CGuiViewUiDebug::DoDoubleTap(float x, float y)
{
	LOGG("CGuiViewUiDebug::DoDoubleTap:  x=%f y=%f", x, y);
	return CGuiView::DoDoubleTap(x, y);
}

bool CGuiViewUiDebug::DoFinishDoubleTap(float x, float y)
{
	LOGG("CGuiViewUiDebug::DoFinishTap: %f %f", x, y);
	return CGuiView::DoFinishDoubleTap(x, y);
}


bool CGuiViewUiDebug::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return CGuiView::DoMove(x, y, distX, distY, diffX, diffY);
}

bool CGuiViewUiDebug::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	return CGuiView::FinishMove(x, y, distX, distY, accelerationX, accelerationY);
}

bool CGuiViewUiDebug::InitZoom()
{
	return CGuiView::InitZoom();
}

bool CGuiViewUiDebug::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	return CGuiView::DoZoomBy(x, y, zoomValue, difference);
}

bool CGuiViewUiDebug::DoScrollWheel(float deltaX, float deltaY)
{
	return CGuiView::DoScrollWheel(deltaX, deltaY);
}

bool CGuiViewUiDebug::DoMultiTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiTap(touch, x, y);
}

bool CGuiViewUiDebug::DoMultiMove(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiMove(touch, x, y);
}

bool CGuiViewUiDebug::DoMultiFinishTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiFinishTap(touch, x, y);
}

bool CGuiViewUiDebug::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	this->keyCode = keyCode;
	this->isShift = isShift;
	this->isAlt = isAlt;
	this->isCtrl = isControl;
	this->isSuper = isSuper;
	
	return CGuiView::KeyDown(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewUiDebug::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyUp(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewUiDebug::KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyPressed(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewUiDebug::DoGamePadButtonDown(CGamePad *gamePad, u8 button)
{
	return true;
}

bool CGuiViewUiDebug::DoGamePadButtonUp(CGamePad *gamePad, u8 button)
{
	return true;
}

bool CGuiViewUiDebug::DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value)
{
	return true;
}

void CGuiViewUiDebug::ActivateView()
{
	LOGG("CGuiViewUiDebug::ActivateView()");
}

void CGuiViewUiDebug::DeactivateView()
{
	LOGG("CGuiViewUiDebug::DeactivateView()");
}
