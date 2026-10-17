#include "CProcessSpawner.h"
#include "DBG_Log.h"
#include <chrono>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <unordered_map>
#include <mutex>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
extern char **environ;

// posix_spawn_file_actions_addchdir_np: glibc 2.29+, macOS 10.15+ (the engine's
// deployment target). Without it Run refuses a workingDirectory rather than
// fork()ing a multi-threaded process.
// __GLIBC_PREREQ is tested in its own #if: clang rejects an undefined
// function-like macro anywhere in an #if expression, short-circuit or not.
#if defined(__APPLE__)
#define MT_SPAWN_HAVE_ADDCHDIR 1
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 29)
#define MT_SPAWN_HAVE_ADDCHDIR 1
#endif
#endif
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

// Pure string function -- compiled on all platforms (not guarded by
// #ifdef _WIN32) so it is unit-testable outside of Windows. See the header
// for the quoting rules.
string CProcessSpawner::BuildWindowsCommandLine(const vector<string> &args)
{
	string cmdLine;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0)
			cmdLine += " ";

		const string &arg = args[i];
		bool needsQuotes = arg.empty() || arg.find_first_of(" \t\"") != string::npos;

		if (!needsQuotes)
		{
			cmdLine += arg;
			continue;
		}

		cmdLine += "\"";
		size_t numBackslashes = 0;
		for (char c : arg)
		{
			if (c == '\\')
			{
				numBackslashes++;
			}
			else if (c == '"')
			{
				cmdLine.append(numBackslashes * 2 + 1, '\\');
				cmdLine += "\"";
				numBackslashes = 0;
			}
			else
			{
				cmdLine.append(numBackslashes, '\\');
				cmdLine += c;
				numBackslashes = 0;
			}
		}
		// Trailing backslashes (right before the closing quote) double, with
		// no escaped-quote suffix since there's no literal quote to protect.
		cmdLine.append(numBackslashes * 2, '\\');
		cmdLine += "\"";
	}
	return cmdLine;
}

pid_t CProcessSpawner::Spawn(const vector<string> &args)
{
	if (args.empty())
	{
		LOGError("CProcessSpawner::Spawn: empty args");
		return -1;
	}

#ifdef _WIN32
	// Windows: CreateProcess
	string cmdLine = BuildWindowsCommandLine(args);

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

	string cmdLine = BuildWindowsCommandLine(args);

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

// How long Run waits after SIGTERM before it sends SIGKILL to the process group.
static const int kRunTerminateGraceMs = 2000;

static int64_t RunNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32

static wstring RunUtf8ToWide(const string &s)
{
	if (s.empty())
		return wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
	wstring w(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], len);
	return w;
}

