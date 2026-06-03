#include "CAuthServiceServer.h"
#include "CNetGameDataProvider.h"
#include "CNetGameUserProfile.h"
#include "CNetGameDataProviderLocalFiles.h"
#include "DBG_Log.h"
#include "SYS_Main.h"
#include "json.hpp"

#include "httplib.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace nlohmann;
using namespace std;

CAuthServiceServer::CAuthServiceServer(CNetGameDataProvider *dataProvider, int port)
: dataProvider(dataProvider)
, port(port)
, tokenExpirySeconds(DEFAULT_TOKEN_EXPIRY_SECONDS)
, rateLimitMaxAttempts(DEFAULT_RATE_LIMIT_MAX_ATTEMPTS)
, rateLimitWindowSeconds(DEFAULT_RATE_LIMIT_WINDOW_SECONDS)
, isRunning(false)
, httpServer(NULL)
, nextProfileId(0)
, lastRateLimitCleanup(0)
{
	mutexSessions = new CSlrMutex("CAuthServiceSessions");
	mutexRateLimit = new CSlrMutex("CAuthServiceRateLimit");
}

CAuthServiceServer::~CAuthServiceServer()
{
	Shutdown();
	delete mutexSessions;
	delete mutexRateLimit;
}

bool CAuthServiceServer::IsValidUsername(const string &username)
{
	if (username.length() < USERNAME_MIN_LENGTH || username.length() > USERNAME_MAX_LENGTH)
		return false;

	for (char c : username)
	{
		if (!isalnum(c) && c != '_' && c != '-')
			return false;
	}
	return true;
}

string CAuthServiceServer::GenerateSessionToken()
{
	std::array<u8, 32> rnd;
	if (!SYS_SecureRandomBytes(rnd.data(), rnd.size()))
	{
		SYS_FatalExit("CAuthServiceServer::GenerateSessionToken: secure RNG failed");
	}

	static const char hex[] = "0123456789abcdef";
	string token;
	token.resize(rnd.size() * 2);
	for (size_t i = 0; i < rnd.size(); i++)
	{
		token[i * 2 + 0] = hex[(rnd[i] >> 4) & 0x0F];
		token[i * 2 + 1] = hex[rnd[i] & 0x0F];
	}
	return token;
}

bool CAuthServiceServer::ValidateSessionToken(const string &token, int &outProfileId, string &outPlayerName)
{
	mutexSessions->Lock();
	auto it = activeSessions.find(token);
	if (it == activeSessions.end())
	{
		mutexSessions->Unlock();
		return false;
	}

	if (time(NULL) > it->second.expiresAt)
	{
		activeSessions.erase(it);
		mutexSessions->Unlock();
		return false;
	}

	outProfileId = it->second.profileId;
	outPlayerName = it->second.playerName;
	mutexSessions->Unlock();
	return true;
}

void CAuthServiceServer::CleanupExpiredSessions()
{
	mutexSessions->Lock();
	time_t now = time(NULL);
	for (auto it = activeSessions.begin(); it != activeSessions.end(); )
	{
		if (now > it->second.expiresAt)
		{
			it = activeSessions.erase(it);
		}
		else
		{
			++it;
		}
	}
	mutexSessions->Unlock();
}

bool CAuthServiceServer::CheckRateLimit(const string &clientIP)
{
	mutexRateLimit->Lock();

	// Periodic cleanup of stale entries
	time_t now = time(NULL);
	if (now - lastRateLimitCleanup >= RATE_LIMIT_CLEANUP_INTERVAL_SECONDS)
	{
		CleanupStaleRateLimitEntries();
		lastRateLimitCleanup = now;
	}

	auto it = rateLimitByIP.find(clientIP);
	if (it != rateLimitByIP.end())
	{
		if (now - it->second.windowStart >= rateLimitWindowSeconds)
		{
			// Window expired, reset
			it->second.attempts = 1;
			it->second.windowStart = now;
		}
		else
		{
			it->second.attempts++;
			if (it->second.attempts > rateLimitMaxAttempts)
			{
				int retryAfter = rateLimitWindowSeconds - (int)(now - it->second.windowStart);
				mutexRateLimit->Unlock();
				return false;
			}
		}
	}
	else
	{
		rateLimitByIP[clientIP] = {1, now};
	}

	mutexRateLimit->Unlock();
	return true;
}

