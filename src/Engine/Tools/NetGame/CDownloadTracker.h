#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <stdint.h>

// Generic progress snapshot for a single download slot.
struct SDownloadProgress
{
	bool isSyncing = false;
	bool isComplete = false;
	bool hasError = false;
	std::string errorMessage;

	std::string currentFileName;
	int filesCompleted = 0;
	int filesTotal = 0;
	uint64_t bytesCompleted = 0;
	uint64_t bytesTotal = 0;
};

struct SDownloadEntry
{
	int id;
	std::string label;
	SDownloadProgress progress;
	bool active;  // false once completed or failed
};

// Central registry for concurrent background downloads.
// Each downloader calls Register() once, Update() every frame, then Complete()/Fail().
// Thread-safe: Update/Complete/Fail may be called from worker threads.
//
// Usage:
//   int id = tracker.Register("Assets (slajerek)");
//   tracker.Update(id, progress);   // each frame from Poll()
//   tracker.Complete(id);           // on success
//   tracker.Fail(id, "error msg");  // on failure
class CDownloadTracker
{
public:
	CDownloadTracker();

	int Register(const std::string &label);
	void Update(int id, const SDownloadProgress &progress);
	void Complete(int id);
	void Fail(int id, const std::string &error);

	// Snapshot of all active (and recently-failed) entries for UI rendering.
	std::vector<SDownloadEntry> GetActive() const;
	bool HasAnyActive() const;

private:
	mutable std::mutex mutex;
	std::vector<SDownloadEntry> entries;
	int nextId;

	SDownloadEntry *FindEntry(int id);
};
