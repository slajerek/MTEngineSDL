/*
 *  GLImageBinding.mm
 *  MusicTracker
 *
 *  Created by mars on 3/23/11.
 *  Copyright 2011 rabidus. All rights reserved.
 *
 */

#include "VID_ImageBinding.h"
#include <time.h>
#include <list>
#include "SYS_Threading.h"

//#define LOG_BINDING

class CBindingImageData //: public
{
public:
	CSlrImage *image;
	CSlrImage **destination;
	CImageData *imageData;
	u8 mode;
	
	CBindingImageData(CSlrImage *image, CSlrImage **destination, u8 mode)
	{
		this->image = image;
		this->imageData = image->loadImageData;
		this->destination = destination;
		this->mode = mode;
	}
};

std::list<CBindingImageData *> imageBindings;

CSlrMutex* bindingMutex = NULL;

void VID_InitImageBindings()
{
	LOGD("VID_InitImageBindings()");
	
	if (bindingMutex == NULL)
	{
		bindingMutex = new CSlrMutex("bindingMutex");
		LOGD("bindingMutex=%x", bindingMutex);
	}
}

void VID_LockImageBindingMutex()
{
#ifdef LOG_BINDING
	LOGD("LockBindingMutex");
#endif

	bindingMutex->Lock();
}

void VID_UnlockImageBindingMutex()
{
#ifdef LOG_BINDING
	LOGD("UnlockBindingMutex");
#endif

	bindingMutex->Unlock();
}

void VID_LoadImage(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources)
{
	CSlrImage *loadImg = new CSlrImage(true, linearScaling);
	loadImg->DelayedLoadImage(fileName, fromResources);
	loadImg->BindImage();
	
	*destination = loadImg;
}

void VID_LoadImageAsync(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources)
{
	LOGR("VID_LoadImageAsync: '%s'", (fileName != NULL ? fileName : "NULL"));
	CSlrImage *loadImg = new CSlrImage(true, linearScaling);
	loadImg->DelayedLoadImage(fileName, fromResources);
	LOGR("loadImg->loadImage=%8.8x", loadImg->loadImageData);
	VID_PostImageBinding(loadImg, destination);
	VID_WaitForImageBindingFinished();
	LOGR("VID_LoadImageAsync: done ('%s')", (fileName != NULL ? fileName : "NULL"));
}

void VID_LoadImageAsyncNoWait(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources)
{
	LOGR("VID_LoadImageAsyncNoWait: '%s'", (fileName != NULL ? fileName : "NULL"));
	CSlrImage *loadImg = new CSlrImage(true, linearScaling);
	loadImg->DelayedLoadImage(fileName, fromResources);
	LOGR("loadImg->loadImage=%8.8x", loadImg->loadImageData);
	VID_PostImageBinding(loadImg, destination);
//	VID_WaitForImageBindingFinished();
	LOGR("VID_LoadImageAsyncNoWait: done ('%s')", (fileName != NULL ? fileName : "NULL"));
}

void VID_PostImageBinding(CSlrImage *image, CSlrImage **dest)
{
	LOGR("VID_PostImageBinding: '%s' width=%f height=%f", (image->resourcePath != NULL ? image->resourcePath : "NULL"), image->width, image->height);
	CBindingImageData *bindingData = new CBindingImageData(image, dest, BINDING_MODE_BIND);

	VID_LockImageBindingMutex();
	imageBindings.push_back(bindingData);
	VID_UnlockImageBindingMutex();
}

void VID_PostImageBinding(CSlrImage *image, CSlrImage **dest, u8 mode)
{
	LOGR("VID_PostImageBinding: '%s' width=%f height=%f mode=%d", (image->resourcePath != NULL ? image->resourcePath : "NULL"), image->width, image->height, mode);
	CBindingImageData *bindingData = new CBindingImageData(image, dest, mode);
	
	VID_LockImageBindingMutex();
	imageBindings.push_back(bindingData);
	VID_UnlockImageBindingMutex();
}


//void VID_PostImageLoadAndBind(CSlrImage *image, CSlrImage **dest)
//{
//	LOGR("VID_PostImageLoadAndBind: '%s' width=%f height=%f", (image->resourcePath != NULL ? image->resourcePath : "NULL"), image->width, image->height);
//	CBindingImageData *bindingData = new CBindingImageData(image, dest, BINDING_MODE_LOAD_AND_BIND);
//	
//	LockBindingMutex();
//	imageBindings.push_back(bindingData);
//	UnlockBindingMutex();
//}

