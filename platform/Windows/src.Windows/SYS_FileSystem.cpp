/*
 *  SYS_CFileSystem.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-20.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifdef WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#include "SYS_FileSystem.h"
#include "SYS_Startup.h"
#ifndef WIN32
#include "TargetConditionals.h"
#endif

#include "MT_API.h"

#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <algorithm>
#include <functional>
#include <Shlobj.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <windows.h>
#include <cstdint>
#include <deque>
#include <thread>
#include <filesystem>

#include "SYS_Startup.h"
#include "SYS_DocsVsRes.h"
#include "VID_Main.h"
#include "CGuiMain.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>

#include "mman.h"
#include "nfd.h"

HWND hWnd = NULL;

std::list<CHttpFileUploadedCallback *> httpFileUploadedCallbacks;

char *gPathToDocuments;
char *gCPathToDocuments;
CSlrString *gUTFPathToDocuments;

char *gPathToResources;

char *gPathToTemp;
char *gCPathToTemp;
CSlrString *gUTFPathToTemp;

char *gPathToSettings;
char *gCPathToSettings;
CSlrString *gUTFPathToSettings;

char *gPathToCurrentDirectory;
char *gCPathToCurrentDirectory;
CSlrString *gUTFPathToCurrentDirectory;

bool sysInitFileSystemDone = false;

void SYS_MoveFilesFromFolderAtoFolderB(const std::string& folderA, const std::string& folderB);

namespace fs = std::filesystem;

bool SYS_MoveFilesAndDeleteSource(const std::string& source, const std::string& destination);

// shall we use new folder for settings (CSIDL_LOCAL_APPDATA instead of CSIDL_COMMON_APPDATA)
//#define SKIP_SETTINGS_MIGRATION

void SYS_InitFileSystem()
{
	if (sysInitFileSystemDone == true)
		return;

	sysInitFileSystemDone = true;

	LOGM("SYS_InitFileSystem\n");

	TCHAR curDir[MAX_PATH];
	DWORD dwRet;

	dwRet = GetCurrentDirectory(MAX_PATH, curDir);

	LOGD("curDir='%s'", curDir);

	gPathToCurrentDirectory = new char[MAX_PATH];
	strcpy(gPathToCurrentDirectory, curDir);
	gCPathToCurrentDirectory = gPathToCurrentDirectory;
	gUTFPathToCurrentDirectory = new CSlrString(gCPathToCurrentDirectory);

	gPathToResources = new char[MAX_PATH];
	sprintf(gPathToResources, "%s\\Resources\\", curDir);
	LOGM("pathToResource=%s", gPathToResources);

	gPathToDocuments = new char[MAX_PATH];
	HRESULT hr = SHGetFolderPath(0, CSIDL_MYDOCUMENTS, 0, 0, gPathToDocuments);
	LOGM("pathToDocuments=%s", gPathToDocuments);
	gCPathToDocuments = gPathToDocuments;
	gUTFPathToDocuments = new CSlrString(gCPathToDocuments);

	gPathToTemp = new char[MAX_PATH];
	GetTempPathA(MAX_PATH, gPathToTemp);
	LOGM("gPathToTemp=%s", gPathToTemp);
	gCPathToTemp = gPathToTemp;
	gUTFPathToTemp = new CSlrString(gCPathToTemp);

	gPathToSettings = new char[MAX_PATH];
	
#if defined(SKIP_SETTINGS_MIGRATION)
	// use old code for settings location

	char* buf = SYS_GetCharBuf();
	if (!SUCCEEDED(SHGetFolderPath(NULL, CSIDL_COMMON_APPDATA, NULL, 0, buf)))
	{
		LOGError("failed to get app setings folder");
		sprintf(gPathToSettings, "%s\\", curDir);
	}
	else
	{
		const char *settingsFolderName = MT_GetSettingsFolderName();
		sprintf(gPathToSettings, "%s\\%s\\", buf, settingsFolderName);

		// check if folder exists & create new if needed
		DWORD dwAttrib = GetFileAttributes(gPathToSettings);
		if (dwAttrib == INVALID_FILE_ATTRIBUTES)
		{
			if (!CreateDirectory(gPathToSettings, NULL))
			{
				LOGError("failed to create app setings folder");
				sprintf(gPathToSettings, "%s\\", curDir);
			}
		}
		strcat(gPathToSettings, "\\");
	}
	SYS_ReleaseCharBuf(buf);

#else
	// upgrade and move settings from old location to a new location
	// we need to move from CSIDL_COMMON_APPDATA to CSIDL_LOCAL_APPDATA as the CSIDL_COMMON_APPDATA is not writable anymore in new Windows (we all love Windows don't we)

	char* buf = SYS_GetCharBuf();

	if (!SUCCEEDED(SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf)))
	{
		LOGError("failed to get app setings folder: CSIDL_LOCAL_APPDATA");
		sprintf(gPathToSettings, "%s\\", curDir);
	}
	else
	{
		const char *settingsFolderName = MT_GetSettingsFolderName();
		sprintf(gPathToSettings, "%s\\%s\\", buf, settingsFolderName);
		
		// check if folder exists & create new if needed
		DWORD dwAttrib = GetFileAttributes(gPathToSettings);
		if (dwAttrib == INVALID_FILE_ATTRIBUTES)
		{
			// settings folder does not exist
			if (!CreateDirectory(gPathToSettings, NULL))
			{
				LOGError("failed to create app setings folder");
				sprintf(gPathToSettings, "%s\\", curDir);
			}
			else
			{
				// created new settings folder, check if we need to move from old location
				char *oldSettingsFolder = NULL;
				bool oldFolderExists = false;
				if (!SUCCEEDED(SHGetFolderPath(NULL, CSIDL_COMMON_APPDATA, NULL, 0, buf)))
				{
					LOGError("failed to get app setings folder: CSIDL_COMMON_APPDATA");
				}
				else
				{
					const char *settingsFolderName = MT_GetSettingsFolderName();
					oldSettingsFolder = new char[MAX_STRING_LENGTH];
					sprintf(oldSettingsFolder, "%s\\%s\\", buf, settingsFolderName);

					if (SYS_FileExists(oldSettingsFolder))
					{
						LOGM("Upgrading settings path, moving settings from %s to %s", oldSettingsFolder, gPathToSettings);
						
						// we need to move all settings files from old folder
						std::string folderFrom = std::string(oldSettingsFolder);
						std::string folderTo = std::string(gPathToSettings);
						SYS_MoveFilesAndDeleteSource(folderFrom, folderTo);
	//					SYS_MoveFilesFromFolderAtoFolderB(folderFrom, folderTo);
						
						// remove old settings folder
//						RemoveDirectory(oldSettingsFolder);
					}

					if (oldSettingsFolder != NULL)
						delete[] oldSettingsFolder;
				}
			}
		}
		
		// do we need this?? --- no!
		//strcat(gPathToSettings, "\\");
		//LOGTODO(" is settings folder correct: %s", gPathToSettings);
	}
	
	SYS_ReleaseCharBuf(buf);

#endif
	
	LOGM("pathToSettings=%s", gPathToSettings);

	gCPathToSettings = gPathToSettings;
	gUTFPathToSettings = new CSlrString(gCPathToSettings);
}

bool SYS_MoveFilesAndDeleteSource(const std::string& source, const std::string& destination)
{
	try
	{
		fs::create_directory(destination);

		for (const auto& entry : fs::directory_iterator(source))
		{
			const auto& path = entry.path();
			auto dest = fs::path(destination) / path.filename();

			if (fs::is_directory(path))
			{
				if (!SYS_MoveFilesAndDeleteSource(path.string(), dest.string()))
				{
					return false;
				}
			}
			else
			{
				fs::copy_file(path, dest, fs::copy_options::overwrite_existing);
			}
		}

		// note, do not delete for now to let old apps still have settings
//		fs::remove_all(source);
	}
	catch (const fs::filesystem_error& e)
	{
//		LOGError("SYS_MoveFilesAndDeleteSource: exception %s", e.what().c_str());
		return false;
	}

	return true;
}

void SYS_MoveFilesFromFolderAtoFolderB(const std::string& folderA, const std::string& folderB)
{
	LOGD("SYS_MoveFilesFromFolderAtoFolderB");
	
	WIN32_FIND_DATA findFileData;
	HANDLE hFind = FindFirstFile((folderA + "\\*").c_str(), &findFileData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		LOGError("FindFirstFile failed with error %s", GetLastError());
		return;
	}

	do
	{
		// Skip directories
		if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			continue;
		}

		std::string sourceFile = folderA + "\\" + findFileData.cFileName;
		std::string destFile = folderB + "\\" + findFileData.cFileName;

		// Move the file
		if (!MoveFile(sourceFile.c_str(), destFile.c_str()))
		{
			LOGError("Failed to move file: %s with error %s", sourceFile.c_str(), GetLastError());
		}

	}
	while (FindNextFile(hFind, &findFileData) != 0);

	FindClose(hFind);
}

CFileItem::CFileItem(char *name, char *fullPath, char *modDate, bool isDir)
{
	this->name = STRALLOC(name);
	this->fullPath = STRALLOC(fullPath);
	this->modDate = STRALLOC(modDate);
	this->isDir = isDir;
}

CFileItem::~CFileItem()
{
	delete this->name;
	delete this->modDate;
}

// comparison, not case sensitive.
bool compare_CFileItem_nocase (CFileItem *first, CFileItem *second)
{
	if (first->isDir == second->isDir)
	{
		unsigned int i=0;
		u32 l1 = strlen(first->name);
		u32 l2 = strlen(second->name);
		while ( (i < l1) && ( i < l2) )
		{
			if (tolower(first->name[i]) < tolower(second->name[i]))
			{
				return true;
			}
			else if (tolower(first->name[i]) > tolower(second->name[i]))
			{
				return false;
			}
			++i;
		}

		if (l1 < l2)
		{
			return true;
		}
		else
		{
			return false;
		}
	}

	if (first->isDir)
		return true;

	return false;
}

char* SYS_FileSystemGetExtension(char* fileName)
{
	int index = -1;
	for (int i = strlen(fileName) - 1; i >= 0; i--)
	{
		if (fileName[i] == '.')
		{
			index = i + 1;
			break;
		}
	}

	if (index == -1)
		return NULL;

	char* buf = (char*)malloc(strlen(fileName) - index + 1);
	int z = 0;
	for (int i = index; i < strlen(fileName); i++)
	{
		if (fileName[i] == '/' || fileName[i] == '\\')
			break;

		buf[z] = fileName[i];
		z++;
	}
	buf[z] = 0x00;
	return buf;
}

#define BUFSIZE 4096
std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions)
{
	return SYS_GetFilesInFolder(directoryPath, extensions, true);
}

std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions, bool withFolders)
{
	LOGD("CFileSystem::GetFiles: %s", directoryPath);
	std::vector<CFileItem *> *files = new std::vector<CFileItem *>();

	WIN32_FIND_DATA ffd;
	LARGE_INTEGER filesize;
	TCHAR szDir[MAX_PATH];
	size_t length_of_arg;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	DWORD dwError =0;

	DWORD  retval = 0;
	BOOL   success;
	TCHAR  buffer[BUFSIZE] = TEXT("");
	TCHAR  buf[BUFSIZE] = TEXT("");
	TCHAR** lppPart = { NULL };

	StringCchLength(directoryPath, MAX_PATH, &length_of_arg);
	if (length_of_arg > (MAX_PATH-3))
	{
		LOGError("CFileSystem::GetFiles: directoryPath too long");
		return files;
	}

	StringCchCopy(szDir, MAX_PATH, directoryPath);
	StringCchCat(szDir, MAX_PATH, TEXT("\\*"));

	hFind = FindFirstFile(szDir, &ffd);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		LOGError("CFileSystem::GetFiles: FindFirstFile");
		return files;
	}

	do
	{
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (withFolders)
			{
				LOGD("<DIR> %s", ffd.cFileName);

				if (!strcmp(ffd.cFileName, ".") || !strcmp(ffd.cFileName, ".."))
				{
				}
				else
				{
					// oh, I forgot that Windows is for retards (as Gideon said)
					// this is not giving full path to file, but just random strings
	/*				retval = GetFullPathName(ffd.cFileName, BUFSIZE, buffer, lppPart);

					if (retval == 0)
					{
						// Handle an error condition.
						LOGError("GetFullPathName failed (%d)\n", GetLastError());
						return NULL;
					}*/
					sprintf(buffer, "%s\\%s", directoryPath, ffd.cFileName);
					LOGD("the full path name is %s", buffer);

					char *modDateDup = strdup("");

					CFileItem *item = new CFileItem(ffd.cFileName, buffer, modDateDup, true);
					files->push_back(item);
				}
			}
		}
		else
		{
			if (extensions != NULL)
			{
				char *fileExtension = SYS_FileSystemGetExtension(ffd.cFileName);

				if (fileExtension != NULL)
				{
					for (std::list<char *>::iterator itExtensions = extensions->begin();
						 itExtensions !=  extensions->end(); itExtensions++)
					{
						char *extension = *itExtensions;

						//LOGD("fileExtension='%s' extension='%s'", fileExtension, extension);
						if (!strcmp(extension, fileExtension))
						{
							//LOGD("adding");
							//LOGD(fname);

							filesize.LowPart = ffd.nFileSizeLow;
							filesize.HighPart = ffd.nFileSizeHigh;
							LOGD("     %s %ld", ffd.cFileName, filesize.QuadPart);

							// note, this below is copy pasted on purpose, because on this shitty platform I fucking do not care (as Windows devs also do).

							/*
							// oh, I forgot that Windows is for retards (as Gideon said)
							//char *fileFullPath = strdup(ffd.path)
							retval = GetFullPathName(ffd.cFileName, BUFSIZE, buffer, lppPart);

							if (retval == 0)
							{
								// Handle an error condition.
								LOGError("GetFullPathName failed (%d)\n", GetLastError());
								return NULL;
							}
							*/

							sprintf(buffer, "%s\\%s", directoryPath, ffd.cFileName);

							CFileItem *item = new CFileItem(ffd.cFileName, buffer, "", false);
							files->push_back(item);
							break;
						}
					}
					free(fileExtension);
				}
			}
			else
			{
				/*
				retval = GetFullPathName(ffd.cFileName, BUFSIZE, buffer, lppPart);

				if (retval == 0)
				{
					// Handle an error condition.
					LOGError("GetFullPathName failed (%d)\n", GetLastError());
					return NULL;
				}
				*/
				sprintf(buffer, "%s\\%s", directoryPath, ffd.cFileName);

				LOGD("the full path name is %s", buffer);

				CFileItem *item = new CFileItem(ffd.cFileName, buffer, (char*)"<mod date>", false);
				files->push_back(item);
			}
		}
	}
	while(FindNextFile(hFind, &ffd) != 0);

	dwError = GetLastError();
	if (dwError != ERROR_NO_MORE_FILES)
	{
		LOGError("CFileSystem::GetFiles: FindNextFile %d", dwError);
	}

	FindClose(hFind);

	LOGD("CFileSystem::GetFiles done");

	std::sort(files->begin(), files->end(), compare_CFileItem_nocase);

	return files;
}

