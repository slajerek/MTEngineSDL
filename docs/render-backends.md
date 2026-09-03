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

## Overrides

`--render-backend=<selection>` beats the config for one run, and is checked
before it. That ordering is deliberate: headless test runs may wipe the settings
directory, so a config-only setting would be unreachable from any automated
test, and an operator debugging a rendering problem should not have to edit a
file the app rewrites underneath them. An unavailable or unknown name is
rejected with a log line and the config is consulted as usual.
