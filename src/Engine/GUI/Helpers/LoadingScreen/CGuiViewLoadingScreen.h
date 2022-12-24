#ifndef _GUI_VIEW_LOADINGSCREEN_
#define _GUI_VIEW_LOADINGSCREEN_

#include "CGuiViewBaseLoadingScreen.h"
#include "CGuiButton.h"

#define GUIVIEWLOADINGSCREEN_TEXT_LEN	255

class CGuiViewLoadingScreen : public CGuiViewBaseLoadingScreen
{
public:
	CGuiViewLoadingScreen(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewLoadingScreen();

	virtual void Render();
	virtual void Render(float posX, float posY);
	//virtual void Render(float posX, float posY, float sizeX, float sizeY);
	virtual void DoLogic();

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);

	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float posX, float posY);

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);

	virtual void ActivateView();
	virtual void DeactivateView();

	virtual void LoadingFinishedSetView(CGuiView *nextView);
	
	virtual void SetLoadingText(const char *text);
	
	char loadingText[GUIVIEWLOADINGSCREEN_TEXT_LEN];
	
	u64 loadStartTime;
	void TimeToStr(long time);
	char strTime[32];
};

#endif //_GUI_VIEW_LOADINGSCREEN_
