#include "CLayoutManager.h"
#include "CSlrString.h"
#include "CByteBuffer.h"
#include "CGuiMain.h"
#include "CSlrKeyboardShortcuts.h"
#include "VID_Main.h"
#include "SYS_CommandLine.h"

CSlrString *settingsPathToLayoutsFile = NULL;

CLayoutData::CLayoutData()
{
	this->layoutName = NULL;
	this->serializedLayoutBuffer = new CByteBuffer();
	this->doNotUpdateViewsPositions = false;
	this->keyShortcut = NULL;
	this->isFullScreenLayout = false;
	this->parentLayout = NULL;
	this->predefinedId = NULL;
	this->translationKey = NULL;
}

CLayoutData::CLayoutData(const char *layoutName, CByteBuffer *serializedLayout, bool doNotUpdateViewsPosition, CSlrKeyboardShortcut *keyShortcut)
{
	this->layoutName = STRALLOC(layoutName);
	this->serializedLayoutBuffer = serializedLayout;
	this->doNotUpdateViewsPositions = doNotUpdateViewsPosition;
	this->keyShortcut = keyShortcut;
	if (keyShortcut != NULL)
	{
		keyShortcut->userData = this;
	}
	this->isFullScreenLayout = false;
	this->parentLayout = NULL;
	this->predefinedId = NULL;
	this->translationKey = NULL;
}

CLayoutData::~CLayoutData()
{
	if (layoutName)
		STRFREE(layoutName);
	if (serializedLayoutBuffer)
		delete serializedLayoutBuffer;

	if (keyShortcut)
	{
		guiMain->RemoveKeyboardShortcut(keyShortcut);
		delete keyShortcut;
	}
	
	if (parentLayout)
		delete parentLayout;
	if (predefinedId)
		STRFREE(predefinedId);
	if (translationKey)
		STRFREE(translationKey);
}

CLayoutManager::CLayoutManager(CGuiMain *guiMain)
{
	this->guiMain = guiMain;
	this->currentLayout = NULL;

	// Parse --layouts-file command line option if not already set by the app
	if (settingsPathToLayoutsFile == NULL)
	{
		for (int i = 0; i < (int)sysCommandLineArguments.size() - 1; i++)
		{
			const char *arg = sysCommandLineArguments[i];
			if (!strcmp(arg, "--layouts-file") || !strcmp(arg, "-layouts-file") || !strcmp(arg, "layouts-file"))
			{
				settingsPathToLayoutsFile = new CSlrString(sysCommandLineArguments[i + 1]);
				break;
			}
		}
	}
}

void CLayoutManager::AddLayout(CLayoutData *layoutData)
{
	LOGD("CLayoutManager::AddLayout: %s", layoutData->layoutName);
	
	// check for existing layout with the same name
	CLayoutData *layout = GetLayoutByName(layoutData->layoutName);
	if (layout != NULL)
	{
		LOGD("CLayoutManager::AddLayout: removed existing layout %s", layoutData->layoutName);
		RemoveAndDeleteLayout(layout);
	}
	
	layouts.push_back(layoutData);

	u64 hash = GetHashCode64(layoutData->layoutName);
	layoutsByHash[hash] = layoutData;

	if (layoutData->predefinedId)
	{
		u64 idHash = GetHashCode64(layoutData->predefinedId);
		predefinedLayoutsById[idHash] = layoutData;
	}
	
	if (layoutData->keyShortcut)
	{
		guiMain->AddKeyboardShortcut(layoutData->keyShortcut);
		layoutData->keyShortcut->userData = layoutData;
	}
}

void CLayoutManager::RemoveAndDeleteLayout(CLayoutData *layoutData)
{
	bool wasCurrent = (currentLayout == layoutData);
	if (wasCurrent)
	{
		currentLayout = NULL;
	}

	layouts.remove(layoutData);

	u64 hash = GetHashCode64(layoutData->layoutName);
	layoutsByHash.erase(hash);

	if (layoutData->predefinedId)
	{
		u64 idHash = GetHashCode64(layoutData->predefinedId);
		predefinedLayoutsById.erase(idHash);
	}

	delete layoutData;

	// Fix #4/#5: ensure currentLayout is never left NULL
	if (wasCurrent)
	{
		if (layouts.empty())
		{
			// Fix #5: always keep at least one workspace
			CLayoutData *defaultLayout = new CLayoutData();
			defaultLayout->layoutName = STRALLOC("Default");
			defaultLayout->doNotUpdateViewsPositions = false;
			AddLayout(defaultLayout);
			currentLayout = defaultLayout;
			SerializeLayoutAsync(defaultLayout);
		}
		else
		{
			// Fix #4: pick next available layout
			currentLayout = layouts.front();
		}
	}
}

