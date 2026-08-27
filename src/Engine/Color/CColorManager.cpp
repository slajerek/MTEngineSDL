#include "CColorManager.h"

#include "CIccProfileCodec.h"
#include "ICC_SRGBProfile.h"
#include "SYS_Threading.h"
#include "DBG_Log.h"

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace
{

const int kMaxCacheEntries = 16;

// (srcDigest, dstDigest, intent, bpc). Content digests, never profile IDs: the
// ICC header's ID field is attacker-controlled and CIccProfileCodec hands it
// back verbatim, so keying on it would let a crafted file borrow the display
// profile's identity -- testing as identity and skipping conversion, or
// colliding with an unrelated cached transform.
//
// The engine variant is NOT part of the key: SetEngineVariant flushes the whole
// cache, so entries from two variants never coexist.
struct CacheKey
{
	u8  srcDigest[16];
	u8  dstDigest[16];
	u8  intent;
	u8  bpc;

	bool operator<(const CacheKey &o) const
	{
		int c = memcmp(srcDigest, o.srcDigest, 16);
		if (c != 0) return c < 0;
		c = memcmp(dstDigest, o.dstDigest, 16);
		if (c != 0) return c < 0;
		if (intent != o.intent) return intent < o.intent;
		return bpc < o.bpc;
	}
};

// Heap-owned, never stored by value: a lease is a pointer into an entry, and
// storing entries by value would leave leases dangling into erased map buckets.
struct CacheEntry
{
	CacheKey      key;
	CmsTransform *transform = NULL;
	CmsProfile   *srcProfile = NULL;
	CmsProfile   *dstProfile = NULL;
	// The engine that made these handles -- they must be destroyed by it, not
	// by whichever engine happens to be current at release time.
	ICmsEngine   *owner = NULL;
	int           refCount = 0;
	bool          evicted = false;   // unlinked from the map, awaiting last release
	u64           lruTick = 0;
};

// Retiring backends are refcounted by the entries that still hold their
// handles, so a lease can never outlive the engine that created it.
struct EngineSlot
{
	ICmsEngine *engine = NULL;
	int         liveEntries = 0;
};

} // namespace

// A lease points at an entry, or at nothing when it is the identity lease.
struct CCmsTransformLease
{
	CacheEntry *entry;   // NULL for identity
};

// The identity lease is a singleton, not heap-allocated: an untagged image on
// an sRGB display hits it on every decode, and allocating one per acquisition
// would leak steadily. ReleaseTransform recognises it and does nothing.
static CCmsTransformLease gIdentityLease = { NULL };

