#ifndef _CGuiViewToolBox_H_
#define _CGuiViewToolBox_H_

#include "SYS_Defs.h"
#include "CGuiWindow.h"
#include "CGuiViewFrame.h"
#include "CGuiList.h"
#include "CGuiButtonSwitch.h"
#include "CGuiLabel.h"
#include <vector>
#include <list>

class CSlrFont;
class CSlrMutex;
class CDebugInterfaceC64;

class CGuiViewToolBoxCallback
{
public:
	virtual void ToolBoxIconPressed(CSlrImage *imgIcon);
};

class CGuiViewToolBox : public CGuiWindow, public CGuiListCallback, CGuiButtonSwitchCallback
{
public:
	CGuiViewToolBox(float posX, float posY, float posZ,
				float iconGapX, float iconGapY,
				float iconSizeX, float iconSizeY, float iconStepX, float iconStepY,
				int numColumns,
				CSlrString *windowName,
				CGuiViewToolBoxCallback *callback);

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	
	virtual bool DoTap(float x, float y);
	
	virtual bool DoRightClick(float x, float y);
//	virtual bool DoFinishRightClick(float x, float y);

//	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);

//	virtual void SetPosition(float posX, float posY);
//	virtual void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);
	
	virtual void Render();
	virtual void DoLogic();
	
	//
	virtual void RenderButtons();

	CGuiViewToolBoxCallback *callback;
	
	CSlrFont *font;
	float fontScale;
	float fontHeight;
	
	virtual void ActivateView();

	virtual bool ButtonPressed(CGuiButton *button);
	
	std::vector<CGuiButton *> buttons;
	
	float iconGapX;
	float iconGapY;
	float iconSizeX;
	float iconSizeY;
	float iconStepX;
	float iconStepY;

	int numColumns;
	
	float backgroundColorR;
	float backgroundColorG;
	float backgroundColorB;
	float backgroundColorA;
	
	CGuiButton *AddIcon(CSlrImage *imgIcon);
	CGuiButton *AddIcon(CSlrImage *imgIcon, float iconX, float iconY);
	void UpdateSize(int numColumns);
	
	float nextIconX;
	float nextIconY;
	int numIconsInCurrentRow;
	int numRows;

};


#endif

