# Render backends: selection, defaults and fallback

Three backends exist: **OpenGL 4**, **Metal** (macOS) and **Direct3D 11**
(Windows, in a build that defines `MT_RENDER_BACKEND_D3D11`). A host picks one
through `VID_Main.h`; the choice takes effect at the **next launch**, because
the surface's colour space and every on-screen pipeline state are built when the
layer is created.

## The default is the best backend this platform HAS

`VID_GetDefaultRenderBackend()`:

| Platform | Default | Falls back to |
|---|---|---|
| macOS | `metal` | `opengl` if `MTLCreateSystemDefaultDevice()` returns nil |
| Windows with `MT_RENDER_BACKEND_D3D11` | `d3d11` | `opengl` if no hardware D3D11 device |
| everything else | `opengl` | — |

**This changed on 2026-08-24.** OpenGL used to be the default everywhere, which
meant a new user on a Mac or a modern Windows box silently ran the slowest path
and **could not have HDR at all** — an extended-range surface needs Metal or
D3D11. A default that decides a feature the user never knew existed is the wrong
default.

**It is a default, not a migration.** It applies only where the config has no
`renderBackend` key. An install that already persisted `opengl` keeps OpenGL —
including one that persisted it merely by visiting a settings menu, since
writing the current value materialises the key. That is correct behaviour for a
default, and it is the answer to "why am I still on OpenGL after the update".

## The fallback is real, not nominal

Both preferred names go through `VID_IsRenderBackendAvailable()`, and on both
platforms that **probes an actual device**:

- `CRenderBackendD3D11::IsAvailable()` — `D3D11CreateDevice` against
  `D3D_DRIVER_TYPE_HARDWARE`, plus a check that the embedded resolve shader is
  not a placeholder. WARP is deliberately *not* accepted: a software rasteriser
  would be slower than OpenGL for a compositor like this one.
- `CRenderBackendMetal::IsAvailable()` — `MTLCreateSystemDefaultDevice() != nil`.

The Metal probe is new, and it exists **because** Metal became the default.
While Metal was opt-in, "Metal always exists on a Mac" was a fair assumption and
`CreateSDLWindow`'s `SYS_FatalExit` was an acceptable failure mode: a user who
asked for Metal and got a fatal exit had at least asked. As the default it is
not acceptable — a Mac that cannot create a device (a remoted session, a GPU in
a bad state) would fail to start the app at all, with hand-editing
`settings.hjson` as the only way out.

Each probe runs **once per process**: these are reachable from per-frame menu
code, and creating a device to answer a menu at 60 Hz is not acceptable. Once is
also correct — a device that cannot be created at startup will not appear later,
and a backend switch needs a restart anyway.

The probe lives in `VID_IsRenderBackendAvailable()` and **nowhere else**. That
is the single query the factory consults, so it is the one place a fallback can
actually happen, and every UI that asks the same question gets the same answer.
Probing again in the factory would be a second answer to the same question,
which is how the two would drift.

## Two vocabularies, and a third

| | Example | Where |
|---|---|---|
| **selection** name | `opengl`, `metal`, `d3d11` | config, `--render-backend=`, nearly every API here |
| **display** name | `OpenGL`, `Metal`, `Direct3D 11` | `VID_GetRenderBackendDisplayName()` — for menus |
| **running** name | `OpenGL4`, `Metal`, `D3D11` | `VID_GetCurrentRenderBackendName()` only |

Mixing them is how a menu came to show a title of `OpenGL4` over a ticked
`OpenGL`. `VID_GetCurrentRenderBackendSelection()` exists solely to map the
running name back to the selection vocabulary so a menu can title itself
consistently.

## What a settings UI must do

Four rules, and each exists because a shipped UI got it wrong:

1. **Enumerate with `VID_GetAvailableRenderBackends()`.** Writing the list out is
   how one menu came to offer Metal on Windows — the guard was true and the
   items were literals.
