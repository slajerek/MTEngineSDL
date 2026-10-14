#include "VID_GainMapDraw.h"
#include "CGainMapShader.h"
#include "CRenderBackend.h"
#include "CSlrImage.h"
#include "MT_UltraHdr.h"
#include "VID_Main.h"
#include "CRenderTarget.h"
#include "SYS_Threading.h"
#include "DBG_Log.h"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

// Main/render thread only (see the header). The scope uses the same pointer.
static CGainMapShader *gGainMapShader = NULL;
static bool gShaderFailed = false;

static std::atomic<int>  gDrawCount(0);
static std::atomic<int>  gSuppressedCount(0);
static std::atomic<bool> gForceFailure(false);

bool VID_GainMapShaderUsable()
{
	if (gGainMapShader == NULL && !gShaderFailed)
	{
		// NULL from the factory -- no backend yet, a Metal backend before its layer
		// exists, or a backend without a gain-map shader (OpenGL) -- is NOT latched:
		// a call before the device is ready must not turn the arm off for the
		// session. It costs one virtual call per frame on OpenGL, nothing else.
		CRenderBackend *be = VID_GetRenderBackend();
		if (be)
		{
			// PC_GPU_FRAME_LOG=1 (measurement aid, option C Task 8): the startup
			// cost of creating + compiling the shader, which every Metal session
			// pays, SDR included. Off by default; never a source edit to turn on.
			static const bool logCost = getenv("PC_GPU_FRAME_LOG") != NULL;
			const auto t0 = std::chrono::steady_clock::now();
			gGainMapShader = be->CreateGainMapShader();
			if (logCost && gGainMapShader != NULL)
				LOGM("PC_GPU_FRAME gain-map shader created in %.3f ms (usable %d)",
					 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
					 (int)gGainMapShader->IsUsable());
		}
		// The constructor compiles synchronously, so IsUsable() is the final answer.
		if (gGainMapShader && !gGainMapShader->IsUsable())
		{
			LOGError("gain map shader failed to compile -- gain-map photos take the float arm");
			delete gGainMapShader;
			gGainMapShader = NULL;
			gShaderFailed = true;   // latched: never recompiled per frame
		}
	}
	return gGainMapShader != NULL && gGainMapShader->IsUsable();
}

bool VID_GainMapShaderCompileFailed()
{
	return gShaderFailed;
}

void MT_GainMapUniformsFrom(const SMTGainMapApply &a, const CSlrImage *base, const CSlrImage *map,
                            SMTGainMapUniforms *out)
{
	SMTGainMapUniforms &u = *out;
	for (int c = 0; c < 3; c++)
	{
		u.logMin[c]   = a.logMinBoost[c];
		u.logMax[c]   = a.logMaxBoost[c];
		u.invGamma[c] = a.invGamma[c];
		u.offSdr[c]   = a.offsetSdr[c];
		u.offHdr[c]   = a.offsetHdr[c];
		u.m0[c] = a.hasMatrix ? a.matrix[0][c] : (c == 0 ? 1.f : 0.f);
		u.m1[c] = a.hasMatrix ? a.matrix[1][c] : (c == 1 ? 1.f : 0.f);
		u.m2[c] = a.hasMatrix ? a.matrix[2][c] : (c == 2 ? 1.f : 0.f);
	}
	// .w is unused by the shader; 1.0 keeps the invGamma == 1 test uniform across lanes.
	u.logMin[3] = u.logMax[3] = u.offSdr[3] = u.offHdr[3] = 0.f;
	u.invGamma[3] = 1.f;
	u.m0[3] = u.m1[3] = u.m2[3] = 0.f;

	u.baseRect[0] = base->defaultTexStartX; u.baseRect[1] = base->defaultTexStartY;
	u.baseRect[2] = base->defaultTexEndX;   u.baseRect[3] = base->defaultTexEndY;
	u.mapRect[0]  = map->defaultTexStartX;  u.mapRect[1]  = map->defaultTexStartY;
	u.mapRect[2]  = map->defaultTexEndX;    u.mapRect[3]  = map->defaultTexEndY;

	u.flags[0] = a.encodeSrgb ? 1.f : 0.f;
	u.flags[1] = (float)a.mapChannels;
	u.flags[2] = 10000.f / 203.f;
	u.flags[3] = base->linearScaling ? 1.f : 0.f;

	// The POSITION RULE (MT_GainMapEvaluate: map coordinate = (x - mapOffset) / mapScale,
	// texel centres at integers), from the APPLY. The derivation is in the header.
	const float bw = base->width, bh = base->height, mw = map->width, mh = map->height;
	const float s = std::max(1.0f, a.mapScale);
	u.mapAffine[0] = bw / (s * mw);
	u.mapAffine[1] = bh / (s * mh);
	u.mapAffine[2] = (0.5f - (0.5f + a.mapOffset[0]) / s) / mw;
	u.mapAffine[3] = (0.5f - (0.5f + a.mapOffset[1]) / s) / mh;
}

