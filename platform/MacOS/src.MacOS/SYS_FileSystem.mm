/*
 *  SYS_CFileSystem.cpp
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 09-11-20.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

// iCloud: http://stackoverflow.com/questions/8375891/apple-icloud-implementation-on-mac-apps

#include "SYS_MacOS.h"
#include <AppKit/AppKit.h>
#include <cstdlib>

#include "SYS_FileSystem.h"
#include "TargetConditionals.h"
#include "CSlrFileFromResources.h"
#include "CSlrString.h"
#include "CSlrDate.h"
#include "MT_API.h"

#include "VID_Main.h"
#include "SYS_Funct.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <filesystem>


#include "nfd.h"


NSString *gOSPathToDocuments;
char *gPathToDocuments;
char *gCPathToDocuments;
CSlrString *gUTFPathToDocuments;
std::string gStdPathToDocuments;

NSString *gOSPathToTemp;
char *gPathToTemp;
char *gCPathToTemp;
CSlrString *gUTFPathToTemp;
std::string gStdPathToTemp;

NSString *gOSPathToResources;
char *gPathToResources;
char *gCPathToResources;
CSlrString *gUTFPathToResources;
std::string gStdPathToResources;

NSString *gOSPathToSettings;
char *gPathToSettings;
char *gCPathToSettings;
CSlrString *gUTFPathToSettings;
std::string gStdPathToSettings;

char *gPathToCurrentDirectory;
char *gCPathToCurrentDirectory;
CSlrString *gUTFPathToCurrentDirectory;
std::string gStdPathToCurrentDirectory;

std::list<CHttpFileUploadedCallback *> httpFileUploadedCallbacks;

void SYS_InitFileSystem()
{
	LOGM("SYS_InitFileSystem");
	//	NSString *folder = [path stringByExpandingTildeInPath];

#if defined(FINAL_RELEASE)
	// use resources in final release
	
	// get current folder
	gPathToCurrentDirectory = new char[PATH_MAX];
	getcwd(gPathToCurrentDirectory, PATH_MAX);
	int len = strlen(gPathToCurrentDirectory);
	gPathToCurrentDirectory[len] = '/';
	gPathToCurrentDirectory[len+1] = 0x00;
	
	LOGD("gPathToCurrentDirectory=%s", gPathToCurrentDirectory);
	
	gCPathToCurrentDirectory = gPathToCurrentDirectory;
	gUTFPathToCurrentDirectory = new CSlrString(gCPathToCurrentDirectory);
	gStdPathToCurrentDirectory = std::string(gPathToCurrentDirectory);
	
	NSError *error;
	
	/// get temp folder
//	NSString *bundleId = [[NSRunningApplication runningApplicationWithProcessIdentifier:getpid()] bundleIdentifier];
	
	const char *settingsFolderName = MT_GetSettingsFolderName();
	NSString *bundleId = [NSString stringWithUTF8String:settingsFolderName];
	
	NSURL *directoryURL = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:bundleId] isDirectory:YES];
	
	//NSLog(@"Create temp folder at: %@", directoryURL);
	[[NSFileManager defaultManager] createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:&error];
	
	gOSPathToTemp = [[NSString alloc] initWithFormat:@"%@/", [directoryURL path]];
	
	//NSLog(@"gOSPathToTemp=%@", gOSPathToTemp);
	
	/// get documents folder
	NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
	NSString *documentsDirectory = [paths firstObject];
	
	gOSPathToDocuments = [[NSString alloc] initWithFormat:@"%@/", documentsDirectory];
	
	//NSLog(@"gOSPathToDocuments=%@", gOSPathToDocuments);
	
	
	/// get settings folder
	paths = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES);		//NSApplicationSupportDirectory,
	NSString *settingsDirectory = [paths firstObject];
	directoryURL = [NSURL fileURLWithPath:[settingsDirectory stringByAppendingPathComponent:bundleId] isDirectory:YES];
	
//	NSLog(@"Create settings folder at: %@", directoryURL);
	[[NSFileManager defaultManager] createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:&error];
	
	gOSPathToSettings = [[NSString alloc] initWithFormat:@"%@/", [directoryURL path]];

//	NSLog(@"gOSPathToSettings=%@", gOSPathToSettings);

	
	// create CSlrString / ANSI-C versions
	const char *path = (const char *)[gOSPathToDocuments UTF8String];
	gCPathToDocuments = strdup(path);
	gPathToDocuments = gCPathToDocuments;
	gUTFPathToDocuments = FUN_ConvertNSStringToCSlrString(gOSPathToDocuments);
	gStdPathToDocuments = std::string([gOSPathToDocuments UTF8String]);
	
	const char *pathResources = (const char *)[gOSPathToDocuments UTF8String];
	gCPathToResources = strdup(path);
	gPathToResources = gCPathToDocuments;
	gUTFPathToResources = FUN_ConvertNSStringToCSlrString(gOSPathToDocuments);
	gStdPathToResources = std::string([gOSPathToDocuments UTF8String]);

	const char *pathTemp = (const char *)[gOSPathToTemp UTF8String];
	gCPathToTemp = strdup(pathTemp);
	gPathToTemp = gCPathToTemp;
	gUTFPathToTemp = FUN_ConvertNSStringToCSlrString(gOSPathToTemp);
	gStdPathToTemp = std::string([gOSPathToTemp UTF8String]);

	const char *pathSettings = (const char *)[gOSPathToSettings UTF8String];
	gCPathToSettings = strdup(pathSettings);
	gPathToSettings = gCPathToSettings;
	gUTFPathToSettings = FUN_ConvertNSStringToCSlrString(gOSPathToSettings);
	gStdPathToSettings = std::string([gOSPathToSettings UTF8String]);
	
	LOGD("gCPathToDocuments=%s", gCPathToDocuments);
	LOGD("gCPathToResources=%s", gCPathToResources);
	LOGD("gCPathToTemp=%s", gCPathToTemp);
	LOGD("gCPathToSettings=%s", gCPathToSettings);
#else
	
	/////////////////////////////////////
	/// development version, use Documents folder
	
//	//gOSPathToDocuments = @"/Users/mars/develop/RockinChristmasTree/_RUNTIME_/Documents/";
	gOSPathToDocuments = @"/Users/mars/develop/MTEngine/_RUNTIME_/Documents/";

//
////#if defined(IS_TRACKER)
////#if !defined(FINAL_RELEASE)
////	gOSPathToDocuments = @"/Users/mars/Documents/ft209/xms/";
////#endif
////#endif
//
	NSLog(@"gOSPathToDocuments=%@", gOSPathToDocuments);
	
	const char *path = (const char *)[gOSPathToDocuments UTF8String];
	gCPathToDocuments = strdup(path);
	gPathToDocuments = gCPathToDocuments;
	//LOGD("gCPathToDocuments='%s'", gCPathToDocuments);
	
	gOSPathToTemp = gOSPathToDocuments; //NSTemporaryDirectory();
	NSLog(@"gOSPathToTemp=%@", gOSPathToTemp);
	const char *pathTemp = (const char *)[gOSPathToTemp UTF8String];
	gCPathToTemp = strdup(pathTemp);

	gOSPathToSettings = gOSPathToDocuments; //NSTemporaryDirectory();
	NSLog(@"gOSPathToSettings=%@", gOSPathToSettings);
	const char *pathSettings = (const char *)[gOSPathToSettings UTF8String];
	gCPathToSettings = strdup(pathSettings);
	
#endif
}

void SYS_DeleteFile(CSlrString *filePath)
{
	filePath->DebugPrint("SYS_DeleteFile: ");
	
	NSString *str = FUN_ConvertCSlrStringToNSString(filePath);
	remove([str fileSystemRepresentation]);
//	[str release];
	
	LOGD("SYS_DeleteFile done");
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
	STRFREE(this->name);
	STRFREE(this->modDate);
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

/*
std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions)
{
	std::vector<CFileItem *> *files = new std::vector<CFileItem *>();
	
	NSString *nsDirectoryPath = [NSString stringWithUTF8String:directoryPath];
	
	NSArray *array = [[NSFileManager defaultManager] directoryContentsAtPath:nsDirectoryPath];
	
	for (NSString *fname in array)
    {
		//LOGD("check file:");
		//LOGD(fname);
        NSDictionary *fileDict =
		[[NSFileManager defaultManager] fileAttributesAtPath:[nsDirectoryPath stringByAppendingPathComponent:fname] traverseLink:NO];
		//NSString *modDate = [[fileDict objectForKey:NSFileModificationDate] description];
		
		NSString* pathExt = [fname pathExtension];
		
		NSString *modDate = [[fileDict objectForKey:NSFileModificationDate] description];
		
		char buf[1024];
		sprintf(buf, "%s%s%s", folderPath, separator, dirp->d_name);
		//LOGD("d_name=%s", buf);
		struct stat st;
		lstat(buf, &st);

		if ([[fileDict objectForKey:NSFileType] isEqualToString: @"NSFileTypeDirectory"])
		{
			//fname = [fname stringByAppendingString:@"/"];
			
			//LOGD("adding");
			//LOGD(fname);
			
			char *fileNameDup = strdup([fname UTF8String]);
			char *modDateDup = strdup([modDate UTF8String]);
			
			CFileItem *item = new CFileItem(fileNameDup, modDateDup, true); 
			files->push_back(item);
		}
		else
		{
			for (std::list<char *>::iterator itExtensions = extensions->begin();
				 itExtensions !=  extensions->end(); itExtensions++)                                                       
			{
				char *extension = *itExtensions;
				NSString *nsExtension = [NSString stringWithUTF8String:extension];
				
				if ([pathExt localizedCaseInsensitiveCompare:nsExtension] == NSOrderedSame)
					//		if ([pathExt isEqualToString:@"xm"])
				{
					//LOGD("adding");
					//LOGD(fname);
					
					char *fileNameDup = strdup([fname UTF8String]);
					char *modDateDup = strdup([modDate UTF8String]);
					
					CFileItem *item = new CFileItem(fileNameDup, modDateDup, false); 
					files->push_back(item);
				}
			}
		}
	}
	
	std::sort(files->begin(), files->end(), compare_CFileItem_nocase);
	
	return files;
}
 */

