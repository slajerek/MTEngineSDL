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

// Defer glDeleteTextures(textureId) to the start of the NEXT frame's
// VID_BindImages() -- the point after the previous frame's ImGui draw data has
// been executed and before the new frame's draw lists are recorded.
//
// WHY: the frame loop records ImGui draw lists (GUI_Render -> RenderImGui ->
// Blit/AddImage capture raw GL texture ids) BEFORE MT_Render() runs, and
// executes them AFTER (PresentFrameBuffer -> ImGui_ImplOpenGL3_RenderDrawData).
// Deleting a texture from MT_Render() (or anywhere after recording) therefore
// poisons the CURRENT frame's already-recorded draw list: the backend then
// calls glBindTexture on a deleted name, which in a core profile context
// raises GL_INVALID_OPERATION (fatal at the next ASSERT_OPENGL).
// Any texture that can be referenced by ImGui draw lists (e.g. a video
// render-target texture wrapped by CSlrImageExternalTexture) MUST be freed
// through this call, never with a direct mid-frame glDeleteTextures.
void VID_PostDeleteGLTexture(unsigned int textureId);
bool VID_IsEmptyImageBindingQueue();
void VID_WaitForImageBindingFinished();
void VID_LoadImage(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources);
void VID_LoadImageAsync(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources);
void VID_LoadImageAsyncNoWait(char *fileName, CSlrImage **destination, bool linearScaling, bool fromResources);
bool VID_BindImages();

#endif //_VID_IMAGE_BINDING_