void VID_PostImageDealloc(CSlrImage *image)
{
	if (image == NULL)
	{
		LOGError("VID_PostImageDealloc: image NULL");
		return;
	}
	LOGR("VID_PostImageDealloc: '%s' width=%f height=%f", (image->resourcePath != NULL ? image->resourcePath : "NULL"), image->width, image->height);
		
	VID_LockImageBindingMutex();
	
	// check if image already is waiting for binding
	for (std::list<CBindingImageData *>::iterator it = imageBindings.begin(); it != imageBindings.end(); it++)
	{
		CBindingImageData *bindingData = *it;
		
		if (bindingData->image == image)
		{
			imageBindings.remove(bindingData);
			delete bindingData;
			VID_UnlockImageBindingMutex();
			return;
		}
	}
	
	CBindingImageData *bindingData = new CBindingImageData(image, NULL, BINDING_MODE_DEALLOC);
	imageBindings.push_back(bindingData);
	
	VID_UnlockImageBindingMutex();
}

void VID_PostImageDestroy(CSlrImage *image)
{
	LOGR("VID_PostImageDestroy: '%s' width=%f height=%f", (image->resourcePath != NULL ? image->resourcePath : "NULL"), image->width, image->height);
	CBindingImageData *bindingData = new CBindingImageData(image, NULL, BINDING_MODE_DESTROY);
	
	VID_LockImageBindingMutex();
	imageBindings.push_back(bindingData);
	VID_UnlockImageBindingMutex();
}

bool VID_IsEmptyImageBindingQueue()
{
	VID_LockImageBindingMutex();

	if (imageBindings.empty())
	{
#ifdef LOG_BINDING
		LOGD("VID_IsEmptyImageBindingQueue: is empty");
#endif

		VID_UnlockImageBindingMutex();
		return true;
	}

#ifdef LOG_BINDING
	LOGD("VID_IsEmptyImageBindingQueue: not empty");
#endif

	VID_UnlockImageBindingMutex();
	return false;
}

#define SLEEP_TIME_MS 30

void VID_WaitForImageBindingFinished()
{
#ifdef LOG_BINDING
	LOGD("VID_WaitForImageBindingFinished");
#endif

#ifdef WIN32
#else
	const long sleepTimeMs = (SLEEP_TIME_MS*1000000L);

	struct timespec sleepTime;
	struct timespec remainingSleepTime;

	sleepTime.tv_sec=0;
	sleepTime.tv_nsec=sleepTimeMs;
#endif

	while(true)
	{
#ifdef WIN32
		Sleep(SLEEP_TIME_MS);
#else
		nanosleep(&sleepTime, &remainingSleepTime);
#endif
		if (VID_IsEmptyImageBindingQueue())
			break;
	}
}

bool VID_BindImages()
{
#ifdef LOG_BINDING
	LOGD("VID_BindImages()");
#endif

	bool ret = false;

	VID_LockImageBindingMutex();

#ifdef LOG_BINDING
	if (imageBindings.empty())
	{
		LOGD("VID_BindImages: is empty");
	}
	else
	{
		LOGD("VID_BindImages: not empty");
	}
#endif

	// image bindings
	if (!imageBindings.empty())
	{
		ret = true;

		while(!imageBindings.empty())
		{
			CBindingImageData *bindingData = imageBindings.front();

			const char *path = bindingData->image->ResourceGetPath();
			LOGR("VID_BindImages: image->loadImage=%8.8x path=%s",
				 bindingData->image->loadImageData,
				 path ? path : "NULL");
			
			u8 mode = bindingData->mode;
			
			const char *bindModeDebugText = "unknown bind mode";
			if (mode == BINDING_MODE_BIND)
			{
				bindModeDebugText = "bound";
				
				bindingData->image->BindImage();
				if (bindingData->imageData)
				{
					if (bindingData->image->loadImageData == bindingData->imageData)
					{
						bindingData->image->FreeLoadImage();
					}
					else
					{
						delete bindingData->imageData;
						bindingData->image = NULL;
					}
				}
				if (bindingData->destination != NULL)
				{
					*bindingData->destination = bindingData->image;
				}
			}
			else if (mode == BINDING_MODE_DEALLOC)
			{
				bindModeDebugText = "delloced";
				bindingData->image->Deallocate();
			}
			else if (mode == BINDING_MODE_DESTROY)
			{
				bindModeDebugText = "destroyed";
				bindingData->image->Deallocate();
				delete bindingData->image;
				bindingData->image = NULL;
			}
			else if (mode == BINDING_MODE_LOAD_AND_BIND)
			{
				SYS_FatalExit("TODO: BINDING_MODE_LOAD_AND_BIND");
			}
			else if (mode == BINDING_MODE_DONT_FREE_IMAGEDATA)
			{
				bindModeDebugText = "bound (image data kept)";
				bindingData->image->BindImage();
				if (bindingData->destination != NULL)
				{
					*bindingData->destination = bindingData->image;
				}
			}

//			LOGD("delete binding %x mode %d path %s", bindingData, bindingData->mode, bindingData->image ? bindingData->image->ResourceGetPath() : "NULL");

			imageBindings.pop_front();
			delete bindingData;
			
			LOGR("VID_BindImages: image %s", bindModeDebugText);
		}
	}

	VID_UnlockImageBindingMutex();

#ifdef LOG_BINDING
	LOGD("VID_BindImages(): done");
#endif

	return ret;
}