2. **Tick `VID_GetEffectiveRenderBackendSelection()`**, never a raw compare with
   the persisted string. Two UIs did the latter and left *every* item unticked
   when a `settings.hjson` from another machine named a backend the build lacks.
3. **Read `VID_GetPersistedRenderBackend()`**, not
   `VID_GetPreferredRenderBackend()`. The latter checks the command line first,
   so under `--render-backend=` the UI would show the flag rather than the saved
   choice and every click would appear to do nothing. Pair it with
   `VID_IsRenderBackendOverriddenByCommandLine()` to say a flag is winning.
4. **Copy the result immediately.** `VID_GetPreferredRenderBackend()`,
   `VID_GetPersistedRenderBackend()` and
   `VID_GetEffectiveRenderBackendSelection()` share **one per-thread buffer**;
   calling a second silently overwrites the first.

`VID_GetDefaultRenderBackend()` has none of problem 4 — it returns a literal.

## HDR rides on this choice

`VID_IsRenderBackendHdrCapable(name)` is true for `metal` on macOS and `d3d11`
on Windows, and it is what a UI should use to **disable** an HDR control rather
than hide it.

Ask it about the **persisted** backend, never the running one: a switch needs a
restart, so a live query would grey the HDR control out for somebody who has
just chosen a capable backend, and leave it enabled after they chose an
incapable one.

A worked example of both the picker and the HDR gating is
`MTEngineSDLDummyApp`, in its Settings menu and its HDR test bench.

## Custom fragment shaders

`CRenderBackend::CreateCustomFragmentShader(name)` returns a
`CRenderShaderCustomFragment` whose GLSL/MSL/HLSL source is supplied at runtime
and can be replaced while the app runs. Implemented on **all three** backends,
for the reason `CreateFlatColorShader` states beside it.

Four things a caller has to know, each of which cost a round to find:

1. **`SetFragmentSource()` is render-thread only**, like every other GPU call
   here. From an imgui_test_engine `TestFunc` -- which runs on its own
   coroutine thread -- it does not fail, it crashes.
2. **The host writes only `mainImage()`.** Each backend prepends a preamble and
   appends the entry point. `GetPreambleLineCount()` is what lets a host rebase
   the compiler's line numbers onto the text its user typed; the OpenGL count
   includes the `#version` line `CRenderShaderOpenGL4::CompileShaders()`
   prepends.
3. **`GetCompileErrorLog()` returns the diagnostics; it does not log them.**
   `LOGError` is a no-op under `GLOBAL_DEBUG_OFF`, which is set on Linux, so a
   log-only design would show an empty error panel precisely where a headless
   CI run is the only way anyone sees the failure.
4. **A failed rebuild keeps the previous program bound**, so a host's preview
   does not blank on a typo.

The uniform block is 240 bytes and its MSL struct must say `packed_float3` --
a plain MSL `float3` is 16 bytes and silently shifts every field after it. The
MSL preamble repeats the C++ header's `static_assert` on that size, so the two
layouts cannot drift apart without one of them refusing to build.

**Four texture channels**, `iChannel0..3`, set through
`SetChannelTexture(n, nativeHandle)` and
`SetChannelSampler(n, filter, wrap)` -- both **render-thread-only pure
stores**; the binding happens where an encoder or device context exists. They
bind at slots **1..4 on every backend**, never 0: ImGui claims slot 0 for its
own draw command, after the user callback that installs the shader. Nothing
restores the higher slots, so `ResetState()` unbinds them.

The `texChannelN` macros in each preamble call `mtChannelUV()`, which does
three things in a fixed order: **flip, wrap, scale**. The flip is
`iChannelUvTransform.z`, on by default -- ShaderToy's `fragCoord` is
bottom-left while a texture's `v = 0` is its top row, so an unflipped channel
samples upside down, and shadertoy.com defaults the same toggle on. The wrap
comes from `iChannelWrap` and is done in the shader because `CSlrImage` pads
textures to a power of two and hardware repeat would tile the padding; it must
run before the scale, in the image's own 0..1 space. The scale is
`iChannelUvTransform.xy`, carrying `defaultTexEndX/Y`. The scale half becomes
a no-op when `NextPow2` goes.