std::vector<CFileItem *> *SYS_GetFilesInFolder(char *directoryPath, std::list<char *> *extensions)
{
	return SYS_GetFilesInFolder(directoryPath, extensions, true);
}

std::vector<CFileItem *> *SYS_GetFilesInFolder(char *folderPath, std::list<char *> *extensions, bool withFolders)
{
	LOGD("CFileSystem::GetFiles: folderPath=%s", folderPath);
	std::vector<CFileItem *> *files = new std::vector<CFileItem *>();
	
	char separator[2];
	
	if (folderPath[strlen(folderPath)-1] == SYS_FILE_SYSTEM_PATH_SEPARATOR)
	{
		separator[0] = 0x00;
	}
	else
	{
		separator[0] = SYS_FILE_SYSTEM_PATH_SEPARATOR;
		separator[1] = 0x00;
	}
	
	DIR *dp;
	struct dirent *dirp;
	
	if((dp  = opendir(folderPath)) == NULL)
	{
		LOGError("Error opening folder: %s", folderPath);
		return files;
	}
	
	while((dirp = readdir(dp)) != NULL)
	{
		if (!strcmp(dirp->d_name, "..") || !strcmp(dirp->d_name, "."))
		{
			continue;
		}
		
		char buf[1024];
		sprintf(buf, "%s%s%s", folderPath, separator, dirp->d_name);
		//LOGD("d_name=%s", buf);
		struct stat st;
		lstat(buf, &st);
		
		if (S_ISDIR(st.st_mode))
		{
			//LOGD("<DIR> %s", dirp->d_name);
			if (withFolders)
			{
				CFileItem *item = new CFileItem(dirp->d_name, buf, (char*)"<mod date>", true);
				files->push_back(item);
			}
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
							//LOGD("adding");
							//LOGD(fname);
							
							//filesize.LowPart = ffd.nFileSizeLow;
							//filesize.HighPart = ffd.nFileSizeHigh;
							//LOGD("     %s %ld", ffd.cFileName, filesize.QuadPart);
							//LOGD("     %s", dirp->d_name);
							
							CFileItem *item = new CFileItem(dirp->d_name, buf, (char*)"<mod date>", false);
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
	
	LOGD("CFileSystem::GetFiles done");
	
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
}

bool SYS_windowAlwaysOnTopBeforeFileDialog = false;

void SYS_DialogOpenFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions,
						CSlrString *defaultFolder,
						CSlrString *windowTitle)
{
	LOGD("SYS_DialogOpenFile");
	
//	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsMainWindowAlwaysOnTop();
	VID_SetMainWindowAlwaysOnTopTemporary(false);
	
	if (defaultFolder != NULL)
	{
		defaultFolder->DebugPrint("defaultFolder=");
	}
	else
	{
		LOGD("defaultFolder=NULL!");
	}
	
	if (windowTitle != NULL)
	{
		windowTitle->DebugPrint("defaultFolder=");
	}
	else
	{
		LOGD("windowTitle=NULL!");
	}
	
	NSMutableArray *extensionsArray = nil;
	
	if (extensions != NULL && !extensions->empty())
	{
		extensionsArray = [[NSMutableArray alloc] init];
		
		for (std::list<CSlrString *>::iterator it = extensions->begin(); it != extensions->end(); it++)
		{
			CSlrString *str = *it;
//			str->DebugPrint("...=");

			NSString *nsStr = FUN_ConvertCSlrStringToNSString(str);
			[extensionsArray addObject:nsStr];
		}
		
//		NSLog(@"%@", extensionsArray);
	}

	NSString *wtitle = nil;
	if (windowTitle != NULL)
	{
		wtitle = FUN_ConvertCSlrStringToNSString(windowTitle);
	}
	
	NSString *dfolder = nil;
	
	if (defaultFolder != NULL)
	{
		dfolder = FUN_ConvertCSlrStringToNSString(defaultFolder);
	}
	
	dispatch_async(dispatch_get_main_queue(), ^{
		NSOpenPanel *panel = [NSOpenPanel openPanel];
		
		// Configure your panel the way you want it
		[panel setCanChooseFiles:YES];
		[panel setCanChooseDirectories:NO];
		[panel setAllowsMultipleSelection:NO];
		
		if (extensionsArray != nil)
		{
//			NSLog(@"SYS_DialogOpenFile: allowed file types=%@", extensionsArray);
			[panel setAllowedFileTypes:extensionsArray];
		}

		// title was removed after OS X 10.11
//		if (wtitle != nil)
//			[panel setTitle:wtitle];

		if (wtitle != nil)
			[panel setMessage:wtitle];

		if (dfolder != nil)
		{
			[panel setDirectoryURL:[NSURL fileURLWithPath:dfolder]];
		}
		
		VID_SetVSyncScreenRefresh(false);
		
		NSWindow *mainWindow = [[[NSApplication sharedApplication] windows] objectAtIndex:0];
		[panel beginSheetModalForWindow:mainWindow completionHandler:^(NSInteger result)
		{
			LOGD("SYS_DialogOpenFile: dialog result=%d", result);
			
			VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);
			VID_SetVSyncScreenRefresh(true);

			if (result == NSModalResponseOK)
			{
				for (NSURL *fileURL in [panel URLs])
				{
					NSString *strPath = [fileURL path];
					CSlrString *outPath = FUN_ConvertNSStringToCSlrString(strPath);
					callback->SystemDialogFileOpenSelected(outPath);
					delete outPath;
				}
			}
			else
			{
				callback->SystemDialogFileOpenCancelled();
			}
			
//			[panel release];
//			if (wtitle != nil)
//				[wtitle release];
//			if (dfolder != nil)
//				[dfolder release];
//			
//			if (extensionsArray != nil)
//				[extensionsArray release];
		}];
		
		[NSApp activateIgnoringOtherApps:YES];
		
	});
}




