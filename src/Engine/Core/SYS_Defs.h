/*
 *  Created by Marcin Skoczylas on 09-11-19.
 *  (c) 2009 Marcin Skoczylas, Licensed under MIT
 */

#ifndef _SYS_DEFS_H_
#define _SYS_DEFS_H_

#define APPLICATION_BUNDLE_NAME "MTEngineSDL"

#define MAX_STRING_LENGTH 8192

#define OPENCV_TRAITS_ENABLE_DEPRECATED

// do not show any UI
//#define MODE_HEADLESS

#define FINAL_RELEASE
#define INIT_DEFAULT_UI_THEME
#define LOAD_CONSOLE_FONT
#define LOAD_CONSOLE_INVERTED_FONT
#define LOAD_DEFAULT_FONT
//#define LOAD_AND_BLIT_ZOOM_SIGN

// common library headers
extern "C"	
{
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#ifndef WIN32
#include <stddef.h>
#endif
#include <string.h>
}

#ifndef WIN32
#include <math.h>
#endif

#if defined(MACOS) || defined(LINUX)
#include <unistd.h>
#endif

#include <iostream>

#if defined(WIN32)
#pragma warning( disable : 4100) //unreferenced parameters
#pragma warning( disable : 4996) //security warnings, strcpy and such

//silences warnings about "nonstandard" function
#define strdup _strdup

#include <windows.h>		// Header File For Windows
#include <windowsx.h>		// Header File For Windows

extern HWND	hWnd;

#define HWND_CLASS_NAME "SDL_app" 

#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif

typedef signed char                 I8;
typedef unsigned char               U8;
typedef short                       I16;
typedef unsigned short              U16;
typedef int                         I32;
typedef unsigned int                U32;
typedef unsigned long long          U64;
typedef long long                   I64;


typedef signed char                 i8;
typedef unsigned char               u8;
typedef short                       i16;
typedef unsigned short              u16;
typedef int                         i32;
typedef unsigned int                u32;
typedef unsigned long long          u64;
typedef long long                   i64;


typedef signed char                 int8;
typedef unsigned char               uint8;
typedef short                       int16;
typedef unsigned short              uint16;
typedef int                         int32;
typedef unsigned int                uint32;

/*
#ifndef LINUX_ALEK
typedef unsigned long long          uint64;
typedef long long                   int64;
#endif
*/

#define MATH_PI (double)(3.1415926535897932384626433832795)

#define GRAVITY_G (9.80665f)

#define DEGTORAD 0.0174532925199432957f
#define RADTODEG 57.295779513082320876f

#ifdef __APPLE__
#include "TargetConditionals.h"
#endif

#if defined(MACOS)
#define DESKTOP
#endif

#if defined(WIN32)
#define DESKTOP
#endif

#define STRBOOL(fvaluebool) ( (fvaluebool) ? "true" : "false" )

#ifndef PATH_MAX
#  ifdef MAX_PATH
#    define PATH_MAX MAX_PATH
#  else
#    define PATH_MAX 1024
#  endif
#endif

///
//#define EXEC_ON_VALGRIND
#define USE_THREADED_IMAGES_LOADING

// 2^16-1
#define MAX_NUM_FRAMES 2147483640

#define MAX_MULTI_TOUCHES       20

#define PLATFORM_TYPE_UNKNOWN   0
#define PLATFORM_TYPE_DESKTOP   1
#define PLATFORM_TYPE_PHONE             2
#define PLATFORM_TYPE_TABLET    3

//
#define MT_PRIORITY_IDLE			0
#define MT_PRIORITY_BELOW_NORMAL	1
#define MT_PRIORITY_NORMAL			2
#define MT_PRIORITY_ABOVE_NORMAL	3
#define MT_PRIORITY_HIGH_PRIORITY	4

#include "stb_sprintf.h"

#endif
