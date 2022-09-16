#ifndef _CSLRKEYBOARDSHORTCUTS_H_
#define _CSLRKEYBOARDSHORTCUTS_H_

#include "SYS_Defs.h"
#include <map>
#include <list>

class CSlrString;
class CByteBuffer;
class CSlrKeyboardShortcutCallback;

#define MT_KEYBOARD_SHORTCUT_GLOBAL	0x01
#define KBZONE_GLOBAL				MT_KEYBOARD_SHORTCUT_GLOBAL

class CSlrKeyboardShortcut
{
public:
	CSlrKeyboardShortcut(CByteBuffer *serializedBuffer, CSlrKeyboardShortcutCallback *callback);
	CSlrKeyboardShortcut(u32 zone, const char *name, i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper, CSlrKeyboardShortcutCallback *callback);
	virtual ~CSlrKeyboardShortcut();

	u32 zone;
	char *name;
	void SetName(char *name);

	u64 hashCode;
	
	i32 keyCode;
	bool isShift;
	bool isAlt;
	bool isControl;	// note this is "cmd" in macOS
	bool isSuper; 	// aka "windows" or "control" key in macOS
	
	CSlrKeyboardShortcutCallback *callback;
	
	void SetKeyCode(i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	
	CSlrString *str;
	char *cstr;
	
	void *userData;
	
	virtual void Run();
	
	void DebugPrint();
	void Serialize(CByteBuffer *byteBuffer);
	void Deserialize(CByteBuffer *byteBuffer);
	
};

class CSlrKeyboardShortcutsZone
{
public:
	CSlrKeyboardShortcutsZone(u32 zoneId);
	u32 zoneId;
	
	std::map<i32, std::list<CSlrKeyboardShortcut *> *> *shortcutsByKeycode;
	std::map<u64, CSlrKeyboardShortcut *> *shortcutByHashcode;

	std::list<CSlrKeyboardShortcut *> shortcuts;
	
	void AddShortcut(CSlrKeyboardShortcut *shortcutToRemove);
	void RemoveShortcut(CSlrKeyboardShortcut *shortcutToRemove);
	CSlrKeyboardShortcut *FindShortcut(i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	CSlrKeyboardShortcut *FindShortcut(u64 hashCode);
	
	void LoadFromByteBuffer(CByteBuffer *byteBuffer);
	void StoreToByteBuffer(CByteBuffer *byteBuffer);

};

class CSlrKeyboardShortcuts
{
public:
	CSlrKeyboardShortcuts();
	
	
	// oh dear it looks like overkill, but I hope it's enough even for huge apps ;)
	// map of zones of map of keyCodes of list of shortcuts combinations (with alt/shift/ctrl)
	std::map<u32, CSlrKeyboardShortcutsZone *> *mapOfZones;

	void AddShortcut(CSlrKeyboardShortcut *shortcutToRemove);
	void RemoveShortcut(CSlrKeyboardShortcut *shortcutToRemove);

	CSlrKeyboardShortcut *FindShortcut(std::list<u32> zones, i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	CSlrKeyboardShortcut *FindShortcut(u32 zone, i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	CSlrKeyboardShortcut *FindGlobalShortcut(i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

	void LoadFromByteBuffer(CByteBuffer *byteBuffer);
	void StoreToByteBuffer(CByteBuffer *byteBuffer);
};

enum CSlrKeyboardShortcutActionType : u8
{
	MT_ACTION_TYPE_UNKNOWN = 0,
	MT_ACTION_TYPE_KEYBOARD_SHORTCUT,
	MT_ACTION_TYPE_MENU
};

class CSlrKeyboardShortcutCallback
{
public:
	virtual bool ProcessKeyboardShortcut(u32 zone, u8 actionType, CSlrKeyboardShortcut *keyboardShortcut);
};

#endif
