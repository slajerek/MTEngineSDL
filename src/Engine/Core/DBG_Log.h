/*
 *  DBG_Log.h -- the engine's logging macros, ONE header for all three platforms.
 *
 *  Created by Marcin Skoczylas on 2009-11-19.
 *  Copyright 2009 Marcin Skoczylas
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */

#ifndef __DBG_LOGF_H__
#define __DBG_LOGF_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef __cplusplus
#include <string>
#include <type_traits>
#endif

// ---------------------------------------------------------------------------
// MT_DEBUG_LOGS -- decided by the BUILD, not by editing this file.
//
// Until 2026-09-05 this header was three per-platform copies with three
// different level maps, and whether logging existed at all was a hardcoded
// `#define GLOBAL_DEBUG_OFF` -- on for Linux, off for the others -- so a Linux
// CI crash printed nothing and the only remedy was a source edit. Now:
//
//   MT_DEBUG_LOGS=1   a DEVELOPMENT build: every macro below is live.
//   MT_DEBUG_LOGS=0   a FINAL build (./build-<os>.sh --prod, unless --logs on):
//                     the verbose macros compile to nothing.
//
// The value comes from mtcaps as a compiler define (`--logs on|off` on the
// build driver). When nothing supplied it -- a bare IDE build of the engine, a
// standalone configure -- it defaults to 1: the development default, and the
// safe direction, because a forgotten switch shows as verbose output rather
// than as a silent binary. The CI `logs: off` leg is what proves the off path
// is wired.
//
// WHAT IS NEVER OFF: LOGError, LOGNError and LOGFatal. They compile in every
// build and reach stderr (macOS, Linux) or the log file and the debugger
// (Windows) whatever the level mask says. The account of a crash was the one
// thing GLOBAL_DEBUG_OFF used to remove first.
// ---------------------------------------------------------------------------
#ifndef MT_DEBUG_LOGS
#define MT_DEBUG_LOGS 1
#endif

// One-release alias for the sites that still ask the old question. Remove
// once no app tests GLOBAL_DEBUG_OFF (they should test MT_DEBUG_LOGS).
#if !MT_DEBUG_LOGS
#define GLOBAL_DEBUG_OFF 1
#endif

// ---------------------------------------------------------------------------
// Levels -- ONE map. The mask is persisted by CGuiViewDebugLog as an integer
// (settings key LogLevel2), so these values are a contract across platforms
// and releases: never renumber, only append at free bits.
// ---------------------------------------------------------------------------
#define DBGLVL_ALL_OFF		0x00000000
#define DBGLVL_FATAL		(1 << 0)
#define DBGLVL_ERROR		(1 << 1)
#define DBGLVL_WARN			(1 << 2)
#define DBGLVL_GUI			(1 << 3)
#define DBGLVL_INFO			(1 << 4)
#define DBGLVL_MAIN			(DBGLVL_INFO)
#define DBGLVL_DATA			(1 << 5)
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
#define DBGLVL_PAINT		(1 << 29)
#define DBGLVL_ADS			(1 << 30)
#define DBGLVL_WEBSERVICE	(1u << 31)

// What LOG_Init starts from, identically on every platform: everything on
// except the four that flood -- per-frame paint, allocation traces, VICE's
// verbose channel and the second debug channel. FATAL and ERROR are in the
// mask for the viewer's sake; _LOGGER does not consult it for them.
#define DBGLVL_DEFAULT_MASK \
	(0xFFFFFFFFu & ~(unsigned int)(DBGLVL_PAINT | DBGLVL_MEMORY | DBGLVL_VICE_VERBOSE | DBGLVL_DEBUG2))

#define IS_SET(flag, bit)       ((flag) & (bit))
#define SET_BIT(var, bit)       ((var) |= (bit))
#define REMOVE_BIT(var, bit)    ((var) &= ~(bit))
#define TOGGLE_BIT(var, bit)    ((var) ^= (bit))

