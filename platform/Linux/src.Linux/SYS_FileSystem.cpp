/*
 *  SYS_FileSystem.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-20.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "SYS_FileSystem.h"
#include "SYS_DocsVsRes.h"
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <functional>
#include "CSlrString.h"
#include "CGuiMain.h"
#include "SYS_Funct.h"
#include "VID_Main.h"
#include "MT_API.h"

#include <gtk/gtk.h>
#include <pwd.h>
#include <sys/mman.h>
#include "nfd.h"

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


std::list<CHttpFileUploadedCallback *> httpFileUploadedCallbacks;

void SYS_InitFileSystem()
{
	LOGM("SYS_InitFileSystem");

	// get current folder
	gPathToCurrentDirectory = new char[PATH_MAX];
	getcwd(gPathToCurrentDirectory, PATH_MAX);

	LOGD("gPathToCurrentDirectory=%s", gPathToCurrentDirectory);

	gCPathToCurrentDirectory = gPathToCurrentDirectory;
	gUTFPathToCurrentDirectory = new CSlrString(gCPathToCurrentDirectory);

	gPathToResources = new char[PATH_MAX];

//#if !defined(USE_DOCS_INSTEAD_OF_RESOURCES)
//	sprintf(gPathToResources, "./Resources/");
//#else
	sprintf(gPathToResources, "./Documents/");
//#endif

	LOGD("pathToResources=%s", gPathToResources);



	gPathToDocuments = new char[PATH_MAX];
	sprintf(gPathToDocuments, "./Documents/");
	gCPathToDocuments = gPathToDocuments;
	gUTFPathToDocuments = new CSlrString(gCPathToDocuments);


	LOGD("pathToDocuments=%s", gPathToDocuments);

	gPathToTemp = new char[PATH_MAX];
//	sprintf(gPathToTemp, "./Temp/");
	sprintf(gPathToTemp, "./Documents/");
	gCPathToTemp = gPathToTemp;
	gUTFPathToTemp = new CSlrString(gCPathToTemp);
	LOGD("gPathToTemp=%s", gPathToTemp);

	const char *homeDir;

	if ((homeDir = getenv("HOME")) == NULL)
	{
	    homeDir = getpwuid(getuid())->pw_dir;
	}

	const char *settingsFolderName = MT_GetSettingsFolderName();

	gPathToSettings = new char[PATH_MAX];
	sprintf(gPathToSettings, "%s/.%s/", homeDir, settingsFolderName);
	LOGD("pathToSettings=%s", gPathToSettings);
	gCPathToSettings = gPathToSettings;
	gUTFPathToSettings = new CSlrString(gCPathToSettings);

	struct stat st = {0};
	if (stat(gCPathToSettings, &st) == -1)
	{
		LOGD("create settings folder: %s", gCPathToSettings);
	    int result = mkdir(gCPathToSettings, S_IRUSR | S_IWUSR | S_IXUSR);

	    LOGD("result=%d", result);
	}
}

char *SYS_GetPathToDocuments()
{
	return gCPathToDocuments;
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

std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions)
{
	LOGD("SYS_GetFilesInFolder::GetFiles: directoryPath=%s", directoryPath);
	std::vector<CFileItem *> *files = new std::vector<CFileItem *>();

	DIR *dp;
    struct dirent *dirp;

    if((dp  = opendir(directoryPath)) == NULL)	//dir.c_str()
    	SYS_FatalExit("Error opening dir: %s", directoryPath);

    while((dirp = readdir(dp)) != NULL)
    {
    	if (!strcmp(dirp->d_name, "..") || !strcmp(dirp->d_name, "."))
    	{
    		continue;
    	}

    	char buf[1024];
    	sprintf(buf, "%s%c%s", directoryPath, SYS_FILE_SYSTEM_PATH_SEPARATOR, dirp->d_name);
    	LOGD("buf (d_name)=%s", buf);
		SYS_FixFileNameSlashes(buf);
    	struct stat st;
    	lstat(buf, &st);

    	if (S_ISDIR(st.st_mode))
    	{
    		LOGD("<DIR> %s", dirp->d_name);

    		char *fileNameDup = strdup(dirp->d_name);
			char *modDateDup = strdup("");

			CFileItem *item = new CFileItem(fileNameDup, buf, modDateDup, true);
			files->push_back(item);
    	}
    	else if (dirp->d_type == DT_REG || dirp->d_type == DT_UNKNOWN)
		{
			if (extensions != NULL)
			{
				char *fileExtension = SYS_GetFileExtension(dirp->d_name);

				if (fileExtension != NULL)
				{
					for (std::list<char *>::iterator itExtensions = extensions->begin();
									itExtensions !=  extensions->end(); itExtensions++)
					{
						char *extension = *itExtensions;

						if (!strcmp(extension, fileExtension))
						{
							LOGD("adding file %s", dirp->d_name);
							//LOGD(fname);

							//filesize.LowPart = ffd.nFileSizeLow;
							//filesize.HighPart = ffd.nFileSizeHigh;
							//LOGD("     %s %ld", ffd.cFileName, filesize.QuadPart);
							//LOGD("     %s", dirp->d_name);

							char *fileNameDup = strdup(dirp->d_name);
							char *modDateDup = strdup("");

							CFileItem *item = new CFileItem(fileNameDup, buf, modDateDup, false);
							files->push_back(item);
							break;
						}
					}

					free(fileExtension);
				}
			}
						else
			{
				CFileItem *item = new CFileItem(dirp->d_name, buf, (char*)"<mod date>", false);
				files->push_back(item);
			}
		}
    }

    closedir(dp);

	std::sort(files->begin(), files->end(), compare_CFileItem_nocase);

	LOGD("found %d files", files->size());

	LOGD("SYS_GetFilesInFolder done");

	return files;
}

char *SYS_GetFileExtension(char *fileName)
{
	LOGD("SYS_GetFileExtension: fileName=%s");
	int index = -1;
	for (int i = strlen(fileName)-1; i >= 0; i--)
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

	char *buf = (char*)malloc(strlen(fileName)-index+1);
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

char *SYS_GetFileName(char *filePath)
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
	
	const char *extension;
	char *fileName = new char [128];
	memset(fileName, 0x00, 128);
	extension = &filePath[offset_extension+1];
	memcpy(fileName, &filePath[offset_name+1], offset_extension - offset_name - 1);
	
	return fileName;
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

void CSystemFileDialogCallback::SystemDialogFileOpenSelected(CSlrString *path)
{
}

void CSystemFileDialogCallback::SystemDialogFileOpenCancelled()
{
}

void CSystemFileDialogCallback::SystemDialogFileSaveSelected(CSlrString *path)
{
}

void CSystemFileDialogCallback::SystemDialogFileSaveCancelled()
{
}

void CSystemFileDialogCallback::SystemDialogPickFolderSelected(CSlrString *path)
{
}

void CSystemFileDialogCallback::SystemDialogPickFolderCancelled()
{
}

bool SYS_windowAlwaysOnTopBeforeFileDialog = false;

//GTK_FILE_CHOOSER_ACTION_OPEN
//GTK_FILE_CHOOSER_ACTION_SAVE
//GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER
//GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER:

// example code in:
// https://sourceforge.net/p/xournal/svn/200/tree/trunk/xournalpp/src/control/stockdlg/XojOpenDlg.cpp#l49

void SYS_DialogOpenFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFolder, CSlrString *windowTitle)
{
	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsWindowAlwaysOnTop();
	VID_SetWindowAlwaysOnTopTemporary(false);

	char *defaultFolderStr = NULL;

	if (defaultFolder != NULL)
	{
		defaultFolderStr = defaultFolder->GetStdASCII();
	}
	else
	{
		defaultFolderStr = new char[32];
		sprintf(defaultFolderStr, "/");
	}

	char *filtersStr = SYS_GetCharBuf();
	for (std::list<CSlrString *>::iterator it = extensions->begin(); it != extensions->end(); it++)
	{
		CSlrString *filter = *it;
		char *filterStr = filter->GetStdASCII();
		strcat(filtersStr, filterStr);
		strcat(filtersStr, ",");
		delete [] filterStr;
	}

	// remove last ","
	filtersStr[strlen(filtersStr)-1] = 0x00;

	LOGD("SYS_DialogOpenFile: filtersStr=%s", filtersStr);

	nfdchar_t *outPathStr = NULL;
	nfdresult_t result = NFD_OpenDialog( filtersStr, defaultFolderStr, &outPathStr );	

	if (result == NFD_OKAY)
	{
		VID_SetWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		CSlrString *outPath = new CSlrString(outPathStr);
		callback->SystemDialogFileOpenSelected(outPath);
	}
	else if (result == NFD_CANCEL)
	{
		VID_SetWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

		callback->SystemDialogFileOpenCancelled();
	}
	else
	{
		guiMain->ShowMessageBox("Error", NFD_GetError());
	}

	delete [] defaultFolderStr;
	SYS_ReleaseCharBuf(filtersStr);
}

void SYS_DialogSaveFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions, CSlrString *defaultFileName, CSlrString *defaultFolder, CSlrString *windowTitle)
{
 	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsWindowAlwaysOnTop();
	VID_SetWindowAlwaysOnTopTemporary(false);

	char *defaultFolderStr = NULL;

        if (defaultFolder != NULL)
        {
                defaultFolderStr = defaultFolder->GetStdASCII();
        }
        
	CSlrString *defaultExtension = NULL;
        char *filtersStr = SYS_GetCharBuf();
        for (std::list<CSlrString *>::iterator it = extensions->begin(); it != extensions->end(); it++)
        {
                CSlrString *filter = *it;
		if (defaultExtension == NULL)
			defaultExtension = filter;

                char *filterStr = filter->GetStdASCII();
                strcat(filtersStr, filterStr);
                strcat(filtersStr, ",");
                delete [] filterStr;
        }

        // remove last ","
        filtersStr[strlen(filtersStr)-1] = 0x00;

        LOGD("SYS_DialogSaveFile: filtersStr=%s", filtersStr);

        nfdchar_t *outPathStr = NULL;
        nfdresult_t result = NFD_SaveDialog( filtersStr, defaultFolderStr, &outPathStr );

	if (result == NFD_OKAY)
        {
                VID_SetWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);
		
		LOGD("outPathStr=%s", outPathStr);
                CSlrString *outPath = new CSlrString(outPathStr);

		// TODO: workaround gtk_dialog does not add default extension...
                if (defaultExtension != NULL)
                {
                        outPath->Concatenate(".");
                        outPath->Concatenate(defaultExtension);
                }

                callback->SystemDialogFileSaveSelected(outPath);
        }   
        else if (result == NFD_CANCEL)
        {
                VID_SetWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);

                callback->SystemDialogFileSaveCancelled();
        }
        else
        {
                guiMain->ShowMessageBox("Error", NFD_GetError());
        }

        delete [] defaultFolderStr;
        SYS_ReleaseCharBuf(filtersStr);
}

void SYS_DialogPickFolder(CSystemFileDialogCallback *callback, CSlrString *defaultFolder)
{
	char *defaultPath = NULL;
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
                guiMain->ShowMessageBox("Error", NFD_GetError());
        }
}

bool SYS_FileExists(const char *path)
{
	if (path == NULL)
		return false;

	struct stat info;
	
	if(stat( path, &info ) != 0)
	{
		return false;
	}
	else
	{
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

        *fileHandle = open(filePath, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);

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
	LOGD("SYS_CreateFolder, path=%s", path);
	mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

void SYS_CreateFolder(CSlrString *path)
{
	LOGD("SYS_CreateFolder");
	path->DebugPrint("SYS_CreateFolder: ");
	char *cPath = path->GetStdASCII();
	mkdir(cPath, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	
	delete [] cPath;
}

void SYS_SetCurrentFolder(CSlrString *path)
{
	LOGD("SYS_SetCurrentFolder");

	path->DebugPrint("SYS_SetCurrentFolder: ");
	char *cPath = path->GetStdASCII();
	chdir(cPath);

	delete [] cPath;
}

const char *SYS_ExecSystemCommand(const char *cmd, int *terminationCode)
{
	LOGD("SYS_ExecSystemCommand: %s", cmd);
	char buffer[128];
	std::string resultStr = "";
	
	FILE* pipe = popen(cmd, "r");
	
	if (!pipe)
	{
		throw std::runtime_error("popen() failed!");
	}
	
	try
	{
		while (fgets(buffer, sizeof buffer, pipe) != NULL)
		{
			LOGD("buffer=%s", buffer);
			resultStr += buffer;
		}
	}
	catch (...)
	{
		pclose(pipe);
		throw;
	}
	
	*terminationCode = WEXITSTATUS(pclose(pipe));

	const char *retStr = STRALLOC(resultStr.c_str());
	return retStr;
}

long SYS_GetFileModifiedTime(const char *filePath)
{
	struct stat result;
	if(stat(filePath, &result)==0)
	{
		return (long)(result.st_mtime);
	}

	return 0;
}


