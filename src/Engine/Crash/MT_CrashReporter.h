#pragma once
#include <stddef.h>

// Fixed-size crash context — all fields are pre-allocated char arrays.
// Safe to read from a signal handler (no heap, no locks after Install()).
struct MT_CrashContext {
    char appTitle[64];
    char appVersion[64];
    char buildInfo[128];        // "Platform Arch Date"
    char settingsDir[512];      // pre-baked: <settingsDir>/crash-reports/
    char currentFolder[1024];
    char currentImage[1024];
    char activeDecodePath[1024];
    char commandLine[2048];
};

// Access the global context (read-only from signal handlers).
const MT_CrashContext *MT_CrashReporter_GetContext();

// --- Called by MTEngineSDL startup (SYS_Startup.cpp) ---

// Check for --show-crash-report <path> at the very start of SYS_MTEngineStartup().
// If found, shows the native dialog and returns true (caller must exit immediately).
// Also detects --simulate-crash and --no-crash-reporter / --crash-reporter flags.
bool MT_CrashReporter_CheckEarlyExit();

// Called after SYS_InitFileSystem() — pre-bakes paths and build info into the context.
void MT_CrashReporter_Init();

// Called after MT_PreInit() — installs OS signal/exception handlers.
// No-op in headless/service mode or when disabled via MT_CrashReporter_SetEnabled(false).
void MT_CrashReporter_Install();

// --- Host app API ---

// Disable or re-enable before Install() is called (e.g. from MT_PreInit()).
void MT_CrashReporter_SetEnabled(bool enabled);

// Update crash context fields at any time from any thread.
// Uses strncpy only — no heap, no blocking.
void MT_CrashContext_SetCurrentFolder(const char *path);
void MT_CrashContext_SetCurrentImage(const char *path);
void MT_CrashContext_SetActiveDecodePath(const char *path);

// --- Internal / platform ---

// Build a timestamped report file path into outPath[outPathSize].
// Returns false if the report directory is not initialised.
bool MT_CrashReporter_BuildReportPath(char *outPath, size_t outPathSize);

// Write crash report to an already-open file descriptor.
// Async-signal-safe on Unix. signum = 0 for test/simulated reports.
void MT_CrashReporter_WriteReportFd(int fd, int signum, void *faultAddr);

// Write a test/simulated report file. Returns false on failure.
// outPath receives the path of the written file.
bool MT_CrashReporter_WriteTestReport(char *outPath, size_t outPathSize);

// Platform-specific — implemented in MT_CrashReporter_{MacOS,Linux,Windows}:
void MT_CrashReporter_InstallPlatform();
void MT_CrashReporter_ShowDialog(const char *reportPath);

// Spawn self with --show-crash-report <path>. Called after writing the report.
// Safe to call from signal handler on Unix (fork+execv only).
void MT_CrashReporter_SpawnHelper(const char *reportPath);

// Returns the pre-baked crash report path set during Init().
// Async-signal-safe to read (set once at Init, read-only after Install).
const char *MT_CrashReporter_GetPrebakedPath();
