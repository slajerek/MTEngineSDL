#include "CMTNativeMenuBarMacOS.h"
#include "DBG_Log.h"
#include <SDL3/SDL.h>

// One-slot record of the last menu item fired via its key equivalent. AppKit
// dispatches the NSMenuItem action during SDL's Cocoa event pump; SDL then also
// delivers the same key as an SDL_EVENT_KEY_DOWN. CMTNativeMenuBarMacOS::
// WasKeyHandledByNativeMenu() reads this so the pump can drop that duplicate,
// preventing the bound action from firing twice. Main/UI-thread access only.
struct MTMenuKeyEquivalentRecord
{
    bool pending = false;
    int  keyCode = 0;
    bool shift = false, alt = false, control = false, super = false;
};
static MTMenuKeyEquivalentRecord sLastMenuKeyEquivalent;

// Obj-C action target — one shared instance per menu bar dispatches all clicks
@interface CMTMenuActionTarget : NSObject
- (void)menuItemFired:(NSMenuItem *)sender;
@end

@implementation CMTMenuActionTarget
- (void)menuItemFired:(NSMenuItem *)sender
{
    CMTMenuItemMacOS *item = (CMTMenuItemMacOS *)[[sender representedObject] pointerValue];
    if (!item || !item->action)
        return;

    // Record when the item was triggered by its key equivalent (a keyDown) as
    // opposed to a mouse click, so the SDL event pump skips re-dispatching that
    // same keystroke to the in-app key router. Without this the action fires
    // twice on macOS (native menu + SDL). Mouse clicks don't produce a matching
    // SDL_EVENT_KEY_DOWN, so we must not arm the record for them.
    NSEvent *cur = [NSApp currentEvent];
    if (cur && [cur type] == NSEventTypeKeyDown && item->shortcut)
    {
        sLastMenuKeyEquivalent.pending = true;
        sLastMenuKeyEquivalent.keyCode = item->shortcut->keyCode;
        sLastMenuKeyEquivalent.shift   = item->shortcut->isShift;
        sLastMenuKeyEquivalent.alt     = item->shortcut->isAlt;
        sLastMenuKeyEquivalent.control = item->shortcut->isControl;
        sLastMenuKeyEquivalent.super   = item->shortcut->isSuper;
    }

    item->action();
}
@end

// NSMenuItem.target is @property(weak), so the action target must be held
// by a strong owner independent of any C++ class member (which is __unsafe_unretained
// under ARC in C++ objects). A file-scope static is always __strong under ARC.
static CMTMenuActionTarget *sSharedActionTarget = nil;

// Helper: convert SDL key code to NSMenuItem keyEquivalent string
static NSString *SDLKeyToNSString(i32 keyCode)
{
    // Printable ASCII — space, digits, letters, and every standard
    // punctuation key (,.'/;=-[]\`) — maps directly to its character as an
    // NSMenuItem key equivalent. SDL keycodes for these all equal their
    // ASCII value.
    if (keyCode >= 0x20 && keyCode <= 0x7E)
    {
        char ch[2] = { (char)(keyCode), 0 };
        return [NSString stringWithUTF8String:ch];
    }
    if (keyCode >= SDLK_F1 && keyCode <= SDLK_F12)
    {
        unichar fKey = NSF1FunctionKey + (keyCode - SDLK_F1);
        return [NSString stringWithCharacters:&fKey length:1];
    }
    // SDL's BACKSPACE (0x08) is the Mac keyboard's "delete" key, whose
    // NSMenuItem key-equivalent character is NSDeleteCharacter (0x7F), not
    // its own SDL keycode value.
    if (keyCode == SDLK_BACKSPACE)
    {
        unichar delKey = NSDeleteCharacter;
        return [NSString stringWithCharacters:&delKey length:1];
    }
    return @"";
}

// Function keys are only recognized as a real key equivalent by AppKit when
// NSEventModifierFlagFunction is set on the item's modifier mask.
static bool SDLKeyIsFunctionKey(i32 keyCode)
{
    return keyCode >= SDLK_F1 && keyCode <= SDLK_F12;
}

// Helper: map MTEngineSDL modifier flags to NSEventModifierFlags
// In MTEngineSDL macOS: isControl = Cmd key (PLATFORM_STR_KEY_CTRL = "Cmd")
static NSEventModifierFlags ShortcutToModMask(CSlrKeyboardShortcut *sc)
{
    NSEventModifierFlags mask = 0;
    if (sc->isControl) mask |= NSEventModifierFlagCommand;
    if (sc->isShift)   mask |= NSEventModifierFlagShift;
    if (sc->isAlt)     mask |= NSEventModifierFlagOption;
    if (sc->isSuper)   mask |= NSEventModifierFlagControl;
    if (SDLKeyIsFunctionKey(sc->keyCode)) mask |= NSEventModifierFlagFunction;
    return mask;
}

// CMTMenuItemMacOS

void CMTMenuItemMacOS::SetLabel(const char *lbl)
{
    label = lbl;
    if (nsItem)
        [nsItem setTitle:[NSString stringWithUTF8String:lbl]];
}

void CMTMenuItemMacOS::SetEnabled(bool v)
{
    enabled = v;
    if (nsItem)
        [nsItem setEnabled:v ? YES : NO];
}

void CMTMenuItemMacOS::SetChecked(bool v)
{
    checked = v;
    if (nsItem)
        [nsItem setState:v ? NSControlStateValueOn : NSControlStateValueOff];
}

void CMTMenuItemMacOS::SetVisible(bool v)
{
    visible = v;
    if (nsItem)
        [nsItem setHidden:v ? NO : YES];
}

// CMTMenuMacOS