void CAuthServiceServer::CleanupStaleRateLimitEntries()
{
	// Called under mutexRateLimit lock
	time_t now = time(NULL);
	for (auto it = rateLimitByIP.begin(); it != rateLimitByIP.end(); )
	{
		if (now - it->second.windowStart >= rateLimitWindowSeconds * 2)
		{
			it = rateLimitByIP.erase(it);
		}
		else
		{
			++it;
		}
	}
}

int CAuthServiceServer::GenerateNextProfileId()
{
	return nextProfileId.fetch_add(1);
}

void CAuthServiceServer::AssignProfileIdsToExistingProfiles()
{
	CNetGameDataProviderLocalFiles *localProvider =
		dynamic_cast<CNetGameDataProviderLocalFiles *>(dataProvider);
	if (!localProvider)
	{
		LOGM("CAuthServiceServer: dataProvider is not local files, skipping profile ID assignment");
		return;
	}

	namespace fs = std::filesystem;
	string playersFolder = localProvider->playersFolder;

	if (!fs::exists(playersFolder))
	{
		LOGM("CAuthServiceServer: players folder does not exist: %s", playersFolder.c_str());
		return;
	}

	// First pass: find max existing ID and count profiles needing assignment
	int maxExistingId = -1;
	int fixedCount = 0;

	for (const auto &entry : fs::directory_iterator(playersFolder))
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".json")
			continue;

		CNetGameUserProfile *profile = localProvider->LoadPlayerProfile(
			entry.path().stem().string());
		if (!profile)
			continue;

		if (profile->profileId > maxExistingId)
			maxExistingId = profile->profileId;

		delete profile;
	}

	// Set nextProfileId to max + 1
	nextProfileId.store(maxExistingId + 1);

	// Second pass: assign IDs to profiles with id == -1
	for (const auto &entry : fs::directory_iterator(playersFolder))
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".json")
			continue;

		CNetGameUserProfile *profile = localProvider->LoadPlayerProfile(
			entry.path().stem().string());
		if (!profile)
			continue;

		if (profile->profileId == -1)
		{
			profile->profileId = GenerateNextProfileId();
			localProvider->SavePlayerProfile(profile);
			LOGM("CAuthServiceServer: assigned profileId=%d to player=%s",
				 profile->profileId, profile->name.c_str());
			fixedCount++;
		}

		delete profile;
	}

	LOGM("CAuthServiceServer: profile ID scan complete. nextProfileId=%d, fixed=%d profiles",
		 nextProfileId.load(), fixedCount);
}

