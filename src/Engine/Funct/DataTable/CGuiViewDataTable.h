#ifndef _GUI_VIEW_DATATABLE_
#define _GUI_VIEW_DATATABLE_

#include "CGuiView.h"
#include "CGuiButton.h"

class CDataTable;

class CGuiViewDataTable : public CGuiView
{
public:
	CGuiViewDataTable(float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewDataTable();

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

	CDataTable *dataTable;

	void SetDataTable(CDataTable *dataTable);

	void MoveTable(float x, float y);

	float tableX, tableY;

	float accelX, accelY;
	float prevScale;

	bool accelerateFinishMoves;

	void UpdateTablePos();

};

#endif //_GUI_VIEW_DATATABLE_
