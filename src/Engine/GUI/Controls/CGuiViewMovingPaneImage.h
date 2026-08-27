#ifndef _CGuiViewMovingPaneImage_h_
#define _CGuiViewMovingPaneImage_h_

#include "CGuiViewMovingPane.h"

class CRenderShader;

class CGuiViewMovingPaneImage : public CGuiViewMovingPane
{
public:
	CGuiViewMovingPaneImage(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	CGuiViewMovingPaneImage(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, const char *titleI18nKey, const char *stableId);
	virtual ~CGuiViewMovingPaneImage();
	
	virtual void RenderImGui();
	virtual void RenderMovingPane();
	
	virtual bool IsInside(float x, float y);
	virtual bool IsInsideView(float x, float y);
	
	virtual void InitPane();
	virtual void InitImage();
	virtual void RefreshEmulatorScreenImageData();
	virtual void CreateEmptyImageData(int imageWidth, int imageHeight);
	virtual bool UpdateImageData();
	
	CImageData *imageData;
	CSlrImage *image;
	bool shouldDeallocImage;
	
	bool imageChanged;
	
	CRenderShader *shader;
	void SetShader(CRenderShader *shader);

	// Display rotation in CLOCKWISE quarter turns (0..3), applied at blit time
	// via BlitQuarterTurns -- the stored texture is never touched. Pane (map)
	// space is DISPLAY-oriented: odd values swap paneWidth/paneHeight, so a
	// host's existing fit/zoom math keeps working against the dimensions the
	// user actually sees. Default 0 leaves every existing host unchanged.
	int rotationQuarters = 0;
	void SetRotationQuarters(int quarterTurns);

	virtual void SetImageData(CImageData *imageData);
	virtual void SetImage(CSlrImage *setImage);

	virtual void SetImageData(CImageData *imageData, bool clearZoom);
	virtual void SetImage(CSlrImage *setImage, bool clearZoom);
	
	virtual void RefreshRenderTextureParameters();
	
	virtual void SetImageKeepAspect(CSlrImage *setImage);
	virtual void SetImageKeepAspect(CSlrImage *setImage, bool clearZoom);

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
