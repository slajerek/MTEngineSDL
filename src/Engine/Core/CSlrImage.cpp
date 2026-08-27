/*
 *  CSlrImage.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-23.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CSlrImage.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "SYS_DocsVsRes.h"
#include "GFX_Types.h"
#include "RES_ResourceManager.h"
#include "CSlrFileZlib.h"
#include "zlib.h"
#include "stb_image.h"
#include "SYS_FileSystem.h"
#include "VID_ImageBinding.h"
#include "VID_Main.h"   // gRenderBackend, for the resident-format funnel

ERenderTextureFormat SlrResidentFormatFor(u8 decodeType, bool backendSupportsFloat)
{
	if (decodeType == IMG_TYPE_RGBA_16F)
		return backendSupportsFloat ? RENDER_TEXTURE_RGBA16F : RENDER_TEXTURE_RGBA8;
	return RENDER_TEXTURE_RGBA8;
}

void CSlrImage::ApplyResidentFormat(CImageData *imageData)
{
	if (imageData == NULL || imageData->isCompressed)
		return;

	const bool backendSupportsFloat =
		(gRenderBackend != NULL) &&
		gRenderBackend->SupportsTextureFormat(RENDER_TEXTURE_RGBA16F);

	const ERenderTextureFormat want =
		SlrResidentFormatFor(imageData->getImageType(), backendSupportsFloat);

	if (want == RENDER_TEXTURE_RGBA8)
	{
		// The GPU path is 8-bit here, so whatever extra the decode carried is
		// given up HERE -- the one place that does it -- rather than at the
		// decoder, so everything upstream of the texture still sees what the
		// file actually contained.
		if (imageData->getImageType() == IMG_TYPE_RGBA_16BIT)
		{
			imageData->ConvertRGBA16BitToRGBA8();
		}
		else if (imageData->getImageType() == IMG_TYPE_RGBA_16F)
		{
			// Live headroom, sampled at the moment of the map and never
			// cached: macOS grants it lazily and then keeps moving it with
			// display brightness. At 1.0 the curve is the identity on 0..1, so
			// this is exactly today's picture.
			const float headroom = (gRenderBackend != NULL)
				? gRenderBackend->GetDisplayHdrHeadroom() : 1.0f;
			imageData->ConvertRGBA16FToRGBA8(headroom, !imageData->floatIsSurfaceEncoded);
		}
	}

	this->residentFormat = want;
	// Only a FLOAT resident texture still holds above-white values. If the
	// funnel just tone-mapped to 8 bits, the peak is gone with it, and carrying
	// the old number forward would have the surface's HDR10 metadata describe
	// range the texture no longer contains.
	this->contentMaxComponent = (want == RENDER_TEXTURE_RGBA16F)
		? imageData->contentMaxComponent : 0.0f;
}

CSlrImage::CSlrImage(CImageData *imageData)
{
	this->name = NULL;
	shouldDeallocLoadImageData = false;
	loadImageData = NULL;
	isFromAtlas = false;
	linearScaling = true;
	this->InitImageLoad(linearScaling);
	this->LoadImage(imageData);
	VID_PostImageBinding(this, NULL);
}

CSlrImage::CSlrImage(CImageData *imageData, bool linearScaling)
{
	this->name = NULL;
	shouldDeallocLoadImageData = false;
	loadImageData = NULL;
	isFromAtlas = false;
	this->linearScaling = linearScaling;
	this->InitImageLoad(linearScaling);
	this->LoadImage(imageData);
	VID_PostImageBinding(this, NULL);
}

CSlrImage::CSlrImage(CImageData *imageData, bool linearScaling, bool bindNow)
: CSlrImageTexture()
{
	this->isActive = false;

	this->resourceState = RESOURCE_STATE_DEALLOCATED;
	
	this->shouldDeallocLoadImageData = false;
	this->loadImageData = NULL;

	this->InitImageLoad(linearScaling);
	this->LoadImage(imageData);
	if (bindNow)
	{
		this->BindImage();
		this->FreeLoadImage();
		this->resourceState = RESOURCE_STATE_LOADED;
	}
	else
	{
		VID_PostImageBinding(this, NULL);
	}
}


CSlrImage::CSlrImage(const char *fileName)
{
	this->name = NULL;
	shouldDeallocLoadImageData = false;
	loadImageData = NULL;
	isFromAtlas = false;
	this->InitImageLoad(linearScaling);
	CImageData *imageData = new CImageData(fileName);
	this->LoadImage(imageData);
	delete imageData;
	VID_PostImageBinding(this, NULL);
}

CSlrImage::CSlrImage(CSlrFile *imgFile, bool linearScaling)
: CSlrImageTexture()
{
	this->isActive = false;

	this->resourceState = RESOURCE_STATE_DEALLOCATED;
	
	this->shouldDeallocLoadImageData = false;
	this->loadImageData = NULL;

	this->InitImageLoad(linearScaling);
	this->LoadImage(imgFile);
	this->BindImage();
	this->FreeLoadImage();

	this->resourceState = RESOURCE_STATE_LOADED;
}

CSlrImage::CSlrImage(const char *fileName, bool linearScaling, bool fromResources)
: CSlrImageTexture()
{
	this->isActive = false;

	LOGR("CSlrImage: %s", fileName);
	shouldDeallocLoadImageData = false;
	loadImageData = NULL;
	isFromAtlas = false;

	ResourceSetPath(fileName, fromResources);

	this->resourceState = RESOURCE_STATE_DEALLOCATED;
	fileLoadError = IMAGE_LOAD_ERROR_NOT_LOADED;

	this->InitImageLoad(linearScaling);

	char buf[4096];
	FILE *fp = NULL;
	sprintf(buf, "%s", fileName);
	LOGD("buf='%s'", buf);
	fp = fopen(buf, "rb");
	if (fp != NULL)
	{
		fclose(fp);
		this->LoadImage(buf, "");
	}
	else
	{
		sprintf(buf, "%s%s", gPathToDocuments, fileName);
		LOGD("===== buf='%s'", buf);
		fp = fopen(buf, "rb");
		if (fp != NULL)
		{
			fclose(fp);
			this->LoadImage(buf, "");
		}
		else
		{
			sprintf(buf, "%s%s.png", gPathToDocuments, fileName);
			LOGD("===== buf='%s'", buf);
			fp = fopen(buf, "rb");
			if (fp != NULL)
			{
				fclose(fp);
				this->LoadImage(buf, "");
			}
			else
			{
				sprintf(buf, "%s%s", gPathToResources, fileName);
				LOGD("====== buf='%s'", buf);
				fp = fopen(buf, "rb");
				if (fp != NULL)
				{
					fclose(fp);
					this->LoadImage(buf, "");
				}
				else
				{
					sprintf(buf, "%s%s.png", gPathToResources, fileName);
					LOGD("===== buf='%s'", buf);
					fp = fopen(buf, "rb");
					if (fp != NULL)
					{
						fclose(fp);
						this->LoadImage(buf, "");
					}
					else
					{
						LOGError("File not found: %s", fileName);
						this->resourceState = RESOURCE_STATE_ERROR;
						this->resourceIsActive = true;
						return;
					}
				}
			}
		}
	}

	this->BindImage();
	this->FreeLoadImage();

	this->resourceState = RESOURCE_STATE_LOADED;

	//	delete nsFileName;
}


CSlrImage::CSlrImage(const char *fileName, bool linearScaling)
: CSlrImageTexture()
{
	LOGR("CSlrImage: %s", fileName);

	shouldDeallocLoadImageData = false;
	loadImageData = NULL;
	isFromAtlas = false;

	this->resourceState = RESOURCE_STATE_DEALLOCATED;
	fileLoadError = IMAGE_LOAD_ERROR_NOT_LOADED;

	this->InitImageLoad(linearScaling);

	char buf[4096];
	FILE *fp = NULL;
	sprintf(buf, "%s", fileName);
	//LOGD("buf='%s'", buf);
	fp = fopen(buf, "rb");
	if (fp != NULL)
	{
		fclose(fp);
		this->LoadImage(buf, "");
	}
	else
	{
		sprintf(buf, "%s%s", gPathToDocuments, fileName);
		//LOGD("======== buf='%s'", buf);
		fp = fopen(buf, "rb");
		if (fp != NULL)
		{
			fclose(fp);
			this->LoadImage(buf, "");
			ResourceSetPath(fileName, false);
		}
		else
		{
			sprintf(buf, "%s%s.png", gPathToDocuments, fileName);
			//LOGD("======= buf='%s'", buf);
			fp = fopen(buf, "rb");
			if (fp != NULL)
			{
				fclose(fp);
				this->LoadImage(buf, "");
				ResourceSetPath(fileName, false);
			}
			else
			{
				sprintf(buf, "%s%s", gPathToResources, fileName);
				//LOGD("====== buf='%s'", buf);
				fp = fopen(buf, "rb");
				if (fp != NULL)
				{
					fclose(fp);
					this->LoadImage(buf, "");
					ResourceSetPath(fileName, true);
				}
				else
				{
					sprintf(buf, "%s%s.png", gPathToResources, fileName);
					//LOGD("===== buf='%s'", buf);
					fp = fopen(buf, "rb");
					if (fp != NULL)
					{
						fclose(fp);
						this->LoadImage(buf, "");
						ResourceSetPath(fileName, true);
					}
					else
					{
						LOGError("File not found: %s", fileName);
						this->resourceState = RESOURCE_STATE_ERROR;
						this->resourceIsActive = true;
						return;
					}
				}
			}
		}
	}

	this->BindImage();
	this->FreeLoadImage();

	this->resourceState = RESOURCE_STATE_LOADED;
	this->resourceIsActive = true;

	//	delete nsFileName;
}

CSlrImage::CSlrImage(bool delayedLoad, bool linearScaling)
: CSlrImageTexture()
{
	this->name = NULL;
	
	shouldDeallocLoadImageData = false;
	loadImageData = NULL;
	isFromAtlas = false;
	this->InitImageLoad(linearScaling);
}

void CSlrImage::DelayedLoadImage(const char *fileName, bool fromResources)
{
	ResourceSetPath(fileName, fromResources);

	if (DelayedLoadImageNoFail(fileName, fromResources) == false)
	{
		this->resourceState = RESOURCE_STATE_ERROR;
		this->resourceIsActive = false;
	}
}

bool CSlrImage::DelayedLoadImageNoFail(const char *fileName, bool fromResources)
{
	if (SYS_FileExists(fileName))
	{
		this->LoadImage(fileName, "");
		return true;
	}

	if (fromResources)
	{
		CSlrFile *file = RES_GetFileFromDeploy(fileName, DEPLOY_FILE_TYPE_GFX);
		if (file != NULL)
		{
			this->LoadImage(file);
			delete file;
			return true;
		}
	}

	char buf[4096];
#ifndef USE_DOCS_INSTEAD_OF_RESOURCES
	if (fromResources)
	{
		sprintf(buf, "%s%s", gPathToResources, fileName);
	}
	else
#endif
	{
		sprintf(buf, "%s%s", gPathToDocuments, fileName);
	}

	if (!SYS_FileExists(buf))
	{
		return false;
	}

	this->LoadImage(buf, "png");
	return true;
}

void CSlrImage::PreloadImage(const char *fileName, bool fromResources)
{
	ResourceSetPath(fileName, fromResources);

	if (fromResources)
	{
		CSlrFile *file = RES_GetFileFromDeploy(fileName, DEPLOY_FILE_TYPE_GFX);
		if (file != NULL)
		{
			this->PreloadImage(file);
			delete file;
			return;
		}
	}

	char buf[4096];
#ifndef USE_DOCS_INSTEAD_OF_RESOURCES
	if (fromResources)
	{
		sprintf(buf, "%s%s.png", gPathToResources, fileName);
	}
	else
#endif
	{
		sprintf(buf, "%s%s.png", gPathToDocuments, fileName);
	}

	//LOGD("------------------------------------------------------ preload: before load imagedata");
	//RES_DebugPrintMemory();

	CImageData *imageData = new CImageData(buf);

//	LOGD("------------------------------------------------------ preload: loaded image data");
//	RES_DebugPrintMemory();

	// The resident-format funnel: one decision, two answers.
	ApplyResidentFormat(imageData);

	// RGBA8 and RGBA16F are both legitimate resident formats (S-5) -- these
	// two file-based entry points call the funnel above like the other four,
	// so they must accept the same answers it can give. Anything else means
	// a decoder produced a type the funnel never agreed to, which is still
	// worth dying for.
	if(imageData->getImageType() != IMG_TYPE_RGBA &&
	   imageData->getImageType() != IMG_TYPE_RGBA_16F)
	{
		SYS_FatalExit("Image %s, type is %2.2x (should be %2.2x or %2.2x)",
				buf, imageData->getImageType(), IMG_TYPE_RGBA, IMG_TYPE_RGBA_16F);
	}

	this->loadImgWidth = imageData->width;
	this->loadImgHeight = imageData->height;
	this->rasterWidth = NextPow2(loadImgWidth);
	this->rasterHeight = NextPow2(loadImgHeight);
	this->origRasterWidth = rasterWidth;
	this->origRasterHeight = rasterHeight;
	this->width = loadImgWidth;
	this->height = loadImgHeight;

	this->resourceLoadingSize = rasterWidth * rasterHeight * 4 * 2;
	this->resourceIdleSize = rasterWidth * rasterHeight * 4;

	this->resourceIsActive = false;
	this->resourceState = RESOURCE_STATE_PRELOADING;

//	LOGD("------------------------------------------------------ preload: before delete");
//	RES_DebugPrintMemory();

	delete imageData;

//	LOGD("------------------------------------------------------ preload: deleted");
//	RES_DebugPrintMemory();

	LOGR("CSlrImage::PreloadImage: rasterWidth=%d rasterHeight=%d resourceLoadingSize=%d resourceIdleSize=%d", rasterWidth, rasterHeight, resourceLoadingSize, resourceIdleSize);

}

void CSlrImage::LoadImage(const char *fileName, const char *fileExt)
{
	char buf[4096];
	this->name = strdup(fileName);
	if (fileExt[0] != '\0')
		sprintf(buf, "%s.%s", fileName, fileExt);
	else
		sprintf(buf, "%s", fileName);

	shouldDeallocLoadImageData = false;
	this->loadImageData = NULL;
	if (!SYS_FileExists(buf))
	{
		SYS_FatalExit("Correct image not found: %s", buf);
	}
	
	CImageData *imageData = new CImageData(buf);
	if (!imageData)
	{
		SYS_FatalExit("Correct image not found: %s", buf);
	}

	// The resident-format funnel: one decision, two answers.
	ApplyResidentFormat(imageData);

	// RGBA8 and RGBA16F are both legitimate resident formats (S-5) -- these
	// two file-based entry points call the funnel above like the other four,
	// so they must accept the same answers it can give. Anything else means
	// a decoder produced a type the funnel never agreed to, which is still
	// worth dying for.
	if(imageData->getImageType() != IMG_TYPE_RGBA &&
	   imageData->getImageType() != IMG_TYPE_RGBA_16F)
	{
		SYS_FatalExit("Image %s, type is %2.2x (should be %2.2x or %2.2x)",
				buf, imageData->getImageType(), IMG_TYPE_RGBA, IMG_TYPE_RGBA_16F);
	}
	this->LoadImage(imageData, RESOURCE_PRIORITY_NORMAL);

	// TODO: buffer is allocated twice, and copied
	delete imageData;

}

void CSlrImage::LoadImage(CImageData *imageData)
{
	// default is static as we can't recreate imageData when resource is activated after deactivation
	this->LoadImage(imageData, RESOURCE_PRIORITY_STATIC);
}

void CSlrImage::LoadImage(CImageData *imageData, u8 resourcePriority)
{
	// default is static as we can't recreate imageData when resource is activated after deactivation
	this->LoadImage(imageData, RESOURCE_PRIORITY_STATIC, false);
}

// KTX2 compressed-image support — shared metadata helper. Sets every member
// the RGBA LoadImage path would set, with the compressed-path values:
//   loadImgWidth/Height    = compressed mip-0 size (KTX2 texture dimensions)
//   rasterWidth/Height     = SAME as loadImg* — block-compressed textures need
//                            NO power-of-two padding; the GPU texture is
//                            exactly the KTX2 dimensions
//   origRasterWidth/Height = same as rasterWidth/Height
//   width/height           = loadImg dimensions
//   defaultTexStartX/Y     = 0.0
//   defaultTexEndX/Y       = 1.0 — no padding => clean full-texture UVs
//   widthD2/heightD2       = width/2, height/2
//   widthM2/heightM2       = width*2, height*2
//   resourcePriority       = passed in
//   resourceLoadingSize    = sum of all mip blockDataSize (transcoded payload;
//                            no transient 2x RGBA staging buffer)
//   resourceIdleSize       = same — the compressed payload stays resident
//   resourceIsActive       = false
//   resourceState          = RESOURCE_STATE_PRELOADING
// All three compressed branches (LoadImage / LoadImageForRebinding /
// RefreshImageParameters) call exactly this helper and then return.
void CSlrImage::SetCompressedImageMetadata(CImageData *compressedImageData, u8 resourcePriority)
{
	this->isCompressed = true;
	this->compressedMipCount = compressedImageData->compressedMipCount;

	this->loadImgWidth = compressedImageData->width;
	this->loadImgHeight = compressedImageData->height;

	// block-compressed textures do NOT need power-of-two padding
	this->rasterWidth = (float)this->loadImgWidth;
	this->rasterHeight = (float)this->loadImgHeight;
	this->origRasterWidth = this->rasterWidth;
	this->origRasterHeight = this->rasterHeight;

	this->width = this->loadImgWidth;
	this->height = this->loadImgHeight;

	this->defaultTexStartX = 0.0f;
	this->defaultTexEndX = 1.0f;
	this->defaultTexStartY = 0.0f;
	this->defaultTexEndY = 1.0f;

	this->widthD2 = this->width / 2.0;
	this->heightD2 = this->height / 2.0;
	this->widthM2 = this->width * 2.0;
	this->heightM2 = this->height * 2.0;

	u32 totalPayload = 0;
	for (int i = 0; i < compressedImageData->compressedMipCount; i++)
	{
		totalPayload += compressedImageData->compressedMips[i].blockDataSize;
	}

	this->resourcePriority = resourcePriority;
	this->resourceLoadingSize = totalPayload;
	this->resourceIdleSize = totalPayload;

	this->resourceIsActive = false;
	this->resourceState = RESOURCE_STATE_PRELOADING;
}

void CSlrImage::LoadImage(CImageData *origImageData, u8 resourcePriority, bool flipImageVertically)
{
	// KTX2 compressed (.ktx2) branch. The CImageData passed here is, in the
	// common CSlrImage(fileName) ctor case, a temp object that the ctor will
	// `delete` immediately after this returns — so the compressed mip buffers
	// (potentially >1 MB) must be MOVED out into a CSlrImage-owned CImageData
	// rather than copied or left to be freed by that delete.
	if (origImageData->isCompressed)
	{
		// free any previously-owned load data, mirroring the RGBA reload path
		if (this->shouldDeallocLoadImageData && this->loadImageData != NULL)
		{
			delete this->loadImageData;
			this->loadImageData = NULL;
		}

		// allocate a fresh CSlrImage-owned compressed CImageData and STEAL the
		// mip array out of the source: copy the pointers across, then null
		// them on the source so its destructor frees only an empty husk.
		CImageData *owned = new CImageData();
		owned->type = IMG_TYPE_GPU_COMPRESSED;
		owned->isCompressed = true;
		owned->width = origImageData->width;
		owned->height = origImageData->height;
		owned->compressedGpuFormat = origImageData->compressedGpuFormat;
		owned->compressedMipCount = origImageData->compressedMipCount;
		owned->compressedMips = origImageData->compressedMips;	// steal pointer

		// source no longer owns the mips — its delete / DeallocCompressed()
		// now sees compressedMips == NULL and is a no-op. (When the source is
		// a caller-owned CImageData* it is left valid-but-empty by design.)
		origImageData->compressedMips = NULL;
		origImageData->compressedMipCount = 0;
		origImageData->isCompressed = false;

		// reuse the existing FreeLoadImage()/Deallocate() lifecycle unchanged
		this->loadImageData = owned;
		this->shouldDeallocLoadImageData = true;

		this->SetCompressedImageMetadata(owned, resourcePriority);
		return;
	}

	// The resident-format funnel: one decision, two answers.
	ApplyResidentFormat(origImageData);

	// RGBA8 and RGBA16F are both legitimate resident formats (S-5); anything
	// else means a decoder produced a type the funnel never agreed to, which
	// is still worth dying for.
	if(origImageData->getImageType() != IMG_TYPE_RGBA &&
	   origImageData->getImageType() != IMG_TYPE_RGBA_16F)
	{
		SYS_FatalExit("Image type is %2.2x (should be %2.2x or %2.2x)",
				origImageData->getImageType(), IMG_TYPE_RGBA, IMG_TYPE_RGBA_16F);
	}

	this->loadImgWidth = origImageData->width;
	this->loadImgHeight = origImageData->height;
	this->rasterWidth = NextPow2(loadImgWidth);
	this->rasterHeight = NextPow2(loadImgHeight);
	this->origRasterWidth = rasterWidth;
	this->origRasterHeight = rasterHeight;
	
	this->width = loadImgWidth;
	this->height = loadImgHeight;

	this->defaultTexStartX = 0.0f;
	this->defaultTexEndX = ((float)loadImgWidth / (float)rasterWidth);
	this->defaultTexStartY = 0.0f;
	this->defaultTexEndY = ((float)loadImgHeight / (float)rasterHeight);

	shouldDeallocLoadImageData = true;
	// The POT repack must keep the SOURCE's type: allocating RGBA8 for a float
	// image would give the buffer half the bytes it needs, and the upload --
	// which reads 8 bytes per pixel because residentFormat says so -- would run
	// off the end of it. (S-5: this is a fourth type-blind path, alongside the
	// three SYS_FatalExit guards.)
	const u8 srcImgType = origImageData->getImageType();
	const size_t srcBytesPerPixel = (srcImgType == IMG_TYPE_RGBA_16F) ? 8 : 4;
	this->loadImageData = new CImageData(rasterWidth, rasterHeight, srcImgType);
	this->loadImageData->AllocImage(false, true);
	this->loadImageData->floatIsSurfaceEncoded = origImageData->floatIsSurfaceEncoded;

	u8 *imageData = (u8*)this->loadImageData->resultData;

	if (rasterWidth == loadImgWidth && rasterHeight == loadImgHeight)
	{
		memcpy(this->loadImageData->resultData, origImageData->resultData,
			   (size_t)origImageData->width * origImageData->height * srcBytesPerPixel);
	}
	else if (srcImgType == IMG_TYPE_RGBA_16F)
	{
		// Row-wise, in the source's own units. The 8-bit branch below cannot
		// serve here: its per-pixel accessors are RGBA8, so every above-white
		// value would be clamped away by the very copy meant to preserve it.
		const size_t srcRow = (size_t)origImageData->width * srcBytesPerPixel;
		const size_t dstRow = (size_t)rasterWidth * srcBytesPerPixel;
		for (u32 y = 0; y < loadImgHeight; y++)
			memcpy(imageData + y * dstRow,
				   (const u8 *)origImageData->resultData + y * srcRow, srcRow);
	}
	else
	{
		for (u32 y = 0; y < loadImgHeight; y++)
		{
			for (u32 x = 0; x < loadImgWidth; x++)
			{
				u8 r,g,b,a;
				origImageData->GetPixelResultRGBA(x, y, &r, &g, &b, &a);
				if (a > 0)
				{
					this->loadImageData->SetPixelResultRGBA(x, y, r, g, b, a);
				}
				else
				{
					// win32 linear scale fix
					this->loadImageData->SetPixelResultRGBA(x, y, 0, 0, 0, 0);
				}
			}
		}
	}

	// BYTES per row, and the swap below must walk BYTES for the same reason:
	// indexing x*4 inside an 8-byte-per-pixel row flips only the first half of
	// every row and leaves the rest, which is a garbled image rather than a
	// failure.
	unsigned int w = (unsigned int)(rasterWidth * srcBytesPerPixel);

	if (flipImageVertically)
	{
		// Row-wise byte swap. The old loop indexed x*4 for x < rasterWidth,
		// which is only the whole row when a pixel IS four bytes -- on an
		// 8-byte float row it flipped the first half of every row and left the
		// rest, producing a garbled image rather than an upside-down one.
		for (int y = 0; y < loadImgHeight / 2; y++)
		{
			u8 *rowTop = imageData + (size_t)y * w;
			u8 *rowBot = imageData + (size_t)(loadImgHeight - 1 - y) * w;
			for (unsigned int b = 0; b < w; b++)
			{
				const u8 tmp = rowTop[b];
				rowTop[b] = rowBot[b];
				rowBot[b] = tmp;
			}
		}
	}

	this->widthD2 = this->width/2.0;
	this->heightD2 = this->height/2.0;
	this->widthM2 = this->width*2.0;
	this->heightM2 = this->height*2.0;

	// debug pause
	//SYS_Sleep(100);

	//LOGR("image width=%3.2f height=%3.2f", width, height);

	this->resourcePriority = resourcePriority;
	this->resourceLoadingSize = rasterWidth * rasterHeight * 4 * 2;
	this->resourceIdleSize = rasterWidth * rasterHeight * 4;

	this->resourceIsActive = false;
	this->resourceState = RESOURCE_STATE_PRELOADING;
}

void CSlrImage::LoadImageForRebinding(CImageData *origImageData, u8 resourcePriority)
{
	// KTX2 compressed (.ktx2) branch. As in the RGBA rebinding path the caller
	// retains ownership of origImageData (shouldDeallocLoadImageData = false);
	// the compressed mips are borrowed, not stolen.
	if (origImageData->isCompressed)
	{
		if (shouldDeallocLoadImageData && this->loadImageData != NULL)
			delete this->loadImageData;

		this->shouldDeallocLoadImageData = false;
		this->loadImageData = origImageData;

		this->SetCompressedImageMetadata(origImageData, resourcePriority);

		this->resourceType = RESOURCE_TYPE_IMAGE_DYNAMIC;
		return;
	}

	// The resident-format funnel: one decision, two answers.
	ApplyResidentFormat(origImageData);

	// RGBA8 and RGBA16F are both legitimate resident formats (S-5); anything
	// else means a decoder produced a type the funnel never agreed to, which
	// is still worth dying for.
	if(origImageData->getImageType() != IMG_TYPE_RGBA &&
	   origImageData->getImageType() != IMG_TYPE_RGBA_16F)
	{
		SYS_FatalExit("Image type is %2.2x (should be %2.2x or %2.2x)",
				origImageData->getImageType(), IMG_TYPE_RGBA, IMG_TYPE_RGBA_16F);
	}

	if (shouldDeallocLoadImageData && this->loadImageData != NULL)
		delete this->loadImageData;
	
	this->shouldDeallocLoadImageData = false;
	this->loadImageData = origImageData;
	this->loadImgWidth = origImageData->width;
	this->loadImgHeight = origImageData->height;
	this->rasterWidth = NextPow2(loadImgWidth);
	this->rasterHeight = NextPow2(loadImgHeight);
	this->origRasterWidth = rasterWidth;
	this->origRasterHeight = rasterHeight;
	this->width = loadImgWidth;
	this->height = loadImgHeight;
	
	// The caller supplies the raw decoded buffer at original pixel dimensions
	// (not padded to POT). CreateTexture will upload at loadImageData->width/height,
	// so UV coords cover the full [0,1] range.
	this->defaultTexStartX = 0.0f;
	this->defaultTexEndX = 1.0f;
	this->defaultTexStartY = 0.0f;
	this->defaultTexEndY = 1.0f;

	this->widthD2 = this->width/2.0;
	this->heightD2 = this->height/2.0;
	this->widthM2 = this->width*2.0;
	this->heightM2 = this->height*2.0;

	this->resourcePriority = resourcePriority;
	// A float image really does cost twice the bytes; saying otherwise here
	// under-reports it to the resource manager.
	const int bppHint = (origImageData->getImageType() == IMG_TYPE_RGBA_16F) ? 8 : 4;
	this->resourceLoadingSize = loadImgWidth * loadImgHeight * bppHint * 2;
	this->resourceIdleSize = loadImgWidth * loadImgHeight * bppHint;
	
	this->resourceIsActive = false;
	this->resourceState = RESOURCE_STATE_PRELOADING;
	this->resourceType = RESOURCE_TYPE_IMAGE_DYNAMIC;
}


void CSlrImage::RefreshImageParameters(CImageData *origImageData, u8 resourcePriority, bool flipImageVertically)
{
	// KTX2 compressed (.ktx2) branch. Re-derive the ~14 metadata fields from
	// the already-owned compressed loadImageData; no RGBA loadImageData is
	// allocated for a compressed image.
	if (origImageData->isCompressed)
	{
		this->SetCompressedImageMetadata(origImageData, resourcePriority);
		return;
	}

	// The resident-format funnel: one decision, two answers.
	ApplyResidentFormat(origImageData);

	// RGBA8 and RGBA16F are both legitimate resident formats (S-5); anything
	// else means a decoder produced a type the funnel never agreed to, which
	// is still worth dying for.
	if(origImageData->getImageType() != IMG_TYPE_RGBA &&
	   origImageData->getImageType() != IMG_TYPE_RGBA_16F)
	{
		SYS_FatalExit("Image type is %2.2x (should be %2.2x or %2.2x)",
				origImageData->getImageType(), IMG_TYPE_RGBA, IMG_TYPE_RGBA_16F);
	}

	this->loadImgWidth = origImageData->width;
	this->loadImgHeight = origImageData->height;
	this->rasterWidth = NextPow2(loadImgWidth);
	this->rasterHeight = NextPow2(loadImgHeight);
	this->origRasterWidth = rasterWidth;
	this->origRasterHeight = rasterHeight;
	this->width = loadImgWidth;
	this->height = loadImgHeight;

	this->defaultTexStartX = 0.0f;
	this->defaultTexEndX = ((float)loadImgWidth / (float)rasterWidth);
	this->defaultTexStartY = 0.0f;
	this->defaultTexEndY = ((float)loadImgHeight / (float)rasterHeight);

	this->shouldDeallocLoadImageData = true;
	// The SOURCE's type, not a hardcoded RGBA8. This entry point calls the
	// funnel above, so residentFormat may already say RGBA16F -- and allocating
	// a 4 bytes/px buffer for it would have the upload read 8 bytes/px straight
	// past the end of it. Latent while every caller passes RGBA8, which is
	// exactly how it would have survived to bite the first one that did not.
	const u8 refreshType = origImageData->getImageType();
	const int refreshBpp = (refreshType == IMG_TYPE_RGBA_16F) ? 8 : 4;
	this->loadImageData = new CImageData(rasterWidth, rasterHeight, refreshType);
	this->loadImageData->AllocImage(false, true);
	this->loadImageData->floatIsSurfaceEncoded = origImageData->floatIsSurfaceEncoded;
	this->loadImageData->contentMaxComponent = origImageData->contentMaxComponent;

	this->widthD2 = this->width/2.0;
	this->heightD2 = this->height/2.0;
	this->widthM2 = this->width*2.0;
	this->heightM2 = this->height*2.0;

	this->resourcePriority = resourcePriority;
	this->resourceLoadingSize = rasterWidth * rasterHeight * refreshBpp * 2;
	this->resourceIdleSize = rasterWidth * rasterHeight * refreshBpp;

	this->resourceIsActive = false;
	this->resourceState = RESOURCE_STATE_PRELOADING;
}


// be careful, it is a hack
void CSlrImage::SetLoadImageData(CImageData *imageData)
{
	this->shouldDeallocLoadImageData = false;
	this->loadImageData = imageData;
}

void CSlrImage::ReBindImageData(CImageData *imageData)
{
	VID_LockImageBindingMutex();
	// THE SIXTH ENTRY POINT, and the reason the funnel is one helper rather
	// than five copies: this one swapped loadImageData and rebound without ever
	// looking at the image type, so a 16-bit or float CImageData handed to it
	// reached upload unconverted. c64d calls this live
	// (CViewC64GoatTracker.cpp:119).
	this->ApplyResidentFormat(imageData);
	this->SetLoadImageData(imageData);
	this->ReBindImage();
	VID_UnlockImageBindingMutex();
}

void CSlrImage::PreloadImage(CSlrFile *imgFile)
{
	if (imgFile == NULL)
	{
		SYS_FatalExit("PreloadImage: imgFile NULL");
	}

	u8 magic = imgFile->ReadByte();
	if (magic != GFX_BYTE_MAGIC1)
	{
		SYS_FatalExit("PreloadImage '%s': bad magic %2.2x", imgFile->fileName, magic);
	}

	u16 version = imgFile->ReadUnsignedShort();
	if (version > GFX_FILE_VERSION)
	{
		SYS_FatalExit("PreloadImage '%s': version not supported %4.4x", imgFile->fileName, version);
	}

	u8 gfxType = imgFile->ReadByte();
	if (gfxType != GFX_FILE_TYPE_RGBA)
	{
		SYS_FatalExit("PreloadImage '%s': type not supported %2.2x", imgFile->fileName, gfxType);
	}

	u32 targetScreenWidth = imgFile->ReadUnsignedShort();
	u32 origImageWidth = imgFile->ReadUnsignedShort();
	u32 origImageHeight = imgFile->ReadUnsignedShort();
	u32 destScreenWidth = imgFile->ReadUnsignedShort();

	this->loadImgWidth = (float)imgFile->ReadUnsignedShort();
	this->loadImgHeight = (float)imgFile->ReadUnsignedShort();
	this->rasterWidth = (float)imgFile->ReadUnsignedShort();
	this->rasterHeight = (float)imgFile->ReadUnsignedShort();

	this->resourceLoadingSize = rasterWidth * rasterHeight * 4 * 2;
	this->resourceIdleSize = rasterWidth * rasterHeight * 4;

	this->resourceIsActive = false;
	this->resourceState = RESOURCE_STATE_PRELOADING;
}

namespace
{
        // stb_image callbacks that operate on a CSlrFile
    int jpegRead(void* user, char* data, int size)
    {
        CSlrFile* stream = static_cast<CSlrFile*>(user);
        return static_cast<int>(stream->Read((u8*)data, size));
    }
    void jpegSkip(void* user, int size)
    {
                LOGError("CSlrImage: jpegSkip=%d not implemented", size);
                CSlrFile* stream = static_cast<CSlrFile*>(user);
                stream->Seek(stream->Tell() + size);
    }
    int jpegEof(void* user)
    {
        CSlrFile* stream = static_cast<CSlrFile*>(user);
        return stream->Eof();
    }
}

void CSlrImage::LoadImage(CSlrFile *imgFile)
{
	if (imgFile == NULL)
	{
		SYS_FatalExit("LoadImage: imgFile NULL");
	}

	// Peek the first byte, then rewind -- neither branch below may consume it
	// before this decides which one it is. GFX_BYTE_MAGIC1 ('g') is our own
	// preload binary format; nothing else starts a file with it (PNG 0x89,
	// JPEG 0xFF, BMP 'B', GIF 'G' uppercase, TIFF 'I'/'M').
	u8 magic = imgFile->ReadByte();
	imgFile->Seek(0);

	if (magic != GFX_BYTE_MAGIC1)
	{
		// Not our own format: decode as an ordinary still image via
		// stb_image's own content sniffing (PNG/JPEG/BMP/GIF/TGA/PSD/...),
		// streamed straight off imgFile through the SAME CSlrFile callback
		// adapter (jpegRead/jpegSkip/jpegEof) the .gfx container's embedded-
		// JPEG payload already uses below (GFX_COMPRESSION_TYPE_JPEG*) --
		// imgFile may be memory- or zlib-backed with no real filesystem path,
		// so this cannot go through CImageData's path-based Load().
		stbi_io_callbacks callbacks;
		callbacks.read = &jpegRead;
		callbacks.skip = &jpegSkip;
		callbacks.eof  = &jpegEof;

		int w = 0, h = 0, channels = 0;
		u8 *pixels = stbi_load_from_callbacks(&callbacks, imgFile, &w, &h, &channels, STBI_rgb_alpha);
		if (pixels == NULL)
		{
			SYS_FatalExit("LoadImage '%s': not a recognized image (magic %2.2x) -- %s",
						  imgFile->fileName, magic, stbi_failure_reason());
		}

		// Hand the decoded RGBA off through the SAME path/CImageData-based
		// LoadImage() below uses (LoadImage(CImageData*, u8)) -- one place
		// does the power-of-two padding, resident-format funnel and copy, for
		// every entry point that produces plain decoded RGBA.
		CImageData *imageData = new CImageData(w, h);
		memcpy(imageData->resultData, pixels, (size_t)w * (size_t)h * 4);
		stbi_image_free(pixels);

		this->LoadImage(imageData, RESOURCE_PRIORITY_NORMAL);
		delete imageData;
		return;
	}

	// Our own format: re-consume the magic byte the peek above rewound past,
	// then parse exactly as before.
	imgFile->ReadByte();

	u16 version = imgFile->ReadUnsignedShort();
	if (version > GFX_FILE_VERSION)
	{
		SYS_FatalExit("LoadImage '%s': version not supported %4.4x", imgFile->fileName, version);
	}

	u8 gfxType = imgFile->ReadByte();
	if (gfxType != GFX_FILE_TYPE_RGBA)
	{
		SYS_FatalExit("LoadImage '%s': type not supported %2.2x", imgFile->fileName, gfxType);
	}

	u32 targetScreenWidth = imgFile->ReadUnsignedShort();
	u32 origImageWidth = imgFile->ReadUnsignedShort();
	u32 origImageHeight = imgFile->ReadUnsignedShort();
	u32 destScreenWidth = imgFile->ReadUnsignedShort();

	this->loadImgWidth = (float)imgFile->ReadUnsignedShort();
	this->loadImgHeight = (float)imgFile->ReadUnsignedShort();
	this->rasterWidth = (float)imgFile->ReadUnsignedShort();
	this->rasterHeight = (float)imgFile->ReadUnsignedShort();

	LOGR("... targetScreenWidth=%d", targetScreenWidth);
	LOGR("... origImageWidth=%d", origImageWidth);
	LOGR("... origImageHeight=%d", origImageHeight);
	LOGR("... destScreenWidth=%d", destScreenWidth);
	LOGR("... imageWidth=%d", loadImgWidth);
	LOGR("... imageHeight=%d", loadImgHeight);

	this->width = ((float)origImageWidth);
	this->height = ((float)origImageHeight);

	LOGR("... rasterWidth=%f", rasterWidth);
	LOGR("... rasterHeight=%f", rasterHeight);

	this->defaultTexStartX = 0.0f;
	this->defaultTexEndX = ((float)loadImgWidth / (float)rasterWidth);
	this->defaultTexStartY = 0.0f;
	this->defaultTexEndY = ((float)loadImgHeight / (float)rasterHeight);

	this->gfxScale = (float)loadImgWidth / (float)origImageWidth;
	LOGR("... gfxScale=%3.2f", this->gfxScale);

	this->origRasterWidth = rasterWidth / gfxScale;
	this->origRasterHeight = rasterWidth / gfxScale;

	u8 compressionType = imgFile->ReadByte();
	u32 numBytes = rasterWidth * rasterHeight * 4;

	u8 *imageBuffer = NULL;

	if (compressionType == GFX_COMPRESSION_TYPE_UNCOMPRESSED)
	{
		imgFile->Read(imageBuffer, numBytes);
	}
	else if (compressionType == GFX_COMPRESSION_TYPE_ZLIB)
	{
		imageBuffer = new u8[numBytes];

		u32 compSize = imgFile->ReadUnsignedInt();

		CSlrFileZlib *fileZlib = new CSlrFileZlib(imgFile);
		fileZlib->Read(imageBuffer, numBytes);

		delete fileZlib;

	}
    else if (compressionType == GFX_COMPRESSION_TYPE_JPEG)
    {
		u32 compSize = imgFile->ReadUnsignedInt();

		stbi_io_callbacks callbacks;
		callbacks.read = &jpegRead;
		callbacks.skip = &jpegSkip;
		callbacks.eof  = &jpegEof;

		int jpegWidth, jpegHeight, jpegChannels;
		imageBuffer = stbi_load_from_callbacks(&callbacks, imgFile, &jpegWidth, &jpegHeight, &jpegChannels, STBI_rgb_alpha);

		//LOGD("failure=%s", stbi_failure_reason());

		LOGR("jpeg loaded: width=%d height=%d channels=%d", jpegWidth, jpegHeight, jpegChannels);

//              CByteBuffer *buf = new CByteBuffer();
//              buf->PutBytes((byte*)this->loadImageData, width * height * 4);
//              buf->storeToDocuments("TESTJPEG");
//              LOGD("stored TESTJPEG");

    }
    else if (compressionType == GFX_COMPRESSION_TYPE_JPEG_ZLIB)
	{
		u32 compSize = imgFile->ReadUnsignedInt();

		CSlrFileZlib *fileZlib = new CSlrFileZlib(imgFile);
		fileZlib->fileSize = compSize;


		stbi_io_callbacks callbacks;
		callbacks.read = &jpegRead;
		callbacks.skip = &jpegSkip;
		callbacks.eof  = &jpegEof;

		int jpegWidth, jpegHeight, jpegChannels;
		imageBuffer = stbi_load_from_callbacks(&callbacks, fileZlib, &jpegWidth, &jpegHeight, &jpegChannels, STBI_rgb_alpha);

		//LOGD("failure=%s", stbi_failure_reason());

		LOGR("jpeg-zlib loaded: width=%d height=%d channels=%d", jpegWidth, jpegHeight, jpegChannels);

		delete fileZlib;
	}
	else SYS_FatalExit("unknown compression");

	this->shouldDeallocLoadImageData = true;
	this->loadImageData = new CImageData(rasterWidth, rasterHeight, IMG_TYPE_RGBA, imageBuffer);

	this->widthD2 = this->width/2.0;
	this->heightD2 = this->height/2.0;
	this->widthM2 = this->width*2.0;
	this->heightM2 = this->height*2.0;

	this->resourceLoadingSize = rasterWidth * rasterHeight * 4 * 2;
	this->resourceIdleSize = rasterWidth * rasterHeight * 4;

	this->resourceIsActive = false;
	this->resourceState = RESOURCE_STATE_PRELOADING;
}

CSlrImage::~CSlrImage()
{
	/*
	 if (dataBuffer != NULL)
	 {
	 //LOGG("freeing image buffer\n");
	 SYS_FREE(dataBuffer);
	 }
	 if (alphaBuffer != NULL)
	 {
	 //LOGG("freeing alpha buffer\n");
	 SYS_FREE(alphaBuffer);
	 }
	 */

	if (this->isFromAtlas == false && this->isBound == true)
	{
		gRenderBackend->DeleteTexture(this);
	}
}
void CSlrImage::SetImageData(CImageData *imageData)
{
	this->SetLoadImageData(imageData);
	this->PostReBind();
}