void CHttpFileUploadedCallback::HttpFileUploadedCallback()
{
}

void SYS_RefreshFiles()
{
	for(std::list<CHttpFileUploadedCallback *>::iterator itCallback = httpFileUploadedCallbacks.begin(); itCallback != httpFileUploadedCallbacks.end(); itCallback++)
	{
		CHttpFileUploadedCallback *callback = *itCallback;
		callback->HttpFileUploadedCallback();
	}
}

FILE *SYS_OpenFile(const char *path, const char *mode)
{
	return fopen(path, mode);
//	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
//	std::wstring wpath = converter.from_bytes(path);
//	return _wfopen(wpath.c_str(), converter.from_bytes(mode).c_str());
}

void GUI_KeyUpAllModifiers()
{
	guiMain->isShiftPressed = false;
	guiMain->isControlPressed = false;
	guiMain->isAltPressed = false;

	guiMain->isLeftShiftPressed = false;
	guiMain->isLeftControlPressed = false;
	guiMain->isLeftAltPressed = false;

	guiMain->isRightShiftPressed = false;
	guiMain->isRightControlPressed = false;
	guiMain->isRightAltPressed = false;
}

bool SYS_windowAlwaysOnTopBeforeFileDialog = false;

void SYS_DialogOpenFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFolder, CSlrString *windowTitle)
{
	LOGD("SYS_DialogOpenFile");

	OPENFILENAME ofn;
    char szFileName[MAX_PATH] = "";

	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsMainWindowAlwaysOnTop();
	//VID_SetWindowAlwaysOnTopTemporary(false);

    ZeroMemory(&ofn, sizeof(ofn));

    ofn.lStructSize = sizeof(ofn); // SEE NOTE BELOW
    ofn.hwndOwner = hWnd;

	char *initialFolder = NULL;

	char *buf = SYS_GetCharBuf();
	char *ext = NULL;

	if (extensions != NULL)
	{
		char *filterExtAll = new char[1024];
		filterExtAll[0] = 0x00;

		char *filterExtSingle = new char[1024];
		filterExtSingle[0] = 0x00;
		
		for (std::list<CSlrString *>::iterator it = extensions->begin();
			it != extensions->end(); it++)
		{
			CSlrString *extStr = *it;
			ext = extStr->GetStdASCII();

			char extBuf[128];
			if (it == extensions->begin())
			{
				sprintf(extBuf, "*.%s", ext);
			}
			else
			{
				sprintf(extBuf, ";*.%s", ext);
			}
			strcat(filterExtAll, extBuf);

			sprintf(extBuf, "Only %s files$*.%s$", ext, ext);
			strcat(filterExtSingle, extBuf);

			free(ext); ext = NULL;
		}
		
		if (extensions->size() == 1)
		{
			sprintf(buf, "Supported files$%s$All Files(*.*)$*.*$", filterExtAll);
		}
		else
		{
			sprintf(buf, "Supported files$%s$%sAll Files(*.*)$*.*$", filterExtAll, filterExtSingle);
		}

		delete [] filterExtAll;
		delete [] filterExtSingle;

		int z = strlen(buf);
		for (int i = 0; i < z; i++)
		{
			if (buf[i] == '$')
				buf[i] = '\0';
		}

		ofn.lpstrFilter = buf;
	    ofn.lpstrDefExt = NULL;
	}

	char *title = NULL;

	if (windowTitle != NULL)
	{
		title = windowTitle->GetStdASCII();
		ofn.lpstrTitle = title;
	}

    ofn.lpstrFile = szFileName;

	if (defaultFolder != NULL)
	{
		initialFolder = defaultFolder->GetStdASCII();
		ofn.lpstrInitialDir = initialFolder;

		LOGD(">> set ofn.lpstrInitialDir='%s'", initialFolder);
	}
	else
	{
		LOGD(">> defaultFolder is NULL");
	}

    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    
    // workaround
    GUI_KeyUpAllModifiers();
    
    LOGD("...... GetOpenFileName");
    if(GetOpenFileName(&ofn))
    {
    	LOGD("..... callback: file open selected");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		if (title != NULL)
			free(title);

		LOGD("szFileName='%s'", szFileName);
		SYS_ReleaseCharBuf(buf);
		CSlrString *outPath = new CSlrString(szFileName);
		callback->SystemDialogFileOpenSelected(outPath);
		if (initialFolder != NULL)
			delete initialFolder;
	}
	else
	{
		LOGD("..... callback: file open cancelled");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		if (title != NULL)
			free(title);
		SYS_ReleaseCharBuf(buf);
		callback->SystemDialogFileOpenCancelled();
	}
}

