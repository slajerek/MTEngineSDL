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

## Overrides

`--render-backend=<selection>` beats the config for one run, and is checked
before it. That ordering is deliberate: headless test runs may wipe the settings
directory, so a config-only setting would be unreachable from any automated
test, and an operator debugging a rendering problem should not have to edit a
file the app rewrites underneath them. An unavailable or unknown name is
rejected with a log line and the config is consulted as usual.
