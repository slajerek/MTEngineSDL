#include "MT_CrashReporter.h"
#include "MT_API.h"

#if defined(__linux__)

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Signal handler — must be async-signal-safe.
// Uses MT_CrashReporter_GetPrebakedPath() (pre-baked at Init time, read-only
// after Install) rather than MT_CrashReporter_BuildReportPath() which calls
// snprintf internally and is NOT async-signal-safe.
static void CrashSignalHandler(int signum, siginfo_t *info, void * /*context*/)
{
    const char *reportPath = MT_CrashReporter_GetPrebakedPath();

    // Fallback: if the pre-baked path is empty/null, use a static literal.
    static const char fallbackPath[] = "/tmp/app-crash.txt";
    if (!reportPath || reportPath[0] == '\0')
        reportPath = fallbackPath;

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
    // Read the report text for stderr fallback.
    FILE *f = fopen(reportPath, "r");
    char reportText[4096]{};
    if (f)
    {
        fread(reportText, 1, sizeof(reportText) - 1, f);
        fclose(f);
    }
    else
    {
        snprintf(reportText, sizeof(reportText),
                 "Could not read crash report:\n%s", reportPath);
    }

    // Try zenity first (common on GNOME desktops).
    if (system("which zenity > /dev/null 2>&1") == 0)
    {
        char cmd[512]{};
        snprintf(cmd, sizeof(cmd),
                 "zenity --text-info --title='%s crashed' --filename='%s' "
                 "--width=600 --height=400 2>/dev/null",
                 MT_GetSettingsFolderName(), reportPath);
        system(cmd);
    }
    else
    {
        // Always print to stderr so the report is visible even without a GUI.
        fprintf(stderr, "\n[%s] Crash report written to: %s\n",
                MT_GetSettingsFolderName(), reportPath);
        fprintf(stderr, "%s\n", reportText);

        // Try kdialog (KDE) as secondary fallback.
        if (system("which kdialog > /dev/null 2>&1") == 0)
        {
            char cmd[512]{};
            snprintf(cmd, sizeof(cmd),
                     "kdialog --title '%s crashed' "
                     "--detailedsorry '%s crashed. See report.' "
                     "'%s' 2>/dev/null",
                     MT_GetSettingsFolderName(), MT_GetSettingsFolderName(),
                     reportPath);
            system(cmd);
        }
    }
}

#endif // __linux__