void CLayoutManager::DeleteAllLayouts()
{
	// Clear references to layouts that are about to be deleted.
	// Note: DeleteAllLayouts() is used by DeserializeLayouts(); layoutsByHash must be cleared
	// otherwise AddLayout() may find freed layouts and attempt to delete them again.
	currentLayout = NULL;
	layoutsByHash.clear();
	predefinedLayoutsById.clear();

	while(!layouts.empty())
	{
		CLayoutData *layoutData = layouts.front();
		layouts.pop_front();
		delete layoutData;
	}
}

void CLayoutManager::SerializeLayout(CLayoutData *layoutData, CByteBuffer *byteBuffer)
{
	byteBuffer->PutString(layoutData->layoutName);
	byteBuffer->PutByteBuffer(layoutData->serializedLayoutBuffer);
	byteBuffer->PutBool(layoutData->doNotUpdateViewsPositions);
	if (layoutData->keyShortcut)
	{
		byteBuffer->PutBool(true);
		layoutData->keyShortcut->Serialize(byteBuffer);
	}
	else
	{
		byteBuffer->PutBool(false);
	}

	// v3: predefinedId
	if (layoutData->predefinedId)
	{
		byteBuffer->PutBool(true);
		byteBuffer->PutString(layoutData->predefinedId);
	}
	else
	{
		byteBuffer->PutBool(false);
	}
}

void CLayoutManager::SerializeLayouts(CByteBuffer *byteBuffer)
{
	byteBuffer->PutU32(layouts.size());
	for (std::list<CLayoutData *>::iterator it = layouts.begin(); it != layouts.end(); it++)
	{
		CLayoutData *layoutData = *it;
		SerializeLayout(layoutData, byteBuffer);
	}
	
	// Fix #2: never write empty currentLayoutName — fall back to first layout
	if (currentLayout && currentLayout->layoutName)
	{
		byteBuffer->PutString(currentLayout->layoutName);
	}
	else if (!layouts.empty() && layouts.front()->layoutName)
	{
		byteBuffer->PutString(layouts.front()->layoutName);
	}
	else
	{
		byteBuffer->PutString("");
	}
}

CLayoutData *CLayoutManager::DeserializeLayout(CByteBuffer *byteBuffer, u16 version)
{
	char *layoutName = byteBuffer->GetString();
	CByteBuffer *serializedLayout = byteBuffer->GetByteBuffer();
	bool isStatic = byteBuffer->GetBool();
	CSlrKeyboardShortcut *keyShortcut = NULL;
	
	if (version >= 0x0002)
	{
		bool hasKeyShortcut = byteBuffer->GetBool();
		if (hasKeyShortcut)
		{
			keyShortcut = new CSlrKeyboardShortcut(byteBuffer, this);
		}
	}
	
	CLayoutData *layoutData = new CLayoutData(layoutName, serializedLayout, isStatic, keyShortcut);

	// v3: predefinedId
	if (version >= 0x0003)
	{
		bool hasPredefinedId = byteBuffer->GetBool();
		if (hasPredefinedId)
		{
			char *predefinedId = byteBuffer->GetString();
			layoutData->predefinedId = STRALLOC(predefinedId);
		}
	}

	return layoutData;
}

void CLayoutManager::DeserializeLayouts(CByteBuffer *byteBuffer, u16 version)
{
	DeleteAllLayouts();
	
	u32 numLayouts = byteBuffer->GetU32();
	for (u32 i = 0; i < numLayouts; i++)
	{
		CLayoutData *layoutData = DeserializeLayout(byteBuffer, version);
		AddLayout(layoutData);
	}
	
	char *currentLayoutName = byteBuffer->GetString();
	LOGD("currentLayoutName=%s", currentLayoutName);

	CLayoutData *layoutData = GetLayoutByName(currentLayoutName);

	// Fix #1: fall back to first layout when saved name is not found (e.g. empty or deleted)
	if (layoutData == NULL && !layouts.empty())
	{
		LOGD("CLayoutManager::DeserializeLayouts: currentLayoutName '%s' not found, falling back to first layout", currentLayoutName);
		layoutData = layouts.front();
	}

	if (layoutData != NULL)
	{
		SetLayoutAsync(layoutData, false);
	}
}

CLayoutData *CLayoutManager::GetLayoutByName(const char *name)
{
	u64 hash = GetHashCode64(name);
	std::map<u64, CLayoutData *>::iterator it = layoutsByHash.find(hash);
	if (it == layoutsByHash.end())
		return NULL;
	
	return it->second;
}

