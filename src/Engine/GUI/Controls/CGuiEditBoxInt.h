/*
 *  CGuiEditBoxInt.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-12-03.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _CGUIEDITBOXINT_H_
#define _CGUIEDITBOXINT_H_

#include "CGuiEditBoxText.h"

class CGuiEditBoxInt : public CGuiEditBoxText
{
public:
	CGuiEditBoxInt(float posX, float posY, float posZ, float fontWidth, float fontHeight,
					 int defaultValue, u8 maxDigits, bool readOnly,
					 CGuiEditBoxTextCallback *callback);

	void SetInteger(int value);
	int GetInteger();

	int value;

	u8 maxDigits;

	virtual bool KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats
};

#endif

