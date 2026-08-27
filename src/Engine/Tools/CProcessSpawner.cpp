#include "CProcessSpawner.h"
#include "DBG_Log.h"

#ifdef _WIN32
#include <windows.h>
#include <unordered_map>
#include <mutex>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
#include <errno.h>
#include <cstring>
extern char **environ;
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
// Windows has no zombie/reap concept: once a process exits and its last HANDLE
// is closed, the OS may fully discard the process object and recycle the PID
// for something unrelated. Re-deriving a HANDLE later via OpenProcess(pid) is
// therefore unreliable for short-lived processes — by the time TryWait/IsRunning
// runs, the PID can already point at a different process, or at nothing at all
// (OpenProcess fails, and callers silently lose the real exit code).
// Cache the HANDLE from CreateProcess and reuse it for the lifetime tracking
// calls below; only OpenProcess() as a fallback for PIDs we didn't spawn.
static std::mutex sProcessHandlesMutex;
static std::unordered_map<pid_t, HANDLE> sProcessHandles;

static void CacheProcessHandle(pid_t pid, HANDLE h)
{
	std::lock_guard<std::mutex> lock(sProcessHandlesMutex);
	sProcessHandles[pid] = h;
}

// Returns a HANDLE usable by the caller (caller must NOT close a cached handle;
// pass ownsHandle=true only for the OpenProcess() fallback, which the caller owns).
static HANDLE AcquireProcessHandle(pid_t pid, DWORD desiredAccess, bool *ownsHandle)
{
	{
		std::lock_guard<std::mutex> lock(sProcessHandlesMutex);
		auto it = sProcessHandles.find(pid);
		if (it != sProcessHandles.end())
		{
			*ownsHandle = false;
			return it->second;
		}
	}
	*ownsHandle = true;
	return OpenProcess(desiredAccess, FALSE, pid);
}

static void ReleaseProcessHandle(pid_t pid)
{
	std::lock_guard<std::mutex> lock(sProcessHandlesMutex);
	auto it = sProcessHandles.find(pid);
	if (it != sProcessHandles.end())
	{
		CloseHandle(it->second);
		sProcessHandles.erase(it);
	}
}
#endif

