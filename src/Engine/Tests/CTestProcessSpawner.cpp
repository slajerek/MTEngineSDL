#include "CTestProcessSpawner.h"
#include "CProcessSpawner.h"
#include "DBG_Log.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>

#ifndef _WIN32
#include <unistd.h>
#include <climits>
#endif

#define PS_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _buf[512]; \
			snprintf(_buf, sizeof(_buf), "FAIL: %s", msg); \
			LOGError("CTestProcessSpawner: %s", _buf); \
			TestCompleted(false, _buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

static long long ElapsedMs(std::chrono::steady_clock::time_point since)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - since).count();
}

void CTestProcessSpawner::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	// --- Windows quoting (pure string logic, every platform) ----------------
	PS_ASSERT(CProcessSpawner::BuildWindowsCommandLine({ "a", "b c", "" }) == "a \"b c\" \"\"",
			  "BuildWindowsCommandLine quotes spaces and empty arguments");
	PS_ASSERT(CProcessSpawner::BuildWindowsCommandLine({ "x\"y", "d:\\dir\\" }) == "\"x\\\"y\" d:\\dir\\",
			  "BuildWindowsCommandLine escapes quotes and leaves unquoted backslashes alone");

	CProcessSpawnOptions opts;
	CProcessRunResult r;

#ifdef _WIN32
	const char *shell = "cmd.exe";

	opts.searchPath = true;
	bool ok = CProcessSpawner::Run({ shell, "/c", "echo out& echo err 1>&2& exit 3" }, opts, r);
	PS_ASSERT(ok && r.started && r.exitCode == 3, "exit code is reported");
	PS_ASSERT(r.stdOut.find("out") != string::npos && r.stdOut.find("err") == string::npos
			  && r.stdErr.find("err") != string::npos, "stdout and stderr are captured separately");

	opts.envVars = { "MT_PROC_TEST=hello" };
	CProcessSpawner::Run({ shell, "/c", "echo %MT_PROC_TEST%" }, opts, r);
	PS_ASSERT(r.stdOut.find("hello") != string::npos, "envVars reach the child");
	opts.envVars.clear();

	auto t0 = std::chrono::steady_clock::now();
	ok = CProcessSpawner::Run({ shell, "/c", "ping -n 30 127.0.0.1 >NUL" }, opts, r, 300);
	PS_ASSERT(!ok && r.timedOut && ElapsedMs(t0) < 10000, "timeout stops the child");
#else
	const char *shell = "/bin/sh";

	bool ok = CProcessSpawner::Run({ shell, "-c", "echo out; echo err 1>&2; exit 3" }, opts, r);
	PS_ASSERT(ok && r.started && r.exitCode == 3, "exit code is reported");
	PS_ASSERT(r.stdOut == "out\n" && r.stdErr == "err\n", "stdout and stderr are captured separately");

	CProcessSpawner::Run({ shell, "-c", "printf '%s|' \"$@\"", "sh", "a b", "c\"d", "$HOME", "" }, opts, r);
	PS_ASSERT(r.exitCode == 0 && r.stdOut == "a b|c\"d|$HOME||", "arguments reach the child verbatim, no shell expansion");

	CProcessSpawner::Run({ shell, "-c", "kill -9 $$" }, opts, r);
	PS_ASSERT(r.exitCode == 128 + 9, "death by signal is reported as 128+signal");

	char tmpReal[PATH_MAX];
	PS_ASSERT(realpath("/tmp", tmpReal) != NULL, "realpath(/tmp)");
	opts.workingDirectory = "/tmp";
	ok = CProcessSpawner::Run({ shell, "-c", "pwd -P" }, opts, r);
	PS_ASSERT(ok && r.stdOut == string(tmpReal) + "\n", "workingDirectory is the child's cwd");
	opts.workingDirectory.clear();

	opts.envVars = { "MT_PROC_TEST=hello" };
	CProcessSpawner::Run({ shell, "-c", "printf %s \"$MT_PROC_TEST\"" }, opts, r);
	PS_ASSERT(r.stdOut == "hello", "envVars reach the child");
	opts.envVars.clear();

	opts.searchPath = true;
	ok = CProcessSpawner::Run({ "sh", "-c", "exit 0" }, opts, r);
	PS_ASSERT(ok && r.exitCode == 0, "searchPath resolves a bare name through PATH");
	ok = CProcessSpawner::Run({ "mt-no-such-tool-xyzzy" }, opts, r);
	PS_ASSERT(!ok && !r.started && !r.error.empty(), "a missing tool fails to start with an error");
	opts.searchPath = false;
	ok = CProcessSpawner::Run({ "sh", "-c", "exit 0" }, opts, r);
	PS_ASSERT(!ok && !r.started, "without searchPath a bare name is not looked up");

	// A background grandchild keeps stdout open: only a stop that reaches the
	// whole process group can finish this quickly.
	auto t0 = std::chrono::steady_clock::now();
	ok = CProcessSpawner::Run({ shell, "-c", "sleep 30 & sleep 30" }, opts, r, 300);
	PS_ASSERT(!ok && r.timedOut && ElapsedMs(t0) < 5000, "timeout stops the child and its grandchildren");

	atomic<bool> cancelFlag(false);
	std::thread canceller([&cancelFlag]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		cancelFlag = true;
	});
	t0 = std::chrono::steady_clock::now();
	ok = CProcessSpawner::Run({ shell, "-c", "sleep 30" }, opts, r, 0, &cancelFlag);
	canceller.join();
	PS_ASSERT(!ok && r.cancelled && !r.timedOut && ElapsedMs(t0) < 5000, "cancel stops the child");

	// 1 MiB on stdout and 1 MiB on stderr at once: a reader that drained one
	// pipe at a time would deadlock once the other pipe's buffer filled.
	string streamed;
	size_t streamedErr = 0;
	ok = CProcessSpawner::Run({ shell, "-c", "head -c 1048576 /dev/zero >&2 & head -c 1048576 /dev/zero; wait" },
							  opts, r, 20000, nullptr,
							  [&streamed, &streamedErr](bool isStdErr, const char *data, size_t length) {
								  if (isStdErr)
									  streamedErr += length;
								  else
									  streamed.append(data, length);
							  });
	PS_ASSERT(ok && r.stdOut.size() == 1048576 && r.stdErr.size() == 1048576,
			  "large stdout and stderr are drained together");
	PS_ASSERT(streamed == r.stdOut && streamedErr == r.stdErr.size(), "onOutput streams the same bytes");
#endif

	TestCompleted(true, "CProcessSpawner::Run passed");
}