void SYS_DialogSaveFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFileName, CSlrString *defaultFolder, CSlrString *windowTitle)
{
	OPENFILENAME ofn;
	char szFileName[MAX_PATH];
	memset(szFileName, 0, MAX_PATH - 1);

	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsMainWindowAlwaysOnTop();
	//VID_SetWindowAlwaysOnTopTemporary(false);

    ZeroMemory(&ofn, sizeof(ofn));

    ofn.lStructSize = sizeof(ofn); // SEE NOTE BELOW
    ofn.hwndOwner = hWnd;

	char *initialFolder = NULL;
	char *buf = SYS_GetCharBuf();
	char *ext = NULL;
	char defExt[16];

	if (extensions != NULL)
	{
		char *filterExtAll = new char[1024];
		filterExtAll[0] = 0x00;

		char *filterExtSingle = new char[1024];
		filterExtSingle[0] = 0x00;
		
		for (std::list<CSlrString *>::iterator it = extensions->begin();
			it != extensions->end(); it++)
		{
			CSlrString *extStr = *it;
			ext = extStr->GetStdASCII();

			char extBuf[128];
			if (it == extensions->begin())
			{
				sprintf(extBuf, "*.%s", ext);
				strcpy(defExt, ext);
			}
			else
			{
				sprintf(extBuf, ";*.%s", ext);
			}
			strcat(filterExtAll, extBuf);

			sprintf(extBuf, "Only %s files$*.%s$", ext, ext);
			strcat(filterExtSingle, extBuf);

			free(ext); ext = NULL;
		}
		
		if (extensions->size() == 1)
		{
			sprintf(buf, "%s file$%s$All Files(*.*)$*.*$", defExt, filterExtAll);
		}
		else
		{
			sprintf(buf, "Supported files$%s$%sAll Files(*.*)$*.*$", filterExtAll, filterExtSingle);
		}

		delete [] filterExtAll;
		delete [] filterExtSingle;

		int z = strlen(buf);
		for (int i = 0; i < z; i++)
		{
			if (buf[i] == '$')
				buf[i] = '\0';
		}

		ofn.lpstrFilter = buf;
	    ofn.lpstrDefExt = defExt;
	}

	char *title = NULL;

	if (windowTitle != NULL)
	{
		title = windowTitle->GetStdASCII();
		ofn.lpstrTitle = title;
	}

	if (defaultFileName != NULL)
	{
		LOGD("defaultFileName != NULL");
		defaultFileName->DebugPrint("defaultFileName=");
		char *fileName = defaultFileName->GetStdASCII();
		strncpy(szFileName, fileName, MAX_PATH-1);
		LOGD("szFileName=%s", szFileName);
		STRFREE(fileName);
	}
    ofn.lpstrFile = szFileName;

	if (defaultFolder != NULL)
	{
		initialFolder = defaultFolder->GetStdASCII();
		ofn.lpstrInitialDir = initialFolder;
	}
    ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
	
	
	// workaround
	GUI_KeyUpAllModifiers();
	
	LOGD("....... GetSaveFileName");
    if(GetSaveFileName(&ofn))
    {
    	LOGD("     ...callback OK");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		if (title != NULL)
			free(title);

		LOGD("szFileName='%s'", szFileName);
		SYS_ReleaseCharBuf(buf);
		CSlrString *outPath = new CSlrString(szFileName);
		callback->SystemDialogFileSaveSelected(outPath);

		if (initialFolder != NULL)
			delete initialFolder;
	}
	else
	{
		LOGD("    ...callback cancelled");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		if (title != NULL)
			free(title);

		SYS_ReleaseCharBuf(buf);
		callback->SystemDialogFileSaveCancelled();
	}
}