// ---------------------------------------------------------------------------
// The one-time composite (Task 3c). See the header for the lifetime rules.
//
// THREADS. The LRU is read and written by the render thread (the scope, the
// probe, the frame tick) and by whatever thread deallocates or destroys a
// CSlrImage (VID_GainMapCompositeForget) -- in production the render thread too
// (VID_BindImages), but a test or a worker may delete an image directly. So the
// list is guarded by one mutex, while every GPU object is created, rendered and
// destroyed on the render thread only: Forget merely moves an entry's target to
// the graveyard, and the frame tick destroys it (CMetalRenderTarget::Destroy
// parks the texture past the frames in flight).

namespace
{
	struct SGainMapComposite
	{
		CSlrImage *base = NULL;
		CSlrImage *map = NULL;
		void *baseTexture = NULL;       // validated on every use: a re-bound base is rebuilt
		void *mapTexture = NULL;
		SMTGainMapUniforms u;           // the draw's uniforms when built (the apply, rects, sizes)
		CRenderTarget *target = NULL;   // owned; its texture IS the composite
		size_t bytes = 0;
		u64 lastDrawnFrame = 0;         // VID_GetCurrentFrameNumber() of the last draw
		u64 lastDrawnSeq = 0;           // a total order of draws, for the LRU
		int slot = 0;                   // the pane that created it (GAINMAP_COMPOSITE_SLOT_*): takeover stays in it
	};

	const int kMaxComposites = 2;   // the photo on screen + the compare 2-up's pinned photo
	// HELD ONLY WHILE DRAWN (controller ruling, Task 3c review): the frame tick
	// releases every entry not drawn in the last kHoldFrames frames. A composite is
	// ~8 B/px of REAL memory the app counts against its RAM guard, so in single view
	// only the photo on screen may hold one -- keeping the previous photo's would
	// halve a small cache's ceiling for nothing; a rebuild is one ~1.75 ms pass.
	const u64 kHoldFrames = 2;
}

// NEVER-DESTROYED heap objects, like the mutex: VID_GainMapCompositeForget runs from
// every ~CSlrImage, and a CSlrImage can die during static destruction, after a
// static std::vector here would already be gone.
static std::vector<SGainMapComposite> &gComposites = *new std::vector<SGainMapComposite>();   // guarded by CompositeMutex()
static std::vector<CRenderTarget *> &gCompositeGraveyard = *new std::vector<CRenderTarget *>(); // guarded; destroyed by the frame tick
static std::atomic<int>    gCompositeGraveyardCount(0);     // gCompositeGraveyard.size(), lock-free reads
static u64 gCompositeSeq = 0;                               // guarded
static std::atomic<int>    gCompositeCount(0);              // gComposites.size(), lock-free reads
static std::atomic<size_t> gCompositeLiveBytes(0);          // every composite texture alive, graveyard included
static std::atomic<int>    gCompositeBuilds(0);
static std::atomic<int>    gCompositeFallbacks(0);
static std::atomic<bool>   gForceCompositeFailure(false);

static CSlrMutex *CompositeMutex()
{
	static CSlrMutex *m = new CSlrMutex("gainMapComposite");   // never destroyed: CSlrImages die at exit too
	return m;
}

namespace
{
	struct SCompositeLock
	{
		SCompositeLock() { CompositeMutex()->Lock(); }
		~SCompositeLock() { CompositeMutex()->Unlock(); }
	};

	enum ECompositeResult { COMPOSITE_OK, COMPOSITE_FAILED, COMPOSITE_LRU_FULL };
}

static size_t CompositeBytesOf(const CRenderTarget *t)
{
	return t ? (size_t)t->GetWidth() * (size_t)t->GetHeight() * 8 : 0;   // RGBA16F
}

