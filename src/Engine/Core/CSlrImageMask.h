#ifndef _CSLRIMAGEMASK2_H_
#define _CSLRIMAGEMASK2_H_

#include "SYS_Defs.h"

#define IMG_MASK_PIXEL_ON	0xFF
#define IMG_MASK_PIXEL_OFF	0x00

class CSlrImageMask
{
public:
	CSlrImageMask(const char *fileName, bool fromResources, bool doThreshold);
	~CSlrImageMask();

	u32 height;
	u32 width;

	u32 dataWidth;
	u32 dataHeight;

	u8 *maskData;
	float gfxScale;
	u8 GetMaskValue(int pX, int pY);
	
	void GetImageRealPos(int pX, int pY, int *imageX, int *imageY);
};

#endif
//_CSLRIMAGEMASK2_H_
