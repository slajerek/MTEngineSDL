/*
 *  GLImageBinding.h
 *  MusicTracker
 *
 *  Created by mars on 3/23/11.
 *  Copyright 2011 rabidus. All rights reserved.
 *
 */

#ifndef _VID_IMAGE_BINDING_
#define _VID_IMAGE_BINDING_

#include "SYS_Defs.h"
#include "SYS_Main.h"
#include "CSlrImage.h"

#define BINDING_MODE_UNKNOWN		0
#define BINDING_MODE_BIND			1
#define BINDING_MODE_LOAD_AND_BIND	2
// only dealloc image buffer
#define BINDING_MODE_DEALLOC		3
// destroy full CSlrImage object (delete)
#define BINDING_MODE_DESTROY		4
#define BINDING_MODE_DONT_FREE_IMAGEDATA		5

void VID_InitImageBindings();
void VID_LockImageBindingMutex();
void VID_UnlockImageBindingMutex();
void VID_PostImageBinding(CSlrImage *image, CSlrImage **dest);
void VID_PostImageBinding(CSlrImage *image, CSlrImage **dest, u8 mode);
void VID_PostImageDealloc(CSlrImage *image);
void VID_PostImageDestroy(CSlrImage *image);
bool VID_IsEmptyImageBindingQueue();
void VID_WaitForImageBindingFinished();
void VID_LoadImage(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources);
void VID_LoadImageAsync(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources);
void VID_LoadImageAsyncNoWait(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources);
bool VID_BindImages();

#endif //_VID_IMAGE_BINDING_
