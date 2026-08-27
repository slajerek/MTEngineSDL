#ifndef _CMT_NATIVE_MENU_BAR_WIN32_H_
#define _CMT_NATIVE_MENU_BAR_WIN32_H_

#include "CMTNativeMenuBar.h"
#include "CSlrKeyboardShortcuts.h"
#include <windows.h>
#include <vector>
#include <map>
#include <functional>
#include <string>

class CMTNativeMenuBarWin32;

class CMTMenuItemWin32 : public CMTMenuItem
{
public:
    UINT  id = 0;
    HMENU hParentMenu = NULL;
    std::string label;
    std::function<void()> action;
    CSlrKeyboardShortcut *shortcut = nullptr;
    bool enabled = true;
    bool checked = false;
    bool visible = true;
    CMTNativeMenuBarWin32 *owner = nullptr;

    void SetLabel(const char *lbl) override;
    void SetEnabled(bool v) override;
    void SetChecked(bool v) override;
    void SetVisible(bool v) override;

    std::wstring BuildItemText() const;
};

class CMTMenuWin32 : public CMTMenu
{
public:
    HMENU hMenu = NULL;
    std::vector<CMTMenuItemWin32 *> items;
    std::vector<CMTMenuWin32 *> subMenus;
    CMTNativeMenuBarWin32 *owner = nullptr;
    HWND hwnd = NULL;

    CMTMenuWin32(const char *lbl, HMENU hMenu, CMTNativeMenuBarWin32 *owner, HWND hwnd);
    ~CMTMenuWin32();

    CMTMenuItem *AddItem(const char *lbl, std::function<void()> action,
                         CSlrKeyboardShortcut *shortcut = nullptr) override;
    CMTMenuItem *AddSeparator() override;
    CMTMenu     *AddSubMenu(const char *lbl) override;
    void         RemoveItem(CMTMenuItem *item) override;
    void         RemoveAllItems() override;
};

class CMTNativeMenuBarWin32 : public CMTNativeMenuBar
{
public:
    explicit CMTNativeMenuBarWin32(HWND hwnd);
    ~CMTNativeMenuBarWin32();

    CMTMenu *AddMenu(const char *label) override;
    void     RemoveMenu(CMTMenu *menu) override;
    bool     IsNative() const override { return true; }
    void     HandlePlatformEvent(void *platformEventData) override;

    UINT AllocId()            { return nextId++; }
    void RegisterItem(UINT id, CMTMenuItemWin32 *item) { idToItem[id] = item; }
    void UnregisterItem(UINT id)                       { idToItem.erase(id);  }

    HWND hwnd = NULL;

private:
    HMENU hMenuBar = NULL;
    std::vector<CMTMenuWin32 *> menus;
    std::map<UINT, CMTMenuItemWin32 *> idToItem;
    UINT nextId = 1000;
};

#endif
