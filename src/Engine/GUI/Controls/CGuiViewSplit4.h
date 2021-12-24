#ifndef _GUI_VIEW_SPLIT4_
#define _GUI_VIEW_SPLIT4_

#include "CGuiView.h"

class CGuiViewSplit4 : public CGuiView
{
public:
	CGuiViewSplit4(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewSplit4();

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
//	virtual void FinishTouches();

	virtual void ActivateView();
	virtual void DeactivateView();

	int numViews;
	CGuiView *views[4];
	
	float translateX[4];
	float translateY[4];
	
	void SetView(u8 viewNum, CGuiView *view);

	void ConvertTap(float x, float y, int *screenNum, float *tapX, float *tapY);
};

#endif //_GUI_VIEW_SPLIT4_
