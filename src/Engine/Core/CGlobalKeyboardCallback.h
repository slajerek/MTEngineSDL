#ifndef _KEYBOARD_CALLBACK_
#define _KEYBOARD_CALLBACK_

class CGlobalKeyboardCallback
{
public:
	// before parsing key events, sent always
	virtual void GlobalPreKeyDownCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual void GlobalPreKeyUpCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

	// normal event parsing pipeline (i.e. can be consumed earlier)
	virtual bool GlobalKeyDownCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool GlobalKeyUpCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool GlobalKeyPressCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool GlobalKeyTextInputCallback(const char *text);
};


#endif
