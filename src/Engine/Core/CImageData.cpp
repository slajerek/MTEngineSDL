#include "CImageData.h"
#include "SYS_Defs.h"
#include "SYS_Funct.h"
// SYS_FopenUtf8 rather than fopen throughout this file: image file names are
// UTF-8 by engine convention, and fopen decodes a char* with the process ANSI
// code page on Windows, so a non-ASCII name failed to open. See SYS_FileUtf8.h.
#include "SYS_FileUtf8.h"
// png.h ONLY. pnginfo.h and pngpriv.h are libpng's PRIVATE headers and were
// included here for the read path that has been commented out for years (it
// called png_read_destroy, which lives in pngpriv.h). libpng 1.5 let an
// application include them; 1.6 refuses with an #error, and it is right to --
// they describe struct internals that are not API. The live code uses the
// public write API only.
#include "png.h"
#include "lodepng.h"
#include "CByteBuffer.h"
#include "IMG_Scale.h"
#include "GFX_Types.h"
#include "zlib.h"
#include "CIccProfileCodec.h"
#include "CExifReader.h"
#include "JPEGWriter.h"
#include "CSlrFileZlib.h"
#include "CSlrFileMemory.h"
#include "stb_image.h"
#include "VID_Main.h"           // gRenderBackend + (transitively, via CRenderBackend.h) EImageGpuFormat.h
#include "basisu_transcoder.h"  // KTX2/UASTC transcoder
#include "CKTX2Loader.h"

#if defined(ANDROID)
#include "SYS_ApkManager.h"
#endif

#include <map>

#define PEDANTIC

//#define FLIP_VERTICAL

struct CImageDataRowIter
{
	CImageDataRowIter(CImageData *imageData): buffer(imageData->width * 3)
	{
		this->imageData = imageData;
		y = 0;
	}
	
	unsigned char* operator*()
	{
		return &buffer[0];
	}
	
	void operator++()
	{
		unsigned i = 0;
		for (unsigned x = 0; x < imageData->width; ++x)
		{
			u8 r,g,b,a;
			
			if (imageData->type == IMG_TYPE_RGB)
			{
				imageData->GetPixelResultRGB(x, y, &r, &g, &b);
				a = 255;
			}
			else if (imageData->type == IMG_TYPE_RGBA)
			{
				imageData->GetPixelResultRGBA(x, y, &r, &g, &b, &a);
			}
			else if (imageData->type == IMG_TYPE_GRAYSCALE)
			{
				u8 v = imageData->GetPixelResultByte(x, y);
				r = g = b = v;
				a = 255;
			}
			buffer[i++] = r;
			buffer[i++] = g;
			buffer[i++] = b;
		}
		
		y++;
	}
	
	int y;
	CImageData *imageData;
	std::vector<unsigned char> buffer;
};

// extension of a file name (with leading dot), "" if none.
static inline const char *IMG_FileExtension(const char *fileName)
{
	if (!fileName) return "";
	const char *dot = strrchr(fileName, '.');
	return dot ? dot : "";
}

CImageData::CImageData()
{
	this->tempData = NULL;
	this->resultData = NULL;
	this->row_pointers = NULL;
	this->type = IMG_TYPE_UNKNOWN;
	this->mask = NULL;
	this->width = 0;
	this->height = 0;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;
}

CImageData::CImageData(const char *fileName)
{
	LOGR("CImageData::CImageData: '%s'", fileName);
	//this->origData = NULL;
	this->tempData = NULL;
	this->resultData = NULL;
	this->row_pointers = NULL;
	this->type = IMG_TYPE_GRAYSCALE;
	this->mask = NULL;
	this->width = 0;
	this->height = 0;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;

	// dispatch by extension: Load() handles all format routing.
	this->Load(fileName, true);

#ifdef USE_BUFFER_OFFSETS
	if (this->type != IMG_TYPE_UNKNOWN && this->type != IMG_TYPE_GPU_COMPRESSED)
	{
		this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
	}
#endif

	//LOGD("pool test CImageData: %d", ++poolTestCImageData);
}

CImageData::CImageData(CByteBuffer *byteBuffer)
{
	LOGR("CImageData::CImageData() from byteBuffer");
	//this->origData = NULL;
	this->tempData = NULL;
	this->resultData = NULL;
	this->row_pointers = NULL;
	this->type = IMG_TYPE_GRAYSCALE;
	this->mask = NULL;
	this->width = 0;
	this->height = 0;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;
	this->LoadFromByteBufferUncompressed(byteBuffer);
	
#ifdef USE_BUFFER_OFFSETS
	if (this->type != IMG_TYPE_UNKNOWN)
	{
		this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
	}
#endif
	
	//LOGD("pool test CImageData: %d", ++poolTestCImageData);
}

// assuming type RGBA
CImageData::CImageData(int width, int height)
{
	this->width = width;
	this->height = height;
	//this->originalHeight = height;
	//this->originalWidth = width;
	//this->origData = NULL;
	this->tempData = NULL;
	this->resultData = NULL;
	this->row_pointers = NULL;
	this->type = IMG_TYPE_RGBA;
	this->mask = NULL;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;

#ifdef USE_BUFFER_OFFSETS
	this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
#endif

	this->AllocImage(false, true);
	
	//LOGD("pool test CImageData: %d", ++poolTestCImageData);
}

CImageData::CImageData(int width, int height, u8 type)
{
	this->width = width;
	this->height = height;
	//this->originalHeight = height;
	//this->originalWidth = width;
	//this->origData = NULL;
	this->tempData = NULL;
	this->resultData = NULL;
	this->row_pointers = NULL;
	this->type = type;
	this->mask = NULL;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;

#ifdef USE_BUFFER_OFFSETS
	this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
#endif

	//LOGD("pool test CImageData: %d", ++poolTestCImageData);
}

CImageData::CImageData(int width, int height, u8 type, bool allocTemp, bool allocResult)
{
	this->width = width;
	this->height = height;
	//this->originalHeight = height;
	//this->originalWidth = width;
	//this->origData = NULL;
	this->tempData = NULL;
	this->resultData = NULL;
	this->row_pointers = NULL;
	this->type = type;
	this->mask = NULL;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;

#ifdef USE_BUFFER_OFFSETS
	this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
#endif

	//LOGD("pool test CImageData: %d", ++poolTestCImageData);

	this->AllocImage(allocTemp, allocResult);
}


CImageData::CImageData(int width, int height, u8 type, u8 *data)
{
	this->width = width;
	this->height = height;
	//this->originalHeight = height;
	//this->originalWidth = width;
	this->type = type;
	//this->origData = NULL;
	this->tempData = NULL;
	this->resultData = data;
	this->row_pointers = NULL;
	this->mask = NULL;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;

#ifdef USE_BUFFER_OFFSETS
	this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
#endif

	//LOGD("pool test CImageData: %d", ++poolTestCImageData);
}

CImageData::CImageData(CImageData *src)
{
	// GPU-compressed images are immutable, load-once, and ownership-transferred,
	// never deep-copied. The copy ctor memcpy's src->resultData, which is NULL
	// for a compressed image -> crash. Guard it as unreachable (design note §2).
	if (src->isCompressed || src->type == IMG_TYPE_GPU_COMPRESSED)
	{
		SYS_FatalExit("CImageData copy ctor: GPU-compressed images cannot be deep-copied");
	}

	this->width = src->width;
	//this->originalWidth = src->width;
	this->height = src->height;
	//this->originalHeight = src->height;
	this->type = src->type;
	this->mask = NULL;
	this->isCompressed = false;
	this->compressedGpuFormat = 0;
	this->compressedMipCount = 0;
	this->compressedMips = NULL;

	//this->origData = NULL;
	this->tempData = NULL;

	// Deep-copy the profile. A clone that silently dropped it would be
	// indistinguishable from a genuinely untagged image, and would then be
	// rendered with the assumed profile instead of its own -- a wrong-colour
	// bug with no error path.
	if (src->iccProfile != NULL && src->iccProfileSize > 0)
		SetIccProfile(src->iccProfile, src->iccProfileSize);
	// The preview hint describes the same pixels, so it travels with them.
	this->previewColorHint       = src->previewColorHint;
	this->previewColorHintSource = src->previewColorHintSource;

	switch(this->type)
	{
		default:
			LOGError("unknown image type: %2.2x", this->type);
			break;
		case IMG_TYPE_GRAYSCALE:
			this->resultData = new u8[width * height];
			memcpy(this->resultData, src->resultData, width * height);
			if (src->tempData)
			{
				this->tempData = new u8[width * height];
				memcpy(this->tempData, src->tempData, width * height);
			}
			break;
		case IMG_TYPE_GRAYSCALE_16BIT:
			this->resultData = (u8*)new unsigned short int[width * height];
			memcpy(this->resultData, src->resultData, width * height * sizeof(unsigned short int));
			if (src->tempData)
			{
				// Sized to what is COPIED -- new u8[w*h] here was a heap overflow
				// for every non-grayscale type (programme review 2026-08-11).
				this->tempData = new u8[width * height * sizeof(unsigned short int)];
				memcpy(this->tempData, src->tempData, width * height * sizeof(unsigned short int));
			}
			break;
		case IMG_TYPE_GRAYSCALE_32BIT:
			this->resultData = (u8*)new long unsigned int[width * height];
			memcpy(this->resultData, src->resultData, width * height * sizeof(unsigned long int));
			if (src->tempData)
			{
				this->tempData = new u8[width * height * sizeof(unsigned long int)];
				memcpy(this->tempData, src->tempData, width * height * sizeof(unsigned long int));
			}
			break;
		case IMG_TYPE_RGB:
			this->resultData = new u8[width * height * 3];
			memcpy(this->resultData, src->resultData, width * height * 3);
			if (src->tempData)
			{
				this->tempData = new u8[width * height * 3];
				memcpy(this->tempData, src->tempData,  width * height * 3);
			}
			break;
		case IMG_TYPE_RGBA_16BIT:
		case IMG_TYPE_RGBA_16F:
			this->floatIsSurfaceEncoded = src->floatIsSurfaceEncoded;
			this->contentMaxComponent = src->contentMaxComponent;
			this->resultData = (u8*)new unsigned short int[width * height * 4];
			memcpy(this->resultData, src->resultData, width * height * 4 * sizeof(unsigned short int));
			if (src->tempData)
			{
				this->tempData = new u8[width * height * 4 * sizeof(unsigned short int)];
				memcpy(this->tempData, src->tempData, width * height * 4 * sizeof(unsigned short int));
			}
			break;
		case IMG_TYPE_RGBA:
			this->resultData = new u8[width * height * 4];
			memcpy(this->resultData, src->resultData, width * height * 4);
			if (src->tempData)
			{
				this->tempData = new u8[width * height * 4];
				memcpy(this->tempData, src->tempData,  width * height * 4);
			}
			break;
		case IMG_TYPE_CIELAB:
			this->resultData = (u8*)new int[width * height * 3];
			memcpy(this->resultData, src->resultData, width * height * 3 * sizeof(int));
			if (src->tempData)
			{
				this->tempData = new u8[width * height * 3 * sizeof(int)];
				memcpy(this->tempData, src->tempData,  width * height * 3 * sizeof(int));
			}
			break;
	}
	this->row_pointers = NULL;

#ifdef USE_BUFFER_OFFSETS
	this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
#endif

	//LOGD("pool test CImageData: %d", ++poolTestCImageData);
}

CImageData::~CImageData()
{
	DeallocImage();
	if (row_pointers)
	{
		for (int y = 0; y < height; y++)
		{
			free(row_pointers[y]);
		}
		free(row_pointers);
		row_pointers = NULL;
	}
	// TODO: DeallocBufferOffsets (!)
}

uint8 *CImageData::GetResultDataAsRGBA()
{
	return (uint8*)this->resultData;
}

void CImageData::copyTemporaryToResult()
{
#ifdef MORE_PEDANTIC
	if (!this->tempData)
	{
		LOGError("copyTemporaryToResult: tempData null");
		SYS_FatalExit();
	}
#endif

	switch(this->type)
	{
		default:
			LOGError("unknown image type: %2.2x", this->type);
			break;
		case IMG_TYPE_GRAYSCALE:
			memcpy(this->resultData, this->tempData, width * height);
			break;
		case IMG_TYPE_GRAYSCALE_16BIT:
			memcpy(this->resultData, this->tempData, width * height * sizeof(unsigned short int));
			break;
		case IMG_TYPE_GRAYSCALE_32BIT:
			memcpy(this->resultData, this->tempData, width * height * sizeof(unsigned long int));
			break;
		case IMG_TYPE_RGB:
			memcpy(this->resultData, this->tempData, width * height * 3);
			break;
		case IMG_TYPE_RGBA:
			memcpy(this->resultData, this->tempData, width * height * 4);
			break;
		case IMG_TYPE_RGBA_16BIT:
		case IMG_TYPE_RGBA_16F:
			memcpy(this->resultData, this->tempData, width * height * 4 * sizeof(unsigned short int));
			break;
		case IMG_TYPE_CIELAB:
			memcpy(this->resultData, this->tempData, width * height * 3 * sizeof(int));
			break;
	}
}

void CImageData::copyResultToTemporary()
{
#ifdef MORE_PEDANTIC
	if (!this->resultData)
	{
		LOGError("copyResultToTemporary: resultData null");
		SYS_FatalExit();
	}
#endif

	switch(this->type)
	{
		default:
			LOGError("unknown image type: %2.2x", this->type);
			break;
		case IMG_TYPE_GRAYSCALE:
			memcpy(this->tempData, this->resultData, width * height);
			break;
		case IMG_TYPE_GRAYSCALE_16BIT:
			memcpy(this->tempData, this->resultData, width * height * sizeof(unsigned short int));
			break;
		case IMG_TYPE_GRAYSCALE_32BIT:
			memcpy(this->tempData, this->resultData, width * height * sizeof(unsigned long int));
			break;
		case IMG_TYPE_RGB:
			memcpy(this->tempData, this->resultData, width * height * 3);
			break;
		case IMG_TYPE_RGBA:
			memcpy(this->tempData, this->resultData, width * height * 4);
			break;
		case IMG_TYPE_RGBA_16BIT:
		case IMG_TYPE_RGBA_16F:
			memcpy(this->tempData, this->resultData, width * height * 4 * sizeof(unsigned short int));
			break;
		case IMG_TYPE_CIELAB:
			memcpy(this->tempData, this->resultData, width * height * 3 * sizeof(int));
			break;
	}
}


u8 CImageData::getImageType()
{
	return this->type;
}

void CImageData::setImageType(u8 type)
{
	this->type = type;
}

void CImageData::setResultImage(u8 *data, u8 type)
{
	this->type = type;
	this->resultData = data;
}

void CImageData::DeallocTemp()
{
	if (this->tempData)
	{
		switch(this->type)
		{
			default:
				SYS_FatalExit("image type unknown: %2.2x", this->type);
				////log_backtrace();
				break;
			case IMG_TYPE_GRAYSCALE:
			case IMG_TYPE_RGB:
			case IMG_TYPE_RGBA:
				//LOGD("delete data");
				delete [] (u8 *)tempData;
				break;
			case IMG_TYPE_GRAYSCALE_16BIT:
				delete [] (unsigned short int*)tempData;
				break;
			case IMG_TYPE_RGBA_16BIT:
			case IMG_TYPE_RGBA_16F:
				// new unsigned short int[w*h*4] -- the array form must match
				// the allocation, or this is a heap corruption rather than a
				// leak.
				delete [] (unsigned short int*)tempData;
				break;
			case IMG_TYPE_GRAYSCALE_32BIT:
				delete [] (unsigned long int*)tempData;
				break;
			case IMG_TYPE_CIELAB:
				delete [] (int *)tempData;
				break;
			case IMG_TYPE_GPU_COMPRESSED:
				// compressed images have no tempData; defensive no-op so a stray
				// pointer doesn't cryptically fatal-exit. Blocks live in compressedMips.
				break;
		}
	}
	this->tempData = NULL;
}

void CImageData::DeallocResult()
{
	if (this->resultData)
	{
		switch(this->type)
		{
			default:
				SYS_FatalExit("image type unknown: %2.2x", this->type);
				////log_backtrace();
				break;
			case IMG_TYPE_GRAYSCALE:
			case IMG_TYPE_RGB:
			case IMG_TYPE_RGBA:
				//LOGD("delete data");
				delete [] (u8 *)resultData;
				break;
			case IMG_TYPE_GRAYSCALE_16BIT:
				delete [] (unsigned short int*)resultData;
				break;
			case IMG_TYPE_RGBA_16BIT:
			case IMG_TYPE_RGBA_16F:
				// new unsigned short int[w*h*4] -- the array form must match
				// the allocation, or this is a heap corruption rather than a
				// leak.
				delete [] (unsigned short int*)resultData;
				break;
			case IMG_TYPE_GRAYSCALE_32BIT:
				delete [] (unsigned long int*)resultData;
				break;
			case IMG_TYPE_CIELAB:
				delete [] (int *)resultData;
				break;
			case IMG_TYPE_GPU_COMPRESSED:
				// compressed images have no resultData; defensive no-op so a stray
				// pointer doesn't cryptically fatal-exit. Blocks live in compressedMips.
				break;
		}
	}
	this->resultData = NULL;
}

