#ifndef _CGuiViewSearch_h_
#define _CGuiViewSearch_h_

#include "CGuiView.h"
#include "CGuiButton.h"
#include <vector>

class CGuiViewSearchCallback
{
public:
	virtual void GuiViewSearchCompleted(u32 index);
};


class CGuiViewSearch : public CGuiView
{
public:
	CGuiViewSearch(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CGuiViewSearchCallback *callback);
	virtual ~CGuiViewSearch();

	virtual void RenderImGui();
	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownRepeat(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

	virtual void ActivateView();
//	virtual void DeactivateView();

	void ClearItems();
	
	std::vector<const char *> items;
	int numFilteredItems;
	int highlightedFilteredItemIndex;
	bool highlightedItemChanged;
	int selectedItemIndex;
	
	CGuiViewSearchCallback *callback;
	void *userData;
	
	ImGuiTextFilter     filter;

	bool viewJustActivated;
	
	void CompleteSelection();
	
	virtual bool FocusLost();
	
	bool hideWindowOnFocusLost;
};

#endif
