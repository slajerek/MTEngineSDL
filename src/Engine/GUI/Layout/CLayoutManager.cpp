#include "CLayoutManager.h"
#include "CSlrString.h"
#include "CByteBuffer.h"
#include "CGuiMain.h"
#include "CSlrKeyboardShortcuts.h"

CLayoutData::CLayoutData()
{
	this->layoutName = NULL;
	this->serializedLayoutBuffer = new CByteBuffer();
	this->doNotUpdateViewsPositions = false;
	this->keyShortcut = NULL;
	this->isFullScreenLayout = false;
	this->parentLayout = NULL;
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
}

CLayoutManager::CLayoutManager(CGuiMain *guiMain)
{
	this->guiMain = guiMain;
	this->currentLayout = NULL;
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
	
	if (layoutData->keyShortcut)
	{
		guiMain->AddKeyboardShortcut(layoutData->keyShortcut);
		layoutData->keyShortcut->userData = layoutData;
	}
}

void CLayoutManager::RemoveAndDeleteLayout(CLayoutData *layoutData)
{
	if (currentLayout == layoutData)
	{
		currentLayout = NULL;
	}
	
	layouts.remove(layoutData);

	u64 hash = GetHashCode64(layoutData->layoutName);
	layoutsByHash.erase(hash);
	
	delete layoutData;
}

void CLayoutManager::DeleteAllLayouts()
{
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
}

void CLayoutManager::SerializeLayouts(CByteBuffer *byteBuffer)
{
	byteBuffer->PutU32(layouts.size());
	for (std::list<CLayoutData *>::iterator it = layouts.begin(); it != layouts.end(); it++)
	{
		CLayoutData *layoutData = *it;
		SerializeLayout(layoutData, byteBuffer);
	}
	
	// TODO: bug: after returning from full screen the current layoutName is NULL
	if (currentLayout && currentLayout->layoutName)
	{
		byteBuffer->PutString(currentLayout->layoutName);
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
	if (layoutData != NULL)
	{
		SetLayoutAsync(layoutData, false);
//		CUiThreadTaskSetLayout *task = new CUiThreadTaskSetLayout(layoutData, false);
//		guiMain->AddUiThreadTask(task);
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

#define LAYOUTS_FILE_VERSION 0x0002

void CLayoutManager::StoreLayouts()
{
	CByteBuffer *byteBuffer = new CByteBuffer();
	
	byteBuffer->PutU8('L');
	byteBuffer->PutU8('T');
	byteBuffer->PutU16(LAYOUTS_FILE_VERSION);

	SerializeLayouts(byteBuffer);
	
	LOGG("LAYOUTS_FILE_NAME is set to=%s", C64D_LAYOUTS_FILE_NAME);
	CSlrString *fileName = new CSlrString(C64D_LAYOUTS_FILE_NAME);
//	fileName->DebugPrint("fileName=");
	byteBuffer->storeToSettings(fileName);
	delete fileName;
	
	delete byteBuffer;
}

void CLayoutManager::LoadLayouts()
{
	LOGD("CLayoutManager::LoadLayouts");
	CByteBuffer *byteBuffer = new CByteBuffer();

	CSlrString *fileName = new CSlrString(C64D_LAYOUTS_FILE_NAME);
	bool available = byteBuffer->loadFromSettings(fileName);
	delete fileName;
	
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
			if (version != LAYOUTS_FILE_VERSION)
			{
				LOGError("CLayoutManager::LoadLayouts: version %04x not supported", version);
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
			if (version != LAYOUTS_FILE_VERSION)
			{
				LOGError("CLayoutManager::LoadLayout: version %04x not supported", version);
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
