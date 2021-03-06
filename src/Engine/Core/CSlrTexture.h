/*
 *  CSlrTexture.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-03-02.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef __VID_CSLRTEXTURE_H__
#define __VID_CSLRTEXTURE_H__

#include "SYS_Defs.h"
#include "CSlrImage.h"

class CSlrTexture
{
public:
	CSlrTexture(CSlrImage *image, float startPosX, float startPosY, float endPosX, float endPosY);
	virtual ~CSlrTexture();

	float startPosX;
	float startPosY;
	float endPosX;
	float endPosY;

	CSlrImage *image;
};


#endif // __VID_CSLRTEXTURE_H__
