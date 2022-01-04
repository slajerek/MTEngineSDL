#ifndef _CRecentFiles_h_
#define _CRecentFiles_h_

#include "SYS_Defs.h"
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
};

class CRecentlyOpenedFiles
{
public:
	CRecentlyOpenedFiles(CSlrString *settingsFileName, CRecentlyOpenedFilesCallback *callback);
	virtual ~CRecentlyOpenedFiles();
	
	CSlrString *settingsFileName;
	CRecentlyOpenedFilesCallback *callback;
	int maxNumberOfFiles;
	
	std::list<CRecentFile *> listOfFiles;
	
	void Clear();
	void Add(CSlrString *filePath);
	
	bool IsMostRecentFilePathAvailable();
	
	CSlrString *GetMostRecentFilePath();
	CSlrString *GetCurrentOpenedFolder();
	
	void RenderImGuiMenu(char *menuItemText);
	
	void Serialize(CByteBuffer *byteBuffer);
	bool Deserialize(CByteBuffer *byteBuffer);
	void StoreToSettings();
	void RestoreFromSettings();
};

#endif
