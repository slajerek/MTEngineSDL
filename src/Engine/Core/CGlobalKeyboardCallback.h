#ifndef _KEYBOARD_CALLBACK_
#define _KEYBOARD_CALLBACK_

class CGlobalKeyboardCallback
{
public:
	virtual bool GlobalKeyDownCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool GlobalKeyUpCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool GlobalKeyPressCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool GlobalKeyTextInputCallback(const char *text);
};


#endif