class CColorManagerImpl
{
public:
	CSlrMutex *mutex;
	EngineSlot current;
	std::vector<EngineSlot> retiring;
	std::map<CacheKey, CacheEntry *> cache;
	u64 lruClock;
	int liveTransforms;
	CmsEngineVariant variant;

#if defined(WIN32) && (!defined(MT_ENABLE_LCMS2) || (MT_ENABLE_LCMS2))
	// lcms2, not CMS_VARIANT_DEFAULT (ICM 2.0) or CMS_VARIANT_WCS: ICM 2.0 has
	// a confirmed, measured defect -- it produces a near-identity transform
	// when a wide-gamut matrix/TRC profile (e.g. Adobe RGB, the common case
	// for pro camera JPEGs) is the transform SOURCE. WCS 1.0 does not have
	// that defect, but lcms2 is the long-term choice: it is the same,
	// independently-audited CMM Linux uses (byte-identical maths on both
	// platforms instead of trusting two different vendor CMMs), its transform
	// creation is not "ruinously" expensive the way WCS's is (see
	// EvictIfNeeded below), and it is MIT-licensed with no Store-compatibility
	// concern. ICM 2.0 and WCS 1.0 remain selectable via SetEngineVariant for
	// users who want them (see PhotoCruise's colour-engine setting).
	CColorManagerImpl()
		: mutex(new CSlrMutex("CColorManager")), lruClock(0), liveTransforms(0),
		  variant(CMS_VARIANT_LCMS2)
	{
		current.engine = CmsCreatePlatformEngine(CMS_VARIANT_LCMS2);
		current.liveEntries = 0;
		if (current.engine == NULL)
			LOGD("CColorManager: no CMS backend on this platform; colour management is identity");
	}
#elif defined(WIN32)
	// lcms2 was not built into this checkout (image-codecs bundle not
	// rebuilt) -- WCS 1.0 over ICM 2.0 for the same reason as above, minus
	// the lcms2-specific wins: it does not have ICM 2.0's wide-gamut-source
	// defect.
	CColorManagerImpl()
		: mutex(new CSlrMutex("CColorManager")), lruClock(0), liveTransforms(0),
		  variant(CMS_VARIANT_WCS)
	{
		current.engine = CmsCreatePlatformEngine(CMS_VARIANT_WCS);
		current.liveEntries = 0;
		if (current.engine == NULL)
			LOGD("CColorManager: no CMS backend on this platform; colour management is identity");
	}
#else
	CColorManagerImpl()
		: mutex(new CSlrMutex("CColorManager")), lruClock(0), liveTransforms(0),
		  variant(CMS_VARIANT_DEFAULT)
	{
		current.engine = CmsCreatePlatformEngine(CMS_VARIANT_DEFAULT);
		current.liveEntries = 0;
		if (current.engine == NULL)
			LOGD("CColorManager: no CMS backend on this platform; colour management is identity");
	}
#endif

	// Caller holds the mutex.
	void DestroyEntry(CacheEntry *e)
	{
		ICmsEngine *eng = e->owner;
		if (eng != NULL)
		{
			if (e->transform != NULL)
			{
				eng->DestroyTransform(e->transform);
				liveTransforms--;
			}
			// Profiles are owned here, not by the transform -- backends treat
			// them as borrowed, so this is the only place they are closed.
			if (e->srcProfile != NULL) eng->CloseProfile(e->srcProfile);
			if (e->dstProfile != NULL) eng->CloseProfile(e->dstProfile);
			ReleaseEngineRef(eng);
		}
		delete e;
	}

	// Caller holds the mutex. Drops one entry's claim on an engine and deletes
	// a retired engine once nothing references it.
	void ReleaseEngineRef(ICmsEngine *eng)
	{
		if (eng == current.engine)
		{
			current.liveEntries--;
			return;
		}
		for (size_t i = 0; i < retiring.size(); i++)
		{
			if (retiring[i].engine != eng)
				continue;
			retiring[i].liveEntries--;
			if (retiring[i].liveEntries <= 0)
			{
				delete retiring[i].engine;
				retiring.erase(retiring.begin() + i);
			}
			return;
		}
	}

	// Caller holds the mutex. Evicts the least-recently-used UNLEASED entry.
	// Leased entries are skipped rather than unlinked: exceeding the cap
	// transiently is far better than destroying a handle a worker is using.
	void EvictIfNeeded()
	{
		while ((int)cache.size() > kMaxCacheEntries)
		{
			std::map<CacheKey, CacheEntry *>::iterator victim = cache.end();
			u64 oldest = 0;
			for (std::map<CacheKey, CacheEntry *>::iterator it = cache.begin(); it != cache.end(); ++it)
			{
				if (it->second->refCount != 0)
					continue;
				if (victim == cache.end() || it->second->lruTick < oldest)
				{
					victim = it;
					oldest = it->second->lruTick;
				}
			}
			if (victim == cache.end())
				return;   // everything is leased; stay over the cap for now
			CacheEntry *e = victim->second;
			cache.erase(victim);
			DestroyEntry(e);
		}
	}
};

static CColorManagerImpl *gImpl = NULL;
static CColorManager *gInstance = NULL;

