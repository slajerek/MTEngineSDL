#if defined(MACOS) || defined(__APPLE__)

#import "ICmsEngine.h"
#import "DBG_Log.h"

#import <ColorSync/ColorSync.h>
#import <CoreFoundation/CoreFoundation.h>

#include <cstring>

namespace
{

// The opaque handles above are just these, reinterpreted.
struct ColorSyncProfileHandle
{
	ColorSyncProfileRef profile;
};

struct ColorSyncTransformHandle
{
	ColorSyncTransformRef transform;
};

CFStringRef IntentToKey(CmsRenderingIntent intent)
{
	switch (intent)
	{
		case CMS_INTENT_PERCEPTUAL:            return kColorSyncRenderingIntentPerceptual;
		case CMS_INTENT_SATURATION:            return kColorSyncRenderingIntentSaturation;
		case CMS_INTENT_ABSOLUTE_COLORIMETRIC: return kColorSyncRenderingIntentAbsolute;
		case CMS_INTENT_RELATIVE_COLORIMETRIC:
		default:                               return kColorSyncRenderingIntentRelative;
	}
}

// One element of the profile sequence. kColorSyncTransformTag is REQUIRED --
// without it the array is not a valid device-to-device chain and creation
// fails. Black-point compensation is an optional key HERE, per element; the
// separate `options` dictionary is for global settings (preferred CMM,
// quality) and silently ignores BPC.
CFDictionaryRef MakeSequenceElement(ColorSyncProfileRef profile,
                                    CmsRenderingIntent intent,
                                    CFStringRef transformTag,
                                    bool bpc)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 4,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (d == NULL)
		return NULL;
	CFDictionarySetValue(d, kColorSyncProfile, profile);
	CFDictionarySetValue(d, kColorSyncRenderingIntent, IntentToKey(intent));
	CFDictionarySetValue(d, kColorSyncTransformTag, transformTag);
	CFDictionarySetValue(d, kColorSyncBlackPointCompensation,
	                     bpc ? kCFBooleanTrue : kCFBooleanFalse);
	return d;
}

class CCmsColorSync : public ICmsEngine
{
public:
	virtual ~CCmsColorSync() {}