CLayoutData *CLayoutManager::GetPredefinedLayoutById(const char *predefinedId)
{
	u64 hash = GetHashCode64(predefinedId);
	std::map<u64, CLayoutData *>::iterator it = predefinedLayoutsById.find(hash);
	if (it == predefinedLayoutsById.end())
		return NULL;
	return it->second;
}

CLayoutData *CLayoutManager::AddPredefinedLayout(const char *predefinedId, const char *translationKey)
{
	LOGD("CLayoutManager::AddPredefinedLayout: id=%s translationKey=%s", predefinedId, translationKey);

	// Check if this predefined layout already exists (e.g. restored from layouts.dat)
	CLayoutData *existing = GetPredefinedLayoutById(predefinedId);
	if (existing)
	{
		LOGD("CLayoutManager::AddPredefinedLayout: found existing layout for id=%s", predefinedId);
		// Update translationKey (it's app-provided, not serialized)
		if (existing->translationKey)
			STRFREE(existing->translationKey);
		existing->translationKey = STRALLOC(translationKey);
		// Do NOT change position — preserve order from file
		return existing;
	}

	// Create new predefined layout with empty buffer.
	// First use will capture the view state via SwitchToPredefinedWorkspace.
	CLayoutData *layoutData = new CLayoutData();
	layoutData->predefinedId = STRALLOC(predefinedId);
	layoutData->translationKey = STRALLOC(translationKey);
	layoutData->layoutName = STRALLOC(predefinedId); // use predefinedId as layoutName
	layoutData->doNotUpdateViewsPositions = false;

	// Insert after the last predefined layout, before first user workspace.
	// This keeps registration order (Factions → Items → ... → ServerAdmin).
	auto insertPos = layouts.end();
	for (auto it = layouts.begin(); it != layouts.end(); ++it)
	{
		if (!(*it)->IsPredefined())
		{
			insertPos = it;
			break;
		}
	}
	layouts.insert(insertPos, layoutData);

	u64 nameHash = GetHashCode64(layoutData->layoutName);
	layoutsByHash[nameHash] = layoutData;

	u64 idHash = GetHashCode64(predefinedId);
	predefinedLayoutsById[idHash] = layoutData;

	return layoutData;
}

void CLayoutManager::RemovePredefinedLayout(const char *predefinedId)
{
	CLayoutData *layoutData = GetPredefinedLayoutById(predefinedId);
	if (layoutData)
	{
		RemoveAndDeleteLayout(layoutData);
	}
}

#define LAYOUTS_FILE_VERSION 0x0003

void CLayoutManager::StoreLayouts()
{
	if (gHeadlessMode)
		return;

	CByteBuffer *byteBuffer = new CByteBuffer();
	
	byteBuffer->PutU8('L');
	byteBuffer->PutU8('T');
	byteBuffer->PutU16(LAYOUTS_FILE_VERSION);

	SerializeLayouts(byteBuffer);
	
	if (settingsPathToLayoutsFile != NULL)
	{
		byteBuffer->storeToFile(settingsPathToLayoutsFile);
	}
	else
	{
		CSlrString *fileName = new CSlrString(C64D_LAYOUTS_FILE_NAME);
		byteBuffer->storeToSettings(fileName);
		delete fileName;
	}
	
	delete byteBuffer;
}

void CLayoutManager::LoadLayouts()
{
	LOGD("CLayoutManager::LoadLayouts");
	CByteBuffer *byteBuffer = new CByteBuffer();

	bool available;
	if (settingsPathToLayoutsFile != NULL)
	{
		available = byteBuffer->readFromFile(settingsPathToLayoutsFile);
	}
	else
	{
		CSlrString *fileName = new CSlrString(C64D_LAYOUTS_FILE_NAME);
		available = byteBuffer->loadFromSettings(fileName);
		delete fileName;
	}
	
	if (available)
	{
		u8 b1 = byteBuffer->GetU8();
		u8 b2 = byteBuffer->GetU8();
		
		if (b1 == 0x00)
		{
			// support old layouts file. TODO: remove me in next versions, this is old, legacy, not needed
			// it used to be here 4 bytes header, skip more 2 bytes
			byteBuffer->GetU8();
			byteBuffer->GetU8();

			// read proper header magic
			b1 = byteBuffer->GetU8();
			b2 = byteBuffer->GetU8();
		}
		
		if (b1 != 'L' || b2 != 'T')
		{
			LOGError("CLayoutManager::LoadLayouts: LT magic not found");
		}
		else
		{
			u16 version = byteBuffer->GetU16();
			if (version > LAYOUTS_FILE_VERSION)
			{
				LOGError("CLayoutManager::LoadLayouts: version %04x not supported (max %04x)", version, LAYOUTS_FILE_VERSION);
			}
			else
			{
				DeserializeLayouts(byteBuffer, version);
			}
		}
	}
	
	if (layouts.empty())
	{
		// create default layout
		CLayoutData *layoutData = new CLayoutData();
		SerializeLayoutAsync(layoutData);
		
		layoutData->layoutName = STRALLOC("Default");
		layoutData->doNotUpdateViewsPositions = false;
		guiMain->layoutManager->AddLayout(layoutData);
		guiMain->layoutManager->currentLayout = layoutData;
		
		// note, we do not need to overwrite layouts.dat file now, it will be anyway restored to default
//		guiMain->layoutManager->StoreLayouts();
	}
	
	delete byteBuffer;
}

