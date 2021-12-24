#include "CGuiViewBaseResourceManager.h"
#include "VID_Main.h"
#include "CDataTable.h"
#include "RES_ResourceManager.h"
#include "CGuiMain.h"
#include "SYS_Threading.h"

CGuiViewBaseResourceManager::CGuiViewBaseResourceManager(float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiViewBaseResourceManager";
}

CGuiViewBaseResourceManager::~CGuiViewBaseResourceManager()
{
}

void CGuiViewBaseResourceManager::RefreshDataTable()
{
}

void CGuiViewBaseResourceManager::SetReturnView(CGuiView *view)
{
}

void CGuiViewBaseResourceManager::DoReturnView()
{
}

void CGuiViewBaseResourceManager::SetGameEditorCallback(CGameEditorCallback *gameEditorCallback)
{
}

void CGameEditorCallback::StartGameEditor(CGuiView *returnView)
{
	LOGError("CGameEditorCallback::StartGameEditor: not implemented");
}

