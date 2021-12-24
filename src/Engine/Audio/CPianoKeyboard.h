/*
 *  CPianoKeyboard.h (CGuiKeyboard)
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-26.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _SLRPIANOKEYBOARD_
#define _SLRPIANOKEYBOARD_

#include "CSlrImage.h"
#include "CGuiView.h"

class CPianoKeyboard;

class CPianoKeyboardCallback
{
public:
	virtual void PianoKeyboardNotePressed(CPianoKeyboard *pianoKeyboard, u8 note);
	virtual void PianoKeyboardNoteReleased(CPianoKeyboard *pianoKeyboard, u8 note);
};

class CPianoKey
{
public:
	CPianoKey(u8 keyNote, u8 keyOctave, const char *keyName, double x, double y, double sizeX, double sizeY, bool isBlackKey);
	
	double x, y;
	double sizeX, sizeY;
	
	u8 keyNote;
	u8 keyOctave;
	char keyName[4];

	// key colour (dest)
	float r,g,b,a;
	
	// current colour (i.e. including fade off)
	float cr,cg,cb,ca;
	
	bool isBlackKey;	// sharp
	
//	virtual void Render(float posX, float posY);
};

class CPianoNoteKeyCode
{
public:
	CPianoNoteKeyCode(u32 keyCode, int keyNote) { this->keyCode = keyCode; this->keyNote = keyNote; }
	u32 keyCode;
	int keyNote;
};

class CPianoKeyboard : public CGuiView
{
public:
	CPianoKeyboard(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CPianoKeyboardCallback *callback);
	~CPianoKeyboard();
	
	int numOctaves;
	
	virtual void InitKeys();
	
	virtual void Render();
	virtual void DoLogic();

	virtual bool DoTap(float x, float y);
	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoFinishDoubleTap(float x, float y);
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	
	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

	virtual void LayoutParameterChanged(CLayoutParameter *layoutParameter);

	virtual void AddDefaultKeyCodes();
	
	double keyWhiteWidth;
	double keyBlackOffset;
	double keyBlackWidth;
	double keyBlackHeight;

	u8 GetPressedNote(float x, float y);
	volatile bool tapped;
	
	const char **octaveNames;
	
	std::vector<CPianoKey *> pianoKeys;
	std::vector<CPianoKey *> pianoWhiteKeys;
	std::vector<CPianoKey *> pianoBlackKeys;

	void SetKeysFadeOut(bool doKeysFadeOut);
	void SetKeysFadeOutSpeed(float speed);
	bool doKeysFadeOut;
	
	CPianoKeyboardCallback *callback;

	float keysFadeOutSpeed;
	float keysFadeOutSpeedOneMinus;
	float keysFadeOutSpeedParameter;	// scaled by 10x for UI

	std::list<CPianoNoteKeyCode *> notesKeyCodes;
	
	int currentOctave;
};

#endif //_SLRPIANOKEYBOARD_
