#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
typedef int pid_t;
#else
#include <sys/types.h>
#endif

using namespace std;

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
};
