#include "MT_CrashReporter.h"
#include "SYS_CommandLine.h"
#include "SYS_FileSystem.h"
#include "MT_API.h"
#include "MT_VERSION.h"
#include "SYS_Main.h"
#include "DBG_Log.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <cerrno>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#elif defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#define write  _write
#define open   _open
#define close  _close
#endif

extern char *gCPathToSettings;
extern bool gHeadlessMode;
extern bool gServiceMode;

namespace {

MT_CrashContext gContext{};
bool gEnabled = true;
bool gInstalled = false;
bool gForceEnabled = false;  // --crash-reporter flag

// Written once during Init(), read-only after Install().
char gReportDir[600]{};                 // <settingsDir>/crash-reports
char gSelfPath[1024]{};                 // argv[0]
char gSignalHandlerReportPath[700]{};   // pre-baked path used by signal handler

} // namespace

const MT_CrashContext *MT_CrashReporter_GetContext() { return &gContext; }

void MT_CrashReporter_SetEnabled(bool enabled) { gEnabled = enabled; }

const char *MT_CrashReporter_GetPrebakedPath() { return gSignalHandlerReportPath; }

void MT_CrashContext_SetCurrentFolder(const char *path)
{
    if (path) strncpy(gContext.currentFolder, path, sizeof(gContext.currentFolder) - 1);
    else gContext.currentFolder[0] = '\0';
}

void MT_CrashContext_SetCurrentImage(const char *path)
{
    if (path) strncpy(gContext.currentImage, path, sizeof(gContext.currentImage) - 1);
    else gContext.currentImage[0] = '\0';
}

void MT_CrashContext_SetActiveDecodePath(const char *path)
{
    if (path) strncpy(gContext.activeDecodePath, path, sizeof(gContext.activeDecodePath) - 1);
    else gContext.activeDecodePath[0] = '\0';
}

bool MT_CrashReporter_CheckEarlyExit()
{
    int argc = SYS_GetArgc();
    const char **argv = SYS_GetArgv();

    // Store self path for later use by SpawnHelper.
    if (argc > 0 && argv[0])
        strncpy(gSelfPath, argv[0], sizeof(gSelfPath) - 1);

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--show-crash-report") == 0 && i + 1 < argc)
        {
            MT_CrashReporter_ShowDialog(argv[i + 1]);
            return true;
        }
        if (strcmp(argv[i], "--no-crash-reporter") == 0)
            gEnabled = false;
        if (strcmp(argv[i], "--crash-reporter") == 0)
            gForceEnabled = true;
    }
    return false;
}

void MT_CrashReporter_Init()
{
    // Pre-bake app info into context.
    strncpy(gContext.appTitle, MT_GetMainWindowTitle(), sizeof(gContext.appTitle) - 1);
    strncpy(gContext.appVersion, MT_VERSION_STRING, sizeof(gContext.appVersion) - 1);
    snprintf(gContext.buildInfo, sizeof(gContext.buildInfo), "%s %s %s %s",
             SYS_GetPlatformNameString(), SYS_GetPlatformArchitectureString(),
             __DATE__, __TIME__);

    // Build command line string.
    gContext.commandLine[0] = '\0';
    int argc = SYS_GetArgc();
    const char **argv = SYS_GetArgv();
    for (int i = 0; i < argc; i++)
    {
        if (i > 0) strncat(gContext.commandLine, " ", sizeof(gContext.commandLine) - strlen(gContext.commandLine) - 1);
        strncat(gContext.commandLine, argv[i], sizeof(gContext.commandLine) - strlen(gContext.commandLine) - 1);
    }

    // Pre-bake report directory: <settingsDir>/crash-reports/
    if (gCPathToSettings && gCPathToSettings[0])
    {
        snprintf(gContext.settingsDir, sizeof(gContext.settingsDir), "%s", gCPathToSettings);
        // Ensure exactly one slash between settingsDir and subdirectory name.
        size_t sLen = strlen(gCPathToSettings);
        bool hasSlash = sLen > 0 && gCPathToSettings[sLen - 1] == '/';
        snprintf(gReportDir, sizeof(gReportDir), "%s%scrash-reports",
                 gCPathToSettings, hasSlash ? "" : "/");
    }
    else
    {
        snprintf(gReportDir, sizeof(gReportDir), "/tmp/crash-reports");
        snprintf(gContext.settingsDir, sizeof(gContext.settingsDir), "/tmp/");
    }

    // Create crash-reports directory now (safe — not in signal handler).
    std::error_code ec;
    std::filesystem::create_directories(gReportDir, ec);

    // Pre-bake the path used by the signal handler (no snprintf at crash time).
    // Uses a fixed filename — overwritten on each crash, which is acceptable.
    {
        size_t rLen = strlen(gReportDir);
        const char *title = gContext.appTitle[0] ? gContext.appTitle : "app";
        snprintf(gSignalHandlerReportPath, sizeof(gSignalHandlerReportPath),
                 "%s/%s-crash.txt", gReportDir, title);
    }
}

void MT_CrashReporter_Install()
{
    // Check headless/service/test mode — suppress unless force-enabled.
    if (!gForceEnabled && (!gEnabled || gHeadlessMode || gServiceMode))
    {
        LOGM("CrashReporter: disabled (headless=%d service=%d enabled=%d)",
             (int)gHeadlessMode, (int)gServiceMode, (int)gEnabled);
        return;
    }

    MT_CrashReporter_InstallPlatform();
    gInstalled = true;
    LOGM("CrashReporter: installed for '%s'", gContext.appTitle);
}

