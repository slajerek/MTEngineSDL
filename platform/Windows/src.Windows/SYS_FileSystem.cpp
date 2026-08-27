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
#include <shellapi.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <windows.h>
#include <cstdint>
#include <deque>
#include <thread>
#include <filesystem>
#include <vector>
#include <cwchar>

#include "SYS_Startup.h"
#include "SYS_DocsVsRes.h"
#include "SYS_WindowsPathUtils.h"
#include "VID_Main.h"
#include "CGuiMain.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <cstring>

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

static std::wstring SYS_WinUtf8ToWide(const char *utf8)
{
	if (utf8 == NULL || utf8[0] == 0)
		return std::wstring();

	int count = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if (count <= 0)
		return std::wstring();

	std::wstring wide(count, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), count);
	if (!wide.empty() && wide.back() == L'\0')
		wide.pop_back();
	return wide;
}

static std::string SYS_WinWideToUtf8(const wchar_t *wide)
{
	if (wide == NULL || wide[0] == 0)
		return std::string();

	int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
	if (count <= 0)
		return std::string();

	std::string utf8(count, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), count, NULL, NULL);
	if (!utf8.empty() && utf8.back() == '\0')
		utf8.pop_back();
	return utf8;
}

static std::string SYS_CSlrStringToUtf8(CSlrString *value)
{
	if (value == NULL)
		return std::string();

	char *utf8 = value->GetUTF8();
	std::string out = utf8 ? utf8 : "";
	free(utf8);
	return out;
}

static std::wstring SYS_WinDialogFilterToWide(std::string filter)
{
	std::wstring wide = SYS_WinUtf8ToWide(filter.c_str());
	for (wchar_t &c : wide)
	{
		if (c == L'$')
			c = L'\0';
	}
	return wide;
}

static std::vector<std::string> SYS_WinDialogBufferToPathsUtf8(const wchar_t *buffer)
{
	std::vector<std::string> dialogParts;
	if (buffer == NULL)
		return dialogParts;

	const wchar_t *part = buffer;
	while (*part != L'\0')
	{
		dialogParts.push_back(SYS_WinWideToUtf8(part));
		part += wcslen(part) + 1;
	}

	return SYS_WindowsExpandDialogSelectionUtf8(dialogParts);
}

static std::wstring SYS_WinUtf8PathToWideLong(const char *path)
{
	std::string normalized = SYS_WindowsNormalizeLongPathUtf8(path ? path : "");
	return SYS_WinUtf8ToWide(normalized.c_str());
}

static char *SYS_DupUtf8Path(const std::string &path)
{
	char *out = new char[path.size() + 1];
	memcpy(out, path.c_str(), path.size() + 1);
	return out;
}

static void SYS_AssignGlobalPath(char **path, char **cPath, CSlrString **utfPath, const std::string &value)
{
	*path = SYS_DupUtf8Path(value);
	*cPath = *path;
	*utfPath = new CSlrString(std::string(*cPath));
}

static std::string SYS_WinGetCurrentDirectoryUtf8()
{
	DWORD needed = GetCurrentDirectoryW(0, NULL);
	if (needed == 0)
		return std::string();

	std::wstring wide(needed, L'\0');
	DWORD copied = GetCurrentDirectoryW(needed, wide.data());
	if (copied == 0)
		return std::string();
	wide.resize(copied);
	return SYS_WinWideToUtf8(wide.c_str());
}

static std::string SYS_WinGetSpecialFolderUtf8(int csidl)
{
	wchar_t buf[MAX_PATH];
	buf[0] = 0;
	if (!SUCCEEDED(SHGetFolderPathW(NULL, csidl, NULL, 0, buf)))
		return std::string();
	return SYS_WinWideToUtf8(buf);
}

