/*
 * CGuiTheme.h
 *
 *  Created on: Jan 31, 2012
 *      Author: mars
 */

#ifndef CGUITHEME_H_
#define CGUITHEME_H_

#include "CSlrImage.h"
#include <list>
#include "CConfigStorage.h"
#include "CSlrFont.h"

#define RESOURCE_LEVEL_THEME	2

class CGuiThemeReloadCallback
{
public:
	void ReloadThemeCallback(CConfigStorage *themeData);
};

class CThemeChangeListener
{
public:
	virtual void UpdateTheme();
};

class CGuiTheme
{
public:
	CGuiTheme();
	CGuiTheme(const char *themeName);
	void Init(const char *themeName);
	~CGuiTheme();
	virtual void InitDefaultTheme();
	
	char *themeName;
	char *themeDisplayName;
	char *themeAuthor;
	char *themeWebLink;
	CConfigStorage *themeData;
	
	CSlrImage *imgBackground;

	CSlrImage *imgBackgroundSelectionPlain;
	CSlrImage *imgBackgroundTextboxEditCursor;

	CSlrImage *imgBackgroundMenu;
	CSlrImage *imgBackgroundLabel;
	CSlrImage *imgBackgroundSelection;

	CSlrImage *imgListSelection;

	CSlrImage *imgButtonBackgroundEnabled;
	CSlrImage *imgButtonBackgroundEnabledPressed;
	CSlrImage *imgButtonBackgroundDisabled;

	CSlrImage *imgSliderEmpty;
	CSlrImage *imgSliderFull;
	CSlrImage *imgSliderGauge;

	// button
	float buttonShadeAmount;
	float buttonShadeDistance;
	float buttonShadeDistance2;

	float buttonEnabledColorR;
	float buttonEnabledColorG;
	float buttonEnabledColorB;
	float buttonEnabledColorA;
	float buttonEnabledColor2R;
	float buttonEnabledColor2G;
	float buttonEnabledColor2B;
	float buttonEnabledColor2A;

	float buttonDisabledColorR;
	float buttonDisabledColorG;
	float buttonDisabledColorB;
	float buttonDisabledColorA;
	float buttonDisabledColor2R;
	float buttonDisabledColor2G;
	float buttonDisabledColor2B;
	float buttonDisabledColor2A;

	float buttonSwitchOnColorR;
	float buttonSwitchOnColorG;
	float buttonSwitchOnColorB;
	float buttonSwitchOnColorA;
	float buttonSwitchOnColor2R;
	float buttonSwitchOnColor2G;
	float buttonSwitchOnColor2B;
	float buttonSwitchOnColor2A;

	float buttonSwitchOffColorR;
	float buttonSwitchOffColorG;
	float buttonSwitchOffColorB;
	float buttonSwitchOffColorA;
	float buttonSwitchOffColor2R;
	float buttonSwitchOffColor2G;
	float buttonSwitchOffColor2B;
	float buttonSwitchOffColor2A;

	float buttonOnTextColorR;
	float buttonOnTextColorG;
	float buttonOnTextColorB;
	
	float buttonOffTextColorR;
	float buttonOffTextColorG;
	float buttonOffTextColorB;
	
	float buttonDisabledTextColorR;
	float buttonDisabledTextColorG;
	float buttonDisabledTextColorB;
	

	// text box
	float textBoxColorR;
	float textBoxColorG;
	float textBoxColorB;
	float textBoxColorA;
	float textBoxColor2R;
	float textBoxColor2G;
	float textBoxColor2B;
	float textBoxColor2A;
	float textBoxCursorBlinkSpeed;
	float cursorColorR;
	float cursorColorG;
	float cursorColorB;
	float cursorColorA;
	
	float focusBorderLineWidth;
	
	// for gui progressbar:
	float currentThemeLoadPercentage;
	
	void LoadImageTheme(CSlrImage **image, const char *imageName, const char *imageDefaultPath, bool linearScale);
	void LoadFontTheme(CSlrImage **image, CSlrFont **font, const char *fontConfigName, const char *fontDefaultPath, bool linearScale);
	
	void LoadTheme(const char *themeName);
	void LoadTheme(CConfigStorage *configStorage);

	// theme reload callbacks
	std::list<CGuiThemeReloadCallback *> themeReloadCallbacks;
	
	void InitDefaultValues();
};

#endif /* CGUITHEME_H_ */
