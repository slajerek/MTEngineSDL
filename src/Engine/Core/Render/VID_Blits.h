#ifndef _VID_BLITS_H_
#define _VID_BLITS_H_

#include "CSlrImage.h"

class CGLLineStrip
{
public:
	float *lineStripData;
	int numberOfPoints;
	int dataLen;
	
	CGLLineStrip()
	{
		this->lineStripData = NULL;
		this->numberOfPoints = 0;
		this->dataLen = 0;
	}
	
	CGLLineStrip(float *lineStripData, int length, int dataLen)
	{
		this->lineStripData = lineStripData;
		this->numberOfPoints = length;
		this->dataLen = dataLen;
	}
	
	void Clear()
	{
		if (lineStripData)
		{
			delete [] lineStripData;
			lineStripData = NULL;
		}
		this->numberOfPoints = 0;
		this->dataLen = 0;
	}
	
	void Update(int dataLen)
	{
		if (this->dataLen != dataLen)
		{
			if (lineStripData)
				delete []lineStripData;
			
			this->lineStripData = new float[dataLen];
		}
	}
	
	~CGLLineStrip()
	{
		if (lineStripData)
			delete [] this->lineStripData;
	}
};

void Blit(CSlrImage *image, float destX, float destY, float z);
void BlitAlpha(CSlrImage *image, float destX, float destY, float z, float alpha);
void BlitSize(CSlrImage *image, float destX, float destY, float z, float size);
void Blit(CSlrImage *image, float destX, float destY, float z, float size, 
		  float texStartX, float texStartY,
		  float texEndX, float texEndY);
void BlitAlpha(CSlrImage *image, float destX, float destY, float z, float size, 
			   float texStartX, float texStartY,
			   float texEndX, float texEndY, float alpha);

void Blit(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY);
void BlitFlipVertical(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY);
void BlitFlipHorizontal(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY);
#define BlitFlippedVertical BlitFlipVertical
#define BlitFlippedHorizontal BlitFlipHorizontal
void BlitCheckAtl(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY);
void BlitAlpha(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY, float alpha);
void BlitAtl(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY);
void BlitAtlFlipHorizontal(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY);
void BlitAtlAlpha(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY, float alpha);
void BlitCheckAtlAlpha(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY, float alpha);
void BlitCheckAtlFlipHorizontal(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY);

void Blit(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
		  float texStartX, float texStartY,
		  float texEndX, float texEndY);

// Maps a NORMALIZED display-space UV (0..1 across the DISPLAYED image, the way
// the user sees it) back to a normalized texture-space UV, for a display
// rotated q CLOCKWISE quarter turns relative to the texture. Convention check:
// q=1 sends display top-right (1,0) to texture (0,0) -- the texture's top-left
// corner appears at the displayed top-right, i.e. the image turned clockwise.
// NORMALIZED ONLY -- for real texel UVs use VID_RotateQuarterUVInRect below.
inline void VID_RotateQuarterUV(int q, float du, float dv, float &outU, float &outV)
{
	switch (q & 3)
	{
		default: outU = du;        outV = dv;        break;
		case 1:  outU = dv;        outV = 1.0f - du; break;   // 90 CW
		case 2:  outU = 1.0f - du; outV = 1.0f - dv; break;   // 180
		case 3:  outU = 1.0f - dv; outV = du;        break;   // 270 CW (= 90 CCW)
	}
}

// Rotation for UVs living inside the image's ACTUAL texel rect
// [sx0..sx1]x[sy0..sy1] (CSlrImage::defaultTex* -- a POT-padded SUB-rect of
// [0,1] for images uploaded via LoadImage). Normalizes (u,v) to display
// fractions of that rect, rotates, and maps back into the same rect, so
// rotated sampling can never leave the image and hit padding. Degenerate
// rects (zero extent) clamp to the rect origin instead of dividing by zero.
inline void VID_RotateQuarterUVInRect(int q, float u, float v,
									  float sx0, float sy0, float sx1, float sy1,
									  float &outU, float &outV)
{
	float du = (sx1 > sx0) ? (u - sx0) / (sx1 - sx0) : 0.0f;
	float dv = (sy1 > sy0) ? (v - sy0) / (sy1 - sy0) : 0.0f;
	float nu, nv;
	VID_RotateQuarterUV(q, du, dv, nu, nv);
	outU = sx0 + nu * (sx1 - sx0);
	outV = sy0 + nv * (sy1 - sy0);
}

// Blit rotated by CLOCKWISE quarter turns. quarterTurns==0 delegates to the
// UV-rect Blit above, so the default path is byte-identical to today. The
// incoming texStart/texEnd are DISPLAY-space UVs inside image->defaultTex*
// (that is what the moving pane computes once pane space is display-oriented).
void BlitQuarterTurns(CSlrImage *image, float destX, float destY, float z,
					  float sizeX, float sizeY,
					  float texStartX, float texStartY, float texEndX, float texEndY,
					  int quarterTurns);

void BlitFlipVertical(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
		  float texStartX, float texStartY, float texEndX, float texEndY);

void BlitInImGuiWindow(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
					   float texStartX, float texStartY,
					   float texEndX, float texEndY, float colorR, float colorG, float colorB, float alpha);