	virtual CmsProfile *OpenProfile(const u8 *bytes, u32 size) override
	{
		if (bytes == NULL || size == 0)
			return NULL;
		CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)bytes, (CFIndex)size);
		if (data == NULL)
			return NULL;
		ColorSyncProfileRef p = ColorSyncProfileCreate(data, NULL);
		CFRelease(data);
		if (p == NULL)
			return NULL;

		ColorSyncProfileHandle *h = new ColorSyncProfileHandle();
		h->profile = p;
		return (CmsProfile *)h;
	}

	virtual void CloseProfile(CmsProfile *profile) override
	{
		ColorSyncProfileHandle *h = (ColorSyncProfileHandle *)profile;
		if (h == NULL)
			return;
		if (h->profile != NULL)
			CFRelease(h->profile);
		delete h;
	}

	virtual CmsTransform *CreateTransform(CmsProfile *src, CmsProfile *dst,
	                                      CmsRenderingIntent intent,
	                                      bool blackPointCompensation) override
	{
		ColorSyncProfileHandle *s = (ColorSyncProfileHandle *)src;
		ColorSyncProfileHandle *d = (ColorSyncProfileHandle *)dst;
		if (s == NULL || d == NULL || s->profile == NULL || d->profile == NULL)
			return NULL;

		CFDictionaryRef e0 = MakeSequenceElement(s->profile, intent,
		                                         kColorSyncTransformDeviceToPCS,
		                                         blackPointCompensation);
		CFDictionaryRef e1 = MakeSequenceElement(d->profile, intent,
		                                         kColorSyncTransformPCSToDevice,
		                                         blackPointCompensation);
		if (e0 == NULL || e1 == NULL)
		{
			if (e0) CFRelease(e0);
			if (e1) CFRelease(e1);
			return NULL;
		}

		const void *elems[2] = { e0, e1 };
		CFArrayRef sequence = CFArrayCreate(kCFAllocatorDefault, elems, 2,
		                                    &kCFTypeArrayCallBacks);
		CFRelease(e0);
		CFRelease(e1);
		if (sequence == NULL)
			return NULL;

		ColorSyncTransformRef t = ColorSyncTransformCreate(sequence, NULL);
		CFRelease(sequence);
		if (t == NULL)
		{
			LOGError("CCmsColorSync: ColorSyncTransformCreate failed");
			return NULL;
		}

		ColorSyncTransformHandle *h = new ColorSyncTransformHandle();
		h->transform = t;
		return (CmsTransform *)h;
	}

	virtual void DestroyTransform(CmsTransform *transform) override
	{
		ColorSyncTransformHandle *h = (ColorSyncTransformHandle *)transform;
		if (h == NULL)
			return;
		if (h->transform != NULL)
			CFRelease(h->transform);
		// Profiles are NOT ours to close -- CColorManager owns them.
		delete h;
	}

	// CM-B's Fast quality tier. ColorSync takes the pixel depth per conversion
	// call, so the SAME transform object converts 8-bit directly -- no promote,
	// no dither, and half the bytes through the CMM. That is the whole point of
	// overriding the portable default, which would do the promote/truncate work
	// the tier exists to skip.
	virtual void Transform8(CmsTransform *transform, const u8 *src, u8 *dst,
	                        u32 pixelCount) override
	{
		if (src == NULL || dst == NULL || pixelCount == 0)
			return;

		ColorSyncTransformHandle *h = (ColorSyncTransformHandle *)transform;
		if (h == NULL || h->transform == NULL)
		{
			if (src != dst)
				memcpy(dst, src, (size_t)pixelCount * 4);
			return;
		}

		const size_t bytesPerRow = (size_t)pixelCount * 4;
		const ColorSyncDataLayout layout = kColorSyncAlphaLast | kColorSyncByteOrderDefault;

		bool ok = ColorSyncTransformConvert(
			h->transform,
			(size_t)pixelCount, 1,
			dst, kColorSync8BitInteger, layout, bytesPerRow,
			src, kColorSync8BitInteger, layout, bytesPerRow,
			NULL);

		if (!ok)
		{
			// Same fail-soft contract as Transform: unmanaged pixels beat
			// garbage, and this call has no error channel by design.
			LOGError("CCmsColorSync: 8-bit ColorSyncTransformConvert failed; passing pixels through");
			if (src != dst)
				memcpy(dst, src, bytesPerRow);
		}
	}

	virtual void Transform(CmsTransform *transform, const u16 *src, u16 *dst,
	                       u32 pixelCount) override
	{
		if (src == NULL || dst == NULL || pixelCount == 0)
			return;

		ColorSyncTransformHandle *h = (ColorSyncTransformHandle *)transform;
		if (h == NULL || h->transform == NULL)
		{
			if (src != dst)
				memcpy(dst, src, (size_t)pixelCount * 4 * sizeof(u16));
			return;
		}

		// No deinterleave needed. ColorSync's data layout understands
		// non-premultiplied RGBA (kColorSyncAlphaLast) at 16-bit depth, so the
		// buffer goes through as-is and alpha is left alone -- which is exactly
		// the interface contract. Treating the image as one long row keeps the
		// call count at one regardless of the caller's row blocking.
		const size_t bytesPerRow = (size_t)pixelCount * 4 * sizeof(u16);
		const ColorSyncDataLayout layout = kColorSyncAlphaLast | kColorSyncByteOrderDefault;

		bool ok = ColorSyncTransformConvert(
			h->transform,
			(size_t)pixelCount, 1,
			dst, kColorSync16BitInteger, layout, bytesPerRow,
			src, kColorSync16BitInteger, layout, bytesPerRow,
			NULL);

		if (!ok)
		{
			// Fail soft: unmanaged pixels beat garbage. Transform() has no
			// error channel by design -- the fallible operation is
			// CreateTransform.
			LOGError("CCmsColorSync: ColorSyncTransformConvert failed; passing pixels through");
			if (src != dst)
				memcpy(dst, src, bytesPerRow);
		}
	}

	virtual const char *GetEngineName() override { return "ColorSync"; }
};

} // namespace

ICmsEngine *CmsCreateColorSyncEngine()
{
	return new CCmsColorSync();
}

#endif // MACOS / __APPLE__
