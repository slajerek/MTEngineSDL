#if defined(WIN32)

#include "ICmsEngine.h"
#include "SYS_Threading.h"
#include "DBG_Log.h"

#include <windows.h>
#include <icm.h>

#include <cstring>
#include <map>
#include <vector>

// ---------------------------------------------------------------------------
// CM-B "Fast" quality tier: NOT overridden here -- unmeasured, like lcms2.
//
// ICM 2.0 and WCS 1.0 both take the pixel format per call (TranslateBitmapBits
// with BM_RGBQUADS-style formats rather than BM_16b_RGB), so an override is
// mechanically straightforward -- closer to the ColorSync shape than to
// lcms2's. It is left undone only because nobody has profiled whether 8-bit is
// actually cheaper for these CMMs.
//
// Note this backend is not the Windows default: the app ships lcms2
// (CMS_VARIANT_LCMS2), so profile CCmsLittleCMS.cpp first.
// ---------------------------------------------------------------------------


namespace
{

struct WinProfileHandle
{
	HPROFILE profile = NULL;
};

// TranslateBitmapBits is not documented thread-safe and four decode workers
// share one transform, so each thread gets its own HTRANSFORM. The map is
// guarded for lookup and insertion only -- never across the translate call.
struct WinTransformHandle
{
	HPROFILE                     src = NULL;   // NOT owned; CColorManager closes these
	HPROFILE                     dst = NULL;
	DWORD                        icmIntent = INTENT_RELATIVE_COLORIMETRIC;
	bool                         wcs = false;
	CSlrMutex                   *mutex = NULL;
	std::map<DWORD, HTRANSFORM>  perThread;
};

DWORD IntentToIcm(CmsRenderingIntent intent)
{
	switch (intent)
	{
		case CMS_INTENT_PERCEPTUAL:            return INTENT_PERCEPTUAL;
		case CMS_INTENT_SATURATION:            return INTENT_SATURATION;
		case CMS_INTENT_ABSOLUTE_COLORIMETRIC: return INTENT_ABSOLUTE_COLORIMETRIC;
		case CMS_INTENT_RELATIVE_COLORIMETRIC:
		default:                               return INTENT_RELATIVE_COLORIMETRIC;
	}
}

HTRANSFORM CreateIcmTransform(WinTransformHandle *h)
{
	HPROFILE profiles[2] = { h->src, h->dst };
	// One intent per profile PAIR, not per profile: nIntents must be
	// nProfiles-1 (an intent applies between each pair of adjacent profiles).
	// Passing nProfiles elements here silently desynced the intent the CMM
	// actually applied and produced a near-identity transform.
	DWORD    intents[1]  = { h->icmIntent };

	// CreateMultiProfileTransform, not CreateColorTransform: the latter takes a
	// LOGCOLORSPACE as its source and so cannot express an arbitrary embedded
	// ICC profile, which is precisely our case.
	//
	// WCS_ALWAYS is what selects the Canon Kyuanos (WCS 1.0) pipeline; without
	// it the call uses the Heidelberg ICM 2.0 engine. That flag IS the engine
	// toggle -- WcsOpenColorProfile is for pairing an ICC profile with WCS
	// device-model/gamut-map profiles, which we do not have.
	DWORD flags = BEST_MODE;
	if (h->wcs)
		flags |= WCS_ALWAYS;

	return CreateMultiProfileTransform(profiles, 2, intents, 1, flags, INDEX_DONT_CARE);
}

class CCmsWindows : public ICmsEngine
{
public:
	// The variant is NORMALISED once, here, and every later decision reads the
	// normalised value. This backend implements exactly two engines: ICM 2.0
	// (CMS_VARIANT_DEFAULT) and WCS 1.0. Anything else -- CMS_VARIANT_LCMS2 on
	// a build where lcms2 is not compiled in, or a variant added later --
	// degrades to WCS, NEVER to ICM 2.0.
	//
	// Why the direction matters: ICM 2.0 has a measured defect converting a
	// wide-gamut profile used as the transform SOURCE (Adobe RGB camera JPEGs,
	// and every RAW Develop output). Silently choosing it because a value fell
	// off the end of a switch is the worst possible failure here, and it is
	// what used to happen -- see CmsCreatePlatformEngine's Windows branch.
	// The factory now also maps LCMS2 -> WCS; this is the second, independent
	// guard, so a direct caller cannot reintroduce it.
	CCmsWindows(CmsEngineVariant requested)
		: variant(requested == CMS_VARIANT_DEFAULT ? CMS_VARIANT_DEFAULT : CMS_VARIANT_WCS)
	{
		if (requested != variant)
			LOGD("CCmsWindows: variant %d is not implemented by this backend; using WCS 1.0",
			     (int)requested);
	}
	virtual ~CCmsWindows() {}

