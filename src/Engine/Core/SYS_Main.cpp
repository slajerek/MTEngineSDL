/*
 *  SYS_Main.cpp
 *
 *  Created by Marcin Skoczylas on 09-11-19.
 *
 */

#include "SYS_Main.h"
#include <stdlib.h>
#include "DBG_Log.h"
#include "VID_Main.h"
#include "CGlobalLogicCallback.h"
#include "SYS_Threading.h"
#include <list>
#include <filesystem>

#if !defined(WIN32)
#include <sys/time.h>
#endif

#define ASSERT_CHECK_ReleaseCharBuf
#define SAFEMALLOC_USEMARKER
#define SAFEMALLOC_MARKER ((unsigned int) 0x4400DEAD)

// remove me:
namespace fs = std::filesystem;

void testFilesystem()
{
	const std::filesystem::path correct_path = u8"\xe4\xb8\xad";
	// deprecated:
	//	const std::string path_as_string = "\xe4\xb8\xad";
	//	const std::filesystem::path correct_path = std::filesystem::u8path(path_as_string);
}


unsigned long SYS_GetCurrentTimeInMillis()
{
	return VID_GetTickCount();
}

void SYS_NotImplemented()
{
	SYS_FatalExit("TODO: NOT IMPLEMENTED");
}

void SYS_FatalExit(char *fmt, ... )
{
	LOGError("SYS_FatalExit:");
	char buffer[4096] = {0};

    va_list args;

    va_start(args, fmt);
	vsnprintf(buffer, 4096, fmt, args);
    va_end(args);

	LOGError(buffer);
	
	abort();
}

void SYS_FatalExit(const char *fmt, ... )
{
	LOGError("SYS_FatalExit:");
	char buffer[4096] = {0};

    va_list args;

    va_start(args, fmt);
	vsnprintf(buffer, 4096, fmt, args);
    va_end(args);

	LOGError(buffer);

	abort();
}

void SYS_FatalExit()
{
	LOGError("SYS_FatalExit()");

	abort();
}

/////////////
void SYS_ShowError(char *fmt, ... )
{
	char buffer[4096] = {0};
	
	va_list args;
	
	va_start(args, fmt);
	vsnprintf(buffer, 4096, fmt, args);
	va_end(args);
	
	LOGError(buffer);
}

void SYS_ShowError(const char *fmt, ... )
{
	char buffer[4096] = {0};
	
	va_list args;
	
	va_start(args, fmt);
	vsnprintf(buffer, 4096, fmt, args);
	va_end(args);
	
	LOGError(buffer);
}

void SYS_CleanExit(char *fmt, ... )
{
	LOGM("SYS_CleanExit:");
	char buffer[4096] = {0};
	
    va_list args;
	
    va_start(args, fmt);
	vsnprintf(buffer, 4096, fmt, args);
    va_end(args);
	
	LOGError(buffer);
	
	exit(0);
}

void SYS_CleanExit(const char *fmt, ... )
{
	LOGM("SYS_CleanExit:");
	char buffer[4096] = {0};
	
    va_list args;
	
    va_start(args, fmt);
	vsnprintf(buffer, 4096, fmt, args);
    va_end(args);
	
	LOGError(buffer);
	
	exit(0);
}

void SYS_CleanExit()
{
	LOGM("SYS_CleanExit()");
	exit(0);
}

////////////

void SYS_Assert(bool condition, const char *fmt, ...)
{
	if (condition == false)
	{
		LOGError("Assert failed:");
		char buffer[4096] = {0};
		
		va_list args;
		
		va_start(args, fmt);
		vsnprintf(buffer, 4096, fmt, args);
		va_end(args);
		
		LOGError(buffer);
		
		abort();
	}
}

void SYS_AssertCrash(const char *fmt, ...)
{
	LOGError("Assert failed:");
	char buffer[4096] = {0};
	
	va_list args;
	
	va_start(args, fmt);
	vsnprintf(buffer, 4096, fmt, args);
	va_end(args);
	
	LOGError(buffer);
	
	
	//abort();
	
}

//#define SAFEMALLOC_MARKER ((unsigned int) 0x4400DEAD)
//#define SAFEMALLOC_USEMARKER

// safe malloc and free wrappers, with optional marker verify (4 byte cost)
void *SYS_Malloc(size_t size)
{

#ifdef SAFEMALLOC_USEMARKER
	size += 4;
#endif

	void *res = malloc(size);
	if (!res)
		LOGError("SYS_MALLOC: cannot allocate %d bytes - out of memory?", size);

#ifdef SAFEMALLOC_USEMARKER

	*((long *)res) = SAFEMALLOC_MARKER;
	return((long *)res+1);

#else

	return(res);

#endif

}

