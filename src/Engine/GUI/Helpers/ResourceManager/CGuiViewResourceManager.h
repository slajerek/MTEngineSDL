#ifndef _GUI_VIEW_RESOURCE_MANAGER_
#define _GUI_VIEW_RESOURCE_MANAGER_

#include "CGuiView.h"
#include "CGuiButton.h"
#include "CGuiViewDataTable.h"
#include "CGuiViewBaseResourceManager.h"

class CSlrMutex;

class CGuiViewResourceManager : public CGuiViewBaseResourceManager, CGuiButtonCallback
{
public:
	CGuiViewResourceManager(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewResourceManager();

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

	CGuiViewDataTable *viewDataTable;
	
	CGuiButton *btnAnimEditor;
	CGuiButton *btnGameEditor;
	CGuiButton *btnDone;
	bool ButtonClicked(CGuiButton *button);
	bool ButtonPressed(CGuiButton *button);
	
	void SetGameEditorCallback(CGameEditorCallback *gameEditorCallback);
	CGameEditorCallback *gameEditorCallback;
	CSlrMutex *mutex;
	
	virtual void RefreshDataTable();

	void SetReturnView(CGuiView *view);
	CGuiView *returnView;
	
	void DoReturnView();
};

#endif //_GUI_VIEW_RESOURCE_MANAGER_