void SYS_DialogPickFolder(CSystemFileDialogCallback* callback, CSlrString* defaultFolder)
{
	char* defaultPath = NULL;
	if (defaultFolder)
	{
		defaultPath = defaultFolder->GetStdASCII();
	}
	else
	{
		defaultPath = STRALLOC(gPathToDocuments);
	}
	char* outPath = NULL;
	nfdresult_t result = NFD_PickFolder(defaultPath, &outPath);
	if (result == NFD_OKAY)
	{
		CSlrString* path = new CSlrString(outPath);
		callback->SystemDialogPickFolderSelected(path);
		free(outPath);
		delete path;
	}
	else if (result == NFD_CANCEL)
	{
		callback->SystemDialogPickFolderCancelled();
	}
	else
	{
		LOGError("SYS_DialogPickFolder: %s", NFD_GetError());
	}
}

bool SYS_FileExists(const char *cPath)
{
	if (cPath == NULL)
		return false;

	struct stat info;

	LOGD("SYS_FileExists, cPath='%s'", cPath);
	
	if(stat( cPath, &info ) != 0)
	{
		LOGD("..false");
		return false;
	}
	else 
	{
		LOGD("..true");
		return true;
	}
}

bool SYS_FileExists(CSlrString *path)
{
	if (path == NULL)
		return false;

	char *cPath = path->GetStdASCII();
	
	struct stat info;
	
	if(stat( cPath, &info ) != 0)
	{
		delete [] cPath;
		return false;
	}
	else 
	{
		delete [] cPath;
		return true;
	}
}

