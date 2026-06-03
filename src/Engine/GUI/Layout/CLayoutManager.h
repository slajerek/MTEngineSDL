#ifndef _CLayoutManager_h_
#define _CLayoutManager_h_

#include "SYS_Main.h"
#include "CSlrKeyboardShortcuts.h"
#include <list>
#include <map>

class CGuiMain;
class CByteBuffer;

#define C64D_LAYOUTS_FILE_NAME 				"layouts.dat"

extern CSlrString *settingsPathToLayoutsFile;

class CLayoutData
{
public:
	CLayoutData();
	CLayoutData(const char *layoutName, CByteBuffer *serializedLayout, bool doNotUpdateViewsPosition, CSlrKeyboardShortcut *keyShortcut);
	virtual ~CLayoutData();
	
	char *layoutName;
	bool doNotUpdateViewsPositions;	// shall we always save views position on layout change, or not (i.e. always restore original positions)
	CByteBuffer *serializedLayoutBuffer;
	CSlrKeyboardShortcut *keyShortcut;

	bool isFullScreenLayout;
	CLayoutData *parentLayout;

	char *predefinedId;      // NULL = user-created; non-NULL = stable ID for app-defined workspaces
	char *translationKey;    // i18n key for display name; NULL = use layoutName
	bool IsPredefined() const { return predefinedId != NULL; }
};

class CLayoutManager : public CSlrKeyboardShortcutCallback
{
public:
	CLayoutManager(CGuiMain *guiMain);
	
	// note this will update layout data after current frame render is finished, expect buffer filled with data in next frame
	virtual void SerializeLayoutAsync(CLayoutData *layoutData);
	virtual void SetLayoutAsync(CLayoutData *layoutData, bool saveCurrentLayout);
	
	virtual void AddLayout(CLayoutData *layoutData);
	virtual void RemoveAndDeleteLayout(CLayoutData *layoutData);
	
	virtual void SerializeLayout(CLayoutData *layoutData, CByteBuffer *byteBuffer);
	virtual void SerializeLayouts(CByteBuffer *byteBuffer);
	CLayoutData *DeserializeLayout(CByteBuffer *byteBuffer, u16 version);
	virtual void DeserializeLayouts(CByteBuffer *byteBuffer, u16 version);

	virtual void DeleteAllLayouts();
	
	virtual void LoadLayouts();
	virtual void StoreLayouts();
	virtual void StoreLayout(CLayoutData *layoutData, CSlrString *filePath);
	virtual CLayoutData *LoadLayout(CSlrString *filePath);
	
	
	CLayoutData *GetLayoutByName(const char *name);

	CLayoutData *AddPredefinedLayout(const char *predefinedId, const char *translationKey);
	void RemovePredefinedLayout(const char *predefinedId);
	CLayoutData *GetPredefinedLayoutById(const char *predefinedId);

	CGuiMain *guiMain;

	std::list<CLayoutData *> layouts;
	std::map<u64, CLayoutData *> layoutsByHash;
	std::map<u64, CLayoutData *> predefinedLayoutsById;
	
	CLayoutData *currentLayout;
	
	//
	virtual bool ProcessKeyboardShortcut(u32 zone, u8 actionType, CSlrKeyboardShortcut *keyboardShortcut);

};

#endif
