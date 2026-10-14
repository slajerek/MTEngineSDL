#ifndef _VID_GainMapDraw_h_
#define _VID_GainMapDraw_h_

// Option C (gain-map resident HDR): drawing a CSlrImage that carries a companion
// gain map (CSlrImage::gainMap). MAIN / RENDER THREAD ONLY, like every other user
// of the backend's shader factories.
//
// THE RULE: every draw of an image that may carry a gain map goes through
// VID_ScopedGainMapDraw, declared AFTER the draw's ScopedImageSampler and BEFORE
// its AddImage / AddImageQuad; the draw is skipped when Suppressed(), and it draws
// scope.Texture() -- never image->TexturePtr() directly. VID_Blits does this for
// every image blit; a new direct AddImage of a cache image must do the same, or a
// gain-mapped photo shows its raw SDR base (unmanaged colour).
//
// TWO PATHS (option C Task 3c):
//   * GAINMAP_DRAW_COMPOSITE (the default; the main pane, compare, crop, develop
//     fade, the editor's held frame): the FIRST draw of the image renders base +
//     map ONCE, at the base texture's full resolution, into a cached RGBA16F
//     texture -- one apply per texel at its centre, the values a float-arm texture
//     holds -- and every draw (that one included) then draws THAT texture through
//     ImGui's own pipeline and the draw's own sampler, exactly as a float-arm image
//     is drawn. Every scale therefore filters applied values the way the float
//     arm does (Decision R-1 by construction), at the float arm's per-frame cost.
//     An engine-owned LRU holds at most VID_GainMapCompositeMaxEntries() such
//     textures, keyed by the base image; see VID_GainMapCompositeForget.
//   * GAINMAP_DRAW_PER_FRAME (grid and filmstrip cells: many small draws): the
//     shader applies the map at every draw (the filtered branch blends applied
//     texels when minified). No texture is kept.
//
// THE FLAG IS ASYMMETRIC (by design): image->gainMapDrawFailed blocks only the
// COMPOSITE path -- a flagged image is never handed a composite again and its
// composite draws are suppressed. A PER_FRAME draw of the same image does not read
// the flag: grid and filmstrip cells keep drawing it through the per-frame shader
// (map applied, colour-correct) while the main pane, which flagged it, waits for
// the app's float re-decode. Only a per-frame draw that itself cannot be queued is
// suppressed (and flags the image).
//
// NOT UPLOADED YET: an image whose base is not bound (isBound false: still queued,
// or a pair split by the binder's frame budget) is suppressed for that draw but
// NEITHER flagged NOR counted -- nothing failed. A bound base whose texture handle
// is missing IS a texture-creation failure and is flagged.

#include "MT_GainMapUniforms.h"
#include <cstddef>   // NULL, size_t

struct ImDrawList;
class CSlrImage;
class CRenderTarget;

// True when the active backend has a compiled gain-map shader. Created lazily on
// the first call and retried while the backend has none to give (no backend yet,
// no Metal layer yet, OpenGL); a genuine compile failure is latched for the
// session (logged once, never retried per frame).
bool VID_GainMapShaderUsable();

// True once a compile failure has been latched (the probe's COMPILE_FAILED).
bool VID_GainMapShaderCompileFailed();

enum EGainMapDrawPath
{
	GAINMAP_DRAW_COMPOSITE = 0,   // draw the cached one-time composite (main pane)
	GAINMAP_DRAW_PER_FRAME = 1,   // apply the map at every draw (grid / filmstrip cells)
};

// Which pane a composite draw belongs to (review round 2). A new composite takes
// over an entry of ITS OWN slot only (then grows; only a full cache takes another
// slot's undrawn entry), so a new main-pane photo never rebuilds the compare
// 2-up's pinned composite: each pane builds its photo once.
enum
{
	GAINMAP_COMPOSITE_SLOT_MAIN   = 0,   // the main pane and everything drawn in its place (crop, fade, held frame)
	GAINMAP_COMPOSITE_SLOT_PINNED = 1,   // the compare 2-up's pinned pane
};

class VID_ScopedGainMapDraw
{
public:
	explicit VID_ScopedGainMapDraw(CSlrImage *image, ImDrawList *dl = NULL,   // NULL = window draw list
	                               EGainMapDrawPath path = GAINMAP_DRAW_COMPOSITE,
	                               int compositeSlot = GAINMAP_COMPOSITE_SLOT_MAIN);
	~VID_ScopedGainMapDraw();
	// The image is drawn with its map applied (shader callback or composite).
	bool Active() const { return active; }
	// Decision R-4: the image has a map that cannot be drawn, or its base is not
	// uploaded yet -- the caller MUST skip its AddImage/AddImageQuad (the raw base is
	// unmanaged colour, never shown).
	bool Suppressed() const { return suppressed; }
	// THE TEXTURE TO DRAW: the composite when this draw takes one, else the image's
	// own texture (a plain image, or a per-frame shader draw). Same UV layout as the
	// image's own texture, so the caller's UVs are unchanged.
	void *Texture() const;
private:
	VID_ScopedGainMapDraw(const VID_ScopedGainMapDraw &);
	VID_ScopedGainMapDraw &operator=(const VID_ScopedGainMapDraw &);
	ImDrawList *drawList = NULL; CSlrImage *image = NULL; void *compositeTexture = NULL;
	bool active = false; bool suppressed = false; bool callbackQueued = false;
};