void SYS_DialogSaveFile(CSystemFileDialogCallback *callback, std::list<CSlrString *> *extensions,
						CSlrString *defaultFileName, CSlrString *defaultFolder,
						CSlrString *windowTitle)
{
//	// temporary remove always on top window flag
	SYS_windowAlwaysOnTopBeforeFileDialog = VID_IsMainWindowAlwaysOnTop();
	VID_SetMainWindowAlwaysOnTopTemporary(false);

	NSMutableArray *extensionsArray = [[NSMutableArray alloc] init];
	
	for (std::list<CSlrString *>::iterator it = extensions->begin(); it != extensions->end(); it++)
	{
		CSlrString *str = *it;
		NSString *nsStr = FUN_ConvertCSlrStringToNSString(str);
		[extensionsArray addObject:nsStr];
	}
	
//	NSLog(@"%@", extensionsArray);

	NSString *fname = nil;
	NSString *wtitle = nil;
	NSString *dfolder = nil;
	
	if (defaultFileName != NULL)
	{
		fname = FUN_ConvertCSlrStringToNSString(defaultFileName);
	}
	if (windowTitle != NULL)
	{
		wtitle = FUN_ConvertCSlrStringToNSString(windowTitle);
	}
	
	if (defaultFolder != NULL)
	{
		dfolder = FUN_ConvertCSlrStringToNSString(defaultFolder);
		NSLog(@"dfolder=%@", dfolder);
	}

	dispatch_async(dispatch_get_main_queue(), ^{
		NSSavePanel *panel = [NSSavePanel savePanel];
		
		// Configure your panel the way you want it
		[panel setAllowsOtherFileTypes:NO];
		[panel setExtensionHidden:YES];
		[panel setCanCreateDirectories:YES];
		[panel setAllowedFileTypes:extensionsArray];

		if (fname != nil)
		{
			[panel setNameFieldStringValue:fname];
		}
		
//		 title was removed after OS X 10.11
				if (wtitle != nil)
					[panel setTitle:wtitle];

//		if (wtitle != nil)
//		{
//			[panel setMessage:wtitle];
//		}
		
		if (dfolder != nil)
		{
			[panel setDirectoryURL:[NSURL fileURLWithPath:dfolder]];
		}

		VID_SetVSyncScreenRefresh(false);
		
		NSWindow *mainWindow = [[[NSApplication sharedApplication] windows] objectAtIndex:0];
		[panel beginSheetModalForWindow:mainWindow completionHandler:^(NSInteger result)
		 {
			 LOGD("SYS_DialogSaveFile: dialog result=%d", result);
			 
			 VID_SetMainWindowAlwaysOnTopTemporary(SYS_windowAlwaysOnTopBeforeFileDialog);
			 VID_SetVSyncScreenRefresh(true);

			 if (result == NSModalResponseOK)
			 {
				 NSString *strPath = [[panel URL] path];
				 CSlrString *outPath = FUN_ConvertNSStringToCSlrString(strPath);
				 callback->SystemDialogFileSaveSelected(outPath);
				 delete outPath;
			 }
			 else
			 {
				 callback->SystemDialogFileSaveCancelled();
			 }
			 
//			 [panel release];
//			 if (fname != nil)
//				 [fname release];
//			 if (wtitle != nil)
//				 [wtitle release];
//			 if (dfolder != nil)
//				 [dfolder release];
//			 [extensionsArray release];
		 }];
		
		[NSApp activateIgnoringOtherApps:YES];
		
	});
	
	LOGD("SYS_DialogSaveFile: done");
	
}