CColorManager::CColorManager() {}
CColorManager::~CColorManager() {}

CColorManager *CColorManager::Instance()
{
	// call_once: two threads first-touching concurrently must not
	// double-construct (programme review 2026-08-11). Init is normally on the
	// main thread, but a decode worker CAN be first in headless test runs.
	static std::once_flag sOnce;
	std::call_once(sOnce, []() {
		gImpl = new CColorManagerImpl();
		gInstance = new CColorManager();
	});
	return gInstance;
}

void CColorManager::Shutdown()
{
	// NOTE: with call_once construction, Instance() after a successful
	// Shutdown() returns NULL (it no longer re-creates). Currently moot --
	// Shutdown has no callers in either repo; the colour manager lives for
	// the process (the app exits via _exit). If a caller ever appears, it
	// owns the ordering.
	if (gInstance == NULL)
		return;

	gImpl->mutex->Lock();
	// Evicted-but-leased entries live OUTSIDE the map (SetEngineVariant
	// unlinks them; they die on their final release and reference a RETIRING
	// engine), so the map scan below cannot see them. The retiring list can:
	// it is non-empty exactly while such entries exist (programme review
	// 2026-08-11 -- tearing down here would leave the eventual release
	// running on a freed gImpl).
	if (!gImpl->retiring.empty())
	{
		LOGError("CColorManager::Shutdown: refusing -- %d retiring engine(s) still hold leased entries",
		         (int)gImpl->retiring.size());
		gImpl->mutex->Unlock();
		return;
	}
	for (std::map<CacheKey, CacheEntry *>::iterator it = gImpl->cache.begin(); it != gImpl->cache.end(); ++it)
	{
		if (it->second->refCount != 0)
		{
			// Tearing down now would retire the backend under a live lease, and
			// the eventual release would run on freed memory.
			LOGError("CColorManager::Shutdown: refusing -- %d lease(s) still outstanding",
			         it->second->refCount);
			gImpl->mutex->Unlock();
			return;
		}
	}
	for (std::map<CacheKey, CacheEntry *>::iterator it = gImpl->cache.begin(); it != gImpl->cache.end(); ++it)
		gImpl->DestroyEntry(it->second);
	gImpl->cache.clear();

	if (gImpl->current.engine != NULL)
	{
		delete gImpl->current.engine;
		gImpl->current.engine = NULL;
	}
	gImpl->mutex->Unlock();

	delete gImpl->mutex;
	delete gImpl;
	gImpl = NULL;
	delete gInstance;
	gInstance = NULL;
}

void CColorManager::SetEngineVariant(CmsEngineVariant newVariant)
{
	gImpl->mutex->Lock();

	// Unlink everything. Unleased entries die now; leased ones are marked and
	// die on their final release, with their (retiring) engine outliving them.
	for (std::map<CacheKey, CacheEntry *>::iterator it = gImpl->cache.begin(); it != gImpl->cache.end(); ++it)
	{
		CacheEntry *e = it->second;
		if (e->refCount == 0)
		{
			gImpl->DestroyEntry(e);
		}
		else
		{
			e->evicted = true;
		}
	}
	gImpl->cache.clear();

	// Retire the old engine if anything still holds its handles.
	if (gImpl->current.engine != NULL)
	{
		if (gImpl->current.liveEntries > 0)
		{
			gImpl->retiring.push_back(gImpl->current);
		}
		else
		{
			delete gImpl->current.engine;
		}
	}
	gImpl->current.engine = CmsCreatePlatformEngine(newVariant);
	gImpl->current.liveEntries = 0;
	gImpl->variant = newVariant;

	gImpl->mutex->Unlock();
}

// Magic-static initialisers: C++11 guarantees thread-safe construction, so
// the historical "first-touch caveat" (two decode workers racing check-then-
// fill) is gone by construction (programme review 2026-08-11). The app-side
// warm blocks remain as harmless belt-and-braces.
const std::vector<u8> &CColorManager::GetSrgbProfileBytes()
{
	static const std::vector<u8> bytes = ICC_BuildSRGBProfileV2();
	return bytes;
}

