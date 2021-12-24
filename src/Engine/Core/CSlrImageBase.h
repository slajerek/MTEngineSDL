#ifndef _CSLRIMAGEBASE_H_
#define _CSLRIMAGEBASE_H_

#include "CSlrResourceBase.h"

#define IMAGE_TYPE_UNKNOWN	0x00
#define IMAGE_TYPE_BITMAP	0x01
#define IMAGE_TYPE_ATLAS	0x02
#define IMAGE_TYPE_FRAGMENT	0x03
#define IMAGE_TYPE_CONTAINER	0x04

#define IMAGE_LOAD_ERROR_NONE           0x00
#define IMAGE_LOAD_ERROR_NOT_LOADED     0x01
#define IMAGE_LOAD_ERROR_NOT_FOUND      0x02
#define IMAGE_LOAD_ERROR_NOT_IMAGE      0x03


class CSlrImageBase : public CSlrResourceBase
{
public:
	CSlrImageBase();
	virtual ~CSlrImageBase();
	
	u8 imageType;
	
	float height;
	float width;
	float heightD2;	// divided by 2
	float widthD2;
	float heightM2;	// multiplied by 2
	float widthM2;

	virtual void Render(float posZ);
	virtual void Render(float posZ, float alpha);
	virtual void RenderMixColor(float posZ, float alpha, float mixColorR, float mixColorG, float mixColorB);
	
	virtual void GetPixel(u16 x, u16 y, u8 *r, u8 *g, u8 *b);

	virtual void Render(float destX, float destY, float z, float sizeX, float sizeY);
	virtual void Render(float destX, float destY, float z, float size,
			  float texStartX, float texStartY,
			  float texEndX, float texEndY);

	virtual void RenderAlpha(float destX, float destY, float z, float alpha);
	virtual void RenderAlpha(float destX, float destY, float z, float sizeX,
			float sizeY, float alpha);
	virtual void RenderAlpha(float destX, float destY, float z, float size,
				   float texStartX, float texStartY,
				   float texEndX, float texEndY, float alpha);
	virtual void RenderAlpha(float destX, float destY, float z, float sizeX, float sizeY,
				   float texStartX, float texStartY,
				   float texEndX, float texEndY,
				   float alpha);
	virtual void RenderAlpha_aaaa(float destX, float destY, float z, float sizeX, float sizeY,
				   float texStartX, float texStartY,
				   float texEndX, float texEndY,
				   float alpha);
	virtual void RenderAlphaColor(float destX, float destY, float z, float sizeX, float sizeY,
				   float texStartX, float texStartY,
				   float texEndX, float texEndY,
				   float colorR, float colorG, float colorB, float alpha);

	virtual void RenderAlphaMixColor(float destX, float destY, float z, float sizeX,
									 float sizeY, float mixColorR, float mixColorG, float mixColorB, float alpha);
	
	virtual void RenderPolygonAlpha(float alpha, float *verts, float *texs, float *norms, u32 numVertices);
	virtual void RenderPolygonMixColor(float mixColorR, float mixColorG, float mixColorB, float mixColorA, float *verts, float *texs, float *norms, u32 numVertices);
	virtual void RenderFlipHorizontal(float destX, float destY, float z, float sizeX, float sizeY);

	// should preload resource and set resource size
	virtual bool ResourcePreload(const char *fileName, bool fromResources);

	// resource should free memory, @returns memory freed
	virtual u32 ResourceDeactivate(bool async);

	// resource should load itself, @returns memory allocated
	virtual u32 ResourceActivate(bool async);

	// get size of resource in bytes
	virtual u32 ResourceGetLoadingSize();
	virtual u32 ResourceGetIdleSize();
};

#endif
//_CSLRIMAGEBASE_H_
