/*
 *  SYS_CFileSystem.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-20.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef __SYS_FILESYSTEM_H__
#define __SYS_FILESYSTEM_H__

#include "SYS_Main.h"
//#include "M_CSlrList.h"
#include <list>
#include <vector>

class CSlrString;

#define MAX_FILENAME_LENGTH 1024

#define SYS_FILE_SYSTEM_PATH_SEPARATOR	'/'
#define SYS_FILE_SYSTEM_EXTENSION_SEPARATOR	'.'

void SYS_InitFileSystem();

extern char *gPathToDocuments;
extern char *gCPathToDocuments;
extern char *gPathToResources;
extern char *gPathToTemp;
extern char *gCPathToTemp;
extern char *gPathToCurrentDirectory;
extern char *gCPathToCurrentDirectory;
extern char *gPathToSettings;
extern char *gCPathToSettings;

extern CSlrString *gUTFPathToDocuments;
extern CSlrString *gUTFPathToTemp;
extern CSlrString *gUTFPathToSettings;
extern CSlrString *gUTFPathToCurrentDirectory;

class CHttpFileUploadedCallback
{
public:
	virtual void HttpFileUploadedCallback();
};

extern std::list<CHttpFileUploadedCallback *> httpFileUploadedCallbacks;

class CFileItem         //: public CSlrListElement
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

std::vector<CFileItem *> *SYS_GetFilesInFolder(const char *directoryPath, std::list<char *> *extensions);
std::vector<CFileItem *> *SYS_GetFilesInFolder(const char *directoryPath, std::list<char *> *extensions, bool withFolders);

void SYS_RefreshFiles();

#include "CSystemFileDialogCallback.h"

void SYS_DialogOpenFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFolder, CSlrString *windowTitle);
void SYS_DialogSaveFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFileName, CSlrString *defaultFolder, CSlrString *windowTitle);
void SYS_DialogPickFolder(CSystemFileDialogCallback *callback, CSlrString *defaultFolder);

void SYS_CreateFolder(const char *path);
void SYS_CreateFolder(CSlrString *path);

bool SYS_FileExists(const char *path);
bool SYS_FileExists(CSlrString *path);
bool SYS_FileDirExists(const char *path);
bool SYS_FileDirExists(CSlrString *path);

uint8 *SYS_MapMemoryToFile(int memorySize, const char *filePath, void **fileDescriptor);
void SYS_UnMapMemoryFromFile(uint8 *memoryMap, int memorySize, void **fileDescriptor);

void SYS_SetCurrentFolder(CSlrString *path);
char* SYS_GetFileExtension(const char* fileName);
char* SYS_GetFileName(const char* filePath);

const char *SYS_ExecSystemCommand(const char *cmd, int *terminationCode);
long SYS_GetFileModifiedTime(const char *filePath);

char* SYS_GetPathToDocuments();

#endif //__SYS_FILESYSTEM_H__