bool SYS_FileDirExists(const char *cPath)
{
	struct stat info;
	
	if(stat( cPath, &info ) != 0)
		return false;
	else if(info.st_mode & S_IFDIR)
		return true;
	else
		return false;
}

bool SYS_FileDirExists(CSlrString *path)
{
	char *cPath = path->GetStdASCII();
	
	struct stat info;
	
	if(stat( cPath, &info ) != 0)
		return false;
	else if(info.st_mode & S_IFDIR)
		return true;
	else
		return false;
	
	delete [] cPath;
}

uint8 *SYS_MapMemoryToFile(int memorySize, char *filePath, void **fileDescriptor)
{
	int *fileHandle = (int*)malloc(sizeof(int));
	fileDescriptor = (void**)(&fileHandle);
	
	*fileHandle = open(filePath, O_RDWR | O_CREAT | O_TRUNC, _S_IREAD | S_IWRITE);
	
	if(*fileHandle == -1)
	{
		LOGError("SYS_MapMemoryToFile: error opening file for writing, path=%s", filePath);
		return NULL;
	}
	
	if(lseek(*fileHandle, memorySize - 1, SEEK_SET) == -1)
	{
		LOGError("SYS_MapMemoryToFile: error in seeking the file, path=%s", filePath);
		return NULL;
	}
	
	if(write(*fileHandle, "", 1) != 1)
	{
		LOGError("SYS_MapMemoryToFile: error in writing the file, path=%s", filePath);
		return NULL;
	}
	
	uint8 *memoryMap = (uint8*)mmap(0, memorySize, PROT_READ | PROT_WRITE, MAP_SHARED, *fileHandle, 0);
	
	if (memoryMap == MAP_FAILED)
	{
		close(*fileHandle);
		
		LOGError("SYS_MapMemoryToFile: error mmaping the file, path=%s", filePath);
		return NULL;
	}

	close(*fileHandle);

	return memoryMap;
}