pid_t CProcessSpawner::Spawn(const vector<string> &args)
{
	if (args.empty())
	{
		LOGError("CProcessSpawner::Spawn: empty args");
		return -1;
	}

#ifdef _WIN32
	// Windows: CreateProcess
	string cmdLine;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0) cmdLine += " ";
		// Quote arguments that contain spaces
		if (args[i].find(' ') != string::npos)
			cmdLine += "\"" + args[i] + "\"";
		else
			cmdLine += args[i];
	}

	STARTUPINFOA si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};

	if (!CreateProcessA(NULL, (LPSTR)cmdLine.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
	{
		LOGError("CProcessSpawner::Spawn: CreateProcess failed: %lu", GetLastError());
		return -1;
	}

	CloseHandle(pi.hThread);
	CacheProcessHandle((pid_t)pi.dwProcessId, pi.hProcess);
	return (pid_t)pi.dwProcessId;
#else
	// Unix: posix_spawn (safer than fork in multi-threaded programs)
	vector<char*> argv;
	for (const auto &arg : args)
		argv.push_back(const_cast<char*>(arg.c_str()));
	argv.push_back(nullptr);

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	// Reset signal handling to defaults and unblock all signals in child
	sigset_t sigdefault;
	sigfillset(&sigdefault);
	posix_spawnattr_setsigdefault(&attr, &sigdefault);
	sigset_t sigmask;
	sigemptyset(&sigmask);
	posix_spawnattr_setsigmask(&attr, &sigmask);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

	pid_t pid;
	int err = posix_spawn(&pid, argv[0], NULL, &attr, argv.data(), environ);
	posix_spawnattr_destroy(&attr);

	if (err != 0)
	{
		LOGError("CProcessSpawner::Spawn: posix_spawn failed: %s", strerror(err));
		return -1;
	}

	LOGM("CProcessSpawner::Spawn: spawned pid=%d cmd=%s", pid, args[0].c_str());
	return pid;
#endif
}

pid_t CProcessSpawner::SpawnWithEnv(const vector<string> &args, const vector<string> &envVars)
{
	if (args.empty())
	{
		LOGError("CProcessSpawner::SpawnWithEnv: empty args");
		return -1;
	}

	if (envVars.empty())
		return Spawn(args);

#ifdef _WIN32
	// Windows: build environment block (null-terminated KEY=VALUE pairs, double-null at end)
	// Start with current environment
	LPCH currentEnv = GetEnvironmentStringsA();
	vector<string> allEnv;
	if (currentEnv)
	{
		const char *p = currentEnv;
		while (*p)
		{
			allEnv.push_back(p);
			p += strlen(p) + 1;
		}
		FreeEnvironmentStringsA(currentEnv);
	}
	for (const auto &ev : envVars)
		allEnv.push_back(ev);

	// Build environment block
	string envBlock;
	for (const auto &e : allEnv)
	{
		envBlock.append(e);
		envBlock.push_back('\0');
	}
	envBlock.push_back('\0');

	string cmdLine;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0) cmdLine += " ";
		if (args[i].find(' ') != string::npos)
			cmdLine += "\"" + args[i] + "\"";
		else
			cmdLine += args[i];
	}

	STARTUPINFOA si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};

	if (!CreateProcessA(NULL, (LPSTR)cmdLine.c_str(), NULL, NULL, FALSE, 0,
						(LPVOID)envBlock.data(), NULL, &si, &pi))
	{
		LOGError("CProcessSpawner::SpawnWithEnv: CreateProcess failed: %lu", GetLastError());
		return -1;
	}

	CloseHandle(pi.hThread);
	CacheProcessHandle((pid_t)pi.dwProcessId, pi.hProcess);
	return (pid_t)pi.dwProcessId;
#else
	// Unix: build custom environ array
	vector<string> allEnvStrs;
	if (environ)
	{
		for (char **ep = environ; *ep; ep++)
			allEnvStrs.push_back(*ep);
	}
	for (const auto &ev : envVars)
		allEnvStrs.push_back(ev);

	vector<char*> envp;
	for (auto &s : allEnvStrs)
		envp.push_back(const_cast<char*>(s.c_str()));
	envp.push_back(nullptr);

	vector<char*> argv;
	for (const auto &arg : args)
		argv.push_back(const_cast<char*>(arg.c_str()));
	argv.push_back(nullptr);

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	sigset_t sigdefault;
	sigfillset(&sigdefault);
	posix_spawnattr_setsigdefault(&attr, &sigdefault);
	sigset_t sigmask;
	sigemptyset(&sigmask);
	posix_spawnattr_setsigmask(&attr, &sigmask);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

	pid_t pid;
	int err = posix_spawn(&pid, argv[0], NULL, &attr, argv.data(), envp.data());
	posix_spawnattr_destroy(&attr);

	if (err != 0)
	{
		LOGError("CProcessSpawner::SpawnWithEnv: posix_spawn failed: %s", strerror(err));
		return -1;
	}

	LOGM("CProcessSpawner::SpawnWithEnv: spawned pid=%d cmd=%s", pid, args[0].c_str());
	return pid;
#endif
}

bool CProcessSpawner::IsRunning(pid_t pid)
{
	if (pid <= 0)
		return false;

#ifdef _WIN32
	bool ownsHandle = false;
	HANDLE hProcess = AcquireProcessHandle(pid, PROCESS_QUERY_INFORMATION, &ownsHandle);
	if (!hProcess)
		return false;

	DWORD exitCode;
	bool running = (GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE);
	if (ownsHandle)
		CloseHandle(hProcess);
	return running;
#else
	// Send signal 0 to check if the process exists
	int result = kill(pid, 0);
	if (result == 0)
		return true;
	if (errno == ESRCH)
		return false;
	// EPERM means process exists but we don't have permission
	return (errno == EPERM);
#endif
}

