/*
 * DBG_Log.cpp
 *
 *  Created on: Jun 9, 2011
 *      Author: mars
 */
#include "DBG_Log.h"
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include "SYS_Main.h"

#if !defined(GLOBAL_DEBUG_OFF)

#if defined(USE_DEBUG_LOG_TO_VIEW)
#include "CGuiViewDebugLog.h"
#endif

#define MAX_BUFFER_LENGTH	40960

#define USE_COLOR_CONSOLE
//#define LOG_FILE
//#define DEBUG_OFF
//#define FULL_LOG

static int logger_currentLogLevel = 0;
pthread_mutex_t loggerMutex;

bool LOG_IsSetLevel(unsigned int level)
{
	return IS_SET(logger_currentLogLevel, level);
}

int  LOG_GetCurrentLogLevel()
{
	return logger_currentLogLevel;
}

void LOG_SetLevel(unsigned int level, bool isOn)
{
	if (isOn)
	{
		SET_BIT(logger_currentLogLevel, level);
	}
	else
	{
		REMOVE_BIT(logger_currentLogLevel, level);
	}
}


#ifdef LOG_FILE
FILE *fpLog = NULL;
#endif

#ifndef DEBUG_OFF
#ifdef FULL_LOG

bool logThisLevel(unsigned int level)
{
	return true;
}

#else

bool logThisLevel(unsigned int level)
{
	return LOG_IsSetLevel(level);
}

#endif // FULL_LOG

#else

bool logThisLevel(unsigned int level)
{
//	if (level == DBGLVL_DEBUG) return true;
	return false;
}
#endif


const char *getLevelStr(unsigned int level)
{
	if (level == DBGLVL_MAIN)
		return "[MAIN] ";
	if (level == DBGLVL_INFO)
		return "[INFO] ";
	if (level == DBGLVL_DEBUG)
		return "[DEBUG]";
	if (level == DBGLVL_DEBUG2)
		return "[DEBG2]";
	if (level == DBGLVL_INPUT)
		return "[INPUT]";
	if (level == DBGLVL_WARN)
		return "[WARN] ";
	if (level == DBGLVL_RES)
		return "[RES]  ";
	if (level == DBGLVL_GUI)
		return "[GUI]  ";
	if (level == DBGLVL_HTTP)
		return "[HTTP] ";
	if (level == DBGLVL_DATABASE)		// cyan
		return "[DB]   ";
	if (level == DBGLVL_XMPLAYER)
		return "[PLAY] ";
	if (level == DBGLVL_AUDIO)
		return "[AUDIO]";
	if (level == DBGLVL_ERROR)
		return "[ERROR]";
	if (level == DBGLVL_TODO)
		return "[TODO] ";
	if (level == DBGLVL_NET)
		return "[NET ] ";
	if (level == DBGLVL_NET_SERVER)
		return "[SERV>]";
	if (level == DBGLVL_NET_CLIENT)
		return "[<CLNT]";
	if (level == DBGLVL_ANIMATION)
		return "[ANIM] ";
	if (level == DBGLVL_LEVEL)
		return "[LEVL] ";
	if (level == DBGLVL_DATA)
		return "[DATA] ";
	if (level == DBGLVL_SCRIPT)
		return ">SCRPT< ";
	if (level == DBGLVL_MEMORY)
		return "[MEM]  ";
	
	if (level == DBGLVL_VICE_DEBUG)
		return "[VICE ]";
	if (level == DBGLVL_VICE_MAIN)
		return "[VICEM]";
	if (level == DBGLVL_VICE_VERBOSE)
		return "[VICEV]";
	
	if (level == DBGLVL_ATARI_DEBUG)
		return "[ATARI]";
	if (level == DBGLVL_ATARI_MAIN)
		return "[ATARI]";


	return "[UNKNOWN]";
}

//#define USE_COUT

void DBG_SendLog(int debugLevel, char *message);

char logBuf[512];