static std::string SYS_WinGetTempPathUtf8()
{
	DWORD needed = GetTempPathW(0, NULL);
	if (needed == 0)
		return std::string();

	std::wstring wide(needed + 1, L'\0');
	DWORD copied = GetTempPathW((DWORD)wide.size(), wide.data());
	if (copied == 0)
		return std::string();
	wide.resize(copied);
	return SYS_WinWideToUtf8(wide.c_str());
}

static bool SYS_WinPathExistsUtf8(const std::string &path)
{
	std::wstring wide = SYS_WinUtf8PathToWideLong(path.c_str());
	if (wide.empty())
		return false;
	return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool SYS_WinCreateDirectoryUtf8(const std::string &path)
{
	std::wstring wide = SYS_WinUtf8PathToWideLong(path.c_str());
	if (wide.empty())
		return false;
	if (CreateDirectoryW(wide.c_str(), NULL))
		return true;
	return GetLastError() == ERROR_ALREADY_EXISTS;
}

// shall we use new folder for settings (CSIDL_LOCAL_APPDATA instead of CSIDL_COMMON_APPDATA)
//#define SKIP_SETTINGS_MIGRATION

void SYS_InitFileSystem()
{
	if (sysInitFileSystemDone == true)
		return;

	sysInitFileSystemDone = true;

	LOGM("SYS_InitFileSystem\n");

	std::string curDir = SYS_WinGetCurrentDirectoryUtf8();
	if (curDir.empty())
		curDir = ".";
	LOGD("curDir='%s'", curDir.c_str());

	SYS_AssignGlobalPath(&gPathToCurrentDirectory, &gCPathToCurrentDirectory,
		&gUTFPathToCurrentDirectory, curDir);

	std::string resourcesPath = curDir + "\\Resources\\";
	gPathToResources = SYS_DupUtf8Path(resourcesPath);
	LOGM("pathToResource=%s", gPathToResources);

	std::string documentsPath = SYS_WinGetSpecialFolderUtf8(CSIDL_MYDOCUMENTS);
	if (documentsPath.empty())
		documentsPath = curDir;
	SYS_AssignGlobalPath(&gPathToDocuments, &gCPathToDocuments,
		&gUTFPathToDocuments, documentsPath);
	LOGM("pathToDocuments=%s", gPathToDocuments);

	std::string tempPath = SYS_WinGetTempPathUtf8();
	if (tempPath.empty())
		tempPath = curDir;
	SYS_AssignGlobalPath(&gPathToTemp, &gCPathToTemp,
		&gUTFPathToTemp, tempPath);
	LOGM("gPathToTemp=%s", gPathToTemp);
	
#if defined(SKIP_SETTINGS_MIGRATION)
	// use old code for settings location
	std::string commonAppData = SYS_WinGetSpecialFolderUtf8(CSIDL_COMMON_APPDATA);
	if (commonAppData.empty())
	{
		LOGError("failed to get app setings folder");
		SYS_AssignGlobalPath(&gPathToSettings, &gCPathToSettings,
			&gUTFPathToSettings, curDir + "\\");
	}
	else
	{
		const char *settingsFolderName = MT_GetSettingsFolderName();
		std::string settingsPath = commonAppData + "\\" + settingsFolderName + "\\";

		// check if folder exists & create new if needed
		if (!SYS_WinPathExistsUtf8(settingsPath))
		{
			if (!SYS_WinCreateDirectoryUtf8(settingsPath))
			{
				LOGError("failed to create app setings folder");
				settingsPath = curDir + "\\";
			}
		}
		SYS_AssignGlobalPath(&gPathToSettings, &gCPathToSettings,
			&gUTFPathToSettings, settingsPath);
	}

#else
	// upgrade and move settings from old location to a new location
	// we need to move from CSIDL_COMMON_APPDATA to CSIDL_LOCAL_APPDATA as the CSIDL_COMMON_APPDATA is not writable anymore in new Windows (we all love Windows don't we)
	std::string localAppData = SYS_WinGetSpecialFolderUtf8(CSIDL_LOCAL_APPDATA);
	if (localAppData.empty())
	{
		LOGError("failed to get app setings folder: CSIDL_LOCAL_APPDATA");
		SYS_AssignGlobalPath(&gPathToSettings, &gCPathToSettings,
			&gUTFPathToSettings, curDir + "\\");
	}
	else
	{
		const char *settingsFolderName = MT_GetSettingsFolderName();
		std::string settingsPath = localAppData + "\\" + settingsFolderName + "\\";
		
		// check if folder exists & create new if needed
		if (!SYS_WinPathExistsUtf8(settingsPath))
		{
			// settings folder does not exist
			if (!SYS_WinCreateDirectoryUtf8(settingsPath))
			{
				LOGError("failed to create app setings folder");
				settingsPath = curDir + "\\";
			}
			else
			{
				// created new settings folder, check if we need to move from old location
				std::string commonAppData = SYS_WinGetSpecialFolderUtf8(CSIDL_COMMON_APPDATA);
				if (commonAppData.empty())
				{
					LOGError("failed to get app setings folder: CSIDL_COMMON_APPDATA");
				}
				else
				{
					std::string oldSettingsFolder = commonAppData + "\\" + settingsFolderName + "\\";

					if (SYS_FileExists(oldSettingsFolder.c_str()))
					{
						LOGM("Upgrading settings path, moving settings from %s to %s", oldSettingsFolder.c_str(), settingsPath.c_str());
						
						// we need to move all settings files from old folder
						SYS_MoveFilesAndDeleteSource(oldSettingsFolder, settingsPath);
	//					SYS_MoveFilesFromFolderAtoFolderB(folderFrom, folderTo);
						
						// remove old settings folder
//						RemoveDirectory(oldSettingsFolder);
					}
				}
			}
		}
		SYS_AssignGlobalPath(&gPathToSettings, &gCPathToSettings,
			&gUTFPathToSettings, settingsPath);
	}

#endif
	
	LOGM("pathToSettings=%s", gPathToSettings);
}

bool SYS_MoveFilesAndDeleteSource(const std::string& source, const std::string& destination)
{
	try
	{
		fs::path sourcePath(SYS_WinUtf8ToWide(source.c_str()));
		fs::path destinationPath(SYS_WinUtf8ToWide(destination.c_str()));
		fs::create_directory(destinationPath);

		for (const auto& entry : fs::directory_iterator(sourcePath))
		{
			const auto& path = entry.path();
			auto dest = destinationPath / path.filename();

			if (fs::is_directory(path))
			{
				std::string fromUtf8 = SYS_WinWideToUtf8(path.wstring().c_str());
				std::string toUtf8 = SYS_WinWideToUtf8(dest.wstring().c_str());
				if (!SYS_MoveFilesAndDeleteSource(fromUtf8, toUtf8))
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
	
	WIN32_FIND_DATAW findFileData;
	std::wstring searchPath = SYS_WinUtf8PathToWideLong((folderA + "\\*").c_str());
	HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findFileData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		LOGError("FindFirstFile failed with error %lu", GetLastError());
		return;
	}

	do
	{
		// Skip directories
		if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			continue;
		}

		std::string fileName = SYS_WinWideToUtf8(findFileData.cFileName);
		std::string sourceFile = folderA + "\\" + fileName;
		std::string destFile = folderB + "\\" + fileName;
		std::wstring sourceWide = SYS_WinUtf8PathToWideLong(sourceFile.c_str());
		std::wstring destWide = SYS_WinUtf8PathToWideLong(destFile.c_str());

		// Move the file
		if (!MoveFileW(sourceWide.c_str(), destWide.c_str()))
		{
			LOGError("Failed to move file: %s with error %lu", sourceFile.c_str(), GetLastError());
		}

	}
	while (FindNextFileW(hFind, &findFileData) != 0);

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

	WIN32_FIND_DATAW ffd;
	LARGE_INTEGER filesize;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	DWORD dwError =0;

	std::string directoryUtf8 = SYS_WindowsPathBackslashes(directoryPath ? directoryPath : "");
	std::string searchUtf8 = directoryUtf8;
	if (!searchUtf8.empty() && searchUtf8.back() != '\\')
		searchUtf8 += "\\";
	searchUtf8 += "*";
	std::wstring searchWide = SYS_WinUtf8PathToWideLong(searchUtf8.c_str());

	hFind = FindFirstFileW(searchWide.c_str(), &ffd);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		LOGError("CFileSystem::GetFiles: FindFirstFileW failed for %s", directoryUtf8.c_str());
		return files;
	}

	do
	{
		std::string entryName = SYS_WinWideToUtf8(ffd.cFileName);
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (withFolders)
			{
				LOGD("<DIR> %s", entryName.c_str());

				if (entryName == "." || entryName == "..")
				{
				}
				else
				{
					std::string fullPath = directoryUtf8;
					if (!fullPath.empty() && fullPath.back() != '\\')
						fullPath += "\\";
					fullPath += entryName;
					LOGD("the full path name is %s", fullPath.c_str());

					char *modDateDup = strdup("");

					CFileItem *item = new CFileItem((char*)entryName.c_str(), (char*)fullPath.c_str(), modDateDup, true);
					files->push_back(item);
				}
			}
		}
		else
		{
			if (extensions != NULL)
			{
				char *fileExtension = SYS_FileSystemGetExtension((char*)entryName.c_str());

				if (fileExtension != NULL)
				{
					for (std::list<char *>::iterator itExtensions = extensions->begin();
						 itExtensions !=  extensions->end(); itExtensions++)
					{
						char *extension = *itExtensions;

						//LOGD("fileExtension='%s' extension='%s'", fileExtension, extension);
						if (!strcmp(extension, fileExtension))
						{
							filesize.LowPart = ffd.nFileSizeLow;
							filesize.HighPart = ffd.nFileSizeHigh;
							LOGD("     %s %ld", entryName.c_str(), filesize.QuadPart);

							std::string fullPath = directoryUtf8;
							if (!fullPath.empty() && fullPath.back() != '\\')
								fullPath += "\\";
							fullPath += entryName;

							CFileItem *item = new CFileItem((char*)entryName.c_str(), (char*)fullPath.c_str(), (char*)"", false);
							files->push_back(item);
							break;
						}
					}
					free(fileExtension);
				}
			}
			else
			{
				std::string fullPath = directoryUtf8;
				if (!fullPath.empty() && fullPath.back() != '\\')
					fullPath += "\\";
				fullPath += entryName;

				LOGD("the full path name is %s", fullPath.c_str());

				CFileItem *item = new CFileItem((char*)entryName.c_str(), (char*)fullPath.c_str(), (char*)"<mod date>", false);
				files->push_back(item);
			}
		}
	}
	while(FindNextFileW(hFind, &ffd) != 0);

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
	if (path == NULL || mode == NULL)
		return NULL;

	std::wstring wpath = SYS_WinUtf8PathToWideLong(path);
	std::wstring wmode = SYS_WinUtf8ToWide(mode);
	if (wpath.empty() || wmode.empty())
		return NULL;

	return _wfopen(wpath.c_str(), wmode.c_str());
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

	OPENFILENAMEW ofn;
	std::vector<wchar_t> selectedFileBuffer(65536, L'\0');

	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsMainWindowAlwaysOnTop();
	//VID_SetWindowAlwaysOnTopTemporary(false);

    ZeroMemory(&ofn, sizeof(ofn));

    ofn.lStructSize = sizeof(ofn); // SEE NOTE BELOW
    ofn.hwndOwner = hWnd;

	std::wstring filterWide;
	std::wstring titleWide;
	std::wstring initialFolderWide;

	if (extensions != NULL)
	{
		std::string filterExtAll;
		std::string filterExtSingle;
		
		for (std::list<CSlrString *>::iterator it = extensions->begin();
			it != extensions->end(); it++)
		{
			CSlrString *extStr = *it;
			std::string ext = SYS_CSlrStringToUtf8(extStr);

			if (it == extensions->begin())
			{
				filterExtAll += "*.";
			}
			else
			{
				filterExtAll += ";*.";
			}
			filterExtAll += ext;

			filterExtSingle += "Only ";
			filterExtSingle += ext;
			filterExtSingle += " files$*.";
			filterExtSingle += ext;
			filterExtSingle += "$";
		}
		
		std::string filterUtf8;
		if (extensions->size() == 1)
		{
			filterUtf8 = "Supported files$" + filterExtAll + "$All Files(*.*)$*.*$";
		}
		else
		{
			filterUtf8 = "Supported files$" + filterExtAll + "$" + filterExtSingle + "All Files(*.*)$*.*$";
		}

		filterWide = SYS_WinDialogFilterToWide(filterUtf8);
		ofn.lpstrFilter = filterWide.c_str();
	    ofn.lpstrDefExt = NULL;
	}

	if (windowTitle != NULL)
	{
		titleWide = SYS_WinUtf8ToWide(SYS_CSlrStringToUtf8(windowTitle).c_str());
		ofn.lpstrTitle = titleWide.c_str();
	}

	    ofn.lpstrFile = selectedFileBuffer.data();

	if (defaultFolder != NULL)
	{
		std::string initialFolder = SYS_CSlrStringToUtf8(defaultFolder);
		initialFolderWide = SYS_WinUtf8ToWide(initialFolder.c_str());
		ofn.lpstrInitialDir = initialFolderWide.c_str();

		LOGD(">> set ofn.lpstrInitialDir='%s'", initialFolder.c_str());
	}
	else
	{
		LOGD(">> defaultFolder is NULL");
	}

	ofn.nMaxFile = (DWORD)selectedFileBuffer.size();
	ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT;
    
    // workaround
	GUI_KeyUpAllModifiers();
    
	LOGD("...... GetOpenFileName");
	if(GetOpenFileNameW(&ofn))
    {
    	LOGD("..... callback: file open selected");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		std::vector<std::string> selectedUtf8 = SYS_WinDialogBufferToPathsUtf8(selectedFileBuffer.data());

		std::vector<CSlrString *> selectedPaths;
		for (const std::string &path : selectedUtf8)
		{
			LOGD("selected file='%s'", path.c_str());
			selectedPaths.push_back(new CSlrString(path));
		}

		if (selectedPaths.empty())
			callback->SystemDialogFileOpenCancelled();
		else
			callback->SystemDialogFilesOpenSelected(&selectedPaths);

		for (CSlrString *path : selectedPaths)
			delete path;
	}
	else
	{
		LOGD("..... callback: file open cancelled");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		callback->SystemDialogFileOpenCancelled();
	}
}

