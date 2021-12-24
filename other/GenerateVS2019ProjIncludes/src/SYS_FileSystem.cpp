#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <algorithm>
#include <functional>
#include <Shlobj.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include "SYS_FileSystem.h"

#define LOGError printf
//#define LOGD printf
#define LOGD {}

#define u32 unsigned int

// we all love Microsoft, don't we?
#define strdup _strdup
#define BUFSIZE 4096

CFileItem::CFileItem(const char* name, const char* fullPath, const char *filterName, bool isDir)
{
	this->name = name;
	this->fullPath = fullPath;
	this->filterName = filterName;
	this->isDir = isDir;
}

CFileItem::~CFileItem()
{
	delete this->name;
}

// comparison, not case sensitive.
bool compare_CFileItem_nocase(CFileItem* first, CFileItem* second)
{
	if (first->isDir == second->isDir)
	{
		unsigned int i = 0;
		u32 l1 = strlen(first->name);
		u32 l2 = strlen(second->name);
		while ((i < l1) && (i < l2))
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

std::vector<CFileItem*>* SYS_GetFoldersInFolder(const char* directoryPath)
{
	LOGD("CFileSystem::GetFiles: %s\n", directoryPath);
	std::vector<CFileItem*>* files = new std::vector<CFileItem*>();

	WIN32_FIND_DATA ffd;
	LARGE_INTEGER filesize;
	TCHAR szDir[MAX_PATH];
	size_t length_of_arg;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	DWORD dwError = 0;

	DWORD  retval = 0;
	BOOL   success;
	TCHAR  buffer[BUFSIZE] = TEXT("");
	TCHAR  buf[BUFSIZE] = TEXT("");
	TCHAR** lppPart = { NULL };

	StringCchLength(directoryPath, MAX_PATH, &length_of_arg);
	if (length_of_arg > (MAX_PATH - 3))
	{
		LOGError("CFileSystem::GetFiles: directoryPath too long\n");
		exit(-1);
	}

	StringCchCopy(szDir, MAX_PATH, directoryPath);
	StringCchCat(szDir, MAX_PATH, TEXT("\\*"));

	hFind = FindFirstFile(szDir, &ffd);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		LOGError("CFileSystem::GetFiles: FindFirstFile\n");
		exit(-1);
	}

	do
	{
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			LOGD("<DIR> %s\n", ffd.cFileName);

			if (!strcmp(ffd.cFileName, ".") || !strcmp(ffd.cFileName, ".."))
			{
			}
			else
			{
				char* fileNameDup = _strdup(ffd.cFileName);

				// oh, I forgot that Windows is for retards (as Gideon said)
				//char *fileFullPath = strdup(ffd.path)

				char* fileFullPath = new char[MAX_PATH];
				sprintf(fileFullPath, "%s\\%s", directoryPath, fileNameDup);
				LOGD("the full path name is %s\n", fileFullPath);

				CFileItem* item = new CFileItem(fileNameDup, fileFullPath, NULL, true);
				files->push_back(item);
			}
		}
		
	} while (FindNextFile(hFind, &ffd) != 0);

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

std::vector<CFileItem*>* SYS_GetFilesInFolder(const char* directoryPath, std::list<const char*>* extensions)
{
	LOGD("CFileSystem::GetFiles: %s\n", directoryPath);
	std::vector<CFileItem*>* files = new std::vector<CFileItem*>();

	WIN32_FIND_DATA ffd;
	LARGE_INTEGER filesize;
	TCHAR szDir[MAX_PATH];
	size_t length_of_arg;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	DWORD dwError = 0;

	DWORD  retval = 0;
	BOOL   success;
	TCHAR  buffer[BUFSIZE] = TEXT("");
	TCHAR  buf[BUFSIZE] = TEXT("");
	TCHAR** lppPart = { NULL };

	StringCchLength(directoryPath, MAX_PATH, &length_of_arg);
	if (length_of_arg > (MAX_PATH - 3))
	{
		LOGError("CFileSystem::GetFiles: directoryPath too long\n");
		exit(-1);
	}

	StringCchCopy(szDir, MAX_PATH, directoryPath);
	StringCchCat(szDir, MAX_PATH, TEXT("\\*"));

	hFind = FindFirstFile(szDir, &ffd);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		LOGError("CFileSystem::GetFiles: FindFirstFile\n");
		exit(-1);
	}

	do
	{
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			LOGD("<DIR> %s\n", ffd.cFileName);

			if (!strcmp(ffd.cFileName, ".") || !strcmp(ffd.cFileName, ".."))
			{
			}
			else
			{
				char* fileNameDup = _strdup(ffd.cFileName);

				// oh, I forgot that Windows is for retards (as Gideon said)
				//char *fileFullPath = strdup(ffd.path)

				char* fileFullPath = new char[MAX_PATH];
				sprintf(fileFullPath, "%s\\%s", directoryPath, fileNameDup);
				LOGD("the full path name is %s\n", fileFullPath);

				char* modDateDup = strdup("");

				CFileItem* item = new CFileItem(fileNameDup, fileFullPath, modDateDup, true);
				files->push_back(item);
			}
		}
		else
		{
			char* fileExtension = SYS_FileSystemGetExtension(ffd.cFileName);

			LOGD("fileExtension=%s\n", fileExtension);
			if (fileExtension != NULL)
			{
				for (std::list<const char*>::iterator itExtensions = extensions->begin();
					itExtensions != extensions->end(); itExtensions++)
				{
					const char* extension = *itExtensions;

					//LOGD("fileExtension='%s' extension='%s'", fileExtension, extension);
					if (!strcmp(extension, fileExtension))
					{
						//LOGD("adding");
						//LOGD(fname);

						filesize.LowPart = ffd.nFileSizeLow;
						filesize.HighPart = ffd.nFileSizeHigh;
						LOGD("     %s %ld\n", ffd.cFileName, filesize.QuadPart);

						char* fileNameDup = strdup(ffd.cFileName);

						// note, this below is copy pasted on purpose, because on this shitty platform I fucking do not care (as Windows devs also do).

						// oh, I forgot that Windows is for retards (as Gideon said)
						//char *fileFullPath = strdup(ffd.path)

						retval = GetFullPathName(ffd.cFileName, BUFSIZE, buffer, lppPart);

						if (retval == 0)
						{
							// Handle an error condition.
							LOGError("GetFullPathName failed (%d)\n", GetLastError());
							return NULL;
						}
						char* fileFullPath = new char[MAX_PATH];
						sprintf(fileFullPath, "%s\\%s", directoryPath, fileNameDup);
						LOGD("the full path name is %s\n", fileFullPath);

						char* modDateDup = strdup("");

						CFileItem* item = new CFileItem(fileNameDup, fileFullPath, modDateDup, false);
						files->push_back(item);
						break;
					}
				}
				free(fileExtension);
			}
		}
	} while (FindNextFile(hFind, &ffd) != 0);

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