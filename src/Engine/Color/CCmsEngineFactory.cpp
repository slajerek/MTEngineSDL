#include "ICmsEngine.h"
#include "DBG_Log.h"

#include <algorithm>
#include <vector>

// Platform dispatch, and nothing else. Deliberately free of any platform API
// call so it compiles everywhere -- including on a platform whose backend has
// not been written yet, which is what keeps the build green between the tasks
// that add them one at a time.

// ---------------------------------------------------------------------------
// ICmsEngine::Transform8 -- the portable default (CM-B "Fast" quality tier)
// ---------------------------------------------------------------------------
//
// Promote to 16 bits, convert, truncate. Deliberately chunked: a full-image
// scratch buffer for a 24MP photo would be ~200 MB per worker, which is the
// same trap the app-side pipeline avoids by row-blocking.
//
// This is a CORRECTNESS fallback, not a performance one -- it does the very
// work the Fast tier exists to skip, so a backend using it will be no faster
// than Accurate (just slightly cheaper: no ordered dither). Overriding it is
// what makes the tier pay. ColorSync does; lcms2 and the Windows CMMs have not
// been profiled for it yet.
void ICmsEngine::Transform8(CmsTransform *transform, const u8 *src, u8 *dst,
                            u32 pixelCount)
{
	if (src == NULL || dst == NULL || pixelCount == 0)
		return;

	const u32 kChunkPixels = 64u * 1024u;          // 512 KB of u16 scratch
	std::vector<u16> scratch((size_t)std::min(pixelCount, kChunkPixels) * 4);

	u32 done = 0;
	while (done < pixelCount)
	{
		const u32 n = std::min(kChunkPixels, pixelCount - done);
		const size_t count = (size_t)n * 4;

		const u8 *s = src + (size_t)done * 4;
		u8       *d = dst + (size_t)done * 4;

		// MSB-replicated, so 255 maps to 65535 and an identity stays identity.
		for (size_t i = 0; i < count; i++)
			scratch[i] = (u16)(((u16)s[i] << 8) | s[i]);

		Transform(transform, scratch.data(), scratch.data(), n);

		// Truncation, not dithering: the caller chose the Fast tier precisely
		// to skip that step, and the engine must not reintroduce it.
		for (size_t i = 0; i < count; i++)
			d[i] = (u8)(scratch[i] >> 8);

		done += n;
	}
}

ICmsEngine *CmsCreatePlatformEngine(CmsEngineVariant variant)
{
#if defined(MACOS) || defined(__APPLE__)
	// ColorSync has no engine variants; WCS and lcms2 are not offered here.
	(void)variant;
	return CmsCreateColorSyncEngine();
#elif defined(WIN32)
	// lcms2 is a selectable Windows variant too (see CmsEngineVariant), not
	// only the Linux default -- CMS_VARIANT_LCMS2 routes here instead of to
	// CmsCreateWindowsEngine, whose own variant switch only knows ICM 2.0
	// (CMS_VARIANT_DEFAULT) and WCS 1.0 (CMS_VARIANT_WCS). Guarded so a
	// checkout that has not rebuilt the image-codecs bundle (no
	// MT_ENABLE_LCMS2) still links -- it just cannot select this variant.
#if MT_ENABLE_LCMS2
	if (variant == CMS_VARIANT_LCMS2)
		return CmsCreateLittleCmsEngine();
#else
	// lcms2 was ASKED FOR but is not in this build (the image-codecs bundle
	// was not rebuilt). Fall back to WCS 1.0 -- never to ICM 2.0.
	//
	// This was a live, silent defect: the app applies the persisted
	// `colorManagement.engine` setting at startup and its DEFAULT is
	// LittleCMS, so a fresh install on such a build asked for lcms2, fell
	// through this function to CmsCreateWindowsEngine(CMS_VARIANT_LCMS2),
	// and landed on ICM 2.0 -- the one engine with the measured
	// wide-gamut-SOURCE conversion defect (a wide-gamut source is the common
	// case for pro camera JPEGs, and it is ALL of RAW Develop's output).
	// WCS does not have that defect, so it is the correct degradation.
	if (variant == CMS_VARIANT_LCMS2)
	{
		LOGD("CmsCreatePlatformEngine: lcms2 requested but not built in; using WCS 1.0 "
		     "(never ICM 2.0 -- it converts wide-gamut sources incorrectly)");
		variant = CMS_VARIANT_WCS;
	}
#endif
	return CmsCreateWindowsEngine(variant);
#elif MT_ENABLE_LCMS2
	// lcms2 has no variants on Linux -- it is unconditionally DEFAULT there.
	(void)variant;
	return CmsCreateLittleCmsEngine();
#else
	// No CMS on this platform/build: colour management degrades to identity.
	// Callers must treat NULL as "unmanaged", never as an error.
	(void)variant;
	return NULL;
#endif
}
