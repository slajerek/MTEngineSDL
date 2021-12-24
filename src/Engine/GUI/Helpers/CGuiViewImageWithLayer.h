#ifndef _CGuiViewImageWithLayer_h_
#define _CGuiViewImageWithLayer_h_

#include "CGuiViewMovingPane.h"
#include "SYS_Defs.h"

class CGuiViewImageWithLayer : public CGuiViewMovingPane
{
public:
	CGuiViewImageWithLayer(float posX, float posY, float sizeX, float sizeY);
	~CGuiViewImageWithLayer();
	
	virtual void UpdateLayer(CImageData *imageDataLayer);

	virtual void CreateImageData();
	virtual bool UpdateImageData();
		
	//
	virtual void SetImage(CSlrImage *setImage);
	virtual void PostRenderMovingPaneCustom();

	CImageData *editLayerImageData;
	CSlrImage *editLayerImage;
};

#endif
