#ifndef _MACOSWRAPPER_H_
#define _MACOSWRAPPER_H_

struct SDL_Window;
enum VID_SystemAppearance : int;
enum VID_DisplayColorGamut : int;

void MACOS_StoreMainWindowPosition();
void MACOS_RestoreMainWindowPosition();
VID_SystemAppearance MACOS_GetSystemAppearance();
void MACOS_ApplyWindowAppearance(SDL_Window *sdlWindow, VID_SystemAppearance appearance);
VID_DisplayColorGamut MACOS_GetMainDisplayColorGamut(SDL_Window *sdlWindow);
void MACOS_ApplyWindowColorGamut(SDL_Window *sdlWindow, VID_DisplayColorGamut gamut);

// The largest POTENTIAL EDR headroom any attached display can grant, or 1.0
// when none can.
//
// POTENTIAL, not current, and the distinction decides whether HDR ever turns
// on: the CURRENT value reads 1.0 until EDR content is actually on screen and
// then ramps over ~1.75 s (measured, spikes/edr/README.md), so probing the
// current value at init would conclude "no HDR display" on every HDR machine.
// The potential value is what the display COULD grant, and it is stable from
// the moment the display is attached.
float MACOS_GetMaxPotentialHdrHeadroom();

#endif