void SYS_DialogOpenFiles(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFolder, CSlrString *windowTitle)
{
	SYS_DialogOpenFile(callback, extensions, defaultFolder, windowTitle);
}

void SYS_DialogSaveFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFileName, CSlrString *defaultFolder, CSlrString *windowTitle)
{
	OPENFILENAMEW ofn;
	std::vector<wchar_t> selectedFileBuffer(65536, L'\0');

	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsMainWindowAlwaysOnTop();
	//VID_SetWindowAlwaysOnTopTemporary(false);

    ZeroMemory(&ofn, sizeof(ofn));

    ofn.lStructSize = sizeof(ofn); // SEE NOTE BELOW
    ofn.hwndOwner = hWnd;

	std::wstring filterWide;
	std::wstring defExtWide;
	std::wstring titleWide;
	std::wstring initialFolderWide;

	if (extensions != NULL)
	{
		std::string filterExtAll;
		std::string filterExtSingle;
		std::string defExt;
		
		for (std::list<CSlrString *>::iterator it = extensions->begin();
			it != extensions->end(); it++)
		{
			CSlrString *extStr = *it;
			std::string ext = SYS_CSlrStringToUtf8(extStr);

			if (it == extensions->begin())
			{
				filterExtAll += "*.";
				defExt = ext;
			}
			else
			{
				filterExtAll += ";*.";
			}
			filterExtAll += ext;

			filterExtSingle += "Only ";
			filterExtSingle += ext;
			filterExtSingle += " files$*.";
			filterExtSingle += ext;
			filterExtSingle += "$";
		}
		
		std::string filterUtf8;
		if (extensions->size() == 1)
		{
			filterUtf8 = defExt + " file$" + filterExtAll + "$All Files(*.*)$*.*$";
		}
		else
		{
			filterUtf8 = "Supported files$" + filterExtAll + "$" + filterExtSingle + "All Files(*.*)$*.*$";
		}

		filterWide = SYS_WinDialogFilterToWide(filterUtf8);
		defExtWide = SYS_WinUtf8ToWide(defExt.c_str());
		ofn.lpstrFilter = filterWide.c_str();
	    ofn.lpstrDefExt = defExtWide.empty() ? NULL : defExtWide.c_str();
	}

	if (windowTitle != NULL)
	{
		titleWide = SYS_WinUtf8ToWide(SYS_CSlrStringToUtf8(windowTitle).c_str());
		ofn.lpstrTitle = titleWide.c_str();
	}

	if (defaultFileName != NULL)
	{
		LOGD("defaultFileName != NULL");
		defaultFileName->DebugPrint("defaultFileName=");
		std::string fileName = SYS_CSlrStringToUtf8(defaultFileName);
		std::wstring fileNameWide = SYS_WinUtf8ToWide(fileName.c_str());
		size_t copyLen = std::min(fileNameWide.size(), selectedFileBuffer.size() - 1);
		wmemcpy(selectedFileBuffer.data(), fileNameWide.c_str(), copyLen);
		selectedFileBuffer[copyLen] = L'\0';
		LOGD("szFileName=%s", fileName.c_str());
	}
	    ofn.lpstrFile = selectedFileBuffer.data();

	if (defaultFolder != NULL)
	{
		initialFolderWide = SYS_WinUtf8ToWide(SYS_CSlrStringToUtf8(defaultFolder).c_str());
		ofn.lpstrInitialDir = initialFolderWide.c_str();
	}
	    ofn.nMaxFile = (DWORD)selectedFileBuffer.size();
	ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
	
	
	// workaround
	GUI_KeyUpAllModifiers();
	
	LOGD("....... GetSaveFileName");
    if(GetSaveFileNameW(&ofn))
    {
    	LOGD("     ...callback OK");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		std::string outPathUtf8 = SYS_WinWideToUtf8(selectedFileBuffer.data());
		LOGD("szFileName='%s'", outPathUtf8.c_str());
		CSlrString *outPath = new CSlrString(outPathUtf8);
		callback->SystemDialogFileSaveSelected(outPath);
		delete outPath;
	}
	else
	{
		LOGD("    ...callback cancelled");
		VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		callback->SystemDialogFileSaveCancelled();
	}
}

