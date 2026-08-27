#include "CMTNativeMenuBarImGui.h"
#include "CSlrKeyboardShortcuts.h"
#include "imgui.h"
#include <algorithm>

// CMTMenuImGui

CMTMenuImGui::CMTMenuImGui(const char *lbl)
{
    label = lbl;
}

CMTMenuItem *CMTMenuImGui::AddItem(const char *lbl, std::function<void()> action,
                                    CSlrKeyboardShortcut *shortcut)
{
    auto item = std::make_unique<CMTMenuItemImGui>();
    item->label   = lbl;
    item->action  = action;
    item->shortcut = shortcut;
    auto *ptr = item.get();
    items.push_back(std::move(item));
    return ptr;
}

CMTMenuItem *CMTMenuImGui::AddSeparator()
{
    auto item = std::make_unique<CMTMenuItemImGui>();
    item->isSeparator = true;
    auto *ptr = item.get();
    items.push_back(std::move(item));
    return ptr;
}

CMTMenu *CMTMenuImGui::AddSubMenu(const char *lbl)
{
    auto sub = std::make_unique<CMTMenuImGui>(lbl);
    auto *ptr = sub.get();
    subMenus.push_back(std::move(sub));
    return ptr;
}

void CMTMenuImGui::RemoveItem(CMTMenuItem *item)
{
    items.erase(std::remove_if(items.begin(), items.end(),
        [item](const std::unique_ptr<CMTMenuItemImGui> &p){ return p.get() == item; }),
        items.end());
}

void CMTMenuImGui::RemoveAllItems()
{
    items.clear();
}

void CMTMenuImGui::Render()
{
    if (!ImGui::BeginMenu(label.c_str()))
        return;

    for (auto &item : items)
    {
        if (!item->visible) continue;
        if (item->isSeparator) { ImGui::Separator(); continue; }
        const char *shortcutStr = item->shortcut ? item->shortcut->cstr : nullptr;
        if (ImGui::MenuItem(item->label.c_str(), shortcutStr, item->checked, item->enabled))
            if (item->action) item->action();
    }
    for (auto &sub : subMenus)
        sub->Render();

    ImGui::EndMenu();
}

// CMTNativeMenuBarImGui

CMTMenu *CMTNativeMenuBarImGui::AddMenu(const char *lbl)
{
    auto menu = std::make_unique<CMTMenuImGui>(lbl);
    auto *ptr = menu.get();
    menus.push_back(std::move(menu));
    return ptr;
}

void CMTNativeMenuBarImGui::RemoveMenu(CMTMenu *menu)
{
    menus.erase(std::remove_if(menus.begin(), menus.end(),
        [menu](const std::unique_ptr<CMTMenuImGui> &p){ return p.get() == menu; }),
        menus.end());
}

void CMTNativeMenuBarImGui::RenderFrame()
{
    if (!ImGui::BeginMainMenuBar()) return;
    for (auto &menu : menus)
        menu->Render();
    ImGui::EndMainMenuBar();
}
