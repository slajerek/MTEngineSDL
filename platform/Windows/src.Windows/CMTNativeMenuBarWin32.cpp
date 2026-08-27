#include "CMTNativeMenuBarWin32.h"
#include "DBG_Log.h"

// Helper: UTF-8 to wstring
static std::wstring UTF8ToWide(const char *utf8)
{
    if (!utf8 || !*utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws.data(), len);
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return ws;
}

// CMTMenuItemWin32

std::wstring CMTMenuItemWin32::BuildItemText() const
{
    std::wstring text = UTF8ToWide(label.c_str());
    if (shortcut && shortcut->cstr && shortcut->cstr[0] != '\0')
        text += L"\t" + UTF8ToWide(shortcut->cstr);
    return text;
}

void CMTMenuItemWin32::SetLabel(const char *lbl)
{
    label = lbl;
    if (!hParentMenu) return;
    MENUITEMINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask  = MIIM_STRING;
    std::wstring text = BuildItemText();
    info.dwTypeData = const_cast<LPWSTR>(text.c_str());
    SetMenuItemInfoW(hParentMenu, id, FALSE, &info);
    if (owner) DrawMenuBar(owner->hwnd);
}

void CMTMenuItemWin32::SetEnabled(bool v)
{
    enabled = v;
    if (!hParentMenu) return;
    EnableMenuItem(hParentMenu, id, MF_BYCOMMAND | (v ? MF_ENABLED : MF_GRAYED));
    if (owner) DrawMenuBar(owner->hwnd);
}

void CMTMenuItemWin32::SetChecked(bool v)
{
    checked = v;
    if (!hParentMenu) return;
    CheckMenuItem(hParentMenu, id, MF_BYCOMMAND | (v ? MF_CHECKED : MF_UNCHECKED));
}

void CMTMenuItemWin32::SetVisible(bool v)
{
    visible = v;
    LOGD("CMTMenuItemWin32::SetVisible: not fully implemented on Win32");
}

// CMTMenuWin32

CMTMenuWin32::CMTMenuWin32(const char *lbl, HMENU hm, CMTNativeMenuBarWin32 *own, HWND hw)
{
    label = lbl;
    hMenu = hm;
    owner = own;
    hwnd  = hw;
}

CMTMenuWin32::~CMTMenuWin32()
{
    for (auto *item : items) { if (item->id) owner->UnregisterItem(item->id); delete item; }
    for (auto *sub : subMenus) delete sub;
    if (hMenu) DestroyMenu(hMenu);
}

CMTMenuItem *CMTMenuWin32::AddItem(const char *lbl, std::function<void()> action_fn,
                                    CSlrKeyboardShortcut *sc)
{
    auto *item     = new CMTMenuItemWin32();
    item->label    = lbl;
    item->action   = action_fn;
    item->shortcut = sc;
    item->id       = owner->AllocId();
    item->hParentMenu = hMenu;
    item->owner    = owner;

    std::wstring text = item->BuildItemText();
    AppendMenuW(hMenu, MF_STRING, item->id, text.c_str());
    owner->RegisterItem(item->id, item);
    DrawMenuBar(hwnd);

    items.push_back(item);
    return item;
}

CMTMenuItem *CMTMenuWin32::AddSeparator()
{
    auto *item = new CMTMenuItemWin32();
    item->isSeparator = true;
    item->id = 0;
    item->owner = owner;
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    items.push_back(item);
    return item;
}

CMTMenu *CMTMenuWin32::AddSubMenu(const char *lbl)
{
    HMENU hSub = CreatePopupMenu();
    std::wstring ws = UTF8ToWide(lbl);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSub, ws.c_str());
    DrawMenuBar(hwnd);

    auto *sub = new CMTMenuWin32(lbl, hSub, owner, hwnd);
    subMenus.push_back(sub);
    return sub;
}

void CMTMenuWin32::RemoveItem(CMTMenuItem *item)
{
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        if (*it == item)
        {
            CMTMenuItemWin32 *w = *it;
            if (!w->isSeparator)
            {
                ::RemoveMenu(hMenu, w->id, MF_BYCOMMAND);
                owner->UnregisterItem(w->id);
            }
            else
            {
                // For separators, we need to find and remove by position
                // since they have id=0. Find the index of this separator.
                int count = GetMenuItemCount(hMenu);
                for (int i = 0; i < count; i++)
                {
                    MENUITEMINFOW info = {};
                    info.cbSize = sizeof(info);
                    info.fMask  = MIIM_TYPE;
                    GetMenuItemInfoW(hMenu, i, TRUE, &info);
                    if (info.fType & MFT_SEPARATOR)
                    {
                        ::RemoveMenu(hMenu, i, MF_BYPOSITION);
                        break;
                    }
                }
            }
            delete w;
            items.erase(it);
            DrawMenuBar(hwnd);
            return;
        }
    }
}

void CMTMenuWin32::RemoveAllItems()
{
    while (GetMenuItemCount(hMenu) > 0)
        ::RemoveMenu(hMenu, 0, MF_BYPOSITION);
    for (auto *item : items) { if (item->id) owner->UnregisterItem(item->id); delete item; }
    items.clear();
    for (auto *sub : subMenus) delete sub;
    subMenus.clear();
    DrawMenuBar(hwnd);
}

// CMTNativeMenuBarWin32

CMTNativeMenuBarWin32::CMTNativeMenuBarWin32(HWND hw) : hwnd(hw)
{
    hMenuBar = CreateMenu();
    SetMenu(hwnd, hMenuBar);
    LOGD("CMTNativeMenuBarWin32: created, hwnd=%p", (void *)hw);
}

CMTNativeMenuBarWin32::~CMTNativeMenuBarWin32()
{
    for (auto *menu : menus) delete menu;
    if (hMenuBar) DestroyMenu(hMenuBar);
}

CMTMenu *CMTNativeMenuBarWin32::AddMenu(const char *lbl)
{
    HMENU hSub = CreatePopupMenu();
    std::wstring ws = UTF8ToWide(lbl);
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hSub, ws.c_str());
    DrawMenuBar(hwnd);

    auto *menu = new CMTMenuWin32(lbl, hSub, this, hwnd);
    menus.push_back(menu);
    return menu;
}

void CMTNativeMenuBarWin32::RemoveMenu(CMTMenu *menu)
{
    for (auto it = menus.begin(); it != menus.end(); ++it)
    {
        if (*it == menu)
        {
            CMTMenuWin32 *w = *it;
            int count = GetMenuItemCount(hMenuBar);
            for (int i = 0; i < count; i++)
            {
                if (GetSubMenu(hMenuBar, i) == w->hMenu)
                {
                    ::RemoveMenu(hMenuBar, i, MF_BYPOSITION);
                    break;
                }
            }
            delete w;
            menus.erase(it);
            DrawMenuBar(hwnd);
            return;
        }
    }
}

// SDL3 removed SDL_syswm.h, so the payload is now a plain Win32 MSG delivered
// by SDL_SetWindowsMessageHook rather than an SDL_SysWMmsg delivered by
// SDL_SYSWMEVENT. Same message, one less wrapper.
void CMTNativeMenuBarWin32::HandlePlatformEvent(void *platformEventData)
{
    MSG *msg = static_cast<MSG *>(platformEventData);
    if (msg == nullptr)
        return;
    if (msg->message == WM_COMMAND && HIWORD(msg->wParam) == 0)
    {
        UINT id = LOWORD(msg->wParam);
        auto it = idToItem.find(id);
        if (it != idToItem.end() && it->second->action)
            it->second->action();
    }
}