void CImageData::SetIccProfile(const u8 *bytes, u32 size)
{
	DeallocIccProfile();
	if (bytes == NULL || size == 0)
		return;
	// Validate before storing. Loaders read these bytes out of untrusted files,
	// and a truncated or malformed profile handed to a CMM is a crash, not a
	// wrong colour -- so an image that would carry one stays untagged instead.
	if (!CIccProfileCodec::ValidateHeader(bytes, size))
	{
		LOGD("CImageData::SetIccProfile: rejected a malformed %d-byte profile", (int)size);
		return;
	}
	this->iccProfile = new u8[size];
	memcpy(this->iccProfile, bytes, size);
	this->iccProfileSize = size;
}

void CImageData::DeallocIccProfile()
{
	if (this->iccProfile)
	{
		delete [] this->iccProfile;
		this->iccProfile = NULL;
	}
	this->iccProfileSize = 0;
}

void CImageData::DeallocImage()
{
	//LOGD("DeallocImage");
	/*if (this->origData)
	{
		switch(this->type)
		{
			default:
				SYS_FatalExit("image type unknown: %2.2x", this->type);
				break;
			case IMG_TYPE_GRAYSCALE:
			case IMG_TYPE_RGB:
				//LOGD("delete data");
				delete (u8 *)origData;
				break;
			case IMG_TYPE_GRAYSCALE_16BIT:
				delete (unsigned short int*)origData;
				break;
			case IMG_TYPE_GRAYSCALE_32BIT:
				delete (unsigned long int*)origData;
				break;
			case IMG_TYPE_CIELAB:
				delete (int *)origData;
				break;
		}
	}*/

	this->DeallocTemp();
	this->DeallocResult();
	this->DeallocCompressed();   // free GPU-compressed mip buffers if any (no-op otherwise)
	this->DeallocIccProfile();
	if (this->mask)
	{
		delete [] this->mask;
		this->mask = NULL;
	}


	//this->origData = NULL;
	//LOGD("DeallocImage finished");
}

// Free GPU-compressed mip block buffers. Safe to call when none allocated
// (compressedMips == NULL), and idempotent across reload (design note §5).
void CImageData::DeallocCompressed()
{
	if (this->compressedMips)
	{
		for (int i = 0; i < this->compressedMipCount; i++)
		{
			delete [] this->compressedMips[i].blockData;
			this->compressedMips[i].blockData = NULL;
		}
		delete [] this->compressedMips;
		this->compressedMips = NULL;
	}
	this->compressedMipCount = 0;
	this->isCompressed = false;
}

void CImageData::AllocImage(bool allocTemp, bool allocResult)
{
	DeallocImage();

	/*
	if (allocOrig)
	{
		switch(this->type)
		{
			default:
				LOGError("unknown image type: %2.2x", this->type);
				break;
			case IMG_TYPE_GRAYSCALE:
				//LOGD("alloc grayscale");
				origData = new u8[this->width * this->height];
				memset(origData, 0x00, this->width * this->height);
				break;
			case IMG_TYPE_GRAYSCALE_16BIT:
				//LOGD("alloc grayscale");
				origData = new unsigned short int[this->width * this->height];
				memset(origData, 0x00, this->width * this->height * sizeof(unsigned short int));
				break;
			case IMG_TYPE_GRAYSCALE_32BIT:
				//LOGD("alloc grayscale");
				origData = new unsigned long int[this->width * this->height];
				memset(origData, 0x00, this->width * this->height * sizeof(unsigned long int));
				break;
			case IMG_TYPE_RGB:
				//LOGD("alloc RGB");
				origData = new u8[this->width * this->height * 3];
				memset(origData, 0x00, this->width * this->height * 3);
				break;
			case IMG_TYPE_CIELAB:
				//LOGD("alloc cielab");
				origData = new int[this->width * this->height * 3];
				memset(origData, 0x00, this->width * this->height * 3 * sizeof(int));
				break;
		}
	}*/

	if (allocTemp)
	{
		switch(this->type)
		{
			default:
				LOGError("unknown image type: %2.2x", this->type);
				break;
			case IMG_TYPE_GRAYSCALE:
				//LOGD("alloc grayscale");
				tempData = new u8[this->width * this->height];
				memset(tempData, 0x00, this->width * this->height);
				break;
			case IMG_TYPE_GRAYSCALE_16BIT:
				//LOGD("alloc grayscale");
				tempData = (u8*)new unsigned short int[this->width * this->height];
				memset(tempData, 0x00, this->width * this->height * sizeof(unsigned short int));
				break;
			case IMG_TYPE_GRAYSCALE_32BIT:
				//LOGD("alloc grayscale");
				tempData = (u8*)new unsigned long int[this->width * this->height];
				memset(tempData, 0x00, this->width * this->height * sizeof(unsigned long int));
				break;
			case IMG_TYPE_RGB:
				//LOGD("alloc RGB");
				tempData = new u8[this->width * this->height * 3];
				memset(tempData, 0x00, this->width * this->height * 3);
				break;
			case IMG_TYPE_RGBA:
				//LOGD("alloc RGB");
				tempData = new u8[this->width * this->height * 4];
				memset(tempData, 0x00, this->width * this->height * 4);
				break;
			case IMG_TYPE_RGBA_16BIT:
			case IMG_TYPE_RGBA_16F:
				tempData = (u8*)new unsigned short int[this->width * this->height * 4];
				memset(tempData, 0x00, this->width * this->height * 4 * sizeof(unsigned short int));
				break;
			case IMG_TYPE_CIELAB:
				//LOGD("alloc cielab");
				tempData = (u8*)new int[this->width * this->height * 3];
				memset(tempData, 0x00, this->width * this->height * 3 * sizeof(int));
				break;
		}
	}
	else
	{
		tempData = NULL;
	}

	if (allocResult)
	{
		switch(this->type)
		{
			default:
				LOGError("unknown image type: %2.2x", this->type);
				break;
			case IMG_TYPE_GRAYSCALE:
				//LOGD("alloc grayscale");
				resultData = new u8[this->width * this->height];
				memset(resultData, 0x00, this->width * this->height);
				break;
			case IMG_TYPE_GRAYSCALE_16BIT:
				//LOGD("alloc grayscale");
				resultData = (u8*)new unsigned short int[this->width * this->height];
				memset(resultData, 0x00, this->width * this->height * sizeof(unsigned short int));
				break;
			case IMG_TYPE_GRAYSCALE_32BIT:
				//LOGD("alloc grayscale");
				resultData = (u8*)new unsigned long int[this->width * this->height];
				memset(resultData, 0x00, this->width * this->height * sizeof(unsigned long int));
				break;
			case IMG_TYPE_RGB:
				//LOGD("alloc RGB");
				resultData = new u8[this->width * this->height * 3];
				memset(resultData, 0x00, this->width * this->height * 3);
				break;
			case IMG_TYPE_RGBA:
				//LOGD("alloc RGB");
				resultData = new u8[this->width * this->height * 4];
				memset(resultData, 0x00, this->width * this->height * 4);
				break;
			case IMG_TYPE_RGBA_16BIT:
			case IMG_TYPE_RGBA_16F:
				resultData = (u8*)new unsigned short int[this->width * this->height * 4];
				memset(resultData, 0x00, this->width * this->height * 4 * sizeof(unsigned short int));
				break;
			case IMG_TYPE_CIELAB:
				//LOGD("alloc cielab");
				resultData = (u8*)new int[this->width * this->height * 3];
				memset(resultData, 0x00, this->width * this->height * 3 * sizeof(int));
				break;
		}
	}
	else
	{
		resultData = NULL;
	}

	if (this->mask)
	{
		delete [] this->mask;
		this->mask = NULL;
	}
}

void CImageData::AllocTempImage()
{
	if (this->tempData)
		this->DeallocTemp();

	switch(this->type)
	{
		default:
			LOGError("unknown image type: %2.2x", this->type);
			break;
		case IMG_TYPE_GRAYSCALE:
			//LOGD("alloc grayscale");
			tempData = new u8[this->width * this->height];
			memset(tempData, 0x00, this->width * this->height);
			break;
		case IMG_TYPE_GRAYSCALE_16BIT:
			//LOGD("alloc grayscale");
			tempData = (u8*)new unsigned short int[this->width * this->height];
			memset(tempData, 0x00, this->width * this->height * sizeof(unsigned short int));
			break;
		case IMG_TYPE_GRAYSCALE_32BIT:
			//LOGD("alloc grayscale");
			tempData = (u8*)new unsigned long int[this->width * this->height];
			memset(tempData, 0x00, this->width * this->height * sizeof(unsigned long int));
			break;
		case IMG_TYPE_RGB:
			//LOGD("alloc RGB");
			tempData = new u8[this->width * this->height * 3];
			memset(tempData, 0x00, this->width * this->height * 3);
			break;
		case IMG_TYPE_RGBA:
			//LOGD("alloc RGB");
			tempData = new u8[this->width * this->height * 4];
			memset(tempData, 0x00, this->width * this->height * 4);
			break;
		case IMG_TYPE_RGBA_16BIT:
		case IMG_TYPE_RGBA_16F:
			tempData = (u8*)new unsigned short int[this->width * this->height * 4];
			memset(tempData, 0x00, this->width * this->height * 4 * sizeof(unsigned short int));
			break;
		case IMG_TYPE_CIELAB:
			//LOGD("alloc cielab");
			tempData = (u8*)new int[this->width * this->height * 3];
			memset(tempData, 0x00, this->width * this->height * 3 * sizeof(int));
			break;
	}
	if (this->mask)
	{
		delete [] this->mask;
		this->mask = NULL;
	}
}

void CImageData::AllocResultImage()
{
	if (this->resultData)
		this->DeallocResult();

	switch(this->type)
	{
		default:
			LOGError("unknown image type: %2.2x", this->type);
			break;
		case IMG_TYPE_GRAYSCALE:
			//LOGD("alloc grayscale");
			resultData = new u8[this->width * this->height];
			memset(resultData, 0x00, this->width * this->height);
			break;
		case IMG_TYPE_GRAYSCALE_16BIT:
			//LOGD("alloc grayscale");
			resultData = (u8*)new unsigned short int[this->width * this->height];
			memset(resultData, 0x00, this->width * this->height * sizeof(unsigned short int));
			break;
		case IMG_TYPE_GRAYSCALE_32BIT:
			//LOGD("alloc grayscale");
			resultData = (u8*)new unsigned long int[this->width * this->height];
			memset(resultData, 0x00, this->width * this->height * sizeof(unsigned long int));
			break;
		case IMG_TYPE_RGB:
			//LOGD("alloc RGB");
			resultData = new u8[this->width * this->height * 3];
			memset(resultData, 0x00, this->width * this->height * 3);
			break;
		case IMG_TYPE_RGBA:
			//LOGD("alloc RGB");
			resultData = new u8[this->width * this->height * 4];
			memset(resultData, 0x00, this->width * this->height * 4);
			break;
		case IMG_TYPE_RGBA_16BIT:
		case IMG_TYPE_RGBA_16F:
			resultData = (u8*)new unsigned short int[this->width * this->height * 4];
			memset(resultData, 0x00, this->width * this->height * 4 * sizeof(unsigned short int));
			break;
		case IMG_TYPE_CIELAB:
			//LOGD("alloc cielab");
			resultData = (u8*)new int[this->width * this->height * 3];
			memset(resultData, 0x00, this->width * this->height * 3 * sizeof(int));
			break;
	}
	if (this->mask)
	{
		delete [] this->mask;
		this->mask = NULL;
	}
}

// IMAGE OBJECT POOL

// the idea here is to precalculate buffer positions first
// create global 2D matrixes of unsigned ints with the image's buffer positions for all x,y
// why?
// some of the algorithms are not easy to re-transform to use stream of pixels (to speed them up)
// instead of
//		for(y=0; y<height; y++)
//			for(x=0; x<width; x++)
//				getPixel(x, y)
// to have
//		for(y=0; y<height; y++)
//			offset = y*width;
//			for(x=0;x<width;x++)
//				getData(offset+x);
//
// but if we are using only 768x576 grayscale images for example, so why not to have just one 2D matrix (~3MB)
// that has the buffer positions for all x,y?
// f.e. this->bufferPositions[][] - pointer to the precalculated _global_static_ matrix of x,y buffer positions for
// 								type&width/height version of the image, precalculation could happen upon creation
//								of the image object (if it was not already precalculated).
// getPixel would be then:
// 		unsigned int bufferPos = bufferPositions[x][y];
// 		return imageData[bufferPos];
// for sure this should significally speed up non-streamed algorithms such as the watershed.

