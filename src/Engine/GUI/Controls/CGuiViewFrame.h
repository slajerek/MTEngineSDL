#ifndef _CGUIVIEWFRAME_H_
#define _CGUIVIEWFRAME_H_

#include "CGuiView.h"
#include "CSlrFont.h"
#include "CGuiButton.h"
#include <list>

class CGuiViewToolBox;
class CGuiViewToolBoxCallback;

#define GUI_FRAME_HAS_FRAME			BV01
#define GUI_FRAME_HAS_TITLE			BV02
#define GUI_FRAME_HAS_CLOSE_BUTTON	BV03
#define GUI_FRAME_IS_TOOLBOX		BV04

#define GUI_FRAME_NO_FRAME	(0)
#define GUI_FRAME_MODE_WINDOW	(GUI_FRAME_HAS_FRAME | GUI_FRAME_HAS_TITLE | GUI_FRAME_HAS_CLOSE_BUTTON)

class CGuiViewFrame : public CGuiView, CGuiButtonCallback
{
public:
	CGuiViewFrame(CGuiView *view, CSlrString *barTitle);
	CGuiViewFrame(CGuiView *view, CSlrString *barTitle, u32 mode);
	
	void Initialize(CGuiView *view, CSlrString *barTitle, u32 mode);
	
	virtual void Render();
	virtual void Render(float posX, float posY);
	//virtual void Render(float posX, float posY, float sizeX, float sizeY);
	virtual void DoLogic();

	float barHeight;

	float barColorR;
	float barColorG;
	float barColorB;
	float barColorA;
	
	float barTextColorR;
	float barTextColorG;
	float barTextColorB;
	float barTextColorA;

	float frameWidth;

	CSlrString *barTitle;
	CSlrImage *imgIconClose;
	CGuiButton *btnCloseWindow;
	
	CSlrFont *barFont;
	float fontSize;
	
	virtual void SetBarTitle(CSlrString *newBarTitle);
	
	//
	virtual bool IsInside(float x, float y);
	virtual bool IsInsideSurroundingFrame(float x, float y);

	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	
	virtual bool DoRightClick(float x, float y);

	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float posX, float posY);
	
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	
	virtual void UpdateSize();
	virtual void SetSize(float sizeX, float sizeY);

	//
	virtual void AddBarToolBox(CGuiViewToolBoxCallback *callback);
	virtual void AddBarIcon(CSlrImage *imageIcon);
	CGuiViewToolBox *viewFrameToolBox;
	
	virtual bool ButtonPressed(CGuiButton *button);

	float barTitleWidth;
	
	bool movingView;
private:
	CGuiView *view;
};

#endif