int  VID_GainMapDrawCountForTest();          // draws with the map applied, shader or composite (monotonic)
int  VID_GainMapSuppressedCountForTest();    // gain-mapped draws suppressed (monotonic)
void VID_GainMapTestForceDrawFailure(bool on);   // test hook: every map treated as undrawable
// Render thread only (MT_ShaderProbe services it). False when there is no usable
// shader, `base` has no map, or either texture is unbound.
bool VID_GainMapRenderToTargetForTest(CSlrImage *base, CRenderTarget *target, const float rgba[4]);

// --- The one-time composite (Task 3c) ----------------------------------------
//
// LIFETIME -- HELD ONLY WHILE DRAWN (controller ruling, Task 3c review round 1):
// an entry is keyed by its base CSlrImage (and validated against the base's and
// the map's texture pointers and the draw's uniforms, magnification flag masked,
// so a stale entry is rebuilt, never drawn). The frame tick releases every entry
// NOT DRAWN IN THE LAST 2 FRAMES: in single view only the photo on screen holds a
// composite; the compare 2-up holds two because it draws both every frame; going
// back to a photo rebuilds it (one ~1.75 ms pass). A NEW composite first takes
// over (target reused) an entry OF ITS OWN SLOT (main pane / pinned pane) not
// drawn in this frame, so in single view two never coexist even for one frame;
// otherwise it grows the cache (compare: one per pane); only a full cache takes
// another slot's undrawn entry. The cap is 2: a third main-pane image in ONE frame
// never takes over an entry drawn this frame (its texture is in this frame's draw
// lists) -- that draw takes the per-frame shader instead.
// An entry is also dropped when its base or its map is deallocated, destroyed or
// re-bound: CSlrImage::Deallocate, ~CSlrImage and ReBindImage call
// VID_GainMapCompositeForget, which may run on any thread -- the texture itself
// is released on the render thread (VID_GainMapCompositeFrameTick, after
// VID_BindImages, through the backend's deferred release, so an in-flight frame
// never samples a freed texture). The composite is RGBA16F whatever the surface
// is, and its values are fixed by the image's apply (its encodeSrgb was chosen at
// decode time, as a float-arm texture's values are), so a surface-format switch
// does not invalidate it.
//
// LIVE BYTES count every composite target alive (held or awaiting the tick). A
// released target's texture -- and the old texture of a reused target that a
// photo of a DIFFERENT size re-creates -- is then parked by the backend for its 3
// frames in flight (CMetalRenderTarget::Destroy) OUTSIDE these bytes: up to one
// extra composite's memory, for 3 frames.
//
// FAILURE: a composite that cannot be allocated or rendered SUPPRESSES the draw
// and sets image->gainMapDrawFailed (Decision R-4: the app re-decodes the photo
// through the float arm); a flagged image is not retried.
int    VID_GainMapCompositeMaxEntries();          // 2: the compare 2-up's two panes (single view holds 1)
size_t VID_GainMapCompositeLiveBytes();           // bytes of every composite texture alive (any thread)
int    VID_GainMapCompositeCount();               // entries in the LRU (any thread)
int    VID_GainMapCompositeBuildsForTest();       // composites rendered (monotonic)
int    VID_GainMapCompositeFallbacksForTest();    // composite draws that took the per-frame shader (LRU full this frame)
bool   VID_GainMapCompositeHasForTest(const CSlrImage *base);   // pointer compare only (any thread)
void   VID_GainMapTestForceCompositeFailure(bool on);   // test hook: every composite allocation fails
// Any thread. Drops every entry whose base or map is `image`; the texture is
// released by the next VID_GainMapCompositeFrameTick. One relaxed load when the
// LRU is empty (every CSlrImage destruction calls it).
void   VID_GainMapCompositeForget(const CSlrImage *image);
// Render thread, once per frame (VID_Main, after VID_BindImages): releases every
// entry not drawn in the last 2 frames, and the textures of forgotten / failed
// entries. Two relaxed loads and no lock while nothing is held or parked.
void   VID_GainMapCompositeFrameTick();
// Render thread only (MT_ShaderProbe). Gets (or builds) `base`'s composite through
// the production cache and draws it over the whole of `target` through a verbatim
// COPY of ImGui's fragment function (blending off) and the sampler production binds
// for `base` -- what a composite draw on screen goes through. False when there is no composite.
bool   VID_GainMapCompositeRenderToTargetForTest(CSlrImage *base, CRenderTarget *target, const float rgba[4]);

#endif
