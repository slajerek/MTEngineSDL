#include "CSlrKeyboardShortcuts.h"
#include "CSlrString.h"
#include "SYS_KeyCodes.h"
#include "SYS_Main.h"
#include "CByteBuffer.h"

CSlrKeyboardShortcutsZone::CSlrKeyboardShortcutsZone(u32 zoneId)
{
	this->zoneId = zoneId;
	shortcutsByKeycode = new std::map<i32, std::list<CSlrKeyboardShortcut *> *>();
	shortcutByHashcode = new std::map<u64, CSlrKeyboardShortcut *>();
}

void CSlrKeyboardShortcutsZone::AddShortcut(CSlrKeyboardShortcut *shortcutToAdd)
{
	if (shortcutToAdd->keyCode > 0)
	{
		std::list<CSlrKeyboardShortcut *> *listOfShortcuts = NULL;
		std::map<i32, std::list<CSlrKeyboardShortcut *> *>::iterator itListOfShortcuts = this->shortcutsByKeycode->find(shortcutToAdd->keyCode);
		if (itListOfShortcuts != this->shortcutsByKeycode->end())
		{
			listOfShortcuts = itListOfShortcuts->second;
		}
		else
		{
			LOGG("...add shortcut keycode %d name=%s %d %d %d",
				 shortcutToAdd->keyCode, shortcutToAdd->name, shortcutToAdd->isShift, shortcutToAdd->isAlt, shortcutToAdd->isControl);
			
			CSlrString *keyCodeName = SYS_KeyCodeToString(shortcutToAdd->keyCode, shortcutToAdd->isShift, shortcutToAdd->isAlt, shortcutToAdd->isControl, shortcutToAdd->isSuper);
//			keyCodeName->DebugPrint("keyCode=");
			delete keyCodeName;
			
			listOfShortcuts = new std::list<CSlrKeyboardShortcut *>();
			(*(this->shortcutsByKeycode))[shortcutToAdd->keyCode] = listOfShortcuts;
		}
		
		// check if exists
		for (std::list<CSlrKeyboardShortcut *>::iterator it = listOfShortcuts->begin(); it != listOfShortcuts->end(); it++)
		{
			CSlrKeyboardShortcut *shortcut = *it;
			if (shortcut->isShift == shortcutToAdd->isShift
				&& shortcut->isAlt == shortcutToAdd->isAlt
				&& shortcut->isControl == shortcutToAdd->isControl
				&& shortcut->isSuper == shortcutToAdd->isSuper)
			{
				LOGError("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
				LOGError("!!");
				LOGError("!!  CSlrKeyboardShortcuts::AddShortcut: shortcut %4.4x already exists in list", shortcutToAdd->keyCode);
				LOGError("Existing shortcut:");
				shortcut->DebugPrint();
				LOGError("NOT added shortcut:");
				shortcutToAdd->DebugPrint();
				LOGError("!!");
				LOGError("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
				return;
				
				listOfShortcuts->remove(shortcut);
				this->shortcutByHashcode->erase(shortcutToAdd->hashCode);
				break;
			}
		}
		
		listOfShortcuts->push_back(shortcutToAdd);
	}
	
	(*(this->shortcutByHashcode))[shortcutToAdd->hashCode] = shortcutToAdd;
}

void CSlrKeyboardShortcutsZone::RemoveShortcut(CSlrKeyboardShortcut *shortcutToRemove)
{
	//this->shortcutByHashcode->erase(shortcutToRemove->hashCode);

	if (shortcutToRemove->keyCode > 0)
	{
		std::map<i32, std::list<CSlrKeyboardShortcut *> *>::iterator itListOfShortcuts = this->shortcutsByKeycode->find(shortcutToRemove->keyCode);
		if (itListOfShortcuts != this->shortcutsByKeycode->end())
		{
			std::list<CSlrKeyboardShortcut *> *listOfShortcuts = itListOfShortcuts->second;
			
			for (std::list<CSlrKeyboardShortcut *>::iterator it = listOfShortcuts->begin(); it != listOfShortcuts->end(); it++)
			{
				CSlrKeyboardShortcut *shortcut = *it;
				if (shortcut->isShift == shortcutToRemove->isShift
					&& shortcut->isAlt == shortcutToRemove->isAlt
					&& shortcut->isControl == shortcutToRemove->isControl
					&& shortcut->isSuper == shortcutToRemove->isSuper)
				{
					listOfShortcuts->remove(shortcut);
					if (listOfShortcuts->empty())
					{
						this->shortcutsByKeycode->erase(shortcutToRemove->keyCode);
						delete listOfShortcuts;
					}

					return;
				}
			}
		}
	}
}

CSlrKeyboardShortcut *CSlrKeyboardShortcutsZone::FindShortcut(i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	std::map<i32, std::list<CSlrKeyboardShortcut *> *>::iterator itListOfShortcuts = this->shortcutsByKeycode->find(keyCode);
	if (itListOfShortcuts != this->shortcutsByKeycode->end())
	{
		//LOGD("... found keyCode");
		std::list<CSlrKeyboardShortcut *> *listOfShortcuts = itListOfShortcuts->second;
		
		for (std::list<CSlrKeyboardShortcut *>::iterator it = listOfShortcuts->begin(); it != listOfShortcuts->end(); it++)
		{
			CSlrKeyboardShortcut *shortcut = *it;
			if (shortcut->isShift == isShift
				&& shortcut->isAlt == isAlt
				&& shortcut->isControl == isControl
				&& shortcut->isSuper == isSuper)
			{
				LOGI("... found shortcut");
				return shortcut;
			}
		}
	}
	return NULL;
}

CSlrKeyboardShortcut *CSlrKeyboardShortcutsZone::FindShortcut(u64 hashCode)
{
	std::map<u64, CSlrKeyboardShortcut *>::iterator it = this->shortcutByHashcode->find(hashCode);
	if (it != this->shortcutByHashcode->end())
	{
		return it->second;
	}
	
	return NULL;
}

void CSlrKeyboardShortcutsZone::StoreToByteBuffer(CByteBuffer *byteBuffer)
{
	byteBuffer->PutU32(shortcutByHashcode->size());
	
	for (std::map<u64, CSlrKeyboardShortcut *>::iterator it = this->shortcutByHashcode->begin();
		 it != this->shortcutByHashcode->end(); it++)
	{
		CSlrKeyboardShortcut *shortcut = it->second;
		byteBuffer->PutU64(shortcut->hashCode);
		byteBuffer->PutI32(shortcut->keyCode);
		byteBuffer->PutBool(shortcut->isShift);
		byteBuffer->PutBool(shortcut->isAlt);
		byteBuffer->PutBool(shortcut->isControl);
		byteBuffer->PutBool(shortcut->isSuper);
	}
}

void CSlrKeyboardShortcutsZone::LoadFromByteBuffer(CByteBuffer *byteBuffer)
{
	LOGD(".... zone=%d", this->zoneId);
	
	// delete all shortcuts mapping
	while(!this->shortcutsByKeycode->empty())
	{
		std::map<i32, std::list<CSlrKeyboardShortcut *> *>::iterator itListOfShortcuts = this->shortcutsByKeycode->begin();
		
		i32 keyCode = itListOfShortcuts->first;
		std::list<CSlrKeyboardShortcut *> *listOfShortcuts = itListOfShortcuts->second;
		
		this->shortcutsByKeycode->erase(keyCode);
		delete listOfShortcuts;
	}

	// create new map
	std::map<u64, CSlrKeyboardShortcut *> *oldShortcutsByHashcode = shortcutByHashcode;
	shortcutByHashcode = new std::map<u64, CSlrKeyboardShortcut *>();

	u32 numShortcuts = byteBuffer->GetU32();
	
	for (u32 i = 0; i < numShortcuts; i++)
	{
		u64 hashCode = byteBuffer->GetU64();
		i32 keyCode = byteBuffer->GetI32();
		bool isShift = byteBuffer->GetBool();
		bool isAlt = byteBuffer->GetBool();
		bool isControl = byteBuffer->GetBool();
		bool isSuper = byteBuffer->GetBool();
		
		std::map<u64, CSlrKeyboardShortcut *>::iterator it = oldShortcutsByHashcode->find(hashCode);
		if (it == oldShortcutsByHashcode->end())
		{
			// removed shortcut?
			LOGError("CSlrKeyboardShortcutsZone::LoadFromByteBuffer: shortcut hashCode=%x not found", hashCode);
			continue;
		}

		CSlrKeyboardShortcut *shortcut = it->second;
		
		// remove from old shortcuts
		oldShortcutsByHashcode->erase(it);
		
		// check if this shortcut does not conflict with existing one
		CSlrKeyboardShortcut *conflictingShortcut = FindShortcut(keyCode, isShift, isAlt, isControl, isSuper);
		if (conflictingShortcut != NULL)
		{
			LOGError("CSlrKeyboardShortcutsZone::LoadFromByteBuffer: shortcut hashCode=%x is conflicting with shortcut %x (%s), skipping", hashCode,
					 conflictingShortcut->hashCode, conflictingShortcut->name);
			
			shortcut->keyCode = -1;
			(*(this->shortcutByHashcode))[hashCode] = shortcut;
			continue;
		}
		
		// add shortcut
		shortcut->SetKeyCode(keyCode, isShift, isAlt, isControl, isSuper);
		this->AddShortcut(shortcut);
	}
	
	// add remaining shortcuts (new added after recent application update, not yet in configuration?)
	for (std::map<u64, CSlrKeyboardShortcut *>::iterator it = oldShortcutsByHashcode->begin();
		 it != oldShortcutsByHashcode->end(); it++)
	{
		CSlrKeyboardShortcut *shortcut = it->second;
		
		// check if this shortcut does not conflict with existing one
		CSlrKeyboardShortcut *conflictingShortcut = FindShortcut(shortcut->keyCode,
																 shortcut->isShift, shortcut->isAlt, shortcut->isControl, shortcut->isSuper);
		if (conflictingShortcut != NULL)
		{
			LOGError("CSlrKeyboardShortcutsZone::LoadFromByteBuffer: shortcut hashCode=%x is conflicting with shortcut %x (%s), skipping", shortcut->hashCode,
					 conflictingShortcut->hashCode, conflictingShortcut->name);
			
			shortcut->keyCode = -1;
			(*(this->shortcutByHashcode))[shortcut->hashCode] = shortcut;
			continue;
		}
		
		this->AddShortcut(shortcut);
	}
	
	delete oldShortcutsByHashcode;
}


CSlrKeyboardShortcuts::CSlrKeyboardShortcuts()
{
	mapOfZones = new std::map<u32, CSlrKeyboardShortcutsZone *>();
}

void CSlrKeyboardShortcuts::AddShortcut(CSlrKeyboardShortcut *shortcutToAdd)
{
	LOGI("CSlrKeyboardShortcuts::AddShortcut: keyCode=%4.4x isShift=%d isAlt=%d isControl=%d", shortcutToAdd->keyCode, shortcutToAdd->isShift, shortcutToAdd->isAlt, shortcutToAdd->isControl);
	
	CSlrKeyboardShortcutsZone *shortcutsZone = NULL;
	
	std::map<u32, CSlrKeyboardShortcutsZone *>::iterator itShortcuts = mapOfZones->find(shortcutToAdd->zone);
	if (itShortcuts != mapOfZones->end())
	{
		shortcutsZone = itShortcuts->second;
		
		// check if hashcode exists
		std::map<u64, CSlrKeyboardShortcut *>::iterator itHashcode = shortcutsZone->shortcutByHashcode->find(shortcutToAdd->hashCode);
		if (itHashcode != shortcutsZone->shortcutByHashcode->end())
		{
			CSlrKeyboardShortcut *shortcut = itHashcode->second;

			LOGError("CSlrKeyboardShortcuts::AddShortcut: duplicated shortcut hashcode %x ('%s' == '%s')", shortcutToAdd->hashCode,
						  shortcut->name, shortcutToAdd->name);

			// this is due to broken macOS Xcode debugger that does not respect above breakpoint properly.
			static int a = 0; a++;
			return;
		}
	}
	else
	{
//		LOGD("...create zone %d", shortcutToAdd->zone);
		shortcutsZone = new CSlrKeyboardShortcutsZone(shortcutToAdd->zone);
		(*mapOfZones)[shortcutToAdd->zone] = shortcutsZone;
	}
	
	shortcutsZone->AddShortcut(shortcutToAdd);
	
	shortcutsZone->shortcuts.push_back(shortcutToAdd);
}

void CSlrKeyboardShortcuts::RemoveShortcut(CSlrKeyboardShortcut *shortcutToRemove)
{
	LOGI("CSlrKeyboardShortcuts::RemoveShortcut: keyCode=%4.4x isShift=%d isAlt=%d isControl=%d isSuper=%d", shortcutToRemove->keyCode, shortcutToRemove->isShift, shortcutToRemove->isAlt, shortcutToRemove->isControl, shortcutToRemove->isSuper);

	std::map< u32, CSlrKeyboardShortcutsZone *>::iterator itZone = mapOfZones->find(shortcutToRemove->zone);
	if (itZone != mapOfZones->end())
	{
		CSlrKeyboardShortcutsZone *zone = itZone->second;
		zone->RemoveShortcut(shortcutToRemove);
	}
}

void CSlrKeyboardShortcuts::LoadFromByteBuffer(CByteBuffer *byteBuffer)
{
	u32 numZones = byteBuffer->GetU32();
	
	for (u32 i = 0; i < numZones; i++)
	{
		u32 zoneId = byteBuffer->GetU32();
		
		std::map< u32, CSlrKeyboardShortcutsZone *>::iterator itZone = mapOfZones->find(zoneId);
		
		if (itZone == mapOfZones->end())
		{
			LOGError("CSlrKeyboardShortcuts::LoadFromByteBuffer: zoneId=%d not found", zoneId);
			// fail :(
			return;
		}
		
		CSlrKeyboardShortcutsZone *zone = itZone->second;
		zone->LoadFromByteBuffer(byteBuffer);
	}
}

void CSlrKeyboardShortcuts::StoreToByteBuffer(CByteBuffer *byteBuffer)
{
	byteBuffer->PutU32(mapOfZones->size());
	for (std::map< u32, CSlrKeyboardShortcutsZone *>::iterator itZone = mapOfZones->begin();
		 itZone != mapOfZones->end(); itZone++)
	{
		CSlrKeyboardShortcutsZone *zone = itZone->second;
		byteBuffer->PutU32(zone->zoneId);
		zone->StoreToByteBuffer(byteBuffer);
	}
}


///

CSlrKeyboardShortcut *CSlrKeyboardShortcuts::FindShortcut(std::list<u32> zones, i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	LOGI("CSlrKeyboardShortcuts::FindShortcut: keyCode=%4.4x isShift=%d isAlt=%d isControl=%d isSuper=%d", keyCode, isShift, isAlt, isControl, isSuper);

	for (std::list<u32>::iterator itZone = zones.begin(); itZone != zones.end(); itZone++)
	{
		u32 zone = *itZone;
		
		CSlrKeyboardShortcut *shortcut = FindShortcut(zone, keyCode, isShift, isAlt, isControl, isSuper);
		if (shortcut != NULL)
		{
			return shortcut;
		}
	}

	LOGI("... global shortcut not found");
	return NULL;
}

CSlrKeyboardShortcut *CSlrKeyboardShortcuts::FindShortcut(u32 zone, i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	std::map< u32, CSlrKeyboardShortcutsZone *>::iterator itZone = mapOfZones->find(zone);

	if (itZone != mapOfZones->end())
	{
		//LOGD("... found zone");
		CSlrKeyboardShortcutsZone *zone = itZone->second;
		
		return zone->FindShortcut(keyCode, isShift, isAlt, isControl, isSuper);
	}
	
	return NULL;
}

CSlrKeyboardShortcut *CSlrKeyboardShortcuts::FindGlobalShortcut(i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return FindShortcut(MT_KEYBOARD_SHORTCUT_GLOBAL, keyCode, isShift, isAlt, isControl, isSuper);
}

CSlrKeyboardShortcut::CSlrKeyboardShortcut(CByteBuffer *serializedBuffer, CSlrKeyboardShortcutCallback *callback)
{
	this->callback = callback;
	this->userData = NULL;
	this->Deserialize(serializedBuffer);
}

CSlrKeyboardShortcut::CSlrKeyboardShortcut(u32 zone, const char *name, i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper,
										   CSlrKeyboardShortcutCallback *callback)
{
	this->zone = zone;
	this->name = STRALLOC(name);
	this->callback = callback;
	
	this->hashCode = GetHashCode64(name);

	this->userData = NULL;

	this->str = NULL;
	this->cstr = NULL;
	
	SetKeyCode(keyCode, isShift, isAlt, isControl, isSuper);
}

CSlrKeyboardShortcut::~CSlrKeyboardShortcut()
{
	STRFREE(this->name);
}

void CSlrKeyboardShortcut::SetName(char *name)
{
	if (this->name)
		STRFREE(this->name);
	
	this->name = STRALLOC(name);
}

void CSlrKeyboardShortcut::SetKeyCode(i32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	this->keyCode = keyCode;
	this->isShift = isShift;
	this->isAlt = isAlt;
	this->isControl = isControl;
	this->isSuper = isSuper;

	if (str != NULL)
	{
		delete this->str;
		str = NULL;
		
		delete [] cstr;
		cstr = NULL;
	}
	
	if (keyCode > 0)
	{
		str = SYS_KeyCodeToString(keyCode, isShift, isAlt, isControl, isSuper);
		cstr = str->GetUTF8();
	}
}

void CSlrKeyboardShortcut::Run()
{
	if (this->callback != NULL)
	{
		this->callback->ProcessKeyboardShortcut(zone, MT_ACTION_TYPE_MENU, this);
	}
}

void CSlrKeyboardShortcut::DebugPrint()
{
	LOGD("CSlrKeyboardShortcut::DebugPrint: zone=%d keyCode=%4.4x name='%s' hashCode=%x addr=%x", zone, keyCode, name, hashCode, this);
	if (str != NULL)
		str->DebugPrint("CSlrKeyboardShortcut::DebugPrint:");
}

void CSlrKeyboardShortcut::Serialize(CByteBuffer *byteBuffer)
{
	byteBuffer->PutU32(zone);
	byteBuffer->PutString(name);
	byteBuffer->PutI32(keyCode);
	byteBuffer->PutBool(isShift);
	byteBuffer->PutBool(isAlt);
	byteBuffer->PutBool(isControl);
	byteBuffer->PutBool(isSuper);
}

void CSlrKeyboardShortcut::Deserialize(CByteBuffer *byteBuffer)
{
	this->zone = byteBuffer->GetU32();
	this->name = byteBuffer->GetString();
	this->hashCode = GetHashCode64(name);

	this->str = NULL;
	this->cstr = NULL;
	
	i32 keyCode = byteBuffer->GetI32();
	bool isShift = byteBuffer->GetBool();
	bool isAlt = byteBuffer->GetBool();
	bool isControl = byteBuffer->GetBool();
	bool isSuper = byteBuffer->GetBool();
	
	SetKeyCode(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CSlrKeyboardShortcutCallback::ProcessKeyboardShortcut(u32 zone, u8 actionType, CSlrKeyboardShortcut *keyboardShortcut)
{
	LOGError("CSlrKeyboardShortcutCallback::ProcessKeyboardShortcut: not implemented for keyboardShortcut=%s", keyboardShortcut->name);
	return false;
}

