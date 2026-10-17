#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <functional>

#ifdef _WIN32
#include <process.h>
typedef int pid_t;
#else
#include <sys/types.h>
#endif

using namespace std;

// How CProcessSpawner::Run starts a child. Empty fields inherit from the parent.
struct CProcessSpawnOptions
{
	// Directory the child starts in; "" keeps the parent's current directory.
	string workingDirectory;

	// KEY=VALUE strings appended to the inherited environment.
	vector<string> envVars;

	// Resolve args[0] through the parent's PATH when it has no directory part
	// (needed for tools such as "cl65" or "java"). On Windows CreateProcess
	// always applies its own lookup, so this flag changes nothing there.
	bool searchPath = false;
};

// What CProcessSpawner::Run observed. stdOut/stdErr hold everything the child
// wrote, also when onOutput streamed it.
struct CProcessRunResult
{
	bool started = false;		// false: error says why the child never ran
	bool timedOut = false;		// stopped because timeoutMs elapsed
	bool cancelled = false;		// stopped because *cancel became true
	int exitCode = -1;			// 128+signal on POSIX when the child died of a signal
	string stdOut;
	string stdErr;
	string error;
};

// Cross-platform process spawn utility
class CProcessSpawner
{
public:
	// Spawn a child process with the given arguments. Returns the PID, or -1 on failure.
	// args[0] is the executable path.
	static pid_t Spawn(const vector<string> &args);

	// Spawn with extra environment variables (KEY=VALUE strings).
	// Inherits the current process environment and appends envVars.
	static pid_t SpawnWithEnv(const vector<string> &args, const vector<string> &envVars);

	// Run a child to completion with stdin on the null device and stdout/stderr
	// captured through pipes. Arguments go to the child verbatim -- there is no
	// shell, so nothing needs quoting. BLOCKS the calling thread: call it from a
	// worker thread, never the UI thread.
	//
	// onOutput, when set, receives every chunk as it arrives, on the calling
	// thread. timeoutMs <= 0 means no timeout; cancel, when set, is polled. On
	// timeout or cancel the child AND everything it started are stopped
	// (POSIX: SIGTERM to its process group, SIGKILL after a grace period;
	// Windows: the job object is terminated), so a compiler driver cannot
	// leave its back ends running.
	//
	// Returns true when the child started and exited on its own; its status is
	// in result.exitCode. Returns false when it did not start, timed out or was
	// cancelled.
	static bool Run(const vector<string> &args, const CProcessSpawnOptions &options,
					CProcessRunResult &result, int timeoutMs = 0,
					const atomic<bool> *cancel = nullptr,
					const function<void(bool isStdErr, const char *data, size_t length)> &onOutput = nullptr);

	// Check if a process with the given PID is still running.
	static bool IsRunning(pid_t pid);

	// Send SIGTERM to the process. Returns true if the signal was sent successfully.
	static bool Kill(pid_t pid);

	// Send SIGKILL to the process (immediate termination). Returns true if the signal was sent.
	static bool ForceKill(pid_t pid);

	// Wait for a process to exit (non-blocking). Returns true if the process has exited.
	// If exitCode is non-null, it will be set to the exit code.
	static bool TryWait(pid_t pid, int *exitCode = nullptr);

	// Get the path to the current executable.
	static string GetExecutablePath();

	// Build a Windows CreateProcess command-line string from an argv-style
	// vector, applying the quoting rules CreateProcess's argument parser
	// expects (Microsoft's "Parsing C++ Command-Line Arguments"): an argument
	// is quoted when it is empty or contains a space, tab, or '"'. Inside a
	// quoted argument, a run of n backslashes immediately preceding a '"'
	// becomes 2n+1 backslashes followed by an escaped quote; a run of n
	// backslashes at the very end of the argument (right before the closing
	// quote) is doubled to 2n. Pure string logic -- compiled on all
	// platforms so it is unit-testable outside of _WIN32.
	static string BuildWindowsCommandLine(const vector<string> &args);
};
