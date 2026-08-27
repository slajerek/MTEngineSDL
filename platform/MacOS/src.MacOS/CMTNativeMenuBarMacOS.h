#ifndef _CMT_NATIVE_MENU_BAR_MACOS_H_
#define _CMT_NATIVE_MENU_BAR_MACOS_H_

#import <AppKit/AppKit.h>
#include "CMTNativeMenuBar.h"
#include "CSlrKeyboardShortcuts.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>

@class CMTMenuActionTarget;

class CMTMenuItemMacOS : public CMTMenuItem
{
public:
    NSMenuItem        *nsItem = nil;
    CMTMenuActionTarget *actionTarget = nil;
    std::string        label;
    std::function<void()> action;
    CSlrKeyboardShortcut *shortcut = nullptr;
    bool enabled = true;
    bool checked = false;
    bool visible = true;

    void SetLabel(const char *lbl) override;
    void SetEnabled(bool v) override;
    void SetChecked(bool v) override;
    void SetVisible(bool v) override;
};

class CMTMenuMacOS : public CMTMenu
{
public:
    NSMenuItem *nsParentItem = nil;
    NSMenu     *nsMenu = nil;
    std::vector<CMTMenuItemMacOS *> items;
    std::vector<CMTMenuMacOS *> subMenus;
    CMTMenuActionTarget *sharedTarget = nil;

    CMTMenuMacOS(const char *lbl, NSMenu *menu, NSMenuItem *parent,
                 CMTMenuActionTarget *target);
    ~CMTMenuMacOS();

    CMTMenuItem *AddItem(const char *lbl, std::function<void()> action,
                         CSlrKeyboardShortcut *shortcut = nullptr) override;
    CMTMenuItem *AddSeparator() override;
    CMTMenu     *AddSubMenu(const char *lbl) override;
    void         RemoveItem(CMTMenuItem *item) override;
    void         RemoveAllItems() override;
};

class CMTNativeMenuBarMacOS : public CMTNativeMenuBar
{
public:
    CMTNativeMenuBarMacOS();
    ~CMTNativeMenuBarMacOS();

    CMTMenu *AddMenu(const char *label) override;
    void     RemoveMenu(CMTMenu *menu) override;
    bool     IsNative() const override { return true; }
    bool     WasKeyHandledByNativeMenu(unsigned int sdlKeyCode,
                                       bool shift, bool alt,
                                       bool control, bool super) override;

private:
    NSMenu             *nsMainMenu = nil;
    CMTMenuActionTarget *target = nil;
    std::vector<CMTMenuMacOS *> menus;
};

#endif
