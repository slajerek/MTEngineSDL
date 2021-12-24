#include "CSlrImageBase.h"
#include "SYS_Main.h"

CSlrImageBase::CSlrImageBase()
{
	resourceType = RESOURCE_TYPE_IMAGE;
}

void CSlrImageBase::GetPixel(u16 x, u16 y, u8 *r, u8 *g, u8 *b)
{
	SYS_FatalExit("CSlrImageBase::GetPixel");	
}

void CSlrImageBase::Render(float posZ)
{
	SYS_FatalExit("CSlrImageBase::Render");
}

void CSlrImageBase::Render(float posZ, float alpha)
{
	SYS_FatalExit("CSlrImageBase::RenderA");
}

void CSlrImageBase::RenderMixColor(float posZ, float alpha, float mixColorR, float mixColorG, float mixColorB)
{
	SYS_FatalExit("CSlrImageBase::RenderMixColor");
}

void CSlrImageBase::Render(float destX, float destY, float z, float sizeX, float sizeY)
{
	SYS_FatalExit("CSlrImageBase::Render");
}

void CSlrImageBase::Render(float destX, float destY, float z, float size,
		  float texStartX, float texStartY,
		  float texEndX, float texEndY)
{
	SYS_FatalExit("CSlrImageBase::Render");
}

void CSlrImageBase::RenderAlpha(float destX, float destY, float z, float alpha)
{
	SYS_FatalExit("CSlrImageBase::Render");
}

void CSlrImageBase::RenderAlpha(float destX, float destY, float z, float sizeX,
		float sizeY, float alpha)
{
	SYS_FatalExit("CSlrImageBase::Render");
}

void CSlrImageBase::RenderAlpha(float destX, float destY, float z, float size,
			   float texStartX, float texStartY,
			   float texEndX, float texEndY, float alpha)
{
	SYS_FatalExit("CSlrImageBase::Render");
}

void CSlrImageBase::RenderAlpha(float destX, float destY, float z, float sizeX, float sizeY,
			   float texStartX, float texStartY,
			   float texEndX, float texEndY,
			   float alpha)
{
	SYS_FatalExit("CSlrImageBase::Render");
}

void CSlrImageBase::RenderAlpha_aaaa(float destX, float destY, float z, float sizeX, float sizeY,
				   float texStartX, float texStartY,
				   float texEndX, float texEndY,
				   float alpha)
{
	SYS_FatalExit("CSlrImageBase::RenderAlpha_aaaa");
}

void CSlrImageBase::RenderAlphaColor(float destX, float destY, float z, float sizeX, float sizeY,
			   float texStartX, float texStartY,
			   float texEndX, float texEndY,
			   float colorR, float colorG, float colorB, float alpha)
{
	SYS_FatalExit("CSlrImageBase::RenderAlphaColor");
}

void CSlrImageBase::RenderAlphaMixColor(float destX, float destY, float z, float sizeX,
								 float sizeY, float mixColorR, float mixColorG, float mixColorB, float alpha)
{
	SYS_FatalExit("CSlrImageBase::RenderAlphaMixColor");
}

void CSlrImageBase::RenderPolygonAlpha(float alpha, float *verts, float *texs, float *norms, unsigned int numVertices)
{
	SYS_FatalExit("CSlrImageBase::RenderPolygonAlpha");
}

void CSlrImageBase::RenderPolygonMixColor(float mixColorR, float mixColorG, float mixColorB, float mixColorA, float *verts, float *texs, float *norms, unsigned int numVertices)
{
	SYS_FatalExit("CSlrImageBase::RenderPolygonMixColor");
}


void CSlrImageBase::RenderFlipHorizontal(float destX, float destY, float z, float sizeX, float sizeY)
{
	SYS_FatalExit("CSlrImageBase::RenderFlipHorizontal");
}

bool CSlrImageBase::ResourcePreload(const char *fileName, bool fromResources)
{
	SYS_FatalExit("CSlrImageBase::ResourcePreload: %s", fileName);
	return 0;	
}

// resource should free memory, @returns memory freed
u32 CSlrImageBase::ResourceDeactivate(bool async)
{
	SYS_FatalExit("CSlrImageBase::ResourceDeactivate");
	return 0;
}

// resource should load itself, @returns memory allocated
u32 CSlrImageBase::ResourceActivate(bool async)
{
	SYS_FatalExit("CSlrImageBase::ResourceActivate");
	return 0;
}

// get size of resource in bytes
u32 CSlrImageBase::ResourceGetLoadingSize()
{
	SYS_FatalExit("CSlrImageBase::ResourceGetLoadingSize");
	return 0;
}

u32 CSlrImageBase::ResourceGetIdleSize()
{
	SYS_FatalExit("CSlrImageBase::ResourceGetIdleSize");
	return 0;
}

CSlrImageBase::~CSlrImageBase()
{

}