void LOG_Init(void)
{
	pthread_mutex_init(&loggerMutex, NULL);

	LOG_SetLevel(DBGLVL_MAIN, true);
	LOG_SetLevel(DBGLVL_DEBUG, true);
	LOG_SetLevel(DBGLVL_DEBUG2, true);
	LOG_SetLevel(DBGLVL_TODO, true);
	LOG_SetLevel(DBGLVL_ERROR, true);
	LOG_SetLevel(DBGLVL_WARN, true);

#ifdef LOG_FILE
	time_t rawtime;
	struct tm * timeinfo;
	time ( &rawtime );
	timeinfo = localtime ( &rawtime );

	sprintf(logBuf, "./log/MTEngine-%02d%02d%02d-%02d%02d.txt", (timeinfo->tm_year-100), (timeinfo->tm_mon+1), timeinfo->tm_mday,
												timeinfo->tm_hour, timeinfo->tm_min);

	fpLog = fopen(logBuf, "wb");

	if (fpLog == NULL)
	{
		mkdir("./log/", 0750);
		fpLog = fopen(logBuf, "wb");
	}

#endif
}

static int backupLogLevel = 0;
void LOG_BackupCurrentLogLevel()
{
	backupLogLevel = logger_currentLogLevel;
}

void LOG_RestoreBackupLogLevel()
{
	logger_currentLogLevel = backupLogLevel;
}

void LOG_SetCurrentLogLevel(int level)
{
	logger_currentLogLevel = level;
}

void LOG_Shutdown(void)
{
	_LOGF(DBGLVL_MAIN, "closing stdlib & logfile\nbye!\n");

#ifdef LOG_FILE
	if (fpLog != NULL)
		fclose(fpLog);
#endif

}

void LOG_LockMutex()
{
	pthread_mutex_lock(&loggerMutex);
}

void LOG_UnlockMutex()
{
	pthread_mutex_unlock(&loggerMutex);
}

void DBG_SendLog(int debugLevel, char *message)
{
	/*
	 *
- Position the Cursor:
  \033[<L>;<C>H
     Or
  \033[<L>;<C>f
  puts the cursor at line L and column C.
- Move the cursor up N lines:
  \033[<N>A
- Move the cursor down N lines:
  \033[<N>B
- Move the cursor forward N columns:
  \033[<N>C
- Move the cursor backward N columns:
  \033[<N>D

- Clear the screen, move to (0,0):
  \033[2J
- Erase to end of line:
  \033[K

- Save cursor position:
  \033[s
- Restore cursor position:
  \033[u
	                            
	 
	  	\033[22;30m - black
		\033[22;31m - red
		\033[22;32m - green
		\033[22;33m - brown
		\033[22;34m - blue
		\033[22;35m - magenta
		\033[22;36m - cyan
		\033[22;37m - gray
		\033[01;30m - dark gray
		\033[01;31m - light red
		\033[01;32m - light green
		\033[01;33m - yellow
		\033[01;34m - light blue
		\033[01;35m - light magenta
		\033[01;36m - light cyan
		\033[01;37m - white
	 */

#ifdef USE_COLOR_CONSOLE
	switch(debugLevel)
	{
		case DBGLVL_MAIN:
			fprintf(stdout, "\033[01;33m");
			break;
		case DBGLVL_ERROR:
			fprintf(stdout, "\033[01;31m");
			break;
		case DBGLVL_WARN:
			fprintf(stdout, "\033[01;31m");
			break;
		case DBGLVL_FATAL:
			fprintf(stdout, "\033[01;31m");
			break;
		case DBGLVL_TODO:
			fprintf(stdout, "\033[01;31m");
			break;
		case DBGLVL_MEMORY:
			fprintf(stdout, "\033[01;31m");
			break;
		case DBGLVL_DATABASE:
			fprintf(stdout, "\033[22;36m");
			break;
		case DBGLVL_DATA:
			fprintf(stdout, "\033[22;36m");
			break;
		case DBGLVL_DEBUG:
			fprintf(stdout, "\033[22;37m");
			break;
		case DBGLVL_DEBUG2:
			fprintf(stdout, "\033[22;37m");
			break;
		case DBGLVL_AUDIO:
			fprintf(stdout, "\033[01;36m");
			break;
		case DBGLVL_RES:
			fprintf(stdout, "\033[01;35m");
			break;
		case DBGLVL_GUI:
			fprintf(stdout, "\033[01;32m");
			break;
		case DBGLVL_LEVEL:
			fprintf(stdout, "\033[01;31m");
			break;
		case DBGLVL_ANIMATION:
			fprintf(stdout, "\033[22;35m");
			break;
		case DBGLVL_NET:
			fprintf(stdout, "\033[22;36m");
			break;
		case DBGLVL_NET_SERVER:
			fprintf(stdout, "\033[01;36m");
			break;
		case DBGLVL_NET_CLIENT:
			fprintf(stdout, "\033[01;35m");
			break;
		default:
			fprintf(stdout, "\033[22;37m");
			break;
	}
#endif
	
	static char buf[MAX_BUFFER_LENGTH];
	struct timeval  tv;
	struct timezone tz;
	struct tm      *tm;

	gettimeofday(&tv, &tz);
	tm = localtime(&tv.tv_sec);

	unsigned int threadId = 0; //valgrind complains: (long int)syscall(224);

	int ms = tv.tv_usec/10000;
	
	sprintf(buf, "%02d:%02d:%02d,%03d %4.4X %s %s\n",
				tm->tm_hour, tm->tm_min, tm->tm_sec, ms,
				threadId, getLevelStr(debugLevel), message);
	
#ifdef LOG_FILE
	//03:22:07,127 000010B4 [DEBUG] CGuiList::CGuiList done
	if (fpLog != NULL)
	{
		fprintf(fpLog, buf);
		fflush(fpLog);
	}
#endif

#if defined(USE_DEBUG_LOG_TO_VIEW)
		if (guiViewDebugLog)
			guiViewDebugLog->AddLog(buf);
#endif

	fprintf(stdout, buf);
	fflush(stdout);

}

