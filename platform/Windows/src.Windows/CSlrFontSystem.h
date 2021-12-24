/*
 *  SYS_CSystemFont.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-07-14.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 * original file by http://www.themusingsofalostprogrammer.com/2010/01/how-to-do-font-rendering-in-opengl-on.html
 * (public domain)
 */

#ifndef _CSYSTEM_FONT_H_
#define _CSYSTEM_FONT_H_

#include "CSlrFont.h"
#include <map>
#include <string>
#include <sstream>

class CSlrFontSystem : public CSlrFont
{
public:
	CSlrFontSystem(char *name, bool withHalo, const std::string &family = "Helvetica", int size = 36);
	CSlrFontSystem(char *name, bool withHalo, int size, const std::string &family = "Helvetica");
	~CSlrFontSystem();
	
	void EnsureCharacters(char *text);
	 
	void BlitText(char *text, float posX, float posY, float posZ, float size);
	void BlitText(char *text, float posX, float posY, float posZ, float size, float alpha);
	void BlitText(char *text, float posX, float posY, float posZ, float fontSizeX, float fontSizeY, float alpha);
	void BlitChar(char chr, float posX, float posY, float posZ, float size);
	void BlitChar(char chr, float posX, float posY, float posZ, float size, float alpha);
	
	void BlitText(const std::string &text, float x, float y, float z, 
				  float colorR, float colorG, float colorB, float alpha, 
				  int align = FONT_ALIGN_LEFT, float scale = 1.0f);
	
	void BlitText(char *text, float x, float y, float z, 
				  float colorR, float colorG, float colorB, float alpha, 
				  int align = FONT_ALIGN_LEFT, float scale = 1.0f);

	char *family;
	const int size;
	bool withHalo;
	float haloR, haloG, haloB; //, haloAlpha;
};

#endif // _CSYSTEM_FONT_H_
