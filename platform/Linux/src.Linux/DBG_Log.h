/*
 *  DBG_Log.h Linux
 *
 *  Created by Marcin Skoczylas on 09-11-19.
 *
 */

#ifndef __DBG_LOGF_H__
#define __DBG_LOGF_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>
#include <type_traits>

// Off by default here, unlike the other platforms -- and that is a real cost
// worth knowing about. With this set EVERY log macro compiles to nothing,
// including the LOGError inside SYS_FatalExit, so a crash on this platform
// prints no diagnostic whatsoever: not one hidden in a file, none written at
// all. That is exactly what happened on 2026-09-03, when a MIDI fault showed up
// in CI as a bare "Segmentation fault" and the cause could only be found by
// commenting this line out for one run and reading the startup trace.
//
// If you are debugging something here and the program is dying silently, this
// line is the first thing to comment out.
#define GLOBAL_DEBUG_OFF

#define DBGLVL_ALL_OFF	0x0000
#define DBGLVL_ALL_ON	0xFFFF
#define DBGLVL_FATAL	(1 << 0)
#define DBGLVL_ERROR	(1 << 1)
#define DBGLVL_WARN		(1 << 2)
#define DBGLVL_GUI		(1 << 3)
#define DBGLVL_INFO		(1 << 4)
#define DBGLVL_MAIN		(DBGLVL_INFO)
#define DBGLVL_DATA		(1 << 5)
#define DBGLVL_CONNECTION	(1 << 6)
#define DBGLVL_HTTP			(DBGLVL_CONNECTION)
#define DBGLVL_DEBUG		(1 << 7)
#define DBGLVL_DATABASE		(1 << 8)
#define DBGLVL_PLUGIN		(1 << 9)
#define DBGLVL_XML			(1 << 10)
#define DBGLVL_RES			(1 << 11)
#define DBGLVL_XMPLAYER		(1 << 12)
#define DBGLVL_AUDIO		(1 << 13)
#define DBGLVL_TODO			(1 << 14)
#define DBGLVL_ANIMATION	(1 << 15)
#define DBGLVL_LEVEL		(1 << 16)
#define DBGLVL_MEMORY		(1 << 17)
#define DBGLVL_SCRIPT		(1 << 18)
#define DBGLVL_NET			(1 << 19)
#define DBGLVL_NET_SERVER	(1 << 20)
#define DBGLVL_NET_CLIENT	(1 << 21)
#define DBGLVL_INPUT		(1 << 22)
#define DBGLVL_VICE_DEBUG	(1 << 23)
#define DBGLVL_VICE_MAIN	(1 << 24)
#define DBGLVL_VICE_VERBOSE	(1 << 25)
#define DBGLVL_ATARI_DEBUG	(1 << 26)
#define DBGLVL_ATARI_MAIN	(1 << 27)
#define DBGLVL_DEBUG2		(1 << 28)

//DBGLVL_CONNECTION

void LOG_Init(void);
bool LOG_IsSetLevel(unsigned int level);
void LOG_SetLevel(unsigned int level, bool isOn);
void LOG_BackupCurrentLogLevel();
void LOG_RestoreBackupLogLevel();
void LOG_SetCurrentLogLevel(int level);
int  LOG_GetCurrentLogLevel();
void LOG_LockMutex();
void LOG_UnlockMutex();
void LOG_Shutdown(void);

#if !defined(GLOBAL_DEBUG_OFF)

#define IS_SET(flag, bit)       ((flag) & (bit))
#define SET_BIT(var, bit)       ((var) |= (bit))
#define REMOVE_BIT(var, bit)    ((var) &= ~(bit))
#define TOGGLE_BIT(var, bit)    ((var) ^= (bit))

#define LOGD(...) _LOGGER_S(DBGLVL_DEBUG, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGVD(...) _LOGGER_S(DBGLVL_VICE_DEBUG, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGD2(...) _LOGGER_S(DBGLVL_DEBUG2, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGM(...) _LOGGER_S(DBGLVL_MAIN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGVM(...) _LOGGER_S(DBGLVL_VICE_MAIN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGVV(...) _LOGGER_S(DBGLVL_VICE_VERBOSE, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGI(...) _LOGGER_S(DBGLVL_INPUT, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGP(...) _LOGGER_S(DBGLVL_PLUGIN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGR(...) _LOGGER_S(DBGLVL_RES, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGG(...) _LOGGER_S(DBGLVL_GUI, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGF(...) _LOGGER_S(DBGLVL_FATAL, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGH(...) _LOGGER_S(DBGLVL_HTTP, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGDATA(...) _LOGGER_S(DBGLVL_DATA, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGA(...) _LOGGER_S(DBGLVL_AUDIO, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGAD(...) _LOGGER_S(DBGLVL_DEBUG, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGL(...) _LOGGER_S(DBGLVL_LEVEL, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGW(...) _LOGGER_S(DBGLVL_CONNECTION, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGX(...) _LOGGER_S(DBGLVL_XMPLAYER, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGN(...) _LOGGER_S(DBGLVL_ANIMATION, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGS(...) _LOGGER_S(DBGLVL_SCRIPT, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGC(...) _LOGGER_S(DBGLVL_NET, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGCS(...) _LOGGER_S(DBGLVL_NET_SERVER, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGCC(...) _LOGGER_S(DBGLVL_NET_CLIENT, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)

#define LOG_Atari_Main(...) _LOGGER_S(DBGLVL_ATARI_MAIN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOG_Atari_Debug(...) _LOGGER_S(DBGLVL_ATARI_DEBUG, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)