void SYS_UnMapMemoryFromFile(uint8 *memoryMap, int memorySize, void **fileDescriptor)
{
	if (munmap(memoryMap, memorySize) == -1)
	{
		LOGError("SYS_UnMapMemoryFromFile: error unmapping the file");
		return;
	}
	
	int *fileHandle = (int*)*fileDescriptor;
	
	close(*fileHandle);
}

void SYS_CreateFolder(const char *path)
{
	LOGTODO("SYS_CreateFolder, path=%s", path);
	CreateDirectory(path, NULL);
}

void SYS_CreateFolder(CSlrString *path)
{
	LOGTODO("SYS_CreateFolder");
	path->DebugPrint("SYS_CreateFolder: ");
	char *cPath = path->GetStdASCII();
	CreateDirectory(cPath, NULL);

	delete [] cPath;
}

void SYS_SetCurrentFolder(CSlrString *path)
{
	LOGD("SYS_SetCurrentFolder");
	path->DebugPrint("SYS_SetCurrentFolder: ");
	char *cPath = path->GetStdASCII();
	SetCurrentDirectory(cPath);

	delete [] cPath;
}

char* SYS_GetFileName(const char* filePath)
{
	//	char *bname = basename(filePath);

	int offset_extension, offset_name;
	int len = strlen(filePath);
	int i;
	for (i = len; i >= 0; i--) {
		if (filePath[i] == '.')
			break;
		if (filePath[i] == '/' || filePath[i] == '\\') {
			i = len;
			break;
		}
	}
	if (i == -1) {
		fprintf(stderr, "Invalid path: %s", filePath);
		LOGError("SYS_GetFileName: invalid path %s", filePath);
		i = 0;
	}
	offset_extension = i;
	for (; i >= 0; i--)
		if (filePath[i] == '/' || filePath[i] == '\\')
			break;
	if (i == -1) {
		fprintf(stderr, "Invalid path: %s", filePath);
		LOGError("SYS_GetFileName: invalid path %s", filePath);
		i = 0;
	}
	offset_name = i;

	const char* extension;
	char* fileName = new char[128];
	memset(fileName, 0x00, 128);
	extension = &filePath[offset_extension + 1];
	memcpy(fileName, &filePath[offset_name + 1], offset_extension - offset_name - 1);

	return fileName;
}

