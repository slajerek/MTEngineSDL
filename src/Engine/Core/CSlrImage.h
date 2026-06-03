/*
 *  CSlrImage.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-23.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef __VID_CSLRIMAGE_H__
#define __VID_CSLRIMAGE_H__

// TODO: rename engine callbacks to not confuse with data load (i.e. ReBind image vs ReBind data image)

#include "SYS_Defs.h"
#include "CImageData.h"
//#include "OpenGLCommon.h"
//#include "XF_Files.h"
//#include "VID_CAppView.h"
#include "CSlrFile.h"
#include "CSlrImageTexture.h"

#include <atomic>

class CAtomicTexturePtr
{
public:
	CAtomicTexturePtr()
	{
		value.store(NULL, std::memory_order_relaxed);
	}

	void *load(std::memory_order order = std::memory_order_seq_cst) const
	{
		return value.load(order);
	}

	void store(void *ptr, std::memory_order order = std::memory_order_seq_cst)
	{
		value.store(ptr, order);
	}

	operator void *() const
	{
		return value.load(std::memory_order_seq_cst);
	}

	CAtomicTexturePtr &operator=(void *ptr)
	{
		value.store(ptr, std::memory_order_seq_cst);
		return *this;
	}

private:
	std::atomic<void *> value;
};

class CSlrImage : public CSlrImageTexture
{
public:
	// load from resources
	CSlrImage(CImageData *imageData);
	CSlrImage(CImageData *imageData, bool linearScaling);
	CSlrImage(CImageData *imageData, bool linearScaling, bool bindNow);
	CSlrImage(const char *fileName);
	CSlrImage(CSlrFile *imgFile, bool linearScaling);
	CSlrImage(const char *fileName, bool linearScaling);
	CSlrImage(const char *fileName, bool linearScaling, bool fromResources);
	//CSlrImage(NSString *fileName, bool linearScaling);
	//CSlrImage(NSString *fileName, NSString *fileExt, bool linearScaling);

	// delayed load
	CSlrImage(bool delayedLoad, bool linearScaling);

	// init from img atlas
	CSlrImage(CSlrImage *imgAtlas, float startX, float startY, float width, float height, float downScale, const char *name);

	virtual ~CSlrImage();

	void InitImageLoad(bool linearScaling);

	void DelayedLoadImage(const char *fileName, bool fromResources);
	void PreloadImage(const char *fileName, bool fromResources);
	void LoadImage(const char *fileName, const char *fileExt);
	void LoadImage(CImageData *imageData);
	void LoadImage(CImageData *imageData, u8 resourcePriority);
	void LoadImage(CImageData *imageData, u8 resourcePriority, bool flipImageVertically);
	void LoadImageForRebinding(CImageData *origImageData, u8 resourcePriority);
	void RefreshImageParameters(CImageData *imageData, u8 resourcePriority, bool flipImageVertically);

	// KTX2 compressed-image support. Shared metadata helper — sets ALL ~14
	// LoadImage member fields for a GPU-compressed image so no compressed
	// branch ever early-returns with half-initialised metadata.
	void SetCompressedImageMetadata(CImageData *compressedImageData, u8 resourcePriority);

	void PreloadImage(CSlrFile *imgFile);
	void LoadImage(CSlrFile *imgFile);

	void SetLoadImageData(CImageData *imageData);
    void ReBindImageData(CImageData *imageData);
	void SetLinearScaling(bool isLinearScaling);
	
	void SetImageData(CImageData *imageData);
	void PostReBind();
	
	u8 fileLoadError;

	void BindImage();
	void ReBindImage();
	void FreeLoadImage();

	const char *name;

	bool linearScaling;

	bool isFromAtlas;
	CSlrImage *imgAtlas;

	CAtomicTexturePtr texturePtr;
	u64 cacheKey;
	bool cacheLinearScaling;

	float rasterHeight;
	float rasterWidth;

	float defaultTexStartX;
	float defaultTexEndX;
	float defaultTexStartY;
	float defaultTexEndY;

	float downScale;

	void DrawLine(float x1, float y1, float x2, float y2);
	void *TexturePtr();

	CImageData *GetImageData(float *imageScale, u32 *width, u32 *height);

public:
	CImageData *loadImageData;
	u32 loadImgWidth;
	u32 loadImgHeight;

	bool shouldDeallocLoadImageData;

	// KTX2 compressed-image support. For an RGBA image these stay false/0 and
	// are never read; the render backend uses them to take the compressed
	// upload path (Task 4.5c).
	bool isCompressed;
	int compressedMipCount;
	
	float gfxScale;

	float origRasterWidth;
	float origRasterHeight;

	virtual void Deallocate();
	virtual bool ResourcePreload(const char *fileName, bool fromResources);
	virtual u32 ResourceGetLoadingSize();
	virtual u32 ResourceGetIdleSize();
	bool DelayedLoadImageNoFail(const char *fileName, bool fromResources);


};


#endif // __VID_CSLRIMAGE_H__
