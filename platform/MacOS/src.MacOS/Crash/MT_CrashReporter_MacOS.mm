// MT_CrashReporter_MacOS.mm
// macOS platform implementation: POSIX signal handlers + NSAlert dialog.
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#include "MT_CrashReporter.h"
#include "DBG_Log.h"
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <sys/types.h>

// Signal handler — must use only async-signal-safe functions.
static void CrashSignalHandler(int signum, siginfo_t *info, void * /*context*/)
{
    // Use the pre-baked path — no snprintf in signal handler.
    const char *reportPath = MT_CrashReporter_GetPrebakedPath();
    if (!reportPath || reportPath[0] == '\0')
        reportPath = "/tmp/app-crash.txt";

    int fd = open(reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        void *faultAddr = info ? info->si_addr : nullptr;
        MT_CrashReporter_WriteReportFd(fd, signum, faultAddr);

        void *frames[64];
        int count = backtrace(frames, 64);
        write(fd, "\n--- Stack Trace (backtrace) ---\n", 33);
        backtrace_symbols_fd(frames, count, fd);
        close(fd);
    }

    MT_CrashReporter_SpawnHelper(reportPath);

    signal(signum, SIG_DFL);
    raise(signum);
}

void MT_CrashReporter_InstallPlatform()
{
    struct sigaction sa{};
    sa.sa_sigaction = CrashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
}

void MT_CrashReporter_ShowDialog(const char *reportPath)
{
    // Initialize NSApp so we can show alerts without a full app lifecycle.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    // Read report file contents.
    NSString *path = [NSString stringWithUTF8String:reportPath];
    NSString *reportText = [NSString stringWithContentsOfFile:path
                                                      encoding:NSUTF8StringEncoding
                                                         error:nil];
    if (!reportText)
        reportText = @"(Could not read crash report file.)";

    // Truncate for display (NSAlert scrollview not available without AppKit gymnastics).
    NSString *displayText = reportText;
    if ([displayText length] > 2000)
        displayText = [[displayText substringToIndex:2000] stringByAppendingString:@"\n…(truncated)"];

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"PhotoCruise crashed";
    alert.informativeText = displayText;
    alert.alertStyle = NSAlertStyleCritical;

    [alert addButtonWithTitle:@"Copy Report"];
    [alert addButtonWithTitle:@"Open Report File"];
    [alert addButtonWithTitle:@"Close"];

    [NSApp activateIgnoringOtherApps:YES];
    NSModalResponse response = [alert runModal];

    if (response == NSAlertFirstButtonReturn)
    {
        // Copy Report
        [[NSPasteboard generalPasteboard] clearContents];
        [[NSPasteboard generalPasteboard] setString:reportText forType:NSPasteboardTypeString];
    }
    else if (response == NSAlertSecondButtonReturn)
    {
        // Open Report File
        [[NSWorkspace sharedWorkspace] selectFile:path
                         inFileViewerRootedAtPath:@""];
    }
    // Close: nothing to do.
}