// ---------------------------------------------------------------------------
// The logger. Always compiled, on every platform, whatever MT_DEBUG_LOGS says.
// ---------------------------------------------------------------------------
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

// The file LOG_Init opened for this run, or "" when it opened none. What a
// test reads back to prove an error was written; what a bug report names.
const char *LOG_GetLogFilePath(void);

int _LOGGER(unsigned int level, const char *fileName, unsigned int lineNum, const char *functionName, const char *format, ...);

#ifdef _MSC_VER
#define MT_LOG_FUNCTION __FUNCTION__
#else
#define MT_LOG_FUNCTION __PRETTY_FUNCTION__
#endif

#ifdef __cplusplus
inline const char* _logArg(const std::string& s) { return s.c_str(); }
template<typename T>
inline typename std::enable_if<!std::is_same<typename std::decay<T>::type, std::string>::value, T&&>::type
_logArg(T&& arg) { return static_cast<T&&>(arg); }

template<typename... Args>
inline int _LOGGER_S(unsigned int level, const char *fileName, unsigned int lineNum,
                     const char *functionName, const char *format, Args&&... args) {
    return _LOGGER(level, fileName, lineNum, functionName, format, _logArg(static_cast<Args&&>(args))...);
}
#else
#define _LOGGER_S _LOGGER
#endif

// Helpers implemented by the Linux and Windows sinks only.
#if !defined(__APPLE__)
void _LOGF(unsigned int level, char *fmt, ... );
void _LOGF(unsigned int level, const char *fmt, ... );
void SYS_Errorf(char *fmt, ...);
void SYS_Errorf(const char *fmt, ...);
void DBG_PrintBytes(void *data, unsigned int numBytes);
void DBG_LogTime();
#endif
void Byte2Hex2digits(unsigned char value, char *bufOut);

// ---------------------------------------------------------------------------
// ALWAYS ON. Not subject to MT_DEBUG_LOGS, not subject to the level mask.
// ---------------------------------------------------------------------------
#define LOGFatal(...)  _LOGGER_S(DBGLVL_FATAL, __FILE__, __LINE__, MT_LOG_FUNCTION, __VA_ARGS__)
#define LOGError(...)  _LOGGER_S(DBGLVL_ERROR, __FILE__, __LINE__, MT_LOG_FUNCTION, __VA_ARGS__)
#define LOGNError(...) _LOGGER_S(DBGLVL_ERROR, __FILE__, __LINE__, MT_LOG_FUNCTION, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Gated on MT_DEBUG_LOGS.
// ---------------------------------------------------------------------------
#if MT_DEBUG_LOGS

#define _MT_LOG(lvl, ...) _LOGGER_S(lvl, __FILE__, __LINE__, MT_LOG_FUNCTION, __VA_ARGS__)

#else

// do {} while(0), NOT "{};" or a bare ';': a macro body that is already a
// complete statement leaves the call site's own trailing ';' as a SECOND,
// sibling empty statement, and in a brace-less "if (x) LOGM(...); else ...;"
// the compiler sees an 'else' with no 'if'. It broke a font loader once.
#define _MT_LOG(lvl, ...) do {} while (0)

#endif

