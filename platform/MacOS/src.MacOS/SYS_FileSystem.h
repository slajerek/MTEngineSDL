/*
 *  SYS_CFileSystem.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-20.
 *  Copyright 2009 Rabidus. All rights reserved.
 *
 */

#ifndef __SYS_CFILESYSTEM_H__
#define __SYS_CFILESYSTEM_H__

#include "SYS_Main.h"
//#include "M_CSlrList.h"
#include <list>
#include <vector>

#define MAX_FILENAME_LENGTH 512

#define SYS_FILE_SYSTEM_PATH_SEPARATOR	'/'
#define SYS_FILE_SYSTEM_EXTENSION_SEPARATOR	'.'

void SYS_InitFileSystem();

class CSlrString;

extern char *gPathToDocuments;
extern char *gCPathToDocuments;
extern CSlrString *gUTFPathToDocuments;
extern std::string gStdPathToDocuments;

extern char *gPathToTemp;
extern char *gCPathToTemp;
extern CSlrString *gUTFPathToTemp;
extern std::string gStdPathToTemp;

extern char *gPathToResources;
extern char *gCPathToResources;
extern CSlrString *gUTFPathToResources;
extern std::string gStdPathToResources;

extern char *gPathToSettings;
extern char *gCPathToSettings;
extern CSlrString *gUTFPathToSettings;
extern std::string gStdPathToSettings;

extern char *gCPathToCurrentDirectory;
extern CSlrString *gUTFPathToCurrentDirectory;
extern std::string gStdPathToCurrentDirectory;

void SYS_DeleteFile(CSlrString *filePath);

class CHttpFileUploadedCallback
{
public:
	virtual void HttpFileUploadedCallback();
};

extern std::list<CHttpFileUploadedCallback *> httpFileUploadedCallbacks;

class CFileItem		//: public CSlrListElement
{
public:
	CFileItem();
	CFileItem(char *name, char *fullPath, char *modDate, bool isDir);
	~CFileItem();
	
	char *name;
	char *fullPath;
	char *modDate;
	bool isDir;
};

class compareFiles
{
	// simple comparison function
public:
	bool operator()(const CFileItem *f1, const CFileItem *f2)
	{
		if (f1->isDir && !f2->isDir)
		{
			return -1;
		}
		if (!f1->isDir && f2->isDir)
		{
			return 1;
		}
		
		return 0;
	}
};

std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions);
std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions, bool withFolders);

void SYS_RefreshFiles();

class CSlrString;

// utf8-compatible equivalent
FILE *SYS_OpenFile(const char *path, const char *mode);

#include "CSystemFileDialogCallback.h"

void SYS_DialogOpenFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions,
						CSlrString *defaultFolder, CSlrString *windowTitle);
void SYS_DialogSaveFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions,
						CSlrString *defaultFileName, CSlrString *defaultFolder, CSlrString *windowTitle);

void SYS_DialogPickFolder(CSystemFileDialogCallback *callback, CSlrString *defaultFolder);

void SYS_CreateFolder(const char *path);
void SYS_CreateFolder(CSlrString *path);

bool SYS_FileExists(const char *path);
bool SYS_FileExists(CSlrString *path);
bool SYS_FileDirExists(CSlrString *path);
bool SYS_FileDirExists(const char *cPath);

uint8 *SYS_MapMemoryToFile(int memorySize, char *filePath, void **fileDescriptor);
void SYS_UnMapMemoryFromFile(uint8 *memoryMap, int memorySize, void **fileDescriptor);

void SYS_SetCurrentFolder(CSlrString *path);
char *SYS_GetFileExtension(const char *fileName);
char *SYS_GetFileName(const char *filePath);
std::string SYS_GetRelativePath(const char* pathToFolder, const char* pathToFile);	// eg. folder /Users/mars/assets  file /Users/mars/assets/folder/file.png  result folder/file.png
std::string SYS_GetAbsolutePath(const char* pathToFolder, const char* relativePath); // eg. folder /Users/mars/assets  file folder/file.png  result /Users/mars/assets/folder/file.png

const char *SYS_ExecSystemCommand(const char *cmd, int *terminationCode);
long SYS_GetFileModifiedTime(const char *filePath);

char *SYS_GetPathToDocuments();

void SYS_OpenURLInBrowser(const char *url);

#endif //__SYS_CFILESYSTEM_H__