bool CProcessSpawner::Kill(pid_t pid)
{
	if (pid <= 0)
		return false;

#ifdef _WIN32
	bool ownsHandle = false;
	HANDLE hProcess = AcquireProcessHandle(pid, PROCESS_TERMINATE, &ownsHandle);
	if (!hProcess)
		return false;

	bool result = TerminateProcess(hProcess, 1);
	if (ownsHandle)
		CloseHandle(hProcess);
	return result;
#else
	int result = ::kill(pid, SIGTERM);
	if (result == 0)
	{
		LOGM("CProcessSpawner::Kill: sent SIGTERM to pid=%d", pid);
		return true;
	}
	LOGError("CProcessSpawner::Kill: kill(%d, SIGTERM) failed: %s", pid, strerror(errno));
	return false;
#endif
}

bool CProcessSpawner::ForceKill(pid_t pid)
{
	if (pid <= 0)
		return false;

#ifdef _WIN32
	bool ownsHandle = false;
	HANDLE hProcess = AcquireProcessHandle(pid, PROCESS_TERMINATE, &ownsHandle);
	if (!hProcess)
		return false;

	bool result = TerminateProcess(hProcess, 1);
	if (ownsHandle)
		CloseHandle(hProcess);
	return result;
#else
	int result = ::kill(pid, SIGKILL);
	if (result == 0)
	{
		LOGM("CProcessSpawner::ForceKill: sent SIGKILL to pid=%d", pid);
		return true;
	}
	LOGError("CProcessSpawner::ForceKill: kill(%d, SIGKILL) failed: %s", pid, strerror(errno));
	return false;
#endif
}

bool CProcessSpawner::TryWait(pid_t pid, int *exitCode)
{
	if (pid <= 0)
		return true;

#ifdef _WIN32
	bool ownsHandle = false;
	HANDLE hProcess = AcquireProcessHandle(pid, PROCESS_QUERY_INFORMATION | SYNCHRONIZE, &ownsHandle);
	if (!hProcess)
		return true; // process doesn't exist

	DWORD result = WaitForSingleObject(hProcess, 0);
	if (result == WAIT_OBJECT_0)
	{
		if (exitCode)
		{
			DWORD code;
			GetExitCodeProcess(hProcess, &code);
			*exitCode = (int)code;
		}
		if (ownsHandle)
			CloseHandle(hProcess);
		else
			ReleaseProcessHandle(pid);
		return true;
	}
	if (ownsHandle)
		CloseHandle(hProcess);
	return false;
#else
	int status;
	pid_t result = waitpid(pid, &status, WNOHANG);
	if (result == pid)
	{
		if (exitCode)
		{
			if (WIFEXITED(status))
				*exitCode = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				*exitCode = 128 + WTERMSIG(status);
			else
				*exitCode = -1;
		}
		return true;
	}
	if (result == -1)
	{
		LOGError("CProcessSpawner::TryWait: waitpid(%d) returned -1, errno=%d (%s)", pid, errno, strerror(errno));
		if (errno == ECHILD)
		{
			// Child already reaped (e.g. by SIGCHLD handler)
			if (exitCode) *exitCode = -1;
			return true;
		}
	}
	return false;
#endif
}

string CProcessSpawner::GetExecutablePath()
{
#ifdef _WIN32
	char buf[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
	if (len > 0 && len < MAX_PATH)
		return string(buf, len);
	return "";
#elif defined(__APPLE__)
	char buf[1024];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0)
		return string(buf);
	// Buffer too small, allocate
	vector<char> bigBuf(size);
	if (_NSGetExecutablePath(bigBuf.data(), &size) == 0)
		return string(bigBuf.data());
	return "";
#elif defined(__linux__)
	char buf[1024];
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len > 0)
	{
		buf[len] = '\0';
		return string(buf);
	}
	return "";
#else
	return "";
#endif
}
