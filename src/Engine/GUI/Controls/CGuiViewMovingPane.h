#ifndef _CGuiViewMovingPane_h_
#define _CGuiViewMovingPane_h_

#include "CGuiView.h"

// TODO: CGuiViewMovingPane  change to CGuiViewMovingPaneImage, make generic/abstract
class CGuiViewMovingPane : public CGuiView
{
public:
	CGuiViewMovingPane(float posX, float posY, float posZ, float sizeX, float sizeY);
	~CGuiViewMovingPane();
	
	virtual void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUpGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual void DoLogic();
	virtual void RenderImGui();
	virtual void PostRenderMovingPaneCustom();
	
	virtual bool DoTap(float x, float y);

	virtual bool DoScrollWheel(float deltaX, float deltaY);
	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool DoRightClick(float x, float y);
	virtual bool DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);

	virtual bool DoNotTouchedMove(float x, float y);
	
	virtual void InitImage();
	virtual void CreateImageData();
	virtual void CreateEmptyImageData(int imageWidth, int imageHeight);
	virtual bool UpdateImageData();
	
	CImageData *imageData;
	CSlrImage *image;
	bool shouldDeallocImage;
	
	bool imageChanged;
	
	virtual void SetImageData(CImageData *imageData);
	virtual void SetImage(CSlrImage *setImage);
	
	CSlrFont *font;
	float fontScale;

	float currentZoom;

	volatile bool cursorInside;
	float cursorX, cursorY;

	void ClearZoom();
	void ZoomMap(float zoom);
	void MoveMap(float diffX, float diffY);

	int imageWidth;
	int imageHeight;
	int rasterWidth;
	int rasterHeight;
	float renderTextureStartX, renderTextureStartY;
	float renderTextureEndX, renderTextureEndY;

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

	void ScreenPosToImagePos(float screenX, float screenY, int *imageX, int *imageY);
	
	float currentFontDataScale;

	bool isBeingMoved;
	void UpdateMapPosition();

	// for double click
	long previousClickTime;
	int previousClickAddr;
	
	// move acceleration
	float accelerateX, accelerateY;
	
	bool isForcedMovingMap;
	float prevMousePosX;
	float prevMousePosY;

	// config
	bool useMultiTouch;
	bool mouseInvert;
};

#endif
