// CAPABILITY GUARD (MT_ENABLE_HTTPLIB). The file stays in all three build lists and
// compiles to nothing when the capability is off -- an Xcode PBXBuildFile
// cannot be conditioned on a build setting, so the condition lives here.
// MT_CAP_HTTP declared this removable and nothing enforced it: MT_ENABLE_HTTPLIB appeared in no source at all.
#if MT_ENABLE_HTTPLIB

#include "CAssetServer.h"

#include "NetGameListenPolicy.h"
#include "SYS_Crypto.h"
#include "SYS_FileSystem.h"

#include "json.hpp"
#include "httplib.h"

#include "DBG_Log.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>

using namespace std;
namespace fs = std::filesystem;

CAssetServer::CAssetServer(int port)
{
	this->port = port;
	this->listenInterface = "127.0.0.1";
	this->useSSL = false;
	this->allowPlaintext = false;
	this->authServicePort = 14880;
	this->authServiceAddress = "localhost";
	this->isRunning = false;
	this->httpServer = nullptr;

	mutexTokenCache = new CSlrMutex("CAssetServer::tokenCache");
	mutexManifests = new CSlrMutex("CAssetServer::manifests");
}

CAssetServer::~CAssetServer()
{
	Shutdown();
	delete mutexTokenCache;
	delete mutexManifests;
}

// ── CRC32 cache ───────────────────────────────────────────────────────

static int64_t FileTimeToTicks(fs::file_time_type ftime)
{
	return (int64_t)ftime.time_since_epoch().count();
}

void CAssetServer::LoadCrc32Cache(const std::string &cachePath)
{
	crc32Cache.clear();
	if (!fs::exists(cachePath))
		return;

	try
	{
		ifstream f(cachePath);
		if (!f.is_open()) return;
		nlohmann::json j = nlohmann::json::parse(f);
		if (!j.contains("entries") || !j["entries"].is_object())
			return;
		for (auto &[path, val] : j["entries"].items())
		{
			SCrc32CacheEntry entry;
			entry.mtime = val.value("mtime", (int64_t)0);
			entry.size = val.value("size", (uint64_t)0);
			entry.crc32 = val.value("crc32", "");
			if (!entry.crc32.empty())
				crc32Cache[path] = entry;
		}
		LOGD("CAssetServer: loaded CRC32 cache: %d entries from %s", (int)crc32Cache.size(), cachePath.c_str());
	}
	catch (...)
	{
		LOGD("CAssetServer: failed to load CRC32 cache from %s", cachePath.c_str());
		crc32Cache.clear();
	}
}

void CAssetServer::SaveCrc32Cache(const std::string &cachePath)
{
	nlohmann::json j;
	nlohmann::json entries = nlohmann::json::object();
	for (auto &[path, entry] : crc32Cache)
	{
		nlohmann::json e;
		e["mtime"] = entry.mtime;
		e["size"] = entry.size;
		e["crc32"] = entry.crc32;
		entries[path] = e;
	}
	j["entries"] = entries;

	try
	{
		ofstream f(cachePath);
		if (f.is_open())
			f << j.dump();
	}
	catch (...) {}
}

std::string CAssetServer::GetCachedOrComputeCrc32(const std::string &absPath, uint64_t fileSize, int64_t mtime)
{
	auto it = crc32Cache.find(absPath);
	if (it != crc32Cache.end()
		&& it->second.mtime == mtime
		&& it->second.size == fileSize)
	{
		return it->second.crc32;
	}

	bool ok = false;
	uint32_t crc = SYS_Crc32File(absPath, &ok);
	string crc32hex = ok ? SYS_Crc32ToHex(crc) : "";

	if (!crc32hex.empty())
	{
		SCrc32CacheEntry entry;
		entry.mtime = mtime;
		entry.size = fileSize;
		entry.crc32 = crc32hex;
		crc32Cache[absPath] = entry;
	}

	return crc32hex;
}

// ── Manifest building ──────────────────────────────────────────────────

