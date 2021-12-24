/*
 *  CGuiEditBoxFloat.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _CGUIEDITBOXFLOAT_H_
#define _CGUIEDITBOXFLOAT_H_

#include "CGuiEditBoxText.h"

class CGuiEditBoxFloat : public CGuiEditBoxText
{
public:
	CGuiEditBoxFloat(float posX, float posY, float posZ, float fontWidth, float fontHeight,
					float defaultValue, u8 maxDigitsL, u8 maxDigitsR, bool readOnly,
					CGuiEditBoxTextCallback *callback);

	CGuiEditBoxFloat(float posX, float posY, float posZ, float fontWidth, float fontHeight,
					 float defaultValue, u8 maxDigitsL, u8 maxDigitsR, u8 maxNumChars, bool readOnly,
					 CGuiEditBoxTextCallback *callback);

	void SetFloat(float value);
	float GetFloat();

	float value;

	u8 maxDigitsL, maxDigitsR;

	virtual bool KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats
};

#endif

