// ../MTEngineSDL/src/Engine/GUI/CMTNativeMenuBar.h
#ifndef _CMT_NATIVE_MENU_BAR_H_
#define _CMT_NATIVE_MENU_BAR_H_

#include <functional>
#include <string>

class CSlrKeyboardShortcut;
class CMTMenu;
class CMTNativeMenuBar;

class CMTMenuItem
{
public:
    virtual ~CMTMenuItem() = default;
    virtual void SetLabel(const char *label) = 0;
    virtual void SetEnabled(bool enabled) = 0;
    virtual void SetChecked(bool checked) = 0;
    virtual void SetVisible(bool visible) = 0;

    bool isSeparator = false;
};

class CMTMenu
{
public:
    std::string label;

    virtual ~CMTMenu() = default;
    virtual CMTMenuItem *AddItem(const char *label,
                                  std::function<void()> action,
                                  CSlrKeyboardShortcut *shortcut = nullptr) = 0;
    virtual CMTMenuItem *AddSeparator() = 0;
    virtual CMTMenu     *AddSubMenu(const char *label) = 0;
    virtual void         RemoveItem(CMTMenuItem *item) = 0;
    virtual void         RemoveAllItems() = 0;
};

class CMTNativeMenuBar
{
public:
    virtual ~CMTNativeMenuBar() = default;
    virtual CMTMenu *AddMenu(const char *label) = 0;
    virtual void     RemoveMenu(CMTMenu *menu) = 0;
    virtual bool     IsNative() const = 0;
    virtual void     RenderFrame() {}
    virtual void     HandlePlatformEvent(void *platformEventData) {}

    // Whether the bar is drawn. Hidden, the ImGui bar is not drawn and takes
    // no space (the main viewport's work area is the whole window); its
    // menus and items stay, and showing it again needs no rebuild. Shortcuts
    // are unaffected: they live in the keyboard-shortcut system. Native bars
    // keep the flag but stay shown: the macOS bar belongs to the system (it
    // hides in fullscreen), and the Win32 HMENU is not detached. For a
    // presentation over the whole window.
    void SetVisible(bool v) { visible = v; }
    bool IsVisible() const  { return visible; }

    // On some platforms (macOS) the OS native menu fires a menu item's key
    // equivalent directly, while SDL ALSO delivers the same keystroke as a
    // normal key event — running the bound action twice (a toggle nets to
    // nothing; an idempotent action silently runs twice). The event pump calls
    // this for each SDL key-down: return true (consuming the record) when the
    // native menu already handled this exact key as a key equivalent, so the
    // caller skips its in-app key routing. Menu bars that do not auto-dispatch
    // key equivalents (ImGui display-only, Win32 text-only accelerators) keep
    // the default and never suppress.
    virtual bool WasKeyHandledByNativeMenu(unsigned int sdlKeyCode,
                                           bool shift, bool alt,
                                           bool control, bool super) { return false; }

protected:
    bool visible = true;
};

// --- Which menu bar the engine creates ---------------------------------------
// PLATFORM_UpdateMenus() runs before MT_PostInit() and creates the bar. An app
// that wants a different backend sets the mode in MT_PreInit(); nothing else
// may create or replace guiMain->nativeMenuBar (replacing it leaks the old bar,
// and on Windows leaves its HMENU attached to the window). See docs/menu-bar.md.
//
//   Native  macOS: native NSMenu (none when headless); Windows: Win32 HMENU;
//           Linux: ImGui. The default; existing apps are unchanged.
//   ImGui   CMTNativeMenuBarImGui on every platform, headless included.
//   None    no bar; guiMain->nativeMenuBar stays null. For apps that draw
//           ImGui::BeginMainMenuBar() themselves.
enum class EMTMenuBarMode { Native, ImGui, None };

inline EMTMenuBarMode &MT_MenuBarModeStorage()
{
	static EMTMenuBarMode mode = EMTMenuBarMode::Native;
	return mode;
}

inline void SYS_SetMenuBarMode(EMTMenuBarMode mode) { MT_MenuBarModeStorage() = mode; }
inline EMTMenuBarMode SYS_GetMenuBarMode() { return MT_MenuBarModeStorage(); }

// What a mode yields on this platform. The ONE mapping: every PLATFORM_UpdateMenus()
// switches on it, and CTestMenuBarMode checks it for every mode. Pure, no state.
enum class EMTMenuBarKind { None, ImGui, Native };

inline EMTMenuBarKind MT_MenuBarKindFor(EMTMenuBarMode mode, bool headless)
{
	switch (mode)
	{
		case EMTMenuBarMode::None:  return EMTMenuBarKind::None;
		case EMTMenuBarMode::ImGui: return EMTMenuBarKind::ImGui;
		case EMTMenuBarMode::Native:
#if defined(LINUX)
			return EMTMenuBarKind::ImGui;     // no native backend on Linux
#elif defined(MACOS)
			return headless ? EMTMenuBarKind::None : EMTMenuBarKind::Native;
#else
			return EMTMenuBarKind::Native;    // Windows: unchanged from before the mode existed
#endif
	}
	return EMTMenuBarKind::None;
}

#endif