void BlitInImGuiWindow(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
					   float texStartX, float texStartY,
					   float texEndX, float texEndY);
void BlitInImGuiWindow(CSlrImage *image, float posX, float posY);
void BlitInImGuiWindow(CSlrImage *image, float posX, float posY, float alpha);
void BlitInImGuiWindow(CSlrImage *image);


void BlitAlpha(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY, 
			   float texStartX, float texStartY,
			   float texEndX, float texEndY, 
			   float alpha);

void BlitMixColor(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY, float alpha,
				  float mixColorR, float mixColorG, float mixColorB);

void BlitAlpha_aaaa(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY, 
			   float texStartX, float texStartY,
			   float texEndX, float texEndY, 
			   float alpha);

void BlitAlphaColor(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
					float texStartX, float texStartY,
					float texEndX, float texEndY,
					float colorR, float colorG, float colorB, float alpha);

void BlitTriangleAlpha(CSlrImage *image, float z, float alpha,
					   float vert1x, float vert1y, float tex1x, float tex1y,
					   float vert2x, float vert2y, float tex2x, float tex2y,
					   float vert3x, float vert3y, float tex3x, float tex3y);

void BlitPolygonAlpha(CSlrImage *image, float alpha, float *verts, float *texs, float *norms, unsigned int numVertices);
void BlitPolygonMixColor(CSlrImage *image, float mixColorR, float mixColorG, float mixColorB, float mixColorA, float *verts, float *texs, float *norms, unsigned int numVertices);

void BlitFilledRectangle(float destX, float destY, float z, float sizeX, float sizeY,
						 float colorR, float colorG, float colorB, float alpha);

void BlitGradientRectangle(float destX, float destY, float z, float sizeX, float sizeY,
						   float colorR1, float colorG1, float colorB1, float colorA1,
						   float colorR2, float colorG2, float colorB2, float colorA2,
						   float colorR3, float colorG3, float colorB3, float colorA3,
						   float colorR4, float colorG4, float colorB4, float colorA4);

void BlitRectangle(float destX, float destY, float z, float sizeX, float sizeY,
				   float colorR, float colorG, float colorB, float alpha);

void BlitLine(float startX, float startY, float endX, float endY, float posZ,
			  float colorR, float colorG, float colorB, float alpha);
void BlitLine(float startX, float startY, float endX, float endY, float posZ,
			  float colorR, float colorG, float colorB, float alpha, float width);


void BlitFilledRectangle(float destX, float destY, float z, float sizeX, float sizeY, 
						 float colorR, float colorG, float colorB, float alpha);
void BlitFilledRectangleOnForeground(float destX, float destY, float z, float sizeX, float sizeY,
									 float colorR, float colorG, float colorB, float alpha);

void BlitRectangle(float destX, float destY, float z, float sizeX, float sizeY, 
				   float colorR, float colorG, float colorB, float alpha);

void BlitRectangle(float destX, float destY, float z, float sizeX, float sizeY,
				   float colorR, float colorG, float colorB, float alpha, float lineWidth);

void BlitLine(float startX, float startY, float endX, float endY, float posZ,
			  float colorR, float colorG, float colorB, float alpha);

void BlitCircle(float centerX, float centerY, float radius, float colorR, float colorG, float colorB, float colorA);
void BlitFilledCircle(float centerX, float centerY, float radius, float colorR, float colorG, float colorB, float colorA);

void VID_EnableSolidsOnly();
void VID_DisableSolidsOnly();

void VID_DisableTextures();
void VID_EnableTextures();

//void BlitFilledPolygon(b2PolygonShape *polygon, float posZ,
//					   float colorR, float colorG, float colorB, float alpha);

void GenerateLineStrip(CGLLineStrip *lineStrip, signed short *data, int start, int count, float posX, float posY, float posZ, float sizeX, float sizeY);
void GenerateLineStripFromFft(CGLLineStrip *lineStrip, float *data, int start, int count, float multiplier, float posX, float posY, float posZ, float sizeX, float sizeY);
void GenerateLineStripFromFloat(CGLLineStrip *lineStrip, float *data, int start, int count, float posX, float posY, float posZ, float sizeX, float sizeY);
void GenerateLineStripFromCircularBuffer(CGLLineStrip *lineStrip, signed short *data, int length, int pos, float posX, float posY, float posZ, float sizeX, float sizeY);
void BlitLineStrip(CGLLineStrip *glLineStrip, float colorR, float colorG, float colorB, float alpha);
void BlitPlus(float posX, float posY, float posZ, float r, float g, float b, float alpha);
void BlitPlus(float posX, float posY, float posZ, float sizeX, float sizeY, float r, float g, float b, float alpha);

void PushMatrix2D();
void PopMatrix2D();
void Translate2D(float posX, float posY, float posZ);
void Rotate2D(float angle);
void Scale2D(float scaleX, float scaleY, float scaleZ);

void BlitRotatedImage(CSlrImage *image, float pX, float pY, float pZ, float rotationAngle, float alpha);

void glEnable2D();
void glDisable2D();

#endif
//_VID_BLITS_H_

