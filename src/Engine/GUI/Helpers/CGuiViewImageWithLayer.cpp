#include "GUI_Main.h"
#include "CGuiViewImageWithLayer.h"

CGuiViewImageWithLayer::CGuiViewImageWithLayer(const char *name, float posX, float posY, float sizeX, float sizeY)
: CGuiViewMovingPaneImage(name, posX, posY, -1, sizeX, sizeY)
{
	imGuiNoWindowPadding = true;
	imGuiNoScrollbar = true;

	InitImage();

	resetLayerImageOnSetImage = true;

	editLayerImageData = NULL;
	editLayerImage = NULL;
}

void CGuiViewImageWithLayer::CreateImageData()
{
//	CreateEmptyImageData(256, 256);
//
//	for (int x = 0; x < 256; x++)
//	{
//		for (int y = 0; y < 256; y++)
//		{
//			imageData->SetPixel(x, y, x, y, 0, 255);
//		}
//	}
}

bool CGuiViewImageWithLayer::UpdateImageData()
{
	// this is virtual method to fill changed image data, returns true when image was changed.
	return false;
}

//
void CGuiViewImageWithLayer::SetImage(CSlrImage *setImage)
{
	SetImage(setImage, true);
}

void CGuiViewImageWithLayer::SetImage(CSlrImage *setImage, bool clearZoom)
{
	guiMain->LockMutex();

	CGuiViewMovingPaneImage::SetImage(setImage, clearZoom);
	
	if (resetLayerImageOnSetImage && setImage != NULL)
	{
		if (editLayerImage)
		{
			VID_PostImageDealloc(editLayerImage);
			editLayerImage = NULL;
		}
		
		if (editLayerImageData)
		{
			delete editLayerImageData;
		}
		
		editLayerImageData = new CImageData(setImage->rasterWidth, setImage->rasterHeight, IMG_TYPE_RGBA);
		editLayerImageData->AllocImage(false, true);
		
		editLayerImage = new CSlrImage(true, false);
		editLayerImage->LoadImageForRebinding(editLayerImageData, RESOURCE_PRIORITY_STATIC);
		
		VID_PostImageBinding(editLayerImage, NULL, BINDING_MODE_DONT_FREE_IMAGEDATA);
	}

	guiMain->UnlockMutex();
}

void CGuiViewImageWithLayer::UpdateLayer(CImageData *imageDataLayer)
{
	/* Example
	 
	// render any "stuff"
	static int z = 0;
	for (int x = 0; x < 100; x++)
	{
		for (int y = 0; y < 100; y++)
		{
			imageDataLayer->SetPixel(x, y, x*z, y*z, 0, 255);
		}
	}
	z++;
	 */
}

// render the editing layer
void CGuiViewImageWithLayer::RenderMovingPane()
{
	if (editLayerImageData == NULL)
		return;
		
	// clear layer
	for (int x = 0; x < image->width; x++)
	{
		for (int y = 0; y < image->height; y++)
		{
			editLayerImageData->SetPixel(x, y, 0, 0, 0, 0);
		}
	}
	
	UpdateLayer(editLayerImageData);
	
	editLayerImage->ReBindImage();

	Blit(editLayerImage, renderMapPosX, renderMapPosY, -1, renderMapSizeX, renderMapSizeY,
		 renderTextureStartX,
		 renderTextureStartY,
		 renderTextureEndX,
		 renderTextureEndY);

}


CGuiViewImageWithLayer::~CGuiViewImageWithLayer()
{
}

