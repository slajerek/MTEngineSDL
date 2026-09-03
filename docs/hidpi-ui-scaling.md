# HiDPI and UI scaling — why an MTEngineSDL app looks tiny on Windows

**Symptom.** The app runs on a high-density display. Every other window —
Explorer, the Start menu, Total Commander — looks normal. Ours renders at
roughly half the size. On macOS the same build looks correct.

Nothing is broken. The two operating systems divide the work differently, and
for a long time this engine did only the macOS half.

**That is fixed: `MT_UiScale.h` (#4) owns the scale now, and the engine applies
it to its own style and its own legacy views.** An app tells it what the scale
should be and scales its own fixed pixel constants with `MT_UiScaled()`; it does
not have to re-derive the platform rule or re-assert the scale after a style
change. Sections 1-3 remain the explanation of WHY, and are worth reading before
changing any of it.

---

## 1. Who scales, and when

| | Who does the scaling | What the app receives | What the app must do |
|---|---|---|---|
| **macOS** | the OS | logical points; a backing scale factor (2.0 on Retina) | nothing — an 18 px font is rasterised into 36 physical px |
| **Windows, DPI-*unaware* process** | the OS, by bitmap-stretching the whole window | pretend-96-DPI pixels | nothing — correct size, but blurry |
| **Windows, DPI-*aware* process** | **the app** | **real physical pixels** | **scale its own UI** — or everything is drawn at 1:1 and looks tiny |

An MTEngineSDL app lands in the third row, and that is not a choice anyone made
in this repository: **SDL3 declares per-monitor DPI awareness on our behalf.**

`other/lib/SDL-release-3.4.14-static/src/video/windows/SDL_windowsvideo.c`,
`WIN_InitDPIAwareness()`:

```c
const char *hint = SDL_GetHint("SDL_WINDOWS_DPI_AWARENESS");
if (!hint || SDL_strcmp(hint, "permonitorv2") == 0) {
    WIN_DeclareDPIAwarePerMonitorV2(_this);   // <- the default
}
```

With no hint set, the process becomes `PER_MONITOR_AWARE_V2`. Windows then
stops compensating and hands over real pixels. At 200% display scaling that
means Explorer scales itself and we do not.

The `.exe` manifest is a red herring: DummyApp's carries no `<dpiAware>`
element at all, and it does not matter, because SDL sets awareness
programmatically at video init, which wins.

---

## 2. Measuring the display DPI — and the trap that will fool you first

The obvious check reports the wrong number, confidently:

```powershell
GetDpiForSystem()      # 96  -> "100%, nothing to fix here"
```

`GetDpiForSystem` returns what **the calling process** is entitled to see, and
a DPI-unaware process (PowerShell, cmd, most quick test harnesses) is always
told 96. **The value is a property of the caller, not of the monitor.**

Ask the monitor instead, from a process that has declared awareness first:

```powershell
[void]$t::SetProcessDPIAware()
$mon = $t::MonitorFromPoint((New-Object System.Drawing.Point(10,10)), 2)
$t::GetDpiForMonitor($mon, 0, [ref]$dx, [ref]$dy)   # 192 -> 200%
```

On the machine where this was written the two calls disagreed 96 vs 192, and
the first answer sent the investigation in the wrong direction until a plain
observation contradicted it: *other applications look fine*. If the display
really were at 100%, Explorer would be tiny too. **When a measurement and an
observation disagree, re-check the measurement.**

Inside the app, SDL3 answers directly and honestly:

- `SDL_GetWindowDisplayScale(window)` — the scale for the window's display
- `SDL_GetDisplayContentScale(displayId)` — per display

(`SDL_GetDisplayDPI` was **removed** in SDL3; there is no direct replacement.)
`CGuiViewUiDebug` already prints both — the quickest in-app check.

---

## 3. What ImGui gives you, and what it does not

ImGui 1.92+ (this tree runs 1.93 WIP) splits scaling into two knobs, and they
are not interchangeable:

| Field | Owner | Use |
|---|---|---|
| `style.FontScaleMain` | **the app** | *"Main global scale factor. May be set by application once, or exposed to end-user."* This is the one a GUI-Scale setting writes. |
| `style.FontScaleDpi` | ImGui's DPI path | Set from the monitor's content scale. Overwritten automatically when `io.ConfigDpiScaleFonts` is on. **Do not write it from a menu** — you would be fighting that path. |

`io.ConfigDpiScaleFonts` looks like the whole answer and is not. ImGui's own
comment: *"This will scale fonts but _NOT_ scale sizes/padding for now."*
Fonts alone still leaves buttons, padding and row heights at 1:1.

**Geometry is separate.** `ImGuiStyle::ScaleAllSizes()` handles it, with a trap
worth stating loudly (`MT_Theme.h` documents it and `CTestThemeScale` guards
it):

> `ScaleAllSizes` does `_MainScale *= scale_factor` and `ImTrunc`s every field.
> Applying it to an already-scaled style both **compounds** and **loses
> precision**. The failure mode is *"the UI grows every time you open
> Settings"*, which is rarely traced back to its cause.

So geometry must always be rebuilt **from a default-constructed `ImGuiStyle`**
and scaled **exactly once** — never scaled in place. In this engine that
rebuild is what a theme does, via `MT_ThemeApplyResolved(resolved, guiScale)`.

---

## 4. What the engine provides

**`MT_UiScale.h` (`src/Engine/GUI/`) is the answer to all of this.** It owns the
scale, so no app has to work out the macOS-versus-Windows rule for itself and
the engine's own legacy views scale with everything else.

| Symbol | What it is |
|---|---|
| `MT_GetUiScale()` | the scale in force. 1.0 until an app sets one, so an app that never opts in behaves exactly as before |
| `MT_UiScaled(v)` | a fixed pixel constant, scaled. Use it **where the constant is written** |
| `MT_SetUiScale(v)` | set it. Clamped to the ladder; re-applies the style, or re-applies the theme at the new scale if one is active |
| `MT_DetectDisplayUiScale()` | what the display asks for. **1.0 on macOS** (the OS already scaled) and 1.0 headless (so no test baseline moves with the build machine's monitor) |
| `MT_UiScaleApplyToImGuiStyle()` | writes `FontScaleMain` and scales geometry. Called from `VID_FinishStyleChange()`; an app should not need it |

and, underneath, the theme pieces it builds on:

| Symbol | Header | What it is |
|---|---|---|
| `MT_kGuiScaleSteps[]`, `MT_kGuiScaleStepCount` | `MT_Theme.h` | the discrete ladder, 0.25 -> 3.00. **Discrete, not a slider**: every value can be pixel-checked, and it bounds font-atlas growth |
| `MT_ThemeClampGuiScale(float)` | `MT_Theme.h` | snaps any value onto the nearest rung |
| `CMTThemeRegistry::SetActiveTheme(id, mode, guiScale)` | `CMTThemeRegistry.h` | re-applies a theme at a new scale -- the safe way to rescale **geometry**, because it rebuilds from a fresh style |
| `MT_ThemeApplyGeometry(ImGuiStyle&)` | `MT_Theme.h` | the geometry table at scale 1.0 |

### What it deliberately does NOT do

It does not make `CGuiButton` and the other legacy controls scale themselves,
and that is a design decision rather than an omission.

A legacy control renders at `posX`/`posY` that its parent view derived from an
ImGui window rect -- **already device pixels** -- while its `sizeX`, `sizeY` and
`fontScale` come from constants the **caller** wrote. A constructor cannot tell
those apart. Scaling them would leave a control's position and its size in
different units, and would double-scale every caller that had already
converted. So the contract runs the other way: **a fixed pixel constant is
scaled where it is written**, by the engine for its own controls and by each app
for its own.

## 5. Recipe for an app

**Startup**, before the first frame -- not in a menu's lazy init, or the first
frame is drawn at the wrong size:

```cpp
float scale = MT_DetectDisplayUiScale();      // or a value the user picked
MT_SetUiScale(scale);
```

**On change** (menu item, combo, wherever): the same call. `MT_SetUiScale`
re-applies the style itself, and re-applies the active theme at the new scale if
the app has one, so there is nothing else to remember.

No restart is needed: it takes effect on the next frame. (Contrast the
*renderer* setting, which does need one -- the device and swapchain already
exist.)

**Scaling your own constants**: `MT_UiScaled(7.0f)` where you would have written
`7.0f`. Not on a value already derived from a view's rect -- that has already
grown.

**What the app still owns**: whether the scale follows the display or a stored
preference, and migrating any geometry the app has already persisted. Those are
policy and app-specific formats; RetroDebugger's are in `C64DUiScale.h` in
that repository.

### A config trap that is NOT there -- and how a bad test invented one

the photo app pairs its read with a `SetFloatSkipConfigSave("ui.guiScale", ...)`
registration in `Save()`, which looks like it must be load-bearing. **It is not,
for this purpose.** Two guesses about why it might be were both tested and both
are false:

- **"Without it the key is dropped."** No. `CConfigStorageHjson` parses the
  whole file into `hjsonRoot` and `SaveConfig()` marshals the whole tree, so
  every key round-trips whether or not anything `Set` it.
- **"It normalises an off-ladder value in the file."** No. It writes to the
  in-memory tree and deliberately skips the save, so with nothing else saving
  afterwards a hand-edited `1.37` is still `1.37` on disk. It *is* clamped in
  memory, so the UI and the menu tick are correct either way.

**Worth recording is how the false version nearly got written down as fact.**
The first test appeared to prove the key was dropped: a value was hand-written
into `settings.hjson` with a PowerShell regex, the app ran, the key was gone.
What had actually happened is that the *edit* corrupted the file -- several
unrelated keys (`MainWindowX`, `renderBackend`, `uiImGuiStyle`) had vanished
too, which is the detail that gives it away. Re-done with a well-formed edit,
the key survives untouched.

The lesson generalises past this key: **when a test says the code is broken,
check the test's own setup before believing it** -- especially when the failure
is larger than the change could explain.

## 6. Where the scale is applied, and why there is no re-assert dance

`VID_SetImGuiStyle` rebuilds every geometry field from a default `ImGuiStyle`
before applying a palette. That silently throws away any scale already on the
style -- and it runs on a theme switch, on a macOS system-appearance flip under
`IMGUI_STYLE_SYSTEM`, and on a custom-style load.

So the scale is re-applied from **`VID_FinishStyleChange()`**, the single tail
every one of those paths passes through. A host does not have to notice a style
change and put the scale back; the first app to hit this did exactly that with a
per-frame guard, and it is gone now.

Two rules that function obeys, both of which cost a bug to learn:

- **It is idempotent.** `ScaleAllSizes` multiplies IN PLACE and `ImTrunc`s every
  field, so calling it twice squares the scale. `MT_UiScaleApplyToImGuiStyle`
  keeps its own copy of the pre-scale geometry and restores it before scaling.
  The symptom of getting this wrong is *"the UI grows every time you open
  Settings"*, which is rarely traced back to its cause; `CTestThemeScale` and
  RetroDebugger's `CTestUiScale` both guard it.
- **A theme outranks it.** `MT_ThemeApplyResolved` builds geometry from a fresh
  style and sets `FontScaleMain` itself, both at the theme's `guiScale`. When a
  theme is active the scale function returns immediately, and `MT_SetUiScale`
  re-applies the theme rather than scaling underneath it.

At scale 1.0 the geometry pass is skipped entirely: `ScaleAllSizes` `ImTrunc`s
even by a factor of one, which would quietly turn IntelliJ's 5.3 rounding and
6.5 spacing into 5 and 6. Scale 1.0 is byte-identical to the engine without this
feature -- which is every macOS host and every Windows host at 100%.

## 7. Should the default be automatic?

The engine **answers** the question (`MT_DetectDisplayUiScale()`) and lets the
**app decide** whether to follow it. That split is deliberate, because the edges
are real and they are not the engine's to resolve:

- it must apply on **first run only**, never overriding a saved choice -- which
  means knowing the app's config schema;
- multi-monitor setups with different scales have no single right answer, and
  `PER_MONITOR_AWARE_V2` means the scale can change while running;
- an app that has already persisted geometry has to migrate it, in its own
  format, or the user's saved layout lands in the wrong place.

RetroDebugger takes the auto default and migrates; it also chose **not** to
re-detect while running, because every re-detection rescales every stored
workspace and its `.ini` geometry is integer, so a laptop that docks and undocks
all day would erode its own layouts by rounding. That is a defensible policy
rather than the only one, which is exactly why it lives in the app.

The alternative, opting out of DPI awareness via
`SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "unaware")`, gives correct sizing for
free by letting Windows stretch the window, at the cost of a blurry UI on every
HiDPI display. It is a legitimate choice for a tool, and the wrong one for an
image viewer.

## 8. Provenance

Written 2026-08-24 while adding **Settings > GUI Scale** to
MTEngineSDLDummyApp. Verified on that machine: Windows 11 ARM64 (Parallels),
primary display 2560×1440 reported at **192 DPI / 200%**, SDL 3.4.14, ImGui
1.93.0 WIP.

What is measured here versus reasoned, because the difference matters to whoever
picks this up next:

- **Measured**: the 96-vs-192 `GetDpiForSystem` discrepancy; that the key
  survives a run and that an off-ladder value is not normalised on disk; that
  the first "config drop" result was an artefact of a corrupted test edit.
- **Read from source, not assumed**: SDL3's default DPI awareness
  (`WIN_InitDPIAwareness`), and ImGui's own comments on `FontScaleMain` /
  `FontScaleDpi` / `ConfigDpiScaleFonts`.
- **Not verified here**: the macOS side of the table. It is the documented
  behaviour of the platform and matches the observation that the same build
  looks right there, but nothing in this session measured a backing scale
  factor on a Mac.

### 2026-08-25 — the engine took ownership

Sections 4 to 7 were rewritten when `MT_UiScale` was added. Before that the
engine only offered the ladder and the theme machinery, and each app was
expected to follow a manual recipe; RetroDebugger did, and that surfaced three
things worth keeping:

- **The app-side version could not fix the engine's own views.**
  `CGuiViewSaveFile`, `CGuiViewSelectFolder`, `CGuiViewToolBox`,
  `CGuiViewFrame` and `CGuiViewUiDebug` all carry fixed pixel constants
  (`buttonSizeX = 60`, `fontScale = 1.5`) that no app can reach. They were
  still half-size on a 200% display after the app had scaled everything it
  owned. That is what moved the scale in here.
- **`ScaleAllSizes` compounding is not theoretical.** RetroDebugger's
  `CTestUiScale` caught it on its first run: `FramePadding` 6 -> 12,
  `ScrollbarSize` 28 -> 56 on the second apply, because the app-side version
  assumed the engine had always just rebuilt the style. True for a theme
  switch, false for a bare re-apply and false for a scale change. Hence the
  pre-scale snapshot in `MT_UiScaleApplyToImGuiStyle`.
- **Re-asserting the scale per frame was a workaround, not a fix.** The app
  ran a per-frame guard that compared two style fields and re-applied when they
  moved. Applying from `VID_FinishStyleChange()` instead -- the one tail every
  style change passes through -- removed the need for it entirely.

Measured on that pass: a Windows 11 x64 machine whose display changed mid-
session from 3456x2158 @ 250% to 1920x1080 @ 100%, which is also how the
"auto is resolved once at startup" edge in #7 stopped being hypothetical.

Still not verified: macOS and Linux builds of any of this. All three build
systems carry the new files, but only the Windows build has been run.