char* SYS_GetFileExtension(const char* fileName)
{
	int index = -1;
	for (int i = (int)strlen(fileName) - 1; i >= 0; i--)
	{
		if (fileName[i] == SYS_FILE_SYSTEM_PATH_SEPARATOR)
		{
			// no extension separator
			char *buf = new char[1];
			buf[0] = 0x00;
			return buf;
		}
		if (fileName[i] == SYS_FILE_SYSTEM_EXTENSION_SEPARATOR)
		{
			index = i+1;
			break;
		}
	}

	if (index == -1)
	{
		char *empty = new char[1];
		empty[0] = 0x00;
		return empty;
	}


	char* buf = (char*)malloc(strlen(fileName) - index + 1);
	int z = 0;
	for (int i = index; i < strlen(fileName); i++)
	{
		if (fileName[i] == '/' || fileName[i] == '\\')
			break;

		buf[z] = fileName[i];
		z++;
	}
	buf[z] = 0x00;
	return buf;
}

char* SYS_GetPathToDocuments()
{
	return gCPathToDocuments;
}

void CSystemFileDialogCallback::SystemDialogFileOpenSelected(CSlrString* path)
{
}

void CSystemFileDialogCallback::SystemDialogFileOpenCancelled()
{
}

void CSystemFileDialogCallback::SystemDialogFileSaveSelected(CSlrString* path)
{
}

void CSystemFileDialogCallback::SystemDialogFileSaveCancelled()
{
}

void CSystemFileDialogCallback::SystemDialogPickFolderSelected(CSlrString* path)
{
}

void CSystemFileDialogCallback::SystemDialogPickFolderCancelled()
{
}

#ifndef WIN32
#include <unistd.h>
#endif
#ifdef WIN32
#define stat _stat
#endif

long SYS_GetFileModifiedTime(const char* filePath)
{
	struct stat result;
	if (stat(filePath, &result) == 0)
	{
		return (long)(result.st_mtime);
	}

	return 0;
}

using namespace std;

