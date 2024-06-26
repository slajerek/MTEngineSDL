#ifndef _SYS_PLATFORM_H_
#define _SYS_PLATFORM_H_

void SYS_PlatformInit();
void SYS_PlatformShutdown();
void SYS_AttachConsole();

void PLATFORM_SetThreadName(const char *name);
void PLATFORM_UpdateMenus();

bool MACOS_IsApplicationFullScreen();
float MACOS_GetBackingScaleFactor(int screen);

#define PLATFORM_SCANCODE_LCTRL	SDL_SCANCODE_LGUI
#define PLATFORM_SCANCODE_RCTRL	SDL_SCANCODE_RGUI
#define PLATFORM_SCANCODE_LGUI	SDL_SCANCODE_LCTRL
#define PLATFORM_SCANCODE_RGUI	SDL_SCANCODE_RCTRL

#define PLATFORM_KMOD_CTRL      KMOD_GUI
#define PLATFORM_KMOD_LCTRL     KMOD_LGUI
#define PLATFORM_KMOD_RCTRL     KMOD_RGUI
#define PLATFORM_KMOD_GUI       KMOD_CTRL
#define PLATFORM_KMOD_LGUI      KMOD_LCTRL
#define PLATFORM_KMOD_RGUI      KMOD_RCTRL

#define PLATFORM_STR_KEY_CTRL   "Cmd"
#define PLATFORM_STR_KEY_GUI    "Ctrl"
#define PLATFORM_STR_UPCASE_KEY_CTRL   "CMD"
#define PLATFORM_STR_UPCASE_KEY_GUI    "CTRL"

#endif
