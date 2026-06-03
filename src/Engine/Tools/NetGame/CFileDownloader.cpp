#include "CFileDownloader.h"

#include "SYS_FileSystem.h"

#include "httplib.h"

#include "DBG_Log.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace std;
namespace fs = std::filesystem;

static bool EnsureParentDirExists_FD(const string &path)
{
	try
	{
		fs::path p(path.c_str());
		fs::path parent = p.parent_path();
		if (parent.empty())
			return true;
		fs::create_directories(parent);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

static uint64_t GetFileSizeOrZero_FD(const string &path)
{
	std::error_code ec;
	uintmax_t sz = fs::file_size(fs::path(path.c_str()), ec);
	if (ec)
		return 0;
	return (uint64_t)sz;
}

CFileDownloader::CFileDownloader()
{
	downloading = false;
	totalKnown = false;
	totalBytes = 0;
	downloadedBytes = 0;
	totalDirty = false;

	stopRequested = false;
	workerDone = false;
	workerSuccess = false;
	workerHttpStatus = 0;

	dlMutex = new CSlrMutex("CFileDownloader");
}

CFileDownloader::~CFileDownloader()
{
	Shutdown();
	delete dlMutex;
	dlMutex = nullptr;
}

bool CFileDownloader::IsDownloading() const { return downloading; }

bool CFileDownloader::IsSuccess() const { return workerSuccess; }

uint64_t CFileDownloader::GetTotalBytes() const { return totalBytes; }

uint64_t CFileDownloader::GetDownloadedBytes() const { return downloadedBytes; }

std::string CFileDownloader::GetLastError() const
{
	// Not locking for a simple string read in typical usage
	return lastError;
}

std::string CFileDownloader::GetDstPath() const { return dstPath; }

std::string CFileDownloader::GetTmpPath() const { return tmpPath; }

bool CFileDownloader::StartDownload(const std::string &u, const std::string &dst)
{
	if (u.empty() || dst.empty())
		return false;

	dlMutex->Lock();
	if (downloading)
	{
		lastError = "Download already in progress";
		dlMutex->Unlock();
		return false;
	}
	dlMutex->Unlock();

	if (!EnsureParentDirExists_FD(dst))
	{
		dlMutex->Lock();
		lastError = "Failed to create destination folder";
		dlMutex->Unlock();
		return false;
	}

	dlMutex->Lock();
	url = u;
	dstPath = dst;
	tmpPath = dst + ".part";
	lastError.clear();
	downloading = true;
	stopRequested = false;
	workerDone = false;
	workerSuccess = false;
	workerHttpStatus = 0;
	totalBytes = 0;
	totalKnown = false;
	totalDirty = false;
	downloadedBytes = 0;
	dlMutex->Unlock();

	StartWorkerThread();
	return true;
}

void CFileDownloader::StartWorkerThread()
{
	dlMutex->Lock();
	std::string u = url;
	std::string dst = dstPath;
	std::string tmp = tmpPath;
	dlMutex->Unlock();

	if (worker.joinable())
		worker.join();

	worker = std::thread(&CFileDownloader::WorkerThreadMain, this, u, dst, tmp);
}

bool CFileDownloader::ParseUrl(const std::string &u, std::string *schemeHostPort, std::string *path)
{
	if (!schemeHostPort || !path)
		return false;

	auto p = u.find("://");
	if (p == std::string::npos)
		return false;

	std::string scheme = u.substr(0, p);
	std::string rest = u.substr(p + 3);
	if (scheme.empty() || rest.empty())
		return false;

	auto slash = rest.find('/');
	std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
	std::string pth = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
	if (hostport.empty())
		return false;

	*schemeHostPort = scheme + "://" + hostport;
	*path = pth.empty() ? std::string("/") : pth;
	return true;
}

bool CFileDownloader::ParseContentRangeTotal(const std::string &contentRange, uint64_t *outTotal)
{
	if (!outTotal)
		return false;

	auto slash = contentRange.find('/');
	if (slash == std::string::npos)
		return false;

	std::string totalStr = contentRange.substr(slash + 1);
	if (totalStr.empty() || totalStr == "*")
		return false;

	uint64_t total = 0;
	for (char c : totalStr)
	{
		if (c < '0' || c > '9')
			break;
		total = total * 10ULL + (uint64_t)(c - '0');
	}
	if (total == 0)
		return false;

	*outTotal = total;
	return true;
}

void CFileDownloader::WorkerThreadMain(std::string u, std::string dst, std::string tmp)
{
	(void)dst;

	std::string schemeHostPort;
	std::string path;
	if (!ParseUrl(u, &schemeHostPort, &path))
	{
		dlMutex->Lock();
		workerSuccess = false;
		workerHttpStatus = 0;
		lastError = "Download failed: invalid URL";
		workerDone = true;
		dlMutex->Unlock();
		return;
	}

	uint64_t resumeFrom = GetFileSizeOrZero_FD(tmp);
	{
		dlMutex->Lock();
		downloadedBytes = resumeFrom;
		dlMutex->Unlock();
	}

	// Capture auth header for use in lambda
	dlMutex->Lock();
	std::string authHeader = authorizationHeader;
	dlMutex->Unlock();

	auto run_get = [&](bool useRange, uint64_t startFrom) -> httplib::Result {
		httplib::Client cli(schemeHostPort);
		cli.set_follow_location(true);
		cli.set_read_timeout(readTimeoutSec, 0);
		cli.set_write_timeout(30, 0);
		cli.set_connection_timeout(connectionTimeoutSec, 0);

		httplib::Headers headers;
		headers.emplace("User-Agent", "LightHeroes");
		if (!authHeader.empty())
			headers.emplace("Authorization", authHeader);
		if (useRange && startFrom > 0)
			headers.emplace("Range", "bytes=" + std::to_string((unsigned long long)startFrom) + "-");

		std::unique_ptr<std::ofstream> out;
		bool shouldRestartWithoutRange = false;
		uint64_t contentRangeTotal = 0;
		bool hasContentRangeTotal = false;

		auto response_handler = [&](const httplib::Response &res) -> bool {
			dlMutex->Lock();
			workerHttpStatus = res.status;
			dlMutex->Unlock();

			if (stopRequested)
				return false;

			if (useRange && startFrom > 0 && res.status == 200)
			{
				shouldRestartWithoutRange = true;
				return false;
			}

			if (!(res.status == 200 || res.status == 206))
				return false;

			if (res.has_header("Content-Range"))
			{
				uint64_t t = 0;
				if (ParseContentRangeTotal(res.get_header_value("Content-Range"), &t))
				{
					contentRangeTotal = t;
					hasContentRangeTotal = true;
				}
			}
			else if (res.has_header("Content-Length"))
			{
				uint64_t t = 0;
				try { t = (uint64_t)stoull(res.get_header_value("Content-Length")); } catch (...) { t = 0; }
				if (t > 0 && (!useRange || startFrom == 0))
				{
					dlMutex->Lock();
					totalBytes = t;
					totalKnown = true;
					totalDirty = true;
					dlMutex->Unlock();
				}
			}

			std::ios::openmode mode = std::ios::binary | std::ios::out;
			mode |= (useRange && startFrom > 0) ? std::ios::app : std::ios::trunc;
			out.reset(new std::ofstream(tmp, mode));
			return out->good();
		};

		auto content_receiver = [&](const char *data, size_t data_length) -> bool {
			if (stopRequested)
				return false;
			if (!out)
				return false;
			out->write(data, (std::streamsize)data_length);
			return out->good();
		};

		auto progress_cb = [&](size_t current, size_t total) -> bool {
			if (stopRequested)
				return false;
			uint64_t dl = startFrom + (uint64_t)current;
			dlMutex->Lock();
			downloadedBytes = dl;
			if (hasContentRangeTotal)
			{
				totalBytes = contentRangeTotal;
				totalKnown = true;
				totalDirty = true;
			}
			else if (total > 0)
			{
				uint64_t t = (uint64_t)total;
				if (useRange && startFrom > 0)
					t += startFrom;
				totalBytes = t;
				totalKnown = true;
				totalDirty = true;
			}
			dlMutex->Unlock();

			if (onProgress)
				onProgress(dl, totalBytes);

			return true;
		};

		auto result = cli.Get(path, headers, response_handler, content_receiver, progress_cb);
		if (shouldRestartWithoutRange)
			return httplib::Result(std::unique_ptr<httplib::Response>(), httplib::Error::Canceled);
		return result;
	};

	httplib::Result res = run_get(resumeFrom > 0, resumeFrom);
	if (!stopRequested && resumeFrom > 0 && (!res || res.error() == httplib::Error::Canceled))
	{
		resumeFrom = 0;
		res = run_get(false, 0);
	}

	bool ok = false;
	int status = 0;
	std::string err;
	if (stopRequested)
	{
		err = "Download cancelled";
	}
	else if (!res)
	{
		std::ostringstream ss;
		ss << "Download failed (network error " << (int)res.error() << ")";
		err = ss.str();
	}
	else
	{
		status = res->status;
		ok = (status == 200 || status == 206);
		if (!ok)
			err = "Download failed (HTTP " + std::to_string(status) + ")";
	}

	dlMutex->Lock();
	workerSuccess = ok;
	workerHttpStatus = status;
	if (!ok && !err.empty())
		lastError = err;
	workerDone = true;
	dlMutex->Unlock();
}

void CFileDownloader::FinalizeRename()
{
	dlMutex->Lock();
	string tmp = tmpPath;
	string dst = dstPath;
	dlMutex->Unlock();

	try
	{
		std::error_code ec;
		if (fs::exists(fs::path(tmp.c_str()), ec))
		{
			fs::rename(fs::path(tmp.c_str()), fs::path(dst.c_str()), ec);
			if (ec)
			{
				ec.clear();
				fs::copy_file(fs::path(tmp.c_str()), fs::path(dst.c_str()), fs::copy_options::overwrite_existing, ec);
				if (!ec)
					fs::remove(fs::path(tmp.c_str()), ec);
			}
		}
	}
	catch (...) {}
}

bool CFileDownloader::Poll()
{
	if (!downloading)
		return false;

	// Refresh downloaded bytes from file size
	dlMutex->Lock();
	string tmp = tmpPath;
	dlMutex->Unlock();

	if (!tmp.empty())
	{
		uint64_t sz = GetFileSizeOrZero_FD(tmp);
		dlMutex->Lock();
		if (sz > downloadedBytes)
			downloadedBytes = sz;
		dlMutex->Unlock();
	}

	dlMutex->Lock();
	bool done = workerDone;
	bool ok = workerSuccess;
	string err = lastError;
	dlMutex->Unlock();

	if (!done)
		return false;

	if (worker.joinable())
		worker.join();

	if (ok)
	{
		FinalizeRename();
		dlMutex->Lock();
		downloading = false;
		lastError.clear();
		dlMutex->Unlock();
	}
	else
	{
		dlMutex->Lock();
		downloading = false;
		if (lastError.empty())
			lastError = "Download failed";
		dlMutex->Unlock();
	}

	if (onComplete)
		onComplete(ok, ok ? "" : err);

	return true;
}

void CFileDownloader::Shutdown()
{
	stopRequested = true;
	if (worker.joinable())
		worker.join();

	dlMutex->Lock();
	downloading = false;
	dlMutex->Unlock();
}
