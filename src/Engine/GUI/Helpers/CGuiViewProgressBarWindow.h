#ifndef _CGuiViewProgressBarWindow_
#define _CGuiViewProgressBarWindow_

#include "CGuiView.h"
#include "CGuiButton.h"

class CGuiViewProgressBarWindowCallback
{
public:
	virtual float GetGuiViewProgressBarWindowValue(void *userData);
};

class CGuiViewProgressBarWindow : public CGuiView, CGuiButtonCallback
{
public:
	CGuiViewProgressBarWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CGuiViewProgressBarWindowCallback *callback);
	virtual ~CGuiViewProgressBarWindow();

	virtual void Render();
	virtual void Render(float posX, float posY);
	//virtual void Render(float posX, float posY, float sizeX, float sizeY);
	virtual void RenderImGui();

	virtual void DoLogic();

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);

	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float posX, float posY);

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	
	virtual bool DoScrollWheel(float deltaX, float deltaY);

	// multi touch
	virtual bool DoMultiTap(COneTouchData *touch, float x, float y);
	virtual bool DoMultiMove(COneTouchData *touch, float x, float y);
	virtual bool DoMultiFinishTap(COneTouchData *touch, float x, float y);

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats
	
	virtual void ActivateView();
	virtual void DeactivateView();

	CGuiButton *btnDone;
	bool ButtonClicked(CGuiButton *button);
	bool ButtonPressed(CGuiButton *button);

	void ShowProgressBar(const char *windowTitle, CGuiViewProgressBarWindowCallback *callback);
	void HideProgressBar();

	CGuiViewProgressBarWindowCallback *callback;
	void *userData;
	
	float currentProgress;
	
	bool beginMessageBoxPopup;
	bool showProgressBar;
};

#endif //_GUI_VIEW_DUMMY_