void CSlrImage::PostReBind()
{
	VID_PostImageBinding(this, NULL, BINDING_MODE_DONT_FREE_IMAGEDATA);
}

void CSlrImage::InitImageLoad(bool linearScaling)
{
	this->isCompressed = false;
	this->compressedMipCount = 0;
	this->isFromAtlas = false;
	this->isBound = false;
	this->imgAtlas = NULL;
	this->linearScaling = linearScaling;
	this->texturePtr.store(NULL, std::memory_order_relaxed);
	this->cacheKey = 0;
	this->cacheLinearScaling = linearScaling;
}

void CSlrImage::BindImage()
{
	//LOGD("BindImage()");

	if (isBound)
		return;

#if !defined(FINAL_RELEASE)
	if (loadImageData == NULL)
	{
		SYS_FatalExit("CSlrImage::BindImage: loadImageData NULL %s", this->ResourceGetPath());
		return;
	}
#endif

//	if (!gMainContext || ![EAGLContext setCurrentContext:gMainContext])
//		SYS_FatalExit("BindImage() self current context failed");

//	if (this->loadImage == NULL)
//		SYS_FatalExit("BindImage() loadImage NULL");

	//	[EAGLContext setCurrentContext:[gMainContext in EAGLView] ];

	gRenderBackend->CreateTexture(this);

	isBound = true;
	isActive = true;

	resourceIsActive = true;
	resourceState = RESOURCE_STATE_LOADED;
	resourceActivatedTime.store(gCurrentFrameTime, std::memory_order_relaxed);
	if (cacheKey != 0)
	{
		RES_CacheTouch(this);
	}

}

