#pragma once

#include <string>

using namespace std;

// Interface for game state snapshot storage (upload/download to shared storage).
// Implementations: CLocalFsSnapshotStorage (local-dev/test) and CMinioSnapshotStorage (cluster).
class ISnapshotStorage
{
public:
	virtual ~ISnapshotStorage() {}

	// Upload a local snapshot file to shared storage for the given gameId.
	virtual bool Upload(const string &gameId, const string &localPath) = 0;

	// Download the latest snapshot for a gameId from shared storage to localPath.
	virtual bool Download(const string &gameId, const string &localPath) = 0;

	// Check if a snapshot exists in shared storage for a gameId.
	virtual bool Exists(const string &gameId) = 0;

	// Delete snapshot from shared storage.
	virtual bool Delete(const string &gameId) = 0;
};

// Local filesystem snapshot storage — copies files to a local directory.
// Used for local-dev mode and testing.
class CLocalFsSnapshotStorage : public ISnapshotStorage
{
public:
	// baseDir: root directory for snapshots (e.g., "./data/dev-game-state")
	CLocalFsSnapshotStorage(const string &baseDir);
	virtual ~CLocalFsSnapshotStorage();

	virtual bool Upload(const string &gameId, const string &localPath) override;
	virtual bool Download(const string &gameId, const string &localPath) override;
	virtual bool Exists(const string &gameId) override;
	virtual bool Delete(const string &gameId) override;

	string GetSnapshotPath(const string &gameId) const;

private:
	string baseDir;
};
