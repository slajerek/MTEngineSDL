#include "SYS_MiniDump.h"
#include "DBG_Log.h"
#include <time.h>

// http://www.debuginfo.com/examples/effmdmpexamples.html

#pragma comment(lib, "dbghelp.lib")

// ─── helpers ────────────────────────────────────────────────────────────────

static const char* ExceptionCodeToString(DWORD code)
{
	switch (code)
	{
		case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
		case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
		case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
		case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
		case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
		case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
		case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
		case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
		case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
		case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
		case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
		case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
		case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
		case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
		case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
		case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
		default:                                 return "UNKNOWN_EXCEPTION";
	}
}

// Write a formatted line to both the crash log file and stderr
static void CrashLog(FILE* fp, const char* fmt, ...)
{
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	fprintf(stderr, "%s", buf);
	if (fp)
	{
		fprintf(fp, "%s", buf);
		fflush(fp);
	}
}

// ─── stack walk ─────────────────────────────────────────────────────────────

static void DumpCallStack(FILE* fp, EXCEPTION_POINTERS* pep)
{
	HANDLE hProcess = GetCurrentProcess();
	HANDLE hThread  = GetCurrentThread();

	// Initialize symbol handler
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	if (!SymInitialize(hProcess, NULL, TRUE))
	{
		CrashLog(fp, "  SymInitialize failed (error %lu) - cannot walk stack\n", GetLastError());
		return;
	}

	CONTEXT ctx = {};
	if (pep && pep->ContextRecord)
	{
		ctx = *pep->ContextRecord;
	}
	else
	{
		RtlCaptureContext(&ctx);
	}

	STACKFRAME64 frame = {};
	DWORD machineType = 0;

#ifdef _M_X64
	machineType = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset    = ctx.Rip;
	frame.AddrPC.Mode      = AddrModeFlat;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrFrame.Mode   = AddrModeFlat;
	frame.AddrStack.Offset = ctx.Rsp;
	frame.AddrStack.Mode   = AddrModeFlat;
#elif defined(_M_IX86)
	machineType = IMAGE_FILE_MACHINE_I386;
	frame.AddrPC.Offset    = ctx.Eip;
	frame.AddrPC.Mode      = AddrModeFlat;
	frame.AddrFrame.Offset = ctx.Ebp;
	frame.AddrFrame.Mode   = AddrModeFlat;
	frame.AddrStack.Offset = ctx.Esp;
	frame.AddrStack.Mode   = AddrModeFlat;
#elif defined(_M_ARM64)
	machineType = IMAGE_FILE_MACHINE_ARM64;
	frame.AddrPC.Offset    = ctx.Pc;
	frame.AddrPC.Mode      = AddrModeFlat;
	frame.AddrFrame.Offset = ctx.Fp;
	frame.AddrFrame.Mode   = AddrModeFlat;
	frame.AddrStack.Offset = ctx.Sp;
	frame.AddrStack.Mode   = AddrModeFlat;
#else
	CrashLog(fp, "  Unsupported architecture for stack walk\n");
	SymCleanup(hProcess);
	return;
#endif

	// Symbol buffer (max name length 512)
	char symbolBuf[sizeof(SYMBOL_INFO) + 512];
	SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuf;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen   = 511;

	IMAGEHLP_LINE64 lineInfo = {};
	lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

	CrashLog(fp, "\n--- Call Stack ---\n");

	for (int i = 0; i < 128; i++)
	{
		if (!StackWalk64(machineType, hProcess, hThread, &frame, &ctx,
		                 NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
		{
			break;
		}

		if (frame.AddrPC.Offset == 0)
			break;

		DWORD64 addr = frame.AddrPC.Offset;

		// Get module name
		char moduleName[MAX_PATH] = "<unknown>";
		HMODULE hModule = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   (LPCSTR)(uintptr_t)addr, &hModule);
		if (hModule)
		{
			GetModuleFileNameA(hModule, moduleName, MAX_PATH);
			// Extract just the filename
			char* lastSlash = strrchr(moduleName, '\\');
			if (lastSlash) memmove(moduleName, lastSlash + 1, strlen(lastSlash + 1) + 1);
		}

		// Get symbol name
		DWORD64 displacement64 = 0;
		const char* funcName = "<unknown>";
		if (SymFromAddr(hProcess, addr, &displacement64, symbol))
		{
			funcName = symbol->Name;
		}

		// Get source file + line
		DWORD displacement32 = 0;
		if (SymGetLineFromAddr64(hProcess, addr, &displacement32, &lineInfo))
		{
			CrashLog(fp, "  [%2d] 0x%016llX  %s!%s + 0x%llX  (%s:%lu)\n",
			         i, (unsigned long long)addr, moduleName, funcName,
			         (unsigned long long)displacement64,
			         lineInfo.FileName, (unsigned long)lineInfo.LineNumber);
		}
		else
		{
			CrashLog(fp, "  [%2d] 0x%016llX  %s!%s + 0x%llX\n",
			         i, (unsigned long long)addr, moduleName, funcName,
			         (unsigned long long)displacement64);
		}
	}

	CrashLog(fp, "--- End Call Stack ---\n\n");

	SymCleanup(hProcess);
}

// ─── minidump creation ──────────────────────────────────────────────────────

static void WriteDumpFile(const char* dumpPath, EXCEPTION_POINTERS* pep, MINIDUMP_TYPE mdt)
{
	HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE,
	                           0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == NULL || hFile == INVALID_HANDLE_VALUE)
		return;

	MINIDUMP_EXCEPTION_INFORMATION mdei = {};
	mdei.ThreadId          = GetCurrentThreadId();
	mdei.ExceptionPointers = pep;
	mdei.ClientPointers    = FALSE;

	MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
	                  hFile, mdt, (pep != 0) ? &mdei : 0, 0, 0);
	CloseHandle(hFile);
}

