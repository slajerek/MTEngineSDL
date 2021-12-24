/*
 *  CSlrFontProportional.h
 *
 *  Created by Marcin Skoczylas on 12-05-10.
 *  Copyright 2012 rabidus. All rights reserved.
 *
 */


#ifndef _CSLR_FONT_PROPORTIONAL_
#define _CSLR_FONT_PROPORTIONAL_

#include "SYS_Defs.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "CByteBuffer.h"
#include "CSlrFont.h"
#include <map>
#include <vector>

class CSlrString;
class CImageData;
class CSlrImage;

#define FONT_PROPORTIONAL_VERSION 2

#define FONT_PROPORTIONAL_TYPE_WITHOUT_TEXTPAGE		1
#define FONT_PROPORTIONAL_TYPE_WITH_TEXTPAGE		2

class KerningInfo
{

public:
	short first;
	short second;
	short amount;

	KerningInfo() :  first( 0 ), second( 0 ), amount( 0 )	{ }
};


class CharDescriptor
{

public:
	short x, y;
	short width;
	short height;
	short xOffset;
	short yOffset;
	short xAdvance;
	short page;

	CharDescriptor() : x( 0 ), y( 0 ), width( 0 ), height( 0 ), xOffset( 0 ), yOffset( 0 ),
		xAdvance( 0 ), page( 0 )
	{ }
};

class CSlrFontProportional : public CSlrFont
{
public:
	CSlrFontProportional();
	CSlrFontProportional(bool fromResources, const char *fontPath);
	CSlrFontProportional(bool fromResources, const char *fontPath, bool linearScale);
	CSlrFontProportional(CByteBuffer *fontData, CSlrImage *texturePage, bool linearScale);
	void LoadFontData(CByteBuffer *fontData, bool linearScale);
	void StoreFontDataToByteBuffer(CByteBuffer *byteBuffer);
	void StoreFontDataToByteBuffer(CByteBuffer *byteBuffer, u8 proportionalFontType);
	void LoadFontAndTexture(CByteBuffer *fontData);
	void StoreFontAndTexture(CImageData *image, CByteBuffer *fontData);
	virtual ~CSlrFontProportional();

	// override
	virtual void BlitText(const char *text, float posX, float posY, float posZ, float scale);
	virtual void BlitText(const char *text, float posX, float posY, float posZ, float scale, float alpha);
	virtual void BlitTextColor(const char *text, float posX, float posY, float posZ, float scale, float colorR, float colorG, float colorB, float alpha);
	virtual void BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float scale, float colorR, float colorG, float colorB, float alpha);
	virtual void BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float scale, float advance, float colorR, float colorG, float colorB, float alpha);

	virtual void BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float scale, float colorR, float colorG, float colorB, float alpha, int align);
	virtual void BlitCharColor(u16 chr, float posX, float posY, float posZ, float size, float r, float g, float b, float alpha);

	virtual void BlitText(const char *text, float posX, float posY, float posZ, float fontSizeX, float fontSizeY, float alpha);
//	virtual void BlitChar(char chr, float posX, float posY, float posZ, float size);
	virtual void BlitChar(char chr, float posX, float posY, float posZ, float scale, float alpha);
	virtual void BlitChar(u16 chr, float posX, float posY, float posZ, float scale, float alpha);
	virtual void BlitChar(u16 chr, float posX, float posY, float posZ, float size);

//	virtual void BlitText(const std::string &text, float x, float y, float z,
//				  float colorR, float colorG, float colorB, float alpha);
//
//	virtual void BlitText(UTFString *text, float x, float y, float z,
//				  float colorR, float colorG, float colorB, float alpha);
//
//	virtual void BlitText(const std::string &text, float x, float y, float z,
//				  float colorR, float colorG, float colorB, float alpha,
//				  int align, float scale);
	virtual void BlitText(const char *text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha,
				  int align, float scale);

	short lineHeight;
	short base;
	short width;
	short height;
	short pages;
	short outline;

	float texDividerX;
	float texDividerY;

	float texAdvX;
	float texAdvY;

	short fontHeight;

	u8 proportionalFontType;
	
	CSlrImage *texturePage;
	CImageData *texturePageImageData;

	std::map<int, CharDescriptor *> chars;
	std::vector<KerningInfo *> kerning;

	int GetKerningPair(int first, int second);
	float GetStringWidth(const char *string);
	float GetStringWidth(CSlrString *string);
	float GetStringWidth(CSlrString *string, float advance);

	virtual void GetTextSize(const char *text, float scale, u16 *width, u16 *height);
	virtual void GetTextSize(const char *text, float scale, float *width, float *height);
	virtual void GetTextSize(CSlrString *text, float scale, float *width, float *height);
	virtual void GetTextSize(CSlrString *text, float scale, float advance, float *width, float *height);
	virtual float GetTextWidth(CSlrString *text, float scale);
	virtual float GetTextWidth(const char *text, float scale);
	virtual float GetCharWidth(char ch, float scale);
	virtual float GetCharHeight(char ch, float scale);

	CharDescriptor *GetCharDescriptor(char ch);
	CharDescriptor *GetCharDescriptorInt(u16 ch);

	bool forceCapitals;
	bool releaseImage;
	
	virtual float GetLineHeight();

	virtual void ResourcesPrepare();

	virtual void ResourceSetPriority(u8 newPriority);
};

#endif
//_CSLR_FONT_PROPORTIONAL_