void CSlrImage::SetLinearScaling(bool isLinearScaling)
{
	this->linearScaling = isLinearScaling;
	if (isBound)
		gRenderBackend->UpdateTextureLinearScaling(this);
}

void CSlrImage::ReBindImage()
{
	//LOGD("BindImage()");
	
#if !defined(FINAL_RELEASE)
	if (loadImageData == NULL)
	{
		SYS_FatalExit("CSlrImage::BindImage: loadImageData NULL %s", this->ResourceGetPath());
		return;
	}
#endif

	gRenderBackend->ReBindTexture(this);
}


void CSlrImage::FreeLoadImage()
{
//	LOGD("------------------------------------------------------ FreeLoadImage");
//	RES_DebugPrintMemory();

	if (shouldDeallocLoadImageData && loadImageData != NULL)
	{
		delete loadImageData;
	}
//	LOGD("------------------------------------------------------ FreeLoadImage done");
//	RES_DebugPrintMemory();

	shouldDeallocLoadImageData = false;
	loadImageData = NULL;
}

void CSlrImage::Deallocate()
{
//	LOGD("CSlrImage::Deallocate");

//	RES_DebugPrintMemory();

	this->FreeLoadImage();

	gRenderBackend->DeleteTexture(this);
	this->texturePtr.store(NULL, std::memory_order_release);

	this->isBound = false;

//	LOGD("CSlrImage::Deallocate: done");
//	RES_DebugPrintMemory();
}