SAssetManifest CAssetServer::BuildManifestForFolder(const std::string &folderPath)
{
	SAssetManifest manifest;
	manifest.version = 1;

	if (folderPath.empty() || !fs::exists(folderPath))
		return manifest;

	for (auto &entry : fs::recursive_directory_iterator(folderPath, fs::directory_options::skip_permission_denied))
	{
		if (!entry.is_regular_file())
			continue;

		string filename = entry.path().filename().string();
		// Skip hidden files
		if (!filename.empty() && filename[0] == '.')
			continue;

		// Get relative path from folderPath
		string relPath = fs::relative(entry.path(), folderPath).string();
		// Normalize separators to forward slash
		replace(relPath.begin(), relPath.end(), '\\', '/');

		uint64_t fileSize = (uint64_t)entry.file_size();
		int64_t mtime = FileTimeToTicks(entry.last_write_time());
		string absPath = entry.path().string();

		string crc32hex = GetCachedOrComputeCrc32(absPath, fileSize, mtime);
		if (crc32hex.empty())
			continue;

		SAssetManifestEntry e;
		e.path = relPath;
		e.crc32 = crc32hex;
		e.size = fileSize;
		manifest.files.push_back(e);
	}

	// Sort by path for deterministic manifest hash
	sort(manifest.files.begin(), manifest.files.end(),
		[](const SAssetManifestEntry &a, const SAssetManifestEntry &b) { return a.path < b.path; });

	manifest.manifestHash = ComputeManifestHash(manifest);
	return manifest;
}

std::string CAssetServer::ComputeManifestHash(const SAssetManifest &manifest)
{
	string combined;
	for (auto &f : manifest.files)
	{
		combined += f.path;
		combined += ":";
		combined += f.crc32;
		combined += "\n";
	}
	uint32_t crc = SYS_Crc32((const uint8_t *)combined.data(), combined.size());
	return SYS_Crc32ToHex(crc);
}

std::vector<std::string> CAssetServer::ScanProjectIds()
{
	vector<string> ids;
	if (projectsFolder.empty() || !fs::exists(projectsFolder))
		return ids;

	for (auto &entry : fs::directory_iterator(projectsFolder))
	{
		if (!entry.is_regular_file())
			continue;

		string name = entry.path().stem().string();
		string ext = entry.path().extension().string();
		bool isLegacyProjectName = name.find("project") == 0;
		bool isServerProjectName = name.size() >= 7
			&& name.compare(name.size() - 7, 7, "-server") == 0;
		if ((ext == ".json" || ext == ".hjson") && (isLegacyProjectName || isServerProjectName))
		{
			ids.push_back(name);
		}
	}

	sort(ids.begin(), ids.end());
	return ids;
}

void CAssetServer::RebuildManifests()
{
	mutexManifests->Lock();

	// Load CRC32 cache from disk (speeds up subsequent rebuilds)
	string cachePath = assetsFolder + "/.manifest-cache.json";
	LoadCrc32Cache(cachePath);

	assetsManifest = BuildManifestForFolder(assetsFolder);
	LOGM("CAssetServer: built assets manifest: %d files, hash=%s",
		 (int)assetsManifest.files.size(), assetsManifest.manifestHash.c_str());

	projectManifests.clear();
	vector<string> projectIds = ScanProjectIds();
	for (auto &pid : projectIds)
	{
		SAssetManifest pm;
		pm.version = 1;

		// Add the project file itself
		string jsonPath = projectsFolder + "/" + pid + ".json";
		if (!fs::exists(jsonPath))
			jsonPath = projectsFolder + "/" + pid + ".hjson";

		if (fs::exists(jsonPath))
		{
			uint64_t fileSize = (uint64_t)fs::file_size(jsonPath);
			int64_t mtime = FileTimeToTicks(fs::last_write_time(jsonPath));
			string crc32hex = GetCachedOrComputeCrc32(jsonPath, fileSize, mtime);
			if (!crc32hex.empty())
			{
				SAssetManifestEntry e;
				e.path = pid + fs::path(jsonPath).extension().string();
				e.crc32 = crc32hex;
				e.size = fileSize;
				pm.files.push_back(e);
			}
		}

		// Add project-specific assets subfolder if it exists
		string projectAssetsDir = projectsFolder + "/assets";
		if (fs::exists(projectAssetsDir) && fs::is_directory(projectAssetsDir))
		{
			SAssetManifest subManifest = BuildManifestForFolder(projectAssetsDir);
			for (auto &f : subManifest.files)
			{
				SAssetManifestEntry e = f;
				e.path = "assets/" + f.path;
				pm.files.push_back(e);
			}
		}

		sort(pm.files.begin(), pm.files.end(),
			[](const SAssetManifestEntry &a, const SAssetManifestEntry &b) { return a.path < b.path; });
		pm.manifestHash = ComputeManifestHash(pm);

		projectManifests[pid] = pm;
		LOGD("CAssetServer: project '%s' manifest: %d files, hash=%s",
			 pid.c_str(), (int)pm.files.size(), pm.manifestHash.c_str());
	}

	// Save CRC32 cache for next restart
	SaveCrc32Cache(cachePath);

	mutexManifests->Unlock();
}

