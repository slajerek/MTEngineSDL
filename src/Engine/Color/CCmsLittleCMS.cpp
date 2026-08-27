#if MT_ENABLE_LCMS2

#include "ICmsEngine.h"
#include "DBG_Log.h"

#include <lcms2.h>

#include <cstring>

// ---------------------------------------------------------------------------
// CM-B "Fast" quality tier: NOT overridden here, and that is a measurement gap,
// not a decision.
//
// This backend inherits ICmsEngine::Transform8's portable default, which
// promotes to 16 bits, calls Transform and truncates -- i.e. it does exactly
// the work the Fast tier exists to skip, so Fast currently buys lcms2 users
// only the dither, not the conversion saving.
//
// Overriding it means creating a SECOND cmsHTRANSFORM: unlike ColorSync (which
// takes the pixel depth per call), lcms2 bakes the pixel format into the
// transform at cmsCreateTransform time -- TYPE_RGBA_8 vs TYPE_RGBA_16. So an
// override has to either build both formats up front (doubling the cost of the
// operation CColorManager's cache exists to amortise) or build the 8-bit one
// lazily under a lock, since four decode workers share one transform.
//
// Before doing either, PROFILE IT. The 77%-of-transform-time figure that
// motivated this tier was measured against ColorSync on macOS; lcms2 may split
// its cost completely differently, and a native 8-bit path might buy little.
// lcms2 is the default engine on BOTH Windows and Linux, so this is the
// measurement that matters most for those platforms.
// ---------------------------------------------------------------------------


namespace
{

struct LcmsProfileHandle
{
	cmsHPROFILE profile = NULL;
};

struct LcmsTransformHandle
{
	cmsHTRANSFORM transform = NULL;
};

cmsUInt32Number IntentToLcms(CmsRenderingIntent intent)
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

class CCmsLittleCMS : public ICmsEngine
{
public:
	virtual ~CCmsLittleCMS() {}

	virtual CmsProfile *OpenProfile(const u8 *bytes, u32 size) override
	{
		if (bytes == NULL || size == 0)
			return NULL;
		cmsHPROFILE p = cmsOpenProfileFromMem(bytes, (cmsUInt32Number)size);
		if (p == NULL)
			return NULL;
		LcmsProfileHandle *h = new LcmsProfileHandle();
		h->profile = p;
		return (CmsProfile *)h;
	}

	virtual void CloseProfile(CmsProfile *profile) override
	{
		LcmsProfileHandle *h = (LcmsProfileHandle *)profile;
		if (h == NULL)
			return;
		if (h->profile != NULL)
			cmsCloseProfile(h->profile);
		delete h;
	}

	virtual CmsTransform *CreateTransform(CmsProfile *src, CmsProfile *dst,
	                                      CmsRenderingIntent intent,
	                                      bool blackPointCompensation) override
	{
		LcmsProfileHandle *s = (LcmsProfileHandle *)src;
		LcmsProfileHandle *d = (LcmsProfileHandle *)dst;
		if (s == NULL || d == NULL || s->profile == NULL || d->profile == NULL)
			return NULL;

		// cmsFLAGS_NOCACHE is NOT optional. A shared cmsHTRANSFORM carries a
		// one-pixel internal cache, so concurrent cmsDoTransform on one handle
		// is a data race without it -- the same four-worker hazard the Windows
		// backend designs around with per-thread handles, closed here by a flag.
		cmsUInt32Number flags = cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA;
		if (blackPointCompensation)
			flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;

		cmsHTRANSFORM t = cmsCreateTransform(s->profile, TYPE_RGBA_16,
		                                     d->profile, TYPE_RGBA_16,
		                                     IntentToLcms(intent), flags);
		if (t == NULL)
		{
			LOGError("CCmsLittleCMS: cmsCreateTransform failed");
			return NULL;
		}
		LcmsTransformHandle *h = new LcmsTransformHandle();
		h->transform = t;
		return (CmsTransform *)h;
	}

	virtual void DestroyTransform(CmsTransform *transform) override
	{
		LcmsTransformHandle *h = (LcmsTransformHandle *)transform;
		if (h == NULL)
			return;
		if (h->transform != NULL)
			cmsDeleteTransform(h->transform);
		// Profiles are borrowed; CColorManager closes them.
		delete h;
	}

	virtual void Transform(CmsTransform *transform, const u16 *src, u16 *dst,
	                       u32 pixelCount) override
	{
		if (src == NULL || dst == NULL || pixelCount == 0)
			return;
		LcmsTransformHandle *h = (LcmsTransformHandle *)transform;
		if (h == NULL || h->transform == NULL)
		{
			if (src != dst)
				memcpy(dst, src, (size_t)pixelCount * 4 * sizeof(u16));
			return;
		}
		// TYPE_RGBA_16 with cmsFLAGS_COPY_ALPHA: the buffer goes through as-is
		// and alpha is carried across untouched.
		cmsDoTransform(h->transform, src, dst, (cmsUInt32Number)pixelCount);
	}

	virtual const char *GetEngineName() override { return "LittleCMS"; }
};

} // namespace

ICmsEngine *CmsCreateLittleCmsEngine()
{
	return new CCmsLittleCMS();
}

#else // !MT_ENABLE_LCMS2

// Stub, matching the CImageDataTIFF.cpp idiom: guard the body, leave the header
// unconditional, leave the file in all three build lists, degrade to a stub.
// Capability gating then needs no file-list churn, which is what makes the whole
// programme affordable.
//
// MEASURED: this is belt-and-braces rather than a link fix. Every call site is
// already guarded -- CCmsEngineFactory.cpp routes around lcms2 on all three
// platforms when the flag is off, and on Windows it degrades to WCS 1.0 rather
// than ICM 2.0 for a documented reason. So the TU compiling to nothing did not
// actually break the link today.
//
// It is here for the call site nobody has written yet: an unguarded caller now
// gets a nullptr, which the factory's own contract already defines as
// "unmanaged, never an error", instead of an undefined symbol at link time in
// a different file.

#include "ICmsEngine.h"

ICmsEngine *CmsCreateLittleCmsEngine()
{
	return nullptr;
}

#endif // MT_ENABLE_LCMS2
