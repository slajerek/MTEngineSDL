# Menu bar backends

The engine owns `guiMain->nativeMenuBar`. `PLATFORM_UpdateMenus()` creates it
during startup, after `GUI_Init()` and before `MT_PostInit()`. An app chooses
the backend in `MT_PreInit()`:

    SYS_SetMenuBarMode(EMTMenuBarMode::ImGui);

| Mode | macOS | Windows | Linux |
|---|---|---|---|
| `Native` (default) | `CMTNativeMenuBarMacOS`; none when headless | `CMTNativeMenuBarWin32` | `CMTNativeMenuBarImGui` |
| `ImGui` | `CMTNativeMenuBarImGui` (SDL's app menu stays) | `CMTNativeMenuBarImGui`, no HMENU | `CMTNativeMenuBarImGui` |
| `None` | no bar | no bar | no bar |

Rules:

- Never call `guiMain->SetNativeMenuBar()` from an app. Replacing the bar leaks
  the old one, and on Windows its HMENU stays attached to the window.
- `Native` on headless macOS leaves `nativeMenuBar` null. Check before use.
- Only the native macOS bar dispatches shortcuts. The ImGui and Win32 backends
  only display the shortcut text, so register shortcuts in the keyboard-shortcut
  system.
- Mutate menus from `MT_PostRenderEndFrame()`, never from `MT_Render()`.
- `SetVisible(false)` hides the bar, for a presentation over the whole window.
  The ImGui backend then draws nothing, so the main viewport's work area is
  the whole window; menus, items and shortcuts stay, and `SetVisible(true)`
  needs no rebuild. Native bars keep the flag and stay shown: the macOS bar
  belongs to the system (fullscreen hides it), and the Win32 HMENU is not
  detached. Call it from `MT_PostRenderEndFrame()`, like any menu change.
- `MT_MenuBarKindFor(mode, headless)` is the mapping in the table above; every
  platform's `PLATFORM_UpdateMenus()` switches on it.
- `CTestMenuBarMode` checks the mapping for every mode, then checks that the
  created bar matches the host's mode. Every host can register it.
