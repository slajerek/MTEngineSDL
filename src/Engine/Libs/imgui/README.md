# Dear ImGui (vendored)

Dear ImGui `1.93.0 WIP` (`IMGUI_VERSION_NUM 19293`), **docking** branch,
upstream `83f668625`, locally modified.

This line drifted once already: it still said `1.92.6 WIP` / `19259` for
months after the 2026-08-17 upgrade, because `tests/test-imgui-patches.sh`
compares PATCH MARKERS against `MTENGINE_PATCHES.md` and can never see a
version string. `imgui.h:32-33` and `MTENGINE_PATCHES.md`'s "Bundled
version" row are the two places that must agree with this one.

**The list of local modifications lives in
[`MTENGINE_PATCHES.md`](MTENGINE_PATCHES.md), not here.** Every patch is also
bracketed in the source:

```
grep -rn "MTENGINE-PATCH" src/Engine/Libs/imgui/ \
     --include='*.h' --include='*.cpp' --include='*.mm'
```

The extension filter is not optional: this file and `MTENGINE_PATCHES.md` both
quote the marker, so an unfiltered grep counts documentation as code.

Run `tests/test-imgui-patches.sh` to check the markers and the registry still
agree, and read `MTENGINE_PATCHES.md`'s *Upgrade procedure* before bumping the
version.

> Historical note: this file previously documented a per-monitor DPI patch
> (`MACOS_GetBackingScaleFactor`, a `FramebufferScale` override). **That patch
> no longer exists** — it was dropped in the 1.90.1 → 1.92.6 upgrade
> (`fc92aff9`) because upstream added `Platform_GetWindowFramebufferScale()`
> (`ImGui_ImplSDL2_GetWindowFramebufferScale` in `imgui_impl_sdl2.cpp`).
> `MACOS_GetBackingScaleFactor()` survives as an ordinary engine function
> (`platform/MacOS/src.MacOS/SYS_MacOS.mm:115`) with three call sites in the
> renderer and UI debug view, but nothing in the ImGui sources calls it.
