#ifndef _GAM_GamePads_H_
#define _GAM_GamePads_H_

#include <SDL.h>
#include "CGamePad.h"
#include "CGamePadAxisMotionToButton.h"

#define MAX_GAMEPADS 4

class CSlrFile;
class CGuiView;

void GAM_InitGamePads();
void GAM_RefreshGamePads();
void GAM_GamePadsEvent(const SDL_Event& event);
CGamePad **GAM_EnumerateGamepads(int *numGamepads);

#endif

