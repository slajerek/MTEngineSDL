#ifndef _GUI_VIEW_FADE_
#define _GUI_VIEW_FADE_

#include "CGuiView.h"

#define FADE_STATE_SHOW 0
#define FADE_STATE_COVER 1
#define FADE_STATE_BLACK FADE_STATE_COVER
#define FADE_STATE_FADEIN 2
#define FADE_STATE_FADEOUT 3

class CGuiViewFade : public CGuiView //, CGuiButtonCallback
{
public:
	CGuiViewFade(float posX, float posY, float posZ, float sizeX, float sizeY,
				 float alphaSpeed, float colorR, float colorG, float colorB);
	~CGuiViewFade();

	virtual void Render();
	virtual void DoLogic();

	u8 state;

	float alpha;
	float alphaSpeed;

	float colorR;
	float colorG;
	float colorB;

	void MakeFadeIn();
	void MakeFadeOut();
	void Show();
	void Cover();
};

#endif
//_GUI_VIEW_FADE_

