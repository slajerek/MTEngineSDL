#pragma once

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
	};

	// Parse --auth-server CLI args. Returns true if --auth-server flag was found.
	static bool ParseArgs(int argc, const char **argv, Config &config);

	// Run the auth service. Blocks until shutdown signal received.
	static void Run(const Config &config);
};
