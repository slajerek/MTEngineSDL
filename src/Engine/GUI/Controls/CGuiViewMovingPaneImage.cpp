#include "CGuiMain.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include "CGuiViewMovingPaneImage.h"
#include "SYS_KeyCodes.h"
#include "VID_ImageBinding.h"
#include "CRenderShader.h"

CGuiViewMovingPaneImage::CGuiViewMovingPaneImage(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiViewMovingPane(name, posX, posY, posZ, sizeX, sizeY, 1, 1)
{
	this->font = NULL;
	this->fontScale = 0.11f;
	
	//
	shouldDeallocImage = false;
	
	rasterWidth = 0;
	rasterHeight = 0;
	
	imageData = NULL;
	image = NULL;
	shader = NULL;

	imageChanged = false;	
	// derived constructor should load image and call InitImage()
}

bool CGuiViewMovingPaneImage::IsInside(float x, float y)
{
	return IsInsideWindowInnerRect(x, y);
}

bool CGuiViewMovingPaneImage::IsInsideView(float x, float y)
{
	return IsInsideWindowInnerRect(x, y);
}

void CGuiViewMovingPaneImage::InitPane()
{
	InitImage();
}

void CGuiViewMovingPaneImage::InitImage()
{
	imageData = NULL;
	
	RefreshEmulatorScreenImageData();
	
	if (imageData == NULL)
		return;

	shouldDeallocImage = true;
	image = new CSlrImage(true, false);
	image->LoadImageForRebinding(imageData, RESOURCE_PRIORITY_STATIC);
	VID_PostImageBinding(image, NULL, BINDING_MODE_DONT_FREE_IMAGEDATA);

	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)paneWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)paneHeight / (float)rasterHeight);
}

void CGuiViewMovingPaneImage::RefreshEmulatorScreenImageData()
{
	imageData = NULL;
//	CreateEmptyImageData(256, 256);
}

bool CGuiViewMovingPaneImage::UpdateImageData()
{
	// this is virtual method to fill changed image data, returns true when image was changed.
	return imageChanged;
}

void CGuiViewMovingPaneImage::CreateEmptyImageData(int imageWidth, int imageHeight)
{
	// images
	this->paneWidth = imageWidth;
	this->paneHeight = imageHeight;
	
	rasterWidth = NextPow2(imageWidth);
	rasterHeight = NextPow2(imageHeight);
	imageData = new CImageData(rasterWidth, rasterHeight, IMG_TYPE_RGBA);
	imageData->AllocImage(false, true);
}

CGuiViewMovingPaneImage::~CGuiViewMovingPaneImage()
{
	
}

void CGuiViewMovingPaneImage::SetImageData(CImageData *imageIn)
{
	SetImageData(imageData, true);
}

void CGuiViewMovingPaneImage::SetImageData(CImageData *imageIn, bool clearZoom)
{
	guiMain->LockMutex();
	
	if (this->image && shouldDeallocImage)
	{
		VID_PostImageDealloc(this->image);
	}
	this->image = NULL;
	
	if (this->imageData)
		delete this->imageData;
	this->imageData = NULL;
	
	if (imageIn == NULL)
	{
		guiMain->UnlockMutex();
		return;
	}

	this->paneWidth = imageIn->width;
	this->paneHeight = imageIn->height;
	
	rasterWidth = NextPow2(paneWidth);
	rasterHeight = NextPow2(paneHeight);
	imageData = new CImageData(rasterWidth, rasterHeight, IMG_TYPE_RGBA);
	imageData->AllocImage(false, true);
	
	for (int x = 0; x < paneWidth; x++)
	{
		for (int y = 0; y < paneHeight; y++)
		{
			u8 r,g,b,a;
			imageIn->GetPixel(x, y, &r, &g, &b, &a);
			imageData->SetPixel(x, y, r, g, b, a);
		}
	}

	shouldDeallocImage = true;
	image = new CSlrImage(true, false);
	image->LoadImageForRebinding(imageData, RESOURCE_PRIORITY_STATIC);
	
	VID_PostImageBinding(image, NULL, BINDING_MODE_DONT_FREE_IMAGEDATA);

	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)paneWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)paneHeight / (float)rasterHeight);

	if (clearZoom)
	{
		ClearZoom();
	}
	
	SetKeepAspectRatio(true, imageIn->width / imageIn->height);

	guiMain->UnlockMutex();
}

