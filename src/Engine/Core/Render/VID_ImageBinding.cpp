/*
 *  GLImageBinding.mm
 *  MusicTracker
 *
 *  Created by mars on 3/23/11.
 *  Copyright 2011 rabidus. All rights reserved.
 *
 */

#include "VID_ImageBinding.h"
#include "CRenderBackend.h"
#include "VID_Main.h"
#include <time.h>
#include <atomic>
#include <chrono>
#include <list>
#include <vector>
#include "SYS_Threading.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl3w.h>
#endif

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

// Raw GL texture ids queued for deferred deletion -- executed at the start of
// VID_BindImages(), i.e. after the previous frame's ImGui draw data ran and
// before the new frame's draw lists are recorded. See VID_PostDeleteGLTexture()
// in the header for the full mid-frame-deletion hazard rationale.
static std::vector<unsigned int> deferredTextureDeletes;

CSlrMutex* bindingMutex = NULL;

// ---------------------------------------------------------------------------
// Frame-budget throttle (see VID_ImageBinding.h for the whole rationale)
// ---------------------------------------------------------------------------
// Atomic only for defensive publication: both are set from the render thread
// and read from the render thread. Default OFF -- unthrottled, as before.
static std::atomic<bool>  bindThrottleEnabled{false};
static std::atomic<float> bindThrottleFactor{1.0f};

// When the previous VID_BindImages() returned. Render-thread only.
//
// The gap between that stamp and the next call IS the rest of the frame:
// VID_BindImages runs at the top of VID_Render, so everything between one
// return and the next entry is the app's own frame work plus the present. That
// is why the whole throttle fits in this file with no hook in the frame loop --
// the one number it needs, it can measure itself. Stamped on EVERY call,
// throttled or not, so flipping the throttle on does not start it off with a
// stale multi-second gap.
static double bindPrevEndSec = -1.0;

static double VID_BindNowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(
			steady_clock::now().time_since_epoch()).count();
}

void VID_SetImageBindingThrottle(bool enabled, float frameBudgetFactor)
{
	if (!(frameBudgetFactor >= 0.1f))   // also catches NaN
		frameBudgetFactor = 0.1f;
	bindThrottleEnabled.store(enabled, std::memory_order_relaxed);
	bindThrottleFactor.store(frameBudgetFactor, std::memory_order_relaxed);
}

bool VID_IsImageBindingThrottleEnabled()
{
	return bindThrottleEnabled.load(std::memory_order_relaxed);
}

double VID_ComputeBindBudgetSec(double targetFps, float frameBudgetFactor,
								double prevFrameRestSec)
{
	if (!(targetFps > 0.0))
		return 0.0;
	if (!(frameBudgetFactor >= 0.1f))
		frameBudgetFactor = 0.1f;
	// No previous call to measure against (first frame, or the clock went
	// backwards): charge nothing for the rest of the frame.
	if (!(prevFrameRestSec > 0.0))
		prevFrameRestSec = 0.0;

	const double framePeriod = 1.0 / targetFps;
	const double budget = framePeriod * (double)frameBudgetFactor - prevFrameRestSec;
	return (budget > 0.0) ? budget : 0.0;
}

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

void VID_PostDeleteGLTexture(unsigned int textureId)
{
	if (textureId == 0)
		return;

	VID_LockImageBindingMutex();
	deferredTextureDeletes.push_back(textureId);
	VID_UnlockImageBindingMutex();
}

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

	// Frame budget for this call (see the header). Computed BEFORE the queue is
	// touched, from the gap since the previous call -- i.e. what the rest of the
	// last frame cost -- so the deadline below caps the whole FRAME, not the
	// binding step alone.
	const bool   bindThrottle = bindThrottleEnabled.load(std::memory_order_relaxed);
	const double bindStartSec = VID_BindNowSec();
	double bindDeadlineSec = 0.0;
	if (bindThrottle)
	{
		bindDeadlineSec = bindStartSec +
			VID_ComputeBindBudgetSec(VID_GetTargetFPS(),
									 bindThrottleFactor.load(std::memory_order_relaxed),
									 (bindPrevEndSec < 0.0) ? -1.0 : (bindStartSec - bindPrevEndSec));
	}
	int bindProcessed = 0;

	// Deferred GL texture deletions (VID_PostDeleteGLTexture): safe here --
	// the previous frame's draw lists have been executed and this frame's
	// have not been recorded yet, so no recorded ImDrawCmd can still
	// reference these ids.
	if (!deferredTextureDeletes.empty())
	{
		// GL-ONLY. This queue is fed exclusively by VID_PostDeleteGLTexture(),
		// whose only caller is CGLRenderTarget -- so on a non-GL backend it is
		// always empty and this never runs. Guarded anyway because glDeleteTextures
		// under Metal is not a harmless no-op: the gl3w symbol is unresolved, so
		// calling it jumps through NULL. Metal's own deferred release lives in
		// CRenderBackendMetal::DeleteTexture.
		CRenderBackend *backend = VID_GetRenderBackend();
		if (backend != NULL && backend->SupportsOpenGLShaders())
			glDeleteTextures((GLsizei)deferredTextureDeletes.size(), (const GLuint *)deferredTextureDeletes.data());
		deferredTextureDeletes.clear();
	}

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
			// Frame-budget stop. Checked BEFORE claiming the next entry, and
			// never before the FIRST one -- the one-per-frame progress
			// guarantee, without which a frame already slower than the budget
			// would bind nothing, forever. The queue keeps its order: whatever
			// is left simply waits for the next frame.
			if (bindThrottle && bindProcessed > 0 && VID_BindNowSec() >= bindDeadlineSec)
				break;
			bindProcessed++;

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

	// Unconditional: the next call's "rest of the frame" measurement must be
	// valid the very first time the throttle is switched on, not one frame later.
	bindPrevEndSec = VID_BindNowSec();

	VID_UnlockImageBindingMutex();

#ifdef LOG_BINDING
	LOGD("VID_BindImages(): done");
#endif

	return ret;
}