void CLayoutManager::StoreLayout(CLayoutData *layoutData, CSlrString *filePath)
{
	CByteBuffer *byteBuffer = new CByteBuffer();
	
	byteBuffer->PutU8('L');
	byteBuffer->PutU8('T');
	byteBuffer->PutU16(LAYOUTS_FILE_VERSION);

	// just one layout
	byteBuffer->PutU32(1);

	SerializeLayout(layoutData, byteBuffer);
	
	byteBuffer->storeToFile(filePath);
	
	delete byteBuffer;
}

CLayoutData *CLayoutManager::LoadLayout(CSlrString *filePath)
{
	CByteBuffer *byteBuffer = new CByteBuffer();

	bool available = byteBuffer->readFromFile(filePath);
	
	if (available)
	{
		u8 b1 = byteBuffer->GetU8();
		u8 b2 = byteBuffer->GetU8();
		
		if (b1 != 'L' || b2 != 'T')
		{
			LOGError("CLayoutManager::LoadLayout: LT magic not found");
		}
		else
		{
			u16 version = byteBuffer->GetU16();
			if (version > LAYOUTS_FILE_VERSION)
			{
				LOGError("CLayoutManager::LoadLayout: version %04x not supported (max %04x)", version, LAYOUTS_FILE_VERSION);
			}
			else
			{
				u32 numLayouts = byteBuffer->GetU32(); // skip
				if (numLayouts != 1)
				{
					LOGWarning("CLayoutManager::LoadLayout: numLayouts in file is %d, loading first layout only", numLayouts);
				}
				CLayoutData *layoutData = DeserializeLayout(byteBuffer, version);
				delete byteBuffer;
				return layoutData;
			}
		}
	}
	
	// failed to load layout
	delete byteBuffer;
	return NULL;
}


//
void CLayoutManager::SerializeLayoutAsync(CLayoutData *layoutData)
{
	guiMain->LockMutex();
	guiMain->layoutStoreOrRestore = LayoutStorageTask::StoreLayout;
	guiMain->layoutForThisFrame = layoutData;
	guiMain->UnlockMutex();
}

void CLayoutManager::SetLayoutAsync(CLayoutData *layoutData, bool saveCurrentLayout)
{
	LOGD("CLayoutManager::SetLayoutAsync: %s | saveCurrentLayout=%s", layoutData ? layoutData->layoutName : "NULL", STRBOOL(saveCurrentLayout));
	guiMain->LockMutex();

	if (layoutData == NULL)
	{
		currentLayout = NULL;
	}
	else
	{
		if (saveCurrentLayout == false)
		{
			currentLayout = NULL;
		}
		
		layoutData->serializedLayoutBuffer->Rewind();
		guiMain->layoutStoreOrRestore = LayoutStorageTask::RestoreLayout;
		guiMain->layoutForThisFrame = layoutData;
	}
	guiMain->UnlockMutex();
}

bool CLayoutManager::ProcessKeyboardShortcut(u32 zone, u8 actionType, CSlrKeyboardShortcut *keyboardShortcut)
{
	LOGD("CLayoutManager::ProcessKeyboardShortcut: keyboardShortcut=%x", keyboardShortcut);
	
	if (!keyboardShortcut->userData)
	{
		LOGError("CLayoutManager::ProcessKeyboardShortcut: no layout");
		return false;
	}
	
	CLayoutData *layoutData = (CLayoutData *)keyboardShortcut->userData;
	SetLayoutAsync(layoutData, true);
	
//	guiMain->ShowNotification("Layout restored", layoutData->layoutName);
	
	return true;
}
