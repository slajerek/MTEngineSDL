#pragma once

#include "SYS_Defs.h"
#include "SYS_Threading.h"
#include <string>
#include <map>
#include <ctime>
#include <atomic>

using namespace std;

class CNetGameDataProvider;

// HTTP REST Auth Service for user registration, login, and session validation.
// Runs as a standalone HTTP server (separate process or in-process for tests).
// Endpoints: POST /api/v1/register, /api/v1/login, /api/v1/validate
class CAuthServiceServer
{
public:
	static const int DEFAULT_PORT = 14550;
	static const int DEFAULT_TOKEN_EXPIRY_SECONDS = 86400; // 24 hours
	static const int DEFAULT_RATE_LIMIT_MAX_ATTEMPTS = 10;
	static const int DEFAULT_RATE_LIMIT_WINDOW_SECONDS = 60;
	static const int RATE_LIMIT_CLEANUP_INTERVAL_SECONDS = 300; // 5 minutes
	static const int USERNAME_MIN_LENGTH = 2;
	static const int USERNAME_MAX_LENGTH = 32;

	CAuthServiceServer(CNetGameDataProvider *dataProvider, int port = DEFAULT_PORT);
	virtual ~CAuthServiceServer();

	void Start();
	void Shutdown();

	CNetGameDataProvider *dataProvider;

	// Configuration
	int port;
	int tokenExpirySeconds;
	int rateLimitMaxAttempts;
	int rateLimitWindowSeconds;

	// Profile ID management — assigns IDs to new profiles and fixes existing ones
	int GenerateNextProfileId();
	void AssignProfileIdsToExistingProfiles();

	// Session token management
	struct SessionInfo
	{
		int profileId;
		string playerName;
		time_t expiresAt;
	};

	string GenerateSessionToken();
	bool ValidateSessionToken(const string &token, int &outProfileId, string &outPlayerName);
	void CleanupExpiredSessions();

	map<string, SessionInfo> activeSessions;
	CSlrMutex *mutexSessions;

	// Rate limiting
	struct RateLimitEntry
	{
		int attempts;
		time_t windowStart;
	};

	bool CheckRateLimit(const string &clientIP);
	void CleanupStaleRateLimitEntries();

	map<string, RateLimitEntry> rateLimitByIP;
	CSlrMutex *mutexRateLimit;
	time_t lastRateLimitCleanup;

	// Username validation
	bool IsValidUsername(const string &username);

	bool isRunning;

private:
	// HTTP server runs in a background thread
	void *httpServer; // httplib::Server* (opaque to avoid header in .h)

	// Next profile ID (thread-safe via atomic)
	std::atomic<int> nextProfileId;
};
