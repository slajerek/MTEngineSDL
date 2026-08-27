// MT_CrashReporter_Windows.cpp
// Windows crash reporter: Vectored Exception Handler, MiniDump, StackWalk64,
// TaskDialog UI.  Compiled only on Windows (_WIN32).

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <strsafe.h>
#include <io.h>        // _open_osfhandle, _close, _write
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <atomic>
#include "MT_CrashReporter.h"
#include "DBG_Log.h"
#include "SYS_CommandLine.h"

// Forward declaration — defined later in this file
void MT_CrashReporter_SpawnHelperWin32(const char *reportPath);

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

// ------------------------------------------------------------------
// Exception handler (Vectored + UnhandledExceptionFilter)
// ------------------------------------------------------------------

static std::atomic<bool> gCrashHandled{false};

static LONG WINAPI CrashExceptionHandler(EXCEPTION_POINTERS *ep)
{
    // One-shot guard: SetUnhandledExceptionFilter can fire from multiple
    // threads concurrently (e.g. if two threads crash simultaneously).
    bool expected = false;
    if (!gCrashHandled.compare_exchange_strong(expected, true))
        return EXCEPTION_CONTINUE_SEARCH;

    // Use the pre-baked path set at Init() — read-only after Install(),
    // so it is safe to read here without locks or heap allocation.
    const char *reportPath = MT_CrashReporter_GetPrebakedPath();

    static const char fallbackPath[] = "C:\\Temp\\app-crash.txt";
    if (!reportPath || reportPath[0] == '\0')
        reportPath = fallbackPath;

    // ------------------------------------------------------------------
    // Write .dmp sidecar alongside the .txt (replace .txt -> .dmp).
    // ------------------------------------------------------------------
    char dmpPath[700]{};
    StringCchCopyA(dmpPath, sizeof(dmpPath), reportPath);
    char *ext = strrchr(dmpPath, '.');
    if (ext) StringCchCopyA(ext, 5, ".dmp");

    HANDLE hDmp = CreateFileA(dmpPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDmp != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          hDmp, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hDmp);
    }

    // ------------------------------------------------------------------
    // Write text report.
    // ------------------------------------------------------------------
    HANDLE hFile = CreateFileA(reportPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        // Wrap the HANDLE in a POSIX fd so MT_CrashReporter_WriteReportFd
        // can use _write() internally.  _close(fd) also closes hFile.
        int fd = _open_osfhandle((intptr_t)hFile, 0);
        if (fd >= 0)
        {
            DWORD exCode   = ep ? ep->ExceptionRecord->ExceptionCode    : 0;
            void *faultAddr = ep ? ep->ExceptionRecord->ExceptionAddress : nullptr;

            // Map Win32 exception code to a POSIX-like signal number for display.
            int sig = (exCode == EXCEPTION_ACCESS_VIOLATION)   ? 11 :
                      (exCode == EXCEPTION_ILLEGAL_INSTRUCTION) ? 4  :
                      (exCode == EXCEPTION_INT_DIVIDE_BY_ZERO)  ? 8  :
                      (int)exCode;

            MT_CrashReporter_WriteReportFd(fd, sig, faultAddr);

            // ---- StackWalk64 trace ----
            static const char stackHdr[] = "\n--- Stack Trace (StackWalk64) ---\n";
            _write(fd, stackHdr, (unsigned int)(sizeof(stackHdr) - 1));

            SymInitialize(GetCurrentProcess(), nullptr, TRUE);

            CONTEXT ctx{};
            if (ep)
                ctx = *ep->ContextRecord;
            else
                RtlCaptureContext(&ctx);

            STACKFRAME64 sf{};
#if defined(_M_ARM64) || defined(_M_ARM64EC)
            // ARM64's CONTEXT has no Rip/Rbp/Rsp (those are x64 register
            // names) -- Pc/Fp/Sp are the equivalents (program counter,
            // frame pointer X29, stack pointer SP).
            DWORD machineType          = IMAGE_FILE_MACHINE_ARM64;
            sf.AddrPC.Offset           = ctx.Pc;
            sf.AddrPC.Mode             = AddrModeFlat;
            sf.AddrFrame.Offset        = ctx.Fp;
            sf.AddrFrame.Mode          = AddrModeFlat;
            sf.AddrStack.Offset        = ctx.Sp;
            sf.AddrStack.Mode          = AddrModeFlat;
#else
            DWORD machineType          = IMAGE_FILE_MACHINE_AMD64;
            sf.AddrPC.Offset           = ctx.Rip;
            sf.AddrPC.Mode             = AddrModeFlat;
            sf.AddrFrame.Offset        = ctx.Rbp;
            sf.AddrFrame.Mode          = AddrModeFlat;
            sf.AddrStack.Offset        = ctx.Rsp;
            sf.AddrStack.Mode          = AddrModeFlat;
#endif

            char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
            SYMBOL_INFO *sym           = (SYMBOL_INFO *)symBuf;
            sym->SizeOfStruct          = sizeof(SYMBOL_INFO);
            sym->MaxNameLen            = MAX_SYM_NAME;

            for (int frame = 0; frame < 64; frame++)
            {
                if (!StackWalk64(machineType,
                                 GetCurrentProcess(), GetCurrentThread(),
                                 &sf, &ctx, nullptr,
                                 SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr))
                    break;
                if (sf.AddrPC.Offset == 0) break;

                char lineBuf[256]{};
                DWORD64 disp = 0;
                if (SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &disp, sym))
                    snprintf(lineBuf, sizeof(lineBuf), "  [%02d] %s + 0x%llx\n",
                             frame, sym->Name, (unsigned long long)disp);
                else
                    snprintf(lineBuf, sizeof(lineBuf), "  [%02d] 0x%016llx\n",
                             frame, (unsigned long long)sf.AddrPC.Offset);

                _write(fd, lineBuf, (unsigned int)strlen(lineBuf));
            }

            _close(fd);  // also closes hFile
        }
        else
        {
            CloseHandle(hFile);
        }
    }

    // Append minidump path reference to the txt report so it's discoverable.
    {
        std::ofstream ofs(reportPath, std::ios::app);
        if (ofs.is_open())
            ofs << "\nMinidump: " << dmpPath << "\n";
    }

    // Spawn a fresh process to show the dialog (we are inside the handler).
    MT_CrashReporter_SpawnHelperWin32(reportPath);

    // Let the OS handle it (writes WER report / shows standard crash dialog).
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------
// Install
// ------------------------------------------------------------------