#define LOGD(...)        _MT_LOG(DBGLVL_DEBUG, __VA_ARGS__)
#define LOGD2(...)       _MT_LOG(DBGLVL_DEBUG2, __VA_ARGS__)
#define LOGM(...)        _MT_LOG(DBGLVL_MAIN, __VA_ARGS__)
#define LOGVD(...)       _MT_LOG(DBGLVL_VICE_DEBUG, __VA_ARGS__)
#define LOGVM(...)       _MT_LOG(DBGLVL_VICE_MAIN, __VA_ARGS__)
#define LOGVV(...)       _MT_LOG(DBGLVL_VICE_VERBOSE, __VA_ARGS__)
#define LOGI(...)        _MT_LOG(DBGLVL_INPUT, __VA_ARGS__)
#define LOGP(...)        _MT_LOG(DBGLVL_PLUGIN, __VA_ARGS__)
#define LOGR(...)        _MT_LOG(DBGLVL_RES, __VA_ARGS__)
#define LOGG(...)        _MT_LOG(DBGLVL_GUI, __VA_ARGS__)
#define LOGF(...)        _MT_LOG(DBGLVL_PAINT, __VA_ARGS__)
#define LOGH(...)        _MT_LOG(DBGLVL_HTTP, __VA_ARGS__)
#define LOGDATA(...)     _MT_LOG(DBGLVL_DATA, __VA_ARGS__)
#define LOGA(...)        _MT_LOG(DBGLVL_AUDIO, __VA_ARGS__)
#define LOGAD(...)       _MT_LOG(DBGLVL_ADS, __VA_ARGS__)
#define LOGL(...)        _MT_LOG(DBGLVL_LEVEL, __VA_ARGS__)
#define LOGW(...)        _MT_LOG(DBGLVL_WEBSERVICE, __VA_ARGS__)
#define LOGX(...)        _MT_LOG(DBGLVL_XMPLAYER, __VA_ARGS__)
#define LOGN(...)        _MT_LOG(DBGLVL_ANIMATION, __VA_ARGS__)
#define LOGS(...)        _MT_LOG(DBGLVL_SCRIPT, __VA_ARGS__)
#define LOGC(...)        _MT_LOG(DBGLVL_NET, __VA_ARGS__)
#define LOGCS(...)       _MT_LOG(DBGLVL_NET_SERVER, __VA_ARGS__)
#define LOGCC(...)       _MT_LOG(DBGLVL_NET_CLIENT, __VA_ARGS__)
#define LOGMEM(...)      _MT_LOG(DBGLVL_MEMORY, __VA_ARGS__)
#define LOGTODO(...)     _MT_LOG(DBGLVL_TODO, __VA_ARGS__)
#define LOGWarning(...)  _MT_LOG(DBGLVL_WARN, __VA_ARGS__)

#define LOG_Atari_Main(...)  _MT_LOG(DBGLVL_ATARI_MAIN, __VA_ARGS__)
#define LOG_Atari_Debug(...) _MT_LOG(DBGLVL_ATARI_DEBUG, __VA_ARGS__)

// The LOGN* family: same levels, kept for the callers that spell them so.
#define LOGND(...)       _MT_LOG(DBGLVL_DEBUG, __VA_ARGS__)
#define LOGNM(...)       _MT_LOG(DBGLVL_MAIN, __VA_ARGS__)
#define LOGNP(...)       _MT_LOG(DBGLVL_PLUGIN, __VA_ARGS__)
#define LOGNR(...)       _MT_LOG(DBGLVL_RES, __VA_ARGS__)
#define LOGNG(...)       _MT_LOG(DBGLVL_GUI, __VA_ARGS__)
#define LOGNF(...)       _MT_LOG(DBGLVL_PAINT, __VA_ARGS__)
#define LOGNN(...)       _MT_LOG(DBGLVL_ANIMATION, __VA_ARGS__)
#define LOGNH(...)       _MT_LOG(DBGLVL_HTTP, __VA_ARGS__)
#define LOGNA(...)       _MT_LOG(DBGLVL_AUDIO, __VA_ARGS__)
#define LOGNL(...)       _MT_LOG(DBGLVL_LEVEL, __VA_ARGS__)
#define LOGNW(...)       _MT_LOG(DBGLVL_WEBSERVICE, __VA_ARGS__)
#define LOGNS(...)       _MT_LOG(DBGLVL_SCRIPT, __VA_ARGS__)
#define LOGNMEM(...)     _MT_LOG(DBGLVL_MEMORY, __VA_ARGS__)
#define LOGNX(...)       _MT_LOG(DBGLVL_XMPLAYER, __VA_ARGS__)
#define LOGNTODO(...)    _MT_LOG(DBGLVL_TODO, __VA_ARGS__)
#define LOGNWarning(...) _MT_LOG(DBGLVL_WARN, __VA_ARGS__)

#endif // __DBG_LOGF_H__