int SystemCapture(
	string         CmdLine,    //Command Line
	string         CmdRunDir,  //set to '.' for current directory
	string& ListStdOut, //Return List of StdOut
	string& ListStdErr, //Return List of StdErr
	uint32_t& RetCode)    //Return Exit Code
{
	int                  Success;
	SECURITY_ATTRIBUTES  security_attributes;
	HANDLE               stdout_rd = INVALID_HANDLE_VALUE;
	HANDLE               stdout_wr = INVALID_HANDLE_VALUE;
	HANDLE               stderr_rd = INVALID_HANDLE_VALUE;
	HANDLE               stderr_wr = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION  process_info;
	STARTUPINFO          startup_info;
	thread               stdout_thread;
	thread               stderr_thread;

	security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
	security_attributes.bInheritHandle = TRUE;
	security_attributes.lpSecurityDescriptor = nullptr;

	if (!CreatePipe(&stdout_rd, &stdout_wr, &security_attributes, 0) ||
		!SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0)) {
		return -1;
	}

	if (!CreatePipe(&stderr_rd, &stderr_wr, &security_attributes, 0) ||
		!SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0)) {
		if (stdout_rd != INVALID_HANDLE_VALUE) CloseHandle(stdout_rd);
		if (stdout_wr != INVALID_HANDLE_VALUE) CloseHandle(stdout_wr);
		return -2;
	}

	ZeroMemory(&process_info, sizeof(PROCESS_INFORMATION));
	ZeroMemory(&startup_info, sizeof(STARTUPINFO));

	startup_info.cb = sizeof(STARTUPINFO);
	startup_info.hStdInput = 0;
	startup_info.hStdOutput = stdout_wr;
	startup_info.hStdError = stderr_wr;

	if (stdout_rd || stderr_rd)
		startup_info.dwFlags |= STARTF_USESTDHANDLES;

	// Make a copy because CreateProcess needs to modify string buffer
	char      CmdLineStr[MAX_PATH];
	strncpy(CmdLineStr, CmdLine.c_str(), MAX_PATH);
	CmdLineStr[MAX_PATH - 1] = 0;

	Success = CreateProcess(
		nullptr,
		CmdLineStr,
		nullptr,
		nullptr,
		TRUE,
		0,
		nullptr,
		CmdRunDir.c_str(),
		&startup_info,
		&process_info
	);
	CloseHandle(stdout_wr);
	CloseHandle(stderr_wr);

	if (!Success) {
		CloseHandle(process_info.hProcess);
		CloseHandle(process_info.hThread);
		CloseHandle(stdout_rd);
		CloseHandle(stderr_rd);
		return -4;
	}
	else {
		CloseHandle(process_info.hThread);
	}

	if (stdout_rd) {
		stdout_thread = thread([&]() {
			DWORD  n;
			const size_t bufsize = 1000;
			char         buffer[bufsize];
			for (;;) {
				n = 0;
				int Success = ReadFile(
					stdout_rd,
					buffer,
					(DWORD)bufsize,
					&n,
					nullptr
				);
				//printf("STDERR: Success:%d n:%d\n", Success, (int)n);
				if (!Success || n == 0)
					break;
				string s(buffer, n);
				//printf("STDOUT:(%s)\n", s.c_str());
				ListStdOut += s;
			}
			//printf("STDOUT:BREAK!\n");
			});
	}

	if (stderr_rd) {
		stderr_thread = thread([&]() {
			DWORD        n;
			const size_t bufsize = 1000;
			char         buffer[bufsize];
			for (;;) {
				n = 0;
				int Success = ReadFile(
					stderr_rd,
					buffer,
					(DWORD)bufsize,
					&n,
					nullptr
				);
				//printf("STDERR: Success:%d n:%d\n", Success, (int)n);
				if (!Success || n == 0)
					break;
				string s(buffer, n);
				//printf("STDERR:(%s)\n", s.c_str());
				ListStdOut += s;
			}
			//printf("STDERR:BREAK!\n");
			});
	}

	WaitForSingleObject(process_info.hProcess, INFINITE);
	if (!GetExitCodeProcess(process_info.hProcess, (DWORD*)&RetCode))
		RetCode = -1;

	CloseHandle(process_info.hProcess);

	if (stdout_thread.joinable())
		stdout_thread.join();

	if (stderr_thread.joinable())
		stderr_thread.join();

	CloseHandle(stdout_rd);
	CloseHandle(stderr_rd);

	return 0;
}

const char* SYS_ExecSystemCommand(const char* cmd, int* terminationCode)
{
	LOGD("SYS_ExecSystemCommand: %s", cmd);

	int            rc;
	uint32_t       RetCode;
	string         ListStdOut;
	string         ListStdErr;

	string strCmd(cmd);

	rc = SystemCapture(
		strCmd,
		".",
		ListStdOut,
		ListStdErr,
		RetCode
	);

	*terminationCode = RetCode;

	return STRALLOC(ListStdOut.c_str());
}

std::vector<std::string> SYS_Win32GetAvailableDrivesPaths()
{
	std::vector<std::string> drives;
	DWORD driveMask = GetLogicalDrives();
	if (driveMask == 0)
	{
		std::cerr << "Failed to get drives." << std::endl;
		return drives; // Return empty vector if there's an error
	}

	for (char drive = 'A'; drive <= 'Z'; drive++) {
		// Check if the corresponding bit is set in the mask
		if (driveMask & (1 << (drive - 'A')))
		{
			std::string drivePath = std::string(1, drive) + ":\\";
			drives.push_back(drivePath);
		}
	}

	return drives;
}

void SYS_OpenURLInBrowser(const char *url)
{
	// The function opens the document or file specified by lpFile in the application specified by lpOperation
	HINSTANCE result = ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);

	// ShellExecute returns a value greater than 32 if successful
	if ((int)result <= 32) 
	{
		LOGError("SYS_OpenURLInBrowser: Failed to open URL: %s", url);
	}
}