void SYS_DialogPickFolder(CSystemFileDialogCallback *callback, CSlrString *defaultFolder)
{
	__block char *defaultPath = NULL;
	if (defaultFolder)
	{
		defaultPath = defaultFolder->GetUTF8();
	}
	else
	{
		defaultPath = STRALLOC(gPathToDocuments);
	}
	
	VID_SetVSyncScreenRefresh(false);

	NFD_PickFolder_Async(defaultPath, ^(nfdresult_t result, nfdchar_t *outPath) {
		VID_SetVSyncScreenRefresh(true);
		STRFREE(defaultPath);
		
		if (result == NFD_OKAY)
		{
			CSlrString *path = new CSlrString(outPath);
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
	});
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

	char *cPath = path->GetUTF8();
	
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

bool SYS_FileDirExists(CSlrString *path)
{
	char *cPath = path->GetUTF8();
	
	struct stat info;
	
	if(stat( cPath, &info ) != 0)
	{
		delete [] cPath;
		return false;
	}
	else if(info.st_mode & S_IFDIR)
	{
		delete [] cPath;
		return true;
	}
	else
	{
		delete [] cPath;
		return false;
	}
}

bool SYS_FileDirExists(const char *cPath)
{
	struct stat info;
	
	if(stat( cPath, &info ) != 0)
	{
		return false;
	}
	else if(info.st_mode & S_IFDIR)
	{
		return true;
	}
	else
	{
		return false;
	}
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
	char *cPath = path->GetUTF8();
	mkdir(cPath, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	
	delete [] cPath;
}

void SYS_SetCurrentFolder(CSlrString *path)
{
	LOGD("SYS_SetCurrentFolder");
	path->DebugPrint("SYS_SetCurrentFolder: ");
	char *cPath = path->GetUTF8();
	chdir(cPath);
	
	delete [] cPath;
}

//////

char *SYS_GetFileName(const char *filePath)
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

char *SYS_GetFileExtension(const char *fileName)
{
	int index = -1;
	for (int i = (int)strlen(fileName)-1; i >= 0; i--)
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
	
	char *buf = new char[strlen(fileName)-index+1];
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

std::string SYS_GetRelativePath(const char* pathToFolder, const char* pathToFile)
{
	namespace fs = std::filesystem;

	fs::path base = fs::absolute(pathToFolder);
	fs::path target = fs::absolute(pathToFile);

	return fs::relative(target, base).generic_string();
}

std::string SYS_GetAbsolutePath(const char* pathToFolder, const char* relativePath)
{
	namespace fs = std::filesystem;
	
	fs::path base = fs::absolute(pathToFolder);
	fs::path relative = relativePath;
	
	fs::path fullPath = fs::absolute(base / relative);
	return fullPath.generic_string();
}


char *SYS_GetPathToDocuments()
{
	return gCPathToDocuments;
}


// NOTE, THE CODE BELOW TAKES 13-20 SECONDS TO EXECUTE COMMAND ON MACOS 10.15.7 in XCode, but in production is OK
// https://stackoverflow.com/questions/67558091/nstask-launch-or-popen-takes-15-20-seconds-to-start-simple-bash-scripts-in-macos

const char *SYS_ExecSystemCommand(const char *cmd, int *terminationCode)
{
	LOGD("***** SYS_ExecSystemCommand: '%s'", cmd);
	
	long t1 = SYS_GetCurrentTimeInMillis();

	NSString *commandToRun = FUN_ConvertCStringToNSString(cmd);
	NSTask *task = [[NSTask alloc] init];
	[task setLaunchPath:@"/bin/sh"];

	NSArray *arguments = [NSArray arrayWithObjects:
						  @"-c" ,
						  [NSString stringWithFormat:@"%@", commandToRun],
						  nil];
	NSLog(@"run command:%@", commandToRun);
	[task setArguments:arguments];

	NSPipe *pipe = [NSPipe pipe];
	[task setStandardOutput:pipe];

	NSFileHandle *file = [pipe fileHandleForReading];

	[task setQualityOfService:NSQualityOfServiceUserInteractive];
	[task launch];

	NSData *data = [file readDataToEndOfFile];

	[task waitUntilExit];

	NSString *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
	const char *cString = [output UTF8String];
	*terminationCode = [task terminationStatus];
	
	long t2 = SYS_GetCurrentTimeInMillis();

	LOGD("... ret=%s", cString);
	LOGD("... time=%f", (t2-t1)/1000.0f);
	
	return STRALLOC(cString);
}

#ifndef WIN32
#include <unistd.h>
#endif
#ifdef WIN32
#define stat _stat
#endif

long SYS_GetFileModifiedTime(const char *filePath)
{
	struct stat result;
	if(stat(filePath, &result)==0)
	{
		return (long)(result.st_mtime);
	}

	return 0;
}

/*
std::string SYS_ExecSystemCommand(const char* cmd)
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
	
	pclose(pipe);
	return resultStr;
}
*/

/*
 TODO: exec and consume output
 
 
 STARTUPINFO si = {sizeof (STARTUPINFO)};
	 PROCESS_INFORMATION pi;
	 CreateProcess( "\\windows\\notepad.exe", NULL,
		 0, 0, 0, 0, 0, 0, &si, &pi );
	 WaitForSingleObject( pi.hProcess, INFINITE );

 ////
 

 #include <iostream>
 #include <stdexcept>
 #include <stdio.h>
 #include <string>

 std::string exec(const char* cmd) {
	 char buffer[128];
	 std::string result = "";
	 FILE* pipe = popen(cmd, "r");
	 if (!pipe) throw std::runtime_error("popen() failed!");
	 try {
		 while (fgets(buffer, sizeof buffer, pipe) != NULL) {
			 result += buffer;
		 }
	 } catch (...) {
		 pclose(pipe);
		 throw;
	 }
	 pclose(pipe);
	 return result;
 }
 Replace popen and pclose with _popen and _pclose for Windows.


 ///

 #include <windows.h>
 #include <atlstr.h>
 //
 // Execute a command and get the results. (Only standard output)
 //
 CStringA ExecCmd(
	 const wchar_t* cmd              // [in] command to execute
 )
 {
	 CStringA strResult;
	 HANDLE hPipeRead, hPipeWrite;

	 SECURITY_ATTRIBUTES saAttr = {sizeof(SECURITY_ATTRIBUTES)};
	 saAttr.bInheritHandle = TRUE; // Pipe handles are inherited by child process.
	 saAttr.lpSecurityDescriptor = NULL;

	 // Create a pipe to get results from child's stdout.
	 if (!CreatePipe(&hPipeRead, &hPipeWrite, &saAttr, 0))
		 return strResult;

	 STARTUPINFOW si = {sizeof(STARTUPINFOW)};
	 si.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	 si.hStdOutput  = hPipeWrite;
	 si.hStdError   = hPipeWrite;
	 si.wShowWindow = SW_HIDE; // Prevents cmd window from flashing.
							   // Requires STARTF_USESHOWWINDOW in dwFlags.

	 PROCESS_INFORMATION pi = { 0 };

	 BOOL fSuccess = CreateProcessW(NULL, (LPWSTR)cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
	 if (! fSuccess)
	 {
		 CloseHandle(hPipeWrite);
		 CloseHandle(hPipeRead);
		 return strResult;
	 }

	 bool bProcessEnded = false;
	 for (; !bProcessEnded ;)
	 {
		 // Give some timeslice (50 ms), so we won't waste 100% CPU.
		 bProcessEnded = WaitForSingleObject( pi.hProcess, 50) == WAIT_OBJECT_0;

		 // Even if process exited - we continue reading, if
		 // there is some data available over pipe.
		 for (;;)
		 {
			 char buf[1024];
			 DWORD dwRead = 0;
			 DWORD dwAvail = 0;

			 if (!::PeekNamedPipe(hPipeRead, NULL, 0, NULL, &dwAvail, NULL))
				 break;

			 if (!dwAvail) // No data available, return
				 break;

			 if (!::ReadFile(hPipeRead, buf, min(sizeof(buf) - 1, dwAvail), &dwRead, NULL) || !dwRead)
				 // Error, the child process might ended
				 break;

			 buf[dwRead] = 0;
			 strResult += buf;
		 }
	 } //for

	 CloseHandle(hPipeWrite);
	 CloseHandle(hPipeRead);
	 CloseHandle(pi.hProcess);
	 CloseHandle(pi.hThread);
	 return strResult;
 } //ExecCmd


 */

void SYS_OpenURLInBrowser(const char *url)
{
	// Prepare the command by adding the prefix needed to open a URL
	std::string command = "open ";
	// Append the URL passed to the function
	command += url;

	// Use the system() function to execute the command
	// system() returns -1 on error, and the return value of the command otherwise
	// It's generally a good idea to check the return value for error handling
	int result = system(command.c_str());

	if (result == -1)
	{
		LOGError("SYS_OpenURLInBrowser: failed to open url %s", url);
	}
}