const std::vector<u8> &CColorManager::GetAdobeRgbProfileBytes()
{
	static const std::vector<u8> bytes = ICC_BuildAdobeRGBProfileV2();
	return bytes;
}

const std::vector<u8> &CColorManager::GetRec709ProfileBytes()
{
	static const std::vector<u8> bytes = ICC_BuildRec709ProfileV2();
	return bytes;
}

const std::vector<u8> &CColorManager::GetDisplayP3ProfileBytes()
{
	static const std::vector<u8> bytes = ICC_BuildDisplayP3ProfileV2();
	return bytes;
}

CCmsTransformLease *CColorManager::AcquireTransform(const u8 *srcProfile, u32 srcSize,
                                                    const u8 *dstProfile, u32 dstSize,
                                                    CmsRenderingIntent intent, bool bpc)
{
	u8 srcDigest[16], dstDigest[16];
	CIccProfileCodec::GetContentDigest(srcProfile, srcSize, srcDigest);
	CIccProfileCodec::GetContentDigest(dstProfile, dstSize, dstDigest);
	return AcquireTransformByDigest(srcDigest, srcProfile, srcSize,
	                                dstDigest, dstProfile, dstSize, intent, bpc);
}

CCmsTransformLease *CColorManager::AcquireTransformByDigest(const u8 srcDigest[16],
                                                            const u8 *srcProfile, u32 srcSize,
                                                            const u8 dstDigest[16],
                                                            const u8 *dstProfile, u32 dstSize,
                                                            CmsRenderingIntent intent, bool bpc)
{
	if (srcProfile == NULL || dstProfile == NULL || srcSize == 0 || dstSize == 0)
		return &gIdentityLease;

	// The fast path that makes untagged-sRGB-on-sRGB-display cost nothing.
	if (memcmp(srcDigest, dstDigest, 16) == 0)
		return &gIdentityLease;

	CacheKey key;
	memcpy(key.srcDigest, srcDigest, 16);
	memcpy(key.dstDigest, dstDigest, 16);
	key.intent = (u8)intent;
	key.bpc = bpc ? 1 : 0;

	gImpl->mutex->Lock();

	std::map<CacheKey, CacheEntry *>::iterator it = gImpl->cache.find(key);
	if (it != gImpl->cache.end())
	{
		CacheEntry *e = it->second;
		e->refCount++;
		e->lruTick = ++gImpl->lruClock;
		gImpl->mutex->Unlock();
		CCmsTransformLease *lease = new CCmsTransformLease();
		lease->entry = e;
		return lease;
	}

	ICmsEngine *eng = gImpl->current.engine;
	if (eng == NULL)
	{
		gImpl->mutex->Unlock();
		return &gIdentityLease;   // no backend: unmanaged, not an error
	}

	// Validate before the CMM sees the bytes. The backends are lenient to
	// differing degrees -- ColorSync, for one, happily opens a profile whose
	// header size field overruns the buffer -- so structural rejection cannot
	// be delegated to them. This is the single choke point every profile
	// passes through, so it is the right place for it.
	if (!CIccProfileCodec::ValidateHeader(srcProfile, srcSize) ||
	    !CIccProfileCodec::ValidateHeader(dstProfile, dstSize))
	{
		gImpl->mutex->Unlock();
		LOGD("CColorManager: profile failed structural validation; falling back to identity");
		return &gIdentityLease;
	}

	// Creation happens under the lock. It is rare (bounded by distinct profile
	// pairs) and serialising it avoids a double-create race; the thing that
	// must never be under the lock is Transform(), which runs per image.
	CmsProfile *sp = eng->OpenProfile(srcProfile, srcSize);
	CmsProfile *dp = eng->OpenProfile(dstProfile, dstSize);
	CmsTransform *t = NULL;
	if (sp != NULL && dp != NULL)
		t = eng->CreateTransform(sp, dp, intent, bpc);

	if (t == NULL)
	{
		// Fail soft. A profile the CMM would not open, or a transform it would
		// not build, must not stop the image being shown.
		if (sp != NULL) eng->CloseProfile(sp);
		if (dp != NULL) eng->CloseProfile(dp);
		gImpl->mutex->Unlock();
		LOGD("CColorManager: transform creation failed; falling back to identity");
		return &gIdentityLease;
	}

	CacheEntry *e = new CacheEntry();
	e->key = key;
	e->transform = t;
	e->srcProfile = sp;
	e->dstProfile = dp;
	e->owner = eng;
	e->refCount = 1;
	e->lruTick = ++gImpl->lruClock;
	gImpl->cache[key] = e;
	gImpl->current.liveEntries++;
	gImpl->liveTransforms++;
	gImpl->EvictIfNeeded();

	gImpl->mutex->Unlock();

	CCmsTransformLease *lease = new CCmsTransformLease();
	lease->entry = e;
	return lease;
}

