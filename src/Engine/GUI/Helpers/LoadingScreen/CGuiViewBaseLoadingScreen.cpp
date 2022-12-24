#include "CGuiViewBaseLoadingScreen.h"
#include "CGuiMain.h"

CGuiViewBaseLoadingScreen::CGuiViewBaseLoadingScreen(float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiViewBaseLoadingScreen";
}

CGuiViewBaseLoadingScreen::~CGuiViewBaseLoadingScreen()
{
}

void CGuiViewBaseLoadingScreen::SetLoadingText(const char *text)
{
}

void CGuiViewBaseLoadingScreen::LoadingFinishedSetView(CGuiView *nextView)
{
	guiMain->SetView(nextView);
	VID_ResetLogicClock();
}

