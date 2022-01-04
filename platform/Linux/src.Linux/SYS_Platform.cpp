#include "DBG_Log.h"
#include "SYS_Platform.h"
#include "SYS_Main.h"
#include <SDL.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

Display *dpy;
Cursor cur;
bool isCursorDefined = false;
bool isCursorVisible = true;

void SYS_PlatformInit()
{
 dpy = XOpenDisplay(NULL);
 if (dpy == NULL) {
  SYS_FatalExit("cannot connect to X server");
 }

 XSynchronize(dpy, true);

/* 
	Pixmap bitmap;
	XColor color;
	char noData[] = { 0 };
  
	bitmap = XCreateBitmapFromData(dpy, win, noData, 1, 1);
	cur = XCreatePixmapCursor(dpy, bitmap, bitmap, &color, &color, 0, 0);
	XFreePixmap(dpy, bitmap);  	
	*/
}

void SYS_PlatformShutdown()
{	
}

void SYS_AttachConsole()
{
}

bool VID_IsMouseCursorVisible()
{
//    if (SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE)

	return isCursorVisible;
}

void X11ShowCursor(bool isVisible)
{
        if (isVisible == false)
        {
                isCursorDefined = true;
//                XDefineCursor(dpy, win, cur);
        }
        else
        {
                if (isCursorDefined)
                {
//                   XUndefineCursor(dpy, win);
                }
        }
        isCursorVisible = isVisible;
}

// SDL cursor control does not work
void VID_ShowMouseCursor()
{
        LOGD("VID_ShowMouseCursor");
//      SDL_ShowCursor(SDL_ENABLE);
        X11ShowCursor(true);
}

void VID_HideMouseCursor()
{
        LOGD("VID_HideMouseCursor");
//      SDL_ShowCursor(SDL_DISABLE);
        X11ShowCursor(false);
}


