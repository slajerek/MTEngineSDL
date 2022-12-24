#ifndef _CMidiInKeyboard_h_
#define _CMidiInKeyboard_h_

#include "SYS_Defs.h"

#include "RtMidi.h"

#include <list>

class CSlrString;

class CMidiInKeyboardCallback
{
public:
	virtual void MidiInKeyboardCallbackNoteOn(int channel, int key, int pressure);
	virtual void MidiInKeyboardCallbackNoteOff(int channel, int key, int pressure);
	virtual void MidiInKeyboardCallbackControlChange(int knobNum, int value);
	virtual void MidiInKeyboardCallbackPitchBend(int value);
};

class CMidiInKeyboard
{
public:
	CMidiInKeyboard(int portNum, CMidiInKeyboardCallback *callback);
	~CMidiInKeyboard();
	
	void Init(int portNum, CMidiInKeyboardCallback *callback);
	void Init(int portNum);

	RtMidiIn *midiIn;
	CMidiInKeyboardCallback *callback;
	
	CSlrString *errorString;

	char deviceName[512];
	static std::list<CSlrString *> *EnumerateAvailablePorts(CSlrString **errorString);
	static std::list<char *> *EnumerateAvailablePorts();
};

#endif
