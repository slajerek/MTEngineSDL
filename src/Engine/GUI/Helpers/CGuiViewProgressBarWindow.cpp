#include "VID_Main.h"
#include "CGuiViewProgressBarWindow.h"
#include "CGuiMain.h"
#include "SYS_KeyCodes.h"
#include "SYS_Platform.h"
#include <SDL.h>

CGuiViewProgressBarWindow::CGuiViewProgressBarWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CGuiViewProgressBarWindowCallback *callback)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	this->userData = NULL;
	this->callback = callback;
	this->currentProgress = 0.0f;
	this->beginMessageBoxPopup = false;
}

CGuiViewProgressBarWindow::~CGuiViewProgressBarWindow()
{
}

void CGuiViewProgressBarWindow::DoLogic()
{
	CGuiView::DoLogic();
}

void CGuiViewProgressBarWindow::Render()
{
	CGuiView::Render();
}

void CGuiViewProgressBarWindow::Render(float posX, float posY)
{
	CGuiView::Render(posX, posY);
}

void CGuiViewProgressBarWindow::RenderImGui()
{
//	PreRenderImGui();

	if (callback)
	{
		currentProgress = callback->GetGuiViewProgressBarWindowValue(userData);
//		LOGD("								CGuiViewProgressBarWindow::RenderImGui: %f", currentProgress);
	}
	
	//
	if (beginMessageBoxPopup)
	{
		ImGui::OpenPopup(name);
		beginMessageBoxPopup = false;
	}
	if (name)
	{
		// Always center this window when appearing
		ImGuiPlatformIO platformIO = ImGui::GetPlatformIO();
		ImVec2 center = platformIO.Monitors[0].MainSize;
		center.x = center.x * 0.5f;
		center.y = center.y * 0.5f;
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(400, 75));
			
		bool popen = true;
		if (ImGui::BeginPopupModal(name, &popen, ImGuiWindowFlags_AlwaysAutoResize))
		{
//			ImGui::Text(name);
			//ProgressBar(float fraction, const ImVec2& size_arg, const char* overlay)
			ImGui::ProgressBar(currentProgress, ImVec2(350, 30));

//			if (ImGui::Button("  Cancel  "))
//			{
////				STRFREE(messageBoxTitle);
////				STRFREE(messageBoxText);
//				ImGui::CloseCurrentPopup();
//
////				if (messageBoxCallback)
////				{
////					messageBoxCallback->MessageBoxCallback();
////				}
////				messageBoxCallback = NULL;
//			}
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar();
	}

//	PostRenderImGui();
}

void CGuiViewProgressBarWindow::ShowProgressBar(const char *windowTitle, CGuiViewProgressBarWindowCallback *callback)
{
	beginMessageBoxPopup = true;
	showProgressBar = true;
	
	this->callback = callback;
	SetName(windowTitle);
	currentProgress = 0.0f;
}

void CGuiViewProgressBarWindow::HideProgressBar()
{
	showProgressBar = false;
}

bool CGuiViewProgressBarWindow::ButtonClicked(CGuiButton *button)
{
	return false;
}

bool CGuiViewProgressBarWindow::ButtonPressed(CGuiButton *button)
{
	return false;
}

//@returns is consumed
bool CGuiViewProgressBarWindow::DoTap(float x, float y)
{
	LOGG("CGuiViewProgressBarWindow::DoTap:  x=%f y=%f", x, y);
	
	return CGuiView::DoTap(x, y);
}

bool CGuiViewProgressBarWindow::DoFinishTap(float x, float y)
{
	LOGG("CGuiViewProgressBarWindow::DoFinishTap: %f %f", x, y);
	return CGuiView::DoFinishTap(x, y);
}

//@returns is consumed
bool CGuiViewProgressBarWindow::DoDoubleTap(float x, float y)
{
	LOGG("CGuiViewProgressBarWindow::DoDoubleTap:  x=%f y=%f", x, y);
	return CGuiView::DoDoubleTap(x, y);
}

bool CGuiViewProgressBarWindow::DoFinishDoubleTap(float x, float y)
{
	LOGG("CGuiViewProgressBarWindow::DoFinishTap: %f %f", x, y);
	return CGuiView::DoFinishDoubleTap(x, y);
}


bool CGuiViewProgressBarWindow::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return CGuiView::DoMove(x, y, distX, distY, diffX, diffY);
}

bool CGuiViewProgressBarWindow::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	return CGuiView::FinishMove(x, y, distX, distY, accelerationX, accelerationY);
}

bool CGuiViewProgressBarWindow::InitZoom()
{
	return CGuiView::InitZoom();
}

bool CGuiViewProgressBarWindow::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	return CGuiView::DoZoomBy(x, y, zoomValue, difference);
}

bool CGuiViewProgressBarWindow::DoScrollWheel(float deltaX, float deltaY)
{
	return CGuiView::DoScrollWheel(deltaX, deltaY);
}

bool CGuiViewProgressBarWindow::DoMultiTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiTap(touch, x, y);
}

bool CGuiViewProgressBarWindow::DoMultiMove(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiMove(touch, x, y);
}

bool CGuiViewProgressBarWindow::DoMultiFinishTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiFinishTap(touch, x, y);
}

bool CGuiViewProgressBarWindow::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyDown(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewProgressBarWindow::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyUp(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CGuiViewProgressBarWindow::KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyPressed(keyCode, isShift, isAlt, isControl, isSuper);
}

void CGuiViewProgressBarWindow::ActivateView()
{
	LOGG("CGuiViewProgressBarWindow::ActivateView()");
}

void CGuiViewProgressBarWindow::DeactivateView()
{
	LOGG("CGuiViewProgressBarWindow::DeactivateView()");
}

float CGuiViewProgressBarWindowCallback::GetGuiViewProgressBarWindowValue(void *userData)
{
	return 0.0f;
}