CImageData *CSlrImage::GetImageData(float *imageScale, u32 *width, u32 *height)
{
	*imageScale = this->gfxScale;
	*width = this->loadImgWidth;
	*height = this->loadImgHeight;

	return this->loadImageData;
}


CSlrImage::CSlrImage(CSlrImage *imgAtlas, float startX, float startY, float width, float height, float downScale, const char *name)
{
	LOGD("ImageFromAtlas: '%s'", name); // %f %f %f %f", name, startX, startY, width, height);

	this->name = name;

	//	this->InitFromAtlas(CSlrImage *imgAtlas, int startX, int startY, int endX, int endY, float downScale);

	this->isFromAtlas = true;
	this->imgAtlas = imgAtlas;
	//(texture[0]) = &(texture[0]);

	if (!(ispow2((int)imgAtlas->rasterWidth)))
		SYS_FatalExit("ImgAtlas is !pow2");

	float atlStartX = 0;
	float atlStartY = 0;
	float atlEndX = 0;
	float atlEndY = 0;

	/*
	if (IMAGE_SCALE_DOWN)
	{
		this->width = width; //(float)((float)atlEndX - (float)atlStartX);
		this->height = height; //(float)((float)atlEndY - (float)atlStartY);

		atlStartX = startX/2; // / (float)(2.0f); //startX >> 1;
		atlEndX = startX/2 + this->width; // / (float)(2.0f); //endX >> 1;
		atlStartY = startY/2; // / (float)(2.0f); //startY >> 1;
		atlEndY = startY/2 + this->height; // / (float)(2.0f); //endY >> 1;

	}
	else if (IMAGE_SCALE_ORIGINAL)*/
	{
		this->width = width; //(float)((float)atlEndX - (float)atlStartX);
		this->height = height; //(float)((float)atlEndY - (float)atlStartY);

		atlStartX = startX;
		atlEndX = startX + width;
		atlStartY = startY;
		atlEndY = startY + height;
	}

	/*
	 2010-12-14 13:52:26.185 KidsChristmasTree[6832:207] InitFromAtlas: 1.000000 1.000000 95.000000 95.000000
	 2010-12-14 13:52:26.189 KidsChristmasTree[6832:207] 0.000977 0.092773 0.000977 0.092773
	 */

	this->rasterWidth = imgAtlas->rasterWidth;
	this->rasterHeight = imgAtlas->rasterHeight;

	this->defaultTexStartX = ((float)atlStartX / (float)imgAtlas->origRasterWidth);
	this->defaultTexEndX = ((float)atlEndX / (float)imgAtlas->origRasterWidth);
	this->defaultTexStartY = ((float)atlStartY / (float)imgAtlas->origRasterHeight);
	this->defaultTexEndY = ((float)atlEndY / (float)imgAtlas->origRasterHeight);
	//LOGD("%f %f %f %f", defaultTexStartX, defaultTexEndX, defaultTexStartY, defaultTexEndY);

	this->widthD2 = this->width/2.0;
	this->heightD2 = this->height/2.0;
	this->widthM2 = this->width*2.0;
	this->heightM2 = this->height*2.0;
}

// resource manager
// should preload resource and set resource size
bool CSlrImage::ResourcePreload(const char *fileName, bool fromResources)
{
	this->resourceIsActive = false;
	this->PreloadImage(fileName, fromResources);
	return true;
}

// get size of resource in bytes
u32 CSlrImage::ResourceGetLoadingSize()
{
	return this->resourceLoadingSize;
}

u32 CSlrImage::ResourceGetIdleSize()
{
	return this->resourceIdleSize;
}

void *CSlrImage::TexturePtr()
{
	void *ptr = texturePtr.load(std::memory_order_acquire);
	if (cacheKey != 0 && ptr == NULL && resourceState == RESOURCE_STATE_DEALLOCATED && resourcePath != NULL)
	{
		RES_CacheGetImage(resourcePath, cacheLinearScaling);
		ptr = texturePtr.load(std::memory_order_acquire);
	}
	if (cacheKey != 0 && ptr != NULL)
	{
		resourceActivatedTime.store(gCurrentFrameTime, std::memory_order_relaxed);
	}
	return ptr;
}