SAssetManifest CAssetServer::GetAssetsManifest()
{
	mutexManifests->Lock();
	SAssetManifest m = assetsManifest;
	mutexManifests->Unlock();
	return m;
}

SAssetManifest CAssetServer::GetProjectManifest(const std::string &projectId)
{
	mutexManifests->Lock();
	auto it = projectManifests.find(projectId);
	SAssetManifest m = (it != projectManifests.end()) ? it->second : SAssetManifest();
	mutexManifests->Unlock();
	return m;
}

nlohmann::json CAssetServer::ManifestToJson(const SAssetManifest &manifest)
{
	nlohmann::json j;
	j["version"] = manifest.version;
	j["manifestHash"] = manifest.manifestHash;
	j["files"] = nlohmann::json::array();
	for (auto &f : manifest.files)
	{
		nlohmann::json fj;
		fj["path"] = f.path;
		fj["crc32"] = f.crc32;
		fj["size"] = f.size;
		j["files"].push_back(fj);
	}
	return j;
}

// ── Token validation ───────────────────────────────────────────────────

std::string CAssetServer::ExtractBearerToken(const std::string &authHeader)
{
	const string prefix = "Bearer ";
	if (authHeader.size() > prefix.size() &&
		authHeader.compare(0, prefix.size(), prefix) == 0)
	{
		return authHeader.substr(prefix.size());
	}
	return "";
}

bool CAssetServer::ValidateToken(const std::string &token, int &outProfileId, std::string &outPlayerName)
{
	if (token.empty())
		return false;

	// Check cache first
	mutexTokenCache->Lock();
	auto it = tokenCache.find(token);
	if (it != tokenCache.end())
	{
		if (time(nullptr) < it->second.validUntil)
		{
			outProfileId = it->second.profileId;
			outPlayerName = it->second.playerName;
			mutexTokenCache->Unlock();
			return true;
		}
		else
		{
			tokenCache.erase(it);
		}
	}
	mutexTokenCache->Unlock();

	// Validate via auth service HTTP call.
	//
	// Scheme-aware: the two-argument httplib::Client(host, port) constructor is
	// ALWAYS plaintext, and this hop carries the client's bearer session token.
	// A production auth service is started with --ssl, so a plaintext POST here
	// never returns 200 and every asset request 401s (final-review C-1); worse,
	// against a plaintext auth service it would put the token on the wire in
	// clear. Build the URL from authServiceUseTls instead.
	std::string authBaseUrl = (authServiceUseTls ? "https://" : "http://")
		+ authServiceAddress + ":" + std::to_string(SYS_ApplyPortOffset(authServicePort));
	httplib::Client authClient(authBaseUrl);
	if (!authClient.is_valid())
	{
		// e.g. https requested in a build without mbedTLS, or an unparseable host.
		LOGError("CAssetServer: auth client init failed for %s", authBaseUrl.c_str());
		return false;
	}
	// Certificate verification stays ON. authServiceCaCertPath only ADDS a trust
	// anchor (private CA / self-signed fixture); with it empty the system store
	// is used. There is deliberately no "skip verification" switch.
	//
	// set_ca_cert_path() only EXISTS under CPPHTTPLIB_SSL_ENABLED -- cpp-httplib
	// declares it inside that guard -- so the call has to be compiled out in a
	// build without an SSL backend, or every such build fails here. MT_CAP_HTTPS=0
	// with MT_CAP_HTTP=1 is a real configuration (RetroDebugger is one): this file
	// rides on MT_ENABLE_HTTPLIB, which does not imply TLS. Guarded on the
	// httplib macro rather than MT_ENABLE_MBEDTLS because that is precisely the
	// condition under which the member is declared.
	//
	// Nothing is lost when it is off: an https:// URL cannot be served by a
	// client built without SSL, so is_valid() above has already returned.
#ifdef CPPHTTPLIB_SSL_ENABLED
	if (authServiceUseTls && !authServiceCaCertPath.empty())
		authClient.set_ca_cert_path(authServiceCaCertPath.c_str());
#endif
	authClient.set_connection_timeout(5, 0);
	authClient.set_read_timeout(5, 0);

	nlohmann::json reqBody;
	reqBody["token"] = token;
	auto res = authClient.Post("/api/v1/validate", reqBody.dump(), "application/json");
	if (!res || res->status != 200)
		return false;

	try
	{
		auto respJson = nlohmann::json::parse(res->body);
		if (!respJson.value("ok", false))
			return false;

		outProfileId = respJson.value("profileId", 0);
		outPlayerName = respJson.value("playerName", "");

		// Cache the validated token
		mutexTokenCache->Lock();
		CachedTokenInfo info;
		info.profileId = outProfileId;
		info.playerName = outPlayerName;
		info.validUntil = time(nullptr) + tokenCacheTTLSeconds;
		tokenCache[token] = info;
		mutexTokenCache->Unlock();

		return true;
	}
	catch (...)
	{
		return false;
	}
}