void MT_CrashReporter_InstallPlatform()
{
    // SetUnhandledExceptionFilter fires only for truly unhandled (fatal)
    // exceptions — after all VEH and SEH handlers have declined. This means
    // C++ exceptions caught by try/catch never reach us, so no false-positive
    // crash dialogs on startup. AddVectoredExceptionHandler fires for ALL
    // exceptions (including handled ones), so we intentionally do NOT install
    // a VEH here.
    SetUnhandledExceptionFilter(CrashExceptionHandler);
}

// ------------------------------------------------------------------
// SpawnHelperWin32 — called from MT_CrashReporter_SpawnHelper() in
// MT_CrashReporter.cpp (declared extern there).
// ------------------------------------------------------------------

void MT_CrashReporter_SpawnHelperWin32(const char *reportPath)
{
    const char *selfPath = SYS_GetArgv()[0];
    if (!selfPath || selfPath[0] == '\0') return;

    // Build command line: "<self>" --show-crash-report "<reportPath>"
    char cmdLine[1200]{};
    snprintf(cmdLine, sizeof(cmdLine),
             "\"%s\" --show-crash-report \"%s\"", selfPath, reportPath);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi))
    {
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);
    }
}

// ------------------------------------------------------------------
// ShowDialog — shown in the spawned helper process
// ------------------------------------------------------------------

void MT_CrashReporter_ShowDialog(const char *reportPath)
{
    // Read crash report text from disk.
    std::ifstream ifs(reportPath);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string reportText = oss.str();

    // TaskDialog requires comctl32 v6 (controlled via application manifest
    // on modern Windows — Visual Studio projects include this by default).
    TASKDIALOGCONFIG tdc{};
    tdc.cbSize        = sizeof(tdc);
    tdc.hwndParent    = nullptr;
    tdc.hInstance     = GetModuleHandle(nullptr);
    tdc.dwFlags       = TDF_ENABLE_HYPERLINKS | TDF_EXPANDED_BY_DEFAULT |
                        TDF_USE_COMMAND_LINKS;
    tdc.pszWindowTitle      = L"Application crashed";
    tdc.pszMainInstruction  = L"The application has crashed";
    tdc.pszContent          = L"A crash report has been written to disk.";

    // Truncate report at 2 000 chars — TaskDialog has display limits.
    std::string display = reportText.substr(0, 2000);
    std::wstring wDisplay(display.begin(), display.end());
    tdc.pszExpandedInformation    = wDisplay.c_str();
    tdc.pszCollapsedControlText   = L"Show crash report";
    tdc.pszExpandedControlText    = L"Hide crash report";

    // Custom buttons.
    static const TASKDIALOG_BUTTON buttons[] = {
        { 100, L"Copy Report to Clipboard" },
        { 101, L"Open Report File" },
        { 102, L"Close" },
    };
    tdc.pButtons       = buttons;
    tdc.cButtons       = 3;
    tdc.nDefaultButton = 102;
    tdc.dwCommonButtons = 0;

    int clicked = 0;
    HRESULT hr = TaskDialogIndirect(&tdc, &clicked, nullptr, nullptr);

    if (FAILED(hr))
    {
        // TaskDialog unavailable (very old Windows or missing manifest).
        // Fall back to a simple MessageBox showing the report path.
        std::wstring wPath(reportPath, reportPath + strlen(reportPath));
        std::wstring msg = L"The application crashed.\n\nReport written to:\n" + wPath;
        MessageBoxW(nullptr, msg.c_str(), L"Crash", MB_OK | MB_ICONERROR);
        return;
    }

    if (clicked == 100)
    {
        // Copy full report text to clipboard.
        if (OpenClipboard(nullptr))
        {
            std::wstring wText(reportText.begin(), reportText.end());
            size_t bytes = (wText.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem)
            {
                memcpy(GlobalLock(hMem), wText.c_str(), bytes);
                GlobalUnlock(hMem);
                EmptyClipboard();
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
            CloseClipboard();
        }
    }
    else if (clicked == 101)
    {
        // Open report file in the default text viewer (usually Notepad).
        std::wstring wPath(reportPath, reportPath + strlen(reportPath));
        ShellExecuteW(nullptr, L"open", wPath.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
}

#endif // _WIN32
