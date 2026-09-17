#ifndef _CGUIEVENT_H_
#define _CGUIEVENT_H_

#include "SYS_Defs.h"

enum GuiEventTypes : u8
{
    GUI_EVENT_TYPE_UNKNOWN,
    GUI_EVENT_TYPE_KEYBOARD,
    GUI_EVENT_TYPE_MOUSE,
    GUI_EVENT_TYPE_JOYSTICK,
    GUI_EVENT_TYPE_MULTITOUCH
};

enum GuiEventKeyboardState : u8
{
	GUI_EVENT_KEYBOARD_KEY_DOWN,
	GUI_EVENT_KEYBOARD_KEY_UP,
	GUI_EVENT_KEYBOARD_KEY_REPEAT
};

enum GuiEventMouseState : u8
{
	GUI_EVENT_MOUSE_LEFT_BUTTON_DOWN,
	GUI_EVENT_MOUSE_LEFT_BUTTON_UP,
	GUI_EVENT_MOUSE_RIGHT_BUTTON_DOWN,
	GUI_EVENT_MOUSE_RIGHT_BUTTON_UP,
	GUI_EVENT_MOUSE_MID_BUTTON_DOWN,
	GUI_EVENT_MOUSE_MID_BUTTON_UP,
	GUI_EVENT_MOUSE_MOVE
};


// generic classes for events, not used widely
//
// The destructor is virtual because these are stored and deleted
// polymorphically: queues hold CGuiEvent* that actually point at
// CGuiEventKeyboard / CGuiEventMouse, and `delete event` on the base pointer
// is undefined behaviour without it -- C++14 sized deallocation hands
// operator delete the BASE size, so the allocator is told the wrong extent of
// the block. CViewC64GoatTracker::ForwardEvents() does exactly that on the
// GoatTracker thread, which is where an unexplained SIGSEGV was reported on
// 2026-09-08.
class CGuiEvent
{
    public:
        CGuiEvent();
        virtual ~CGuiEvent();
        u8 type;
};

class CGuiEventKeyboard : public CGuiEvent
{
public:
	CGuiEventKeyboard(GuiEventKeyboardState keyboardState, u32 mtKey, u32 systemKeyCode);
    u32 mtKey;
    u32 systemKeyCode;
	
	u8 keyboardState;
};

class CGuiEventMouse : public CGuiEvent
{
public:
    CGuiEventMouse(GuiEventMouseState mouseState, int x, int y);
	int x;
	int y;
	
	u8 mouseState;
};

#endif
