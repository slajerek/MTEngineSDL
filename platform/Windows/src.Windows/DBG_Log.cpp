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
#include "SYS_FileSystem.h"
#include <cstring>

// The logger is ALWAYS compiled; MT_DEBUG_LOGS gates only the verbose macros
// in DBG_Log.h. See that header.

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


static int logger_currentLogLevel = (int)DBGLVL_DEFAULT_MASK;
CSlrMutex *loggerMutex = NULL;
static char logger_filePath[4096] = {0};

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

bool logThisLevel(unsigned int level)
{
	return true;
}

#else

bool shouldLog = true;

bool logThisLevel(unsigned int level)
{
	if (shouldLog == false)
		return false;

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


const char *getLevelStr(int level)
{
	if (level == DBGLVL_FATAL)
		return "[FATAL]";
	if (level == DBGLVL_PAINT)
		return "[PAINT]";
	if (level == DBGLVL_ADS)
		return "[ADS]  ";
	if (level == DBGLVL_WEBSERVICE)
		return "[WEBSV]";
	if (level == DBGLVL_WARN)
		return "[WARN] ";
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
CLogByteBuffer *logEventsBuffer = NULL;

void DBG_SendLog(int debugLevel, char *message);

char logBuf[512];
HANDLE pipeHandle = INVALID_HANDLE_VALUE;

void LOG_Init(void)
{
	if (loggerMutex == NULL)
		loggerMutex = new CSlrMutex("loggerMutex");

	// ONE default, the same on every platform: DBGLVL_DEFAULT_MASK in DBG_Log.h.
	logger_currentLogLevel = (int)DBGLVL_DEFAULT_MASK;

LOG_SetLevel(DBGLVL_DEBUG, true);
	LOG_SetLevel(DBGLVL_DEBUG2, true);
	LOG_SetLevel(DBGLVL_TODO, true);
	LOG_SetLevel(DBGLVL_ERROR, true);
	LOG_SetLevel(DBGLVL_WARN, true);

#ifdef LOG_FILE
	SYSTEMTIME tmeCurrent;
	GetLocalTime(&tmeCurrent);

	DWORD processId = GetCurrentProcessId();

	// --log-dir <path>: read from the CRT's own __argc/__argv rather than
	// SYS_GetArgv(), because LOG_Init() runs before
	// SYS_SetCommandLineArguments() in every platform's main() (Windows
	// included). macOS's DBG_Log.mm reads NSProcessInfo directly for the
	// identical reason; __argc/__argv are populated by CRT startup before
	// main() runs, so they are always safe to read here regardless of that
	// ordering. Previously this platform had no --log-dir support at all, so
	// every log landed in .\log\ (or, when that didn't exist, the process's
	// current directory) no matter what the caller asked for.
	const char *logDirArg = NULL;
	for (int i = 1; i < __argc - 1; i++)
	{
		if (strcmp(__argv[i], "--log-dir") == 0)
		{
			logDirArg = __argv[i + 1];
			break;
		}
	}

	if (logDirArg != NULL)
	{
		CreateDirectoryA(logDirArg, NULL);
		sprintf(logBuf, "%s\\MTEngine-%04d%02d%02d-%02d%02d%02d-%d.txt", logDirArg, tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay,
			tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, processId);
	}
	else
	{
		// NO --log-dir. Two candidates, and NEITHER of them is the bare current
		// directory any more.
		//
		// It used to be `.\log\` with a fallback to the CWD, and since nothing
		// ever CREATED `.\log\` (the --log-dir branch above calls
		// CreateDirectoryA; this one never did), the fallback is what actually
		// ran, every time. So the log landed in whatever directory the process
		// happened to start in -- which for a build agent, or for F5 in Visual
		// Studio, is INSIDE THE CHECKOUT. `log/` is gitignored in every app;
		// a bare MTEngine-*.txt in the repo root is not. That is where the
		// stray log dumps in the app repos came from (one host app had six at
		// its repo root and three more beside its .vcxproj).
		//
		// `.\log\` is still honoured when it EXISTS, because that is the
		// documented behaviour and creating the directory is a clear opt-in.
		// Otherwise the log goes to %TEMP%\MTEngine\, which is the right home
		// for a file nobody asked for by name.
		DWORD logDirAttrs = GetFileAttributesA(".\\log");
		if (logDirAttrs != INVALID_FILE_ATTRIBUTES && (logDirAttrs & FILE_ATTRIBUTE_DIRECTORY))
		{
			sprintf(logBuf, "./log/MTEngine-%04d%02d%02d-%02d%02d%02d-%d.txt", tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay,
				tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, processId);
		}
		else
		{
			char tempPath[MAX_PATH];
			DWORD tempLen = GetTempPathA(MAX_PATH, tempPath);
			if (tempLen > 0 && tempLen < MAX_PATH)
			{
				// GetTempPathA always ends with a backslash.
				sprintf(logBuf, "%sMTEngine", tempPath);
				CreateDirectoryA(logBuf, NULL);
				sprintf(logBuf, "%sMTEngine\\MTEngine-%04d%02d%02d-%02d%02d%02d-%d.txt", tempPath,
					tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay,
					tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, processId);
			}
			else
			{
				// No TEMP at all. Create .\log\ rather than spray the CWD.
				CreateDirectoryA(".\\log", NULL);
				sprintf(logBuf, "./log/MTEngine-%04d%02d%02d-%02d%02d%02d-%d.txt", tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay,
					tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, processId);
			}
		}
	}

	fpLog = SYS_OpenFile(logBuf, "wb");

	if (fpLog == NULL)
	{
		// Last resort. Still not the CWD: create the directory the chosen path
		// wanted and retry there, so a missing directory cannot silently
		// redirect the log into a source tree.
		CreateDirectoryA(".\\log", NULL);
		sprintf(logBuf, "./log/MTEngine-%04d%02d%02d-%02d%02d%02d-%d.txt", tmeCurrent.wYear, tmeCurrent.wMonth, tmeCurrent.wDay,
			tmeCurrent.wHour, tmeCurrent.wMinute, tmeCurrent.wSecond, processId);

		fpLog = SYS_OpenFile(logBuf, "wb");
	}

	// What LOG_GetLogFilePath() returns. macOS and Linux record theirs the
	// same way; this platform declared the buffer and never filled it, so
	// the path came back empty and CTestLoggingAlwaysOn failed at step 1.
	if (fpLog != NULL)
		snprintf(logger_filePath, sizeof(logger_filePath), "%s", logBuf);
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

	// BESIDE THE EXECUTABLE, never the current directory: the cwd is the git
	// root for a development build and a package for a final one, and neither
	// holds the console. Legacy fallback: platform\Windows\_RUNTIME_ relative to
	// the exe's own directory, where it has always lived.
	char consolePath[MAX_PATH * 2] = {0};
	{
		char exeDir[MAX_PATH] = {0};
		DWORD n = GetModuleFileNameA(NULL, exeDir, MAX_PATH);
		if (n > 0 && n < MAX_PATH)
		{
			char *slash = strrchr(exeDir, '\\');
			if (slash) *slash = 0;
			snprintf(consolePath, sizeof(consolePath), "%s\\LogConsole.exe", exeDir);
			if (GetFileAttributesA(consolePath) == INVALID_FILE_ATTRIBUTES)
				snprintf(consolePath, sizeof(consolePath), "%s\\..\\..\\..\\_RUNTIME_\\LogConsole.exe", exeDir);
			if (GetFileAttributesA(consolePath) == INVALID_FILE_ATTRIBUTES)
				consolePath[0] = 0;
		}
	}

    if(consolePath[0] == 0 ||
       CreateProcess(consolePath,          // Application name
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
	
		// BOUNDED: a console that started but never opened its pipe used to
		// hang the process here forever. Two seconds, then log without it.
		for (int attempt = 0; attempt < 130; attempt++)
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
	if (fpLog != NULL)
		fclose(fpLog);
	fpLog = NULL;
#endif

}

const char *LOG_GetLogFilePath(void)
{
	return logger_filePath;
}

void LOG_LockMutex()
{
	// A LOGError before LOG_Init (a crash handler, a static initialiser) must
	// not dereference a NULL mutex; it logs unlocked instead.
	if (loggerMutex != NULL)
		loggerMutex->Lock();
}

void LOG_UnlockMutex()
{
	if (loggerMutex != NULL)
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
	if (logEventsBuffer == NULL)
		logEventsBuffer = new CLogByteBuffer(8192);
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

	if (pipeHandle != INVALID_HANDLE_VALUE)
	{
		DWORD b;
		WriteFile(pipeHandle, sizeBuf, 2, &b, NULL);
		WriteFile(pipeHandle, logEventsBuffer->data, logEventsBuffer->index, &b, NULL);
		FlushFileBuffers(pipeHandle);
	}
#endif

	// FATAL and ERROR also go to the debugger, always: this is what a
	// developer sees in Visual Studio's Output pane for a build with logs off.
	if (debugLevel == DBGLVL_FATAL || debugLevel == DBGLVL_ERROR)
	{
		OutputDebugStringA(message);
		OutputDebugStringA("\n");
	}

#ifdef LOG_FILE
	//03:22:07,127 000010B4 [DEBUG] CGuiList::CGuiList done
	if (fpLog != NULL)
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

void _LOGF(unsigned int level, char *fmt, ... )
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
	// FATAL and ERROR are the always-on path: never filtered by MT_DEBUG_LOGS
	// and never by the level mask. Everything else needs both.
	const bool alwaysOn = (level & (DBGLVL_FATAL | DBGLVL_ERROR)) != 0;
#if !MT_DEBUG_LOGS
	if (!alwaysOn)
		return 0;
#endif
	if (!alwaysOn && !logThisLevel(level))
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

