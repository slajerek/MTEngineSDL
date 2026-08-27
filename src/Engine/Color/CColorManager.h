#pragma once

#include "SYS_Defs.h"
#include "ICmsEngine.h"

#include <vector>

class CSlrMutex;

// A leased transform. transform == NULL means identity (source and destination
// are the same profile) or fail-soft (no backend, a profile the CMM refused) --
// either way the caller skips all pixel work. Opaque by design: callers must
// not touch backend handles.
struct CCmsTransformLease;

// Owns the platform CMS backend and caches transforms across decodes.
//
// Transform creation is the expensive operation in every CMS -- ruinously so
// for WCS -- and a folder of camera JPEGs asks for the same source/destination
// pair thousands of times, so it must be cached.
class CColorManager
{
public:
	static CColorManager *Instance();

	// Engine teardown. Must not be called with leases outstanding: it retires
	// the backend, and a later ReleaseTransform would run on a destroyed
	// manager. Logs and refuses rather than tearing down under a live lease.
	static void Shutdown();

	// Rebuilds the backend (the Windows ICM/WCS toggle). Every cached entry is
	// unlinked; entries still leased are destroyed when their last lease is
	// released, and the retiring backend outlives them.
	void SetEngineVariant(CmsEngineVariant variant);

	// Deterministic built-in profile bytes, built once.
	static const std::vector<u8> &GetSrgbProfileBytes();
	static const std::vector<u8> &GetAdobeRgbProfileBytes();
	// CM-E: sRGB primaries with a gamma-2.4 TRC (Rec.709/BT.1886 stand-in),
	// the optional pinned video source profile. Same lazily-built-once
	// pattern and first-touch caveat as the two above.
	static const std::vector<u8> &GetRec709ProfileBytes();
	// CM-F: Display P3 (export output space). Same pattern and caveat.
	static const std::vector<u8> &GetDisplayP3ProfileBytes();

	// Lease a src->dst transform. NEVER returns NULL:
	//   - equal content digests            -> identity lease
	//   - no backend / malformed profile / backend refusal -> identity lease
	// Fail-soft is deliberate: unmanaged pixels beat a crash, and the caller
	// has no better option at decode time.
	//
	// The byte-taking form digests BOTH profiles on every call. CM-B should
	// prefer AcquireTransformByDigest with the source digest memoised on the
	// image and the destination digest memoised against the display serial.
	CCmsTransformLease *AcquireTransform(const u8 *srcProfile, u32 srcSize,
	                                     const u8 *dstProfile, u32 dstSize,
	                                     CmsRenderingIntent intent, bool bpc);
	CCmsTransformLease *AcquireTransformByDigest(const u8 srcDigest[16],
	                                             const u8 *srcProfile, u32 srcSize,
	                                             const u8 dstDigest[16],
	                                             const u8 *dstProfile, u32 dstSize,
	                                             CmsRenderingIntent intent, bool bpc);
	void ReleaseTransform(CCmsTransformLease *lease);

	static bool IsIdentity(CCmsTransformLease *lease);

	// No-op for identity leases. Runs OUTSIDE the manager mutex: a transform
	// that takes milliseconds must not serialise four decode workers.
	void Transform(CCmsTransformLease *lease, const u16 *src, u16 *dst, u32 pixelCount);

	// RGBA8 form (CM-B's Fast quality tier): same conversion, 8-bit in and out,
	// letting the caller skip the promote and the dither. Identity leases copy
	// (or do nothing when src == dst), exactly as the 16-bit form does.
	void Transform8(CCmsTransformLease *lease, const u8 *src, u8 *dst, u32 pixelCount);

	const char *GetEngineName();

	// --- test/diagnostic introspection ---
	int GetCacheEntryCount();      // entries currently in the map
	int GetLiveTransformCount();   // backend transforms created minus destroyed

	// The lease is opaque, but a cache test has to prove that two acquisitions
	// of one key share ONE backend transform -- counts alone cannot tell
	// sharing from two entries that happen to total the same. Returns an
	// opaque identity, never a usable handle.
	static const void *DebugGetTransformIdentity(CCmsTransformLease *lease);

private:
	CColorManager();
	~CColorManager();
};
