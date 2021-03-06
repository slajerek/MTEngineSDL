#ifndef _GUI_VIEW_BASE_RESOURCE_MANAGER_
#define _GUI_VIEW_BASE_RESOURCE_MANAGER_

#include "CGuiView.h"
#include "CGuiButton.h"
//#include "CGuiViewDataTable.h"

class CSlrMutex;

class CGameEditorCallback
{
public:
	virtual void StartGameEditor(CGuiView *returnView);
};

class CGuiViewBaseResourceManager : public CGuiView
{
public:
	CGuiViewBaseResourceManager(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewBaseResourceManager();
	virtual void SetGameEditorCallback(CGameEditorCallback *gameEditorCallback);
	virtual void RefreshDataTable();
	virtual void SetReturnView(CGuiView *view);
	virtual void DoReturnView();
};

#endif //_GUI_VIEW_BASE_RESOURCE_MANAGER_
