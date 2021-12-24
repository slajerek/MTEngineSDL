/*
 *  CSlrFontBitmap.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-23.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef __VID_CSlrFontBitmap_H__
#define __VID_CSlrFontBitmap_H__

#include "SYS_Defs.h"
#include "CSlrFont.h"
//#include "CSlrImage.h"


class CSlrImage;

class CSlrFontBitmap : public CSlrFont
{
public:
	CSlrFontBitmap(const char *name, CSlrImage *fntImageData,
			 float fntWidth, float fntHeight, float fntPitchX, float fntPitchY);

	~CSlrFontBitmap();

	void GetFontChar(unsigned char c, u8 *x, u8 *y);
	void GetFontCharNoInvert(unsigned char c, u8 *x, u8 *y);
	void GetFontChar(bool invert, unsigned char c, u8 *x, u8 *y);
//	void BlitText(char *text, float posX, float posY, float posZ);
	void BlitText(const char *text, float posX, float posY, float posZ, float size);
	void BlitText(bool invert, const char *text, float posX, float posY, float posZ, float size);
	void BlitText(const char *text, float posX, float posY, float posZ, float size, float alpha);
	void BlitText(const char *text, float posX, float posY, float posZ, float fontSizeX, float fontSizeY, float alpha);
	void BlitText(const char *text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha);
	void BlitText(const std::string &text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha,
				  int align, float scale);

	void BlitText(const char *text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha,
				  int align, float scale);


	virtual void BlitTextColor(const char *text, float posX, float posY, float posZ, float size, float colorR, float colorG, float colorB, float alpha);
	virtual void BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float scale, float colorR, float colorG, float colorB, float alpha);

	void BlitChar(char chr, float posX, float posY, float posZ, float size);
	void BlitChar(char chr, float posX, float posY, float posZ, float size, float alpha);

	virtual float GetTextWidth(const char *text, float scale);

	float width, height, pitchX, pitchY;
	CSlrImage *image;
	
	virtual void ResourcesPrepare();
	
};


#endif

