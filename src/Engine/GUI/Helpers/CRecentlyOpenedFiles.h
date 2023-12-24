#ifndef _CRecentFiles_h_
#define _CRecentFiles_h_

#include "SYS_Defs.h"
#include "SYS_Threading.h"
#include <list>

class CByteBuffer;
class CSlrString;

#define C64D_RECENTS_FILE_NAME				"recents.dat"

class CRecentlyOpenedFilesCallback
{
public:
	virtual void RecentlyOpenedFilesCallbackSelectedMenuItem(CSlrString *filePath);
};

class CRecentFile
{
public:
	CRecentFile(CSlrString *filePath);
	~CRecentFile();
	CSlrString *filePath;
	char *fileName;
	bool isAvailable;
};

class CRecentlyOpenedFiles : public CSlrThread
{
public:
	CRecentlyOpenedFiles(CSlrString *settingsFileName, CRecentlyOpenedFilesCallback *callback);
	virtual ~CRecentlyOpenedFiles();
		
	CSlrString *settingsFileName;
	CRecentlyOpenedFilesCallback *callback;
	int maxNumberOfFiles;
	
	CSlrMutex *filesListMutex;
	std::list<CRecentFile *> listOfFiles;
	
	void Clear();
	void Add(CSlrString *filePath);
	void Remove(CSlrString *filePath);
	bool Exists(CSlrString *filePath);

	virtual void ThreadRun(void *passData);
	void RefreshAreFilesAvailable();
	u32 countFilesAvailableRefresh;
	
	bool IsMostRecentFilePathAvailable();
	
	CSlrString *GetMostRecentFilePath();
	CSlrString *GetCurrentOpenedFolder();
	
	void RenderImGuiMenu(const char *menuItemText);
	
	void Serialize(CByteBuffer *byteBuffer);
	bool Deserialize(CByteBuffer *byteBuffer);
	void StoreToSettings();
	void RestoreFromSettings();

};

#endif