void DBG_PrintBytes(void *data, unsigned int numBytes)
{
	LOG_LockMutex();

	for (unsigned int i = 0; i < numBytes; i++)
	{
		unsigned char c = ((unsigned char *)data)[i];
		printf("%2.2x ", c);
	}
	fflush(stdout);
	LOG_UnlockMutex();
}

void LOGT(unsigned int level, char *what)
{
	if (!logThisLevel(level))
		return;
	_LOGF(level, what);
}

void LOGT(unsigned int level, const char *what)
{
	if (!logThisLevel(level))
		return;

	_LOGF(level, what);
}

void _LOGF(unsigned int level, char *fmt, ... )
{
    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(level, buffer);

	LOG_UnlockMutex();

}

void _LOGF(unsigned int level, std::string what)
{
	if (!logThisLevel(level))
		return;
	_LOGF(level, what.c_str());
}

void _LOGF(unsigned int level, const char *fmt, ... )
{
	if (!logThisLevel(level))
		return;
    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(level, buffer);

	LOG_UnlockMutex();
}

////////LOGG

void LOGG(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_GUI))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_GUI, buffer);

	LOG_UnlockMutex();
}

void LOGG(std::string what)
{
	if (!logThisLevel(DBGLVL_GUI))
		return;

	_LOGF(DBGLVL_GUI, what.c_str());
}

void LOGG(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_GUI))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_GUI, buffer);

	LOG_UnlockMutex();
}
///////////// LOGG

////////LOGD

void LOGD(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_DEBUG))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_DEBUG, buffer);

	LOG_UnlockMutex();

}

void LOGD(std::string what)
{
	if (!logThisLevel(DBGLVL_DEBUG))
		return;

	_LOGF(DBGLVL_DEBUG, what.c_str());
}

void LOGD(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_DEBUG))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_DEBUG, buffer);

	LOG_UnlockMutex();
}
///////////// LOGD

////////LOGD

void LOGD2(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_DEBUG2))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_DEBUG2, buffer);

	LOG_UnlockMutex();

}

void LOGD2(std::string what)
{
	if (!logThisLevel(DBGLVL_DEBUG2))
		return;

	_LOGF(DBGLVL_DEBUG2, what.c_str());
}

void LOGD2(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_DEBUG2))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_DEBUG2, buffer);

	LOG_UnlockMutex();
}
///////////// LOGD2

////////LOGI

void LOGI(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_INPUT))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_INPUT, buffer);

	LOG_UnlockMutex();

}

void LOGI(std::string what)
{
	if (!logThisLevel(DBGLVL_INPUT))
		return;

	_LOGF(DBGLVL_INPUT, what.c_str());
}

void LOGI(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_INPUT))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_INPUT, buffer);

	LOG_UnlockMutex();
}
///////////// LOGI


////////LOGM

void LOGM(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_MAIN))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_MAIN, buffer);

	LOG_UnlockMutex();

}

void LOGM(std::string what)
{
	if (!logThisLevel(DBGLVL_MAIN))
		return;

	_LOGF(DBGLVL_MAIN, what.c_str());
}

void LOGM(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_MAIN))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_MAIN, buffer);

	LOG_UnlockMutex();
}
///////////// LOGM

////////LOGL

void LOGL(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_LEVEL))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_LEVEL, buffer);

	LOG_UnlockMutex();

}

