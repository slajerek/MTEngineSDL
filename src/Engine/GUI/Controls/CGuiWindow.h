#ifndef _CGuiWindow_H_
#define _CGuiWindow_H_

#include "SYS_Defs.h"
#include "CGuiViewFrame.h"

class CGuiWindowCallback;

// TODO: REFACTOR: CGuiWindow is not needed anymore as CGuiView is ImGui window
class CGuiWindow : public CGuiView
{
	public:
	CGuiWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
			   CSlrString *windowName);
	CGuiWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
			   CSlrString *windowName, CGuiWindowCallback *callback);
	CGuiWindow(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
			   CSlrString *windowName, u32 mode, CGuiWindowCallback *callback);
	
	virtual void Initialize(float posX, float posY, float posZ, float sizeX, float sizeY,
							CSlrString *windowName, u32 mode, CGuiWindowCallback *callback);
	
	virtual bool DoTap(float x, float y);
	
	virtual bool DoRightClick(float x, float y);
	//	virtual bool DoFinishRightClick(float x, float y);

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	
	virtual void SetSize(float sizeX, float sizeY);
	virtual void SetPosition(float posX, float posY);
	virtual void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);
	
	virtual void Render();
	virtual void DoLogic();
	
	//
	CGuiViewFrame *viewFrame;
	
	CGuiWindowCallback *callback;
	
	virtual void ActivateView();
	
	virtual void RenderWindowBackground();
	
	void WindowCloseButtonPressed();
	
//	void AddToolBoxIcon(CSlrImage *imgIcon);
	
};

class CGuiWindowCallback
{
public:
	// returns: cancel close
	virtual bool GuiWindowCallbackWindowClose(CGuiWindow *window);
};


#endif