CMTMenuMacOS::CMTMenuMacOS(const char *lbl, NSMenu *menu, NSMenuItem *parent,
                            CMTMenuActionTarget *target_)
{
    label = lbl;
    nsMenu = menu;
    nsParentItem = parent;
    sharedTarget = target_;
}

CMTMenuMacOS::~CMTMenuMacOS()
{
    for (auto *item : items) delete item;
    for (auto *sub : subMenus) delete sub;
}

CMTMenuItem *CMTMenuMacOS::AddItem(const char *lbl, std::function<void()> action_fn,
                                    CSlrKeyboardShortcut *sc)
{
    auto *item = new CMTMenuItemMacOS();
    item->label    = lbl;
    item->action   = action_fn;
    item->shortcut = sc;

    NSString *title  = [NSString stringWithUTF8String:lbl];
    NSString *keyEq  = sc ? SDLKeyToNSString(sc->keyCode) : @"";
    NSEventModifierFlags modMask = sc ? ShortcutToModMask(sc) : NSEventModifierFlagCommand;

    NSMenuItem *nsItem = [[NSMenuItem alloc]
        initWithTitle:title
               action:@selector(menuItemFired:)
        keyEquivalent:keyEq];
    [nsItem setKeyEquivalentModifierMask:modMask];
    [nsItem setTarget:sharedTarget];
    // Store raw C++ pointer as represented object for the action callback
    [nsItem setRepresentedObject:[NSValue valueWithPointer:item]];

    item->nsItem       = nsItem;
    item->actionTarget = sharedTarget;

    [nsMenu addItem:nsItem];
    items.push_back(item);
    return item;
}

CMTMenuItem *CMTMenuMacOS::AddSeparator()
{
    auto *item = new CMTMenuItemMacOS();
    item->isSeparator = true;
    [nsMenu addItem:[NSMenuItem separatorItem]];
    items.push_back(item);
    return item;
}

CMTMenu *CMTMenuMacOS::AddSubMenu(const char *lbl)
{
    NSString   *title  = [NSString stringWithUTF8String:lbl];
    NSMenu     *subNS  = [[NSMenu alloc] initWithTitle:title];
    NSMenuItem *parent = [[NSMenuItem alloc] initWithTitle:title
                                                    action:nil
                                             keyEquivalent:@""];
    [parent setSubmenu:subNS];
    [nsMenu addItem:parent];

    auto *sub = new CMTMenuMacOS(lbl, subNS, parent, sharedTarget);
    subMenus.push_back(sub);
    return sub;
}

void CMTMenuMacOS::RemoveItem(CMTMenuItem *item)
{
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        if (*it == item)
        {
            CMTMenuItemMacOS *macItem = *it;
            if (!macItem->isSeparator && macItem->nsItem)
                [nsMenu removeItem:macItem->nsItem];
            delete macItem;
            items.erase(it);
            return;
        }
    }
}

void CMTMenuMacOS::RemoveAllItems()
{
    [nsMenu removeAllItems];
    for (auto *item : items) delete item;
    items.clear();
    for (auto *sub : subMenus) delete sub;
    subMenus.clear();
}

// CMTNativeMenuBarMacOS

CMTNativeMenuBarMacOS::CMTNativeMenuBarMacOS()
{
    nsMainMenu = [[NSApplication sharedApplication] mainMenu];
    if (!sSharedActionTarget)
        sSharedActionTarget = [[CMTMenuActionTarget alloc] init];
    target = sSharedActionTarget;

    // Strip SDL's auto-generated menus (keep only index 0: the Apple/app menu)
    while ([nsMainMenu numberOfItems] > 1)
        [nsMainMenu removeItemAtIndex:1];

    LOGD("CMTNativeMenuBarMacOS: created, mainMenu items=%d", (int)[nsMainMenu numberOfItems]);
}

CMTNativeMenuBarMacOS::~CMTNativeMenuBarMacOS()
{
    for (auto *menu : menus) delete menu;
}

CMTMenu *CMTNativeMenuBarMacOS::AddMenu(const char *lbl)
{
    NSString   *title  = [NSString stringWithUTF8String:lbl];
    NSMenu     *subNS  = [[NSMenu alloc] initWithTitle:title];
    NSMenuItem *parent = [[NSMenuItem alloc] initWithTitle:title
                                                    action:nil
                                             keyEquivalent:@""];
    [parent setSubmenu:subNS];
    [nsMainMenu addItem:parent];

    auto *menu = new CMTMenuMacOS(lbl, subNS, parent, target);
    menus.push_back(menu);
    return menu;
}

bool CMTNativeMenuBarMacOS::WasKeyHandledByNativeMenu(unsigned int sdlKeyCode,
                                                      bool shift, bool alt,
                                                      bool control, bool super)
{
    if (sLastMenuKeyEquivalent.pending
        && sLastMenuKeyEquivalent.keyCode == (int)sdlKeyCode
        && sLastMenuKeyEquivalent.shift   == shift
        && sLastMenuKeyEquivalent.alt     == alt
        && sLastMenuKeyEquivalent.control == control
        && sLastMenuKeyEquivalent.super   == super)
    {
        sLastMenuKeyEquivalent.pending = false;
        return true;
    }
    return false;
}

void CMTNativeMenuBarMacOS::RemoveMenu(CMTMenu *menu)
{
    for (auto it = menus.begin(); it != menus.end(); ++it)
    {
        if (*it == menu)
        {
            CMTMenuMacOS *macMenu = *it;
            if (macMenu->nsParentItem)
                [nsMainMenu removeItem:macMenu->nsParentItem];
            delete macMenu;
            menus.erase(it);
            return;
        }
    }
}
