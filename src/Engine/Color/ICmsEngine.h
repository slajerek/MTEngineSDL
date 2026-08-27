#pragma once

#include "SYS_Defs.h"

// Opaque backend types. Every platform CMS has its own profile and transform
// handles; nothing above this interface may look inside them.
struct CmsProfile;
struct CmsTransform;

enum CmsRenderingIntent : int
{
	CMS_INTENT_PERCEPTUAL             = 0,
	CMS_INTENT_RELATIVE_COLORIMETRIC  = 1,
	CMS_INTENT_SATURATION             = 2,
	CMS_INTENT_ABSOLUTE_COLORIMETRIC  = 3
};

enum CmsEngineVariant : int
{
	CMS_VARIANT_DEFAULT = 0,   // ColorSync (macOS) / ICM 2.0 (Windows) / lcms2 (Linux)
	CMS_VARIANT_WCS     = 1,   // Windows only; other platforms treat it as DEFAULT
	CMS_VARIANT_LCMS2   = 2    // Windows (selectable alongside ICM 2.0/WCS 1.0) and
	                           // Linux (where it IS the DEFAULT); macOS treats it as DEFAULT
};

// The narrow seam between colour policy and the three platform CMSes.
//
// Narrow on purpose: three implementations means every method is written three
// times and can diverge three ways, so the surface is kept to what the pipeline
// genuinely needs. Two things it deliberately does NOT do:
//
//  - It takes no rendering intent per call. The intent is part of the transform
//    and therefore part of the cache key.
//  - It exposes no profile introspection. Identity questions are answered from
//    the bytes by CIccProfileCodec, not by asking a backend "what colour space
//    is this" -- policy code that branches on profile identity is how
//    single-step conversion quietly becomes two-step.
class ICmsEngine
{
public:
	virtual ~ICmsEngine() {}

	// NULL on malformed input. Callers pre-validate with
	// CIccProfileCodec::ValidateHeader, but a backend must ALSO survive
	// anything that slips past it: profiles arrive from untrusted files, and a
	// CMM handed a broken one crashes rather than returning bad colour.
	virtual CmsProfile *OpenProfile(const u8 *bytes, u32 size) = 0;
	virtual void        CloseProfile(CmsProfile *profile) = 0;

	// NULL on failure -- the caller (CColorManager) then falls back to identity
	// rather than propagating an error. This is the only call in the interface
	// that can report failure, so anything fallible must happen here.
	//
	// The profiles are NOT owned by the returned transform: CColorManager owns
	// them and closes them exactly once, after DestroyTransform. A backend that
	// closed them here would double-free.
	virtual CmsTransform *CreateTransform(CmsProfile *src, CmsProfile *dst,
	                                      CmsRenderingIntent intent,
	                                      bool blackPointCompensation) = 0;
	virtual void          DestroyTransform(CmsTransform *transform) = 0;

	// RGBA16, native-endian, 4 channels in and out; src may alias dst.
	// Alpha passes through untouched -- it is not a colour channel and must
	// never be run through a CMM.
	//
	// Must be safe for concurrent calls on the SAME transform: four decode
	// workers share one. Each backend guarantees that its own way (immutable
	// transforms on macOS, per-thread handles on Windows, a NOCACHE flag on
	// lcms2).
	//
	// Cannot fail. A backend that finds itself unable to convert copies src to
	// dst unchanged and logs -- unmanaged pixels beat garbage or a crash.
	virtual void Transform(CmsTransform *transform, const u16 *src, u16 *dst,
	                       u32 pixelCount) = 0;

	// RGBA8 fast path: the same conversion as Transform, but taking and
	// returning 8-bit pixels so the caller can skip the 8->16 promote and the
	// dither back down. Same concurrency contract as Transform, same alpha
	// rule, and it cannot fail.
	//
	// NOT pure: the default implementation below promotes to 16 bits in
	// bounded chunks, calls Transform and truncates, so every backend is
	// correct without writing any code. A backend should override it only when
	// its CMM has a genuinely cheaper 8-bit path -- ColorSync does, because it
	// takes the pixel depth per conversion call. Whether lcms2 and the Windows
	// CMMs are faster with a native 8-bit transform has NOT been measured; see
	// the note in CCmsLittleCMS.cpp / CCmsWindows.cpp before assuming.
	virtual void Transform8(CmsTransform *transform, const u8 *src, u8 *dst,
	                        u32 pixelCount);

	// For the About box and diagnostics: "ColorSync" / "ICM 2.0" / "WCS 1.0" /
	// "LittleCMS".
	virtual const char *GetEngineName() = 0;
};

// The platform backend, or NULL when none is compiled in. Defined once in
// CCmsEngineFactory.cpp, which compiles on every platform and does nothing but
// dispatch -- that is what lets a platform without a backend still link.
ICmsEngine *CmsCreatePlatformEngine(CmsEngineVariant variant);

// Per-backend constructors, each defined in its own platform file.
ICmsEngine *CmsCreateColorSyncEngine();                        // CCmsColorSync.mm
ICmsEngine *CmsCreateWindowsEngine(CmsEngineVariant variant);  // CCmsWindows.cpp
ICmsEngine *CmsCreateLittleCmsEngine();                        // CCmsLittleCMS.cpp