D3D11 compiles the host's HLSL with `D3DCompile` at `ps_4_0`. That is not the
second production path `tools/embed-hlsl-shaders.ps1` forbids: its rule guards
a shader which also has committed bytecode, and runtime text has none.

A worked example is `MTEngineSDLDummyApp`, in `Examples > Shader Toy`.

## The gain-map shader (option C, `VID_GainMapDraw.h`)

A `CSlrImage` may carry a companion **gain map** (`CSlrImage::gainMap`, owned:
the base's `Deallocate()` deallocates it, `~CSlrImage` deletes it), with its
apply parameters (`gainMapApply`, `SMTGainMapApply` in `MT_UltraHdr.h`) and its
linear peak (`gainMapPeak`, which the resident funnel reports as
`contentMaxComponent` although the texture is RGBA8). Both textures are RGBA8; the
shader applies the map **per pixel at draw time**, so an Ultra HDR photo is
resident at 4 B/px + a quarter-size map instead of an 8 B/px float texture.
The CPU twin `MT_GainMapEvaluate` is the shader's oracle (a host's GPU parity
test compares the two at 1:1, minified and through an EXIF turn).

- **Factory:** `CRenderBackend::CreateGainMapShader()` -> `CGainMapShader *`,
  compiled synchronously. Metal: `CRenderShaderGainMapMetal`
  (`platform/MacOS/shaders/MTGainMap.metal`, embedded like the other shaders).
  OpenGL returns NULL (it never takes this path); D3D11 returns NULL until its
  port. `VID_GainMapShaderUsable()` (main/render thread) creates it lazily,
  retries a NULL (no backend or no layer yet, or a backend without one) and
  **latches a compile failure** for the session.
