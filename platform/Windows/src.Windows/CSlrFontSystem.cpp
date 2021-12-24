/*
 *  SYS_CSystemFont.mm
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-07-14.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

/*
CBitmap bitmap;
CBitmap *pOldBmp;
CDC MemDC;

CDC *pDC = GetDC();
MemDC.CreateCompatibleDC(pDC);
bitmap.CreateCompatibleBitmap(pDC, width, height );

pOldBmp = MemDC.SelectObject(&MyBmp);

CBrush brush;
brush.CreateSolidBrush(RGB(255,0,0));

CRect rect;
rect.SetRect (0,0,40,40);
MemDC.SelectObject(&brush);

MemDC.DrawText("Hello",6, &rect, DT_CENTER );
MemDC.SetTextColor(RGB(0,0,255));
GetDC()->BitBlt(0, 0, 50, 50, &MemDC, 0, 0, SRCCOPY);

//When done, than:
MemDC.SelectObject(pOldBmp);
ReleaseDC(&MemDC);
ReleaseDC(pDC);

bitmap.Save(_T("C:\\test.bmp"), Gdiplus::ImageFormatJPEG);

*/


#include "CSlrFontSystem.h"
#include "SYS_Main.h"	// for EXEC_ON_VALGRIND

#include <iostream>
#include <string>
using namespace std;

#ifndef EXEC_ON_VALGRIND

CSlrFontSystem::~CSlrFontSystem()
{
}

CSlrFontSystem::CSlrFontSystem(char *name, bool withHalo, const std::string &family, int size)
: size(size), CSlrFont(name)
{
//	this->family = [[NSString alloc] initWithCString:family.c_str()
//											encoding:NSUTF8StringEncoding];
	//cout << "Cleaning up chars, size: " << chars.size() << endl;
	//chars.clear();
	
	this->withHalo = withHalo;
	this->haloR = 0.0;
	this->haloG = 0.0;
	this->haloB = 0.0;
	//this->haloAlpha = 1.0;
	this->fontType = FONT_TYPE_SYSTEM;
}

CSlrFontSystem::CSlrFontSystem(char *name, bool withHalo, int size, const string &family)
: size(size), CSlrFont(name)
{
//	this->family = [[NSString alloc] initWithCString:family.c_str()
//											encoding:NSUTF8StringEncoding];
	//cout << "Cleaning up chars, size: " << chars.size() << endl;
	//chars.clear();

	this->withHalo = withHalo;
	this->haloR = 0.0;
	this->haloG = 0.0;
	this->haloB = 0.0;
	//this->haloAlpha = 1.0;
	this->fontType = FONT_TYPE_SYSTEM;
}

static const float zero = 0.006f;
static const float one = 0.994f;

/*
 // helper function to draw a rectangle
 static void setRectData(float x, float y, float z, float w, float h, 
 float *vert, float *tex, float *norm, float wtex = one, float htex = one,
 float xstart = zero, float ystart = zero)
 {
 for(int i = 0; i < 18; i += 3) {
 
 norm[i + 0] = 0.0f;
 norm[i + 1] = 0.0f;
 norm[i + 2] = -1.0f;
 }
 
 int i = 0;
 
 wtex += xstart;
 htex += ystart;
 
 vert[i + 0] = x;
 vert[i + 1] = y;
 vert[i + 2] = z;
 
 vert[i + 3] = x;
 vert[i + 4] = y + h;
 vert[i + 5] = z;
 
 vert[i + 6] = x + w;
 vert[i + 7] = y + h;
 vert[i + 8] = z;
 
 tex[i + 0] = xstart;
 tex[i + 1] = ystart;
 tex[i + 2] = zero;
 
 tex[i + 3] = xstart;
 tex[i + 4] = htex;
 tex[i + 5] = zero;
 
 tex[i + 6] = wtex;
 tex[i + 7] = htex;
 tex[i + 8] = zero;
 
 i += 9;
 
 vert[i + 0] = x;
 vert[i + 1] = y;
 vert[i + 2] = z;
 
 vert[i + 3] = x + w;
 vert[i + 4] = y + h;
 vert[i + 5] = z;
 
 vert[i + 6] = x + w;
 vert[i + 7] = y;
 vert[i + 8] = z;
 
 tex[i + 0] = xstart;
 tex[i + 1] = ystart;
 tex[i + 2] = zero;
 
 tex[i + 3] = wtex;
 tex[i + 4] = htex;
 tex[i + 5] = zero;
 
 tex[i + 6] = wtex;
 tex[i + 7] = ystart;
 tex[i + 8] = zero;
 }
 */

//static bool loadingIt = true;

void CSlrFontSystem::BlitText(char *text, float posX, float posY, float posZ, float size)
{
	float colorR = 1.0;
	float colorG = 1.0;
	float colorB = 1.0;
	float alpha = 1.0;
	
	float x = posX;
	float y = posY;
	float z = posZ;
	
	int numChars = strlen(text);
	
	float scale = (size) / (float)(this->size);

	/*
	for(int i = 0, count = 0; i < numChars; i += count)
	{
		Char c = getChar(text + i, &count);

		float w = c.w * scale;
		
		if (withHalo)
			BlitTexture(c.texHalo, x, y, z, w, c.h * scale, zero, zero, c.w_tw, c.h_th,
						haloR, haloG, haloB, alpha);

		BlitTexture(c.tex, x, y, z, w, c.h * scale, zero, zero, c.w_tw, c.h_th,
					colorR, colorG, colorB, alpha);
		x += w;
	}*/
}

