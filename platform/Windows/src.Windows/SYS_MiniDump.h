#ifndef _SYS_MINIDUMP_H_
#define _SYS_MINIDUMP_H_

#include <windows.h>
#include <tchar.h>
#include <dbghelp.h>
#include <stdio.h>
#include <crtdbg.h>

// Install the crash handler — call this early in main() before any other initialization.
// Registers SetUnhandledExceptionFilter so that on crash we get:
//   1. A human-readable callstack printed to stderr and to a crash log file
//   2. A minidump .dmp file for post-mortem debugging
void SYS_InstallCrashHandler();

// Low-level helpers (can also be called manually)
void SYS_CreateMiniDump(EXCEPTION_POINTERS* pep);
void SYS_CreateMaxiDump(EXCEPTION_POINTERS* pep);

#endif
