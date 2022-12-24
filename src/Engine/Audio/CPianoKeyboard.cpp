/*
 *  CPianoKeyboard (CPianoKeyboard.cpp)
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-26.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#include "CPianoKeyboard.h"
#include "VID_Main.h"
#include "CGuiMain.h"
#include "CLayoutParameter.h"
#include "CMidiInKeyboard.h"

#define OCT_NAME_FONT_SIZE_X 8.0
#define OCT_NAME_FONT_SIZE_Y 8.0
#define OCT_NAME_GAP_X 2.0
#define OCT_NAME_GAP_Y 1.0

const char *pianoKeyboardKeyNames = "CDEFGAB";

void CPianoKeyboardCallback::PianoKeyboardNotePressed(CPianoKeyboard *pianoKeyboard, CPianoKey *pianoKey)
{
}

void CPianoKeyboardCallback::PianoKeyboardNoteReleased(CPianoKeyboard *pianoKeyboard, CPianoKey *pianoKey)
{
}


CPianoKeyboard::CPianoKeyboard(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CPianoKeyboardCallback *callback)
:CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	mutex = new CSlrMutex("CPianoKeyboard");
	
	imGuiNoWindowPadding = true;
	imGuiNoScrollbar = true;

	midiPortNum = -1;
	midiChannel = 0;
	
	keyWhiteWidth = 1.0/7.0;
	keyBlackOffset = keyWhiteWidth * (3.0f/4.0f);
	keyBlackWidth = keyWhiteWidth * (2.0f/4.0f);
	keyBlackHeight = (3.0f/5.0f);
	
	this->numOctaves = 8;
	this->octaveNames = new const char *[this->numOctaves];
	octaveNames[0] = "0";
	octaveNames[1] = "I";
	octaveNames[2] = "II";
	octaveNames[3] = "III";
	octaveNames[4] = "IV";
	octaveNames[5] = "V";
	octaveNames[6] = "VI";
	octaveNames[7] = "VII";
//	octaveNames[8] = "VIII";
//	octaveNames[9] = "IX";
	
	SetKeysFadeOut(true);
	SetKeysFadeOutSpeed(0.40f);

	currentOctave = 4;
	
	this->callback = callback;
	
	this->InitKeys();
	
	AddLayoutParameter(new CLayoutParameterBool("Keys fade out", &doKeysFadeOut));
	AddLayoutParameter(new CLayoutParameterFloat("Keys fade out speed", &keysFadeOutSpeedParameter, 0.0f, 20.0f));
	
	// TODO
	midiInKeyboard = new CMidiInKeyboard(0, this);
	if (midiInKeyboard->midiIn->isPortOpen())
	{
		midiPortNum = 0;
	}
}

void CPianoKeyboard::SetKeysFadeOut(bool doKeysFadeOut)
{
	this->doKeysFadeOut = doKeysFadeOut;
}

void CPianoKeyboard::SetKeysFadeOutSpeed(float speed)
{
	this->keysFadeOutSpeed = speed;
	this->keysFadeOutSpeedOneMinus = 1.0f - this->keysFadeOutSpeed;
	this->keysFadeOutSpeedParameter = speed * 10.0f;
}

void CPianoKeyboard::LayoutParameterChanged(CLayoutParameter *layoutParameter)
{
	this->keysFadeOutSpeed = keysFadeOutSpeedParameter / 10.0f;
	this->keysFadeOutSpeedOneMinus = 1.0f - this->keysFadeOutSpeed;
}

CPianoKey::CPianoKey(u8 keyNote, u8 keyOctave, const char *keyName, double x, double y, double sizeX, double sizeY, bool isBlackKey)
{
	this->keyNote = keyNote; this->keyOctave = keyOctave, this->x = x; this->y = y; this->sizeX = sizeX; this->sizeY = sizeY; this->isBlackKey = isBlackKey;
	strcpy(this->keyName, keyName);
	
	if (!isBlackKey)
	{
		r = g = b = a = 1.0f;
		cr = cg = cb = ca = 1.0f;
	}
	else
	{
		r = g = b = 0; a = 1.0f;
		cr = cg = cb = 0; ca = 1.0f;
	}
	
//	LOGD("CPianoKey: %d %s: %6.3f %6.3f %6.3f %6.3f", keyNote, keyName, x, y, sizeX, sizeY);
}

void CPianoKeyboard::InitKeys()
{
	AddDefaultKeyCodes();

	char *keyName = SYS_GetCharBuf();

	double fOctaveStep = 1 / (double)numOctaves;
	keyWhiteWidth  = fOctaveStep * 1.0/7.0;
	keyBlackOffset = keyWhiteWidth * (3.0f/4.0f);
	keyBlackWidth  = keyWhiteWidth * (2.0f/4.0f);
	keyBlackHeight = (3.0f/5.0f);

	CPianoKey *key = NULL;
	
	int keyNum = 0;
	for (int octaveNum = 0; octaveNum < numOctaves; octaveNum++)
	{
		double octaveOffset = fOctaveStep * (double)octaveNum;
		for (int keyNumInOctave = 0; keyNumInOctave < 7; keyNumInOctave++)
		{
			sprintf(keyName, "%c-%d", pianoKeyboardKeyNames[keyNumInOctave], octaveNum);
			key = new CPianoKey(keyNum, octaveNum, keyName,
								keyWhiteWidth * (double)keyNumInOctave + octaveOffset,
								0.0f,
								keyWhiteWidth,
								1.0f, false);
			keyNum++;
			pianoKeys.push_back(key);
			pianoWhiteKeys.push_back(key);
			
			if (keyNumInOctave == 0
				|| keyNumInOctave == 1
				|| keyNumInOctave == 3
				|| keyNumInOctave == 4
				|| keyNumInOctave == 5)
			{
				sprintf(keyName, "%c#%d", pianoKeyboardKeyNames[keyNumInOctave], octaveNum);
				key = new CPianoKey(keyNum, octaveNum, keyName,
									keyBlackOffset + keyWhiteWidth * (double)keyNumInOctave + octaveOffset,
									0.0f,
									keyBlackWidth,
									keyBlackHeight, true);
				keyNum++;
				pianoKeys.push_back(key);
				pianoBlackKeys.push_back(key);
			}
		}
	}
	
	SYS_ReleaseCharBuf(keyName);
	LOGD("InitKeys done");
}

void CPianoKeyboard::Render()
{
//	LOGD("CPianoKeyboard::Render");
	
	for (std::vector<CPianoKey *>::iterator it = pianoWhiteKeys.begin(); it != pianoWhiteKeys.end(); it++)
	{
		CPianoKey *key = *it;
		BlitFilledRectangle(this->posX + key->x * this->sizeX, this->posY + key->y * this->sizeY, -1,
							key->sizeX * this->sizeX, key->sizeY * this->sizeY,
							key->cr, key->cg, key->cb, key->ca);
		
		// border
		BlitRectangle(this->posX + key->x * this->sizeX, this->posY + key->y * this->sizeY, -1,
					  key->sizeX * this->sizeX, key->sizeY * this->sizeY,
					  0, 0, 0, 1);
	}

	for (std::vector<CPianoKey *>::iterator it = pianoBlackKeys.begin(); it != pianoBlackKeys.end(); it++)
	{
		CPianoKey *key = *it;
		BlitFilledRectangle(this->posX + key->x * this->sizeX, this->posY + key->y * this->sizeY, -1,
							key->sizeX * this->sizeX, key->sizeY * this->sizeY,
							key->cr, key->cg, key->cb, key->ca);

		BlitRectangle(this->posX + key->x * this->sizeX, this->posY + key->y * this->sizeY, -1,
					  key->sizeX * this->sizeX, key->sizeY * this->sizeY,
					  0, 0, 0, 1);
	}

}

void CPianoKeyboard::DoLogic()
{
//	LOGD("CPianoKeyboard::DoLogic");
	if (doKeysFadeOut)
	{
		for (std::vector<CPianoKey *>::iterator it = pianoKeys.begin(); it != pianoKeys.end(); it++)
		{
			CPianoKey *key = *it;
			
			key->cr = key->cr * keysFadeOutSpeedOneMinus + key->r * keysFadeOutSpeed;
			key->cg = key->cg * keysFadeOutSpeedOneMinus + key->g * keysFadeOutSpeed;
			key->cb = key->cb * keysFadeOutSpeedOneMinus + key->b * keysFadeOutSpeed;
			key->ca = key->ca * keysFadeOutSpeedOneMinus + key->a * keysFadeOutSpeed;
		}
	}
}

bool CPianoKeyboard::AddKeyToPressed(CPianoKey *key)
{
	bool found = false;
	for (std::list<CPianoKey *>::iterator it = pressedKeys.begin(); it != pressedKeys.end(); it++)
	{
		if (key == *it)
		{
			// note is already pressed
			found = true;
			break;
		}
	}
			
	if (!found)
	{
		// note was not pressed, add
		pressedKeys.push_back(key);
	}
	
	return found;
}

bool CPianoKeyboard::RemoveKeyFromPressed(CPianoKey *key)
{
	for (std::list<CPianoKey *>::iterator it = pressedKeys.begin(); it != pressedKeys.end(); it++)
	{
		if (*it == key)
		{
			pressedKeys.erase(it);
			return true;
		}
	}
	
	return false;
}

bool CPianoKeyboard::DoTap(float x, float y)
{
	LOGG("CPianoKeyboard::DoTap: x=%f y=%f posX=%f posY=%f sizeX=%f sizeY=%f", x, y, posX, posY, sizeX, sizeY);

	mutex->Lock();
	CPianoKey *pressedKey = GetPianoKey(x, y);
	
	if (pressedKey != NULL)
	{
		AddKeyToPressed(pressedKey);
		
		tappedKey = pressedKey;

	//	u8 pressedNoteKey = currentNotePressed->keyNote + editNoteOctave*12;
		
		if (callback != NULL)
			callback->PianoKeyboardNotePressed(this, pressedKey);
	}
	mutex->Unlock();
	
	return (pressedKey != NULL);
}

bool CPianoKeyboard::IsInsideKey(float x, float y, CPianoKey *key)
{
	float lx = this->posX + key->x * this->sizeX;
	float ly = this->posY + key->y * this->sizeY;
	float rx = lx + key->sizeX * this->sizeX;
	float ry = ly + key->sizeY * this->sizeY;

	if (x >= lx && x < rx
		&& y >= ly && y < ry)
	{
		return true;
	}
	return false;
}

CPianoKey *CPianoKeyboard::GetPianoKey(float x, float y)
{
	for (std::vector<CPianoKey *>::iterator it = pianoBlackKeys.begin(); it != pianoBlackKeys.end(); it++)
	{
		CPianoKey *key = *it;
		if (IsInsideKey(x, y, key))
		{
			return key;
		}
	}
	for (std::vector<CPianoKey *>::iterator it = pianoWhiteKeys.begin(); it != pianoWhiteKeys.end(); it++)
	{
		CPianoKey *key = *it;
		if (IsInsideKey(x, y, key))
		{
			return key;
		}
	}
	return NULL;
}

bool CPianoKeyboard::DoDoubleTap(float x, float y)
{
	return this->DoTap(x, y);
}

bool CPianoKeyboard::DoFinishTap(float x, float y)
{
	if (IsInsideView(x, y))
	{
		CPianoKey *releasedKey = GetPianoKey(x, y);
		
		mutex->Lock();
		if (releasedKey != NULL)
		{
			if (RemoveKeyFromPressed(releasedKey))
			{
				if (callback != NULL)
					callback->PianoKeyboardNoteReleased(this, releasedKey);
			}
		}
		
		if (tappedKey == releasedKey)
			tappedKey = NULL;
		
		mutex->Unlock();
		return true;
	}
	return false;
}

bool CPianoKeyboard::DoFinishDoubleTap(float x, float y)
{
	return this->DoFinishTap(x, y);
}

bool CPianoKeyboard::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	LOGG("CPianoKeyboard::DoMove");
	
	if (!IsInsideView(x, y))
		return false;
	
	CPianoKey *pianoKey = GetPianoKey(x, y);
	
	mutex->Lock();
	
	if (tappedKey != NULL && pianoKey != tappedKey)
	{
		if (RemoveKeyFromPressed(tappedKey))
		{
			if (callback != NULL)
				callback->PianoKeyboardNoteReleased(this, tappedKey);
		}
	}
	
	if (pianoKey != NULL)
	{
		AddKeyToPressed(pianoKey);
		tappedKey = pianoKey;
		if (callback != NULL)
			callback->PianoKeyboardNotePressed(this, pianoKey);
	}
	
	mutex->Unlock();
	
	return true; //this->DoTap(x, y);
}

bool CPianoKeyboard::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
//	if (x < sizeX-menuButtonSizeX)
//		return this->DoFinishTap(x, y);
	
	return false;
}

bool CPianoKeyboard::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGD("CPianoKeyboard::KeyDown: keyCode=%x", keyCode);
	
	// TODO: make via callback
	if (keyCode == '[' && isShift == false && isAlt == false && isControl == false && isSuper == false)
	{
		if (currentOctave > 0)
			currentOctave--;
		return true;
	}
	else if (keyCode == ']' && isShift == false && isAlt == false && isControl == false && isSuper == false)
	{
		if (currentOctave < numOctaves-2)
			currentOctave++;
		return true;
	}
	else if (this->callback != NULL)
	{
		// scan for note key code
		for (std::list<CPianoNoteKeyCode *>::iterator it = notesKeyCodes.begin(); it != notesKeyCodes.end(); it++)
		{
			CPianoNoteKeyCode *noteKeyCode = *it;
			if (keyCode == noteKeyCode->keyCode
				&& isShift == noteKeyCode->isShift
				&& isAlt == noteKeyCode->isAlt
				&& isControl == noteKeyCode->isControl
				&& isSuper == noteKeyCode->isSuper)
			{
				int keyNum = noteKeyCode->keyNote + currentOctave*12;
				if (keyNum >= 0 && keyNum < pianoKeys.size())
				{
					CPianoKey *pianoKey = pianoKeys[keyNum];
					AddKeyToPressed(pianoKey);
					this->callback->PianoKeyboardNotePressed(this, pianoKey);
				}
				return true;
			}
		}
	}
	
	return false;
}

bool CPianoKeyboard::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGD("CPianoKeyboard::KeyUp: keyCode=%x", keyCode);

	if (this->callback != NULL)
	{
		// scan for note key code
		for (std::list<CPianoNoteKeyCode *>::iterator it = notesKeyCodes.begin(); it != notesKeyCodes.end(); it++)
		{
			CPianoNoteKeyCode *noteKeyCode = *it;
			
			// TODO: what to do with isShift, isAlt, etc? do we need that anyway?
			if (keyCode == noteKeyCode->keyCode)
			{
				int keyNum = noteKeyCode->keyNote + currentOctave*12;
				if (keyNum >= 0 && keyNum < pianoKeys.size())
				{
					CPianoKey *pianoKey = pianoKeys[keyNum];
					RemoveKeyFromPressed(pianoKey);
					this->callback->PianoKeyboardNoteReleased(this, pianoKey);
				}
				return true;
			}
		}
	}

	return false;
}

void CPianoKeyboard::MidiInKeyboardCallbackNoteOn(int channel, int key, int pressure)
{
	LOGD("CPianoKeyboard::MidiInKeyboardCallbackNoteOn: channel=%d key=%d pressure=%d", channel, key, pressure);
	if (this->callback)
	{
		if (channel == midiChannel)
		{
			if (key >= 0 && key < pianoKeys.size())
			{
				mutex->Lock();
				CPianoKey *pianoKey = pianoKeys[key];
				AddKeyToPressed(pianoKey);
				this->callback->PianoKeyboardNotePressed(this, pianoKey);
				mutex->Unlock();
			}
		}
	}
}

void CPianoKeyboard::MidiInKeyboardCallbackNoteOff(int channel, int key, int pressure)
{
	LOGD("CPianoKeyboard::MidiInKeyboardCallbackNoteOff: channel=%d key=%d pressure=%d", channel, key, pressure);
	
//	// add array of pressed notes
//	if (tappedKey)
//	{
//		if (tappedKey->keyNote != key)
//			return;
//	}
	
	if (this->callback)
	{
		if (channel == midiChannel)
		{
			if (key >= 0 && key < pianoKeys.size())
			{
				mutex->Lock();
				CPianoKey *pianoKey = pianoKeys[key];
				RemoveKeyFromPressed(pianoKey);
				this->callback->PianoKeyboardNoteReleased(this, pianoKey);
				mutex->Unlock();
			}
		}
	}
}

void CPianoKeyboard::AddDefaultKeyCodes()
{
	// 0 = C-0
	notesKeyCodes.push_back(new CPianoNoteKeyCode('z', 0));		// C-0
	notesKeyCodes.push_back(new CPianoNoteKeyCode('s', 1));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('x', 2));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('d', 3));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('c', 4));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('v', 5));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('g', 6));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('b', 7));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('h', 8));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('n', 9));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('j', 10));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('m', 11));
	notesKeyCodes.push_back(new CPianoNoteKeyCode(',', 12));	// C-1
	notesKeyCodes.push_back(new CPianoNoteKeyCode('l', 13));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('.', 14));
	notesKeyCodes.push_back(new CPianoNoteKeyCode(';', 15));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('/', 16));
	
	notesKeyCodes.push_back(new CPianoNoteKeyCode('q', 12));	// C-1
	notesKeyCodes.push_back(new CPianoNoteKeyCode('2', 13));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('w', 14));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('3', 15));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('e', 16));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('r', 17));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('5', 18));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('t', 19));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('6', 20));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('y', 21));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('7', 22));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('u', 23));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('i', 24));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('9', 25));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('o', 26));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('0', 27));
	notesKeyCodes.push_back(new CPianoNoteKeyCode('p', 28));
	
}

CPianoKeyboard::~CPianoKeyboard()
{
	while(!pianoKeys.empty())
	{
		CPianoKey *key = pianoKeys.back();
		pianoKeys.pop_back();
		
		delete key;
	}

	while(!notesKeyCodes.empty())
	{
		CPianoNoteKeyCode *keyCode = notesKeyCodes.back();
		notesKeyCodes.pop_back();
		
		delete keyCode;
	}
}

// UI
bool CPianoKeyboard::HasContextMenuItems()
{
	return true;
}

void CPianoKeyboard::RenderContextMenuItems()
{
	// TODO: add derived from base layout parameter that is rendering proper menu for midi in (yes, they are bound to the emu)
	if (ImGui::BeginMenu("MIDI In"))
	{
		int p = 0;
		std::list<char *> *midiInPort = CMidiInKeyboard::EnumerateAvailablePorts();
		int numPorts = midiInPort->size();
		
//		if (numPorts > 5)
//		{
			//		if (ImGui::BeginMenu("Port"))
//		{
			
			for (std::list<char *>::iterator it = midiInPort->begin(); it != midiInPort->end(); )
			{
				char *strMidiInPort = *it;
				bool checked = (p == midiPortNum);
				if (ImGui::MenuItem(strMidiInPort, NULL, &checked))
				{
					LOGD("SELECTED %s", strMidiInPort);
					midiInKeyboard->midiIn->closePort();
					midiInKeyboard->midiIn->openPort(p);
					midiPortNum = p;
				}
				p++;
				it++;
				STRFREE(strMidiInPort);
			}
			
			delete midiInPort;
			
//			ImGui::EndMenu();
//		}
		
		if (numPorts == 0)
		{
			ImGui::MenuItem("No MIDI inputs available", NULL, false, false);
		}
		
		if (numPorts > 0)
		{
			ImGui::Separator();

			if (ImGui::InputInt("Channel", &midiChannel))
			{
			}
		}
		
		ImGui::EndMenu();
	}
	ImGui::Separator();
}

