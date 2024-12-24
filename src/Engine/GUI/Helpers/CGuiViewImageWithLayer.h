#ifndef _CGuiViewImageWithLayer_h_
#define _CGuiViewImageWithLayer_h_

#include "CGuiViewMovingPaneImage.h"
#include "SYS_Defs.h"

class CGuiViewImageWithLayer : public CGuiViewMovingPaneImage
{
public:
	CGuiViewImageWithLayer(const char *name, float posX, float posY, float sizeX, float sizeY);
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
