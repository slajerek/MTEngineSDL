#include "SYS_Defs.h"
#include "CGuiViewWindowsManager.h"

CGuiViewWindowsManager::CGuiViewWindowsManager(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
}

void CGuiViewWindowsManager::AddWindow(CGuiWindow *window)
{
	this->AddGuiElement(window);
}

void CGuiViewWindowsManager::HideWindow(CGuiWindow *window)
{
	window->visible = false;
}

void CGuiViewWindowsManager::ShowWindow(CGuiWindow *window)
{
	window->visible = true;
}