void CAuthServiceServer::Start()
{
	if (isRunning)
		return;

	int actualPort = SYS_ApplyPortOffset(port);
	LOGM("CAuthServiceServer: starting on port %d (base %d)", actualPort, port);

	// Assign profile IDs to existing profiles before accepting requests
	AssignProfileIdsToExistingProfiles();

	httplib::Server *server = new httplib::Server();
	httpServer = server;

	// Limit request body size to 1MB to prevent memory exhaustion from malicious Content-Length
	server->set_payload_max_length(1024 * 1024);

	// POST /api/v1/register
	server->Post("/api/v1/register", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Content-Type", "application/json");

		// Rate limit check
		if (!CheckRateLimit(req.remote_addr))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "rate_limited";
			mutexRateLimit->Lock();
			auto it = rateLimitByIP.find(req.remote_addr);
			int retryAfter = rateLimitWindowSeconds;
			if (it != rateLimitByIP.end())
				retryAfter = rateLimitWindowSeconds - (int)(time(NULL) - it->second.windowStart);
			mutexRateLimit->Unlock();
			resp["retryAfterSeconds"] = retryAfter > 0 ? retryAfter : 1;
			res.status = 429;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Parse request
		json reqBody;
		try {
			reqBody = json::parse(req.body);
		} catch (...) {
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_json";
			res.status = 400;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		string username = reqBody.value("username", "");
		string password = reqBody.value("password", "");

		// Validate username
		if (!IsValidUsername(username))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_username";
			res.status = 400;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Validate password (minimum 4 characters)
		if (password.length() < 4)
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_password";
			res.status = 400;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Check if username already exists (case-insensitive)
		if (dataProvider->PlayerExists(username))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "username_taken";
			res.status = 409;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Create profile with assigned ID
		CNetGameUserProfile *profile = dataProvider->CreateAndSavePlayerProfile(username, password);
		if (!profile)
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "creation_failed";
			res.status = 500;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Assign profile ID
		profile->profileId = GenerateNextProfileId();
		dataProvider->SavePlayerProfile(profile);

		// Create session token
		string token = GenerateSessionToken();
		time_t expiresAt = time(NULL) + tokenExpirySeconds;

		mutexSessions->Lock();
		activeSessions[token] = {profile->profileId, profile->name, expiresAt};
		mutexSessions->Unlock();

		LOGM("CAuthServiceServer: registered user=%s profileId=%d",
			 profile->name.c_str(), profile->profileId);

		json resp;
		resp["ok"] = true;
		resp["profileId"] = profile->profileId;
		resp["token"] = token;
		resp["expiresAt"] = (int64_t)expiresAt;
		res.status = 200;
		res.set_content(resp.dump(), "application/json");

		delete profile;
	});

	// POST /api/v1/login
	server->Post("/api/v1/login", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Content-Type", "application/json");

		// Rate limit check
		if (!CheckRateLimit(req.remote_addr))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "rate_limited";
			mutexRateLimit->Lock();
			auto it = rateLimitByIP.find(req.remote_addr);
			int retryAfter = rateLimitWindowSeconds;
			if (it != rateLimitByIP.end())
				retryAfter = rateLimitWindowSeconds - (int)(time(NULL) - it->second.windowStart);
			mutexRateLimit->Unlock();
			resp["retryAfterSeconds"] = retryAfter > 0 ? retryAfter : 1;
			res.status = 429;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Parse request
		json reqBody;
		try {
			reqBody = json::parse(req.body);
		} catch (...) {
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_json";
			res.status = 400;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		string username = reqBody.value("username", "");
		string password = reqBody.value("password", "");

		// Authorize
		vector<u8> hash = dataProvider->PasswordToHash(password);
		CNetGameUserProfile *profile = dataProvider->PlayerAuthorize(username, hash);
		if (!profile)
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_credentials";
			res.status = 401;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Create session token
		string token = GenerateSessionToken();
		time_t expiresAt = time(NULL) + tokenExpirySeconds;

		mutexSessions->Lock();
		activeSessions[token] = {profile->profileId, profile->name, expiresAt};
		mutexSessions->Unlock();

		LOGM("CAuthServiceServer: login user=%s profileId=%d",
			 profile->name.c_str(), profile->profileId);

		json resp;
		resp["ok"] = true;
		resp["profileId"] = profile->profileId;
		resp["token"] = token;
		resp["expiresAt"] = (int64_t)expiresAt;
		res.status = 200;
		res.set_content(resp.dump(), "application/json");

		delete profile;
	});

	// POST /api/v1/validate — NOT rate-limited (used by lobby server internally)
	server->Post("/api/v1/validate", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Content-Type", "application/json");

		// Parse request
		json reqBody;
		try {
			reqBody = json::parse(req.body);
		} catch (...) {
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_json";
			res.status = 400;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		string token = reqBody.value("token", "");
		if (token.empty())
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "missing_token";
			res.status = 400;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		int profileId;
		string playerName;
		if (ValidateSessionToken(token, profileId, playerName))
		{
			json resp;
			resp["ok"] = true;
			resp["profileId"] = profileId;
			resp["playerName"] = playerName;
			res.status = 200;
			res.set_content(resp.dump(), "application/json");
		}
		else
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_token";
			res.status = 401;
			res.set_content(resp.dump(), "application/json");
		}
	});

	// Start server in background thread
	isRunning = true;
	thread serverThread([server, this]() {
		int actualPort = SYS_ApplyPortOffset(port);
		LOGM("CAuthServiceServer: HTTP server listening on port %d (base %d)", actualPort, port);
		server->listen("0.0.0.0", actualPort);
		LOGM("CAuthServiceServer: HTTP server stopped");
	});
	serverThread.detach();

	LOGM("CAuthServiceServer: started");
}

void CAuthServiceServer::Shutdown()
{
	if (!isRunning)
		return;

	LOGM("CAuthServiceServer: shutting down");

	httplib::Server *server = static_cast<httplib::Server *>(httpServer);
	if (server)
	{
		server->stop();
		// Give the server thread a moment to finish
		SYS_Sleep(100);
		delete server;
		httpServer = NULL;
	}

	isRunning = false;
	LOGM("CAuthServiceServer: shutdown complete");
}
