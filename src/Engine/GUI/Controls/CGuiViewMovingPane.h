#ifndef _CGuiViewMovingPane_h_
#define _CGuiViewMovingPane_h_

#include "CGuiView.h"

enum MovingPaneStyle
{
	MovingPaneStyle_MultiTouch,
	MovingPaneStyle_LeftClick,
	MovingPaneStyle_RightClick
};

class CGuiViewMovingPane : public CGuiView
{
public:
	CGuiViewMovingPane(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, float paneWidth, float paneHeight);
	virtual ~CGuiViewMovingPane();
	
	virtual void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUpGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual void RenderImGui();
	virtual void RenderMovingPane();
	
	virtual bool DoScrollWheel(float deltaX, float deltaY);
	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);

	virtual bool DoRightClick(float x, float y);
	virtual bool DoFinishRightClick(float x, float y);

	virtual bool DoNotTouchedMove(float x, float y);

	float currentZoom;
	
	float minZoom, maxZoom;

	volatile bool cursorInside;
	float zoomCursorX, zoomCursorY;

	void ClearZoom();
	void ZoomMap(float zoom);
	void MoveMap(float diffX, float diffY);

	int paneWidth;
	int paneHeight;

	float mapPosX, mapPosY, mapSizeX, mapSizeY;
	float renderMapPosX, renderMapPosY, renderMapSizeX, renderMapSizeY;

	bool renderMapValues;

	float cellSizeX, cellSizeY;
	float cellStartX, cellStartY;
	float cellEndX, cellEndY;
	float cellStartIndexX;
	float cellStartIndexY;
	int cellStartIndex;
	int numCellsInWidth;
	int numCellsInHeight;

	void ScreenPosToPanePos(float screenX, float screenY, int *imageX, int *imageY);
	
	void UpdatePositionAndZoom();

	// for double click
	long previousClickTime;
	int previousClickAddr;
	
	bool isForcedMovingMap;
	float prevMousePosX;
	float prevMousePosY;

	// config
	void SetMovingPaneStyle(MovingPaneStyle movingPaneStyle);
	MovingPaneStyle movingPaneStyle;
	bool mouseInvert;
//	bool changeCursorOnMove;
	
};

#endif