bool CProcessSpawner::Run(const vector<string> &args, const CProcessSpawnOptions &options,
						  CProcessRunResult &result, int timeoutMs, const atomic<bool> *cancel,
						  const function<void(bool isStdErr, const char *data, size_t length)> &onOutput)
{
	result = CProcessRunResult();
	if (args.empty())
	{
		result.error = "empty args";
		return false;
	}

	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE outRead = NULL, outWrite = NULL, errRead = NULL, errWrite = NULL;
	if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0))
	{
		result.error = "CreatePipe failed: " + to_string(GetLastError());
		if (outRead) { CloseHandle(outRead); CloseHandle(outWrite); }
		return false;
	}
	SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
	HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
							   OPEN_EXISTING, 0, NULL);

	// Inherit ONLY these three handles: without the list, a concurrent spawn on
	// another thread would hand our pipe ends to its child and the pipes would
	// never report EOF.
	HANDLE inherited[3] = { nulIn, outWrite, errWrite };
	SIZE_T attrSize = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
	vector<char> attrBuf(attrSize);
	LPPROC_THREAD_ATTRIBUTE_LIST attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
	InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
	UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
							  inherited, sizeof(inherited), NULL, NULL);

	STARTUPINFOEXW si = {};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = nulIn;
	si.StartupInfo.hStdOutput = outWrite;
	si.StartupInfo.hStdError = errWrite;
	si.lpAttributeList = attrList;

	wstring cmdLine = RunUtf8ToWide(BuildWindowsCommandLine(args));
	vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
	cmdLineBuf.push_back(L'\0');

	DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
	wstring envBlock;
	if (!options.envVars.empty())
	{
		LPWCH currentEnv = GetEnvironmentStringsW();
		if (currentEnv)
		{
			const wchar_t *p = currentEnv;
			while (*p)
			{
				size_t n = wcslen(p);
				envBlock.append(p, n);
				envBlock.push_back(L'\0');
				p += n + 1;
			}
			FreeEnvironmentStringsW(currentEnv);
		}
		for (const auto &ev : options.envVars)
		{
			envBlock.append(RunUtf8ToWide(ev));
			envBlock.push_back(L'\0');
		}
		envBlock.push_back(L'\0');
		flags |= CREATE_UNICODE_ENVIRONMENT;
	}
	wstring cwd = RunUtf8ToWide(options.workingDirectory);

	// The job holds the child and everything it starts, so a timeout or cancel
	// stops the whole tree, and closing the job reaps stragglers.
	HANDLE job = CreateJobObjectW(NULL, NULL);
	if (job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	}

	PROCESS_INFORMATION pi = {};
	BOOL created = CreateProcessW(NULL, cmdLineBuf.data(), NULL, NULL, TRUE, flags,
								  envBlock.empty() ? NULL : (LPVOID)envBlock.data(),
								  cwd.empty() ? NULL : cwd.c_str(), &si.StartupInfo, &pi);
	DWORD createError = created ? 0 : GetLastError();
	DeleteProcThreadAttributeList(attrList);
	CloseHandle(outWrite);
	CloseHandle(errWrite);
	if (nulIn != INVALID_HANDLE_VALUE)
		CloseHandle(nulIn);

	if (!created)
	{
		result.error = "CreateProcess failed: " + to_string(createError);
		CloseHandle(outRead);
		CloseHandle(errRead);
		if (job)
			CloseHandle(job);
		return false;
	}
	if (job)
		AssignProcessToJobObject(job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	result.started = true;

	HANDLE pipes[2] = { outRead, errRead };
	string *sinks[2] = { &result.stdOut, &result.stdErr };
	bool exited = false;
	bool stopRequested = false;
	int64_t deadline = timeoutMs > 0 ? RunNowMs() + timeoutMs : 0;
	int64_t killDeadline = 0;
	char buf[4096];

	while (pipes[0] || pipes[1] || !exited)
	{
		bool readAny = false;
		for (int i = 0; i < 2; i++)
		{
			if (!pipes[i])
				continue;
			DWORD avail = 0;
			if (!PeekNamedPipe(pipes[i], NULL, 0, NULL, &avail, NULL))
			{
				CloseHandle(pipes[i]);	// ERROR_BROKEN_PIPE: every writer is gone
				pipes[i] = NULL;
				continue;
			}
			if (avail == 0)
				continue;
			DWORD got = 0;
			DWORD want = avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf);
			if (ReadFile(pipes[i], buf, want, &got, NULL) && got > 0)
			{
				sinks[i]->append(buf, got);
				if (onOutput)
					onOutput(i == 1, buf, got);
				readAny = true;
			}
		}

		if (!exited && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
			exited = true;

		if (!stopRequested)
		{
			if (cancel && cancel->load())
				result.cancelled = stopRequested = true;
			else if (deadline && RunNowMs() >= deadline)
				result.timedOut = stopRequested = true;
			if (stopRequested)
			{
				if (job)
					TerminateJobObject(job, 1);
				else
					TerminateProcess(pi.hProcess, 1);
				killDeadline = RunNowMs() + kRunTerminateGraceMs;
			}
		}
		else if (RunNowMs() >= killDeadline)
		{
			// Something outside the job still holds a pipe: stop waiting for it.
			for (int i = 0; i < 2; i++)
				if (pipes[i])
				{
					CloseHandle(pipes[i]);
					pipes[i] = NULL;
				}
		}

		if (!readAny)
			Sleep(10);
	}

	DWORD code = (DWORD)-1;
	GetExitCodeProcess(pi.hProcess, &code);
	result.exitCode = (int)code;
	CloseHandle(pi.hProcess);
	if (job)
		CloseHandle(job);
	return !stopRequested;
}

#else

// The executable posix_spawn should run: args[0] itself when it names a path
// (or PATH lookup is off), else the first executable regular file on PATH.
static string RunResolveExecutable(const string &name, bool searchPath)
{
	if (!searchPath || name.find('/') != string::npos)
		return name;
	const char *pathEnv = getenv("PATH");
	if (!pathEnv)
		return "";
	string path(pathEnv);
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find(':', start);
		if (end == string::npos)
			end = path.size();
		string dir = path.substr(start, end - start);
		string candidate = (dir.empty() ? string(".") : dir) + "/" + name;
		struct stat st;
		if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(candidate.c_str(), X_OK) == 0)
			return candidate;
		start = end + 1;
	}
	return "";
}

static bool RunMakePipe(int fds[2])
{
	if (pipe(fds) != 0)
		return false;
	// Both ends close-on-exec so no OTHER child inherits them; posix_spawn's
	// dup2 onto 1/2 gives our child its own non-CLOEXEC copies.
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	return true;
}