void LOGL(std::string what)
{
	if (!logThisLevel(DBGLVL_LEVEL))
		return;

	_LOGF(DBGLVL_LEVEL, what.c_str());
}

void LOGL(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_LEVEL))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_MAIN, buffer);

	LOG_UnlockMutex();
}
///////////// LOGL

////////LOGS

void LOGS(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_SCRIPT))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_SCRIPT, buffer);

	LOG_UnlockMutex();

}

void LOGS(std::string what)
{
	if (!logThisLevel(DBGLVL_SCRIPT))
		return;

	_LOGF(DBGLVL_SCRIPT, what.c_str());
}

void LOGS(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_SCRIPT))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_MAIN, buffer);

	LOG_UnlockMutex();
}
///////////// LOGS

////////LOGN

void LOGN(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ANIMATION))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ANIMATION, buffer);

	LOG_UnlockMutex();

}

void LOGN(std::string what)
{
	if (!logThisLevel(DBGLVL_ANIMATION))
		return;

	_LOGF(DBGLVL_ANIMATION, what.c_str());
}

void LOGN(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ANIMATION))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ANIMATION, buffer);

	LOG_UnlockMutex();
}
///////////// LOGN
////////LOGA

void LOGA(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_AUDIO))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_AUDIO, buffer);

	LOG_UnlockMutex();
}

void LOGA(std::string what)
{
	if (!logThisLevel(DBGLVL_AUDIO))
		return;

	_LOGF(DBGLVL_AUDIO, what.c_str());
}

void LOGA(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_AUDIO))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_AUDIO, buffer);

	LOG_UnlockMutex();
}
///////////// LOGA

////////LOGC

void LOGC(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_NET))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_NET, buffer);

	LOG_UnlockMutex();
}

void LOGC(std::string what)
{
	if (!logThisLevel(DBGLVL_NET))
		return;

	_LOGF(DBGLVL_NET, what.c_str());
}

void LOGC(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_NET))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_NET, buffer);

	LOG_UnlockMutex();
}
///////////// LOGC

////////LOGCC

void LOGCC(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_NET_CLIENT))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_NET_CLIENT, buffer);

	LOG_UnlockMutex();
}

void LOGCC(std::string what)
{
	if (!logThisLevel(DBGLVL_NET_CLIENT))
		return;

	_LOGF(DBGLVL_NET_CLIENT, what.c_str());
}

void LOGCC(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_NET_CLIENT))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_NET_CLIENT, buffer);

	LOG_UnlockMutex();
}
///////////// LOGCC

////////LOGCS

void LOGCS(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_NET_SERVER))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_NET_SERVER, buffer);

	LOG_UnlockMutex();
}

void LOGCS(std::string what)
{
	if (!logThisLevel(DBGLVL_NET_SERVER))
		return;

	_LOGF(DBGLVL_NET_SERVER, what.c_str());
}

void LOGCS(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_NET_SERVER))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_NET_SERVER, buffer);

	LOG_UnlockMutex();
}
///////////// LOGCS


////////LOGX

void LOGX(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_XMPLAYER))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_XMPLAYER, buffer);

	LOG_UnlockMutex();
}

void LOGX(std::string what)
{
	if (!logThisLevel(DBGLVL_XMPLAYER))
		return;

	_LOGF(DBGLVL_XMPLAYER, what.c_str());
}

void LOGX(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_XMPLAYER))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_XMPLAYER, buffer);

	LOG_UnlockMutex();
}
///////////// LOGX

////////LOGP

void LOGP(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_PLUGIN))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_PLUGIN, buffer);

	LOG_UnlockMutex();
}

void LOGP(std::string what)
{
	if (!logThisLevel(DBGLVL_PLUGIN))
		return;

	_LOGF(DBGLVL_PLUGIN, what.c_str());
}

void LOGP(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_PLUGIN))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_PLUGIN, buffer);

	LOG_UnlockMutex();
}
///////////// LOGP



////////LOGR

void LOGR(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_RES))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_RES, buffer);

	LOG_UnlockMutex();
}

void LOGR(std::string what)
{
	if (!logThisLevel(DBGLVL_RES))
		return;

	_LOGF(DBGLVL_RES, what.c_str());
}

void LOGR(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_RES))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_RES, buffer);

	LOG_UnlockMutex();
}
///////////// LOGR

//

////////LOG_Atari_Main