	virtual CmsProfile *OpenProfile(const u8 *bytes, u32 size) override
	{
		if (bytes == NULL || size == 0)
			return NULL;

		PROFILE p;
		p.dwType = PROFILE_MEMBUFFER;          // no temp files
		p.pProfileData = (PVOID)bytes;
		p.cbDataSize = (DWORD)size;

		HPROFILE h = OpenColorProfileA(&p, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
		if (h == NULL)
		{
			LOGError("CCmsWindows: OpenColorProfileA failed (err %lu)", GetLastError());
			return NULL;
		}

		// OpenColorProfile is lenient; make it prove the profile is usable.
		BOOL valid = FALSE;
		if (!IsColorProfileValid(h, &valid) || !valid)
		{
			LOGError("CCmsWindows: IsColorProfileValid rejected profile (err %lu)", GetLastError());
			CloseColorProfile(h);
			return NULL;
		}

		WinProfileHandle *wh = new WinProfileHandle();
		wh->profile = h;
		return (CmsProfile *)wh;
	}

	virtual void CloseProfile(CmsProfile *profile) override
	{
		WinProfileHandle *wh = (WinProfileHandle *)profile;
		if (wh == NULL)
			return;
		if (wh->profile != NULL)
			CloseColorProfile(wh->profile);
		delete wh;
	}

	virtual CmsTransform *CreateTransform(CmsProfile *src, CmsProfile *dst,
	                                      CmsRenderingIntent intent,
	                                      bool blackPointCompensation) override
	{
		WinProfileHandle *s = (WinProfileHandle *)src;
		WinProfileHandle *d = (WinProfileHandle *)dst;
		if (s == NULL || d == NULL || s->profile == NULL || d->profile == NULL)
			return NULL;

		// ICM 2.0 has no first-class black-point-compensation flag. Accepted
		// and documented rather than faked: CM-B maps the BPC setting onto
		// intent choice on this engine.
		(void)blackPointCompensation;

		WinTransformHandle *h = new WinTransformHandle();
		h->src = s->profile;
		h->dst = d->profile;
		h->icmIntent = IntentToIcm(intent);
		h->wcs = (variant == CMS_VARIANT_WCS);
		h->mutex = new CSlrMutex("CCmsWindows");

		// Build the calling thread's handle eagerly: creation is the fallible
		// step and CreateTransform is the only call that can report failure.
		HTRANSFORM t = CreateIcmTransform(h);
		if (t == NULL)
		{
			LOGError("CCmsWindows: CreateMultiProfileTransform failed (err %lu)", GetLastError());
			delete h->mutex;
			delete h;
			return NULL;
		}
		h->perThread[GetCurrentThreadId()] = t;
		return (CmsTransform *)h;
	}

	virtual void DestroyTransform(CmsTransform *transform) override
	{
		WinTransformHandle *h = (WinTransformHandle *)transform;
		if (h == NULL)
			return;
		for (std::map<DWORD, HTRANSFORM>::iterator it = h->perThread.begin();
		     it != h->perThread.end(); ++it)
		{
			if (it->second != NULL)
				DeleteColorTransform(it->second);
		}
		// Profiles are borrowed, never closed here -- closing them would
		// double-free against CColorManager.
		delete h->mutex;
		delete h;
	}

	virtual void Transform(CmsTransform *transform, const u16 *src, u16 *dst,
	                       u32 pixelCount) override
	{
		if (src == NULL || dst == NULL || pixelCount == 0)
			return;

		WinTransformHandle *h = (WinTransformHandle *)transform;
		if (h == NULL)
		{
			if (src != dst)
				memcpy(dst, src, (size_t)pixelCount * 4 * sizeof(u16));
			return;
		}

		HTRANSFORM t = GetThreadTransform(h);
		if (t == NULL)
		{
			// Fail soft. Transform() has no error channel by design, and
			// unmanaged pixels beat garbage or a crash.
			LOGError("CCmsWindows: no transform for this thread; passing pixels through");
			if (src != dst)
				memcpy(dst, src, (size_t)pixelCount * 4 * sizeof(u16));
			return;
		}

		// ICM has no 16-bit RGBA format, so deinterleave to BM_16b_RGB
		// triplets (icm.h, 16 bits per channel) around the call and
		// reinterleave after. NOT BM_xRGBQUADS: that is 8 bits per channel and
		// would silently reinstate the 8-to-8 transform this programme exists
		// to avoid. Alpha bypasses the CMM entirely, which the interface
		// contract requires anyway.
		const u32 kBlock = 65536;
		std::vector<u16> scratchIn, scratchOut;

		u32 done = 0;
		while (done < pixelCount)
		{
			const u32 n = (pixelCount - done > kBlock) ? kBlock : (pixelCount - done);
			scratchIn.resize((size_t)n * 3);
			scratchOut.resize((size_t)n * 3);

			const u16 *s = src + (size_t)done * 4;
			for (u32 i = 0; i < n; i++)
			{
				scratchIn[i * 3 + 0] = s[i * 4 + 0];
				scratchIn[i * 3 + 1] = s[i * 4 + 1];
				scratchIn[i * 3 + 2] = s[i * 4 + 2];
			}

			BOOL ok = TranslateBitmapBits(
				t,
				&scratchIn[0], BM_16b_RGB, n, 1, (DWORD)(n * 3 * sizeof(u16)),
				&scratchOut[0], BM_16b_RGB, (DWORD)(n * 3 * sizeof(u16)),
				NULL, 0);

			u16 *o = dst + (size_t)done * 4;
			if (ok)
			{
				for (u32 i = 0; i < n; i++)
				{
					o[i * 4 + 0] = scratchOut[i * 3 + 0];
					o[i * 4 + 1] = scratchOut[i * 3 + 1];
					o[i * 4 + 2] = scratchOut[i * 3 + 2];
					o[i * 4 + 3] = s[i * 4 + 3];   // alpha copied around the CMM
				}
			}
			else
			{
				LOGError("CCmsWindows: TranslateBitmapBits failed (err %lu)", GetLastError());
				if (o != s)
					memcpy(o, s, (size_t)n * 4 * sizeof(u16));
			}
			done += n;
		}
	}

	virtual const char *GetEngineName() override
	{
		return (variant == CMS_VARIANT_WCS) ? "WCS 1.0" : "ICM 2.0";
	}

private:
	// True only when the thread is provably gone. A NULL handle means the id
	// no longer names a thread at all; WAIT_OBJECT_0 on a SYNCHRONIZE handle
	// means it has exited. Anything else (including an access failure) is
	// treated as ALIVE, so we can never delete a transform another thread is
	// inside TranslateBitmapBits with.
	static bool ThreadHasExited(DWORD tid)
	{
		HANDLE th = OpenThread(SYNCHRONIZE, FALSE, tid);
		if (th == NULL)
			return GetLastError() == ERROR_INVALID_PARAMETER;   // no such thread
		const DWORD w = WaitForSingleObject(th, 0);
		CloseHandle(th);
		return w == WAIT_OBJECT_0;
	}

	// Caller holds h->mutex. The per-thread map is keyed by thread id and its
	// entries used to live as long as the cache entry did, so every distinct
	// thread that ever converted through this pair left one live HTRANSFORM
	// behind -- and PhotoCruise spawns a first-frame worker per clip, so the
	// ids keep changing (programme review, engine finding 1). Pruning is
	// bounded work, only above a threshold, and only for threads proven dead.
	void PruneDeadThreadTransforms(WinTransformHandle *h)
	{
		const size_t kPruneAbove = 16;   // ~4 decode + render + executor + churn
		if (h->perThread.size() <= kPruneAbove)
			return;
		for (std::map<DWORD, HTRANSFORM>::iterator it = h->perThread.begin();
			 it != h->perThread.end(); )
		{
			if (it->first != GetCurrentThreadId() && ThreadHasExited(it->first))
			{
				if (it->second != NULL)
					DeleteColorTransform(it->second);
				it = h->perThread.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	HTRANSFORM GetThreadTransform(WinTransformHandle *h)
	{
		const DWORD tid = GetCurrentThreadId();
		h->mutex->Lock();
		std::map<DWORD, HTRANSFORM>::iterator it = h->perThread.find(tid);
		if (it != h->perThread.end())
		{
			HTRANSFORM t = it->second;
			h->mutex->Unlock();
			return t;
		}
		// Only on the insert path: a hit costs nothing, and the map can only
		// grow here.
		PruneDeadThreadTransforms(h);
		HTRANSFORM t = CreateIcmTransform(h);
		if (t != NULL)
			h->perThread[tid] = t;
		h->mutex->Unlock();
		return t;
	}

	CmsEngineVariant variant;
};

} // namespace

ICmsEngine *CmsCreateWindowsEngine(CmsEngineVariant variant)
{
	return new CCmsWindows(variant);
}

#endif // WIN32