void CColorManager::ReleaseTransform(CCmsTransformLease *lease)
{
	if (lease == NULL || lease == &gIdentityLease)
		return;   // the identity lease is a singleton; never freed

	gImpl->mutex->Lock();
	CacheEntry *e = lease->entry;
	if (e != NULL)
	{
		e->refCount--;
		if (e->refCount <= 0 && e->evicted)
			gImpl->DestroyEntry(e);   // unlinked earlier; this was the last user
	}
	gImpl->mutex->Unlock();
	delete lease;
}

bool CColorManager::IsIdentity(CCmsTransformLease *lease)
{
	return lease == NULL || lease->entry == NULL || lease->entry->transform == NULL;
}

void CColorManager::Transform(CCmsTransformLease *lease, const u16 *src, u16 *dst, u32 pixelCount)
{
	if (IsIdentity(lease))
	{
		if (src != dst && src != NULL && dst != NULL && pixelCount > 0)
			memcpy(dst, src, (size_t)pixelCount * 4 * sizeof(u16));
		return;
	}
	// Outside the mutex on purpose: this is the expensive per-image call, and
	// four decode workers must run it concurrently.
	CacheEntry *e = lease->entry;
	e->owner->Transform(e->transform, src, dst, pixelCount);
}

void CColorManager::Transform8(CCmsTransformLease *lease, const u8 *src, u8 *dst,
                               u32 pixelCount)
{
	if (IsIdentity(lease))
	{
		if (src != dst && src != NULL && dst != NULL && pixelCount > 0)
			memcpy(dst, src, (size_t)pixelCount * 4);
		return;
	}
	// Outside the mutex, same reasoning as Transform: this is the expensive
	// per-image call and four decode workers must run it concurrently.
	CacheEntry *e = lease->entry;
	e->owner->Transform8(e->transform, src, dst, pixelCount);
}

const char *CColorManager::GetEngineName()
{
	gImpl->mutex->Lock();
	const char *name = gImpl->current.engine ? gImpl->current.engine->GetEngineName() : "none";
	gImpl->mutex->Unlock();
	return name;
}

int CColorManager::GetCacheEntryCount()
{
	gImpl->mutex->Lock();
	int n = (int)gImpl->cache.size();
	gImpl->mutex->Unlock();
	return n;
}

int CColorManager::GetLiveTransformCount()
{
	gImpl->mutex->Lock();
	int n = gImpl->liveTransforms;
	gImpl->mutex->Unlock();
	return n;
}

const void *CColorManager::DebugGetTransformIdentity(CCmsTransformLease *lease)
{
	if (lease == NULL || lease->entry == NULL)
		return NULL;
	return (const void *)lease->entry->transform;
}
