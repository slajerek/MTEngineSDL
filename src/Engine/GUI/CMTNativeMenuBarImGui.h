#ifndef _CMT_NATIVE_MENU_BAR_IMGUI_H_
#define _CMT_NATIVE_MENU_BAR_IMGUI_H_

#include "CMTNativeMenuBar.h"
#include <vector>
#include <memory>
#include <string>
#include <functional>

class CSlrKeyboardShortcut;

struct CMTMenuItemImGui : public CMTMenuItem
{
    std::string label;
    std::function<void()> action;
    CSlrKeyboardShortcut *shortcut = nullptr;
    bool enabled = true;
    bool checked = false;
    bool visible = true;

    void SetLabel(const char *lbl) override   { label   = lbl; }
    void SetEnabled(bool v) override           { enabled = v;   }
    void SetChecked(bool v) override           { checked = v;   }
    void SetVisible(bool v) override           { visible = v;   }
};

class CMTMenuImGui : public CMTMenu
{
public:
    explicit CMTMenuImGui(const char *lbl);

    CMTMenuItem *AddItem(const char *lbl, std::function<void()> action,
                         CSlrKeyboardShortcut *shortcut = nullptr) override;
    CMTMenuItem *AddSeparator() override;
    CMTMenu     *AddSubMenu(const char *lbl) override;
    void         RemoveItem(CMTMenuItem *item) override;
    void         RemoveAllItems() override;

    void Render();

    std::vector<std::unique_ptr<CMTMenuItemImGui>> items;
    std::vector<std::unique_ptr<CMTMenuImGui>> subMenus;
};

class CMTNativeMenuBarImGui : public CMTNativeMenuBar
{
public:
    CMTMenu *AddMenu(const char *label) override;
    void     RemoveMenu(CMTMenu *menu) override;
    bool     IsNative() const override { return false; }
    void     RenderFrame() override;

    std::vector<std::unique_ptr<CMTMenuImGui>> menus;
};

#endif
