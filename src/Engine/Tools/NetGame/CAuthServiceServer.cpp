// CAPABILITY GUARD (MT_ENABLE_HTTPLIB). The file stays in all three build lists and
// compiles to nothing when the capability is off -- an Xcode PBXBuildFile
// cannot be conditioned on a build setting, so the condition lives here.
// MT_CAP_HTTP declared this removable and nothing enforced it: MT_ENABLE_HTTPLIB appeared in no source at all.
#if MT_ENABLE_HTTPLIB

#include "CAuthServiceServer.h"
#include "CNetGameDataProvider.h"
#include "CNetGameUserProfile.h"
#include "CNetGameDataProviderLocalFiles.h"
#include "NetGameListenPolicy.h"
#include "DBG_Log.h"
#include "SYS_Main.h"
#include "SYS_Crypto.h"
#include "CSlrString.h"
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
, lastSessionCleanup(0)
, lastRateLimitCleanup(0)
, dummyAuthProfile(NULL)
{
	mutexSessions = new CSlrMutex("CAuthServiceSessions");
	mutexRateLimit = new CSlrMutex("CAuthServiceRateLimit");
}

CAuthServiceServer::~CAuthServiceServer()
{
	Shutdown();
	delete mutexSessions;
	delete mutexRateLimit;
	delete dummyAuthProfile;
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
	// Periodic cleanup of expired sessions, throttled the same way the rate-limit
	// map is: at most once per RATE_LIMIT_CLEANUP_INTERVAL_SECONDS, checked here
	// since /validate is the highest-traffic endpoint that touches activeSessions.
	mutexSessions->Lock();
	time_t now = time(NULL);
	bool needsCleanup = (now - lastSessionCleanup >= RATE_LIMIT_CLEANUP_INTERVAL_SECONDS);
	if (needsCleanup)
		lastSessionCleanup = now;
	mutexSessions->Unlock();

	if (needsCleanup)
		CleanupExpiredSessions();

	mutexSessions->Lock();
	auto it = activeSessions.find(token);
	if (it == activeSessions.end())
	{
		mutexSessions->Unlock();
		return false;
	}

	if (now > it->second.expiresAt)
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

void CAuthServiceServer::LogoutSession(const string &token)
{
	mutexSessions->Lock();
	activeSessions.erase(token);
	mutexSessions->Unlock();
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
	// Same pass also drops expired-but-never-redeemed lobby tickets (a
	// redeemed one is already gone -- RedeemLobbyTicket erases on success).
	for (auto it = lobbyTickets.begin(); it != lobbyTickets.end(); )
	{
		if (now > it->second.expiresAt)
		{
			it = lobbyTickets.erase(it);
		}
		else
		{
			++it;
		}
	}
	mutexSessions->Unlock();
}

string CAuthServiceServer::IssueLobbyTicket(int profileId, const string &playerName)
{
	string ticket = GenerateSessionToken();
	time_t expiresAt = time(NULL) + LOBBY_TICKET_TTL_SECONDS;

	mutexSessions->Lock();
	lobbyTickets[ticket] = {profileId, playerName, expiresAt};
	mutexSessions->Unlock();

	return ticket;
}

bool CAuthServiceServer::RedeemLobbyTicket(const string &ticket, int &outProfileId, string &outPlayerName)
{
	mutexSessions->Lock();
	auto it = lobbyTickets.find(ticket);
	if (it == lobbyTickets.end())
	{
		mutexSessions->Unlock();
		return false;
	}

	if (time(NULL) > it->second.expiresAt)
	{
		lobbyTickets.erase(it);
		mutexSessions->Unlock();
		return false;
	}

	outProfileId = it->second.profileId;
	outPlayerName = it->second.playerName;
	// Single-use: erase on successful redeem, before releasing the lock, so a
	// concurrent redeem of the same ticket can never observe it twice.
	lobbyTickets.erase(it);
	mutexSessions->Unlock();
	return true;
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

bool CAuthServiceServer::CheckValidateRateLimit(const string &clientIP)
{
	mutexRateLimit->Lock();

	// Periodic cleanup of stale entries (shared with CheckRateLimit's timer).
	time_t now = time(NULL);
	if (now - lastRateLimitCleanup >= RATE_LIMIT_CLEANUP_INTERVAL_SECONDS)
	{
		CleanupStaleRateLimitEntries();
		lastRateLimitCleanup = now;
	}

	auto it = validateRateLimitByIP.find(clientIP);
	if (it != validateRateLimitByIP.end())
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
			if (it->second.attempts > VALIDATE_RATE_LIMIT_MAX)
			{
				mutexRateLimit->Unlock();
				return false;
			}
		}
	}
	else
	{
		validateRateLimitByIP[clientIP] = {1, now};
	}

	mutexRateLimit->Unlock();
	return true;
}

