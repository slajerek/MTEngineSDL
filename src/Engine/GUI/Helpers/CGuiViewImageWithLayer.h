#ifndef _CGuiViewImageWithLayer_h_
#define _CGuiViewImageWithLayer_h_

#include "CGuiViewMovingPaneImage.h"
#include "SYS_Defs.h"

class CGuiViewImageWithLayer : public CGuiViewMovingPaneImage
{
public:
	CGuiViewImageWithLayer(const char *name, float posX, float posY, float sizeX, float sizeY);
	CGuiViewImageWithLayer(const char *name, float posX, float posY, float sizeX, float sizeY, const char *titleI18nKey, const char *stableId);
	~CGuiViewImageWithLayer();
	
	virtual void UpdateLayer(CImageData *imageDataLayer);

	virtual void RefreshEmulatorScreenImageData();
	virtual bool UpdateImageData();
		
	//
	virtual void SetImage(CSlrImage *setImage);
	virtual void SetImage(CSlrImage *setImage, bool clearZoom);
	virtual void RenderMovingPane();

	bool resetLayerImageOnSetImage;
	CImageData *editLayerImageData;
	CSlrImage *editLayerImage;
};

#endif
