# MTEngine local patches to Dear ImGui

Dear ImGui in `src/Engine/Libs/imgui/` is **vendored and locally modified**.
This file is the complete list of those modifications. Every one of them is
also bracketed in the source with

```
// [MTENGINE-PATCH: <id>] <one-line reason>
...patched code...
// [/MTENGINE-PATCH: <id>]
```

so `grep -rn "MTENGINE-PATCH" src/Engine/Libs/imgui/ --include='*.h'
--include='*.cpp' --include='*.mm'` finds them all in one command (the
extension filter matters: this file and README.md quote the marker too), and `tests/test-imgui-patches.sh` fails the moment a marker exists
without a row here, or a row here without a marker.

**Bundled version:** Dear ImGui `1.93.0 WIP` (`IMGUI_VERSION_NUM 19293`),
**docking** branch, upstream `83f668625`. Reference clean clone used for
diffing: `~/develop/imgui` (branch `docking`). Upgraded from `1.92.6 WIP`
(`19259`, upstream `943fcc4f1`) on 2026-08-17 — 472 upstream commits, 6880
changed lines across the vendored set.

## Vendored, UNPATCHED, but load-bearing — check these on every upgrade

`imgui_impl_dx11.{h,cpp}` (added S-6 A3, byte-identical to upstream
`83f668625`) carries no `MTENGINE-PATCH` marker and therefore, correctly, no
row in the table below — `tests/test-imgui-patches.sh` has nothing to say about
it. But the registry's rows double as the UPGRADE CHECKLIST, and the upgrade
procedure is "copy the new sources over the bundled tree", so an unpatched file
with load-bearing semantics is exactly the thing that changes underneath us with
nobody looking. Two such semantics, both found by review rather than by a test:

* **BOTH of its samplers are mip-clamped.** `pTexSamplerLinear` and
  `pTexSamplerNearest` are created from ONE descriptor that sets
  `MinLOD = MaxLOD = 0.0f` before either, so neither ever samples a mip level.
  Our own D3D11 backend must therefore build its own sampler states with
  `MaxLOD = D3D11_FLOAT32_MAX`, or the KTX2/UASTC -> BC7 mipped atlas samples
  level 0 only — shimmering when minified, and the full-resolution fetch cost
  the mip chain exists to avoid. (The Metal backend already keeps its own two
  states, for the sibling reason recorded in the `metal-ktx2-mipmaps` row.)
  **`MaxLOD` alone is not the whole fix**, and stopping there is the trap: GL
  parity comes from the FILTER as well.
  `CRenderBackendOpenGL4::UpdateTextureLinearScaling` sets `MIN=LINEAR,
  MAG=NEAREST` for a non-linear image — nearest MAGNIFICATION only — so the
  D3D11 equivalent is **`D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR`**, NOT
  ImGui's `D3D11_FILTER_MIN_MAG_MIP_POINT`, which would make minification
  point-sampled too and alias every downscaled bitmap font.
* **`ImGui_ImplDX11_SetSwapChainDescs` copies ONE descriptor whatever the
  count**: it `resize(count)`s and then `memcpy`s `sizeof(DXGI_SWAP_CHAIN_DESC)`
  rather than `count *` that. Harmless while the only call site passes 1 — which
  is why it is recorded here rather than patched — but the obvious S-6 use, an
  HDR10 `R10G10B10A2_UNORM` template with an 8-bit fallback, would read
  uninitialised heap for element [1] — `ImVector::resize` does NOT
  value-initialise (it grows capacity and sets `Size`, nothing more), and
  `ImGui_ImplDX11_CreateWindow` really does walk the whole vector as a fallback
  chain, so the garbage IS read. Pass exactly one template, or bracket a patch
  and add a row when something needs more. Note also it is **not declared in
  `imgui_impl_dx11.h`** — only a local forward declaration in the `.cpp` — so a
  caller must declare the prototype itself, and a wholesale backend replacement
  that renames or drops it fails at LINK time, not at compile time.

## Extension hierarchy — try these in order before editing a source file

1. `imconfig.h` — the sanctioned extension point. Nothing else is needed for
   defines.
2. `IMGUI_USER_CONFIG` — a separate header, if `imconfig.h` itself would have
   to be edited beyond uncommenting an existing option.