void SYS_DialogPickFolder(CSystemFileDialogCallback* callback, CSlrString* defaultFolder)
{
	char* defaultPath = NULL;
	if (defaultFolder)
	{
		defaultPath = defaultFolder->GetUTF8();
	}
	else
	{
		defaultPath = STRALLOC(gPathToDocuments);
	}
	char* outPath = NULL;
	nfdresult_t result = NFD_PickFolder(defaultPath, &outPath);
	if (result == NFD_OKAY)
	{
		CSlrString* path = new CSlrString(std::string(outPath));
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
	free(defaultPath);
}

// Move the file to the Recycle Bin via SHFileOperation (FOF_ALLOWUNDO).
// Never falls back to permanent delete on failure.
namespace {

// Converts a wide string to UTF-8.
std::string PC_WideToUtf8(const std::wstring &w)
{
	if (w.empty()) return std::string();
	int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
	                              nullptr, 0, nullptr, nullptr);
	if (len <= 0) return std::string();
	std::string s(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
	                    &s[0], len, nullptr, nullptr);
	return s;
}

// IFileOperation progress sink: captures the path the deleted file lands at in
// the Recycle Bin (the "$R..." item), reported via PostDeleteItem's
// psiNewlyCreated. This is what makes Windows trash-undo possible.
class PCTrashSink : public IFileOperationProgressSink
{
public:
	std::wstring recycledPath;

	IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr) return E_POINTER;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IFileOperationProgressSink))
		{
			*ppv = static_cast<IFileOperationProgressSink *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	IFACEMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&ref_); }
	IFACEMETHODIMP_(ULONG) Release() override
	{
		LONG c = InterlockedDecrement(&ref_);
		if (c == 0) delete this;
		return c;
	}

	IFACEMETHODIMP StartOperations() override { return S_OK; }
	IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
	IFACEMETHODIMP PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
	IFACEMETHODIMP PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
	IFACEMETHODIMP PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
	IFACEMETHODIMP PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
	IFACEMETHODIMP PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
	IFACEMETHODIMP PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
	IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem *) override { return S_OK; }
	IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem *, HRESULT hr, IShellItem *psiNewlyCreated) override
	{
		if (SUCCEEDED(hr) && psiNewlyCreated != nullptr)
		{
			PWSTR p = nullptr;
			if (SUCCEEDED(psiNewlyCreated->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
			{
				recycledPath = p;
				CoTaskMemFree(p);
			}
		}
		return S_OK;
	}
	IFACEMETHODIMP PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
	IFACEMETHODIMP PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) override { return S_OK; }
	IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
	IFACEMETHODIMP ResetTimer()  override { return S_OK; }
	IFACEMETHODIMP PauseTimer()  override { return S_OK; }
	IFACEMETHODIMP ResumeTimer() override { return S_OK; }

private:
	LONG ref_ = 1;
};

} // namespace