void *SYS_MallocNoCheck(size_t size)
{

#ifdef SAFEMALLOC_USEMARKER
	size += 4;
#endif

	void *res = malloc(size);

#ifdef SAFEMALLOC_USEMARKER

	*((long *)res) = SAFEMALLOC_MARKER;
	return((long *)res+1);

#else

	return(res);

#endif

}

void SYS_Free(void **ptr)
{
	long *p;

	if (!(*ptr))
		LOGError("SYS_FREE: freeing null");

	p = (long *)*ptr;

#ifdef SAFEMALLOC_USEMARKER

	p -= 1;
	if (*p != SAFEMALLOC_MARKER)
	{
		if (*p == (SAFEMALLOC_MARKER ^ 0xFFFFFFFF))
		{
			LOGError("SYS_FREE: freeing pointer twice");
		}
		else
		{
			LOGError("SYS_FREE: freeing not alloced pointer");
		}
	}
	*p = SAFEMALLOC_MARKER ^ 0xFFFFFFFF;

#endif

	free(p);
	*ptr = NULL;

}

char *SYS_StrAlloc(char *str)
{
	size_t len = strlen(str);
	char *ret = (char*)SYS_Malloc(len+1);
	strcpy(ret, str);
	return ret;
}

void SYS_StrFree(char **str)
{
	SYS_Free((void**)str);
}

std::list<char *> charBufsPool;

CSlrMutex *charBufsMutex;

bool _sysCharBufsInit = false;

void SYS_InitCharBufPool()
{
	if (_sysCharBufsInit == false)
	{
		charBufsMutex = new CSlrMutex("charBufsMutex");
		_sysCharBufsInit = true;
	}
}

char *SYS_GetCharBuf()
{
	char *buf = NULL;

	charBufsMutex->Lock();
	if (charBufsPool.empty())
	{
		buf = new char[MAX_STRING_LENGTH];
	}
	else
	{
		buf = charBufsPool.front();
		charBufsPool.pop_front();
	}
	charBufsMutex->Unlock();

	buf[0] = 0x00;
	return buf;
}

void SYS_ReleaseCharBuf(char *buf)
{
	charBufsMutex->Lock();
	
#if defined(ASSERT_CHECK_ReleaseCharBuf)
	for (std::list<char *>::iterator it = charBufsPool.begin(); it != charBufsPool.end(); it++)
	{
		char *b = *it;
		if (b == buf)
		{
			LOGError("SYS_ReleaseCharBuf: freeing twice %x", buf);
			LOGError("SYS_ReleaseCharBuf: %x = '%s'", buf, buf);
			SYS_FatalExit("SYS_ReleaseCharBuf failed: releasing twice");
			charBufsMutex->Unlock();
			return;
		}
	}
	
	// check sanity of the buffer, i.e. check case when not our buffer is being released
	for (int i = 0; i < MAX_STRING_LENGTH; i++)
	{
		buf[i] = 0;
	}
#endif
	charBufsPool.push_back(buf);
	charBufsMutex->Unlock();
}

// get bare key code (e.g. not shifted)
// NOTE: this does not reflect the keyboard layout. Shitty SDL2 is too proud to do it right:
// https://discourse.libsdl.org/t/sdl-2-how-to-read-shift-x-keys/21238

u32 SYS_GetBareKey(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	//LOGD("SYS_GetBareKey: key=%d", keyCode);
	
	if (keyCode >= 'A' && keyCode <= 'Z')
	{
		return keyCode + 0x20;
	}
	
	// de-translate shifted keys
	switch(keyCode)
	{
		case 33: keyCode = '1'; break;
		case 64: keyCode = '2'; break;
		case 35: keyCode = '3'; break;
		case 36: keyCode = '4'; break;
		case 37: keyCode = '5'; break;
		case 94: keyCode = '6'; break;
		case 38: keyCode = '7'; break;
		case 42: keyCode = '8'; break;
		case 40: keyCode = '9'; break;
		case 41: keyCode = '0'; break;
		case 95: keyCode = '-'; break;
		case 43: keyCode = '='; break;
		case 58: keyCode = ';'; break;
		case 34: keyCode = '\''; break;
		case 60: keyCode = ','; break;
		case 62: keyCode = '.'; break;
		case 63: keyCode = '/'; break;
	}
	
	return keyCode;
}