- **Uniforms ride with each draw.** `CGainMapShader::AddDrawCallback(dl, map, u)`
  queues an ImGui callback whose user data ImGui COPIES into the draw list: the
  pipeline switch, the map at fragment texture 1 and `SMTGainMapUniforms`
  (`MT_GainMapUniforms.h`, 12 float4 = 192 bytes, the MSL struct byte for byte) at
  fragment buffer 0. Slot 0's texture and sampler are the draw's own (ImGui's and
  `ScopedImageSampler`'s), so a cache image keeps magnify-nearest. Nothing is read
  from the shader object at render time but its pipeline, which is rebuilt when
  the surface's colour format changes (SDR <-> HDR). The uniforms carry the
  position rule as an affine map from the base's texel rect to the map's, read
  from the apply (`mapScale`/`mapOffset`), never recomputed from sizes.
- **One applied value per base texel, as the float arm's texture holds** (Decision
  R-1). Magnified (nearest): the draw's own sampler picks the texel and the map is
  applied at THAT TEXEL'S CENTRE, never at the fragment's uv (which would vary the
  gain inside one zoomed-in pixel). Minified (texel footprint > 1 pixel): the
  fragment reads the four base texels the hardware's bilinear footprint covers,
  applies the map to each, and blends. Filtering base and map first would not
  commute with the nonlinear apply. The MAP's own bilinear blend is done in float
  in the shader (four reads), not by the sampler: the hardware's ~8-bit filter
  weights, amplified by the map gamma's `pow` and the sRGB toe, exceeded the
  parity bound on gamma-2.2 and 3-channel maps. The magnified branch READS its base
  texel at `floor(p)` (not through the sampler, whose sub-texel rounding could pair
  a texel with its neighbour's gain), and every clamp stops at the image's own texel
  rect (`baseRect` / `mapRect`), so a power-of-two-padded `LoadImage` texture works
  too. A surface pipeline that fails to build is latched for that colour format and
  retried only when the format changes; draws meanwhile are suppressed.
- **THE RULE:** every draw of an image that may carry a map declares
  `VID_ScopedGainMapDraw scope(image, drawList[, path])` after its
  `ScopedImageSampler` and before its `AddImage`/`AddImageQuad`, **skips the draw
  when `scope.Suppressed()`** (Decision R-4: a map that cannot be drawn -- no usable
  shader, an unbound map, a composite that cannot be allocated -- must not show its
  raw base, which is unmanaged colour; `image->gainMapDrawFailed` is set for the
  owner to re-decode), and **draws `scope.Texture()`**, never
  `image->TexturePtr()` (the composite below, or the image's own texture). The
  scope is also suppressed when the shader cannot build its pipeline for the
  surface's current format (`AddDrawCallback` returns false; checked at record
  time). Every image blit in `VID_Blits.cpp` does this; a per-frame scope resets
  ImGui's render state (`ImDrawCallback_ResetRenderState`) when it ends. An image
  without a map costs one pointer test. **A base not uploaded yet** (`isBound`
  false: still queued, or a pair the binder's frame budget split) is suppressed
  for that draw but NOT flagged and NOT counted -- nothing failed; a bound base
  with no texture handle is a texture-creation failure and is flagged. **The flag
  is asymmetric:** `gainMapDrawFailed` blocks only the composite path; a
  `GAINMAP_DRAW_PER_FRAME` draw (grid / filmstrip cells) does not read it and
  keeps drawing the image applied. **Recorded R-4 exception:** on Metal, a surface
  format change between recording a per-frame draw and rendering it, with a
  failed pipeline rebuild, draws the raw base for that one frame
  (`CRenderShaderGainMapMetal::DrawCallback`, logged once); the next frame's
  record-time check suppresses it.
- **Two paths (Task 3c, 2026-10-08).** The per-frame shader above costs ~1.7 ms
  per Mpx of minified fragments with precise maths (22.5 ms for one 12 Mpx photo
  fit to a 3456x2234 window), so the scope takes one of two paths:
  - `GAINMAP_DRAW_COMPOSITE` (the default; every engine blit, so the main pane):
    the FIRST draw of the image renders base + map ONCE, at the base TEXTURE's
    full size, into a cached RGBA16F target (`CGainMapShader::RenderComposite`:
    the offscreen pipeline, uniform `flags.w = 2` = one apply per base texel at its
    centre, unconditionally the one-texel branch, blending off). That target holds
    what a float-arm texture of the same photo holds -- RGBA16F, the apply's
    `encodeSrgb` and matrix (chosen at decode time from the surface, exactly as the
    float arm's CPU values are), base alpha -- in the base texture's own texel
    layout, so the caller's UVs are unchanged. Every draw then hands THAT texture
    to ImGui's own pipeline with the draw's own sampler: the path a float-arm image
    takes, so every scale filters applied values exactly as the float arm does
    (Decision R-1 by construction) at the float arm's per-frame cost. No callback,
    no state reset. The pass runs on the render thread during recording, on its
    own command buffer, committed on the backend's queue before the frame's (the
    `MT_ShaderProbe` pattern), so the frame that draws it sees it done.
  - `GAINMAP_DRAW_PER_FRAME` (grid and filmstrip cells -- many small draws): the
    callback path above, every frame. Decision R-1's per-frame applied-value
    filtering is now this path only.
- **The composite cache** (`VID_GainMapDraw.cpp`) -- **held only while drawn**
  (controller ruling, Task 3c review round 1): `VID_GainMapCompositeFrameTick()`
  (render thread, VID_Main, right after `VID_BindImages`) releases every entry
  NOT DRAWN IN THE LAST 2 FRAMES. In single view only the photo on screen holds a
  composite; the compare 2-up holds two because it draws both panes every frame;
  returning to a photo rebuilds it (one ~1.75 ms pass, inside the maintainer's
  once-per-photo ruling). Why: a composite is real memory the app counts against
  its RAM guard, and keeping the previous photo's would cut a small cache's
  ceiling (100 MB setting: to ~5 MB) or cost ~800 MB at 50 Mpx. The cap is
  `VID_GainMapCompositeMaxEntries()` = 2. Entries are keyed by the base
  `CSlrImage` and validated on every use against the base's and the map's texture
  pointers and the draw's uniforms (the magnification flag masked out -- the
  composite overrides it), so a stale entry is rebuilt, never drawn. A NEW
  composite first **takes over** the entry drawn least recently among those OF ITS
  OWN SLOT not drawn in the current frame, reusing its target
  (`CRenderTarget::Create` is a no-op at the same size); with none it grows the
  cache, and only a full cache takes another slot's undrawn entry. The slot is the
  pane (`VID_ScopedGainMapDraw(..., compositeSlot)`: `GAINMAP_COMPOSITE_SLOT_MAIN`
  by default, `GAINMAP_COMPOSITE_SLOT_PINNED` for the compare 2-up's pinned pane;
  review round 2): the main pane draws first, so without slots a new main-pane
  photo arriving after a stretch without a main-pane composite (gain map -> SDR ->
  gain map) would take the pinned entry and rebuild it. So in single view the previous photo's composite is replaced at
  the very frame the next photo is first drawn and two never coexist, not even
  for the tick's 2-frame window (the app's RAM guard never dips by a second
  composite on an arrow press); the compare 2-up holds both because its main pane
  is drawn before the pinned one. An entry drawn in the current frame is **never**
  taken over (its texture is already in this frame's draw lists); when both were
  drawn this frame a third main-pane draw takes the per-frame shader
  (`VID_GainMapCompositeFallbacksForTest`).
  `CSlrImage::Deallocate`, `~CSlrImage` and `ReBindImage` (which may rewrite a
  texture in place, invisible to the validation; `ReBindImageData` comes through
  it) call `VID_GainMapCompositeForget(image)` (any thread; one relaxed load while
  nothing is held), which drops every entry whose base or map is that image; the
  target is destroyed by the frame tick on the render thread, whose
  `CMetalRenderTarget::Destroy` parks the texture past the frames in flight. The
  tick costs two relaxed loads and no lock while nothing is held or parked. The
  containers are never-destroyed heap objects (Forget can run during static
  destruction). **Live bytes** count every composite target alive, held or awaiting
  the tick; a released texture -- and the old texture of a reused target that a
  photo of a different size re-creates -- is then parked for its 3 frames in
  flight OUTSIDE them (up to one composite's memory, for 3 frames). A
  surface-format switch does not invalidate a composite (its values are fixed by
  the apply, as a float-arm texture's are; the app re-decodes for a new surface);
  the Metal backend has no device-loss path. Memory: 8 B/px of the base, 93 MiB
  for a 12 Mpx photo; one in single view, two in compare. `VID_GainMapCompositeLiveBytes()` /
  `VID_GainMapCompositeCount()` (any thread) are what the app counts against its
  RAM guard. A composite that cannot be allocated or rendered suppresses the draw
  and flags the image (Decision R-4); a flagged image is not retried. Measured
  (M2 Max, headless Metal, 3456x2234 attachment, Release): the build of a
  4032x3024 composite 1.75-1.77 ms GPU once; every frame after it 0.90-0.92 ms
  p50 at fit, 1:1 and 2x, the float arm's 0.87-1.07 ms; a 12-cell grid on the
  per-frame shader 0.94-0.98 ms (float 0.90-0.92).
- **Binding the pair:** `VID_PostImageBindingPair(map, base, mode)` queues both
  under one lock or neither, the map first; `VID_PostImageDealloc(base)` while the
  base is still queued cancels the map's queued bind too (or, when a frame-budget
  stop already bound the map, queues the map's DEALLOC). After `Deallocate()` a
  base keeps its (deallocated) map object and apply: it is for deletion only, never
  re-bound.
- Tests: `VID_GainMapDrawCountForTest` (a draw with the map applied, shader OR
  composite) / `VID_GainMapSuppressedCountForTest`, `VID_GainMapTestForceDrawFailure`
  (both paths), `VID_GainMapCompositeBuildsForTest` (monotonic, the once-per-photo
  assertion), `VID_GainMapCompositeHasForTest`, `VID_GainMapTestForceCompositeFailure`
  (every composite allocation fails), and the render-thread probe
  `MT_ShaderProbeRequestGainMapParity(base, w, h, rgba, throughComposite)`: a draw
  into an RGBA16F target through the shader's fragment function, or -- with
  `throughComposite` -- the image's composite (through the production LRU) drawn
  through a verbatim COPY of ImGui's fragment function (`mtFragmentPlain` in
  `MTGainMap.metal`; ImGui's own pipeline cannot be driven offscreen), blending
  off, with the production sampler (`CGainMapShader::DrawTextureToTargetForTest`),
  read back as floats. A future backend port (D3D11, Task 9) implements `RenderComposite` and
  `DrawTextureToTargetForTest` beside `AddDrawCallback`.

`tools/embed-metal-shaders.sh` reads per-file compiler flags from a
`// embed-metal-shaders-flags:` line in the `.metal` file (inside the hashed
source). `MTGainMap.metal` used it for `-fno-fast-math` until 2026-10-08; it is
now compiled with the Metal compiler's default fast math, measured to leave every
CPU-twin parity case unchanged (worst 0.102 of the bound either way) while the
precise transcendentals cost a 12-cell grid 3.5 ms per frame against 0.75 ms.

## Overrides

`--render-backend=<selection>` beats the config for one run, and is checked
before it. That ordering is deliberate: headless test runs may wipe the settings
directory, so a config-only setting would be unreachable from any automated
test, and an operator debugging a rendering problem should not have to edit a
file the app rewrites underneath them. An unavailable or unknown name is
rejected with a log line and the config is consulted as usual.

## Image binding modes (`VID_ImageBinding.h`)

A `CSlrImage` decoded off the render thread is uploaded by posting it with
`VID_PostImageBinding(image, dest, mode)`; `VID_BindImages()` (render thread)
calls `BindImage()` -> the backend's `CreateTexture()` and then applies the mode
to the CPU copy, `loadImageData`:

| Mode | After `BindImage()` |
|---|---|
| `BINDING_MODE_BIND` (1) | frees `loadImageData` **unconditionally** -- even when `CreateTexture` produced no handle -- and on a snapshot mismatch deletes the snapshot and nulls `image` without writing `dest` |
| `BINDING_MODE_DONT_FREE_IMAGEDATA` (5) | keeps `loadImageData` -- for an owner that re-uploads from it (`ReBindImage` / `SetImageData`) or reads it back |
| `BINDING_MODE_BIND_FREE_IMAGEDATA` (6) | `FreeLoadImage()` **only when `TexturePtr()` is non-NULL**, and always writes `dest` |

Mode 6 (2026-10-07) is for an image whose
texture IS the image: keeping the CPU copy beside it doubles its memory (an
RGBA16F texture is 8 B/px and so is its half-float copy). A failed texture keeps
its data, so its owner can still decide. After a mode-6 bind `loadImageData` is
NULL: such an image cannot `ReBindImage()`, and nothing may read its pixels. What
survives the free is what `LoadImageForRebinding` copied out of the data --
`width`/`height`, `residentFormat` and `contentMaxComponent` (the EDR peak).
`FreeLoadImage()` deletes only when `shouldDeallocLoadImageData` is set.

A photo browser's cache (decode pool and develop lane) binds with mode 6;
that app's display histogram is binned on the decode worker before the post,
since nothing can read the pixels afterwards. Measured on its edited-browse
benchmark (2026-10-07, macOS, non-idle): peak RSS 28.5 -> 25.1 GB (-11.8 %).