bool SYS_FileDeleteToTrash(const char *path, std::string *outTrashPath, std::string *outError)
{
	// Convert UTF-8 -> wide (shell APIs take a normal path, no \\?\ prefix).
	int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
	if (wlen <= 0)
	{
		if (outError) *outError = "path conversion failed";
		return false;
	}
	std::wstring wpath(wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);
	if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

	// Ensure COM is available on this thread. If it is already initialized (even
	// with a different apartment model) we proceed and skip the balancing uninit.
	HRESULT hrInit  = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	bool    needUninit = (hrInit == S_OK || hrInit == S_FALSE);

	bool         ok = false;
	std::string  errMsg;
	std::wstring recycled;

	IFileOperation *op = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
	                              IID_PPV_ARGS(&op));
	if (SUCCEEDED(hr) && op != nullptr)
	{
		// FOF_ALLOWUNDO routes the delete to the Recycle Bin; FOF_NO_UI keeps it
		// silent (no confirmation/progress/error dialogs).
		op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NO_UI);

		IShellItem *item = nullptr;
		hr = SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&item));
		if (SUCCEEDED(hr) && item != nullptr)
		{
			PCTrashSink *sink = new PCTrashSink();
			DWORD cookie = 0;
			op->Advise(sink, &cookie);

			hr = op->DeleteItem(item, nullptr);
			if (SUCCEEDED(hr)) hr = op->PerformOperations();

			BOOL aborted = FALSE;
			if (SUCCEEDED(hr)) op->GetAnyOperationsAborted(&aborted);

			op->Unadvise(cookie);

			if (SUCCEEDED(hr) && !aborted)
			{
				ok       = true;
				recycled = sink->recycledPath;   // "" if the shell did not report it
			}
			else
			{
				errMsg = "IFileOperation delete failed (hr=0x" + std::to_string((unsigned long)hr) + ")";
			}
			sink->Release();
			item->Release();
		}
		else
		{
			errMsg = "SHCreateItemFromParsingName failed (hr=0x" + std::to_string((unsigned long)hr) + ")";
		}
		op->Release();
	}
	else
	{
		errMsg = "CoCreateInstance(FileOperation) failed (hr=0x" + std::to_string((unsigned long)hr) + ")";
	}

	if (needUninit) CoUninitialize();

	if (!ok)
	{
		if (outError) *outError = errMsg;
		return false;
	}
	if (outTrashPath && !recycled.empty())
		*outTrashPath = PC_WideToUtf8(recycled);
	return true;
}

