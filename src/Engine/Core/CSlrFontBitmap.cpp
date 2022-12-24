/*
 *  CSlrFontBitmap.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-23.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CSlrFontBitmap.h"
#include "DBG_Log.h"
#include "CSlrString.h"
#include "CSlrImage.h"

// handle fixed fonts

CSlrFontBitmap::CSlrFontBitmap(const char *name, CSlrImage *fntImageData,
				   float fntWidth, float fntHeight, float fntPitchX, float fntPitchY)
:CSlrFont(name)
{
//	LOGT("CSlrFontBitmap()\n");
	this->image = fntImageData;
	this->width = fntWidth;
	this->height = fntHeight;
	this->pitchX = fntPitchX;
	this->pitchY = fntPitchY;
	this->fontType = FONT_TYPE_BITMAP;
}

CSlrFontBitmap::~CSlrFontBitmap()
{
	if (name != NULL)
		free(name);
	name = NULL;
	
//	LOGT("~CSlrFontBitmap()\n");
}

void CSlrFontBitmap::ResourcesPrepare()
{
	this->image->ResourcePrepare();
}

void CSlrFontBitmap::GetFontChar(bool invert, unsigned char c, u8 *x, u8 *y)
{
	GetFontChar(c, x, y);
	
	if (invert)
	{
		*y += 5;
	}
}

void CSlrFontBitmap::GetFontChar(unsigned char c, u8 *x, u8 *y)
{
	if (c > 0x80)
	{
		c -= 0x80;
		GetFontCharNoInvert(c, x, y);
		*y += 5;
	}
	else
	{
		GetFontCharNoInvert(c, x, y);
	}
}

void CSlrFontBitmap::GetFontCharNoInvert(unsigned char c, u8 *x, u8 *y)
{
	if (c >= 'A' && c <= 'Z')
	{
		*y = 0;
		*x = (u8)(c - 'A');
		return;
	}
	if (c >= 'a' && c <= 'z')
	{
		*y = 1;
		*x = (u8)(c - 'a');
		return;
	}
	if (c >= '0' && c <= '9')
	{
		*y = 2;
		*x = (u8)(c - '0');
		return;
	}
	
	switch (c)
	{
		case '(': *y = 3; *x = 0; break;
		case '{': *y = 3; *x = 1; break;
		case '[': *y = 3; *x = 2; break;
		case '<': *y = 3; *x = 3; break;
		case '!': *y = 3; *x = 4; break;
		case '@': *y = 3; *x = 5; break;
		case '#': *y = 3; *x = 6; break;
		case '$': *y = 3; *x = 7; break;
		case '%': *y = 3; *x = 8; break;
		case '^': *y = 3; *x = 9; break;
		case '&': *y = 3; *x = 10; break;
		case '*': *y = 3; *x = 11; break;
		case '?': *y = 3; *x = 12; break;
		case '_': *y = 3; *x = 13; break;
		case '+': *y = 3; *x = 14; break;
		case '-': *y = 3; *x = 15; break;
		case '=': *y = 3; *x = 16; break;
		case ':': *y = 2; *x = 13; break; 
		case '.': *y = 3; *x = 18; break; 
		case ';': *y = 3; *x = 17; break; 
		case ',': *y = 2; *x = 11; break; 
		case '"': *y = 3; *x = 19; break;
		case '/': *y = 3; *x = 20; break;
		case '\\': *y = 2; *x = 12; break; 
		case '\'': *y = 3; *x = 21; break;
		case '>': *y = 3; *x = 22; break;
		case ']': *y = 3; *x = 23; break;
		case '}': *y = 3; *x = 24; break;
		case ')': *y = 3; *x = 25; break;
		/*case 'ą': *y = 4; *x = 0; break;
		case 'ć': *y = 4; *x = 1; break;
		case 'ę': *y = 4; *x = 2; break;
		case 'ł': *y = 4; *x = 3; break;
		case 'ń': *y = 4; *x = 4; break;
		case 'ó': *y = 4; *x = 5; break;
		case 'ś': *y = 4; *x = 6; break;
		case 'ż': *y = 4; *x = 7; break;
		case 'ź': *y = 4; *x = 8; break;
		case 'Ą': *y = 4; *x = 9; break;
		case 'Ć': *y = 4; *x = 10; break;
		case 'Ę': *y = 4; *x = 11; break;
		case 'Ł': *y = 4; *x = 12; break;
		case 'Ń': *y = 4; *x = 13; break;
		case 'Ó': *y = 4; *x = 14; break;
		case 'Ś': *y = 4; *x = 15; break;
		case 'Ż': *y = 4; *x = 16; break;
		case 'Ź': *y = 4; *x = 17; break;*/
		case ' ': *y = 2; *x = 10; break;
		case 255: *y = 4; *x = 25; break;	// cursor
		default: *x = 0; *y = 0; break;
	}
}

