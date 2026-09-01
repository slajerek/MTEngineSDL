# Render loop re-entrancy

`VID_Render()` (`src/Engine/Core/Render/VID_Main.cpp`) can be re-entered **on its
own stack**, and until this was guarded that corrupted the ImGui frame state.

## How the re-entry happens

1. `SDL_filterEventCallback` (installed with `SDL_SetEventFilter` in
   `VID_RenderLoop`) calls `VID_Render()` when it sees
   `SDL_WINDOWEVENT_RESIZED`. That is the long-standing workaround for
   [SDL bug: window turns black on resize](https://stackoverflow.com/questions/34967628/sdl2-window-turns-black-on-resize)
   — without it the window stays black for the duration of a live drag.
2. SDL calls an event **filter** inline from `SDL_PushEvent()` — not from the
   event queue, and not necessarily from `SDL_PollEvent`. So the filter runs on
   whatever stack produced the event.
3. On Windows, plenty of ordinary calls make the OS resize the window
   *synchronously*: `DrawMenuBar()` after a native menu-bar change (non-client
   recalc → `WM_WINDOWPOSCHANGED`), `SetWindowPos`, a style change, etc. SDL's
   `WIN_WindowProc` turns those into `SDL_WINDOWEVENT_RESIZED` right there.
4. If any of that happens **during** the frame — i.e. from `GUI_Render()` →
   `MT_Render()` → view code — the filter re-enters `VID_Render()` while the
   ImGui frame is still open.

The nested call issues a second `ImGui::NewFrame()`, and the next frame trips:

```
imgui.cpp: (g.FrameCount == 0 || g.FrameCountEnded == g.FrameCount) &&
           "Forgot to call Render() or EndFrame() at the end of the previous frame?"
```

In debug builds that is an assert dialog; in release the assert is compiled out
and ImGui simply runs with inconsistent frame state.

Found via the photo app: choosing an entry from the Language menu rebuilt the
native Win32 menu bar from inside `MT_Render()` and asserted 100% of the time.
macOS was unaffected — an NSMenu lives in the system menu bar, so rebuilding it
never resizes the app window.

## The guard

`VID_Render()` sets `gVidRenderInProgress` for the duration of the frame (RAII,
so it clears on every exit path) and returns immediately if it is already set.
Skipping the nested render loses nothing: the outer frame is in flight and will
be presented, and the render loop draws again right after.

`VID_IsRenderingFrame()` (declared in `VID_Main.h`) exposes the flag so app code
can defer OS-window work instead of relying on the backstop.

## Guidance for apps

The guard is a safety net, not a licence. Keep mutation of OS window state —
native menu bars especially — **out of the ImGui frame**: do it from
`MT_PostRenderEndFrame()`, which runs after `ImGui::Render()`. the photo app's
`specs/claude/architecture/native-menu.md` documents that pattern.
