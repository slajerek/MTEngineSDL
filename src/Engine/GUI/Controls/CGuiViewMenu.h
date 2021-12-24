#ifndef _GUIVIEWMENU_
#define _GUIVIEWMENU_

#include "CGuiView.h"
#include "CGuiButton.h"
#include <list>

class CSlrMutex;
class CSlrFont;
class CGuiViewMenu;

class CGuiViewMenuItem
{
public:
	// regular menu item
	CGuiViewMenuItem(float height);
	
	// will create sub-menu based on main menu
	CGuiViewMenuItem(float height, CGuiViewMenu *parentMenu);
	~CGuiViewMenuItem();
	
	CGuiViewMenu *menu;

	CGuiViewMenu *subMenu;

	bool isSelected;
	float height;
	
	virtual void SetSelected(bool selected);
	virtual void RenderItem(float px, float py, float pz);
	virtual bool KeyDown(u32 keyCode);
	virtual bool KeyUp(u32 keyCode);
	
	virtual void AddMenuItem(CGuiViewMenuItem *menuItem);
	
	virtual void Execute();

	virtual void DebugPrint();
};

class CGuiViewMenuCallback
{
public:
	virtual void MenuCallbackItemEntered(CGuiViewMenuItem *menuItem);
	virtual void MenuCallbackItemChanged(CGuiViewMenuItem *menuItem);
};


class CGuiViewMenu : public CGuiView, CGuiButtonCallback
{
public:
	CGuiViewMenu(float posX, float posY, float posZ, float sizeX, float sizeY, CGuiViewMenuCallback *callback);
	virtual ~CGuiViewMenu();

	virtual void Render();
	virtual void DoLogic();

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);

	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float posX, float posY);

	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	
	// multi touch
	virtual bool DoMultiTap(COneTouchData *touch, float x, float y);
	virtual bool DoMultiMove(COneTouchData *touch, float x, float y);
	virtual bool DoMultiFinishTap(COneTouchData *touch, float x, float y);

	virtual void FinishTouches();

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats
	
	virtual bool DoScrollWheel(float deltaX, float deltaY);

	virtual void ActivateView();
	virtual void DeactivateView();
	
	std::list<CGuiViewMenuItem *> menuItems;
	std::list<CGuiViewMenuItem *>::iterator selectedItem;

	std::list<CGuiViewMenuItem *>::iterator firstVisibleItem;

	void AddMenuItem(CGuiViewMenuItem *menuItem);
	void InitSelection();
	void ClearSelection();
	void SelectMenuItem(CGuiViewMenuItem *menuItemToSelect);
	void SelectNext();
	void SelectPrev();
	
	void ClearItems();
	
	CGuiViewMenu *currentSubMenu;
	CGuiViewMenu *parentMenu;
	
	void ExecuteSelectedItem();

	CGuiViewMenuCallback *callback;
};

#endif //_GUIVIEWMENU_
