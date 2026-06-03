#include "CGuiViewDummySimple.h"
#include "CGuiMain.h"

CGuiViewDummySimple::CGuiViewDummySimple(const char *name, float posX, float posY, float sizeX, float sizeY)
: CGuiView(name, posX, posY, sizeX, sizeY)
{
//	imGuiNoWindowPadding = true;
//	imGuiNoScrollbar = true;
}

CGuiViewDummySimple::CGuiViewDummySimple(const char *name, float posX, float posY, float sizeX, float sizeY, const char *titleI18nKey, const char *stableId)
: CGuiView(name, posX, posY, sizeX, sizeY, titleI18nKey, stableId)
{
}

CGuiViewDummySimple::~CGuiViewDummySimple()
{
}

void CGuiViewDummySimple::RenderImGui()
{
	PreRenderImGui();
	this->Render();
	PostRenderImGui();
}

bool CGuiViewDummySimple::HasContextMenuItems()
{
	return false;
}

void CGuiViewDummySimple::RenderContextMenuItems()
{
}

void CGuiViewDummySimple::ActivateView()
{
	LOGG("CGuiViewDummySimple::ActivateView()");
}

void CGuiViewDummySimple::DeactivateView()
{
	LOGG("CGuiViewDummySimple::DeactivateView()");
}
