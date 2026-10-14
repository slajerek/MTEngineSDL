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
// bind; then FreeLoadImage() once the texture exists. Unlike BINDING_MODE_BIND,
// a CreateTexture that produced no handle keeps its data (its owner decides),
// and the destination is always written. For images whose texture IS the
// image: keeping the CPU copy beside it doubles their memory.
#define BINDING_MODE_BIND_FREE_IMAGEDATA	6

void VID_InitImageBindings();
void VID_LockImageBindingMutex();
void VID_UnlockImageBindingMutex();
void VID_PostImageBinding(CSlrImage *image, CSlrImage **dest);
void VID_PostImageBinding(CSlrImage *image, CSlrImage **dest, u8 mode);
void VID_PostImageDealloc(CSlrImage *image);
void VID_PostImageDestroy(CSlrImage *image);

// Option C: the pair's strong guarantee. Queues `first` then `second` under ONE
// lock, or neither: both CBindingImageData are allocated before the lock; if the
// second push_back throws, the first is popped and the exception rethrown. The bind
// loop processes them in that order, and a frame-budget stop that splits them is
// harmless when `first` is the gain map: a base bound a frame later sees its map
// already bound. No destination is written (NULL). The mode it is called with is
// BINDING_MODE_BIND_FREE_IMAGEDATA in production (Decision R-3), DONT_FREE in tests.
// VID_PostImageDealloc(second) while `second` is still queued cancels both, or
// queues `first`'s DEALLOC when a frame-budget stop already bound it.
void VID_PostImageBindingPair(CSlrImage *first, CSlrImage *second, u8 mode);

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

// ---------------------------------------------------------------------------
// Frame-budget throttle for VID_BindImages()
// ---------------------------------------------------------------------------
// OFF by default, and off is bit-for-bit the historical behaviour: the whole
// queue is drained inside one frame, however long that takes.
//
// WHY IT EXISTS. Every binding is a full glTexImage2D of a decoded image. A
// photo cache filling four decode workers at once therefore lands several
// full-resolution uploads in ONE frame -- a 100ms+ frame. A still picture
// survives that; a PLAYING VIDEO does not. Its clock is audio-driven, so the
// stall does not slow the clip down, it makes Update()'s catch-up scan
// consume-drop every frame the stall ran past. That is the visible stutter over
// the first seconds after a folder opens.
//
// WHAT ON MEANS. The drain stops as soon as this frame's time budget is spent
// and resumes on the next frame. The budget is
//
//     one target frame period (1 / VID_GetTargetFPS()) * frameBudgetFactor
//     MINUS whatever the rest of the previous frame already cost
//
// so the ceiling is the WHOLE FRAME, not the binding step in isolation -- which
// is the only version of the rule that actually keeps the next frame on time.
// factor 1.0 (the default) = never overrun the frame period; 1.10 = 10% overrun
// tolerated. Values below 0.1 are clamped: a budget of zero would leave nothing
// but the one-per-frame progress guarantee below.
//
// The cost of ON is throughput: fewer images bound per frame. Nothing is lost,
// only deferred -- the pixels are already in RAM, waiting, which is exactly
// where they were sitting anyway.
//
// GUARANTEES
//  * At least one queue entry is processed per call, whatever the budget says.
//    Without it a frame already slower than the budget would never bind
//    anything and images would never appear at all.
//  * The queue stays strictly FIFO. A DEALLOC/DESTROY queued BEHIND a deferred
//    binding waits with it, so its memory is released a frame or two later than
//    it used to be. Running those ahead is NOT an option: DESTROY deletes the
//    CSlrImage, and a still-queued binding for that same image would dangle.
//  * Deferred glDeleteTextures (VID_PostDeleteGLTexture) are never throttled --
//    they are cheap, and they are the one thing that must happen at this exact
//    point in the frame.
//  * VID_WaitForImageBindingFinished() (RES_LoadResourcesAsync's blocking
//    drain, from a loader thread) still terminates: the progress guarantee
//    above means the render loop retires at least one entry per frame, so a
//    throttled queue drains slower but never stalls.
//
// Call from the render thread, once per frame, from whatever knows the answer
// (for a media app: "is a clip playing right now").
void VID_SetImageBindingThrottle(bool enabled, float frameBudgetFactor = 1.0f);
bool VID_IsImageBindingThrottleEnabled();

// The throttle's arithmetic, as a pure function: seconds of binding this call
// may spend. Exposed so it can be pinned by a unit test without a live frame
// loop. A negative prevFrameRestSec (no previous call to measure from) counts
// as zero; the result never goes below zero.
double VID_ComputeBindBudgetSec(double targetFps, float frameBudgetFactor,
								double prevFrameRestSec);

#endif //_VID_IMAGE_BINDING_
