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
#include "Render/ERenderTextureFormat.h"

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

// THE RESIDENT-FORMAT FUNNEL (S-5).
//
// A decoder may produce any decode-time type, but exactly one decision turns
// that into a resident format, and it has only two answers. Pure and free of
// any backend so it can be tested without one -- the caller passes the
// backend's answer rather than the function reaching for it:
//
//   IMG_TYPE_RGBA        -> RGBA8
//   IMG_TYPE_RGBA_16BIT  -> RGBA8   (unchanged: unorm16 is about PRECISION,
//                                    and promoting it to float would gain no
//                                    range while doubling the memory)
//   IMG_TYPE_RGBA_16F    -> RGBA16F when the backend can take it, else RGBA8
//                                    via the tone-map
//   anything else        -> RGBA8   (the guards downstream reject it)
ERenderTextureFormat SlrResidentFormatFor(u8 decodeType, bool backendSupportsFloat);

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

	// Applies SlrResidentFormatFor to `imageData`, converting IN PLACE when the
	// answer is RGBA8 but the data is not, and setting residentFormat.
	//
	// ONE helper rather than the same two lines at six entry points: five
	// copies of a two-answer decision is precisely how the sixth entry point
	// (ReBindImageData) came to exist without one.
	void ApplyResidentFormat(CImageData *imageData);

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

	// What the GPU texture is made of. Set by the resident-format funnel
	// (SlrResidentFormatFor) at load time and read by CreateTexture /
	// ReBindTexture. RGBA8 until something decides otherwise, so every
	// existing path keeps exactly today's behaviour.
	ERenderTextureFormat residentFormat = RENDER_TEXTURE_RGBA8;

	// The format the LIVE texture was actually created with. A rebind that
	// finds these two disagreeing must destroy and recreate rather than write:
	// neither backend's rebind reallocates storage (GL glTexSubImage2D, Metal
	// replaceRegion), so writing 8-byte pixels into a 4-byte allocation is a
	// buffer overrun, not a wrong colour.
	ERenderTextureFormat boundFormat = RENDER_TEXTURE_RGBA8;

	// The content's peak linear value, carried up from the CImageData by the
	// funnel so the viewer can describe what it is displaying without holding
	// on to the decode buffer.
	float contentMaxComponent = 0.0f;

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