void CSlrFontSystem::BlitText(char *text, float posX, float posY, float posZ, float size, float alpha)
{
	float colorR = 1.0;
	float colorG = 1.0;
	float colorB = 1.0;
	
	float x = posX;
	float y = posY;
	float z = posZ;
	
	/*
	int numChars = strlen(text);
	float scale = (size) / (float)(this->size);
	
	for(int i = 0, count = 0; i < numChars; i += count)
	{
		Char c = getChar(text + i, &count);
	
		float w = c.w * scale;

		if (withHalo)
			BlitTexture(c.texHalo, x, y, z, w, c.h * scale, zero, zero, c.w_tw, c.h_th,
						haloR, haloG, haloB, alpha);
		
		BlitTexture(c.tex, x, y, z, w, c.h * scale, zero, zero, c.w_tw, c.h_th,
					colorR, colorG, colorB, alpha);
		x += w;
	}*/
}

void CSlrFontSystem::BlitText(char *text, float posX, float posY, float posZ, float fontSizeX, float fontSizeY, float alpha)
{
	float colorR = 1.0;
	float colorG = 1.0;
	float colorB = 1.0;
	
	float x = posX;
	float y = posY;
	float z = posZ;
	
	/*
	int numChars = strlen(text);
	float scale = (size) / (float)(this->size);
	
	for(int i = 0, count = 0; i < numChars; i += count)
	{
		Char c = getChar(text + i, &count);

		if (withHalo)
			BlitTexture(c.texHalo, x, y, z, fontSizeX, fontSizeY, zero, zero, c.w_tw, c.h_th,
						haloR, haloG, haloB, alpha);
		
		BlitTexture(c.tex, x, y, z, fontSizeX, fontSizeY, zero, zero, c.w_tw, c.h_th,
					colorR, colorG, colorB, alpha);
		x += fontSizeX;
	}*/
}

void CSlrFontSystem::BlitChar(char chr, float posX, float posY, float posZ, float size)
{
	float colorR = 1.0f;
	float colorG = 1.0f;
	float colorB = 1.0f;
	float alpha = 1.0f;
	
	float x = posX;
	float y = posY;
	float z = posZ;

	/*
	int count = 0;
	float scale = (size) / (float)(this->size);
	
	Char c = getChar(&chr, &count);

#ifndef RELEASE
	if (count != 1)
		SYS_FatalExit("CSlrFontSystem::BlitChar: char='%c' count=%d", chr, count);
#endif
	
	if (withHalo)
		BlitTexture(c.texHalo, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
					haloR, haloG, haloB, alpha);
		
	BlitTexture(c.tex, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
					colorR, colorG, colorB, alpha);

	*/
}

void CSlrFontSystem::BlitChar(char chr, float posX, float posY, float posZ, float size, float alpha)
{
	float colorR = 1.0f;
	float colorG = 1.0f;
	float colorB = 1.0f;
	
	float x = posX;
	float y = posY;
	float z = posZ;
	
	int count = 0;
	float scale = (size) / (float)(this->size);
	
	/*
	Char c = getChar(&chr, &count);
	
#ifndef RELEASE
	if (count != 1)
		SYS_FatalExit("CSlrFontSystem::BlitChar: char='%c' count=%d", chr, count);
#endif

	if (withHalo)
		BlitTexture(c.texHalo, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
					haloR, haloG, haloB, alpha);

	BlitTexture(c.tex, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
				colorR, colorG, colorB, alpha);

	*/
}

void CSlrFontSystem::BlitText(const string &text, float x, float y, float z, 
							  float colorR, float colorG, float colorB, float alpha, 
							  int align, float scale)
{
	//LOGD("blit CSlrFontSystem");
	// getSize() is an expensive operation, so we use sizeDirty.
	/*
	Size size;
	bool sizeDirty = true;
	
	if(align > -1) {
		
		if(sizeDirty)
			size = getSize(text, scale);
		
		sizeDirty = false;
		
		if(align > 0)
			x -= size.width;
		else
			x -= size.width / 2;
	}

	for(int i = 0, count = 0; i < text.size(); i += count)
	{
		Char c = getChar(text.c_str() + i, &count);
		
		if (withHalo)
			BlitTexture(c.texHalo, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
						haloR, haloG, haloB, alpha);
			
		BlitTexture(c.tex, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
					colorR, colorG, colorB, alpha);
		x += c.w * scale;
	}*/
}

void CSlrFontSystem::EnsureCharacters(char *text)
{
	LOGD("EnsureCharacters");
	LOGD(text);
	
	/*
	std::string text2 = [text UTF8String];
	for(int i = 0, count = 0; i < text2.size(); i += count)
	{
		Char c = getChar(text2.c_str() + i, &count);
	}*/

	LOGD("EnsureCharacters done");
}

void CSlrFontSystem::BlitText(char *textN, float x, float y, float z, 
							  float colorR, float colorG, float colorB, float alpha, 
							  int align, float scale)
{
	//	LOGD("CSlrFontSystem::BlitText(NSString *textN");
	/*
	std::string text = [textN UTF8String];
	
	//	cout << "text=" << text << "endl";
	
	// getSize() is an expensive operation, so we use sizeDirty.
	Size size;
	bool sizeDirty = true;
	
	if(align > -1) {
		
		if(sizeDirty)
			size = getSize(text, scale);
		
		sizeDirty = false;
		
		if(align > 0)
			x -= size.width;
		else
			x -= size.width / 2;
	}
	
	for(int i = 0, count = 0; i < text.size(); i += count)
	{
		Char c = getChar(text.c_str() + i, &count);
		
		if (withHalo)
			BlitTexture(c.texHalo, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
						haloR, haloG, haloB, alpha);

		BlitTexture(c.tex, x, y, z, c.w * scale, c.h * scale, zero, zero, c.w_tw, c.h_th,
					colorR, colorG, colorB, alpha);
		x += c.w * scale;
	}
	*/
}


#endif