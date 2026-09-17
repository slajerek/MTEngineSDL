#pragma once


// CAPABILITY GUARD (MT_ENABLE_HTTPLIB). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_HTTP declared this removable and nothing enforced it: MT_ENABLE_HTTPLIB appeared in no source at all.
#if MT_ENABLE_HTTPLIB

#include "SYS_Defs.h"
#include "SYS_Threading.h"
#include <string>
#include <map>
#include <ctime>
#include <atomic>
#include <thread>

using namespace std;

class CNetGameDataProvider;
class CNetGameUserProfile;

// HTTP REST Auth Service for user registration, login, and session validation.
// Runs as a standalone HTTP server (separate process or in-process for tests).
// Endpoints: POST /api/v1/register, /api/v1/login, /api/v1/validate, /api/v1/logout
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
	// /api/v1/validate is rate-limited separately from register/login (higher
	// ceiling: it's polled far more often, e.g. by the lobby server per-connect).
	static const int VALIDATE_RATE_LIMIT_MAX = 600;
	// /api/v1/lobby-ticket is rate-limited separately from register/login too.
	// C-short made the ticket fetch a PER-CONNECTION-ATTEMPT operation (CNetClient
	// re-fetches on every RECONNECT, ~1 Hz), not a per-login one -- sharing login's
	// 10/minute bucket meant ~10 seconds of a flapping lobby locked the IP out of
	// /login as well, and behind a NAT that denied login to every other player on
	// that address (final-review I-3).
	static const int TICKET_RATE_LIMIT_MAX = 120;
	// Per-account lockout (independent of the per-IP rate limiter above).
	static const int ACCOUNT_LOCKOUT_FAILURES = 10;
	static const int ACCOUNT_LOCKOUT_SECONDS = 900; // 15 minutes
	// Single-use lobby ticket: short-lived, ENet-safe stand-in for the session
	// token. /api/v1/lobby-ticket mints one from a valid session token;
	// /api/v1/redeem-ticket consumes it exactly once. Kept short because it
	// only needs to survive the HTTPS round trip -> ENet authorize packet ->
	// lobby's redeem call, not a whole play session.
	static const int LOBBY_TICKET_TTL_SECONDS = 60;

	CAuthServiceServer(CNetGameDataProvider *dataProvider, int port = DEFAULT_PORT);
	virtual ~CAuthServiceServer();

	// Returns false when the server refused to start (bad TLS cert/key, or
	// plaintext requested on a non-loopback interface without allowPlaintext).
	bool Start();
	void Shutdown();

	// True for the loopback interface: "127.0.0.1", "localhost", "::1" (exact match).
	static bool IsLoopbackInterface(const std::string &iface);

	CNetGameDataProvider *dataProvider;

	// Configuration
	int port;
	int tokenExpirySeconds;
	int rateLimitMaxAttempts;
	int rateLimitWindowSeconds;

	// Network/TLS configuration
	std::string listenInterface = "127.0.0.1";
	bool useSSL = false;
	std::string sslCertPath, sslKeyPath;
	// Allow plaintext HTTP on a non-loopback interface (trusted network only).
	// Loopback plaintext is always allowed regardless of this flag.
	bool allowPlaintext = false;

	// Shared secret an internal caller (the lobby server) must present to
	// /api/v1/redeem-ticket. Constant-time compared (SYS_ConstantTimeEquals)
	// against the request's "secret" field. Empty means the endpoint is
	// unauthenticated -- Start() logs a LOGWarning once when that is the
	// case, and every "secret" value is accepted.
	std::string internalSecret;

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
	// Erases a session unconditionally. Used by /api/v1/logout, which is
	// idempotent -- erasing a token that isn't present is a silent no-op.
	void LogoutSession(const string &token);
	void CleanupExpiredSessions();

	map<string, SessionInfo> activeSessions;
	CSlrMutex *mutexSessions;
	// Guards throttling CleanupExpiredSessions() to at most once per
	// RATE_LIMIT_CLEANUP_INTERVAL_SECONDS (checked/updated under mutexSessions).
	time_t lastSessionCleanup;

	// Single-use lobby ticket management (the session token itself never
	// crosses ENet -- see LOBBY_TICKET_TTL_SECONDS above).
	struct TicketInfo
	{
		int profileId;
		string playerName;
		time_t expiresAt;
	};

	// Mints a ticket for an already-validated session (profileId/playerName
	// come from ValidateSessionToken). Uses the same random-token routine as
	// GenerateSessionToken() -- a lobby ticket has no other state to derive
	// from, so it is simply another 32-random-bytes/64-hex value.
	string IssueLobbyTicket(int profileId, const string &playerName);
	// Consumes a ticket exactly once: found + unexpired -> true (and the
	// entry is erased before returning, so a second redeem of the same
	// ticket always fails). Unknown, expired, or already-redeemed -> false.
	bool RedeemLobbyTicket(const string &ticket, int &outProfileId, string &outPlayerName);

	map<string, TicketInfo> lobbyTickets; // guarded by mutexSessions

	// Rate limiting
	struct RateLimitEntry
	{
		int attempts;
		time_t windowStart;
	};

	bool CheckRateLimit(const string &clientIP);
	// Separate, higher-ceiling limiter for /api/v1/validate (VALIDATE_RATE_LIMIT_MAX
	// per rateLimitWindowSeconds) -- it is polled far more often than register/login.
	bool CheckValidateRateLimit(const string &clientIP);
	// Separate bucket for /api/v1/lobby-ticket (TICKET_RATE_LIMIT_MAX per
	// rateLimitWindowSeconds) -- see the constant's comment.
	bool CheckTicketRateLimit(const string &clientIP);
	void CleanupStaleRateLimitEntries();

	map<string, RateLimitEntry> rateLimitByIP;
	map<string, RateLimitEntry> validateRateLimitByIP;
	map<string, RateLimitEntry> ticketRateLimitByIP;
	CSlrMutex *mutexRateLimit;
	time_t lastRateLimitCleanup;

	// Per-account lockout, independent of the per-IP rate limiter above.
	// Guarded by mutexRateLimit; stale entries are cleaned in CleanupStaleRateLimitEntries.
	struct LockoutEntry
	{
		int failures;
		time_t lockedUntil;   // 0 == not locked
		time_t lastFailureAt; // for stale-entry cleanup only, not part of the lockout decision
	};
	map<string, LockoutEntry> lockoutByUser;

	// Username validation
	bool IsValidUsername(const string &username);

	bool isRunning;

private:
	// HTTP server runs in a background thread
	void *httpServer; // httplib::Server* (opaque to avoid header in .h)
	// The listen thread, kept (not detached) so Shutdown() can join it before
	// deleting the httplib::Server: every route lambda captures `this`, so
	// deleting the object out from under a live thread is a UAF
	// (final-review Minor 1).
	std::thread serverThread;

	// Next profile ID (thread-safe via atomic)
	std::atomic<int> nextProfileId;

	// Equal-work login: a static verifier (never a real account) built once in
	// Start() from a random secret, with the same salt/iteration shape as a
	// real profile. The locked-account and unknown-username login paths run
	// CheckPassword() against this and discard the result, so all three
	// outcomes (locked / unknown user / known user + wrong password) pay for
	// one PBKDF2 derivation each -- otherwise response latency would reveal
	// account existence/lock state even though the response bodies are
	// byte-identical. Read-only after construction (CheckPassword never
	// mutates a version==1 profile), so concurrent request threads share it
	// safely without locking.
	CNetGameUserProfile *dummyAuthProfile;
};

#endif // MT_ENABLE_HTTPLIB