bool SYS_FileExists(const char *cPath)
{
	if (cPath == NULL)
		return false;

	LOGD("SYS_FileExists, cPath='%s'", cPath);
	
	DWORD attr = GetFileAttributesW(SYS_WinUtf8PathToWideLong(cPath).c_str());
	if(attr == INVALID_FILE_ATTRIBUTES)
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

	char *cPath = path->GetUTF8();
	bool exists = SYS_FileExists(cPath);
	free(cPath);
	return exists;
}

bool SYS_FileDirExists(const char *cPath)
{
	if (cPath == NULL)
		return false;

	DWORD attr = GetFileAttributesW(SYS_WinUtf8PathToWideLong(cPath).c_str());
	if(attr == INVALID_FILE_ATTRIBUTES)
		return false;
	return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool SYS_FileDirExists(CSlrString *path)
{
	char *cPath = path->GetUTF8();
	bool exists = SYS_FileDirExists(cPath);
	free(cPath);
	return exists;
}

uint8 *SYS_MapMemoryToFile(int memorySize, char *filePath, void **fileDescriptor)
{
	int *fileHandle = (int*)malloc(sizeof(int));
	fileDescriptor = (void**)(&fileHandle);
	
	std::wstring wpath = SYS_WinUtf8PathToWideLong(filePath);
	*fileHandle = _wopen(wpath.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_BINARY, _S_IREAD | _S_IWRITE);
	
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
	SYS_WinCreateDirectoryUtf8(path ? path : "");
}

void SYS_CreateFolder(CSlrString *path)
{
	LOGTODO("SYS_CreateFolder");
	path->DebugPrint("SYS_CreateFolder: ");
	char *cPath = path->GetUTF8();
	SYS_WinCreateDirectoryUtf8(cPath);

	free(cPath);
}

void SYS_SetCurrentFolder(CSlrString *path)
{
	LOGD("SYS_SetCurrentFolder");
	path->DebugPrint("SYS_SetCurrentFolder: ");
	char *cPath = path->GetUTF8();
	SetCurrentDirectoryW(SYS_WinUtf8PathToWideLong(cPath).c_str());

	free(cPath);
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

#ifndef WIN32
#include <unistd.h>
#endif
#ifdef WIN32
#define stat _stat
#endif

long SYS_GetFileModifiedTime(const char* filePath)
{
	struct _stat64 result;
	std::wstring wpath = SYS_WinUtf8PathToWideLong(filePath);
	if (_wstat64(wpath.c_str(), &result) == 0)
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

std::string SYS_GetRelativePath(const char* pathToFolder, const char* pathToFile)
{
	fs::path base = fs::absolute(fs::path(SYS_WinUtf8ToWide(pathToFolder)));
	fs::path target = fs::absolute(fs::path(SYS_WinUtf8ToWide(pathToFile)));

	return SYS_WinWideToUtf8(fs::relative(target, base).generic_wstring().c_str());
}

std::string SYS_GetAbsolutePath(const char* pathToFolder, const char* relativePath)
{
	fs::path base = fs::absolute(fs::path(SYS_WinUtf8ToWide(pathToFolder)));
	fs::path relative = fs::path(SYS_WinUtf8ToWide(relativePath));

	fs::path fullPath = fs::absolute(base / relative);
	return SYS_WinWideToUtf8(fullPath.generic_wstring().c_str());
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
