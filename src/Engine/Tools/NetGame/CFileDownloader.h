#pragma once

#include <string>
#include <stdint.h>
#include <thread>
#include <atomic>
#include <functional>

#include "SYS_Threading.h"

// Generic resumable HTTP file downloader with Range header support.
// Downloads to `<dst>.part` then renames on success. Thread-safe progress tracking.
//
// Usage:
//   CFileDownloader dl;
//   dl.onProgress = [](uint64_t downloaded, uint64_t total) { ... };
//   dl.StartDownload("https://example.com/file.bin", "/path/to/file.bin");
//   // Call Poll() each frame; it returns true when download completes
//   dl.Shutdown();
class CFileDownloader
{
public:
	CFileDownloader();
	~CFileDownloader();

	// Start or resume download. dstPath is final file; temp is dstPath + ".part".
	// Returns false if already downloading or invalid args.
	bool StartDownload(const std::string &url, const std::string &dstPath);

	// Stop current download (safe to call if not downloading)
	void Shutdown();

	// Poll worker thread state; returns true if download just finished (success or fail).
	// On success, .part is renamed to dstPath. Call from main thread.
	bool Poll();

	// State queries (thread-safe)
	bool IsDownloading() const;
	bool IsSuccess() const;
	uint64_t GetTotalBytes() const;
	uint64_t GetDownloadedBytes() const;
	std::string GetLastError() const;
	std::string GetDstPath() const;
	std::string GetTmpPath() const;

	// Optional progress callback (called from worker thread)
	std::function<void(uint64_t downloaded, uint64_t total)> onProgress;

	// Optional completion callback (called from Poll on main thread)
	std::function<void(bool success, const std::string &error)> onComplete;

	// Optional auth header (e.g., "Bearer <token>")
	std::string authorizationHeader;

	// Timeouts
	int connectionTimeoutSec = 30;
	int readTimeoutSec = 300;

private:
	void StartWorkerThread();
	void WorkerThreadMain(std::string url, std::string dstPath, std::string tmpPath);

	bool ParseUrl(const std::string &url, std::string *schemeHostPort, std::string *path);
	bool ParseContentRangeTotal(const std::string &contentRange, uint64_t *outTotal);
	void FinalizeRename();

	std::atomic<bool> stopRequested;
	std::atomic<bool> workerDone;
	bool workerSuccess;
	int workerHttpStatus;
	std::thread worker;

	bool downloading;
	uint64_t totalBytes;
	uint64_t downloadedBytes;
	bool totalKnown;
	bool totalDirty;

	std::string url;
	std::string dstPath;
	std::string tmpPath;
	std::string lastError;

	CSlrMutex *dlMutex;
};
