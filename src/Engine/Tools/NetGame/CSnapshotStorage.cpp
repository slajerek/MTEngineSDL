#include "CSnapshotStorage.h"
#include "DBG_Log.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================================
// CLocalFsSnapshotStorage
// ============================================================================

CLocalFsSnapshotStorage::CLocalFsSnapshotStorage(const string &baseDir)
	: baseDir(baseDir)
{
}

CLocalFsSnapshotStorage::~CLocalFsSnapshotStorage()
{
}

string CLocalFsSnapshotStorage::GetSnapshotPath(const string &gameId) const
{
	return baseDir + "/snapshots/" + gameId + "/snapshot.json";
}

bool CLocalFsSnapshotStorage::Upload(const string &gameId, const string &localPath)
{
	string destPath = GetSnapshotPath(gameId);

	// Ensure directory exists
	fs::path destDir = fs::path(destPath).parent_path();
	std::error_code ec;
	fs::create_directories(destDir, ec);
	if (ec)
	{
		LOGError("CLocalFsSnapshotStorage::Upload: failed to create dir %s: %s",
				 destDir.string().c_str(), ec.message().c_str());
		return false;
	}

	// Copy file
	fs::copy_file(localPath, destPath, fs::copy_options::overwrite_existing, ec);
	if (ec)
	{
		LOGError("CLocalFsSnapshotStorage::Upload: failed to copy %s -> %s: %s",
				 localPath.c_str(), destPath.c_str(), ec.message().c_str());
		return false;
	}

	LOGD("CLocalFsSnapshotStorage::Upload: gameId=%s uploaded to %s", gameId.c_str(), destPath.c_str());
	return true;
}

bool CLocalFsSnapshotStorage::Download(const string &gameId, const string &localPath)
{
	string srcPath = GetSnapshotPath(gameId);

	if (!fs::exists(srcPath))
	{
		LOGError("CLocalFsSnapshotStorage::Download: snapshot not found for gameId=%s at %s",
				 gameId.c_str(), srcPath.c_str());
		return false;
	}

	// Ensure destination directory exists
	fs::path destDir = fs::path(localPath).parent_path();
	std::error_code ec;
	if (!destDir.empty())
		fs::create_directories(destDir, ec);

	fs::copy_file(srcPath, localPath, fs::copy_options::overwrite_existing, ec);
	if (ec)
	{
		LOGError("CLocalFsSnapshotStorage::Download: failed to copy %s -> %s: %s",
				 srcPath.c_str(), localPath.c_str(), ec.message().c_str());
		return false;
	}

	LOGD("CLocalFsSnapshotStorage::Download: gameId=%s downloaded to %s", gameId.c_str(), localPath.c_str());
	return true;
}

bool CLocalFsSnapshotStorage::Exists(const string &gameId)
{
	return fs::exists(GetSnapshotPath(gameId));
}

bool CLocalFsSnapshotStorage::Delete(const string &gameId)
{
	string path = GetSnapshotPath(gameId);
	std::error_code ec;
	fs::remove(path, ec);
	if (ec)
	{
		LOGError("CLocalFsSnapshotStorage::Delete: failed to remove %s: %s",
				 path.c_str(), ec.message().c_str());
		return false;
	}

	// Also try to remove the game directory if empty
	fs::path gameDir = fs::path(path).parent_path();
	fs::remove(gameDir, ec); // ignore error — may not be empty

	return true;
}
