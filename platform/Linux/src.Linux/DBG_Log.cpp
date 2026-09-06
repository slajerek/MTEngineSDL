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
#include <unistd.h>
#include "SYS_Main.h"

// The logger is ALWAYS compiled; MT_DEBUG_LOGS gates only the verbose macros
// in DBG_Log.h. Until 2026-09-05 this whole file was inside
// #if !defined(GLOBAL_DEBUG_OFF), which on this platform was on by default --
// so a crash printed nothing at all.

#if defined(USE_DEBUG_LOG_TO_VIEW)
#include "CGuiViewDebugLog.h"
#endif

#define MAX_BUFFER_LENGTH	40960

#define USE_COLOR_CONSOLE
// A file sink, as on macOS and Windows -- this platform never had one, so a
// headless run's errors existed only on a stdout nobody captured.
#define LOG_FILE
//#define DEBUG_OFF
//#define FULL_LOG

static int logger_currentLogLevel = (int)DBGLVL_DEFAULT_MASK;
static char logger_filePath[4096] = {0};
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
	if (level == DBGLVL_FATAL)
		return "[FATAL]";
	if (level == DBGLVL_PAINT)
		return "[PAINT]";
	if (level == DBGLVL_ADS)
		return "[ADS]  ";
	if (level == DBGLVL_WEBSERVICE)
		return "[WEBSV]";
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

char logBuf[4096];

void LOG_Init(void)
{
	pthread_mutex_init(&loggerMutex, NULL);

	// ONE default, the same on every platform: DBGLVL_DEFAULT_MASK in DBG_Log.h.
	logger_currentLogLevel = (int)DBGLVL_DEFAULT_MASK;

#ifdef LOG_FILE
	time_t rawtime;
	struct tm * timeinfo;
	time ( &rawtime );
	timeinfo = localtime ( &rawtime );

	// --log-dir <path>, read from /proc/self/cmdline: LOG_Init runs before
	// SYS_SetCommandLineArguments in every platform's main(), so the engine's
	// own argv copy is not there yet. macOS reads NSProcessInfo and Windows
	// __argv for the same reason.
	char logDir[3072] = {0};
	{
		FILE *cl = fopen("/proc/self/cmdline", "rb");
		if (cl != NULL)
		{
			static char cmdline[65536];
			size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, cl);
			fclose(cl);
			cmdline[n] = 0;
			const char *p = cmdline, *end = cmdline + n;
			const char *prev = NULL;
			while (p < end)
			{
				if (prev != NULL && strcmp(prev, "--log-dir") == 0)
				{
					snprintf(logDir, sizeof(logDir), "%s", p);
					break;
				}
				prev = p;
				p += strlen(p) + 1;
			}
		}
	}
	if (logDir[0] == 0)
	{
		// ${XDG_CACHE_HOME:-~/.cache}/MTEngine -- never the cwd, which is the
		// git root for a development build and a release package for a final
		// one, and neither wants a log/ directory appearing in it.
		const char *xdg = getenv("XDG_CACHE_HOME");
		const char *home = getenv("HOME");
		if (xdg != NULL && xdg[0] != 0)
			snprintf(logDir, sizeof(logDir), "%s/MTEngine", xdg);
		else if (home != NULL && home[0] != 0)
			snprintf(logDir, sizeof(logDir), "%s/.cache/MTEngine", home);
		else
			snprintf(logDir, sizeof(logDir), "/tmp/MTEngine");
	}
	mkdir(logDir, 0750);

	snprintf(logBuf, sizeof(logBuf), "%s/MTEngine-%02d%02d%02d-%02d%02d%02d-%d.txt", logDir,
			 (timeinfo->tm_year-100), (timeinfo->tm_mon+1), timeinfo->tm_mday,
			 timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, (int)getpid());

	fpLog = fopen(logBuf, "wb");
	if (fpLog != NULL)
		snprintf(logger_filePath, sizeof(logger_filePath), "%s", logBuf);
#endif
}

const char *LOG_GetLogFilePath(void)
{
	return logger_filePath;
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

	int ms = tv.tv_usec/1000;
	
	sprintf(buf, "%02d:%02d:%02d,%03d %4.4X %s %s\n",
				tm->tm_hour, tm->tm_min, tm->tm_sec, ms,
				threadId, getLevelStr(debugLevel), message);
	
#ifdef LOG_FILE
	//03:22:07,127 000010B4 [DEBUG] CGuiList::CGuiList done
	if (fpLog != NULL)
	{
		fprintf(fpLog, "%s", buf);
		fflush(fpLog);
	}
#endif

#if defined(USE_DEBUG_LOG_TO_VIEW)
		if (guiViewDebugLog)
			guiViewDebugLog->AddLog(buf);
#endif

	fprintf(stdout, "%s", buf);
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
	// FATAL and ERROR are the always-on path: never filtered by MT_DEBUG_LOGS
	// and never by the level mask. Everything else needs both.
	const bool alwaysOn = (level & (DBGLVL_FATAL | DBGLVL_ERROR)) != 0;
#if !MT_DEBUG_LOGS
	if (!alwaysOn)
		return 0;
#endif
	if (!alwaysOn && !logThisLevel(level))
		return 0;

    char buffer[MAX_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
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


