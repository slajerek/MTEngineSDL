/*
 *  SYS_CFileSystem.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-20.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef __SYS_CFILESYSTEM_H__
#define __SYS_CFILESYSTEM_H__

#include "SYS_Main.h"
//#include "M_CSlrList.h"
#include <list>
#include <vector>
#include "CSlrString.h"

#define MAX_FILENAME_LENGTH 512

#define SYS_FILE_SYSTEM_PATH_SEPARATOR	'\\'
#define SYS_FILE_SYSTEM_EXTENSION_SEPARATOR	'.'

void SYS_InitFileSystem();

extern char *gPathToDocuments;
extern char *gCPathToDocuments;
extern char *gPathToTemp;
extern char *gPathToResources;
extern char *gCPathToTemp;
extern char *gPathToSettings;
extern char *gCPathToSettings;

extern CSlrString *gUTFPathToDocuments;
extern CSlrString *gUTFPathToTemp;
extern CSlrString *gUTFPathToSettings;

extern char *gPathToCurrentDirectory;
extern char *gCPathToCurrentDirectory;
extern CSlrString *gUTFPathToCurrentDirectory;


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

char* SYS_FileSystemGetExtension(char* fileName);
std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions);
std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions, bool withFolders);

class compareFiles
{
	// simple comparison function
public:
	bool operator()(const CFileItem *f1, const CFileItem *f2) const
	{
		// Directories sort before files.
		//
		// This MUST be a strict weak ordering. The previous body returned
		// -1 / 1 / 0 out of a bool function: -1 and 1 BOTH convert to true,
		// so dir-vs-file and file-vs-dir each reported "less than" and every
		// such pair compared less than the other. std::sort is undefined
		// behaviour on a comparator like that -- it can misorder the listing
		// or run past the end of the sequence on a large enough folder.
		if (f1->isDir != f2->isDir)
			return f1->isDir;

		return false;
	}
};


void SYS_RefreshFiles();

// utf8-compatible equivalent
FILE *SYS_OpenFile(const char *path, const char *mode);

#include "CSystemFileDialogCallback.h"

void SYS_DialogOpenFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFolder, CSlrString *windowTitle);
void SYS_DialogOpenFiles(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFolder, CSlrString *windowTitle);
void SYS_DialogSaveFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFileName, CSlrString *defaultFolder, CSlrString *windowTitle);
void SYS_DialogPickFolder(CSystemFileDialogCallback* callback, CSlrString* defaultFolder);

void SYS_CreateFolder(const char *path);
void SYS_CreateFolder(CSlrString *path);

bool SYS_FileExists(const char *path);
bool SYS_FileExists(CSlrString *path);

// Move the file at `path` to the OS trash/recycle bin.
// Returns true on success; on failure sets *outError (if non-null).
// On success, if the platform reports where the file landed, *outTrashPath is set
// to that location (used to support undo/restore); left untouched otherwise.
// (Windows SHFileOperation does not report the recycled path, so it stays empty.)
// Never silently deletes permanently.
bool SYS_FileDeleteToTrash(const char *path, std::string *outTrashPath, std::string *outError);
bool SYS_FileDirExists(const char *path);
bool SYS_FileDirExists(CSlrString *path);

uint8 *SYS_MapMemoryToFile(int memorySize, char *filePath, void **fileDescriptor);
void SYS_UnMapMemoryFromFile(uint8 *memoryMap, int memorySize, void **fileDescriptor);

void SYS_SetCurrentFolder(CSlrString *path);
char* SYS_GetFileExtension(const char* fileName);
char* SYS_GetFileName(const char* filePath);
long SYS_GetFileModifiedTime(const char* filePath);
const char* SYS_ExecSystemCommand(const char* cmd, int* terminationCode);
std::vector<std::string> SYS_Win32GetAvailableDrivesPaths();

char* SYS_GetPathToDocuments();

std::string SYS_GetRelativePath(const char* pathToFolder, const char* pathToFile);
std::string SYS_GetAbsolutePath(const char* pathToFolder, const char* relativePath);

void SYS_OpenURLInBrowser(const char *url);

#endif //__SYS_CFILESYSTEM_H__