void CGuiViewMovingPaneImage::SetImage(CSlrImage *setImage)
{
	SetImage(setImage, true);
}

void CGuiViewMovingPaneImage::SetImage(CSlrImage *setImage, bool clearZoom)
{
	guiMain->LockMutex();
	if (shouldDeallocImage && this->image)
	{
		VID_PostImageDealloc(this->image);
	}

	// we are not owner of this image
	shouldDeallocImage = false;
	this->image = setImage;
	
	if (image)
	{
		paneWidth = image->width;
		paneHeight = image->height;
		rasterWidth = image->rasterWidth;
		rasterHeight = image->rasterHeight;
	}

	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)paneWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)paneHeight / (float)rasterHeight);

	if (clearZoom)
	{
		ClearZoom();
	}

	SetKeepAspectRatio(true, paneWidth / paneHeight);

	guiMain->UnlockMutex();
}

void CGuiViewMovingPaneImage::RefreshRenderTextureParameters()
{
	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)paneWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)paneHeight / (float)rasterHeight);
}

void CGuiViewMovingPaneImage::SetImageKeepAspect(CSlrImage *setImage)
{
	SetImageKeepAspect(setImage, true);
}

void CGuiViewMovingPaneImage::SetImageKeepAspect(CSlrImage *setImage, bool clearZoom)
{
//	LOGD("CGuiViewMovingPaneImage::SetImageKeepAspect: setImage=%x width=%f height=%f aspect=%f", setImage, setImage->width, setImage->height, (setImage->width / setImage->height));
	SetImage(setImage, clearZoom);
	if (setImage != NULL)
	{
		SetKeepAspectRatio(true, paneWidth / paneHeight);
	}
}

void CGuiViewMovingPaneImage::RenderMovingPane()
{
}

void CGuiViewMovingPaneImage::RenderImGui()
{
//	if (image == NULL)
//		return;
	
	PreRenderImGui();
	
//	frameCounter++;
		
//	float gap = 1.0f;
	
//	BlitRectangle(posX-gap, posY-gap, -1, sizeX+gap*2, sizeY+gap*2, 0.3, 0.3, 0.3, 0.7f);
	
	// blit
	
//	VID_SetClipping(posX, posY, sizeX, sizeY);
	
	
	//	LOGD("renderTextureStartX=%f renderTextureEndX=%f renderTextureStartY=%f renderTextureEndY=%f",
	//		 renderTextureStartX, renderTextureEndX, renderTextureStartY, renderTextureEndY);

//	LOGD("CGuiViewMovingPaneImage::RenderImGui: image=%x", image);
	
	if (image)
	{
		if (UpdateImageData())
		{
			image->ReBindImage();
		}

		if (image->isBound)
		{
			if (shader)
			{
				shader->UseShaderProgram();
			}
			
			Blit(image, renderMapPosX, renderMapPosY, -1, renderMapSizeX, renderMapSizeY,
				 renderTextureStartX,
				 renderTextureStartY,
				 renderTextureEndX,
				 renderTextureEndY);
			
			if (shader)
			{
				shader->ResetState();
			}
		}
	}
	
	RenderMovingPane();

//	VID_ResetClipping();
	
	//	// debug cursor
	//	float px = cursorX * sizeX + posX;
	//	float py = cursorY * sizeY + posY;
	//	BlitPlus(px, py, posZ, 15, 15, 1, 0, 0, 1);

	PostRenderImGui();
}

void CGuiViewMovingPaneImage::ScreenPosToImagePos(float screenX, float screenY, int *imageX, int *imageY)
{
	ScreenPosToPanePos(screenX, screenY, imageX, imageY);
}

void CGuiViewMovingPaneImage::SetShader(CRenderShader *shader)
{
	this->shader = shader;
}