3. `IM_VEC2_CLASS_EXTRA` / `IM_VEC4_CLASS_EXTRA` macros — for type interop.
4. Editing an ImGui source file — **last resort**, and only when the
   alternative is a disproportionate workaround. Bracket it, and add a row
   here with an honest *check on upgrade*.

## The patches

> **The registry cites files, not line numbers, on purpose.** Task 2 inserts two comment lines per bracketed pair, so any line number written in Task 1 is stale the moment Task 2 finishes — 11 of the 13 would have shipped wrong on day one. The markers exist so line numbers do not have to be maintained; carrying both recreates the fourth-copy-rot problem deviation 4 rejects the `.patch` for. `grep -rn "MTENGINE-PATCH: <id>" src/Engine/Libs/imgui/` locates any patch in one command, which is what the line number was for.
>
> **Origin** carries the engine commit and the ImGui version the patch was written against — design `#12` item 3 requires it and rev 1 dropped it. It is the signal a reviewer actually wants ("which upgrade has this already survived"): `436c720c` and `82825784` predate the 1.90.1→1.92.6 upgrade `fc92aff9`, while `7fe8946d`, `d6250e4d`, `61da7884` and `844b6135` postdate it.

| id | Files | Origin | Reason | Check on upgrade |
|---|---|---|---|---|
| `config-stb-sprintf` | `imconfig.h` | pre-1.92.6 | Uncomments `IMGUI_USE_STB_SPRINTF`. Faster and locale-independent formatting; the engine already vendors stb_sprintf. | Confirm the option still exists under this name and is still commented out by default upstream. Sanctioned extension point — no conflict expected. |
| `config-drawidx-32` | `imconfig.h` | pre-1.92.6 | Uncomments `#define ImDrawIdx unsigned int`. 16-bit indices overflow on dense UI: the filmstrip draws hundreds of rects per frame. | Same option, same place. If upstream ever changes the default to 32-bit, delete this line rather than keeping a redundant define. |
| `config-test-engine` | `imconfig.h` | `82825784`, ImGui 1.90.1 (pre-upgrade) | Adds an `#ifdef MT_ENABLE_IMGUI_TEST_ENGINE` block (renamed from `ENABLE_IMGUI_TEST_ENGINE` 2026-08-23, so it matches the `MT_ENABLE_*` scheme the capability vocabulary uses) turning on `IMGUI_ENABLE_TEST_ENGINE`, `IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL` and `IMGUI_TEST_ENGINE_ENABLE_CAPTURE`. The coupling point to the sibling vendored `src/Engine/Libs/imgui_test_engine/`. | The test engine is a **separate repo on its own release cadence**. After an ImGui bump, check the imgui_test_engine version compatibility table before assuming the hooks still line up; a mismatch shows up as a link error or a silently inert test run, not a compile error. |
| `config-math-operators` | `imconfig.h` | moved here 2026-08-17, ImGui 1.93.0 | `#define IMGUI_DEFINE_MATH_OPERATORS`, so `ImVec2` arithmetic is available in every TU. **Was `header-math-operators`, injected at line 1 of `imgui.h`.** | **This patch is now cheap.** `imconfig.h` is the sanctioned extension point, and `imgui.h` includes it at :70 — long before its own `#ifdef IMGUI_DEFINE_MATH_OPERATORS` block at :3073 — so the effect is identical while surviving a wholesale file replacement. The old line-1 form did **not** survive: the 1.93.0 copy destroyed it exactly as the previous registry predicted, which is why it moved. |
| `tabbar-triangle-hidden` | `imgui.h`, `imgui.cpp` ×2 | `436c720c`, ImGui 1.90.1 (pre-upgrade) | Adds `io.ConfigIsTabBarTriangleHidden`, inits it to `false`, and skips the dock-node "unhide tab bar" `AddTriangleFilled` when set. Three hunks, one feature. | All three sites or none. Losing only the guard leaves a dead flag and a triangle the app deliberately hides; losing only the field fails to compile, which is the safe half. Check that the draw site still exists in `DockNodeUpdate*` and has not moved to a different function. |
| `window-userdata` | `imgui_internal.h` | pre-1.92.6 | `void *userData;` as the last member of `ImGuiWindow`; the engine binds the owning `CGuiView*` to it. | Upstream reserves the right to reorganise `ImGuiWindow` freely. The field must stay **last** so member-order churn upstream never lands on it. If upstream ever adds a first-class user-data slot, migrate to it and delete this. |
| `docking-stale-layout-recovery` | `imgui.cpp` ×3 | `d6250e4d` + `7fe8946d`, ImGui 1.92.6 (post-upgrade) | Three upstream `IM_ASSERT`s replaced by recovery paths: a null dock node floats the window instead of aborting, a split node that still owns a `TabBar` has it removed, and a **dockspace node with no `HostWindow` is skipped for the frame** (added 2026-08-28 after `IM_ASSERT(node->HostWindow)` fired in RetroDebugger). All three fire when restoring a **stale workspace `.ini`** written by an older build. | **The other dangerous one, and the one that fails silently.** Losing these surfaces neither at build time nor in any test — only as a crash, for a user with an old layout file, on a machine you do not own. After every upgrade, re-locate **all three** asserts in the new source and re-apply. The third was found the hard way: sites 1 and 2 survived the 1.92.6 -> 1.93.0 upgrade intact, but the same failure had a third landing site nobody had hit yet, so a green patch check did **not** mean the family was covered. When this assert family fires again, assume a new site rather than a lost patch until you have checked the markers. If upstream has fixed the underlying issue, verify by *deleting* the patch and restoring an old `.ini`, not by reading the changelog. |
| `metal-ktx2-mipmaps` | `imgui_impl_metal.mm` | `61da7884`, ImGui 1.92.6; **relocated** 2026-08-17 for 1.93.0 | **Comment only.** Warns that linear mip filtering is load-bearing for KTX2 mipmapped textures uploaded by the engine. No code change. | **The check earned its keep on the very first upgrade.** 1.93.0 replaced the hard-coded `constexpr sampler ... mip_filter::linear` in the shader source with real `MTLSamplerState` objects — and now builds **two**, linear and nearest, selectable per draw via `ImGui_ImplMetal_DrawCallback_SetSampler{Linear,Nearest}`. The behaviour survives as `samplerStateLinear` (bound by default), so the comment moved to `samplerDescriptor.mipFilter = MTLSamplerMipFilterLinear`. New hazard to watch: do not switch a mipped KTX2 texture to the NEAREST sampler, which is `MipFilterNearest`. |
| `sdl3-viewport-to-window` | `imgui_impl_sdl3.cpp` | **renamed from `sdl2-viewport-to-window` at the SDL3 port (S-2 Task 3, 2026-08-18)**; originally `844b6135` era, ImGui 1.92.6 | Adds exported `SDL_Window *ImGui_ImplSDL3_ImGuiViewportToSDLWindow(ImGuiViewport*)` so engine code can reach the SDL window behind a viewport. **The patch got smaller in the move:** the SDL2 version reached into `PlatformUserData->Window` itself; SDL3's backend already ships `ImGui_ImplSDL3_GetSDLWindowFromViewport()` (`static`, via `PlatformHandle`/`WindowID`), so this is now a one-line forward to upstream's own lookup instead of a parallel one that could drift from it. | Still **not declared in `imgui_impl_sdl3.h`** — that header is byte-identical to upstream and the sole caller (`VID_Main.cpp`) declares the prototype itself. A wholesale backend replacement silently drops the definition; the failure is a link error, which is at least loud. The standing suggestion holds: if upstream ever exports its helper, delete this patch rather than keep both. |

> Every Files cell is a **bare filename**. That is the point of the note above: the markers carry the location, and `grep -rn "MTENGINE-PATCH: <id>" src/Engine/Libs/imgui/` prints it with today's line number rather than the one that was true when this file was written. The plan's own citation table (near the top) keeps the exact line numbers, because it is a snapshot for the person doing Task 2 — not a document that has to stay true afterwards.

## Upgrade procedure

1. `bash tests/test-imgui-patches.sh` on the **current** tree — it must pass
   before you start, or you are upgrading from an unknown state.
2. `bash tests/test-imgui-patches.sh --emit-patch > /tmp/imgui-local.patch`
   to capture the current diff against the clean clone.
3. Update the clean clone (`~/develop/imgui`) to the target version, copy the
   new sources over the bundled tree, and update the "Bundled version" line
   above.
4. Walk this table **top to bottom**, re-applying each patch and doing what
   its *check on upgrade* column says. Re-bracket as you go.
5. `bash tests/test-imgui-patches.sh` must pass again.
6. Restore a workspace `.ini` written by the previous build and confirm the
   app starts — this is the only test that exercises
   `docking-stale-layout-recovery`.