// Build a timestamped report file path into outPath[outPathSize].
// Returns false if gReportDir is not set.
bool MT_CrashReporter_BuildReportPath(char *outPath, size_t outPathSize)
{
    if (gReportDir[0] == '\0') return false;

    time_t t = time(nullptr);
    struct tm *tm = localtime(&t);
    char timestamp[32]{};
    snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d-%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    // Sanitize app title for filename (replace spaces with hyphens, lowercase).
    char safeTitle[64]{};
    const char *src = gContext.appTitle;
    char *dst = safeTitle;
    while (*src && dst < safeTitle + sizeof(safeTitle) - 1)
    {
        char c = *src++;
        *dst++ = (c == ' ') ? '-' : (char)tolower((unsigned char)c);
    }

    snprintf(outPath, outPathSize, "%s/%s-crash-%s.txt", gReportDir, safeTitle, timestamp);
    return true;
}

bool MT_CrashReporter_WriteTestReport(char *outPath, size_t outPathSize)
{
    if (!MT_CrashReporter_BuildReportPath(outPath, outPathSize)) return false;

    int fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    MT_CrashReporter_WriteReportFd(fd, 0, nullptr);
    close(fd);
    return true;
}

// Write fields to fd using only write() — async-signal-safe.
static void WriteFd(int fd, const char *s)
{
    if (s && s[0]) write(fd, s, strlen(s));
}

static void WriteFdLine(int fd, const char *label, const char *value)
{
    WriteFd(fd, label);
    WriteFd(fd, ": ");
    WriteFd(fd, value && value[0] ? value : "(none)");
    WriteFd(fd, "\n");
}

// Write a decimal uint64 to fd — async-signal-safe.
static void WriteFdU64(int fd, unsigned long long v)
{
    char buf[24];
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    if (v == 0) { write(fd, "0", 1); return; }
    while (v > 0 && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    write(fd, buf + i, sizeof(buf) - 1 - i);
}

// Write a hex pointer to fd — async-signal-safe.
static void WriteFdPtr(int fd, const void *p)
{
    unsigned long long v = (unsigned long long)(uintptr_t)p;
    char buf[20] = "0x0000000000000000";
    for (int i = 17; v > 0; i--, v >>= 4)
        buf[i] = "0123456789abcdef"[v & 0xf];
    write(fd, buf, 18);
}

void MT_CrashReporter_WriteReportFd(int fd, int signum, void *faultAddr)
{
    const MT_CrashContext *ctx = &gContext;

    // Use appTitle in banner so engine stays app-agnostic.
    WriteFd(fd, "=== ");
    WriteFd(fd, ctx->appTitle[0] ? ctx->appTitle : "App");
    WriteFd(fd, " Crash Report ===\n\n");

    WriteFdLine(fd, "App",     ctx->appTitle);
    WriteFdLine(fd, "Version", ctx->appVersion);
    WriteFdLine(fd, "Build",   ctx->buildInfo);
    WriteFd(fd, "\n");

    // Timestamp as Unix epoch (async-signal-safe — no localtime).
    WriteFd(fd, "Timestamp: ");
    WriteFdU64(fd, (unsigned long long)time(nullptr));
    WriteFd(fd, " (Unix epoch seconds)\n");

    // Signal info.
    if (signum != 0)
    {
        const char *sigName =
            signum == 11 ? "SIGSEGV" :
            signum == 6  ? "SIGABRT" :
            signum == 4  ? "SIGILL"  :
            signum == 7  ? "SIGBUS"  :
            signum == 8  ? "SIGFPE"  : "UNKNOWN";
        WriteFd(fd, "Signal: ");
        WriteFdU64(fd, (unsigned long long)signum);
        WriteFd(fd, " (");
        WriteFd(fd, sigName);
        WriteFd(fd, ")\n");

        if (faultAddr)
        {
            WriteFd(fd, "Fault address: ");
            WriteFdPtr(fd, faultAddr);
            WriteFd(fd, "\n");
        }
    }
    else
    {
        WriteFd(fd, "Signal: SIMULATED (not a real crash)\n");
    }

    WriteFd(fd, "\n--- App State ---\n");
    WriteFdLine(fd, "Command line",   ctx->commandLine);
    WriteFdLine(fd, "Current folder", ctx->currentFolder);
    WriteFdLine(fd, "Current image",  ctx->currentImage);
    WriteFdLine(fd, "Active decode",  ctx->activeDecodePath);
    WriteFd(fd, "\n--- Paths ---\n");
    WriteFdLine(fd, "Settings dir",   ctx->settingsDir);

    WriteFd(fd, "\n--- Stack Trace ---\n");
    if (signum == 0)
        WriteFd(fd, "(simulated -- no stack trace)\n");
    // Real stack trace appended by the platform signal handler after this call.

    WriteFd(fd, "\n--- Privacy Notice ---\n");
    WriteFd(fd, "This report may contain local file paths from your system.\n");
    WriteFd(fd, "=== End of Report ===\n");
}

void MT_CrashReporter_SpawnHelper(const char *reportPath)
{
#if defined(__APPLE__) || defined(__linux__)
    // Use fork+execv — async-signal-safe.
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child: exec self with --show-crash-report.
        const char *args[] = { gSelfPath, "--show-crash-report", reportPath, nullptr };
        execv(gSelfPath, (char *const *)args);
        _exit(1);  // execv failed
    }
    // Parent: continue (will re-raise signal and terminate).
#elif defined(_WIN32)
    // Windows implementation in MT_CrashReporter_Windows.cpp
    extern void MT_CrashReporter_SpawnHelperWin32(const char *reportPath);
    MT_CrashReporter_SpawnHelperWin32(reportPath);
#endif
}
