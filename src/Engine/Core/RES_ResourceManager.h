/*
 *  CResourceManager.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-03-02.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef __CRESOURCEMANAGER_H__
#define __CRESOURCEMANAGER_H__

#include "SYS_Main.h"
#include "RES_DeployFile.h"

using namespace std;

#include <map>
#include <string.h>
#include <list>

#include "CSlrImage.h"
#include "CSlrTexture.h"
#include "CSlrFile.h"
#include "CSlrFont.h"
#include "CSlrFileMemory.h"
#include "CSlrMusicFile.h"
#include "CSlrResourceBase.h"
#include "SYS_Threading.h"

class CDataTable;

extern u64 gMaxMemoryForResources;
extern u64 gCurrentResourceMemoryTaken;

extern volatile u8 gResourceManagerState;
#define RESOURCE_MANAGER_STATE_INITIALIZING	0
#define RESOURCE_MANAGER_STATE_IDLE			1
#define RESOURCE_MANAGER_STATE_PREPARING	2
#define RESOURCE_MANAGER_STATE_LOADING		3
#define RESOURCE_MANAGER_STATE_SKIP_LOAD	4

extern u64 gMemoryTakenAtStart;

class CSlrFileZlib;

class CResourceManagerCallback
{
public:
	virtual void ResourcesLoaded(void *userData);
};

void RES_Init(u16 destScreenWidth);
void RES_SetMaxSystemMemory();
void RES_SetMaxSystemMemory(u32 maxMemory);

void RES_AddResource(const char *resourceName, int resourceLevel, CSlrResourceBase *data);
CSlrResourceBase *RES_GetResource(const char *resourceName);

//void RES_DeleteResource(std::string resourceName);
//void RES_DeleteResources(int level);
//void RES_DeleteResourcesAboveLevel(int level);

class CImageLoadData
{
public:
	CImageLoadData(const char *imageName, bool linearScaling);
	~CImageLoadData();

	const char *imageName;
	bool linearScaling;
};

void RES_RegisterImage(const char *imageName, bool linearScaling);
CSlrImage *RES_RegisterAndLoadImage(const char *imageName, bool linearScaling);
CSlrImage *RES_RegisterAndLoadImage(const char *imageName, bool linearScaling, int resourceLevel);

// have to have loadingData (via RES_RegisterImage):
CSlrImage *RES_GetImageSync(const char *imageName, bool fromResources);
CSlrImage *RES_GetImageSync(const char *imageName, int resourceLevel, bool fromResources);

// simple image load:
CSlrImage *RES_GetImageAsync(const char *imageName, bool linearScaling, bool fromResources);
CSlrImage *RES_GetImageAsync(const char *imageName, bool linearScaling, int resourceLevel, bool fromResources);

// synced image load (locks gui renderer):
CSlrImage *RES_GetImage(const char *imageName, bool linearScaling, bool fromResources);
CSlrImage *RES_GetImage(const char *imageName, bool linearScaling, int resourceLevel, bool fromResources);
CSlrImage *RES_GetImage(const char *imageName, bool linearScaling);
CSlrImage *RES_GetImage(const char *imageName);

CSlrImage *RES_LoadImageFromFileOS(CSlrString *path, bool linearScaling);
CSlrImage *RES_LoadImageFromFileOS(const char *path, bool linearScaling);

// synced image load (locks gui renderer), if no image returns placeholder
CSlrImage *RES_GetImageOrPlaceholder(const char *imageName, bool linearScaling, bool fromResources);
CSlrImage *RES_GetImageOrPlaceholder(const char *imageName, bool linearScaling, int resourceLevel, bool fromResources);

void RES_ReleaseImage(CSlrImageBase *image);
void RES_ReleaseImage(CSlrImageBase *image, int resourceLevel);

void RES_ReleaseImage(CSlrImage *image);
void RES_ReleaseImage(CSlrImage *image, int resourceLevel);

void RES_DeactivateResource(CSlrResourceBase *res);
void RES_RemoveResource(const char *resourceName);

CSlrFile *RES_OpenFileFromResources(const char *fileName, u8 fileType);
CSlrFile *RES_OpenFileFromDocuments(const char *fileName, u8 fileType);
CSlrFile *RES_OpenFile(bool fromResources, const char *fileName, u8 fileType);
CSlrFile *RES_GetFile(const char *fileName, u8 fileType);
CSlrFile *RES_GetFile(bool fromResources, const char *fileName, u8 fileType);

CSlrFile *RES_CreateFile(const char *fileName, u8 fileType);

CSlrFileZlib *RES_GetFileZlib(const char *fileName);
CSlrFileZlib *RES_GetFileZlib(const char *fileName, u8 fileType);

CSlrFileMemory *RES_GetSound(const char *fileName);
CSlrFileMemory *RES_GetSound(bool fromResources, const char *fileName);

CSlrMusicFile *RES_GetMusic(const char *fileName, bool seekable);
CSlrMusicFile *RES_GetMusic(const char *fileName, bool seekable, int resourceLevel, bool fromResources);

void RES_RegisterImageFromAtlas(CSlrImage *imageAtlas, const char *name, int startPosX, int startPosY, int endPosX, int endPosY, float scale);

CSlrImage *RES_GetImageFromAtlas(const char *name);

CSlrFont *RES_GetFont(const char *fontName);
CSlrFont *RES_GetFontAsync(const char *fontName, bool fromResources);
CSlrFont *RES_GetFontAsync(const char *fontName, bool fromResources, bool linearScale);
CSlrFont *RES_GetFontAsync(const char *fontName, int resourceLevel, bool fromResources);
CSlrFont *RES_GetFontAsync(const char *fontName, int resourceLevel, bool fromResources, bool linearScale);

u32 RES_PrepareMemory(u32 memoryNeeded, bool async);

// TODO: is it possible to sync unsynced PrepareMemory?
//void RES_PrepareMemorySync(u32 memoryNeeded, bool async);

// only preallocate resources (good for application startup)
void RES_SetStateSkipResourcesLoading();
void RES_SetStateIdle();

void RES_StartResourcesAllocate();
std::list<CSlrResourceBase *> *RES_GetAllocatedResourcesList();
void RES_ResourcePrepare(CSlrResourceBase *resource);
void RES_PreloadResourcesList(std::list<CSlrResourceBase *> *resourcesList);
void RES_StartResourcesLoadingAsync(CResourceManagerCallback *callback, void *userData);
void RES_LoadResourcesSync(CResourceManagerCallback *callback, void *userData);
void RES_StartResourcesAllocateForLoadingScreen();
void RES_ResourcesPreparingFinishedForLoadingScreen();


void RES_DebugPrintResources();
void RES_DebugPrintResourcesToLoad();
void RES_DebugPrintMemory();
CDataTable *RES_DebugGetDataTable();
void RES_DebugRender();

u8 RES_GetResourceManagerState();

void RES_ClearResourcesToLoad();

void RES_LockMutex(const char *whoLocked);
void RES_UnlockMutex(const char *whoLocked);

class CResourceLoaderThread : public CSlrThread
{
public:
	virtual void ThreadRun(void *data);
};

#endif //__CRESOURCEMANAGER_H__