// Render thread. `image` has a bound map and the shader is usable. On OK, *outTexture
// is the composite's texture, valid for this frame's draw lists.
static ECompositeResult CompositeFor(CSlrImage *image, int slot, void **outTexture)
{
	*outTexture = NULL;
	SMTGainMapUniforms u;
	MT_GainMapUniformsFrom(image->gainMapApply, image, image->gainMap, &u);
	// flags.w is the draw's magnification mode (image->linearScaling); the composite
	// pass overrides it (2), so it is no part of what the composite holds: masked out
	// of the key, or a SetLinearScaling toggle would rebuild for nothing.
	u.flags[3] = 0.f;
	void *baseTex = image->TexturePtr(), *mapTex = image->gainMap->TexturePtr();
	const u64 frame = VID_GetCurrentFrameNumber();

	SCompositeLock lock;
	SGainMapComposite *e = NULL;
	for (SGainMapComposite &c : gComposites)
		if (c.base == image) { e = &c; break; }

	if (e != NULL && e->baseTexture == baseTex && e->mapTexture == mapTex
	    && memcmp(&e->u, &u, sizeof(u)) == 0 && e->target != NULL)
	{
		e->lastDrawnFrame = frame;
		e->lastDrawnSeq = ++gCompositeSeq;
		*outTexture = e->target->GetTexture();
		return COMPOSITE_OK;
	}

	if (e == NULL)
	{
		// A NEW composite first TAKES OVER an entry not drawn in this frame -- the
		// one drawn least recently -- reusing its target (CRenderTarget::Create is a
		// no-op at the same size), and only grows the cache when every entry was
		// drawn this frame (the compare 2-up: the main pane is drawn before the
		// pinned one, so both are held). In single view the previous photo's
		// composite is therefore replaced at the very frame the next photo is first
		// drawn: never two at once, not even for the frame tick's 2-frame window,
		// so the app's RAM guard never dips by a second composite on an arrow press
		// (review round 1). An entry drawn THIS frame is never taken over: its
		// texture is already in this frame's draw lists. Writing a target a
		// previous frame sampled is safe: the pass is committed on the same queue
		// after those frames, and Metal orders a tracked texture's write after
		// earlier command buffers' reads.
		// SLOTS (review round 2): a takeover stays within the pane that draws --
		// the main pane (slot 0) never takes the compare 2-up's PINNED composite
		// (slot 1). The main pane draws first, so a pinned entry is still "not drawn
		// this frame" when a new main-pane photo arrives; after the main pane drew
		// no composite for 2+ frames (gain map -> SDR/HEIC/video -> gain map) the
		// pinned entry would otherwise be the only candidate, and be rebuilt.
		// Order: an entry of this slot not drawn this frame, else grow, else (cache
		// full) any entry not drawn this frame, else the per-frame fallback.
		auto pick = [&](bool sameSlotOnly) {
			SGainMapComposite *best = NULL;
			for (SGainMapComposite &c : gComposites)
				if (c.lastDrawnFrame != frame && (!sameSlotOnly || c.slot == slot)
				    && (best == NULL || c.lastDrawnSeq < best->lastDrawnSeq))
					best = &c;
			return best;
		};
		e = pick(true);
		if (e == NULL && (int)gComposites.size() < kMaxComposites)
		{
			gComposites.push_back(SGainMapComposite());
			e = &gComposites.back();
		}
		if (e == NULL)
			e = pick(false);
		if (e == NULL)
			return COMPOSITE_LRU_FULL;   // every slot drawn this frame: the caller draws per frame
		CRenderTarget *keep = e->target;
		*e = SGainMapComposite();
		e->target = keep;
		e->base = image;
		e->slot = slot;
	}

	// (Re)build into e.
	e->map = image->gainMap;
	e->baseTexture = baseTex;
	e->mapTexture = mapTex;
	e->u = u;
	CRenderBackend *be = VID_GetRenderBackend();
	if (e->target == NULL && be != NULL && !gForceCompositeFailure.load(std::memory_order_relaxed))
		e->target = be->CreateRenderTarget();
	const size_t before = CompositeBytesOf(e->target);
	const bool ok = e->target != NULL && !gForceCompositeFailure.load(std::memory_order_relaxed)
	             && gGainMapShader->RenderComposite(image, e->target);
	const size_t after = CompositeBytesOf(e->target);
	if (after >= before) gCompositeLiveBytes.fetch_add(after - before, std::memory_order_relaxed);
	else                 gCompositeLiveBytes.fetch_sub(before - after, std::memory_order_relaxed);
	if (!ok)
	{
		// A draw failure (Decision R-4): the entry goes, its target to the graveyard.
		if (e->target != NULL)
		{
			gCompositeGraveyard.push_back(e->target);
			gCompositeGraveyardCount.store((int)gCompositeGraveyard.size(), std::memory_order_relaxed);
		}
		gComposites.erase(gComposites.begin() + (e - gComposites.data()));
		gCompositeCount.store((int)gComposites.size(), std::memory_order_relaxed);
		LOGError("gain-map composite of %dx%d could not be allocated or rendered -- the draw is suppressed",
		         (int)image->width, (int)image->height);
		return COMPOSITE_FAILED;
	}
	e->bytes = after;
	e->lastDrawnFrame = frame;
	e->lastDrawnSeq = ++gCompositeSeq;
	gCompositeCount.store((int)gComposites.size(), std::memory_order_relaxed);
	gCompositeBuilds.fetch_add(1, std::memory_order_relaxed);
	*outTexture = e->target->GetTexture();
	return COMPOSITE_OK;
}

