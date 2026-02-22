/*
 *  DBG_Logf.cpp
 *  MobiTracker
 *
 * [C] Marcin Skoczylas
 * debug console/file code 
 *
 */

// date time in win32
// http://stackoverflow.com/questions/1695288/getting-the-current-time-in-milliseconds-from-the-system-clock-in-windows

#include "DBG_ConStream.h"
#include "DBG_Log.h"
//#include "SYS_Defs.h"
#include "CLogByteBuffer.h"
#include "SYS_Threading.h"
#include "SYS_Main.h"

#if !defined(GLOBAL_DEBUG_OFF)

#if defined(USE_DEBUG_LOG_TO_VIEW)
#include "CGuiViewDebugLog.h"
#endif

#define USE_COUT

#define LOG_CONSOLE
#define WIN_CONSOLE
#define LOG_FILE

//#define DEBUG_OFF
//#define FULL_LOG

#define BUFSIZE (32*1024)

/*
 TODO:
 + (NSString *) myFormattedString:(NSString *)format, ... {
 va_list args;
 va_start(args, format);
 
 NSString *str = [[NSString alloc] initWithFormat:format arguments:args];
 [str autorelease];
 
 va_end(args);
 
 return [NSString stringWithFormat:@"Foo: %@.", str];
 }
 */


static int logger_currentLogLevel;
CSlrMutex *loggerMutex;

#ifdef LOG_FILE
FILE *fpLog = NULL;
#endif

#ifdef WIN_CONSOLE
ConStream m_Log;
#endif

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

bool LOG_IsSetLevel(unsigned int level)
{
	return IS_SET(logger_currentLogLevel, level);
}

#ifndef DEBUG_OFF
#ifdef FULL_LOG

bool logThisLevel(int level)
{
	return true;
}

#else

bool shouldLog = true;

bool logThisLevel(int level)
{
	if (shouldLog == false)
		return false;

	return LOG_IsSetLevel(level);
}

#endif // FULL_LOG

#else

bool logThisLevel(int level)
{
//	if (level == DBGLVL_DEBUG) return true;
	return false;
}
#endif


