#ifndef _SYS_PLATFORM_H_
#define _SYS_PLATFORM_H_

void SYS_PlatformInit();
void SYS_PlatformShutdown();

void PLATFORM_SetThreadName(const char *name);
void PLATFORM_UpdateMenus();

void SYS_AttachConsole();
void SYS_AttachWindowsConsoleToStdOutIfNotRedirected();
void SYS_DetachWindowsConsole();
void SYS_SetMainProcessPriorityBoostDisabled(bool isPriorityBoostDisabled);
void SYS_SetMainProcessPriority(int priority);

#define PLATFORM_SCANCODE_LCTRL SDL_SCANCODE_LCTRL
#define PLATFORM_SCANCODE_RCTRL SDL_SCANCODE_RCTRL
#define PLATFORM_SCANCODE_LGUI  SDL_SCANCODE_LGUI
#define PLATFORM_SCANCODE_RGUI  SDL_SCANCODE_RGUI

#define PLATFORM_KMOD_CTRL      KMOD_CTRL
#define PLATFORM_KMOD_LCTRL     KMOD_LCTRL
#define PLATFORM_KMOD_RCTRL     KMOD_RCTRL
#define PLATFORM_KMOD_GUI       KMOD_GUI
#define PLATFORM_KMOD_LGUI      KMOD_LGUI
#define PLATFORM_KMOD_RGUI      KMOD_RGUI

#define PLATFORM_STR_KEY_CTRL   "Ctrl"
#define PLATFORM_STR_KEY_GUI    "Gui"
#define PLATFORM_STR_UPCASE_KEY_CTRL   "CTRL"
#define PLATFORM_STR_UPCASE_KEY_GUI    "CMD"

#endif
