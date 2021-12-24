#ifndef _SLRIMAGETEXTURE_H_
#define _SLRIMAGETEXTURE_H_

#include "CSlrImageBase.h"

class CSlrImageTexture : public CSlrImageBase
{
public:
	volatile bool isActive;
	volatile bool isBound;

	CSlrImageTexture();
	
	virtual void DelayedLoadImage(const char *fileName, bool fromResources);
	virtual void BindImage();
	virtual void FreeLoadImage();
	virtual void Deallocate();

	virtual bool CheckIfActive();
	virtual void Render(float posZ);
	virtual void Render(float posZ, float alpha);
	virtual void RenderMixColor(float posZ, float alpha, float mixColorR, float mixColorG, float mixColorB);

	virtual void Render(float destX, float destY, float z, float sizeX, float sizeY);
	virtual void Render(float destX, float destY, float z, float size,
			  float texStartX, float texStartY,
			  float texEndX, float texEndY);
	virtual void Render(float destX, float destY, float z,
						float sizeX, float sizeY,
						float texStartX, float texStartY,
						float texEndX, float texEndY);
	
	
	virtual void RenderAlpha(float destX, float destY, float z, float alpha);
	virtual void RenderAlpha(float destX, float destY, float z, float sizeX,
			float sizeY, float alpha);
	virtual void RenderAlpha(float destX, float destY, float z, float size,
				   float texStartX, float texStartY,
				   float texEndX, float texEndY, float alpha);
	virtual void RenderAlpha(float destX, float destY, float z,
				   float sizeX, float sizeY,
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

	///////// resource base
	// resource should free memory, @returns memory freed
	virtual u32 ResourceDeactivate(bool async);

	// resource should load itself, @returns memory allocated
	virtual u32 ResourceActivate(bool async);
	
	virtual const char *ResourceGetTypeName();
};

#endif
//