const char *getLevelStr(int level)
{
	if (level == DBGLVL_MAIN)
		return "[MAIN] ";
	if (level == DBGLVL_DEBUG)
		return "[DEBUG]";
	if (level == DBGLVL_INPUT)
		return "[INPUT]";
	if (level == DBGLVL_RES)
		return "[RES]  ";
	if (level == DBGLVL_GUI)
		return "[GUI]  ";
	if (level == DBGLVL_XMPLAYER)
		return "[XM]   ";
	if (level == DBGLVL_AUDIO)
		return "[AUDIO]";
	if (level == DBGLVL_HTTP)
		return "[HTTP] ";
	if (level == DBGLVL_ANIMATION)
		return "[ANIM] ";
	if (level == DBGLVL_MEMORY)
		return "[MEM ] ";
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

// FORMATOWANIE w NSLog jest SPIERDOLONE chociaz sie kompiluje!!
// NSLog(@"%@", s);
// NSLog((@"%s [Line %d] " fmt), __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__);

//#define USE_COUT
CLogByteBuffer *logEventsBuffer;

void DBG_SendLog(int debugLevel, char *message);

char logBuf[512];
HANDLE pipeHandle;

void LOG_Init(void)
{
	loggerMutex = new CSlrMutex("loggerMutex");

	LOG_SetLevel(DBGLVL_MAIN, true);
	LOG_SetLevel(DBGLVL_DEBUG, true);
	LOG_SetLevel(DBGLVL_DEBUG2, true);
	LOG_SetLevel(DBGLVL_TODO, true);
	LOG_SetLevel(DBGLVL_ERROR, true);
	LOG_SetLevel(DBGLVL_WARN, true);

#ifdef LOG_FILE
	SYSTEMTIME tmeCurrent;
	GetLocalTime(&tmeCurrent);

	DWORD processId = GetCurrentProcessId();

	sprintf(logBuf, "./log/MTEngine-%04d%02d%02d-%02d%02d%02d-%d.txt", tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay,
		tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, processId);

	fpLog = fopen(logBuf, "wb");

	if (fpLog == NULL)
	{
		sprintf(logBuf, "MTEngine-%04d%02d%02d-%02d%02d%02d-%d.txt", tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay,
			tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, processId);

		fpLog = fopen(logBuf, "wb");
	}
#endif

#ifdef WIN_CONSOLE
	//m_Log.Open();
#endif

#ifdef LOG_CONSOLE
//	DWORD processId = GetCurrentProcessId();

#ifdef WIN_CONSOLE
	m_Log << "processId=" << processId << std::endl;
	m_Log << "start LogConsole.exe";
#endif

	STARTUPINFO         siStartupInfo;
	PROCESS_INFORMATION piProcessInfo;

    memset(&siStartupInfo, 0, sizeof(siStartupInfo));
    memset(&piProcessInfo, 0, sizeof(piProcessInfo));

    siStartupInfo.cb = sizeof(siStartupInfo);

	//hostProcID.ToString()
      //          + " \"" + LogEngine.Settings.settingsName + "\" \"" + windowCaption + "\"";

//	sprintf(logBuf, " %d \"MTEngine\" \"MTEngine log console (" __DATE__ " " __TIME__ ")\"");
	sprintf(logBuf, " %d \"MTEngine\" \"MTEngine log console\"", processId);

    if(CreateProcess("LogConsole.exe",     // Application name
                     logBuf,                 // Application arguments
                     0,
                     0,
                     FALSE,
                     CREATE_DEFAULT_ERROR_MODE,
                     0,
                     0,                              // Working directory
                     &siStartupInfo,
                     &piProcessInfo) == FALSE)
	{
		DWORD err = GetLastError();
		//m_Log << "error=" << err << std::endl;
	}
	else
	{
		//m_Log << "ok, connect to pipe" << std::endl;
		// connect to pipe
		sprintf(logBuf, "\\\\.\\pipe\\logconsole%d", processId);
	
		while(true)
		{
			pipeHandle = CreateFile(logBuf,
						GENERIC_READ | GENERIC_WRITE,
						0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

			if (pipeHandle != INVALID_HANDLE_VALUE)
				break;

			Sleep(15);
		}
		//m_Log << "pipe connected" << std::endl;
	}

	logEventsBuffer = new CLogByteBuffer(8192);

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

int  LOG_GetCurrentLogLevel()
{
	return logger_currentLogLevel;
}

void LOG_Shutdown(void)
{
	_LOGF(DBGLVL_MAIN, "closing stdlib & logfile\nbye!\n");
	
#ifdef LOG_FILE
	fclose(fpLog);
#endif

}

void LOG_LockMutex()
{
#ifdef WIN_CONSOLE
	//m_Log << "LOCK" << endl;
#endif
	loggerMutex->Lock();
}

void LOG_UnlockMutex()
{
#ifdef WIN_CONSOLE
	//m_Log << "UNLOCK" << endl;
#endif
	loggerMutex->Unlock();
}

int a = 3;	// some stupid workaround for Microsoft's shitty compiler
void DBG_SendLog(int debugLevel, char *message)
{
	if (message == NULL)
	{
		a = 4;
		return;
	}

	SYSTEMTIME tmeCurrent;
	GetLocalTime(&tmeCurrent);

	DWORD threadId = GetCurrentThreadId();

#ifdef WIN_CONSOLE
	//m_Log << message << std::endl;
	//fflush(stdout);
#endif

#ifdef LOG_CONSOLE
	logEventsBuffer->Clear();
	logEventsBuffer->putInt(debugLevel);
	logEventsBuffer->putInt(tmeCurrent.wYear);
	logEventsBuffer->putByte(tmeCurrent.wMonth);
	logEventsBuffer->putByte(tmeCurrent.wDay);
	logEventsBuffer->putByte(tmeCurrent.wHour);
	logEventsBuffer->putByte(tmeCurrent.wMinute);
	logEventsBuffer->putByte(tmeCurrent.wSecond);
	logEventsBuffer->putInt(tmeCurrent.wMilliseconds);
	logEventsBuffer->putString("");	//method
	sprintf(logBuf, "%8.8X", threadId);
	logEventsBuffer->putString(logBuf);	// thread
	logEventsBuffer->putString(message);

	unsigned char sizeBuf[2];
	sizeBuf[0] = (unsigned char)((logEventsBuffer->index) >> 8);
	sizeBuf[1] = (unsigned char)(logEventsBuffer->index);

	DWORD b;
	WriteFile(pipeHandle, sizeBuf, 2, &b, NULL);
	WriteFile(pipeHandle, logEventsBuffer->data, logEventsBuffer->index, &b, NULL);
	FlushFileBuffers(pipeHandle);
#endif

#ifdef LOG_FILE
	//03:22:07,127 000010B4 [DEBUG] CGuiList::CGuiList done
	fprintf(fpLog, "%02d:%02d:%02d,%03d %8.8X %s %s\n", 
		tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, tmeCurrent.wMilliseconds,
		threadId, getLevelStr(debugLevel), message);
	fflush(fpLog);
#endif

#ifdef USE_DEBUG_LOG_TO_VIEW
	//03:22:07,127 000010B4 [DEBUG] CGuiList::CGuiList done
	if (guiViewDebugLog)
		guiViewDebugLog->AddLog("%02d:%02d:%02d,%03d %8.8X %s %s\n",
			tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, tmeCurrent.wMilliseconds,
			threadId, getLevelStr(debugLevel), message);
#endif

}

void DBG_PrintBytes(void *data, unsigned int numBytes)
{
	LOG_LockMutex();
	
	static char buf[2];
//	unsigned char *array = data;
	for (unsigned int i = 0; i < numBytes; i++)
	{
		unsigned char c = ((unsigned char *)data)[i];
		printf("%2.2x ", c);
	}
	fflush(stdout);
	LOG_UnlockMutex();
}

void LOGT(int level, char *what)
{
	if (!logThisLevel(level))
		return;
	_LOGF(level, what);
}

void LOGT(int level, const char *what)
{
	if (!logThisLevel(level))
		return;

	_LOGF(level, what);
}

void _LOGF(int level, char *fmt, ... )
{
    char buffer[BUFSIZE] = {0};

    va_list args;
	
    va_start(args, fmt);
    vsnprintf(buffer, BUFSIZE-2, fmt, args);
    va_end(args);
	
	LOG_LockMutex();

	DBG_SendLog(level, buffer);

	LOG_UnlockMutex();
	
}

void _LOGF(int level, std::string what)
{
	if (!logThisLevel(level))
		return;
	_LOGF(level, what.c_str());
}

void _LOGF(int level, const char *fmt, ... )
{
	if (!logThisLevel(level))
		return;

    char buffer[BUFSIZE] = {0};
	
    va_list args;
	
    va_start(args, fmt);
    vsnprintf(buffer, BUFSIZE-2, fmt, args);
    va_end(args);
	
	LOG_LockMutex();

	DBG_SendLog(level, buffer);

	LOG_UnlockMutex();
}

int _LOGGER(unsigned int level, const char *fileName, unsigned int lineNum, const char *functionName, const char *format, ...)
{
	if (!logThisLevel(level))
		return 0;

	char buffer[BUFSIZE] = {0};

	va_list args;

	va_start(args, format);
	vsnprintf(buffer, BUFSIZE-2, format, args);
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
	
    char buffer[BUFSIZE] = {0};
	
    va_list args;
	
    va_start(args, fmt);
    vsnprintf(buffer, BUFSIZE - 2, fmt, args);
    va_end(args);
	
	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ERROR, buffer);

	LOG_UnlockMutex();
}

void SYS_Errorf(const char *fmt, ... )
{
	//m_Log << "ERROR:" << std::endl;
	
    char buffer[BUFSIZE] = {0};
	
    va_list args;
	
    va_start(args, fmt);
    vsnprintf(buffer, BUFSIZE - 2, fmt, args);
    va_end(args);
	
	LOG_LockMutex();

	DBG_SendLog(DBGLVL_ERROR, buffer);

	LOG_UnlockMutex();
}

/*
 void Byte2Hex2digits(unsigned char value, char *bufOut)
 {
 unsigned char c1;
 unsigned char c2;
 
 c1 = (unsigned char)(value & 0xF0);
 c1 = (unsigned char)(value >> 4);
 
 c2 = (unsigned char)(value & 0x0F);
 
 bufOut[0] = (unsigned char)hexTable[c1];
 bufOut[1] = (unsigned char)hexTable[c2];
 
 }
 
*/

#else

void LOG_Init(void) {}
void LOG_SetLevel(unsigned int level, bool isOn) {}
void LOG_BackupCurrentLogLevel() {}
void LOG_RestoreBackupLogLevel() {}
void LOG_SetCurrentLogLevel(int level) {}
void LOG_Shutdown(void) {}

#endif
