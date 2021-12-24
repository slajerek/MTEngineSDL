/*
 *  CGuiViewNumKeys.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-05-25.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CSlrImage.h"
#include "CGuiElement.h"
#include "CGuiView.h"

class CGuiViewNumKeysCallback;

class CGuiViewNumKeys : public CGuiView
{
public:
	CGuiViewNumKeys(float posX, float posY, float posZ, float sizeX, float sizeY, CGuiViewNumKeysCallback *callback);
	
	virtual void Render();
	CGuiViewNumKeysCallback *callback;

};