bool CAuthServiceServer::CheckTicketRateLimit(const string &clientIP)
{
	mutexRateLimit->Lock();

	// Periodic cleanup of stale entries (shared with CheckRateLimit's timer).
	time_t now = time(NULL);
	if (now - lastRateLimitCleanup >= RATE_LIMIT_CLEANUP_INTERVAL_SECONDS)
	{
		CleanupStaleRateLimitEntries();
		lastRateLimitCleanup = now;
	}

	auto it = ticketRateLimitByIP.find(clientIP);
	if (it != ticketRateLimitByIP.end())
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
			if (it->second.attempts > TICKET_RATE_LIMIT_MAX)
			{
				mutexRateLimit->Unlock();
				return false;
			}
		}
	}
	else
	{
		ticketRateLimitByIP[clientIP] = {1, now};
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

	for (auto it = validateRateLimitByIP.begin(); it != validateRateLimitByIP.end(); )
	{
		if (now - it->second.windowStart >= rateLimitWindowSeconds * 2)
		{
			it = validateRateLimitByIP.erase(it);
		}
		else
		{
			++it;
		}
	}

	for (auto it = ticketRateLimitByIP.begin(); it != ticketRateLimitByIP.end(); )
	{
		if (now - it->second.windowStart >= rateLimitWindowSeconds * 2)
		{
			it = ticketRateLimitByIP.erase(it);
		}
		else
		{
			++it;
		}
	}

	// Stale lockout entries: not currently locked, and no failure recorded
	// within the last rate-limit window (mirrors the per-IP staleness rule above).
	for (auto it = lockoutByUser.begin(); it != lockoutByUser.end(); )
	{
		bool locked = (it->second.lockedUntil != 0 && now < it->second.lockedUntil);
		if (!locked && now - it->second.lastFailureAt >= rateLimitWindowSeconds * 2)
		{
			it = lockoutByUser.erase(it);
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

bool CAuthServiceServer::IsLoopbackInterface(const std::string &iface)
{
	return NetGameIsLoopbackInterface(iface);
}

bool CAuthServiceServer::Start()
{
	if (isRunning)
		return true;

	int actualPort = SYS_ApplyPortOffset(port);
	LOGM("CAuthServiceServer: starting on port %d (base %d)", actualPort, port);

	// Assign profile IDs to existing profiles before accepting requests
	AssignProfileIdsToExistingProfiles();

	// Equal-work login: build the dummy verifier once, from a random secret
	// that is never written anywhere and never matches a real login attempt.
	// See the member comment in the header for why this exists.
	if (!dummyAuthProfile)
	{
		dummyAuthProfile = new CNetGameUserProfile();
		vector<u8> dummySecret(32);
		if (!SYS_SecureRandomBytes(dummySecret.data(), dummySecret.size()))
		{
			LOGError("CAuthServiceServer: failed to seed the dummy auth verifier (RNG failure)");
			delete dummyAuthProfile;
			dummyAuthProfile = NULL;
			return false;
		}
		dummyAuthProfile->SetPassword(dummySecret);
	}

	if (internalSecret.empty())
		LOGWarning("CAuthServiceServer: redeem-ticket is unauthenticated: set --internal-secret");

	httplib::Server *server = NULL;
	if (useSSL)
	{
#if MT_ENABLE_MBEDTLS
		httplib::SSLServer *ssl = new httplib::SSLServer(sslCertPath.c_str(), sslKeyPath.c_str());
		if (!ssl->is_valid())
		{
			LOGError("CAuthServiceServer: TLS init failed (cert=%s key=%s)", sslCertPath.c_str(), sslKeyPath.c_str());
			delete ssl;
			return false;
		}
		server = ssl;
#else
		LOGError("CAuthServiceServer: built without mbedTLS, --ssl unavailable");
		return false;
#endif
	}
	else
	{
		if (!IsLoopbackInterface(listenInterface) && !allowPlaintext)
		{
			LOGError("CAuthServiceServer: refusing plaintext HTTP on %s (use --ssl, or --allow-plaintext for a trusted network)", listenInterface.c_str());
			return false;
		}
		if (!IsLoopbackInterface(listenInterface))
			LOGWarning("CAuthServiceServer: PLAINTEXT on %s — credentials are exposed on the wire", listenInterface.c_str());
		server = new httplib::Server();
	}
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

		// Uniform registration error: an invalid username, a too-short password, and a
		// taken username all produce the SAME 400 {ok:false, error:"registration_rejected"}
		// response -- distinct error codes here would let an attacker distinguish "bad
		// input" from "that account exists" (a username-enumeration oracle). All three
		// checks run unconditionally (no &&/|| short-circuit) so malformed input always
		// pays for the PlayerExists disk check too, the same as well-formed input --
		// otherwise "did my request reach the disk check" becomes a (minor) signal.
		//
		// Malformed UTF-8 in the username throws out of Utf8StringToLowercase
		// (utf8::next), which PlayerExists() uses to resolve the account -- that
		// escaped the handler as a free 500 + log noise (final-review Minor 5).
		// Catch it here and fold it into the same uniform rejection.
		bool validUsername = IsValidUsername(username);
		bool validPassword = (password.length() >= 4);
		bool alreadyExists = false;
		bool malformedUsername = false;
		try {
			alreadyExists = dataProvider->PlayerExists(username);
		} catch (...) {
			malformedUsername = true;
		}
		bool rejected = !validUsername || !validPassword || alreadyExists || malformedUsername;
		if (rejected)
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "registration_rejected";
			res.status = 400;
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

		// Lockout key is lowercased to match account resolution: PlayerExists /
		// LoadPlayerProfile / SavePlayerProfile (CNetGameDataProviderLocalFiles) all
		// resolve the account case-insensitively via Utf8StringToLowercase(). Keying
		// the lockout map on the raw, case-sensitive username would let an attacker
		// spread failed attempts across case variants ("Alice", "ALICE", "aLiCe", all
		// the same account) and never trip the threshold.
		// Utf8StringToLowercase (utf8::next) THROWS on malformed UTF-8 -- httplib
		// turns that into a free 500 plus log noise from any unauthenticated
		// caller (final-review Minor 5). Malformed input can never name a real
		// account, so treat it exactly like wrong credentials: the same
		// byte-identical 401 every other failing login gets.
		string lockoutKey;
		try {
			lockoutKey = Utf8StringToLowercase(username);
		} catch (...) {
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_credentials";
			res.status = 401;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Account lockout check first: while locked, refuse even the RIGHT password,
		// with the SAME response body as "wrong password" / "no such user" -- distinct
		// bodies here would tell an attacker whether the account exists/is locked.
		mutexRateLimit->Lock();
		time_t lockCheckNow = time(NULL);
		auto lockIt = lockoutByUser.find(lockoutKey);
		bool isLocked = (lockIt != lockoutByUser.end() && lockIt->second.lockedUntil != 0
						  && lockCheckNow < lockIt->second.lockedUntil);
		mutexRateLimit->Unlock();

		vector<u8> hash = dataProvider->PasswordToHash(password);

		if (isLocked)
		{
			// Equal-work: a locked account must cost as much as a real wrong-password
			// check (which pays for PBKDF2 inside PlayerAuthorize/CheckPassword below),
			// or the lock's existence leaks through response latency even though the
			// body is byte-identical to "wrong password". Run the same PBKDF2 derivation
			// against the static dummy verifier and discard the result.
			dummyAuthProfile->CheckPassword(hash);

			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_credentials";
			res.status = 401;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Authorize. An unknown username fails inside PlayerAuthorize at LoadPlayerProfile
		// (file-not-found) and never reaches PBKDF2, while a known username with the wrong
		// password does pay for PBKDF2 (inside CheckPassword). Checking existence first and
		// running the same dummy PBKDF2 derivation on the "unknown" branch equalizes the
		// three outcomes (locked / unknown user / known user + wrong password) to one PBKDF2
		// derivation each -- otherwise timing alone would reveal whether an account exists.
		bool userExists = false;
		CNetGameUserProfile *profile = NULL;
		try {
			userExists = dataProvider->PlayerExists(username);
			profile = userExists ? dataProvider->PlayerAuthorize(username, hash) : NULL;
		} catch (...) {
			// Same reasoning as the lockout-key catch above: anything that throws
			// out of account resolution is not a valid account, and must not
			// become a 500. Fall through to the uniform 401 below.
			userExists = false;
			profile = NULL;
		}
		if (!userExists)
			dummyAuthProfile->CheckPassword(hash);

		if (!profile)
		{
			// Wrong password or unknown user (indistinguishable on purpose): count a
			// failure toward this account's lockout.
			mutexRateLimit->Lock();
			time_t now = time(NULL);
			LockoutEntry &entry = lockoutByUser[lockoutKey];
			// An EXPIRED lock starts a fresh count. Without this the failure
			// counter stays at/above ACCOUNT_LOCKOUT_FAILURES forever, so one
			// wrong password every ~900s re-locked a targeted account
			// indefinitely at 1/10th the usual cost (final-review Minor 4).
			if (entry.lockedUntil != 0 && now >= entry.lockedUntil)
			{
				entry.failures = 0;
				entry.lockedUntil = 0;
			}
			entry.failures++;
			entry.lastFailureAt = now;
			if (entry.failures >= ACCOUNT_LOCKOUT_FAILURES)
				entry.lockedUntil = now + ACCOUNT_LOCKOUT_SECONDS;
			mutexRateLimit->Unlock();

			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_credentials";
			res.status = 401;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		// Successful login resets the lockout entry.
		mutexRateLimit->Lock();
		lockoutByUser.erase(lockoutKey);
		mutexRateLimit->Unlock();

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

	// POST /api/v1/validate — rate-limited per IP, separately from register/login
	// (VALIDATE_RATE_LIMIT_MAX per rateLimitWindowSeconds): it's polled far more
	// often, e.g. by the lobby server on every connect.
	server->Post("/api/v1/validate", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Content-Type", "application/json");

		if (!CheckValidateRateLimit(req.remote_addr))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "rate_limited";
			mutexRateLimit->Lock();
			auto it = validateRateLimitByIP.find(req.remote_addr);
			int retryAfter = rateLimitWindowSeconds;
			if (it != validateRateLimitByIP.end())
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

	// POST /api/v1/lobby-ticket — mints a single-use, short-lived (LOBBY_TICKET_TTL_SECONDS)
	// ticket for a valid session token. The ticket, not the session token, is what
	// crosses ENet to the lobby server; the lobby redeems it via /api/v1/redeem-ticket.
	// Rate-limited on its OWN per-IP bucket (TICKET_RATE_LIMIT_MAX per window),
	// not register/login's: since C-short this is fetched once per CONNECTION
	// ATTEMPT (every RECONNECT, ~1 Hz), so sharing login's 10/minute ceiling let a
	// flapping client burn the IP's login budget in ~10 seconds -- and behind a NAT,
	// every other player's too (final-review I-3).
	server->Post("/api/v1/lobby-ticket", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Content-Type", "application/json");

		if (!CheckTicketRateLimit(req.remote_addr))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "rate_limited";
			mutexRateLimit->Lock();
			auto it = ticketRateLimitByIP.find(req.remote_addr);
			int retryAfter = rateLimitWindowSeconds;
			if (it != ticketRateLimitByIP.end())
				retryAfter = rateLimitWindowSeconds - (int)(time(NULL) - it->second.windowStart);
			mutexRateLimit->Unlock();
			resp["retryAfterSeconds"] = retryAfter > 0 ? retryAfter : 1;
			res.status = 429;
			res.set_content(resp.dump(), "application/json");
			return;
		}

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

		int profileId;
		string playerName;
		if (!ValidateSessionToken(token, profileId, playerName))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_token";
			res.status = 401;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		string ticket = IssueLobbyTicket(profileId, playerName);

		json resp;
		resp["ok"] = true;
		resp["ticket"] = ticket;
		res.status = 200;
		res.set_content(resp.dump(), "application/json");
	});

	// POST /api/v1/redeem-ticket — internal call (lobby server -> auth service):
	// consumes a single-use lobby ticket. Authenticated by a shared secret
	// (internalSecret), constant-time compared, rather than by rate limit -- this
	// is a service-to-service call, not a client-facing one.
	server->Post("/api/v1/redeem-ticket", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Content-Type", "application/json");

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

		string ticket = reqBody.value("ticket", "");
		string providedSecret = reqBody.value("secret", "");

		if (!internalSecret.empty())
		{
			vector<u8> expected(internalSecret.begin(), internalSecret.end());
			vector<u8> provided(providedSecret.begin(), providedSecret.end());
			if (!SYS_ConstantTimeEquals(expected, provided))
			{
				json resp;
				resp["ok"] = false;
				resp["error"] = "forbidden";
				res.status = 403;
				res.set_content(resp.dump(), "application/json");
				return;
			}
		}

		int profileId;
		string playerName;
		if (!RedeemLobbyTicket(ticket, profileId, playerName))
		{
			json resp;
			resp["ok"] = false;
			resp["error"] = "invalid_ticket";
			res.status = 401;
			res.set_content(resp.dump(), "application/json");
			return;
		}

		json resp;
		resp["ok"] = true;
		resp["profileId"] = profileId;
		resp["playerName"] = playerName;
		res.status = 200;
		res.set_content(resp.dump(), "application/json");
	});

	// POST /api/v1/logout — idempotent: always 200 {ok:true}, whether or not the
	// token was present. Not rate-limited (same trust level as validate would be
	// under the higher ceiling, and a client logs out at most once per session).
	server->Post("/api/v1/logout", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Content-Type", "application/json");

		json reqBody;
		try {
			reqBody = json::parse(req.body);
		} catch (...) {
			reqBody = json::object();
		}

		string token = reqBody.value("token", "");
		if (!token.empty())
			LogoutSession(token);

		json resp;
		resp["ok"] = true;
		res.status = 200;
		res.set_content(resp.dump(), "application/json");
	});

	// Bind SYNCHRONOUSLY, before returning true: listen() inside the thread
	// reports a failed bind (port in use, interface unavailable) only as a
	// missing log line, while Start() has already returned true, Run() has
	// logged "auth service started" and systemd calls the unit healthy
	// (final-review Minor 2). bind_to_port() answers here; listen_after_bind()
	// runs the accept loop in the thread.
	if (!server->bind_to_port(listenInterface, actualPort))
	{
		LOGError("CAuthServiceServer: bind failed on %s:%d (base %d)", listenInterface.c_str(), actualPort, port);
		delete server;
		httpServer = NULL;
		return false;
	}

	// Start server in background thread. The handle is KEPT (a member, not
	// detached) so Shutdown() can join it before deleting the server object
	// whose route lambdas all capture `this`.
	isRunning = true;
	serverThread = thread([server, this, actualPort]() {
		LOGM("CAuthServiceServer: %s server listening on %s:%d (base %d)",
			 useSSL ? "https" : "http", listenInterface.c_str(), actualPort, port);
		server->listen_after_bind();
		LOGM("CAuthServiceServer: HTTP server stopped");
	});

	// Wait until the accept loop is actually running. httplib::Server::stop()
	// is a no-op while is_running_ is still false, so a Shutdown() that raced
	// the thread's entry into listen_after_bind() would leave it looping
	// forever and join() would hang.
	server->wait_until_ready();

	LOGM("CAuthServiceServer: started (%s://%s)", useSSL ? "https" : "http", listenInterface.c_str());
	return true;
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
		// JOIN, do not sleep-and-hope: every route lambda captures `this`, so
		// deleting the server while listen_after_bind() is still unwinding is
		// exactly the use-after-free shape B3b spent four rounds eliminating for
		// game servers (final-review Minor 1).
		if (serverThread.joinable())
			serverThread.join();
		delete server;
		httpServer = NULL;
	}

	isRunning = false;
	LOGM("CAuthServiceServer: shutdown complete");
}

#endif // MT_ENABLE_HTTPLIB
