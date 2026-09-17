#pragma once


// CAPABILITY GUARD (MT_ENABLE_HTTPLIB). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_HTTP declared this removable and nothing enforced it: MT_ENABLE_HTTPLIB appeared in no source at all.
#if MT_ENABLE_HTTPLIB

#include "CAuthServiceServer.h"
#include <string>

using namespace std;

// Standalone auth service process entry point.
// Parses CLI arguments, creates data provider and auth server,
// runs until shutdown signal (SIGTERM/SIGINT).
class CAuthServiceProcess
{
public:
	struct Config
	{
		int port = CAuthServiceServer::DEFAULT_PORT;
		string appPath;
		string configPath;
		int rateLimitMaxAttempts = CAuthServiceServer::DEFAULT_RATE_LIMIT_MAX_ATTEMPTS;
		int rateLimitWindowSeconds = CAuthServiceServer::DEFAULT_RATE_LIMIT_WINDOW_SECONDS;
		string listenInterface = "127.0.0.1";
		bool useSSL = false;
		string sslCertPath, sslKeyPath;
		bool allowPlaintext = false;
		// Shared secret /api/v1/redeem-ticket requires from its caller (the
		// lobby server). Empty means the endpoint is unauthenticated -- the
		// auth server logs a LOGWarning at Start() when that is the case.
		//
		// THREE sources, resolved in Run() in this order of precedence:
		//   1. --internal-secret-file <path>  (file contents, trimmed)
		//   2. MT_INTERNAL_SECRET             (environment)
		//   3. --internal-secret <value>      (CLI, dev only)
		// The CLI form is last because a command line is world-readable via
		// `ps` (final-review Minor 7) -- the private apps' own
		// internal-secret accessors already deprecate their equivalent flag
		// for exactly that reason.
		string internalSecret;
		string internalSecretFile;
	};

	// Parse --auth-server CLI args. Returns true if --auth-server flag was found.
	static bool ParseArgs(int argc, const char **argv, Config &config);

	// Run the auth service. Blocks until shutdown signal received.
	static void Run(const Config &config);
};

#endif // MT_ENABLE_HTTPLIB
