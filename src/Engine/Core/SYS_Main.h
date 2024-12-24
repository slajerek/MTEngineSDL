/*
 *  SYS_Main.h
 *
 *  Created by Marcin Skoczylas on 09-11-19.
 *  Licensed under MIT
 *
 */

#ifndef __SYS_MAIN_H__
#define __SYS_MAIN_H__

#include "SYS_Defs.h"
#include "DBG_Log.h"
#include "SYS_Funct.h"

unsigned long SYS_GetCurrentTimeInMillis();

void SYS_Shutdown();

void SYS_FatalExit();
void SYS_FatalExit(char *fmt, ... );
void SYS_FatalExit(const char *fmt, ... );

void SYS_CleanExit();
void SYS_CleanExit(char *fmt, ... );
void SYS_CleanExit(const char *fmt, ... );

// Note, this should be implemented per platform in SYS_Platform
void SYS_RestartApplication();

void SYS_Print(const char *format, ...);
void SYS_PrintError(const char *format, ...);

//#if !defined(FINAL_RELEASE)
void SYS_Assert(bool condition, const char *fmt, ...);
void SYS_AssertCrash(const char *fmt, ...);
//#else
//#define SYS_Assert //
//#define SYS_AssertCrash //
//#endif

void SYS_AssertCrashInRelease(const char *fmt, ...);

#define GET_CHARBUF(bufname) char *bufname = SYS_GetCharBuf();
#define REL_CHARBUF(bufname) SYS_ReleaseCharBuf(bufname); bufname = NULL;
#define RELEASE_CHARBUF(bufname) SYS_ReleaseCharBuf(bufname); bufname = NULL;

void SYS_InitCharBufPool();
char *SYS_GetCharBuf();
void SYS_ReleaseCharBuf(char *buf);

u32 SYS_GetBareKey(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
u32 SYS_GetShiftedKey(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

char *SYS_GetCurrentDateTimeString();

long SYS_GetTickCount();

void Byte2Hex1digitR(u8 value, char *bufOut);
void Byte2Hex2digits(u8 value, char *bufOut);
uint8 Bits2Byte(char *bufIn);
void Byte2Bits(u8 value, char *bufOut);
void Byte2BitsWithoutEndingZero(u8 value, char *bufOut);
u32 GetHashCode32(const char *text);
u64 GetHashCode64(const char *text);

#define BV00		(1 <<  0)
#define BV01		(1 <<  1)
#define BV02		(1 <<  2)
#define BV03		(1 <<  3)
#define BV04		(1 <<  4)
#define BV05		(1 <<  5)
#define BV06		(1 <<  6)
#define BV07		(1 <<  7)
#define BV08		(1 <<  8)
#define BV09		(1 <<  9)
#define BV10		(1 << 10)
#define BV11		(1 << 11)
#define BV12		(1 << 12)
#define BV13		(1 << 13)
#define BV14		(1 << 14)
#define BV15		(1 << 15)
#define BV16		(1 << 16)
#define BV17		(1 << 17)
#define BV18		(1 << 18)
#define BV19		(1 << 19)
#define BV20		(1 << 20)
#define BV21		(1 << 21)
#define BV22		(1 << 22)
#define BV23		(1 << 23)
#define BV24		(1 << 24)
#define BV25		(1 << 25)
#define BV26		(1 << 26)
#define BV27		(1 << 27)
#define BV28		(1 << 28)
#define BV29		(1 << 29)
#define BV30		(1 << 30)
#define BV31		(1 << 31)

#define IS_SET(flag, bit)	((flag) & (bit))
#define SET_BIT(var, bit)	((var) |= (bit))
#define REMOVE_BIT(var, bit)	((var) &= ~(bit))
#define TOGGLE_BIT(var, bit)	((var) ^= (bit))

void *SYS_Malloc(size_t size);
void *SYS_MallocNoCheck(size_t size);
void SYS_Free(void **ptr);
char *SYS_StrAlloc(char *str);
void SYS_StrFree(char **str);

/*
#define STRALLOC(point)         SYS_StrAlloc((point))
#define STRFREE(point)                                          \
do                                                              \
{                                                               \
if (!(point))                                                   \
{                                                               \
LOGError("STRFREEing NULL in %s, line %d\n", __FILE__, __LINE__ ); \
}                                                               \
else SYS_StrFree(&(point));                                             \
} while(0)
*/

void LOG_PrintHexArray(u8 *data, int size);

const char *SYS_GetPlatformNameString();
const char *SYS_GetPlatformArchitectureString();

#endif // __SYS_MAIN_H__
