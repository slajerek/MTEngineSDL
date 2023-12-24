#ifndef _CGuiViewDummySimple_h_
#define _CGuiViewDummySimple_h_

#include "CGuiView.h"

class CGuiViewDummySimple : public CGuiView
{
public:
	CGuiViewDummySimple(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewDummySimple();

	virtual void RenderImGui();

	virtual bool HasContextMenuItems();
	virtual void RenderContextMenuItems();

	virtual void ActivateView();
	virtual void DeactivateView();
};

#endif //_CGuiViewDummySimple_h_