// ── HTTP Server ────────────────────────────────────────────────────────

bool CAssetServer::Start()
{
	if (isRunning)
		return true;

	// A TLS asset server must not hand a client's bearer token to a REMOTE auth
	// service in clear. Loopback is exempt (the token never leaves the machine);
	// anything else with useSSL on and authServiceUseTls off is a misconfiguration
	// that would silently downgrade the credential hop, so refuse to start
	// (final-review C-1).
	if (useSSL && !authServiceUseTls && !NetGameIsLoopbackInterface(authServiceAddress))
	{
		LOGError("CAssetServer: refusing to start -- TLS listener but plaintext auth hop to %s (bearer tokens would go out in clear); pass --auth-tls", authServiceAddress.c_str());
		return false;
	}

	RebuildManifests();

	httplib::Server *server = NULL;
	if (useSSL)
	{
#if MT_ENABLE_MBEDTLS
		httplib::SSLServer *ssl = new httplib::SSLServer(sslCertPath.c_str(), sslKeyPath.c_str());
		if (!ssl->is_valid())
		{
			LOGError("CAssetServer: TLS init failed (cert=%s key=%s)", sslCertPath.c_str(), sslKeyPath.c_str());
			delete ssl;
			return false;
		}
		server = ssl;
#else
		LOGError("CAssetServer: built without mbedTLS, --ssl unavailable");
		return false;
#endif
	}
	else
	{
		if (!NetGameIsLoopbackInterface(listenInterface) && !allowPlaintext)
		{
			LOGError("CAssetServer: refusing plaintext HTTP on %s (use --ssl, or --allow-plaintext for a trusted network)", listenInterface.c_str());
			return false;
		}
		if (!NetGameIsLoopbackInterface(listenInterface))
			LOGWarning("CAssetServer: PLAINTEXT on %s — assets are exposed on the wire", listenInterface.c_str());
		server = new httplib::Server();
	}
	httpServer = server;

	// ── Health check (no auth) ──
	server->Get("/health", [](const httplib::Request &, httplib::Response &res) {
		res.set_content("{\"status\":\"ok\"}", "application/json");
	});

	// ── Auth middleware lambda ──
	auto requireAuth = [this](const httplib::Request &req, int &outProfileId, std::string &outPlayerName) -> bool {
		string authHeader = req.get_header_value("Authorization");
		string token = ExtractBearerToken(authHeader);
		return ValidateToken(token, outProfileId, outPlayerName);
	};

	// ── Global assets endpoints ──

	server->Get("/api/v1/assets/manifest-hash", [this, requireAuth](const httplib::Request &req, httplib::Response &res) {
		int pid; string pname;
		if (!requireAuth(req, pid, pname)) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

		mutexManifests->Lock();
		string hash = assetsManifest.manifestHash;
		mutexManifests->Unlock();

		nlohmann::json j;
		j["manifestHash"] = hash;
		res.set_content(j.dump(), "application/json");
	});

	server->Get("/api/v1/assets/manifest", [this, requireAuth](const httplib::Request &req, httplib::Response &res) {
		int pid; string pname;
		if (!requireAuth(req, pid, pname)) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

		mutexManifests->Lock();
		nlohmann::json j = ManifestToJson(assetsManifest);
		mutexManifests->Unlock();

		res.set_content(j.dump(), "application/json");
	});

	server->Get("/api/v1/assets/file", [this, requireAuth](const httplib::Request &req, httplib::Response &res) {
		int pid; string pname;
		if (!requireAuth(req, pid, pname)) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

		string filePath = req.get_param_value("path");
		if (filePath.empty() || filePath.find("..") != string::npos || filePath[0] == '/')
		{
			res.status = 400;
			res.set_content("{\"error\":\"invalid path\"}", "application/json");
			return;
		}

		string fullPath = assetsFolder + "/" + filePath;
		if (!fs::exists(fullPath) || !fs::is_regular_file(fullPath))
		{
			res.status = 404;
			res.set_content("{\"error\":\"not found\"}", "application/json");
			return;
		}

		uint64_t fileSize = (uint64_t)fs::file_size(fullPath);

		// Check for Range header
		if (req.has_header("Range"))
		{
			string rangeHeader = req.get_header_value("Range");
			// Parse "bytes=N-"
			uint64_t rangeStart = 0;
			if (rangeHeader.find("bytes=") == 0)
			{
				string rangeStr = rangeHeader.substr(6);
				auto dashPos = rangeStr.find('-');
				if (dashPos != string::npos)
				{
					string startStr = rangeStr.substr(0, dashPos);
					try { rangeStart = (uint64_t)stoull(startStr); } catch (...) { rangeStart = 0; }
				}
			}

			if (rangeStart >= fileSize)
			{
				res.status = 416; // Range Not Satisfiable
				return;
			}

			uint64_t contentLength = fileSize - rangeStart;

			ifstream f(fullPath, ios::binary);
			if (!f.is_open()) { res.status = 500; return; }
			f.seekg((streamoff)rangeStart);
			string content(contentLength, '\0');
			f.read(&content[0], (streamsize)contentLength);

			res.status = 206;
			res.set_header("Content-Range", "bytes " + to_string(rangeStart) + "-" + to_string(fileSize - 1) + "/" + to_string(fileSize));
			res.set_header("Content-Length", to_string(contentLength));
			res.set_content(content, "application/octet-stream");
		}
		else
		{
			ifstream f(fullPath, ios::binary);
			if (!f.is_open()) { res.status = 500; return; }
			string content((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());

			res.set_header("Content-Length", to_string(fileSize));
			res.set_content(content, "application/octet-stream");
		}
	});

	// ── Project endpoints ──

	server->Get(R"(/api/v1/projects/([^/]+)/manifest-hash)", [this, requireAuth](const httplib::Request &req, httplib::Response &res) {
		int pid; string pname;
		if (!requireAuth(req, pid, pname)) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

		string projectId = req.matches[1];

		mutexManifests->Lock();
		auto it = projectManifests.find(projectId);
		if (it == projectManifests.end())
		{
			mutexManifests->Unlock();
			res.status = 404;
			res.set_content("{\"error\":\"project not found\"}", "application/json");
			return;
		}
		string hash = it->second.manifestHash;
		mutexManifests->Unlock();

		nlohmann::json j;
		j["manifestHash"] = hash;
		res.set_content(j.dump(), "application/json");
	});

	server->Get(R"(/api/v1/projects/([^/]+)/manifest)", [this, requireAuth](const httplib::Request &req, httplib::Response &res) {
		int pid; string pname;
		if (!requireAuth(req, pid, pname)) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

		string projectId = req.matches[1];

		mutexManifests->Lock();
		auto it = projectManifests.find(projectId);
		if (it == projectManifests.end())
		{
			mutexManifests->Unlock();
			res.status = 404;
			res.set_content("{\"error\":\"project not found\"}", "application/json");
			return;
		}
		nlohmann::json j = ManifestToJson(it->second);
		mutexManifests->Unlock();

		res.set_content(j.dump(), "application/json");
	});

	server->Get(R"(/api/v1/projects/([^/]+)/file)", [this, requireAuth](const httplib::Request &req, httplib::Response &res) {
		int pid; string pname;
		if (!requireAuth(req, pid, pname)) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

		string projectId = req.matches[1];
		string filePath = req.get_param_value("path");
		if (filePath.empty() || filePath.find("..") != string::npos || filePath[0] == '/')
		{
			res.status = 400;
			res.set_content("{\"error\":\"invalid path\"}", "application/json");
			return;
		}

		// Project files are served from projectsFolder
		// The filePath is relative: could be "project08.json" or "assets/NPC/goblin.png"
		string fullPath = projectsFolder + "/" + filePath;
		if (!fs::exists(fullPath) || !fs::is_regular_file(fullPath))
		{
			res.status = 404;
			res.set_content("{\"error\":\"not found\"}", "application/json");
			return;
		}

		uint64_t fileSize = (uint64_t)fs::file_size(fullPath);

		if (req.has_header("Range"))
		{
			string rangeHeader = req.get_header_value("Range");
			uint64_t rangeStart = 0;
			if (rangeHeader.find("bytes=") == 0)
			{
				string rangeStr = rangeHeader.substr(6);
				auto dashPos = rangeStr.find('-');
				if (dashPos != string::npos)
				{
					string startStr = rangeStr.substr(0, dashPos);
					try { rangeStart = (uint64_t)stoull(startStr); } catch (...) { rangeStart = 0; }
				}
			}

			if (rangeStart >= fileSize) { res.status = 416; return; }

			uint64_t contentLength = fileSize - rangeStart;
			ifstream f(fullPath, ios::binary);
			if (!f.is_open()) { res.status = 500; return; }
			f.seekg((streamoff)rangeStart);
			string content(contentLength, '\0');
			f.read(&content[0], (streamsize)contentLength);

			res.status = 206;
			res.set_header("Content-Range", "bytes " + to_string(rangeStart) + "-" + to_string(fileSize - 1) + "/" + to_string(fileSize));
			res.set_header("Content-Length", to_string(contentLength));
			res.set_content(content, "application/octet-stream");
		}
		else
		{
			ifstream f(fullPath, ios::binary);
			if (!f.is_open()) { res.status = 500; return; }
			string content((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
			res.set_header("Content-Length", to_string(fileSize));
			res.set_content(content, "application/octet-stream");
		}
	});

	// Bind SYNCHRONOUSLY, before returning true: listen() inside the thread would
	// report a failed bind (port in use, interface unavailable) only as a missing
	// log line, while Start() had already returned true and the caller logged
	// "started" (final-review Minor 2). bind_to_port() gives us the answer here;
	// listen_after_bind() then runs the accept loop in the thread.
	int actualPort = SYS_ApplyPortOffset(port);
	if (!server->bind_to_port(listenInterface, actualPort))
	{
		LOGError("CAssetServer: bind failed on %s:%d (base %d)", listenInterface.c_str(), actualPort, port);
		delete server;
		httpServer = nullptr;
		return false;
	}

	// Start server in background thread. The thread handle is KEPT (a member,
	// not detached) so Shutdown() can join it before deleting the server object
	// its route lambdas are still inside.
	isRunning = true;
	serverThread = thread([server, this, actualPort]() {
		LOGM("CAssetServer: %s server listening on %s:%d (base %d)",
			 useSSL ? "https" : "http", listenInterface.c_str(), actualPort, port);
		server->listen_after_bind();
		LOGM("CAssetServer: stopped");
	});

	// Wait until the accept loop is actually running. httplib::Server::stop()
	// is a no-op while is_running_ is still false, so a Shutdown() that raced
	// the thread's entry into listen_after_bind() would leave it looping
	// forever and join() would hang.
	server->wait_until_ready();

	LOGM("CAssetServer: started (%s://%s)", useSSL ? "https" : "http", listenInterface.c_str());
	return true;
}

void CAssetServer::Shutdown()
{
	if (!isRunning)
		return;

	LOGM("CAssetServer: shutting down");

	httplib::Server *server = static_cast<httplib::Server *>(httpServer);
	if (server)
	{
		server->stop();
		// JOIN, do not sleep-and-hope: every route lambda captures `this` and
		// runs on that thread, so deleting the server (or this object) while
		// listen_after_bind() is still unwinding is a use-after-free
		// (final-review Minor 1).
		if (serverThread.joinable())
			serverThread.join();
		delete server;
		httpServer = nullptr;
	}

	isRunning = false;
	LOGM("CAssetServer: shutdown complete");
}

#endif // MT_ENABLE_HTTPLIB
