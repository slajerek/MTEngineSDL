#ifndef __MT_KEY_CODES_H__
#define __MT_KEY_CODES_H__

#include "GUI_Main.h"

#define MTKEY_NOTHING			0x00

#define MTKEY_BACKSPACE			0x08
#define MTKEY_TAB				0x09
#define MTKEY_ENTER				0x0D
#define MTKEY_SPACEBAR			0x20
#define MTKEY_LEFT_APOSTROPHE	0x60
#define MTKEY_RIGHT_APOSTROPHE	0xAB
#define MTKEY_TILDE				0x7E

// ¨	utf code is C2A8, but 0X00 is reserved for MTKEY_SPECIAL. TODO: change MTKEY_SPECIAL to X0000000
#define MTKEY_UMLAUT			0xC0A8

#define MTKEY_SPECIAL_KEYS_START	0xF000
#define MTKEY_ARROW_LEFT		SDLK_LEFT
#define MTKEY_ARROW_RIGHT		SDLK_RIGHT
#define MTKEY_ARROW_UP			SDLK_UP
#define MTKEY_ARROW_DOWN		SDLK_DOWN
#define MTKEY_DELETE			SDLK_DELETE
#define MTKEY_HARDWARE_BACK		0xF006
#define MTKEY_PAGE_DOWN			SDLK_PAGEDOWN
#define MTKEY_PAGE_UP			SDLK_PAGEUP
#define MTKEY_INSERT			SDLK_INSERT
#define MTKEY_HOME				SDLK_HOME
#define MTKEY_END				SDLK_END
#define MTKEY_PRINT_SCREEN		SDLK_PRINTSCREEN
#define MTKEY_PAUSE_BREAK		SDLK_PAUSE

#define MTKEY_LSHIFT			SDLK_LSHIFT
#define MTKEY_RSHIFT			SDLK_RSHIFT
#define MTKEY_LALT				SDLK_LALT
#define MTKEY_RALT				SDLK_RALT

// TODO: fix me
#if defined(MACOS)
#define MTKEY_LCONTROL			SDLK_LGUI
#define MTKEY_RCONTROL			SDLK_RGUI
#define MTKEY_LSUPER			SDLK_LCTRL
#define MTKEY_RSUPER			SDLK_RCTRL
#else
#define MTKEY_LCONTROL			SDLK_LCTRL
#define MTKEY_RCONTROL			SDLK_RCTRL
#define MTKEY_LSUPER			SDLK_LGUI
#define MTKEY_RSUPER			SDLK_RGUI
#endif

#define MTKEY_CAPS_LOCK			SDLK_CAPSLOCK

#define MTKEY_NUM_0				SDLK_KP_0
#define MTKEY_NUM_1				SDLK_KP_1
#define MTKEY_NUM_2				SDLK_KP_2
#define MTKEY_NUM_3				SDLK_KP_3
#define MTKEY_NUM_4				SDLK_KP_4
#define MTKEY_NUM_5				SDLK_KP_5
#define MTKEY_NUM_6				SDLK_KP_6
#define MTKEY_NUM_7				SDLK_KP_7
#define MTKEY_NUM_8				SDLK_KP_8
#define MTKEY_NUM_9				SDLK_KP_9
#define MTKEY_NUM_LOCK			SDLK_NUMLOCKCLEAR
#define MTKEY_NUM_EQUAL			SDLK_KP_EQUALS
#define MTKEY_NUM_DIVIDE		SDLK_KP_DIVIDE
#define MTKEY_NUM_MULTIPLY		SDLK_KP_MULTIPLY
#define MTKEY_NUM_MINUS			SDLK_KP_MINUS
#define MTKEY_NUM_PLUS			SDLK_KP_PLUS
#define MTKEY_NUM_DELETE		SDLK_KP_CLEAR
#define MTKEY_NUM_ENTER			SDLK_KP_ENTER
#define MTKEY_NUM_DOT			SDLK_KP_PERIOD

#define MTKEY_F1				SDLK_F1
#define MTKEY_F2				SDLK_F2
#define MTKEY_F3				SDLK_F3
#define MTKEY_F4				SDLK_F4
#define MTKEY_F5				SDLK_F5
#define MTKEY_F6				SDLK_F6
#define MTKEY_F7				SDLK_F7
#define MTKEY_F8				SDLK_F8
#define MTKEY_F9				SDLK_F9
#define MTKEY_F10				SDLK_F10
#define MTKEY_F11				SDLK_F11
#define MTKEY_F12				SDLK_F12
#define MTKEY_F13				SDLK_F13
#define MTKEY_F14				SDLK_F14
#define MTKEY_F15				SDLK_F15
#define MTKEY_F16				SDLK_F16

#define MTKEY_ESC				SDLK_ESCAPE

#define MTKEY_SPECIAL_SHIFT		0x0100
#define MTKEY_SPECIAL_CONTROL	0x0200
#define MTKEY_SPECIAL_ALT		0x0400
#define MTKEY_SPECIAL_SUPER		0x0800

class CSlrString;

CSlrString *SYS_KeyName(u32 keyCode);
CSlrString *SYS_KeyUpperCodeToString(u32 keyCode);
CSlrString *SYS_KeyCodeToString(u32 keyCode);
CSlrString *SYS_KeyCodeToString(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

u32 SYS_KeyCodeConvertSpecial(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

bool SYS_IsKeyCodeSpecial(u32 keyCode);

#endif
//