// grayscale
u8 CImageData::GetPixelResultByte(int x, int y)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelResultByte: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return 0x00;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("GetPixelResultByte: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultByte: result data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

void CImageData::SetPixelResultByte(int x, int y, u8 val)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelResultByte: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("SetPixelResultByte: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelResultByte: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	imageData[offset] = val;
#else
	imageData[y * width + x] = val;
#endif

}

u8 CImageData::GetPixelResultByteSafe(int x, int y)
{
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		return 0x00;
	}
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("GetPixelResultByte: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultByte: result data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

void CImageData::SetPixelResultByteSafe(int x, int y, u8 val)
{
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		return;
	}
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("SetPixelResultByte: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelResultByte: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	imageData[offset] = val;
#else
	imageData[y * width + x] = val;
#endif

}

u8 CImageData::GetPixelResultByteBorder(int x, int y)
{
	if (x < 0)
		x = 0;
	if (x >= this->width)
		x = this->width-1;
	if (y < 0)
		y = 0;
	if (y >= this->height-1)
		y = this->height-1;

#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("GetPixelResultByte: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultByte: result data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

u8 CImageData::GetPixelTemporaryByte(int x, int y)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelTemporaryByte: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return 0x00;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("GetPixelTemporaryByte: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelTemporaryByte: data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	u8 *imageData = (u8 *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

void CImageData::SetPixelTemporaryByte(int x, int y, u8 val)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelTemporaryByte: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("SetPixelTemporaryByte: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelTemporaryByte: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	imageData[offset] = val;
#else
	imageData[y * width + x] = val;
#endif

}

u8 *CImageData::getGrayscaleResultData()
{
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("getGrayscaleResultData: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
#endif
	return (u8 *)this->resultData;
}

void CImageData::setGrayscaleResultData(u8 *data)
{
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("setGrayscaleResultData: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
#endif
	this->resultData = data;
}

u8 *CImageData::getGrayscaleTemporaryData()
{
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("getGrayscaleTemporaryData: image type is not grayscale (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
#endif
	return (u8 *)this->tempData;
}

unsigned short CImageData::GetPixelResultGrayscale16Bit(int x, int y)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelResultGrayscale16Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return 0x00;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_16BIT)
	{
		LOGError("GetPixelResultGrayscale16Bit: image type is not unsigned short (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultGrayscale16Bit: result data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	short unsigned int *imageData = (short unsigned int *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

void CImageData::SetPixelResultGrayscale16Bit(int x, int y, short unsigned int val)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelResultGrayscale16Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_16BIT)
	{
		LOGError("SetPixelResultGrayscale16Bit: image type is not unsigned short (%2.2x)", this->type);
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelResultGrayscale16Bit: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	short unsigned int *imageData = (short unsigned int *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	imageData[offset] = val;
#else
	imageData[y * width + x] = val;
#endif

}

unsigned short CImageData::GetPixelTemporaryGrayscale16Bit(int x, int y)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelTemporaryGrayscale16Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return 0x00;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_16BIT)
	{
		LOGError("GetPixelTemporaryGrayscale16Bit: image type is not unsigned short (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelTemporaryGrayscale16Bit: data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	short unsigned int *imageData = (short unsigned int *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

void CImageData::SetPixelTemporaryGrayscale16Bit(int x, int y, short unsigned int val)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelTemporaryGrayscale16Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_16BIT)
	{
		LOGError("SetPixelTemporaryGrayscale16Bit: image type is not unsigned short (%2.2x)", this->type);
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelTemporaryGrayscale16Bit: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	short unsigned int *imageData = (short unsigned int *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	imageData[offset] = val;
#else
	imageData[y * width + x] = val;
#endif

}

short unsigned int *CImageData::getGrayscale16BitResultData()
{
	if (this->type != IMG_TYPE_GRAYSCALE_16BIT)
	{
		LOGError("getGrayscale16BitResultData: image type is not short int (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	return (short unsigned int *)this->resultData;
}

void CImageData::setGrayscale16BitResultData(short unsigned int *data)
{
	if (this->type != IMG_TYPE_GRAYSCALE_16BIT)
	{
		LOGError("setGrayscale16BitResultData: image type is not short int (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	this->resultData = (u8*)data;
}

//long
unsigned long int CImageData::GetPixelResultGrayscale32Bit(int x, int y)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelResultGrayscale32Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return 0x00;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_32BIT)
	{
		LOGError("GetPixelResultGrayscale32Bit: image type is not unsigned long (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultGrayscale32Bit: result data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	long unsigned int *imageData = (long unsigned int *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

void CImageData::SetPixelResultGrayscale32Bit(int x, int y, long unsigned int val)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelResultGrayscale32Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_32BIT)
	{
		LOGError("SetPixelResultGrayscale32Bit: image type is not unsigned long (%2.2x)", this->type);
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelResultGrayscale32Bit: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	long unsigned int *imageData = (long unsigned int *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	imageData[offset] = val;
#else
	imageData[y * width + x] = val;
#endif

}

unsigned long int CImageData::GetPixelTemporaryGrayscale32Bit(int x, int y)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelTemporaryGrayscale32Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return 0x00;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_32BIT)
	{
		LOGError("GetPixelTemporaryGrayscale32Bit: image type is not unsigned long (%2.2x)", this->type);
		//log_backtrace();
		return 0x00;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelTemporaryGrayscale32Bit: data is null\n");
		//log_backtrace();
		return 0x00;
	}
#endif
	long unsigned int *imageData = (long unsigned int *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	return imageData[offset];
#else
	return imageData[y * width + x];
#endif

}

void CImageData::SetPixelTemporaryGrayscale32Bit(int x, int y, long unsigned int val)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelTemporaryGrayscale32Bit: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_GRAYSCALE_32BIT)
	{
		LOGError("SetPixelTemporaryGrayscale32Bit: image type is not unsigned long (%2.2x)", this->type);
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelTemporaryGrayscale32Bit: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	long unsigned int *imageData = (long unsigned int *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
	imageData[offset] = val;
#else
	imageData[y * width + x] = val;
#endif

}

long unsigned int *CImageData::getGrayscale32BitResultData()
{
	if (this->type != IMG_TYPE_GRAYSCALE_32BIT)
	{
		LOGError("getGrayscale32BitResultData: image type is not long int (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	return (long unsigned int *)this->resultData;
}

void CImageData::setGrayscale32BitResultData(long unsigned int *data)
{
	if (this->type != IMG_TYPE_GRAYSCALE_32BIT)
	{
		LOGError("setGrayscale32BitResultData: image type is not long int (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	this->resultData = (u8*)data;
}

// rgb

// this might be confusing with rgba
//void CImageData::GetPixel(int x, int y, u8 *r, u8 *g, u8 *b)
//{
//	GetPixelResultRGB(x, y, r, g, b);
//}

void CImageData::GetPixelResultRGB(int x, int y, u8 *r, u8 *g, u8 *b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelResultRGB: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGB)
	{
		LOGError("GetPixelResultRGB: image type is not rgb");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultRGB: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif
	*r = imageData[offset++];
	*g = imageData[offset++];
	*b = imageData[offset];
}

void CImageData::SetPixel(int x, int y, u8 r, u8 g, u8 b, u8 a)
{
	SetPixelResultRGBA(x, y, r, g, b, a);
}

void CImageData::SetPixelResultRGB(int x, int y, u8 r, u8 g, u8 b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixel: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGB)
	{
		LOGError("SetPixelResultRGB: image type is not rgb");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelResultRGB: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif

	imageData[offset++] = r;
	imageData[offset++] = g;
	imageData[offset] = b;
}

void CImageData::GetPixelTemporaryRGB(int x, int y, u8 *r, u8 *g, u8 *b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelTemporaryRGB: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGB)
	{
		LOGError("GetPixelTemporaryRGB: image type is not rgb");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelTemporaryRGB: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif

	*r = imageData[offset++];
	*g = imageData[offset++];
	*b = imageData[offset];

}

void CImageData::SetPixelTemporaryRGB(int x, int y, u8 r, u8 g, u8 b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelTemporaryRGB: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGB)
	{
		LOGError("SetPixelTemporaryRGB: image type is not rgb");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelTemporaryRGB: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif

	imageData[offset++] = r;
	imageData[offset++] = g;
	imageData[offset] = b;
}

u8 *CImageData::getRGBResultData()
{
	if (this->type != IMG_TYPE_RGB)
	{
		LOGError("getRGBResultData: image type is not rgb (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	return (u8 *)this->resultData;
}

void CImageData::setRGBResultData(u8 *data)
{
	if (this->type != IMG_TYPE_RGB)
	{
		LOGError("setRGBResultData: image type is not rgb (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	this->resultData = data;
}

/////////////////RGBA
// rgb
void CImageData::GetPixel(int x, int y, u8 *r, u8 *g, u8 *b, u8 *a)
{
	GetPixelResultRGBA(x, y, r, g, b, a);
}

void CImageData::GetPixelFloat(int x, int y, float *r, float *g, float *b, float *a)
{
	u8 ur, ug, ub, ua;
	GetPixelResultRGBA(x, y, &ur, &ug, &ub, &ua);
	
	*r = (float)ur / 255.0f;
	*g = (float)ug / 255.0f;
	*b = (float)ub / 255.0f;
	*a = (float)ua / 255.0f;
}

void CImageData::GetPixelResultRGBA(int x, int y, u8 *r, u8 *g, u8 *b, u8 *a)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelResultRGBA: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGBA)
	{
		LOGError("GetPixelResultRGBA: image type is not rgba");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultRGBA: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	// 16-bit colour answers this 8-bit question by scaling, not truncating:
	// (v * 255 + 32767) / 65535 rounds to nearest, so 65535 -> 255 and
	// 32768 -> 128. A plain >> 8 would map 65535 to 255 but 257 to 1 and
	// biases every value downward.
	// Float answers it by clamping to 0..1 and scaling: an above-white value
	// has nowhere to go in 8 bits, and this accessor is not the tone-map
	// (that is ConvertRGBA16FToRGBA8, which is given a headroom).
	if (this->type == IMG_TYPE_RGBA_16F)
	{
		const u16 *px = (const u16 *)this->resultData;
		const size_t off = ((size_t)y * (size_t)width + (size_t)x) * 4;
		u8 *out[4] = { r, g, b, a };
		for (int c = 0; c < 4; c++)
		{
			float v = HalfToFloat(px[off + c]);
			if (v < 0.0f) v = 0.0f;
			if (v > 1.0f) v = 1.0f;
			*out[c] = (u8)(v * 255.0f + 0.5f);
		}
		return;
	}
	if (this->type == IMG_TYPE_RGBA_16BIT)
	{
		const unsigned short *px = (const unsigned short *)this->resultData;
		const size_t off = ((size_t)y * (size_t)width + (size_t)x) * 4;
		*r = (u8)(((unsigned)px[off + 0] * 255u + 32767u) / 65535u);
		*g = (u8)(((unsigned)px[off + 1] * 255u + 32767u) / 65535u);
		*b = (u8)(((unsigned)px[off + 2] * 255u + 32767u) / 65535u);
		*a = (u8)(((unsigned)px[off + 3] * 255u + 32767u) / 65535u);
		return;
	}

	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	//unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 4 + x * 4;
#endif
	*r = imageData[offset++];
	*g = imageData[offset++];
	*b = imageData[offset++];
	*a = imageData[offset];
}

// The 16-bit reader, for callers that want the precision the file carried.
// Returns false for any other type rather than inventing values.
bool CImageData::GetPixelResultRGBA16Bit(int x, int y,
                                      unsigned short *r, unsigned short *g,
                                      unsigned short *b, unsigned short *a)
{
	if (this->type != IMG_TYPE_RGBA_16BIT || this->resultData == NULL)
		return false;
	if (x < 0 || y < 0 || x >= width || y >= height)
		return false;
	const unsigned short *px = (const unsigned short *)this->resultData;
	const size_t off = ((size_t)y * (size_t)width + (size_t)x) * 4;
	*r = px[off + 0]; *g = px[off + 1]; *b = px[off + 2]; *a = px[off + 3];
	return true;
}

// Collapse a 16-bit image to RGBA8 IN PLACE (new buffer, old one freed).
// The GPU path is 8-bit, so something has to do this; doing it here means one
// implementation with one rounding rule instead of a copy at every consumer.
bool CImageData::ConvertRGBA16BitToRGBA8()
{
	if (this->type != IMG_TYPE_RGBA_16BIT || this->resultData == NULL)
		return false;
	const size_t count = (size_t)width * (size_t)height * 4;
	const unsigned short *src = (const unsigned short *)this->resultData;
	u8 *dst = new u8[count];
	for (size_t i = 0; i < count; i++)
		dst[i] = (u8)(((unsigned)src[i] * 255u + 32767u) / 65535u);
	delete [] (unsigned short int *)this->resultData;
	this->resultData = dst;
	this->type = IMG_TYPE_RGBA;
	return true;
}

// The 16-bit writer. There was a getter and no setter, so nothing outside a
// decoder could build a 16-bit image -- including a test.
void CImageData::SetPixelResultRGBA16Bit(int x, int y,
                                      unsigned short r, unsigned short g,
                                      unsigned short b, unsigned short a)
{
	if (this->type != IMG_TYPE_RGBA_16BIT || this->resultData == NULL)
		return;
	if (x < 0 || y < 0 || x >= width || y >= height)
		return;
	unsigned short *px = (unsigned short *)this->resultData;
	const size_t off = ((size_t)y * (size_t)width + (size_t)x) * 4;
	px[off + 0] = r; px[off + 1] = g; px[off + 2] = b; px[off + 3] = a;
}

// ---------------------------------------------------------------------------
// IMG_TYPE_RGBA_16F (S-5)
// ---------------------------------------------------------------------------

// CONVENIENCE ACCESSOR, NOT THE HOT PATH -- see the header. Each call converts
// in software; the bulk producers write half directly with FloatToHalf.
bool CImageData::GetPixelResultFloat(int x, int y, float *r, float *g, float *b, float *a)
{
	if (this->type != IMG_TYPE_RGBA_16F || this->resultData == NULL)
		return false;
	if (x < 0 || y < 0 || x >= width || y >= height)
		return false;
	const u16 *px = (const u16 *)this->resultData;
	const size_t off = ((size_t)y * (size_t)width + (size_t)x) * 4;
	*r = HalfToFloat(px[off + 0]); *g = HalfToFloat(px[off + 1]);
	*b = HalfToFloat(px[off + 2]); *a = HalfToFloat(px[off + 3]);
	return true;
}

void CImageData::SetPixelResultFloat(int x, int y, float r, float g, float b, float a)
{
	if (this->type != IMG_TYPE_RGBA_16F || this->resultData == NULL)
		return;
	if (x < 0 || y < 0 || x >= width || y >= height)
		return;
	u16 *px = (u16 *)this->resultData;
	const size_t off = ((size_t)y * (size_t)width + (size_t)x) * 4;
	px[off + 0] = FloatToHalf(r); px[off + 1] = FloatToHalf(g);
	px[off + 2] = FloatToHalf(b); px[off + 3] = FloatToHalf(a);
}

// unorm16 -> half, IN PLACE (same buffer size: both are 4 x u16 per pixel, so
// this reinterprets rather than reallocates).
//
// Promoting integer 16-bit to float gains no range -- the source had none
// above 1.0 to begin with -- so nothing in the resident funnel does this. It
// exists for producers that compute in float, hand off through a 16-bit
// carrier, and need the float type at the end.
bool CImageData::ConvertRGBA16BitToRGBA16F()
{
	MT_ASSERT_NOT_RENDER_THREAD("ConvertRGBA16BitToRGBA16F");

	if (this->type != IMG_TYPE_RGBA_16BIT || this->resultData == NULL)
		return false;
	const size_t count = (size_t)width * (size_t)height * 4;
	u16 *px = (u16 *)this->resultData;
	for (size_t i = 0; i < count; i++)
		px[i] = FloatToHalf((float)px[i] * (1.0f / 65535.0f));
	this->type = IMG_TYPE_RGBA_16F;
	return true;
}

// Standard 8x8 Bayer threshold matrix, values 0..63. LIFTED from the app's
// PC_ColorTransform.cpp (DitherTo8), which stays the master copy: the engine
// cannot include an app header, and both halves of the pipeline must quantise
// the same way or an image that took the float path would band differently
// from one that did not. Ordered rather than random on purpose -- it is
// spatially DETERMINISTIC, so a test that renders the same pixels twice gets
// the same bytes twice.
static const u8 kImgBayer8[8][8] = {
	{  0, 32,  8, 40,  2, 34, 10, 42 },
	{ 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44,  4, 36, 14, 46,  6, 38 },
	{ 60, 28, 52, 20, 62, 30, 54, 22 },
	{  3, 35, 11, 43,  1, 33,  9, 41 },
	{ 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47,  7, 39, 13, 45,  5, 37 },
	{ 63, 31, 55, 23, 61, 29, 53, 21 },
};

// half -> RGBA8 with a tone-map, IN PLACE (new buffer, old one freed).
//
// THE CURVE, and why it is this one: extended Reinhard, normalised so that the
// value equal to `headroom` maps exactly to 1.0 --
//
//     out = v * (1 + v/hÂ²) / (1 + v)
//
// At headroom 1.0 this is EXACTLY the identity -- v*(1+v)/(1+v) == v -- so the
// 0..1 body comes out precisely where it does today, which is the property the
// regression test pins, and anything above 1.0 clips as it does today.
//
// At headroom > 1.0 the curve maps 0..headroom onto 0..1, so above-white values
// stay SEPARABLE from one another rather than all flattening to 255. That
// separation is the point: an 8-bit output has no headroom whatever the display
// does, so the parameter buys highlight DETAIL, not brightness. The body
// darkens slightly in exchange.
//
// Reinhard rather than something filmic (ACES, Hable) deliberately: those bake
// in a contrast/saturation look, and this is a CULLING tool -- a photographer
// judging a frame needs to see the frame, not a grade. The roll-off is the
// minimum intervention that maps an unbounded range into a bounded one.
bool CImageData::ConvertRGBA16FToRGBA8(float headroom, bool inputIsLinear)
{
	MT_ASSERT_NOT_RENDER_THREAD("ConvertRGBA16FToRGBA8");

	if (this->type != IMG_TYPE_RGBA_16F || this->resultData == NULL)
		return false;
	if (!(headroom >= 1.0f))     // also catches NaN
		headroom = 1.0f;

	const size_t pixels = (size_t)width * (size_t)height;
	const u16 *src = (const u16 *)this->resultData;
	u8 *dst = new u8[pixels * 4];
	const float invH2 = 1.0f / (headroom * headroom);

	for (size_t i = 0; i < pixels; i++)
	{
		const int x = (int)(i % (size_t)width);
		const int y = (int)(i / (size_t)width);
		for (int c = 0; c < 3; c++)
		{
			float v = HalfToFloat(src[i * 4 + c]);
			// The tone-map has to happen in LINEAR, so undo the surface
			// encoding first when the caller says the pixels carry it.
			if (!inputIsLinear)
				v = SrgbExtendedDecode(v);
			if (v < 0.0f) v = 0.0f;
			v = v * (1.0f + v * invH2) / (1.0f + v);
			if (v > 1.0f) v = 1.0f;
			// Back to 8-bit sRGB, with the app's ordered dither so the float
			// lane bands no worse than the 16-bit one.
			const float e = SrgbExtendedEncode(v);
			const u32 q = (u32)(e * 65535.0f + 0.5f);
			const u32 d = (u32)kImgBayer8[y & 7][x & 7] * 4 + 2;   // 2..254
			const u32 o = (q + d) / 257u;
			dst[i * 4 + c] = (u8)(o > 255u ? 255u : o);
		}
		float av = HalfToFloat(src[i * 4 + 3]);
		if (av < 0.0f) av = 0.0f;
		if (av > 1.0f) av = 1.0f;
		dst[i * 4 + 3] = (u8)(av * 255.0f + 0.5f);
	}

	delete [] (u16 *)this->resultData;
	this->resultData = dst;
	this->type = IMG_TYPE_RGBA;
	return true;
}

void CImageData::SetPixelResultRGBA(int x, int y, u8 r, u8 g, u8 b, u8 a)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelResultRGBA: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGBA)
	{
		LOGError("SetPixelResultRGBA: image type is not rgba");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelResultRGBA: result data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 4 + x * 4;
#endif
	
//      x=0  x=1  x=2  x=3
// y=0	RGBA RGBA RGBA RGBA
// y=1  RGBA RGBA RGBA RGBA

	
//      RGBA RGBA RGBA RGBA | RGBA RGBA RGBA RGBA | RGBA RGBA ...

	imageData[offset++] = r;
	imageData[offset++] = g;
	imageData[offset++] = b;
	imageData[offset] = a;
}

void CImageData::GetPixelTemporaryRGBA(int x, int y, u8 *r, u8 *g, u8 *b, u8 *a)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelTemporaryRGBA: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGBA)
	{
		LOGError("GetPixelTemporaryRGBA: image type is not rgba");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelTemporaryRGBA: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 4 + x * 4;
#endif

	*r = imageData[offset++];
	*g = imageData[offset++];
	*b = imageData[offset++];
	*a = imageData[offset];

}

void CImageData::SetPixelTemporaryRGBA(int x, int y, u8 r, u8 g, u8 b, u8 a)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelTemporaryRGBA: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_RGBA)
	{
		LOGError("SetPixelTemporaryRGBA: image type is not rgba");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelTemporaryRGBA: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	u8 *imageData = (u8 *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 4 + x * 4;
#endif

	imageData[offset++] = r;
	imageData[offset++] = g;
	imageData[offset++] = b;
	imageData[offset] = a;
}

void CImageData::EraseContent(u8 r, u8 g, u8 b, u8 a)
{
	for (int x = 0; x < width; x++)
	{
		for (int y = 0; y < height; y++)
		{
			SetPixelResultRGBA(x, y, r,g,b,a);
		}
	}
}


u8 *CImageData::getResultDataForUpload()
{
	if (this->type != IMG_TYPE_RGBA && this->type != IMG_TYPE_RGBA_16F)
	{
		LOGError("getResultDataForUpload: type %2.2x is not uploadable", this->type);
		return NULL;
	}
	return (u8 *)this->resultData;
}

u8 *CImageData::getRGBAResultData()
{
	if (this->type != IMG_TYPE_RGBA)
	{
		LOGError("getRGBResultData: image type is not rgba (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	return (u8 *)this->resultData;
}

void CImageData::setRGBAResultData(u8 *data)
{
	// RGBA8 and RGBA16F both. This guard exists to catch data that does not
	// match the image's own type, and half-float pixels in an IMG_TYPE_RGBA_16F
	// image are not that -- the funnel already agreed to the type.
	if (this->type != IMG_TYPE_RGBA && this->type != IMG_TYPE_RGBA_16F)
	{
		LOGError("setRGBResultData: image type is not rgba/rgba16f (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	this->resultData = data;
}

// cielab
void CImageData::GetPixelResultCIELAB(int x, int y, int *l, int *a, int *b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixel: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_CIELAB)
	{
		LOGError("GetPixelResultCIELAB: image type is not cielab");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelResultCIELAB: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	int *imageData = (int *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif

	*l = imageData[offset++];
	*a = imageData[offset++];
	*b = imageData[offset];
}

void CImageData::SetPixelResultCIELAB(int x, int y, int l, int a, int b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelResultCIELAB: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_CIELAB)
	{
		LOGError("SetPixelResultCIELAB: image type is not cielab");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelResultCIELAB: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	int *imageData = (int *)this->resultData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif

	imageData[offset++] = l;
	imageData[offset++] = a;
	imageData[offset] = b;
}

void CImageData::GetPixelTemporaryCIELAB(int x, int y, int *l, int *a, int *b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::GetPixelTemporaryCIELAB: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_CIELAB)
	{
		LOGError("GetPixelTemporaryCIELAB: image type is not cielab");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("GetPixelTemporaryCIELAB: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	int *imageData = (int *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif

	*l = imageData[offset++];
	*a = imageData[offset++];
	*b = imageData[offset];
}

void CImageData::SetPixelTemporaryCIELAB(int x, int y, int l, int a, int b)
{
#ifdef PEDANTIC
	if (x < 0 || y < 0 || x >= width || y >= height)
	{
		LOGError("CImageData::SetPixelTemporaryCIELAB: outside image (x=%d y=%d w=%d h=%d)", x, y, width, height);
		//log_backtrace();
		return;
	}
#endif
#ifdef MORE_PEDANTIC
	if (this->type != IMG_TYPE_CIELAB)
	{
		LOGError("SetPixelTemporaryCIELAB: image type is not cielab");
		//log_backtrace();
		return;
	}
	if (this->resultData == NULL)
	{
		LOGError("SetPixelTemporaryCIELAB: data is null\n");
		//log_backtrace();
		return;
	}
#endif
	int *imageData = (int *)this->tempData;

#ifdef USE_BUFFER_OFFSETS
	unsigned int offset = this->bufferOffsets->offsets[x][y];
#else
	unsigned int offset = y * width * 3 + x * 3;
#endif

	imageData[offset++] = l;
	imageData[offset++] = a;
	imageData[offset] = b;
}

int *CImageData::getCIELABResultData()
{
	if (this->type != IMG_TYPE_CIELAB)
	{
		LOGError("getCIELABResultData: image type is not CIELAB (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	return (int *)this->resultData;
}

void CImageData::setCIELABResultData(int *data)
{
	if (this->type != IMG_TYPE_CIELAB)
	{
		LOGError("setCIELABResultData: image type is not CIELAB (%2.2x)", this->type);
		//log_backtrace();
		SYS_FatalExit();
	}
	this->resultData = (u8*)data;
}

void CImageData::ConvertToGrayscale()
{
	this->ConvertToByte();
}

void CImageData::ConvertToGrayscale(u8 componentNum)
{
	this->ConvertToByte(componentNum);
}

void CImageData::ConvertToByte()
{
	//LOGD("ConvertToByte()");
	if (this->type == IMG_TYPE_GRAYSCALE_16BIT)
	{
		u8 *newData = new u8[this->width * this->height];
		unsigned short int *imageData = (unsigned short int *)this->resultData;
		unsigned int size = this->width * this->height;
		unsigned short int val;
		for (unsigned int i = 0; i < size; i++)
		{
			val = imageData[i];
			newData[i] = (u8)(val & 0x00FF);
			//if (i % 10000 == 0)
				//LOGD("%d/%d set", i, size);
		}
		//LOGD("dealloc");
		DeallocImage();
		//LOGD("dealloc ok");
		this->type = IMG_TYPE_GRAYSCALE;
		this->resultData = newData;
	}
	else if (this->type == IMG_TYPE_RGBA)
	{
		u8 *newData = new u8[this->width * this->height];
		for (unsigned int x = 0; x < this->width; x++)
		{
			for (unsigned int y = 0; y < this->height; y++)
			{
				u8 r,g,b,a;
				this->GetPixelResultRGBA(x, y, &r, &g, &b, &a);
				
				int v = (r+g+b)/3;
				
				newData[y * width + x] = v;
			}
		}
		//LOGD("dealloc");
		DeallocImage();
		//LOGD("dealloc ok");
		this->type = IMG_TYPE_GRAYSCALE;
		this->resultData = newData;
	}
	else
	{
		SYS_FatalExit("CImageData::ConvertToByte: image type %2.2x not implemented", this->type);
	}
	//LOGD("ConvertToByte() done");
}

void CImageData::ConvertToByte(u8 componentNum)
{
	//LOGD("ConvertToByte()");
	if (this->type == IMG_TYPE_RGBA)
	{
		u8 *newData = new u8[this->width * this->height];
		for (unsigned int x = 0; x < this->width; x++)
		{
			for (unsigned int y = 0; y < this->height; y++)
			{
				u8 r,g,b,a;
				this->GetPixelResultRGBA(x, y, &r, &g, &b, &a);
				
				int v = 0;
				switch(componentNum)
				{
					case 0: v = r; break;
					case 1: v = g; break;
					case 2: v = b; break;
					case 3: v = a; break;
				}
				
				newData[y * width + x] = v;
			}
		}
		//LOGD("dealloc");
		DeallocImage();
		//LOGD("dealloc ok");
		this->type = IMG_TYPE_GRAYSCALE;
		this->resultData = newData;
	}
	else
	{
		SYS_FatalExit("CImageData::ConvertToByte: image type %2.2x not implemented", this->type);
	}
	//LOGD("ConvertToByte() done");
}

void CImageData::ConvertToRGBA()
{
	//LOGD("CImageData::ConvertToRGBA");
	if (this->type == IMG_TYPE_RGBA)
	{
		return;
	}
	
	if (this->type == IMG_TYPE_GRAYSCALE)
	{
		u8 *newData = new u8[this->width * this->height * 4];
		for (unsigned int x = 0; x < this->width; x++)
		{
			for (unsigned int y = 0; y < this->height; y++)
			{
				u8 v = this->GetPixelResultByte(x, y);
				unsigned int offset = y * width * 4 + x * 4;
				newData[offset++] = v;
				newData[offset++] = v;
				newData[offset++] = v;
				newData[offset] = 255;
			}
		}
		//LOGD("dealloc");
		DeallocImage();
		//LOGD("dealloc ok");
		this->type = IMG_TYPE_RGBA;
		this->resultData = newData;
		
	}
	else
	{
		SYS_FatalExit("CImageData::ConvertToRGBA: image type %2.2x not implemented", this->type);
	}
	//LOGD("CImageData::ConvertToRGBA: done");
}

void CImageData::ConvertToRGB()
{
	//LOGD("CImageData::ConvertToRGB");
	if (this->type == IMG_TYPE_GRAYSCALE)
	{
		u8 *newData = new u8[this->width * this->height * 3];
		for (unsigned int x = 0; x < this->width; x++)
		{
			for (unsigned int y = 0; y < this->height; y++)
			{
				u8 v = this->GetPixelResultByte(x, y);
				unsigned int offset = y * width * 3 + x * 3;
				newData[offset++] = v;
				newData[offset++] = v;
				newData[offset++] = v;
			}
		}
		//LOGD("dealloc");
		DeallocImage();
		//LOGD("dealloc ok");
		this->type = IMG_TYPE_RGB;
		this->resultData = newData;
		
	}
	else if (this->type == IMG_TYPE_RGBA)
	{
		u8 *newData = new u8[this->width * this->height * 3];
		for (unsigned int x = 0; x < this->width; x++)
		{
			for (unsigned int y = 0; y < this->height; y++)
			{
				u8 r,g,b,a;
				this->GetPixelResultRGBA(x, y, &r, &g, &b, &a);
				unsigned int offset = y * width * 3 + x * 3;
				newData[offset++] = r;
				newData[offset++] = g;
				newData[offset++] = b;
			}
		}
		//LOGD("dealloc");
		DeallocImage();
		//LOGD("dealloc ok");
		this->type = IMG_TYPE_RGB;
		this->resultData = newData;
		
	}
	else
	{
		SYS_FatalExit("CImageData::ConvertToRGB: image type %2.2x not implemented", this->type);
	}
	//LOGD("CImageData::ConvertToRGBA: done");
}

void CImageData::ConvertToGrayscale16Bit()
{
	LOGD("ConvertToGrayscale16Bit()");
	if (this->type != IMG_TYPE_GRAYSCALE_32BIT)
	{
		LOGError("image type is not long int, not implemented");
		SYS_FatalExit();
	}
	unsigned short int *newData = new unsigned short int[this->width * this->height];
	unsigned long int *imageData = (unsigned long int *)this->resultData;
	unsigned int size = this->width * this->height;
	unsigned long int val;
	for (unsigned int i = 0; i < size; i++)
	{
		val = imageData[i];
		newData[i] = (unsigned short int)(val & 0x00FF);
		//if (i % 10000 == 0)
			//LOGD("%d/%d set", i, size);
	}
	//LOGD("dealloc");
	DeallocImage();
	//LOGD("dealloc ok");
	this->type = IMG_TYPE_GRAYSCALE_16BIT;
	this->resultData = (u8*)newData;

	LOGD("~ConvertToGrayscale16Bit()");
}

void CImageData::ConvertToGrayscale16BitCount()
{
	LOGD("ConvertToGrayscale16BitCount()");
	if (this->type != IMG_TYPE_RGB)
	{
		LOGError("image type is not rgb, not implemented");
		SYS_FatalExit();
	}

	short unsigned int *newData = new short unsigned int [this->width * this->height];
	u8 *imageData = (u8 *)this->resultData;
	short unsigned int classNum = 0;
	map<int, short unsigned int> colors;

	for (int x = 0; x < width; x++)
	{
		for (int y = 0; y < height; y++)
		{
			u8 r = imageData[y * width * 3 + x * 3    ];
			u8 g = imageData[y * width * 3 + x * 3 + 1];
			u8 b = imageData[y * width * 3 + x * 3 + 2];

			int colorVal = 0x00 | (r << 16) | (g << 8) | b;
			map<int, short unsigned int>::iterator val = colors.find(colorVal);
			short unsigned int curColor = 0;
			if (val == colors.end())
			{
				curColor = classNum++;
				//curColor *= 0x20;
				LOGD("found new color: %2.2x %2.2x %2.2x = %d, x=%d y=%d", r, g, b, curColor, x, y);
				colors[colorVal] = curColor;
				if (classNum == 0xFFFF)
				{
					LOGError("CImageData::ConvertToGrayscale16BitCount: more than 0xFFFF classes");
					SYS_FatalExit();
				}
			}
			else
			{
				curColor = (*val).second;
			}

			newData[y * width + x] = curColor;
			//LOGD("x=%d y=%d col=%d", x, y, curColor);
		}
	}
	LOGD("DeallocImage()");
	DeallocImage();
	LOGD("DeallocImage() finished");
	this->type = IMG_TYPE_GRAYSCALE_16BIT;
	this->resultData = (u8*)newData;
	LOGD("~ConvertToGrayscale16BitCount()");

}

// Are 16-bit samples stored the way this host reads an unsigned short? PNG is
// big-endian by spec; almost every machine we ship on is little-endian, but
// asking is cheaper than assuming and costs nothing at runtime.
static bool PC_HostIsLittleEndian()
{
	const unsigned short probe = 0x0102;
	return *(const unsigned char *)&probe == 0x02;
}

// 16-bit PNG writer, for the two 16-bit image types. Separate from Save()'s
// 8-bit machinery on purpose: that code allocates one byte per sample and its
// fill chain has branches for GRAYSCALE, RGB and RGBA only -- so a
// GRAYSCALE_16BIT image was accepted by the guard, given width-byte rows, and
// then written from rows nothing ever filled. It saved uninitialised heap.
//
// PNG stores 16-bit samples big-endian; png_set_swap converts from native
// order on a little-endian host, mirroring the read path exactly.
bool CImageData::SavePNG16(const char *fileName)
{
	const bool isGray = (this->type == IMG_TYPE_GRAYSCALE_16BIT);
	if (!isGray && this->type != IMG_TYPE_RGBA_16BIT)
		return false;
	if (this->resultData == NULL || width <= 0 || height <= 0)
		return false;

	FILE *fp = SYS_FopenUtf8(fileName, "wb");
	if (fp == NULL)
	{
		LOGError("CImageData::SavePNG16: cannot open '%s' for writing", fileName);
		return false;
	}

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png == NULL) { fclose(fp); return false; }
	png_infop info = png_create_info_struct(png);
	if (info == NULL) { png_destroy_write_struct(&png, NULL); fclose(fp); return false; }

	png_bytep *rows = NULL;
	if (setjmp(png_jmpbuf(png)))
	{
		free(rows);
		png_destroy_write_struct(&png, &info);
		fclose(fp);
		return false;
	}

	png_init_io(png, fp);
	png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height, 16,
	             isGray ? PNG_COLOR_TYPE_GRAY : PNG_COLOR_TYPE_RGB_ALPHA,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	png_write_info(png, info);
#ifdef PNG_WRITE_SWAP_SUPPORTED
	if (PC_HostIsLittleEndian())
		png_set_swap(png);
#endif

	const size_t samplesPerPixel = isGray ? 1 : 4;
	const size_t rowSamples = (size_t)width * samplesPerPixel;
	rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)height);
	if (rows == NULL)
	{
		png_destroy_write_struct(&png, &info);
		fclose(fp);
		return false;
	}
	// Rows point INTO resultData -- no copy, and no per-row malloc to leak.
	for (int y = 0; y < height; y++)
		rows[y] = (png_bytep)((unsigned short *)this->resultData + (size_t)y * rowSamples);

	png_write_image(png, rows);
	png_write_end(png, NULL);

	free(rows);
	png_destroy_write_struct(&png, &info);
	fclose(fp);
	return true;
}

void CImageData::Save(const char *fileName)
{
	// Float has no writer here: every path below treats resultData as bytes or
	// as unorm16, and half bits are neither. Refuse loudly rather than emit a
	// file whose pixels are the bit patterns of the wrong format -- no
	// production caller does this today, and the day one appears it should
	// fail at the call rather than in an image viewer.
	if (this->type == IMG_TYPE_RGBA_16F)
	{
		LOGError("CImageData::Save: IMG_TYPE_RGBA_16F has no writer -- "
				 "tone-map to RGBA8 or convert first ('%s')", fileName);
		return;
	}

	// The 16-bit types get the 16-bit writer; everything below this is the
	// 8-bit path and stays exactly as it was.
	if (this->type == IMG_TYPE_GRAYSCALE_16BIT || this->type == IMG_TYPE_RGBA_16BIT)
	{
		if (!SavePNG16(fileName))
			LOGError("CImageData::Save: 16-bit write failed for '%s'", fileName);
		return;
	}

	if (this->type != IMG_TYPE_GRAYSCALE
		&& this->type != IMG_TYPE_RGB
		&& this->type != IMG_TYPE_RGBA)
	{
		LOGError("saving image type %2.2x not implemented (%s)", this->type, fileName);
		return;
	}
	png_byte color_type = PNG_COLOR_TYPE_GRAY;
	png_byte bit_depth = 8;

	png_structp png_ptr;
	png_infop info_ptr;
	png_bytep* row_pointers = NULL;

	int x, y;

	// create file
	FILE *fp = SYS_FopenUtf8(fileName, "wb");
	if (!fp)
	{
		LOGError("CImageData::Save: File %s could not be opened for writing", fileName);
		return;
	}

	if (this->type == IMG_TYPE_GRAYSCALE || this->type == IMG_TYPE_RGB || this->type == IMG_TYPE_GRAYSCALE_16BIT)
	{
		row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * this->height);
		for (y=0; y < height; y++)
			row_pointers[y] = (png_byte*) malloc(this->width);
	}
	else if (this->type == IMG_TYPE_RGBA)
	{
		color_type = PNG_COLOR_TYPE_RGB_ALPHA;
		row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * this->height);
		for (y=0; y < height; y++)
			row_pointers[y] = (png_byte*) malloc(this->width*4);
	}


	if (this->type == IMG_TYPE_GRAYSCALE)
	{
		u8 *byteData = (u8 *)this->resultData;

		for (y = 0; y < height; y++)
		{
			png_byte* row = row_pointers[y];
			for (x = 0; x < width; x++)
			{
				row[x] = byteData[y * this->width + x];
			}
		}
	}
	else if (this->type == IMG_TYPE_RGB)
	{
		u8 *byteData = (u8 *)this->resultData;
		// scale first
		u8 min, max, val;

		max = 0x00;
		min = 0xFF;

		for (int x = 0; x < this->width; x++)
		{
			for (int y = 0; y < this->height; y++)
			{
				val = (byteData[y * (this->width * 3)+ (x * 3)]
					+ byteData[y * (this->width * 3)+ (x * 3) + 1]
					+ byteData[y * (this->width * 3)+ (x * 3) + 2]) / 3;

				if (val < min)
					min = val;
				if (val > max)
					max = val;
			}
		}

		//LOGD("Save: max=%d min=%d", max, min);
		if (max != min)
		{
			for (int y = 0; y < this->height; y++)
			{
				png_byte* row = row_pointers[y];
				for (int x = 0; x < this->width; x++)
				{
					u8 val = (byteData[y * (this->width * 3)+ (x * 3)]
							+ byteData[y * (this->width * 3)+ (x * 3) + 1]
							+ byteData[y * (this->width * 3)+ (x * 3) + 2]) / 3;
					u8 valCalc = ((val - min) * 255) / (max - min);
					//LOGD("x=%d y=%d val=%d valCalc=%d", x, y, val, valCalc);
					row[x] = valCalc;
				}
			}

		}
	}
	else if (this->type == IMG_TYPE_RGBA)
	{
		u8 *byteData = (u8 *)this->resultData;
		for (int y = 0; y < this->height; y++)
		{
			png_byte* row = row_pointers[y];
			for (int x = 0; x < this->width; x++)
			{
				u8 r = (byteData[y * (this->width * 4)+ (x * 4)]);
				u8 g = (byteData[y * (this->width * 4)+ (x * 4) + 1]);
				u8 b = (byteData[y * (this->width * 4)+ (x * 4) + 2]);
				u8 a = (byteData[y * (this->width * 4)+ (x * 4) + 3]);
				row[(x*4)] = r;
				row[(x*4) + 1] = g;
				row[(x*4) + 2] = b;
				row[(x*4) + 3] = a;
			}
		}
	}
	else if (this->type == IMG_TYPE_GRAYSCALE_16BIT)
	{
		unsigned short int *shortData = (unsigned short int *)this->resultData;

		// scale first
		unsigned short int min, max, val;

		max = 0x0000;
		min = 0xFFFF;

		for (int x = 0; x < this->width; x++)
		{
			for (int y = 0; y < this->height; y++)
			{
				val = shortData[y * this->width + x];

				if (val < min)
					min = val;
				if (val > max)
					max = val;
			}
		}

		//LOGD("Save: max=%d min=%d", max, min);
		if (max != min)
		{
			for (int y = 0; y < this->height; y++)
			{
				png_byte* row = row_pointers[y];
				for (int x = 0; x < this->width; x++)
				{
					unsigned short int val = shortData[y * this->width + x];
					u8 valCalc = ((val - min) * 255) / (max - min);
					//LOGD("x=%d y=%d val=%d valCalc=%d", x, y, val, valCalc);
					row[x] = valCalc;
				}
			}

		}
	}

	// initialize stuff
	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

	if (!png_ptr)
	{
		LOGError("png_create_write_struct failed");
		return;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		LOGError("png_create_info_struct failed");
		return;
	}

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("init_io error");
		return;
	}

	png_init_io(png_ptr, fp);


	// write header
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("error writing header");
		return;
	}

	png_set_IHDR(png_ptr, info_ptr, width, height,
		     bit_depth, color_type, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

	png_write_info(png_ptr, info_ptr);

	// write bytes
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("error writing bytes");
		return;
	}

	png_write_image(png_ptr, row_pointers);

	// end write
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("error at end of write");
		return;
	}

	png_write_end(png_ptr, NULL);

	// cleanup heap allocation
	for (y=0; y < height; y++)
		free(row_pointers[y]);
	free(row_pointers);

	png_destroy_write_struct(&png_ptr, &info_ptr);

	fclose(fp);
	LOGD("CImageData (width=%d height=%d) saved as '%s'", this->width, this->height, fileName);
}

void CImageData::SaveScaled(const char *fileName, short int min, short int max)
{
	if (this->type != IMG_TYPE_GRAYSCALE
		&& this->type != IMG_TYPE_RGB
		&& this->type != IMG_TYPE_GRAYSCALE_16BIT)
	{
		LOGError("saving image type %2.2x not implemented (%s)", this->type, fileName);
		return;
	}
	png_byte color_type = PNG_COLOR_TYPE_GRAY;
	png_byte bit_depth = 8;

	png_structp png_ptr;
	png_infop info_ptr;
	png_bytep * row_pointers;

	int x, y;

	// create file
	FILE *fp = SYS_FopenUtf8(fileName, "wb");
	if (!fp)
	{
		LOGError("CImageData::Save: File %s could not be opened for writing", fileName);
		return;
	}

	row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * this->height);
	for (y=0; y < height; y++)
		row_pointers[y] = (png_byte*) malloc(this->width);

	if (this->type == IMG_TYPE_GRAYSCALE)
	{
		u8 *byteData = (u8 *)this->resultData;

		for (y = 0; y < height; y++)
		{
			png_byte* row = row_pointers[y];
			for (x = 0; x < width; x++)
			{
				u8 val = byteData[y * this->width + x];
				u8 valCalc = ((val - min) * 255) / (max - min);
				//LOGD("x=%d y=%d val=%d valCalc=%d", x, y, val, valCalc);
				row[x] = valCalc;
			}
		}
	}
	else if (this->type == IMG_TYPE_RGB)
	{
		u8 *byteData = (u8 *)this->resultData;
		//LOGD("SaveScaled: max=%d min=%d", max, min);
		if (max != min)
		{
			for (int y = 0; y < this->height; y++)
			{
				png_byte* row = row_pointers[y];
				for (int x = 0; x < this->width; x++)
				{
					u8 val = (byteData[y * (this->width * 3)+ (x * 3)]
							+ byteData[y * (this->width * 3)+ (x * 3) + 1]
							+ byteData[y * (this->width * 3)+ (x * 3) + 2]) / 3;
					u8 valCalc = ((val - min) * 255) / (max - min);
					//LOGD("x=%d y=%d val=%d valCalc=%d", x, y, val, valCalc);
					row[x] = valCalc;
				}
			}

		}
	}
	else if (this->type == IMG_TYPE_GRAYSCALE_16BIT)
	{
		unsigned short int *shortData = (unsigned short int *)this->resultData;

		LOGD("SaveScaled: max=%d min=%d", max, min);
		if (max != min)
		{
			for (int y = 0; y < this->height; y++)
			{
				png_byte* row = row_pointers[y];
				for (int x = 0; x < this->width; x++)
				{
					unsigned short int val = shortData[y * this->width + x];
					u8 valCalc = ((val - min) * 255) / (max - min);
					//LOGD("x=%d y=%d val=%d valCalc=%d", x, y, val, valCalc);
					row[x] = valCalc;
				}
			}

		}
	}

	// initialize stuff
	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

	if (!png_ptr)
	{
		LOGError("png_create_write_struct failed");
		return;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		LOGError("png_create_info_struct failed");
		return;
	}

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("init_io error");
		return;
	}

	png_init_io(png_ptr, fp);


	// write header
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("error writing header");
		return;
	}

	png_set_IHDR(png_ptr, info_ptr, width, height,
		     bit_depth, color_type, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

	png_write_info(png_ptr, info_ptr);

	// write bytes
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("error writing bytes");
		return;
	}

	png_write_image(png_ptr, row_pointers);

	/// end write
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("error at end of write");
		return;
	}

	png_write_end(png_ptr, NULL);

	// cleanup heap allocation
	for (y=0; y < height; y++)
		free(row_pointers[y]);
	free(row_pointers);

	row_pointers = NULL;

	png_destroy_write_struct(&png_ptr, &info_ptr);
	free(png_ptr);
	free(info_ptr);


	fclose(fp);
	LOGD("CImageData saved as '%s'", fileName);

}

const char *CImageData::GetLoadError()
{
	return stbi_failure_reason();
}

// Decode a JPEG that stb_image refused, after repairing it IN MEMORY.
//
// One repair, deliberately: trim trailing NUL padding and append the EOI
// marker. That is the damage seen in practice and the only one that can be
// fixed without inventing pixels. Anything else -- a broken header, a
// corrupt Huffman table -- still fails, and should.
//
// Returns true and fills resultData/width/height on success.
bool CImageData::LoadRepairedJpeg(const char *fileName)
{
	FILE *f = fopen(fileName, "rb");
	if (f == NULL) return false;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	// A guard, not a policy: this path runs only after a failed decode, but a
	// pathological file should not become a pathological allocation.
	const long kMaxRepairBytes = 256L * 1024L * 1024L;
	if (len < 4 || len > kMaxRepairBytes) { fclose(f); return false; }

	u8 *buf = (u8 *)malloc((size_t)len + 2);
	if (buf == NULL) { fclose(f); return false; }
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) { free(buf); return false; }

	// JPEG only. The missing-EOI repair means nothing for PNG or BMP, and
	// guessing at other formats would turn a clear failure into a vague one.
	if (!(buf[0] == 0xFF && buf[1] == 0xD8)) { free(buf); return false; }

	size_t end = (size_t)len;
	while (end > 2 && buf[end - 1] == 0x00)
		end--;
	// Already correctly terminated -- then the failure was something else and
	// re-decoding the same bytes would only fail again.
	if (end >= 2 && buf[end - 2] == 0xFF && buf[end - 1] == 0xD9) { free(buf); return false; }
	buf[end++] = 0xFF;
	buf[end++] = 0xD9;

	this->resultData = stbi_load_from_memory(buf, (int)end, &this->width, &this->height, NULL, 4);
	free(buf);
	return this->resultData != NULL;
}

// PNG via libpng. Replaces the stb_image path for .png (Load falls back to stb
// if this refuses the file, which libpng 1.6 does for some malformed PNGs that
// stb accepts -- a bad critical-chunk CRC, a broken zlib stream).
//
// Output contract is EXACTLY what stb_image produced here: tightly packed
// RGBA8, top row first, `new u8[]` so DeallocResult's delete[] matches. Every
// input colour type is normalised to that, so callers cannot tell which
// decoder ran -- except that the ICC profile now arrives in the same pass.
bool CImageData::LoadPNG(const char *fileName)
{
	FILE *fp = SYS_FopenUtf8(fileName, "rb");
	if (fp == NULL)
		return false;

	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png == NULL) { fclose(fp); return false; }
	png_infop info = png_create_info_struct(png);
	if (info == NULL) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return false; }

	u8 *pixels = NULL;
	png_bytep *rows = NULL;

	// libpng longjmps here on any error it raises. Everything freed below must
	// therefore be declared ABOVE this point and zero-initialised.
	if (setjmp(png_jmpbuf(png)))
	{
		delete [] pixels;
		free(rows);
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		return false;
	}

	png_init_io(png, fp);
	png_read_info(png, info);

	png_uint_32 w = 0, h = 0;
	int bitDepth = 0, colorType = 0;
	png_get_IHDR(png, info, &w, &h, &bitDepth, &colorType, NULL, NULL, NULL);

	// The ICC profile, in this pass. This is the whole metadata argument: with
	// stb the caller had to open the file a SECOND time and parse a header
	// just to find these bytes.
	{
		png_charp name = NULL;
		png_bytep profile = NULL;
		png_uint_32 profileLen = 0;
		int compression = 0;
		if (png_get_iCCP(png, info, &name, &compression, &profile, &profileLen) == PNG_INFO_iCCP
			&& profile != NULL && profileLen > 0)
		{
			SetIccProfile((u8 *)profile, (u32)profileLen);
		}
	}

	// 16-bit input KEEPS its 16 bits. A 16-bit PNG is what Lightroom and
	// Photoshop write when the point of the export was to avoid banding, so
	// truncating it at the decoder throws away the only thing that made the
	// file worth its size. The image comes out as IMG_TYPE_RGBA_16BIT and the GPU
	// path converts at upload -- see CSlrImage.
	//
	// libpng hands 16-bit samples over in PNG byte order (big-endian);
	// png_set_swap fixes that on a little-endian host so the buffer is native
	// unsigned short, which is what every reader of it assumes.
	const bool keep16 = (bitDepth == 16);
	if (keep16)
	{
#ifdef PNG_READ_SWAP_SUPPORTED
		if (PC_HostIsLittleEndian())
			png_set_swap(png);
#endif
	}
	if (colorType == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (colorType == PNG_COLOR_TYPE_RGB
		|| colorType == PNG_COLOR_TYPE_GRAY
		|| colorType == PNG_COLOR_TYPE_PALETTE)
	{
		// The filler is a SAMPLE VALUE, so it has to match the bit depth:
		// 0xFF into a 16-bit alpha channel is 255/65535 -- very nearly
		// transparent -- not opaque.
		png_set_filler(png, keep16 ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
	}
	if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	// Adam7: collapses the seven passes into whole rows for us.
	png_set_interlace_handling(png);
	png_read_update_info(png, info);

	// Refuse anything whose row stride is not the 4 bytes/pixel promised above
	// rather than writing past the buffer -- a transform that silently did not
	// apply must not become a heap overflow.
	const size_t sampleBytes = keep16 ? 2 : 1;
	const png_size_t rowBytes = png_get_rowbytes(png, info);
	if (w == 0 || h == 0 || rowBytes != (png_size_t)w * 4 * sampleBytes)
	{
		LOGError("CImageData::LoadPNG: unexpected %lu-byte rows for %ux%u RGBA%s in '%s'",
		         (unsigned long)rowBytes, (unsigned)w, (unsigned)h,
		         keep16 ? "16" : "8", fileName);
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		return false;
	}

	// Allocated as unsigned short[] for the 16-bit case so DeallocResult's
	// delete[] (unsigned short int*) matches the array form exactly.
	pixels = keep16
		? (u8 *)new unsigned short int[(size_t)w * (size_t)h * 4]
		: new u8[(size_t)w * (size_t)h * 4];
	rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)h);
	if (rows == NULL)
	{
		delete [] pixels;
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		return false;
	}
	for (png_uint_32 y = 0; y < h; y++)
		rows[y] = pixels + (size_t)y * (size_t)w * 4 * sampleBytes;

	png_read_image(png, rows);
	png_read_end(png, NULL);

	free(rows);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(fp);

	this->width      = (int)w;
	this->height     = (int)h;
	this->type       = keep16 ? IMG_TYPE_RGBA_16BIT : IMG_TYPE_RGBA;
	this->resultData = pixels;
	return true;
}

bool CImageData::Load(const char *fileName, bool dealloc)
{
	LOGR("CImageData::Load: %s", fileName);

	// Drop any profile from a previous load BEFORE the extension dispatch
	// below. The specialized loaders (TIFF/WebP/HEIF/AVIF/RAW) return without
	// ever reaching the `if (dealloc) DeallocImage()` further down, so without
	// this a tagged image reloaded as an untagged one would keep the old
	// profile and be colour-managed as the wrong file. The profile describes
	// the file being loaded, so this is never conditional on `dealloc`.
	DeallocIccProfile();
	// Same reasoning, same unconditional reset: the specialised loaders below
	// return without reaching this class's own dealloc, so a stale hint would
	// otherwise describe the PREVIOUS file.
	previewColorHint       = EExifColorSpaceHint::Unknown;
	previewColorHintSource = EExifColorHintSource::None;
	decodeWasRepaired      = false;

	// Free the PREVIOUS pixel buffer BEFORE the extension dispatch: the
	// specialized loaders below assign fresh buffers without freeing the old
	// one and return without reaching the stbi path's dealloc, so a reused
	// CImageData leaked its previous image (programme review 2026-08-11).
	// DeallocImage() is idempotent, so the KTX2 loader's own dealloc and the
	// stbi path both stay correct.
	if (dealloc)
		DeallocImage();

	// .ktx2 files go through the KTX2/UASTC transcode path; all else unchanged.
	const char *ext = IMG_FileExtension(fileName);
	if (strcasecmp(ext, ".ktx2") == 0)
		return this->LoadKTX2(fileName);
	if (strcasecmp(ext, ".tiff") == 0 || strcasecmp(ext, ".tif") == 0)
		return this->LoadTIFF(fileName);
	if (strcasecmp(ext, ".webp") == 0)
		return this->LoadWebP(fileName);
	if (strcasecmp(ext, ".heic") == 0 || strcasecmp(ext, ".heif") == 0)
		return this->LoadHEIF(fileName);
	if (strcasecmp(ext, ".avif") == 0)
		return this->LoadAVIF(fileName);
	{
		static const char *rawExts[] = {
			".cr2", ".cr3", ".nef", ".arw", ".dng", ".raf", ".rw2", ".orf", ".pef", nullptr
		};
		for (int i = 0; rawExts[i]; i++)
			if (strcasecmp(ext, rawExts[i]) == 0) return this->LoadRAWPreview(fileName);
	}

	// PNG goes to libpng, not stb_image: same pixels, measurably faster with
	// SIMD, and the ICC profile comes back in this one pass instead of costing
	// a second read of the file header. A refusal falls through to the stb
	// chain below -- libpng 1.6 rejects some malformed PNGs that stb accepts,
	// and showing a damaged picture beats showing none.
	if (strcasecmp(ext, ".png") == 0)
	{
		if (this->LoadPNG(fileName))
			return true;
		LOGD("CImageData::Load: libpng refused '%s'; retrying with stb_image", fileName);
	}

	this->type = IMG_TYPE_RGBA;
	this->resultData = stbi_load(fileName, &this->width, &this->height, NULL, 4);
	if (this->resultData == NULL)
	{
		// SALVAGE PASS. stb_image is strict about the trailing EOI marker: it
		// decodes the entire image, then throws the result away with
		// "expected marker" because the file does not end in FF D9. Files
		// like that are common in the wild -- an interrupted copy or a
		// download that padded the tail with NULs -- and macOS Preview,
		// Finder and every browser show them without complaint, so a
		// photographer reasonably reads our refusal as OUR bug.
		//
		// Retry from memory with the file repaired: drop the NUL padding and
		// append the EOI that should have been there. If the entropy data is
		// genuinely short, stb pads the missing part and we get however much
		// of the picture survived -- which is the point. Nothing is written
		// to disk; the file is not ours to modify.
		if (LoadRepairedJpeg(fileName))
		{
			decodeWasRepaired = true;
			LOGD("CImageData::Load: %s repaired (missing EOI) -- decoded %dx%d",
			     fileName, this->width, this->height);
		}
	}
	if (this->resultData == NULL)
	{
		// LAST RESORT: the operating system's own decoder. stb_image aborts on
		// the first bad symbol; ImageIO and WIC decode up to the damage and
		// keep what they got, which is why files we refuse open in Preview.
		// Measured on a real library: this recovers photos with corruption
		// INSIDE the entropy stream, which no amount of marker repair can
		// reach. Returns false on Linux, where there is no such decoder.
		if (LoadWithPlatformDecoder(fileName))
		{
			decodeWasRepaired = true;
			LOGD("CImageData::Load: %s recovered by the platform decoder -- %dx%d",
			     fileName, this->width, this->height);
		}
	}
	if (this->resultData == NULL)
	{
		LOGError("CImageData::Load: %s failed", fileName);
	}
	else
	{
		// stb_image discards every byte of metadata, so the profile needs its
		// own pass over the file. ReadFileHeader, not ReadFile: this runs on
		// the decode workers for every JPEG/PNG/BMP, and ReadFile would read up
		// to 16 MB and build tag strings we immediately throw away. 128 KB is
		// enough -- APP2 precedes SOS and iCCP precedes IDAT by spec.
		CExifData iccOnly = CExifReader::ReadFileHeader(fileName, 131072, true);
		if (!iccOnly.iccProfile.empty())
			SetIccProfile(&iccOnly.iccProfile[0], (u32)iccOnly.iccProfile.size());
	}

	/*
	
	std::vector<unsigned char> image;
	unsigned imgWidth, imgHeight;
	unsigned error = lodepng::decode(image, imgWidth, imgHeight, fileName);
	
	type = IMG_TYPE_RGBA;
	this->width = imgWidth;
	this->height = imgHeight;
	
	// If there's an error, display it.
	if(error != 0)
	{
		LOGError("LodePNG error: %s fileName=%s", lodepng_error_text(error), fileName);
		return false;
	}
	
	// Here the PNG is loaded in "image". All the rest of the code is SDL and OpenGL stuff.
	
	// transfer raw pointers to CImageData
	if (dealloc)
		this->resultData = new u8[this->width * this->height * 4];
	
	u8 *byteData = (u8 *)this->resultData;
	
	u32 imageSize = width * height * 4;
	
	for (u32 z = 0; z < imageSize; z++)
	{
		byteData[z] = image[z];
	}
	*/
	
	/*
#if !defined(IOS) || (defined(MACOS) && MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_9)

	LOGR("CImageData::Load: %s", fileName);
	if (dealloc)
		DeallocImage();

	png_u8 color_type;
	png_u8 bit_depth;

	png_structp png_ptr;
	png_infop info_ptr;
	int number_of_passes;
	//png_bytep *row_pointers;

	png_u8 header[8];	// 8 is the maximum size that can be checked

#if defined(ANDROID)
	// synchronous
	SYS_ApkOpenFile(fileName);
	SYS_ApkFileRead(header, 8);
#else

	int x, y;

	// open file and test for it being a png
	FILE *fp = SYS_FopenUtf8(fileName, "rb");
	if (!fp)
	{
		LOGError("CImageData::Load: '%s' not found", fileName);
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	fread(header, 1, 8, fp);
#endif

	if (png_sig_cmp(header, 0, 8))
	{
		LOGError("CImageData::Load: '%s' is not png", fileName);
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	// initialize stuff
	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

	if (!png_ptr)
	{
		LOGError("png_create_read_struct failed");
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		LOGError("png_create_info_struct failed");
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		LOGError("init_io error");
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

#if defined(ANDROID)
	png_set_read_fn(png_ptr, NULL, png_zip_read);
#else
	png_init_io(png_ptr, fp);
#endif

	png_set_sig_bytes(png_ptr, 8);

	png_read_info(png_ptr, info_ptr);

	width = info_ptr->width;
	height = info_ptr->height;
	color_type = info_ptr->color_type;
	bit_depth = info_ptr->bit_depth;

	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth == 8)
	{
		LOGD("PNG_COLOR_TYPE_GRAY");
		number_of_passes = png_set_interlace_handling(png_ptr);
		png_read_update_info(png_ptr, info_ptr);

		// read file
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			LOGError("error during reading image");
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}

		if (dealloc)
		{
			row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
			for (y=0; y < height; y++)
				row_pointers[y] = (png_byte*) malloc(info_ptr->rowbytes);
		}

		png_read_image(png_ptr, row_pointers);
#if defined(ANDROID)
		SYS_ApkCloseFile();
#else
		fclose(fp);
#endif

		type = IMG_TYPE_GRAYSCALE;

		// transfer row pointers to CImageData
		if (dealloc)
			this->resultData = new byte[this->width * this->height];

		u8 *byteData = (u8 *)this->resultData;

		for (y = 0; y < height; y++)
		{
			png_byte* row = row_pointers[y];
			for (x = 0; x < width; x++)
			{
				//png_byte* ptr = &(row[x]);	 // *4
				byteData[y * this->width + x] = row[x];
			}
		}

		//png_read_end(png_ptr, info_ptr);
		png_read_destroy(png_ptr, info_ptr, (png_infop)0);
		free(png_ptr);
		free(info_ptr);
	}
	else if (color_type == PNG_COLOR_TYPE_RGB && bit_depth == 8)
	{
		//LOGD("PNG_COLOR_TYPE_RGB");
		number_of_passes = png_set_interlace_handling(png_ptr);
		png_read_update_info(png_ptr, info_ptr);

		// read file
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			LOGError("error during reading image");
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}

		if (dealloc)
		{
			row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
			for (y=0; y < height; y++)
				row_pointers[y] = (png_byte*) malloc(info_ptr->rowbytes);
		}

		png_read_image(png_ptr, row_pointers);

#if defined(ANDROID)
		SYS_ApkCloseFile();
#else
		fclose(fp);
#endif

//		type = IMG_TYPE_GRAYSCALE;
//
//		// transfer row pointers to CImageData
//		if (dealloc)
//			this->resultData = new byte[this->width * this->height];
//
//		u8 *byteData = (u8 *)this->resultData;
//
//		for (y = 0; y < height; y++)
//		{
//			png_byte* row = row_pointers[y];
//			for (x = 0; x < width; x++)
//			{
//				float val = (row[x*3] + row[x*3 + 1] + row[x*3 + 2]);
//				u8 val2 = (byte)(val / 3.0f);
//				byteData[y * this->width + x] = val2;
//			}
//		}
	 

		//LOGD("convert to RGBA");
		type = IMG_TYPE_RGBA;

		// transfer row pointers to CImageData
		if (dealloc)
			this->resultData = new byte[this->width * this->height * 4];

		u8 *byteData = (u8 *)this->resultData;

		for (y = 0; y < height; y++)
		{
			//LOGD("y=%d", y);
#ifdef FLIP_VERTICAL
			png_byte* row = row_pointers[(this->height-y)-1];
#else
			png_byte* row = row_pointers[y];
#endif
			for (x = 0; x < width; x++)
			{
				//LOGD("x=%d", x);
				u8 r = row[x*3];
				u8 g = row[x*3 + 1];
				u8 b = row[x*3 + 2];
				//LOGD("%4d %4d %2.2x %2.2x %2.2x %2.2x", y, x, r, g, b, a);
				byteData[y * this->width*4 + (x*4)] = r;
				byteData[y * this->width*4 + (x*4) + 1] = g;
				byteData[y * this->width*4 + (x*4) + 2] = b;
				byteData[y * this->width*4 + (x*4) + 3] = 255;
			}
		}

		//png_read_end(png_ptr, info_ptr);
		png_read_destroy(png_ptr, info_ptr, (png_infop)0);
		free(png_ptr);
		free(info_ptr);
	}
	else if (color_type == PNG_COLOR_TYPE_RGB_ALPHA && bit_depth == 8)
	{
		//LOGD("PNG_COLOR_TYPE_RGB_ALPHA");
		number_of_passes = png_set_interlace_handling(png_ptr);
		png_read_update_info(png_ptr, info_ptr);

		//LOGD("rowbytes=%d", info_ptr->rowbytes);
		//LOGD("number_of_passes=%d", number_of_passes);

		// read file
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			LOGError("error during reading image");
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}

		if (dealloc)
		{
			row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
			for (y=0; y < height; y++)
				row_pointers[y] = (png_byte*) malloc(info_ptr->rowbytes);
		}

		png_read_image(png_ptr, row_pointers);

		#if defined(ANDROID)
		SYS_ApkCloseFile();
#else
		fclose(fp);
#endif

		//LOGD("transfer row pointers");
		type = IMG_TYPE_RGBA;

		// transfer row pointers to CImageData
		if (dealloc)
			this->resultData = new byte[this->width * this->height * 4];

		u8 *byteData = (u8 *)this->resultData;

		for (y = 0; y < height; y++)
		{
			//LOGD("y=%d", y);
#ifdef FLIP_VERTICAL
			png_byte* row = row_pointers[(this->height - y) -1];
#else
			png_byte* row = row_pointers[y];
#endif
			for (x = 0; x < width; x++)
			{
				//LOGD("x=%d", x);
				u8 r = row[x*4];
				u8 g = row[x*4 + 1];
				u8 b = row[x*4 + 2];
				u8 a = row[x*4 + 3];
				//LOGD("%4d %4d %2.2x %2.2x %2.2x %2.2x", y, x, r, g, b, a);
				byteData[y * this->width*4 + (x*4)] = r;
				byteData[y * this->width*4 + (x*4) + 1] = g;
				byteData[y * this->width*4 + (x*4) + 2] = b;
				byteData[y * this->width*4 + (x*4) + 3] = a;
			}
		}

		//png_read_end(png_ptr, info_ptr);
		png_read_destroy(png_ptr, info_ptr, (png_infop)0);
		free(png_ptr);
		free(info_ptr);
	}
	else if (color_type == PNG_COLOR_TYPE_RGB && bit_depth == 16)
	{
		//LOGD("PNG_COLOR_TYPE_RGB bit depth=16");
		number_of_passes = png_set_interlace_handling(png_ptr);
		png_read_update_info(png_ptr, info_ptr);

		// read file
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			LOGError("error during reading image");
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}

		if (dealloc)
		{
			row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
			for (y=0; y < height; y++)
				row_pointers[y] = (png_byte*) malloc(info_ptr->rowbytes);
		}

		png_read_image(png_ptr, row_pointers);

#if defined(ANDROID)
		SYS_ApkCloseFile();
#else
		fclose(fp);
#endif

		//LOGD("convert to RGBA");
		type = IMG_TYPE_RGBA;

		// transfer row pointers to CImageData
		if (dealloc)
			this->resultData = new byte[this->width * this->height * 4];

		u8 *byteData = (u8 *)this->resultData;

		for (y = 0; y < height; y++)
		{
			//LOGD("y=%d", y);
#ifdef FLIP_VERTICAL
			png_byte* row = row_pointers[(this->height-y)-1];
#else
			png_byte* row = row_pointers[y];
#endif
			for (x = 0; x < width; x++)
			{
				//LOGD("x=%d", x);
				u8 r = row[x*6];
				u8 g = row[x*6 + 2];
				u8 b = row[x*6 + 4];
				//LOGD("%4d %4d %2.2x %2.2x %2.2x %2.2x", y, x, r, g, b, a);
				byteData[y * this->width*4 + (x*4)] = r;
				byteData[y * this->width*4 + (x*4) + 1] = g;
				byteData[y * this->width*4 + (x*4) + 2] = b;
				byteData[y * this->width*4 + (x*4) + 3] = 255;
			}
		}

		//png_read_end(png_ptr, info_ptr);
		png_read_destroy(png_ptr, info_ptr, (png_infop)0);
		free(png_ptr);
		free(info_ptr);
	}
	else if (color_type == PNG_COLOR_TYPE_RGB_ALPHA && bit_depth == 16)
	{
		//LOGD("PNG_COLOR_TYPE_RGB bit depth=16");
		number_of_passes = png_set_interlace_handling(png_ptr);
		png_read_update_info(png_ptr, info_ptr);

		// read file
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			LOGError("error during reading image");
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}

		if (dealloc)
		{
			row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
			for (y=0; y < height; y++)
				row_pointers[y] = (png_byte*) malloc(info_ptr->rowbytes);
		}

		png_read_image(png_ptr, row_pointers);

#if defined(ANDROID)
		SYS_ApkCloseFile();
#else
		fclose(fp);
#endif

		//LOGD("convert to RGBA");
		type = IMG_TYPE_RGBA;

		// transfer row pointers to CImageData
		if (dealloc)
			this->resultData = new byte[this->width * this->height * 4];

		u8 *byteData = (u8 *)this->resultData;

		for (y = 0; y < height; y++)
		{
			//LOGD("y=%d", y);
#ifdef FLIP_VERTICAL
			png_byte* row = row_pointers[(this->height-y)-1];
#else
			png_byte* row = row_pointers[y];
#endif
			for (x = 0; x < width; x++)
			{
				//LOGD("x=%d", x);
				u8 r = row[x*8];
				u8 g = row[x*8 + 2];
				u8 b = row[x*8 + 4];
				u8 a = row[x*8 + 6];
				//LOGD("%4d %4d %2.2x %2.2x %2.2x %2.2x", y, x, r, g, b, a);
				byteData[y * this->width*4 + (x*4)] = r;
				byteData[y * this->width*4 + (x*4) + 1] = g;
				byteData[y * this->width*4 + (x*4) + 2] = b;
				byteData[y * this->width*4 + (x*4) + 3] = a;
			}
		}

		//png_read_end(png_ptr, info_ptr);
		png_read_destroy(png_ptr, info_ptr, (png_infop)0);
		free(png_ptr);
		free(info_ptr);
	}
	else
	{
		LOGError("unknown png type: color_type=%d bit_depth=%d", color_type, bit_depth);
		this->type = IMG_TYPE_UNKNOWN;
		//png_read_end(png_ptr, info_ptr);
		png_read_destroy(png_ptr, info_ptr, (png_infop)0);
		free(png_ptr);
		free(info_ptr);
		return false;
	}

	LOGR("Image loaded from '%s'", fileName);
	return true;

#else

	SYS_FatalExit("Not supported on iOS");

#endif
	 */

	return this->resultData != nullptr;
}

// =====================================================================
// KTX2 / UASTC compressed-image decode path (design note §0, §9).
//
// Two outcomes, decided by gRenderBackend->GetPreferredCompressedFormat():
//
//  * BC7 / ASTC_4x4 -> transcode EVERY mip level to that GPU block format,
//    fill compressedMips[], set type=IMG_TYPE_GPU_COMPRESSED / isCompressed.
//
//  * IMG_GPU_UNCOMPRESSED (or no render backend) -> transcode MIP 0 ONLY to
//    cTFRGBA32, populate resultData as a normal IMG_TYPE_RGBA image
//    (isCompressed=false). It then flows through the existing RGBA path
//    unchanged. Mips are dropped in this fallback (legacy path is single-level).
// =====================================================================
bool CImageData::LoadKTX2(const char *fileName)
{
	LOGR("CImageData::LoadKTX2: %s", fileName);
	DeallocImage();

	// --- pick target GPU format from the render backend capability API ---
	// EImageGpuFormat / gRenderBackend reached via VID_Main.h (-> CRenderBackend.h
	// -> EImageGpuFormat.h). Do NOT #include EImageGpuFormat.h directly: it lives
	// under Core/Render/ and is not on the Core/ include path (design note §8.2).
	EImageGpuFormat gpuFormat = IMG_GPU_UNCOMPRESSED;
	if (gRenderBackend != NULL)
		gpuFormat = gRenderBackend->GetPreferredCompressedFormat();

	const bool wantCompressed = (gpuFormat == IMG_GPU_BC7 || gpuFormat == IMG_GPU_ASTC_4x4);

	basist::transcoder_texture_format tf;
	if (gpuFormat == IMG_GPU_ASTC_4x4)
		tf = basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
	else if (gpuFormat == IMG_GPU_BC7)
		tf = basist::transcoder_texture_format::cTFBC7_RGBA;
	else
		tf = basist::transcoder_texture_format::cTFRGBA32;  // uncompressed fallback

	// --- read whole file into memory ---
	FILE *fp = SYS_FopenUtf8(fileName, "rb");
	if (!fp)
	{
		LOGError("CImageData::LoadKTX2: '%s' not found", fileName);
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}
	fseek(fp, 0, SEEK_END);
	long fileSize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (fileSize <= 0)
	{
		LOGError("CImageData::LoadKTX2: '%s' empty or unreadable", fileName);
		fclose(fp);
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}
	u8 *fileBytes = new u8[fileSize];
	size_t rd = fread(fileBytes, 1, fileSize, fp);
	fclose(fp);
	if ((long)rd != fileSize)
	{
		LOGError("CImageData::LoadKTX2: short read on '%s'", fileName);
		delete [] fileBytes;
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	// --- safe preflight: parse identifier, header, level index ---
	KTX2HeaderInfo ktx2Hdr;
	bool validHeader = KTX2_ReadHeaderForDispatch(fileBytes, (size_t)fileSize, ktx2Hdr);

	// --- dispatch ---
	// BasisLZ (supercompression=1) or vkFormat=UNDEFINED → existing transcoder path
	// Concrete vkFormat with no supercompression → new CKTX2Loader software path
	// Anything else → clean failure

	if (!validHeader) {
		LOGError("CImageData::LoadKTX2: invalid KTX2 container '%s'", fileName);
		delete [] fileBytes;
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	const bool isBasis = (ktx2Hdr.supercompressionScheme == 1 /*BasisLZ*/
	                      || ktx2Hdr.vkFormat == 0 /*UNDEFINED — UASTC in DFD*/);
	const bool isConcrete = (!isBasis && ktx2Hdr.supercompressionScheme == 0);

	if (!isBasis && !isConcrete) {
		LOGError("CImageData::LoadKTX2: unsupported supercompression scheme %u in '%s'",
		         ktx2Hdr.supercompressionScheme, fileName);
		delete [] fileBytes;
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	if (isConcrete) {
		// --- concrete-format software decode ---
		KTX2DecodeResult res;
		if (!KTX2_DecodeToRGBA8(fileBytes, (size_t)fileSize, res)) {
			LOGError("CImageData::LoadKTX2: concrete-format decode failed for '%s'", fileName);
			delete [] fileBytes;
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}
		delete [] fileBytes;
		this->width      = res.width;
		this->height     = res.height;
		this->type       = IMG_TYPE_RGBA;
		this->resultData = res.rgba8;    // CImageData takes ownership
		this->isCompressed = false;
		return true;
	}

	// --- BasisLZ / UASTC path (unchanged) ---
	basist::basisu_transcoder_init();
	basist::ktx2_transcoder transcoder;
	if (!transcoder.init(fileBytes, (uint32_t)fileSize))
	{
		LOGError("CImageData::LoadKTX2: ktx2_transcoder::init failed for '%s'", fileName);
		delete [] fileBytes;
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}
	if (!transcoder.start_transcoding())
	{
		LOGError("CImageData::LoadKTX2: start_transcoding failed for '%s'", fileName);
		delete [] fileBytes;
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	uint32_t levels = transcoder.get_levels();
	if (levels == 0)
	{
		LOGError("CImageData::LoadKTX2: '%s' has 0 mip levels", fileName);
		delete [] fileBytes;
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	if (!wantCompressed)
	{
		// ----- uncompressed fallback: transcode mip 0 only to RGBA32 -----
		basist::ktx2_image_level_info li;
		if (!transcoder.get_image_level_info(li, 0, 0, 0))
		{
			LOGError("CImageData::LoadKTX2: get_image_level_info(0) failed for '%s'", fileName);
			delete [] fileBytes;
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}

		int logicalW = (int)li.m_orig_width;
		int logicalH = (int)li.m_orig_height;
		u8 *rgba = new u8[(size_t)logicalW * logicalH * 4];

		// cTFRGBA32: output buffer size and row pitch are expressed in PIXELS.
		// Transcode directly at the logical dimensions so resultData is a plain
		// tightly-packed RGBA8 image, identical to the stbi_load layout.
		if (!transcoder.transcode_image_level(0, 0, 0,
											  rgba, (uint32_t)(logicalW * logicalH),
											  tf, 0, (uint32_t)logicalW, (uint32_t)logicalH))
		{
			LOGError("CImageData::LoadKTX2: RGBA32 transcode of mip 0 failed for '%s'", fileName);
			delete [] rgba;
			delete [] fileBytes;
			this->type = IMG_TYPE_UNKNOWN;
			return false;
		}

		delete [] fileBytes;

		this->width = logicalW;
		this->height = logicalH;
		this->type = IMG_TYPE_RGBA;
		this->resultData = rgba;
		this->isCompressed = false;
		this->compressedGpuFormat = 0;
		this->compressedMipCount = 0;
		this->compressedMips = NULL;
		LOGR("CImageData::LoadKTX2: '%s' decoded to RGBA32 fallback %dx%d", fileName, logicalW, logicalH);
		return true;
	}

	// ----- compressed path: transcode every mip level to GPU blocks -----
	this->width  = (int)transcoder.get_width();
	this->height = (int)transcoder.get_height();
	LOGR("CImageData::LoadKTX2: %s  %dx%d  %d mip levels  gpuFormat=%d",
		 fileName, this->width, this->height, levels, (int)gpuFormat);

	this->compressedMips = new SCompressedMip[levels];
	this->compressedMipCount = 0;

	// BC7 and ASTC 4x4 are both 16 bytes per 4x4 block (design note §8.6).
	const uint32_t bytesPerBlock = 16;

	bool ok = true;
	for (uint32_t level = 0; level < levels; level++)
	{
		basist::ktx2_image_level_info li;
		if (!transcoder.get_image_level_info(li, level, 0, 0))
		{
			LOGError("CImageData::LoadKTX2: get_image_level_info failed level=%d", level);
			ok = false;
			break;
		}

		uint32_t totalBlocks = li.m_total_blocks;
		uint32_t outSize = totalBlocks * bytesPerBlock;
		u8 *blocks = new u8[outSize];

		if (!transcoder.transcode_image_level(level, 0, 0, blocks, totalBlocks, tf))
		{
			LOGError("CImageData::LoadKTX2: transcode_image_level failed level=%d", level);
			delete [] blocks;
			ok = false;
			break;
		}

		// GOTCHA §8.1: store BOTH logical (orig) and physical (block-padded) sizes.
		this->compressedMips[level].origWidth  = (int)li.m_orig_width;
		this->compressedMips[level].origHeight = (int)li.m_orig_height;
		this->compressedMips[level].physWidth  = (int)li.m_width;
		this->compressedMips[level].physHeight = (int)li.m_height;
		this->compressedMips[level].blockData = blocks;
		this->compressedMips[level].blockDataSize = outSize;
		this->compressedMipCount++;

		LOGR("  mip %d: orig %dx%d  phys %dx%d  blocks=%dx%d  payload=%d bytes",
			 level, li.m_orig_width, li.m_orig_height, li.m_width, li.m_height,
			 li.m_num_blocks_x, li.m_num_blocks_y, outSize);
	}

	delete [] fileBytes;

	if (!ok)
	{
		DeallocCompressed();
		this->type = IMG_TYPE_UNKNOWN;
		return false;
	}

	this->type = IMG_TYPE_GPU_COMPRESSED;
	this->isCompressed = true;
	this->compressedGpuFormat = (u8)gpuFormat;
	LOGR("CImageData::LoadKTX2: '%s' transcoded OK, %d mips, gpuFormat=%d",
		 fileName, this->compressedMipCount, (int)gpuFormat);
	return true;
}

void CImageData::RawSave(const char *fileName)
{
	FILE *fp = SYS_FopenUtf8(fileName, "wb");
	if (!fp)
	{
		LOGError("CImageData::Save: fp NULL (%s)", fileName);
		//log_backtrace();
		return;
	}

	fwrite(&(this->width), sizeof(int), 1, fp);
	fwrite(&(this->height), sizeof(int), 1, fp);
	fwrite(this->resultData, 1, this->width * this->height, fp);
	fclose(fp);
	LOGD("CImageData saved as '%s'", fileName);
}

void CImageData::RawLoad(const char *fileName)
{
	FILE *fp = SYS_FopenUtf8(fileName, "rb");
	if (!fp)
	{
		LOGError("CImageData::Load: fp NULL (%s)", fileName);
		//log_backtrace();
		return;
	}

	DeallocImage();

	fread(&(this->width), sizeof(int), 1, fp);
	fread(&(this->height), sizeof(int), 1, fp);
	resultData = new u8[this->width * this->height];
	type = IMG_TYPE_GRAYSCALE;
	fread(this->resultData, 1, this->width*this->height, fp);
	fclose(fp);
	LOGD("CImageData loaded from '%s'", fileName);
}

void CImageData::LoadFromByteBufferUncompressed(CByteBuffer *byteBuffer)
{
	DeallocImage();
	u8 m = byteBuffer->GetByte();
	if (m != 'G')
	{
		LOGError("CImageData::LoadFromByteBufferUncompressed: magic not found");
		return;
	}
	u8 v = byteBuffer->GetByte();
	if (v != 0x01)
	{
		LOGError("CImageData::LoadFromByteBufferUncompressed: version unknown (%2.2x)", v);
		return;
	}
	this->width = byteBuffer->GetI32();
	this->height = byteBuffer->GetI32();
	this->type = byteBuffer->GetByte();
	
	int len = GetDataLength();
	resultData = byteBuffer->getBytes(len);

}

void CImageData::StoreToByteBufferUncompressed(CByteBuffer *byteBuffer)
{
	byteBuffer->PutByte('G');
	byteBuffer->PutByte(0x01);
	byteBuffer->PutI32(this->width);
	byteBuffer->PutI32(this->height);
	byteBuffer->PutByte(this->type);
	
	int len = GetDataLength();
	byteBuffer->PutBytes((u8*)resultData, len);
}

int CImageData::GetDataLength()
{
	int len = -1;
	if (type == IMG_TYPE_GRAYSCALE)
	{
		len = this->width * this->height;
	}
	else if (type == IMG_TYPE_RGB)
	{
		len = this->width * this->height * 3;
	}
	else if (type == IMG_TYPE_RGBA)
	{
		len = this->width * this->height * 4;
	}
	else SYS_FatalExit("not implemented");

	return len;
}

void CImageData::FlipVertically()
{
	u8 *imageData = (u8*)resultData;
	
//	unsigned int offset = y * width * 4 + x * 4;

	int w = this->width*4;
	int h = this->height;
	
	for (int y = 0; y < this->height/2; y++)
	{
		for (int x = 0; x < this->width; x++)
		{
			u8 r = imageData[y*w + (x*4) + 0];
			u8 g = imageData[y*w + (x*4) + 1];
			u8 b = imageData[y*w + (x*4) + 2];
			u8 a = imageData[y*w + (x*4) + 3];
			
			imageData[y*w + (x*4) + 0] = imageData[(h-1-y)*w + (x*4) + 0];
			imageData[y*w + (x*4) + 1] = imageData[(h-1-y)*w + (x*4) + 1];
			imageData[y*w + (x*4) + 2] = imageData[(h-1-y)*w + (x*4) + 2];
			imageData[y*w + (x*4) + 3] = imageData[(h-1-y)*w + (x*4) + 3];
			
			imageData[(h-1-y)*w + (x*4) + 0] = r;
			imageData[(h-1-y)*w + (x*4) + 1] = g;
			imageData[(h-1-y)*w + (x*4) + 2] = b;
			imageData[(h-1-y)*w + (x*4) + 3] = a;
			
		}
	}
}

// nearest neighbor && grayscale only
void CImageData::Scale(float scaleX, float scaleY)
{
	LOGD("CImageData::Scale");

	int newWidth = this->width * scaleX;
	int newHeight = this->height * scaleY;

	int oldWidth = this->width;

	this->width = newWidth;
	this->height = newHeight;

	float scaleStepX = 1 / scaleX; //this->width / newWidth;
	float scaleStepY = 1 / scaleY; //this->height / newHeight;

	LOGD("scaleStepX = %f scaleStepY = %f", scaleStepX, scaleStepY);

	if (this->type != IMG_TYPE_GRAYSCALE)
	{
		LOGError("scale for img type %d not implemented yet", this->type);
		SYS_FatalExit();
	}

	u8 *data = (u8 *)this->resultData;

	this->resultData = NULL;
	this->AllocResultImage();

#ifdef USE_BUFFER_OFFSETS
	this->bufferOffsets = IMG_GetBufferOffsets(this->type, this->height, this->width);
#endif

	u8 *newData = (u8 *)this->resultData;

	float origPosX = 0;
	float origPosY = 0;
	for (int x = 0; x < newWidth; x++)
	{
		origPosY = 0;
		for (int y = 0; y < newHeight; y++)
		{
			u8 val = data[(int)origPosY * oldWidth + (int)origPosX];
			newData[y * newWidth +x] = val;
			origPosY += scaleStepY;
		}
		origPosX += scaleStepX;
	}

	delete [] data;
	this->resultData = newData;
	LOGD("CImageData::Scale finished");
}

void CImageData::DrawLine(int startX, int startY, int endX, int endY, u8 r, u8 g, u8 b)
{
	int x0 = startX;
	int y0 = startY;
	int x1 = endX;
	int y1 = endY;

	bool steep = abs(y1 - y0) > abs(x1 - x0);

	if (steep)
	{
		int tmp = x0;
		x0 = y0;
		y0 = tmp;

		tmp = x1;
		x1 = y1;
		y1 = tmp;
	}

	if (x0 > x1)
	{
		int tmp = x0;
		x0 = x1;
		x1 = tmp;

		tmp = y0;
		y0 = y1;
		y1 = tmp;
	}
	int deltax = x1 - x0;
	int deltay = abs(y1 - y0);
	int error = deltax / 2;
	int ystep;
	int y = y0;
	if (y0 < y1)
	{
		ystep = 1;
	}
	else
	{
		ystep = -1;
	}

	for (int x = x0; x <= x1; x++)
	{
		if (steep)
		{
			this->SetPixelResultRGB(y, x, r, g, b);
		}
		else
		{
			this->SetPixelResultRGB(x, y, r, g, b);
		}
		error = error - deltay;
		if (error < 0)
		{
			y = y + ystep;
			error = error + deltax;
		}
	}
}

void CImageData::DrawLine(int startX, int startY, int endX, int endY, u8 r, u8 g, u8 b, u8 a, int thickness)
{
	int thickness2 = thickness/2;
	
	int x0 = startX;
	int y0 = startY;
	int x1 = endX;
	int y1 = endY;

	bool steep = abs(y1 - y0) > abs(x1 - x0);

	if (steep)
	{
		int tmp = x0;
		x0 = y0;
		y0 = tmp;

		tmp = x1;
		x1 = y1;
		y1 = tmp;
	}

	if (x0 > x1)
	{
		int tmp = x0;
		x0 = x1;
		x1 = tmp;

		tmp = y0;
		y0 = y1;
		y1 = tmp;
	}
	int deltax = x1 - x0;
	int deltay = abs(y1 - y0);
	int error = deltax / 2;
	int ystep;
	int y = y0;
	if (y0 < y1)
	{
		ystep = 1;
	}
	else
	{
		ystep = -1;
	}

	for (int x = x0; x <= x1; x++)
	{
		if (steep)
		{
			this->DrawFilledRectangle(y-thickness2, x-thickness2, y+thickness2, x+thickness2, r, g, b, a);
//			this->SetPixelResultRGBA(y, x, r, g, b, a);
		}
		else
		{
			this->DrawFilledRectangle(x-thickness2, y-thickness2, x+thickness2, y+thickness2, r, g, b, a);
//			this->SetPixelResultRGBA(x, y, r, g, b, a);
		}
		error = error - deltay;
		if (error < 0)
		{
			y = y + ystep;
			error = error + deltax;
		}
	}
}

void CImageData::DrawFilledRectangle(int leftX, int topY, int rightX, int bottomY, u8 r, u8 g, u8 b, u8 a)
{
	for (int x = leftX; x < rightX; x++)
	{
		for (int y = topY; y < bottomY; y++)
		{
			this->SetPixelResultRGBA(x, y, r, g, b, a);
		}
	}
}

bool CImageData::isInsideCircularMask(int x, int y)
{
	if (this->mask == NULL)
	{
		this->mask = new u8[this->width*this->height];
		memset(this->mask, 0, this->width*this->height);

		int spotX = this->width/2;
		int spotY = this->height/2;
		int radius = UMIN((this->height/2)-3, (this->width/2)-3);
		int radius2 = radius * radius;

		int dx, dy, d;
		//LOGD("circle spot: x=%d y=%d radius=%d", spotX, spotY, radius);

		for (int x = 0; x < this->width; x++)
		{
			for (int y = 0; y < this->height; y++)
			{
				dx = x - spotX;
				dy = y - spotY;

				d = dx * dx + dy * dy;

				if (d < radius2)
				{
					this->mask[y * this->width + x] = 0xFF;
				}
			}
		}
	}
	if (this->mask[y * this->width + x] == 0)
		return false;
	return true;
}

void CImageData::debugPrint()
{
	//LOGR("Image width=%d height=%d type=%d", width, height, type);

	if (this->type == IMG_TYPE_GRAYSCALE)
	{
		char buf[MAX_STRING_LENGTH*4];
		char buf2[MAX_STRING_LENGTH];
		for (int y = 0; y < height; y++)
		{
			sprintf(buf, "%-2.2x: ", y);
			for (int x = 0; x < width; x++)
			{
				sprintf(buf2, "%-2.2x ", this->GetPixelResultByte(x, y));
				strcat(buf, buf2);
			}
			LOGD(buf);
		}
	}
	else if (this->type == IMG_TYPE_GRAYSCALE_16BIT)
	{
		char buf[MAX_STRING_LENGTH*4];
		char buf2[MAX_STRING_LENGTH];
		for (int y = 0; y < height; y++)
		{
			sprintf(buf, "%-2.2x: ", y);
			for (int x = 0; x < width; x++)
			{
				sprintf(buf2, "%-4.4x ", this->GetPixelResultGrayscale16Bit(x, y));
				strcat(buf, buf2);
			}
			LOGD(buf);
		}

	}
}

void CImageData::CopyDataFrom(CImageData *src)
{
	this->width = src->width;
	this->height = src->height;

	if (resultData)
	{
		delete [] resultData;
	}
	
	resultData = new u8[GetDataLength()];
	
	// TODO: fixme and get the real size per pixel
	memcpy(resultData, src->resultData, GetDataLength());

	// The profile travels with the pixels it describes (see the copy ctor).
	if (src->iccProfile != NULL && src->iccProfileSize > 0)
		SetIccProfile(src->iccProfile, src->iccProfileSize);
	else
		DeallocIccProfile();
	// Copied-or-cleared, never left standing: copying FROM a source with no
	// hint must erase this one's.
	this->previewColorHint       = src->previewColorHint;
	this->previewColorHintSource = src->previewColorHintSource;
}

void CImageData::DrawImage(CImageData *drawImage, int x, int y, int width, int height, float alpha)
{
	if (this->type != IMG_TYPE_RGBA)
	{
		SYS_FatalExit("CImageData::DrawImage: image type %d not supported", this->type);
	}
	
	CImageData *image = drawImage;
	if (width != drawImage->width || height != drawImage->height)
	{
		// rescale
		image = IMG_Scale(drawImage, width, height);
	}

	uint8 *imageData = (uint8 *)this->resultData;
	uint8 *imageDataDraw = (uint8 *)image->resultData;

	for (int py = 0; py < height; py++)
	{
		if (py + y >= this->height)
			break;
		
		unsigned int offset = (y + py) * this->width * 4 + x * 4;
		unsigned int offsetDraw = py * image->width * 4;
		
		for (int px = 0; px < width; px++)
		{
			if (x + px >= this->width)
				break;
			
			uint8 r1,g1,b1,a1;
			r1 = imageData[offset    ];
			g1 = imageData[offset + 1];
			b1 = imageData[offset + 2];
			a1 = imageData[offset + 3];
			
			uint8 r2,g2,b2,a2;
			r2 = imageDataDraw[offsetDraw    ];
			g2 = imageDataDraw[offsetDraw + 1];
			b2 = imageDataDraw[offsetDraw + 2];
			a2 = imageDataDraw[offsetDraw + 3];

			float drawAlpha1 = 1.0f - (((float)a2)/255.0f * alpha);
			float drawAlpha2 = ((float)a2)/255.0f * alpha;
			
			imageData[offset    ] = (uint8)  ( (float)r1 * drawAlpha1 + (float)r2 * drawAlpha2 );
			imageData[offset + 1] = (uint8)  ( (float)g1 * drawAlpha1 + (float)g2 * drawAlpha2 );
			imageData[offset + 2] = (uint8)  ( (float)b1 * drawAlpha1 + (float)b2 * drawAlpha2 );
			imageData[offset + 3] = a1;
			
			offset += 4;
			offsetDraw += 4;
		}
	}
	
	if (image != drawImage)
		delete image;
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


void CImageData::StoreToByteBuffer(CByteBuffer *byteBuffer, int compressionType)
{
	u8 *imageBuffer = NULL;
	u32 numBytes;
	
	if (this->type == IMG_TYPE_RGB)
	{
		numBytes = width*height*3;
		imageBuffer = this->getRGBResultData();
	}
	else if (this->type == IMG_TYPE_RGBA)
	{
		numBytes = width*height*4;
		imageBuffer = this->getRGBAResultData();
	}
	else
	{
		SYS_FatalExit("CImageData::StoreToByteBuffer: image type %d not supported", this->type);
	}

	byteBuffer->PutByte('G');
	byteBuffer->PutByte(0x02);
	byteBuffer->PutI32(this->width);
	byteBuffer->PutI32(this->height);
	byteBuffer->PutU8(this->type);
	byteBuffer->PutU8(compressionType);
	
	if (compressionType == GFX_COMPRESSION_TYPE_UNCOMPRESSED)
	{
		byteBuffer->putByte(GFX_COMPRESSION_TYPE_UNCOMPRESSED);		// compression algo 0x00 = no compression
		byteBuffer->putBytes(imageBuffer, numBytes);
	}
	else if (compressionType == GFX_COMPRESSION_TYPE_ZLIB)
	{
		uLong outBufferSize = compressBound(numBytes);
		u8 *outBuffer = new u8[outBufferSize];
		
		int result = compress2(outBuffer, &outBufferSize, imageBuffer, numBytes, 9);
		
		if (result != Z_OK)
		{
			SYS_FatalExit("zlib error: %d", result);
		}
		
		u32 outSize = (u32)outBufferSize;
		
		LOGD("..original size=%d compressed=%d", numBytes, outSize);
		
		byteBuffer->PutU32(outSize);
		byteBuffer->PutBytes(outBuffer, outSize);
		
		delete [] outBuffer;
	}
	else if (compressionType == GFX_COMPRESSION_TYPE_JPEG)
	{
		CImageData *imageToStore = NULL;
		if (this->type == IMG_TYPE_RGBA)
		{
			imageToStore = new CImageData(this);
			imageToStore->ConvertToRGB();
		}
		else if (this->type == IMG_TYPE_RGB)
		{
			imageToStore = this;
		}
		else
		{
			SYS_FatalExit("CImageData::StoreToByteBuffer: image type %d not supported for compression %d", this->type, compressionType);
		}
		
		numBytes = width*height*3;
		
		CImageDataRowIter rowIter(imageToStore);
		
		unsigned char *jpegBuf = NULL;
		unsigned long outSize = 0;
		
		JPEGWriter writer;
		writer.header(this->width, this->height, 3, JPEG::COLOR_RGB);
		writer.setQuality(85);
		writer.write(&jpegBuf, &outSize, rowIter);
		
		byteBuffer->PutU32((unsigned int)outSize);
		byteBuffer->PutBytes(jpegBuf, (unsigned int)outSize);
		
		LOGD("..original size RGB=%d compressed=%d", numBytes, outSize);
		
		free(jpegBuf);

		if (imageToStore != this)
		{
			delete imageToStore;
		}
	}
	else if (compressionType == GFX_COMPRESSION_TYPE_JPEG_ZLIB)
	{
		CImageData *imageToStore = NULL;
		if (this->type == IMG_TYPE_RGBA)
		{
			imageToStore = new CImageData(this);
			imageToStore->ConvertToRGB();
		}
		else if (this->type == IMG_TYPE_RGB)
		{
			imageToStore = this;
		}
		else
		{
			SYS_FatalExit("CImageData::StoreToByteBuffer: image type %d not supported for compression %d", this->type, compressionType);
		}

		numBytes = width*height*3;

		CImageDataRowIter rowIter(imageToStore);
		
		unsigned char *jpegBuf = NULL;
		unsigned long outJpegSize = 0;
		
		// jcmarker.c
		//  M_SOI   = 0xd8		// StartOfImage marker
		//	4 bytes - ASCII "JFIF": emit_jfif_app0 (j_compress_ptr cinfo)
		
		JPEGWriter writer;
		writer.header(this->width, this->height, 3, JPEG::COLOR_RGB);
		writer.setQuality(85);
		writer.write(&jpegBuf, &outJpegSize, rowIter);
		
		
		uLong outBufferSize = compressBound(outJpegSize);
		u8 *outBuffer = new u8[outBufferSize];
		
		int result = compress2(outBuffer, &outBufferSize, jpegBuf, outJpegSize, 9);
		
		if (result != Z_OK)
		{
			SYS_FatalExit("zlib error: %d", result);
		}
		
		u32 outSize = (u32)outBufferSize;
		
		byteBuffer->putUnsignedInt((unsigned int)outSize);
		byteBuffer->putBytes(outBuffer, (unsigned int)outSize);
		
		LOGD("..original size RGB=%d compressed=%d", numBytes, outSize);
		
		free(jpegBuf);
		delete [] outBuffer;
	}
}

CImageData *CImageData::GetFromByteBuffer(CByteBuffer *byteBuffer)
{
	if (byteBuffer->GetU8() != 'G')
	{
		LOGError("CImageData::GetFromByteBuffer: magic not found");
		return NULL;
	}
	
	if (byteBuffer->GetU8() != 0x02)
	{
		LOGError("CImageData::GetFromByteBuffer: version not correct");
		return NULL;
	}
	
	int width = byteBuffer->GetI32();
	int height = byteBuffer->GetI32();
	u8 type = byteBuffer->GetU8();
	u8 compressionType = byteBuffer->GetU8();
	
	u8 *imageBuffer = NULL;
	
	int numBytes = 0;
	if (type == IMG_TYPE_RGB)
	{
		numBytes = width*height*3;
	}
	else if (type == IMG_TYPE_RGBA)
	{
		numBytes = width*height*4;
	}
	else
	{
		SYS_FatalExit("CImageData::StoreToByteBuffer: image type %d not supported", type);
	}

	if (compressionType == GFX_COMPRESSION_TYPE_UNCOMPRESSED)
	{
		imageBuffer = (u8*)malloc( numBytes );
		byteBuffer->GetBytes(imageBuffer, numBytes);
	}
	else if (compressionType == GFX_COMPRESSION_TYPE_ZLIB)
	{
		imageBuffer = (u8*)malloc( numBytes );

		u32 compressedSize = byteBuffer->GetU32();
		u8 *compressedData = (u8*)malloc( compressedSize );
		byteBuffer->GetBytes(compressedData, compressedSize);
		CSlrFileMemory *memFile = new CSlrFileMemory(compressedData, compressedSize);
		
		CSlrFileZlib *fileZlib = new CSlrFileZlib(memFile);
		fileZlib->Read(imageBuffer, numBytes);
		
		delete fileZlib;
		free(compressedData);
		
		delete memFile;
	}
	else if (compressionType == GFX_COMPRESSION_TYPE_JPEG)
	{
		u32 compressedSize = byteBuffer->GetU32();
		u8 *compressedData = (u8*)malloc( compressedSize );
		byteBuffer->GetBytes(compressedData, compressedSize);
		CSlrFileMemory *memFile = new CSlrFileMemory(compressedData, compressedSize);

		stbi_io_callbacks callbacks;
		callbacks.read = &jpegRead;
		callbacks.skip = &jpegSkip;
		callbacks.eof  = &jpegEof;
		
		int jpegWidth, jpegHeight, jpegChannels;
		imageBuffer = stbi_load_from_callbacks(&callbacks, memFile, &jpegWidth, &jpegHeight, &jpegChannels, STBI_rgb_alpha);
		
		//LOGD("failure=%s", stbi_failure_reason());
		
		LOGD("jpeg loaded: width=%d height=%d channels=%d", jpegWidth, jpegHeight, jpegChannels);
		
		free(compressedData);
		delete memFile;
	}
	else if (compressionType == GFX_COMPRESSION_TYPE_JPEG_ZLIB)
	{
		u32 compressedSize = byteBuffer->GetU32();
		u8 *compressedData = (u8*)malloc( compressedSize );
		byteBuffer->GetBytes(compressedData, compressedSize);
		CSlrFileMemory *memFile = new CSlrFileMemory(compressedData, compressedSize);
		
		
		CSlrFileZlib *fileZlib = new CSlrFileZlib(memFile);
		fileZlib->fileSize = compressedSize;
		
		
		stbi_io_callbacks callbacks;
		callbacks.read = &jpegRead;
		callbacks.skip = &jpegSkip;
		callbacks.eof  = &jpegEof;
		
		int jpegWidth, jpegHeight, jpegChannels;
		imageBuffer = stbi_load_from_callbacks(&callbacks, fileZlib, &jpegWidth, &jpegHeight, &jpegChannels, STBI_rgb_alpha);
		
		//LOGD("failure=%s", stbi_failure_reason());
		
		LOGD("jpeg-zlib loaded: width=%d height=%d channels=%d", jpegWidth, jpegHeight, jpegChannels);
		
		delete fileZlib;
		
		free(compressedData);
		delete memFile;
	}
	else SYS_FatalExit("CImageData::GetFromByteBuffer: unknown compression type %2.2x", compressionType);
	
	CImageData *imageData = new CImageData(width, height, type, imageBuffer);
	return imageData;
}


/* debug:
 *
 	//head
	LOGD("head barrier");
	MPI_Barrier(MPI_COMM_WORLD);
	//sleep(2);
	MPI_Barrier(MPI_COMM_WORLD);

	// debug 'sync'
	LOGD("send SYNC str");
	char buf[5];
	buf[0] = 'S'; buf[1] = 'Y'; buf[2] = 'N'; buf[3] = 'C';
	for (int nodeId = 1; nodeId < clsNumProcesses; nodeId++)
	{
		MPI_Send(buf, 4, MPI_CHAR, nodeId, DEF_MSG_TAG, MPI_COMM_WORLD);
	}


	// worker
	LOGD("worker barrier");
	MPI_Barrier(MPI_COMM_WORLD);
	//sleep(2);
	MPI_Barrier(MPI_COMM_WORLD);

	char buf[5];
	MPI_Recv(buf, 4, MPI_CHAR, HEAD_NODE, DEF_MSG_TAG, MPI_COMM_WORLD, &status);
	LOGD("RECEIVED SYNC STR: %c %c %c %c", buf[0], buf[1], buf[2], buf[3]);

*/