void CSlrFontBitmap::BlitText(const char *text, float posX, float posY, float posZ, float size)
{
	const char *pText;
	u8 lX, lY;
	float letterX, letterY;
	
	if (!text || text[0] == '\0')
		return;
	
	for (pText = text ; *pText != '\0'; pText++)
	{
		//GetFontChar(*pText, &lY, &lX);	// [<- opposite]
		
		unsigned char c = (unsigned char)*pText;
		GetFontChar(c, &lX, &lY);		// [<- normal]
		
		// opposite
		//lY = 25 - lY;
		
		letterX = (lX * pitchX);
		letterY = (lY * pitchY);
				
		//RenderKeyClip(imageData, posX, posY, letterX, letterY, width, height);
		image->Render(posX, posY, posZ, size, letterX, letterY, letterX+width, letterY+height);
		
		//posY -= width;	// [<- opposite]
		posX = (posX + size);		// [<- normal]
		
	}
}

void CSlrFontBitmap::BlitTextColor(CSlrString *text, float posX, float posY, float posZ, float scale, float colorR, float colorG, float colorB, float alpha)
{
	u8 lX, lY;
	float letterX, letterY;
	
	int l = text->GetLength();
	
	for (int i = 0; i < l; i++)
	{
		unsigned char c = (unsigned char)text->GetChar(i);
		GetFontChar(c, &lX, &lY);		// [<- normal]
		
		// opposite
		//lY = 25 - lY;
		
		letterX = (lX * pitchX);
		letterY = (lY * pitchY);
		
		//RenderKeyClip(imageData, posX, posY, letterX, letterY, width, height);
		image->RenderAlphaColor(posX, posY, posZ, scale, scale,
									letterX, letterY, letterX+width, letterY+height,
									colorR, colorG, colorB, alpha);
		
		//posY -= width;	// [<- opposite]
		posX = (posX + scale);		// [<- normal]
		
	}
}

void CSlrFontBitmap::BlitText(bool invert, const char *text, float posX, float posY, float posZ, float size)
{
	const char *pText;
	u8 lX, lY;
	float letterX, letterY;
	
	if (!text || text[0] == '\0')
		return;
	
	for (pText = text ; *pText != '\0'; pText++)
	{
		//GetFontChar(*pText, &lY, &lX);	// [<- opposite]
		GetFontChar(invert, *pText, &lX, &lY);		// [<- normal]
		
		// opposite
		//lY = 25 - lY;
		
		letterX = (lX * pitchX);
		letterY = (lY * pitchY);
		
		//RenderKeyClip(imageData, posX, posY, letterX, letterY, width, height);
		image->Render(posX, posY, posZ, size, letterX, letterY, letterX+width, letterY+height);
		
		//posY -= width;	// [<- opposite]
		posX = (posX + size);		// [<- normal]
		
	}
}


// with alpha blending
void CSlrFontBitmap::BlitText(const char *text, float posX, float posY, float posZ, float size, float alpha)
{
	const char *pText;
	u8 lX, lY;
	float letterX, letterY;
	
	if (!text || text[0] == '\0')
		return;
	
	for (pText = text ; *pText != '\0'; pText++)
	{
		//GetFontChar(*pText, &lY, &lX);	// [<- opposite]
		GetFontChar(*pText, &lX, &lY);		// [<- normal]
		
		// opposite
		//lY = 25 - lY;
		
		letterX = (lX * pitchX);
		letterY = (lY * pitchY);
		
		//ReKeyClip(imageData, posX, posY, alpha, letterX, letterY, width, height);
		image->RenderAlpha(posX, posY, posZ, size, letterX, letterY, letterX+width, letterY+height, alpha);
		
		//posY -= width;	// [<- opposite]		
		posX = (posX + size);		// [<- normal]
	}
}

void CSlrFontBitmap::BlitTextColor(const char *text, float posX, float posY, float posZ, float size, float colorR, float colorG, float colorB, float alpha)
{
	const char *pText;
	u8 lX, lY;
	float letterX, letterY;
	
	if (!text || text[0] == '\0')
		return;
	
	for (pText = text ; *pText != '\0'; pText++)
	{
		//GetFontChar(*pText, &lY, &lX);	// [<- opposite]
		GetFontChar(*pText, &lX, &lY);		// [<- normal]
		
		// opposite
		//lY = 25 - lY;
		
		letterX = (lX * pitchX);
		letterY = (lY * pitchY);
		
		//ReKeyClip(imageData, posX, posY, alpha, letterX, letterY, width, height);
		image->RenderAlphaColor(posX, posY, posZ, size, size,
									letterX, letterY, letterX+width, letterY+height,
									colorR, colorG, colorB, alpha);
		
		//posY -= width;	// [<- opposite]
		posX = (posX + size);		// [<- normal]
	}	
}


