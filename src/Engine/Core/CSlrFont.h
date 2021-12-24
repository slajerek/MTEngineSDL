/*
 *  CSlrFont.h
 *  MusicTracker
 *
 *  Created by Marcin Skoczylas on 11-04-04.
 *  Copyright 2011 rabidus. All rights reserved.
 *
 */


#ifndef _CSLR_FONT_
#define _CSLR_FONT_

#include "SYS_Defs.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "CSlrResourceBase.h"

#define FONT_TYPE_UNKNOWN 0
#define FONT_TYPE_BITMAP 1
#define FONT_TYPE_SYSTEM 2
#define FONT_TYPE_PROPORTIONAL 3

#define FONT_ALIGN_LEFT -1
#define FONT_ALIGN_CENTER 0
#define FONT_ALIGN_RIGHT 1

class CSlrString;

// base abstract class
class CSlrFont : public CSlrResourceBase
{
public:
	CSlrFont();
	CSlrFont(const char *name);
	virtual ~CSlrFont();

	char *name;

	u8 fontType;
	
	float scaleAdjust;
	
	// override
	virtual void BlitText(const char *text, float posX, float posY, float posZ, float size);
	virtual void BlitText(const char *text, float posX, float posY, float posZ, float size, float alpha);
	virtual void BlitTextColor(const char *text, float posX, float posY, float posZ, float size, float colorR, float colorG, float colorB, float alpha);
	virtual void BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float size, float colorR, float colorG, float colorB, float alpha);
	virtual void BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float size, float advance, float colorR, float colorG, float colorB, float alpha);
	virtual void BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float scale, float colorR, float colorG, float colorB, float alpha, int align);
	virtual void BlitText(const char *text, float posX, float posY, float posZ, float fontSizeX, float fontSizeY, float alpha);
	virtual void BlitChar(const char chr, float posX, float posY, float posZ, float size);
	virtual void BlitChar(const char chr, float posX, float posY, float posZ, float size, float alpha);
	virtual void BlitChar(u16 chr, float posX, float posY, float posZ, float scale, float alpha);
	virtual void BlitChar(u16 chr, float posX, float posY, float posZ, float size);
	virtual void BlitCharColor(u16 chr, float posX, float posY, float posZ, float size, float r, float g, float b, float alpha);

	virtual void BlitText(const std::string &text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha);

	virtual void BlitText(const char *text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha);

	virtual void BlitText(const std::string &text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha,
				  int align, float scale);

	virtual void BlitText(const char *text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha,
				  int align, float scale);

	virtual void BlitText(const std::string &text, float x, float y, float z,
				  float alpha, int align, float scale);

	virtual void GetTextSize(const char *text, float scale, u16 *width, u16 *height);
	virtual void GetTextSize(const char *text, float scale, float *width, float *height);
	virtual void GetTextSize(CSlrString *text, float scale, float *width, float *height);
	virtual void GetTextSize(CSlrString *text, float scale, float advance, float *width, float *height);
	virtual float GetTextWidth(const char *text, float scale);
	virtual float GetTextWidth(CSlrString *text, float scale);
	virtual float GetCharWidth(char ch, float scale);
	virtual float GetCharHeight(char ch, float scale);
	
	virtual float GetLineHeight();
	
	virtual void ResourcesPrepare();
	virtual const char *ResourceGetTypeName();
};

#endif
//_CSLR_FONT_

