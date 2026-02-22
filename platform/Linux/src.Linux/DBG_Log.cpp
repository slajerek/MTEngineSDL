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


int _LOGGER(unsigned int level, const char *fileName, unsigned int lineNum, const char *functionName, const char *format, ...)
{
	if (!logThisLevel(level))
		return 0;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);

	LOG_LockMutex();

	DBG_SendLog(level, buffer);

	LOG_UnlockMutex();

	return 1;
}

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