// ─── public API ─────────────────────────────────────────────────────────────

void SYS_CreateMiniDump(EXCEPTION_POINTERS* pep)
{
	MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
	WriteDumpFile("MTEngineCrash.dmp", pep, mdt);
}

void SYS_CreateMaxiDump(EXCEPTION_POINTERS* pep)
{
	MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(MiniDumpWithFullMemory |
	                                     MiniDumpWithFullMemoryInfo |
	                                     MiniDumpWithHandleData |
	                                     MiniDumpWithThreadInfo |
	                                     MiniDumpWithUnloadedModules);
	WriteDumpFile("MTEngineCrash.dmp", pep, mdt);
}

// ─── top-level exception handler ────────────────────────────────────────────

static LONG WINAPI SYS_CrashExceptionFilter(EXCEPTION_POINTERS* pep)
{
	// Generate timestamped filenames
	SYSTEMTIME t;
	GetLocalTime(&t);
	DWORD pid = GetCurrentProcessId();

	char crashLogPath[MAX_PATH];
	char dumpPath[MAX_PATH];
	snprintf(crashLogPath, MAX_PATH, "MTEngine-crash-%04d%02d%02d-%02d%02d%02d-%lu.txt",
	         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (unsigned long)pid);
	snprintf(dumpPath, MAX_PATH, "MTEngine-crash-%04d%02d%02d-%02d%02d%02d-%lu.dmp",
	         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (unsigned long)pid);

	// Open crash log file
	FILE* fp = fopen(crashLogPath, "w");

	DWORD exCode = pep->ExceptionRecord->ExceptionCode;
	void* exAddr = pep->ExceptionRecord->ExceptionAddress;

	CrashLog(fp, "=== MTEngine CRASH REPORT ===\n");
	CrashLog(fp, "Exception: 0x%08lX (%s)\n", (unsigned long)exCode, ExceptionCodeToString(exCode));
	CrashLog(fp, "Address:   0x%p\n", exAddr);
	CrashLog(fp, "Thread:    %lu\n", (unsigned long)GetCurrentThreadId());
	CrashLog(fp, "Process:   %lu\n", (unsigned long)pid);

	// For access violations, show read/write and target address
	if (exCode == EXCEPTION_ACCESS_VIOLATION && pep->ExceptionRecord->NumberParameters >= 2)
	{
		ULONG_PTR rwFlag = pep->ExceptionRecord->ExceptionInformation[0];
		ULONG_PTR targetAddr = pep->ExceptionRecord->ExceptionInformation[1];
		const char* rwStr = (rwFlag == 0) ? "reading" : (rwFlag == 1) ? "writing" : "executing";
		CrashLog(fp, "Access violation %s address 0x%p\n", rwStr, (void*)targetAddr);
	}

	// Dump the call stack
	DumpCallStack(fp, pep);

	// Write the minidump file
	MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
	                                     MiniDumpScanMemory |
	                                     MiniDumpWithThreadInfo |
	                                     MiniDumpWithUnloadedModules);

	HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE,
	                           0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	BOOL dumpOk = FALSE;
	if (hFile != NULL && hFile != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION mdei = {};
		mdei.ThreadId          = GetCurrentThreadId();
		mdei.ExceptionPointers = pep;
		mdei.ClientPointers    = FALSE;

		dumpOk = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
		                           hFile, mdt, &mdei, 0, 0);
		CloseHandle(hFile);
	}

	CrashLog(fp, "Crash log: %s\n", crashLogPath);
	if (dumpOk)
		CrashLog(fp, "Minidump:  %s\n", dumpPath);
	else
		CrashLog(fp, "Minidump:  FAILED to create\n");

	CrashLog(fp, "=== END CRASH REPORT ===\n");

	if (fp)
		fclose(fp);

	// Show a message box so the user knows what happened
	char msg[1024];
	snprintf(msg, sizeof(msg),
	         "MTEngine crashed!\n\n"
	         "Exception: %s (0x%08lX)\n\n"
	         "Crash log saved to:\n%s\n\n"
	         "Minidump saved to:\n%s\n\n"
	         "Please send these files for diagnosis.",
	         ExceptionCodeToString(exCode), (unsigned long)exCode,
	         crashLogPath,
	         dumpOk ? dumpPath : "(failed to create)");

	MessageBoxA(NULL, msg, "MTEngine Crash", MB_OK | MB_ICONERROR);

	return EXCEPTION_EXECUTE_HANDLER;
}

void SYS_InstallCrashHandler()
{
	SetUnhandledExceptionFilter(SYS_CrashExceptionFilter);
}
