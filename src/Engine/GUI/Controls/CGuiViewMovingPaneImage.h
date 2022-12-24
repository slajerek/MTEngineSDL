#ifndef _CGuiViewMovingPaneImage_h_
#define _CGuiViewMovingPaneImage_h_

#include "CGuiViewMovingPane.h"

class CGuiViewMovingPaneImage : public CGuiViewMovingPane
{
public:
	CGuiViewMovingPaneImage(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewMovingPaneImage();
	
	virtual void RenderImGui();
	virtual void PostRenderMovingPaneCustom();
	
	virtual void InitPane();
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

	int rasterWidth;
	int rasterHeight;
	float renderTextureStartX, renderTextureStartY;
	float renderTextureEndX, renderTextureEndY;

	void ScreenPosToImagePos(float screenX, float screenY, int *imageX, int *imageY);
	
	float currentFontDataScale;

};

#endif
