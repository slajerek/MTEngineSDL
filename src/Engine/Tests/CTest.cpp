#include "SYS_Defs.h"
#include "CTest.h"
#include "DBG_Log.h"

#include <cstdlib>
#include <filesystem>

// The directory the process started in, captured by SYS_InitFileSystem on
// every platform. Declared here rather than through the per-platform
// SYS_FileSystem.h, the way other engine-neutral code reaches these globals.
extern char *gCPathToCurrentDirectory;

CTest::CTest()
{
	currentStep = 0;
	isRunning = false;
	callback = NULL;
}

CTest::~CTest()
{
}

void CTest::StepCompleted(int stepId, bool success, const char *message)
{
	if (callback)
	{
		callback->OnTestStepCompleted(this, stepId, success, message);
	}
}

void CTest::ReportRequiredGap(const char *what)
{
	if (what == NULL || what[0] == 0)
		return;
	if (!requiredGap.empty())
		requiredGap += "; ";
	requiredGap += what;
	// LOUD even in a normal run: the whole point is that this stops being
	// something only a careful reader of a summary string would notice.
	LOGError("CTest: %s -- REQUIRED COVERAGE MISSING: %s", GetName(), what);
}

void CTest::TestSkipped(const char *reason)
{
	// Set BEFORE completing: TestCompleted() dispatches to the suite
	// synchronously, and the suite reads WasSkipped() inside that callback.
	skipped = true;
	TestCompleted(true, reason);
}

void CTest::TestCompleted(bool success, const char *summary)
{
	if (completed)
	{
		// Never re-enter the suite's dispatch chain (see CTest.h). Loud,
		// because a double completion means the test's own control flow is
		// wrong even though the run may still score green.
		LOGError("CTest: %s completed twice ('%s') -- ignoring the second call",
		         GetName(), summary ? summary : "");
		return;
	}
	completed = true;
	isRunning = false;
	Teardown();
	if (callback)
	{
		callback->OnTestCompleted(this, success, summary);
	}
}

// --- the project root ---------------------------------------------------------

static std::string FindProjectRoot()
{
	namespace fs = std::filesystem;

	const char *fromEnv = getenv("MT_TEST_PROJECT_DIR");
	if (fromEnv != NULL && fromEnv[0] != '\0')
		return std::string(fromEnv);

	std::error_code ec;
	fs::path dir;
	if (gCPathToCurrentDirectory != NULL && gCPathToCurrentDirectory[0] != '\0')
		dir = fs::path(gCPathToCurrentDirectory);
	else
		dir = fs::current_path(ec);
	if (ec || dir.empty())
		return std::string();

	// mtengine.caps marks an app root; .git marks any checkout, and it is a
	// FILE in a worktree, so "exists" rather than "is_directory". Sixteen
	// levels is far more than platform/<P>/prod/<arch>/ needs and bounds a
	// walk that starts somewhere unexpected.
	for (int depth = 0; depth < 16; depth++)
	{
		std::error_code ec2;
		if (fs::exists(dir / "mtengine.caps", ec2) || fs::exists(dir / ".git", ec2))
			return dir.string();
		fs::path parent = dir.parent_path();
		if (parent == dir)
			break;
		dir = parent;
	}
	return std::string();
}

const std::string &CTest::ProjectRootPath()
{
	static const std::string root = FindProjectRoot();
	return root;
}

std::string CTest::ResolveProjectPath(const char *relative)
{
	const std::string &root = ProjectRootPath();
	if (root.empty() || relative == NULL)
		return std::string();
	if (relative[0] == '\0')
		return root;
	return (std::filesystem::path(root) / relative).string();
}
