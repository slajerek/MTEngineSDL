/*
 *  DBG_Log.h WIN32
 *
 *  Created by Marcin Skoczylas on 09-11-19.
 *  Copyright 2009 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef __DBG_LOGF_H__
#define __DBG_LOGF_H__

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>
#include <type_traits>

#define GLOBAL_DEBUG_OFF

#define DBGLVL_ALL_OFF	0x0000
#define DBGLVL_ALL_ON	0xFFFF
#define DBGLVL_FATAL	(1 << 0)
#define DBGLVL_ERROR	(1 << 1)
#define DBGLVL_WARN		(1 << 2)
#define DBGLVL_GUI		(1 << 3)
#define DBGLVL_INFO		(1 << 4)
#define DBGLVL_MAIN		(DBGLVL_INFO)
#define DBGLVL_TRANSACTION	(1 << 5)
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
#define DBGLVL_PAINT		(1 << 28)
#define DBGLVL_DEBUG2		(1 << 29)

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

#define LOGD(...) _LOGGER_S(DBGLVL_DEBUG, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGVD(...) _LOGGER_S(DBGLVL_VICE_DEBUG, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGD2(...) _LOGGER_S(DBGLVL_DEBUG2, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGM(...) _LOGGER_S(DBGLVL_MAIN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGVM(...) _LOGGER_S(DBGLVL_VICE_MAIN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGVV(...) _LOGGER_S(DBGLVL_VICE_VERBOSE, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGI(...) _LOGGER_S(DBGLVL_INPUT, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGP(...) _LOGGER_S(DBGLVL_PLUGIN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGR(...) _LOGGER_S(DBGLVL_RES, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGG(...) _LOGGER_S(DBGLVL_GUI, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGF(...) _LOGGER_S(DBGLVL_PAINT, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGH(...) _LOGGER_S(DBGLVL_HTTP, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGDATA(...) _LOGGER_S(DBGLVL_TRANSACTION, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGA(...) _LOGGER_S(DBGLVL_AUDIO, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGAD(...) _LOGGER_S(DBGLVL_DEBUG, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGL(...) _LOGGER_S(DBGLVL_LEVEL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGW(...) _LOGGER_S(DBGLVL_CONNECTION, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGX(...) _LOGGER_S(DBGLVL_XMPLAYER, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGN(...) _LOGGER_S(DBGLVL_ANIMATION, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGS(...) _LOGGER_S(DBGLVL_SCRIPT, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGC(...) _LOGGER_S(DBGLVL_NET, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGCS(...) _LOGGER_S(DBGLVL_NET_SERVER, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGCC(...) _LOGGER_S(DBGLVL_NET_CLIENT, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define LOG_Atari_Main(...) _LOGGER_S(DBGLVL_ATARI_MAIN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_Atari_Debug(...) _LOGGER_S(DBGLVL_ATARI_DEBUG, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define LOGMEM(...) _LOGGER_S(DBGLVL_MEMORY, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGTODO(...) _LOGGER_S(DBGLVL_TODO, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGWarning(...) _LOGGER_S(DBGLVL_WARN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGError(...) _LOGGER_S(DBGLVL_ERROR, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define LOGND(...) _LOGGER_S(DBGLVL_DEBUG, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNM(...) _LOGGER_S(DBGLVL_MAIN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNP(...) _LOGGER_S(DBGLVL_PLUGIN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNR(...) _LOGGER_S(DBGLVL_RES, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNG(...) _LOGGER_S(DBGLVL_GUI, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNF(...) _LOGGER_S(DBGLVL_PAINT, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNN(...) _LOGGER_S(DBGLVL_ANIMATION, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNH(...) _LOGGER_S(DBGLVL_HTTP, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNA(...) _LOGGER_S(DBGLVL_AUDIO, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNL(...) _LOGGER_S(DBGLVL_LEVEL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNW(...) _LOGGER_S(DBGLVL_CONNECTION, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNS(...) _LOGGER_S(DBGLVL_SCRIPT, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNMEM(...) _LOGGER_S(DBGLVL_MEMORY, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNX(...) _LOGGER_S(DBGLVL_XMPLAYER, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNTODO(...) _LOGGER_S(DBGLVL_TODO, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNWarning(...) _LOGGER_S(DBGLVL_WARN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGNError(...) _LOGGER_S(DBGLVL_ERROR, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

void _LOGF(int level, char *fmt, ... );
void _LOGF(int level, const char *fmt, ... );

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

#define LOGD(...) ; 
#define LOGD2(...) ;
#define LOGM(...) ;
#define LOGI(...) ;
#define LOGP(...) ;
#define LOGR(...) ; 
#define LOGG(...) ; 
#define LOGF(...) ; 
#define LOGH(...) ; 
#define LOGA(...) ;
#define LOGAD(...) ;
#define LOGDATA(...) ;
#define LOGN(...) ;
#define LOGS(...) ;
#define LOGC(...) ;
#define LOGCS(...) ;
#define LOGCC(...) ;
#define LOGMEM(...) ;
#define LOGL(...) ; 
#define LOGW(...) ; 
#define LOGX(...) ;
#define LOGVV(...) ;
#define LOGVD(...) ;
#define LOGVM(...) ;
#define LOG_Atari_Main(...) ;
#define LOG_Atari_Debug(...) ;

#define LOGTODO(...) ; 
#define LOGWarning(...) ; 
#define LOGError(...) ; 

#define LOGND(...) ; 
#define LOGNM(...) ; 
#define LOGNP(...) ;
#define LOGNR(...) ; 
#define LOGNG(...) ; 
#define LOGNF(...) ; 
#define LOGNH(...) ; 
#define LOGNA(...) ; 
#define LOGNN(...) ;
#define LOGNS(...) ;
#define LOGNMEM(...) ;
#define LOGNL(...) ; 
#define LOGNW(...) ; 
#define LOGNX(...) ; 
#define LOGNTODO(...) ; 
#define LOGNWarning(...) ; 
#define LOGNError(...) ; 

#endif
// GLOBAL_DEBUG_OFF


#endif __DBG_LOGF_H__