VID_ScopedGainMapDraw::VID_ScopedGainMapDraw(CSlrImage *image, ImDrawList *dl, EGainMapDrawPath path,
                                             int compositeSlot)
{
	drawList = dl ? dl : ImGui::GetWindowDrawList();
	this->image = image;
	if (image == NULL || image->gainMap == NULL)
		return;

	// Not uploaded yet (still queued, or the pair split by a frame-budget stop: the
	// map bound, the base not): suppressed for this draw, NOT flagged and NOT
	// counted -- nothing failed, the base simply has no texture to draw yet, and the
	// next frame draws it normally. Flagging here made a freshly delivered photo
	// look undrawable (blank pane, float re-decode, folder denial, latch strike).
	if (!image->isBound)
	{
		suppressed = true;
		return;
	}

	// isBound alone is not enough: BindImage sets it even when CreateTexture
	// produced no handle (CSlrImage::BindImage).
	const bool drawable = !gForceFailure.load(std::memory_order_relaxed)
	    && image->gainMap->isBound && image->gainMap->TexturePtr() != NULL
	    && image->TexturePtr() != NULL && VID_GainMapShaderUsable();

	if (drawable && path == GAINMAP_DRAW_COMPOSITE)
	{
		// A flagged image is not retried: its composite (a large allocation) already
		// failed, or its map could not be drawn, and the app is re-decoding it.
		ECompositeResult r = COMPOSITE_FAILED;
		if (!image->gainMapDrawFailed.load(std::memory_order_acquire))
			r = CompositeFor(image, compositeSlot, &compositeTexture);
		if (r == COMPOSITE_OK)
		{
			// ImGui's own pipeline draws it, through the draw's own sampler: no callback.
			active = true;
			gDrawCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (r == COMPOSITE_FAILED)
		{
			suppressed = true;
			image->gainMapDrawFailed.store(true, std::memory_order_release);
			gSuppressedCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		// COMPOSITE_LRU_FULL: a third main-pane photo in one frame -- the per-frame
		// shader below gives the same values (Task 3's parity) for this draw.
		gCompositeFallbacks.fetch_add(1, std::memory_order_relaxed);
	}

	bool queued = false;
	if (drawable)
	{
		SMTGainMapUniforms u;
		MT_GainMapUniformsFrom(image->gainMapApply, image, image->gainMap, &u);
		// False = the shader cannot draw on the surface as it is now (its pipeline
		// for the current format failed to build): suppressed below, like any
		// undrawable map, and not counted as a shader draw.
		queued = gGainMapShader->AddDrawCallback(drawList, image->gainMap->TexturePtr(), u);
	}
	if (queued)
	{
		active = true;
		callbackQueued = true;
		gDrawCount.fetch_add(1, std::memory_order_relaxed);
	}
	else
	{
		// Decision R-4: a map that cannot be drawn suppresses the draw -- the raw base
		// is unmanaged colour and is never shown. The app's browser reads the flag
		// and re-decodes the photo on the float arm.
		suppressed = true;
		image->gainMapDrawFailed.store(true, std::memory_order_release);
		gSuppressedCount.fetch_add(1, std::memory_order_relaxed);
	}
}

VID_ScopedGainMapDraw::~VID_ScopedGainMapDraw()
{
	// Back to ImGui's own pipeline, projection, sampler and vertex buffer. A
	// composite draw switched nothing.
	if (callbackQueued)
		drawList->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}

void *VID_ScopedGainMapDraw::Texture() const
{
	if (compositeTexture != NULL)
		return compositeTexture;
	return image != NULL ? image->TexturePtr() : NULL;
}

int VID_GainMapDrawCountForTest()
{
	return gDrawCount.load(std::memory_order_relaxed);
}

int VID_GainMapSuppressedCountForTest()
{
	return gSuppressedCount.load(std::memory_order_relaxed);
}

void VID_GainMapTestForceDrawFailure(bool on)
{
	gForceFailure.store(on, std::memory_order_relaxed);
}

bool VID_GainMapRenderToTargetForTest(CSlrImage *base, CRenderTarget *target, const float rgba[4])
{
	if (base == NULL || target == NULL || base->gainMap == NULL || !VID_GainMapShaderUsable())
		return false;
	return gGainMapShader->RenderToTargetForTest(base, target, rgba);
}

int VID_GainMapCompositeMaxEntries()
{
	return kMaxComposites;
}

size_t VID_GainMapCompositeLiveBytes()
{
	return gCompositeLiveBytes.load(std::memory_order_relaxed);
}

int VID_GainMapCompositeCount()
{
	return gCompositeCount.load(std::memory_order_relaxed);
}

int VID_GainMapCompositeBuildsForTest()
{
	return gCompositeBuilds.load(std::memory_order_relaxed);
}

int VID_GainMapCompositeFallbacksForTest()
{
	return gCompositeFallbacks.load(std::memory_order_relaxed);
}

bool VID_GainMapCompositeHasForTest(const CSlrImage *base)
{
	SCompositeLock lock;
	for (const SGainMapComposite &c : gComposites)
		if (c.base == base)
			return true;
	return false;
}

void VID_GainMapTestForceCompositeFailure(bool on)
{
	gForceCompositeFailure.store(on, std::memory_order_relaxed);
}

void VID_GainMapCompositeForget(const CSlrImage *image)
{
	// Every CSlrImage's Deallocate and destructor come here: one relaxed load while
	// the LRU is empty (an SDR session, every app that draws no gain map).
	if (image == NULL || gCompositeCount.load(std::memory_order_relaxed) == 0)
		return;
	SCompositeLock lock;
	for (size_t i = 0; i < gComposites.size();)
	{
		if (gComposites[i].base == image || gComposites[i].map == image)
		{
			if (gComposites[i].target != NULL)
				gCompositeGraveyard.push_back(gComposites[i].target);
			gComposites.erase(gComposites.begin() + i);
			gCompositeGraveyardCount.store((int)gCompositeGraveyard.size(), std::memory_order_relaxed);
		}
		else
			i++;
	}
	gCompositeCount.store((int)gComposites.size(), std::memory_order_relaxed);
}

void VID_GainMapCompositeFrameTick()
{
	// Every frame of every app, SDR included: nothing held and nothing parked costs
	// two relaxed loads and no lock.
	if (gCompositeCount.load(std::memory_order_relaxed) == 0
	    && gCompositeGraveyardCount.load(std::memory_order_relaxed) == 0)
		return;
	std::vector<CRenderTarget *> dead;
	{
		SCompositeLock lock;
		// Held only while drawn: an entry not drawn in the last kHoldFrames frames
		// goes. This runs before the frame number advances, so `now` is the frame
		// just rendered; an entry drawn in it or the one before stays.
		const u64 now = VID_GetCurrentFrameNumber();
		for (size_t i = 0; i < gComposites.size();)
		{
			if (now - gComposites[i].lastDrawnFrame >= kHoldFrames)
			{
				if (gComposites[i].target != NULL)
					gCompositeGraveyard.push_back(gComposites[i].target);
				gComposites.erase(gComposites.begin() + i);
			}
			else
				i++;
		}
		gCompositeCount.store((int)gComposites.size(), std::memory_order_relaxed);
		dead.swap(gCompositeGraveyard);
		gCompositeGraveyardCount.store(0, std::memory_order_relaxed);
	}
	for (CRenderTarget *t : dead)
	{
		gCompositeLiveBytes.fetch_sub(CompositeBytesOf(t), std::memory_order_relaxed);
		t->Destroy();   // Metal parks the texture past the frames in flight
		delete t;
	}
}

bool VID_GainMapCompositeRenderToTargetForTest(CSlrImage *base, CRenderTarget *target, const float rgba[4])
{
	if (base == NULL || target == NULL || base->gainMap == NULL || !VID_GainMapShaderUsable())
		return false;
	if (!base->gainMap->isBound || base->gainMap->TexturePtr() == NULL || base->TexturePtr() == NULL)
		return false;
	void *tex = NULL;
	if (CompositeFor(base, GAINMAP_COMPOSITE_SLOT_MAIN, &tex) != COMPOSITE_OK || tex == NULL)
		return false;
	const float uv[4] = { base->defaultTexStartX, base->defaultTexStartY, base->defaultTexEndX, base->defaultTexEndY };
	return gGainMapShader->DrawTextureToTargetForTest(tex, uv, base->linearScaling, target, rgba);
}