void CSlrFontBitmap::BlitText(const char *text, float posX, float posY, float posZ, float fontSizeX, float fontSizeY, float alpha)
{
	const char *pText;
	u8 lX, lY;
	float letterX, letterY;
	
	if (!text || text[0] == '\0')
		return;
	
	for (pText = text ; *pText != '\0'; pText++)
	{
		//GetFontChar(*pText, &lY, &lX);	// [<- opposite]
		GetFontChar(*pText, &lX, &lY);		// [<- normal]
		
		// opposite
		//lY = 25 - lY;
		
		letterX = (lX * pitchX);
		letterY = (lY * pitchY);
		
		//RenderKeyClip(imageData, posX, posY, alpha, letterX, letterY, width, height);
		image->RenderAlpha(posX, posY, posZ, fontSizeX, fontSizeY, letterX, letterY, letterX+width, letterY+height, alpha);
		
		//posY -= width;	// [<- opposite]		
		posX = (posX + fontSizeX);		// [<- normal]
	}
}

void CSlrFontBitmap::BlitText(const char *text, float x, float y, float z,
				  float colorR, float colorG, float colorB, float alpha)
{
	//LOGTODO("CSlrFontBitmap::BlitText: RGB");
#if defined(WIN32) || defined(LINUX) || defined(ANDROID)
	this->BlitText(text, x, y, z, 18.0f, alpha);
#else
	SYS_FatalExit("CSlrFontBitmap::BlitText: RGB");
#endif
}

void CSlrFontBitmap::BlitText(const std::string &text, float x, float y, float z, 
			  float colorR, float colorG, float colorB, float alpha, 
			  int align = FONT_ALIGN_LEFT, float scale = 1.0f)
{
	//LOGTODO("CSlrFontBitmap::BlitText: RGB");
	this->BlitText((char*)text.c_str(), x, y, z, scale);
}

void CSlrFontBitmap::BlitText(const char *text, float x, float y, float z,
			  float colorR, float colorG, float colorB, float alpha, 
			  int align = FONT_ALIGN_LEFT, float scale = 1.0f)
{
	//LOGTODO("CSlrFontBitmap::BlitText: RGB");
#if defined(WIN32) || defined(LINUX) || defined(ANDROID)
	this->BlitText(text, x, y, z, scale, alpha);
#else
	SYS_FatalExit("CSlrFontBitmap::BlitText: RGB");
#endif
}

void CSlrFontBitmap::BlitChar(char chr, float posX, float posY, float posZ, float size)
{
	//LOGD("CSlrFontBitmap::BlitChar: chr='%c' posX=%f posY=%f posZ=%f size=%f", chr, posX, posY, posZ, size);
	u8 lX, lY;
	float letterX, letterY;
	
	if (chr == '\0')
		return;
	
	//GetFontChar(chr, &lY, &lX);	// [<- opposite]	
	GetFontChar(chr, &lX, &lY); // [<- normal]
	
	// opposite
	//lY = 25 - lY;
	
	letterX = (lX * pitchX);
	letterY = (lY * pitchY);
	
	image->Render(posX, posY, posZ, size, letterX, letterY, letterX+width, letterY+height);
}

void CSlrFontBitmap::BlitChar(char chr, float posX, float posY, float posZ, float size, float alpha)
{
	u8 lX, lY;
	float letterX, letterY;
	
	if (chr == '\0')
		return;
	
	//GetFontChar(chr, &lY, &lX);	// [<- opposite]	
	GetFontChar(chr, &lX, &lY);	// [<- normal]
	
	// opposite
	//lY = 25 - lY;
	
	letterX = (lX * pitchX);
	letterY = (lY * pitchY);
	
	image->RenderAlpha(posX, posY, posZ, size, /*alpha,*/ letterX, letterY, letterX+width, letterY+height, alpha);
	
}

float CSlrFontBitmap::GetTextWidth(const char *text, float scale)
{
	float len = strlen(text);
	
	return (len*scale);
}

float CSlrFontBitmap::GetCharWidth(char ch, float scale)
{
	return scale;
}

float CSlrFontBitmap::GetCharHeight(char ch, float scale)
{
	return scale;
}

