# MTEngine local patches to Dear ImGui Test Engine

`src/Engine/Libs/imgui_test_engine/` is **vendored and locally modified**, the
same way `src/Engine/Libs/imgui/` is, and this file is the complete list of
those modifications. It exists for one reason: the upgrade procedure for a
vendored tree is "copy the new sources over the bundled tree", which destroys
every local edit that nobody wrote down.

Same marker convention as the sibling registry:

```
// [MTENGINE-PATCH: <id>] <one-line reason>
...patched code...
// [/MTENGINE-PATCH: <id>]
```

`tests/test-imgui-patches.sh` checks **both** trees and fails the moment a
marker exists without a row here, or a row here without a marker.

**Bundled version:** the test engine is a **separate repository on its own
release cadence** from Dear ImGui, and the two have a compatibility table. After
an ImGui bump, check it before assuming the hooks still line up — a mismatch
shows up as a link error or a silently inert test run, not a compile error.
That warning is also the *check on upgrade* for imgui's own `config-test-engine`
patch, which is this tree's coupling point.

## The patches

| id | Files | Origin | Reason | Check on upgrade |
|---|---|---|---|---|
| `capability-gate` | `imgui_capture_tool.cpp`, `imgui_te_context.cpp`, `imgui_te_coroutine.cpp`, `imgui_te_engine.cpp`, `imgui_te_exporters.cpp`, `imgui_te_perftool.cpp`, `imgui_te_ui.cpp`, `imgui_te_utils.cpp` — all eight, one bracketed pair each | 2026-08-28, engine `dc798c9e`-era, capability programme Phase 3/5 | Wraps each translation unit in `#if MT_ENABLE_IMGUI_TEST_ENGINE`, so `MT_CAP_TEST_ENGINE=0` yields eight empty TUs instead of eight compile failures. `imconfig.h` gates `IMGUI_ENABLE_TEST_ENGINE` on that same expression, so with the capability off imgui carries no test-engine hooks and these files cannot compile — `ImGuiItemStatusFlags_Openable` and friends do not exist. **The build systems cannot drop the files instead:** a `PBXBuildFile` takes no condition, so a per-file exclusion would be MSBuild/CMake-only and macOS would silently diverge. Same guard, same reason, as `src/Engine/Tests/CImGuiTestEngine.cpp`. | **All eight or none.** Losing any one of them reopens `MT_CAP_TEST_ENGINE=0` as a build failure — loud, but only for whoever is building with the capability off, which is the capability matrix and nobody else day to day. Verify by actually building the DummyApp with `MT_CAP_TEST_ENGINE=0` in its manifest, not by reading the guards: the proof is that `nm` on the binary finds **zero** `ImGuiTestEngine_*` symbols where an all-on build has ~300. If upstream ever adds its own build-level opt-out, migrate to it and delete this. |

> Files are cited as bare filenames on purpose — the markers carry the location,
> and `grep -rn "MTENGINE-PATCH: <id>" src/Engine/Libs/imgui_test_engine/`
> prints it with today's line numbers rather than the ones that were true when
> this file was written.

## A note on `imgui_te_engine.cpp` and its BOM

That file — and only that file — begins with a UTF-8 BOM (`EF BB BF`). The
guard is inserted **after** the BOM, so the BOM stays at byte 0. Putting it
before would leave a stray character mid-file and fail to compile. Any tool that
re-applies this patch has to preserve that ordering.

## Upgrade procedure

1. `bash tests/test-imgui-patches.sh` on the **current** tree — it must pass
   before you start, or you are upgrading from an unknown state.
2. Check the test engine's compatibility table against the bundled ImGui
   version before copying anything.
3. Copy the new sources over the bundled tree and re-apply the table above,
   re-bracketing as you go.
4. `bash tests/test-imgui-patches.sh` must pass again.
5. Build an app with `MT_CAP_TEST_ENGINE=0` **and** with it on, and check the
   symbol counts both ways. Step 4 only proves the markers are present; it
   cannot tell a guard that compiles from a guard that works.