#define LOGMEM(...) _LOGGER_S(DBGLVL_MEMORY, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGTODO(...) _LOGGER_S(DBGLVL_TODO, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGWarning(...) _LOGGER_S(DBGLVL_WARN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGError(...) _LOGGER_S(DBGLVL_ERROR, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)

#define LOGND(...) _LOGGER_S(DBGLVL_DEBUG, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNM(...) _LOGGER_S(DBGLVL_MAIN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNP(...) _LOGGER_S(DBGLVL_PLUGIN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNR(...) _LOGGER_S(DBGLVL_RES, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNG(...) _LOGGER_S(DBGLVL_GUI, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNF(...) _LOGGER_S(DBGLVL_FATAL, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNN(...) _LOGGER_S(DBGLVL_ANIMATION, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNH(...) _LOGGER_S(DBGLVL_HTTP, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNA(...) _LOGGER_S(DBGLVL_AUDIO, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNL(...) _LOGGER_S(DBGLVL_LEVEL, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNW(...) _LOGGER_S(DBGLVL_CONNECTION, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNS(...) _LOGGER_S(DBGLVL_SCRIPT, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNMEM(...) _LOGGER_S(DBGLVL_MEMORY, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNX(...) _LOGGER_S(DBGLVL_XMPLAYER, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNTODO(...) _LOGGER_S(DBGLVL_TODO, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNWarning(...) _LOGGER_S(DBGLVL_WARN, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#define LOGNError(...) _LOGGER_S(DBGLVL_ERROR, __FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)

void _LOGF(unsigned int level, char *fmt, ... );
void _LOGF(unsigned int level, const char *fmt, ... );

int _LOGGER(unsigned int level, const char *fileName, unsigned int lineNum, const char *functionName, const char *format, ...);

inline const char* _logArg(const std::string& s) { return s.c_str(); }
template<typename T>
inline typename std::enable_if<!std::is_same<typename std::decay<T>::type, std::string>::value, T&&>::type
_logArg(T&& arg) { return static_cast<T&&>(arg); }

template<typename... Args>
inline int _LOGGER_S(unsigned int level, const char *fileName, unsigned int lineNum,
                     const char *functionName, const char *format, Args&&... args) {
    return _LOGGER(level, fileName, lineNum, functionName, format, _logArg(static_cast<Args&&>(args))...);
}

void SYS_Errorf(char *fmt, ...);
void SYS_Errorf(const char *fmt, ...);

void Byte2Hex2digits(unsigned char value, char *bufOut);
void DBG_PrintBytes(void *data, unsigned int numBytes);

void DBG_LogTime();

#else

// do {} while(0), NOT a bare ';': a macro that expands to just ';' eats only
// the first statement slot of a brace-less "if (x) LOGM(...); else ...;" --
// the call site's own trailing ';' becomes a SECOND, sibling empty statement
// outside the if, leaving 'else' with no 'if' to attach to. Found on Linux
// (the only platform where GLOBAL_DEBUG_OFF is on by default) building
// the photo app: CViewthe photo appMain.cpp's font loader hit exactly this shape
// and failed with "error: 'else' without a previous 'if'". do{}while(0) is
// a single statement either way, so it is safe in every if/else/for shape,
// including as a macro argument list with commas.
#define LOGD(...) do {} while(0)
#define LOGD2(...) do {} while(0)
#define LOGM(...) do {} while(0)
#define LOGI(...) do {} while(0)
#define LOGP(...) do {} while(0)
#define LOGR(...) do {} while(0)
#define LOGG(...) do {} while(0)
#define LOGF(...) do {} while(0)
#define LOGH(...) do {} while(0)
#define LOGA(...) do {} while(0)
#define LOGAD(...) do {} while(0)
#define LOGDATA(...) do {} while(0)
#define LOGN(...) do {} while(0)
#define LOGS(...) do {} while(0)
#define LOGC(...) do {} while(0)
#define LOGCS(...) do {} while(0)
#define LOGCC(...) do {} while(0)
#define LOGMEM(...) do {} while(0)
#define LOGL(...) do {} while(0)
#define LOGW(...) do {} while(0)
#define LOGX(...) do {} while(0)
#define LOGVV(...) do {} while(0)
#define LOGVD(...) do {} while(0)
#define LOGVM(...) do {} while(0)

#define LOG_Atari_Main(...) do {} while(0)
#define LOG_Atari_Debug(...) do {} while(0)

#define LOGTODO(...) do {} while(0)
#define LOGWarning(...) do {} while(0)
#define LOGError(...) do {} while(0)

#define LOGND(...) do {} while(0)
#define LOGNM(...) do {} while(0)
#define LOGNP(...) do {} while(0)
#define LOGNR(...) do {} while(0)
#define LOGNG(...) do {} while(0)
#define LOGNF(...) do {} while(0)
#define LOGNH(...) do {} while(0)
#define LOGNA(...) do {} while(0)
#define LOGNN(...) do {} while(0)
#define LOGNS(...) do {} while(0)
#define LOGNMEM(...) do {} while(0)
#define LOGNL(...) do {} while(0)
#define LOGNW(...) do {} while(0)
#define LOGNX(...) do {} while(0)
#define LOGNTODO(...) do {} while(0)
#define LOGNWarning(...) do {} while(0)
#define LOGNError(...) do {} while(0)

#endif
// GLOBAL_DEBUG_OFF

#endif //__DBG_LOGF_H__
