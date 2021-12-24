/*
 *  CSlrTexture.mm
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-03-02.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CSlrTexture.h"

CSlrTexture::CSlrTexture(CSlrImage *image, float startPosX, float startPosY, float endPosX, float endPosY)
{
	this->image = image;
	this->startPosX = startPosX;
	this->startPosY = startPosY;
	this->endPosX = endPosX;
	this->endPosY = endPosY;
}

CSlrTexture::~CSlrTexture()
{
}

