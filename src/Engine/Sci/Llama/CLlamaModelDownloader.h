#pragma once

#include <string>
#include <stdint.h>

class CLlamaModelManager;
class CFileDownloader;

// Resumable model downloader that uses CFileDownloader for the actual HTTP transfer.
// Persists one active download in settings.hjson so it can resume after restart.
class CLlamaModelDownloader
{
public:
	CLlamaModelDownloader();
	~CLlamaModelDownloader();

	// Resume an unfinished download from settings (if any).
	void TryAutoResume();

	// Start (or resume) downloading `url` to `dstPath` for the given modelId.
	bool StartDownload(const std::string &modelId, const std::string &url, const std::string &dstPath);

	// Poll worker thread state, finalize rename, update settings.
	void Update(CLlamaModelManager *modelManager);

	// Stop a running download if any.
	void Shutdown();

	bool IsActive() const;
	bool IsDownloading() const;
	std::string GetModelId() const;
	std::string GetDstPath() const;
	std::string GetTmpPath() const;
	uint64_t GetTotalBytes() const;
	uint64_t GetDownloadedBytes() const;
	std::string GetLastError() const;

private:
	void LoadFromConfig();
	void SaveActiveToConfig(bool active);
	void ClearConfig();

	void FinalizeSuccess(CLlamaModelManager *modelManager);
	void SetErrorAndStop(const std::string &msg);

	// Model-specific state
	bool active;
	std::string modelId;
	std::string url;
	std::string dstPath;
	std::string tmpPath;
	std::string lastError;
	uint64_t totalBytes;
	bool totalDirty;

	// Delegates actual downloading to CFileDownloader
	CFileDownloader *downloader;
};