u32 SYS_GetShiftedKey(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	//LOGD("SYS_GetShiftedKey: key=%d", keyCode);
	if (isShift)
	{
		switch(keyCode)
		{
			case '1':
				return '!';
			case '2':
				return '@';
			case '3':
				return '#';
			case '4':
				return '$';
			case '5':
				return '%';
			case '6':
				return '^';
			case '7':
				return '&';
			case '8':
				return '*';
			case '9':
				return '(';
			case '0':
				return ')';
			case '-':
				return '_';
			case '=':
				return '+';
			case ';':
				return ':';
			case '\'':
				return '"';
			case ',':
				return '<';
			case '.':
				return '>';
			case '/':
				return '?';
			case '`':
				return '~';
			case '[':
				return '{';
			case ']':
				return '}';
			case '\\':
				return '|';
		}
		
		if (keyCode < SDLK_CAPSLOCK) //1073741881)
		{
			return toupper(keyCode);
		}
		else
		{
			return keyCode;
		}
		
	}
	
	return keyCode;
}

char *SYS_GetCurrentDateTimeString()
{
	char *buf = new char[32];

#if !defined(WIN32)

	struct timeval  tv;
	struct timezone tz;
	struct tm      *tm;
	
	gettimeofday(&tv, &tz);
	tm = localtime(&tv.tv_sec);
	
	//unsigned int ms = (unsigned int)tv.tv_usec/(unsigned int)10000;
		
	sprintf(buf, "%02d/%02d/%02d %02d:%02d",
			(tm->tm_year - 100), tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min);
	
#else

	SYSTEMTIME tmeCurrent;
	GetLocalTime(&tmeCurrent);

	sprintf(buf, "%02d/%02d/%02d %02d:%02d",
			tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay, tmeCurrent.wHour, tmeCurrent.wMinute);
#endif

	return buf;
}

long SYS_GetTickCount()
{
	return VID_GetTickCount();
//	timeval ts;
//	gettimeofday(&ts,0);
//	return (long)(ts.tv_sec * 1000 + (ts.tv_usec / 1000));
}

static const char *hexTable = "0123456789ABCDEF"; //"0123456789abcdef";

void Byte2Hex1digitR(u8 value, char *bufOut)
{
	unsigned char c2;
	
	c2 = (unsigned char)(value & 0x0F);
	bufOut[0] = (unsigned char)hexTable[c2];
	
}

void Byte2Hex2digits(u8 value, char *bufOut)
{
	unsigned char c1;
	unsigned char c2;
	
	c1 = (unsigned char)(value & 0xF0);
	c1 = (unsigned char)(value >> 4);
	
	c2 = (unsigned char)(value & 0x0F);
	
	bufOut[0] = (unsigned char)hexTable[c1];
	bufOut[1] = (unsigned char)hexTable[c2];
	
}

uint8 Bits2Byte(char *bufIn)
{
	uint8 value = 0x00;
	for (int i = 0; i < 8; i++)
	{
		uint8 b = bufIn[7-i] == '0' ? 0 : 1;
		value |= (b << i);
	}
	return value;
}

void Byte2Bits(u8 value, char *bufOut)
{
	for (int i = 7; i >= 0; i -= 1)
	{
		bufOut[i] = '0' + (value & 0x01);
		value >>= 1;
	}
	
	bufOut[8] = 0x00;
}

void Byte2BitsWithoutEndingZero(u8 value, char *bufOut)
{
	for (int i = 7; i >= 0; i -= 1)
	{
		bufOut[i] = '0' + (value & 0x01);
		value >>= 1;
	}
}

u32 GetHashCode32(const char *text)
{
	unsigned long length = strlen(text);
	unsigned int retVal = 0x00000000;
	unsigned int sum = 0;
	
	u8 step = 0;
	for (int i = 0; i < length; i++)
	{
		char c = text[i];
		sum += i*c;
		
		if (step == 0)
		{
			retVal ^= c;
			step++;
		}
		else if (step == 1)
		{
			retVal ^= ((c << 8) & 0x0000FF00);
			step++;
		}
		else if (step == 2)
		{
			retVal ^= ((c << 16) & 0x00FF0000);
			step++;
		}
		else if (step == 3)
		{
			retVal ^= ((c << 24) & 0xFF000000);
			step = 0;
		}
	}
	
	retVal ^= sum;
	
	//LOGD("=========> GetHashCode from '%s'=%8.8x", text, retVal);
	return retVal;
}

