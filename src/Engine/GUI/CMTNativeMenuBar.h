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
};

#endif