void LOG_Atari_Main(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ATARI_MAIN))
		return;
	
	char buffer[4096] = {0};
	
	va_list args;
	
	va_start(args, fmt);
	vsprintf(buffer, fmt, args);
	va_end(args);
	
	LOG_LockMutex();
	
	DBG_SendLog(DBGLVL_ATARI_MAIN, buffer);
	
	LOG_UnlockMutex();
}

void LOG_Atari_Main(std::string what)
{
	if (!logThisLevel(DBGLVL_ATARI_MAIN))
		return;
	
	_LOGF(DBGLVL_ATARI_MAIN, what.c_str());
}

void LOG_Atari_Main(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ATARI_MAIN))
		return;
	
	char buffer[4096] = {0};
	
	va_list args;
	
	va_start(args, fmt);
	vsprintf(buffer, fmt, args);
	va_end(args);
	
	LOG_LockMutex();
	
	DBG_SendLog(DBGLVL_ATARI_MAIN, buffer);
	
	LOG_UnlockMutex();
}
///////////// LOG_Atari_Main

////////LOG_Atari_Debug

void LOG_Atari_Debug(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ATARI_DEBUG))
		return;
	
	char buffer[4096] = {0};
	
	va_list args;
	
	va_start(args, fmt);
	vsprintf(buffer, fmt, args);
	va_end(args);
	
	LOG_LockMutex();
	
	DBG_SendLog(DBGLVL_ATARI_DEBUG, buffer);
	
	LOG_UnlockMutex();
}

void LOG_Atari_Debug(std::string what)
{
	if (!logThisLevel(DBGLVL_ATARI_DEBUG))
		return;
	
	_LOGF(DBGLVL_ATARI_DEBUG, what.c_str());
}

void LOG_Atari_Debug(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ATARI_DEBUG))
		return;
	
	char buffer[4096] = {0};
	
	va_list args;
	
	va_start(args, fmt);
	vsprintf(buffer, fmt, args);
	va_end(args);
	
	LOG_LockMutex();
	
	DBG_SendLog(DBGLVL_ATARI_DEBUG, buffer);
	
	LOG_UnlockMutex();
}
///////////// LOG_Atari_Debug

////////LOGMEM

void LOGMEM(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_MEMORY))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_MEMORY, buffer);

	LOG_UnlockMutex();
}

void LOGMEM(std::string what)
{
	if (!logThisLevel(DBGLVL_MEMORY))
		return;

	_LOGF(DBGLVL_MEMORY, what.c_str());
}

void LOGMEM(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_MEMORY))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_MEMORY, buffer);

	LOG_UnlockMutex();
}
///////////// LOGMEM

////////LOGTODO

void LOGTODO(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_TODO))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();
	DBG_SendLog(DBGLVL_TODO, buffer);
	LOG_UnlockMutex();

}

void LOGTODO(std::string what)
{
	if (!logThisLevel(DBGLVL_TODO))
		return;

	_LOGF(DBGLVL_TODO, what.c_str());
}

void LOGTODO(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_TODO))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();
	DBG_SendLog(DBGLVL_TODO, buffer);
	LOG_UnlockMutex();
}
///////////// LOGTODO
void LOGError(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ERROR))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ERROR, buffer);

	LOG_UnlockMutex();

}

void LOGError(std::string what)
{
	if (!logThisLevel(DBGLVL_ERROR))
		return;

	_LOGF(DBGLVL_ERROR, what.c_str());
}

void LOGError(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_ERROR))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ERROR, buffer);

	LOG_UnlockMutex();
}
/////////////

///////////// LOGTODO
void LOGWarning(char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_WARN))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_WARN, buffer);

	LOG_UnlockMutex();

}

void LOGWarning(std::string what)
{
	if (!logThisLevel(DBGLVL_WARN))
		return;

	_LOGF(DBGLVL_WARN, what.c_str());
}

void LOGWarning(const char *fmt, ... )
{
	if (!logThisLevel(DBGLVL_WARN))
		return;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_WARN, buffer);

	LOG_UnlockMutex();
}
/////////////


///////////////

void SYS_Errorf(char *fmt, ... )
{
	//m_Log << "ERROR:" << std::endl;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ERROR, buffer);

	LOG_UnlockMutex();
}

void SYS_Errorf(const char *fmt, ... )
{
	//m_Log << "ERROR:" << std::endl;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ERROR, buffer);

	LOG_UnlockMutex();
}

#else

void LOG_Init(void) {}
void LOG_SetLevel(unsigned int level, bool isOn) {}
void LOG_Shutdown(void) {}

#endif

