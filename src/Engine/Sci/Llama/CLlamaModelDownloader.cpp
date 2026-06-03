#include "CLlamaModelDownloader.h"

#include "CFileDownloader.h"
#include "CLlamaModelManager.h"

#include "SYS_DefaultConfig.h"
#include "SYS_FileSystem.h"

#include "DBG_Log.h"

#include <filesystem>

using namespace std;
namespace fs = std::filesystem;

static const char *kCfgActive = "llama.download.active";
static const char *kCfgModelId = "llama.download.modelId";
static const char *kCfgUrl = "llama.download.url";
static const char *kCfgDst = "llama.download.dst";
static const char *kCfgTmp = "llama.download.tmp";
static const char *kCfgHeaders = "llama.download.headers"; // legacy key (from curl implementation)
static const char *kCfgTotalBytes = "llama.download.totalBytes";

CLlamaModelDownloader::CLlamaModelDownloader()
{
	active = false;
	totalBytes = 0;
	totalDirty = false;
	downloader = nullptr;

	LoadFromConfig();
}

CLlamaModelDownloader::~CLlamaModelDownloader()
{
	Shutdown();
	delete downloader;
	downloader = nullptr;
}

void CLlamaModelDownloader::LoadFromConfig()
{
	active = false;
	totalBytes = 0;
	totalDirty = false;
	lastError.clear();
	modelId.clear();
	url.clear();
	dstPath.clear();
	tmpPath.clear();

	if (gApplicationDefaultConfig)
	{
		bool a = gApplicationDefaultConfig->GetBool(kCfgActive, false);
		if (a)
		{
			string s;
			gApplicationDefaultConfig->GetStdString(kCfgModelId, &modelId, "");
			gApplicationDefaultConfig->GetStdString(kCfgUrl, &url, "");
			gApplicationDefaultConfig->GetStdString(kCfgDst, &dstPath, "");
			gApplicationDefaultConfig->GetStdString(kCfgTmp, &tmpPath, "");
			gApplicationDefaultConfig->GetStdString(kCfgTotalBytes, &s, "0");
			try { totalBytes = (uint64_t)stoull(s); } catch (...) { totalBytes = 0; }
			active = true;
		}
	}
}

void CLlamaModelDownloader::SaveActiveToConfig(bool a)
{
	if (!gApplicationDefaultConfig)
		return;

	gApplicationDefaultConfig->SetBoolSkipConfigSave(kCfgActive, &a);
	if (a)
	{
		gApplicationDefaultConfig->SetStdString(kCfgModelId, &modelId);
		gApplicationDefaultConfig->SetStdString(kCfgUrl, &url);
		gApplicationDefaultConfig->SetStdString(kCfgDst, &dstPath);
		gApplicationDefaultConfig->SetStdString(kCfgTmp, &tmpPath);
		gApplicationDefaultConfig->SetStdString(kCfgTotalBytes, std::to_string((unsigned long long)totalBytes));
	}

	gApplicationDefaultConfig->SaveConfig();
}

void CLlamaModelDownloader::ClearConfig()
{
	if (!gApplicationDefaultConfig)
		return;

	bool a = false;
	gApplicationDefaultConfig->SetBoolSkipConfigSave(kCfgActive, &a);
	gApplicationDefaultConfig->SetStdString(kCfgModelId, std::string(""));
	gApplicationDefaultConfig->SetStdString(kCfgUrl, std::string(""));
	gApplicationDefaultConfig->SetStdString(kCfgDst, std::string(""));
	gApplicationDefaultConfig->SetStdString(kCfgTmp, std::string(""));
	gApplicationDefaultConfig->SetStdString(kCfgHeaders, std::string(""));
	gApplicationDefaultConfig->SetStdString(kCfgTotalBytes, std::string("0"));
	gApplicationDefaultConfig->SaveConfig();
}

bool CLlamaModelDownloader::IsActive() const { return active; }

bool CLlamaModelDownloader::IsDownloading() const
{
	return downloader && downloader->IsDownloading();
}

std::string CLlamaModelDownloader::GetModelId() const { return modelId; }
std::string CLlamaModelDownloader::GetDstPath() const { return dstPath; }
std::string CLlamaModelDownloader::GetTmpPath() const { return tmpPath; }

uint64_t CLlamaModelDownloader::GetTotalBytes() const
{
	return downloader ? downloader->GetTotalBytes() : totalBytes;
}

uint64_t CLlamaModelDownloader::GetDownloadedBytes() const
{
	return downloader ? downloader->GetDownloadedBytes() : 0;
}

std::string CLlamaModelDownloader::GetLastError() const
{
	if (downloader)
	{
		string err = downloader->GetLastError();
		if (!err.empty())
			return err;
	}
	return lastError;
}

void CLlamaModelDownloader::TryAutoResume()
{
	bool shouldResume = active && !IsDownloading() && !modelId.empty() && !url.empty() && !dstPath.empty();

	if (!shouldResume)
		return;

	if (SYS_FileExists(dstPath.c_str()))
	{
		active = false;
		ClearConfig();
		return;
	}

	StartDownload(modelId, url, dstPath);
}

bool CLlamaModelDownloader::StartDownload(const std::string &mid, const std::string &u, const std::string &dst)
{
	if (mid.empty() || u.empty() || dst.empty())
		return false;

	if (IsDownloading())
	{
		lastError = "Download already in progress";
		return false;
	}

	modelId = mid;
	url = u;
	dstPath = dst;
	tmpPath = dst + ".part";
	lastError.clear();
	active = true;
	totalDirty = false;

	SaveActiveToConfig(true);

	// Ensure the destination directory exists (e.g. publisher/repo/ subfolder)
	try { fs::create_directories(fs::path(dst).parent_path()); } catch (...) {}

	delete downloader;
	downloader = new CFileDownloader();
	if (!downloader->StartDownload(u, dst))
	{
		lastError = downloader->GetLastError();
		active = false;
		delete downloader;
		downloader = nullptr;
		return false;
	}

	return true;
}

void CLlamaModelDownloader::FinalizeSuccess(CLlamaModelManager *modelManager)
{
	if (modelManager)
		modelManager->SetModelPath(modelId, dstPath);

	active = false;
	lastError.clear();

	ClearConfig();
}

void CLlamaModelDownloader::SetErrorAndStop(const std::string &msg)
{
	lastError = msg;
	active = false;

	ClearConfig();
}

void CLlamaModelDownloader::Update(CLlamaModelManager *modelManager)
{
	if (!downloader || !active)
		return;

	// Persist total size once it becomes known
	uint64_t tot = downloader->GetTotalBytes();
	if (tot > 0 && tot != totalBytes && gApplicationDefaultConfig)
	{
		totalBytes = tot;
		gApplicationDefaultConfig->SetStdString(kCfgTotalBytes, std::to_string((unsigned long long)tot));
		gApplicationDefaultConfig->SaveConfig();
	}

	bool finished = downloader->Poll();
	if (!finished)
		return;

	bool ok = downloader->IsSuccess();
	string err = downloader->GetLastError();

	delete downloader;
	downloader = nullptr;

	if (ok)
		FinalizeSuccess(modelManager);
	else
		SetErrorAndStop(err.empty() ? "Download failed" : err);
}

void CLlamaModelDownloader::Shutdown()
{
	if (downloader)
	{
		downloader->Shutdown();
		delete downloader;
		downloader = nullptr;
	}
	active = false;
}