u64 GetHashCode64(const char *text)
{
	unsigned long length = strlen(text);
	u64 retVal = 0x0000000000000000;
	u64 sum = 0;
	
	u8 step = 0;
	for (int i = 0; i < length; i++)
	{
		u64 c = text[i];
		sum += i*c;
		
		if (step == 0)
		{
			retVal ^= c;		// 0x00000000000000FF
			step++;
		}
		else if (step == 1)
		{
			retVal ^= ((c << 8)  & 0x000000000000FF00);
			step++;
		}
		else if (step == 2)
		{
			retVal ^= ((c << 16) & 0x0000000000FF0000);
			step++;
		}
		else if (step == 3)
		{
			retVal ^= ((c << 24) & 0x00000000FF000000);
			step = 4;
		}
		else if (step == 4)
		{
			retVal ^= ((c << 32) & 0x000000FF00000000);
			step = 5;
		}
		else if (step == 5)
		{
			retVal ^= ((c << 40) & 0x0000FF0000000000);
			step = 6;
		}
		else if (step == 6)
		{
			retVal ^= ((c << 48) & 0x00FF000000000000);
			step = 7;
		}
		else if (step == 7)
		{
			retVal ^= ((c << 56) & 0xFF00000000000000);
			step = 0;
		}
	}
	
	retVal ^= sum;
	
	//LOGD("=========> GetHashCode64 from '%s'=%lld", text, retVal);
	return retVal;
}

void SYS_Print(const char *format, ...)
{
	char *buffer = SYS_GetCharBuf();
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, MAX_STRING_LENGTH, format, args);
	va_end(args);
	SYS_ReleaseCharBuf(buffer);
	
	fprintf(stdout, "%s\n", buffer);
}

void SYS_PrintError(const char *format, ...)
{
	char *buffer = SYS_GetCharBuf();
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, MAX_STRING_LENGTH, format, args);
	va_end(args);
	SYS_ReleaseCharBuf(buffer);

	fprintf(stderr, "%s\n", buffer);
}

void LOG_PrintHexArray(u8 *data, int size)
{
	char *buf = SYS_GetCharBuf();
	char hex[4] = {0,0,0x20,0};
	
	for (int i = 0; i < size; i++)
	{
		sprintfHexCode8(hex, data[i]);
		strcat(buf, hex);
	}
	
	LOGD("%s", buf);
	
	SYS_ReleaseCharBuf(buf);
}

const char* SYS_GetPlatformNameString() {
#if defined(_WIN32) || defined(_WIN64)
	return "Windows";
#elif defined(__linux__)
#ifdef __ANDROID__
	return "Android";
#else
	return "Linux";
#endif
#elif defined(__APPLE__) || defined(__MACH__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
	return "iOS";
#elif TARGET_OS_MAC
	return "macOS";
#else
	return "Unknown Apple platform";
#endif
#else
	return "Unknown platform";
#endif
}

const char *SYS_GetPlatformArchitectureString() { //Get current architecture, detectx nearly every architecture. Coded by Freak
	#if defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
	#elif defined(i386) || defined(__i386__) || defined(__i386) || defined(_M_IX86)
	return "x86_32";
	#elif defined(__ARM_ARCH_2__)
	return "ARM2";
	#elif defined(__ARM_ARCH_3__) || defined(__ARM_ARCH_3M__)
	return "ARM3";
	#elif defined(__ARM_ARCH_4T__) || defined(__TARGET_ARM_4T)
	return "ARM4T";
	#elif defined(__ARM_ARCH_5_) || defined(__ARM_ARCH_5E_)
	return "ARM5"
	#elif defined(__ARM_ARCH_6T2_) || defined(__ARM_ARCH_6T2_)
	return "ARM6T2";
	#elif defined(__ARM_ARCH_6__) || defined(__ARM_ARCH_6J__) || defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__)
	return "ARM6";
	#elif defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__)
	return "ARM7";
	#elif defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__)
	return "ARM7A";
	#elif defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__)
	return "ARM7R";
	#elif defined(__ARM_ARCH_7M__)
	return "ARM7M";
	#elif defined(__ARM_ARCH_7S__)
	return "ARM7S";
	#elif defined(__aarch64__) || defined(_M_ARM64)
	return "ARM64";
	#elif defined(mips) || defined(__mips__) || defined(__mips)
	return "MIPS";
	#elif defined(__sh__)
	return "SUPERH";
	#elif defined(__powerpc) || defined(__powerpc__) || defined(__powerpc64__) || defined(__POWERPC__) || defined(__ppc__) || defined(__PPC__) || defined(_ARCH_PPC)
	return "POWERPC";
	#elif defined(__PPC64__) || defined(__ppc64__) || defined(_ARCH_PPC64)
	return "POWERPC64";
	#elif defined(__sparc__) || defined(__sparc)
	return "SPARC";
	#elif defined(__m68k__)
	return "M68K";
	#else
	return "UNKNOWN";
	#endif
}