bool CProcessSpawner::Run(const vector<string> &args, const CProcessSpawnOptions &options,
						  CProcessRunResult &result, int timeoutMs, const atomic<bool> *cancel,
						  const function<void(bool isStdErr, const char *data, size_t length)> &onOutput)
{
	result = CProcessRunResult();
	if (args.empty())
	{
		result.error = "empty args";
		return false;
	}

#ifndef MT_SPAWN_HAVE_ADDCHDIR
	if (!options.workingDirectory.empty())
	{
		result.error = "workingDirectory is not supported on this platform";
		return false;
	}
#endif

	string exe = RunResolveExecutable(args[0], options.searchPath);
	if (exe.empty())
	{
		result.error = "not found on PATH: " + args[0];
		return false;
	}

	int outPipe[2], errPipe[2];
	if (!RunMakePipe(outPipe))
	{
		result.error = string("pipe failed: ") + strerror(errno);
		return false;
	}
	if (!RunMakePipe(errPipe))
	{
		result.error = string("pipe failed: ") + strerror(errno);
		close(outPipe[0]);
		close(outPipe[1]);
		return false;
	}

	vector<char*> argv;
	for (const auto &arg : args)
		argv.push_back(const_cast<char*>(arg.c_str()));
	argv.push_back(nullptr);

	vector<string> envStrs;
	vector<char*> envp;
	if (!options.envVars.empty())
	{
		if (environ)
			for (char **ep = environ; *ep; ep++)
				envStrs.push_back(*ep);
		for (const auto &ev : options.envVars)
			envStrs.push_back(ev);
		for (auto &e : envStrs)
			envp.push_back(const_cast<char*>(e.c_str()));
		envp.push_back(nullptr);
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
	posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
#ifdef MT_SPAWN_HAVE_ADDCHDIR
	if (!options.workingDirectory.empty())
		posix_spawn_file_actions_addchdir_np(&actions, options.workingDirectory.c_str());
#endif

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	sigset_t sigdefault;
	sigfillset(&sigdefault);
	posix_spawnattr_setsigdefault(&attr, &sigdefault);
	sigset_t sigmask;
	sigemptyset(&sigmask);
	posix_spawnattr_setsigmask(&attr, &sigmask);
	// Own process group (pgid == pid), so a stop reaches the grandchildren too.
	posix_spawnattr_setpgroup(&attr, 0);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);

	pid_t pid;
	int err = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(),
						  envp.empty() ? environ : envp.data());
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	close(outPipe[1]);
	close(errPipe[1]);

	if (err != 0)
	{
		result.error = string("posix_spawn failed: ") + strerror(err);
		close(outPipe[0]);
		close(errPipe[0]);
		return false;
	}
	result.started = true;
	LOGM("CProcessSpawner::Run: spawned pid=%d cmd=%s", pid, exe.c_str());

	int fds[2] = { outPipe[0], errPipe[0] };
	string *sinks[2] = { &result.stdOut, &result.stdErr };
	bool exited = false;
	int status = 0;
	bool haveStatus = false;
	bool stopRequested = false;
	bool killed = false;
	int64_t deadline = timeoutMs > 0 ? RunNowMs() + timeoutMs : 0;
	int64_t killDeadline = 0;
	char buf[4096];

	// Runs until BOTH the child has exited and every writer of the pipes is
	// gone: a grandchild that keeps stdout open keeps the run going (or is
	// stopped with the group on timeout/cancel).
	while (fds[0] >= 0 || fds[1] >= 0 || !exited)
	{
		if (!exited)
		{
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid)
				exited = haveStatus = true;
			else if (r == -1 && errno == ECHILD)
				exited = true;	// reaped elsewhere (a SIGCHLD handler); status unknown
		}

		if (!stopRequested)
		{
			if (cancel && cancel->load())
				result.cancelled = stopRequested = true;
			else if (deadline && RunNowMs() >= deadline)
				result.timedOut = stopRequested = true;
			if (stopRequested)
			{
				::kill(-pid, SIGTERM);
				killDeadline = RunNowMs() + kRunTerminateGraceMs;
			}
		}
		else if (!killed && RunNowMs() >= killDeadline)
		{
			::kill(-pid, SIGKILL);
			killed = true;
		}
		else if (killed && RunNowMs() >= killDeadline + kRunTerminateGraceMs)
		{
			// A writer left the process group (setsid) and still holds a pipe:
			// stop waiting for it. The child itself got SIGKILL and is reaped above.
			for (int i = 0; i < 2; i++)
				if (fds[i] >= 0)
				{
					close(fds[i]);
					fds[i] = -1;
				}
		}

		if (fds[0] < 0 && fds[1] < 0)
		{
			usleep(10 * 1000);
			continue;
		}

		struct pollfd pfds[2];
		int idx[2];
		int n = 0;
		for (int i = 0; i < 2; i++)
		{
			if (fds[i] < 0)
				continue;
			pfds[n].fd = fds[i];
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			idx[n++] = i;
		}
		if (poll(pfds, n, 50) <= 0)
			continue;

		for (int k = 0; k < n; k++)
		{
			if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			int i = idx[k];
			ssize_t got = read(fds[i], buf, sizeof(buf));
			if (got > 0)
			{
				sinks[i]->append(buf, (size_t)got);
				if (onOutput)
					onOutput(i == 1, buf, (size_t)got);
			}
			else if (got == 0 || (errno != EAGAIN && errno != EINTR))
			{
				close(fds[i]);
				fds[i] = -1;
			}
		}
	}

	if (haveStatus)
	{
		if (WIFEXITED(status))
			result.exitCode = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			result.exitCode = 128 + WTERMSIG(status);
	}
	return !stopRequested;
}

#endif

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
