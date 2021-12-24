#ifndef _GUI_VIEW_BASELOADINGSCREEN_
#define _GUI_VIEW_BASELOADINGSCREEN_

#include "CGuiView.h"

class CGuiViewBaseLoadingScreen : public CGuiView
{
public:
	CGuiViewBaseLoadingScreen(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewBaseLoadingScreen();

	virtual void LoadingFinishedSetView(CGuiView *nextView);
	virtual void SetLoadingText(char *text);	
};

#endif //_GUI_VIEW_BASELOADINGSCREEN_
