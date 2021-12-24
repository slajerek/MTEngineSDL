#include "CGuiEvent.h"

CGuiEvent::CGuiEvent()
{
    this->type = GUI_EVENT_TYPE_UNKNOWN;
}

CGuiEventKeyboard::CGuiEventKeyboard(GuiEventKeyboardState keyboardState, u32 mtKey, u32 systemKeyCode)
{
	this->type = GUI_EVENT_TYPE_KEYBOARD;
	this->keyboardState = keyboardState;
    this->mtKey = mtKey;
    this->systemKeyCode = systemKeyCode;
}

CGuiEventMouse::CGuiEventMouse(GuiEventMouseState mouseState, int x, int y)
{
	this->type = GUI_EVENT_TYPE_MOUSE;
	this->mouseState = mouseState;
    this->x = x;
    this->y = y;
}
