#include "CAuthServiceProcess.h"
#include "CAuthServiceServer.h"
#include "CNetGameDataProviderLocalFiles.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "SYS_FileSystem.h"
#include "DBG_Log.h"
#include <cstring>
#include <csignal>

static volatile bool sAuthShutdownRequested = false;

static void AuthServiceSignalHandler(int sig)
{
	LOGM("CAuthServiceProcess: received signal %d, shutting down", sig);
	sAuthShutdownRequested = true;
}

bool CAuthServiceProcess::ParseArgs(int argc, const char **argv, Config &config)
{
	bool isAuthServer = false;

	for (int i = 0; i < argc; i++)
	{
		if (strcmp(argv[i], "--auth-server") == 0)
			isAuthServer = true;
		else if (strcmp(argv[i], "--auth-port") == 0 && i + 1 < argc)
			config.port = atoi(argv[++i]);
		else if (strcmp(argv[i], "--app-path") == 0 && i + 1 < argc)
			config.appPath = argv[++i];
		else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
			config.configPath = argv[++i];
		else if (strcmp(argv[i], "--rate-limit-max") == 0 && i + 1 < argc)
			config.rateLimitMaxAttempts = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rate-limit-window") == 0 && i + 1 < argc)
			config.rateLimitWindowSeconds = atoi(argv[++i]);
	}

	return isAuthServer;
}

void CAuthServiceProcess::Run(const Config &config)
{
	LOGM("CAuthServiceProcess::Run: port=%d appPath=%s", config.port, config.appPath.c_str());

	// Install signal handlers for graceful shutdown
	sAuthShutdownRequested = false;
	signal(SIGTERM, AuthServiceSignalHandler);
	signal(SIGINT, AuthServiceSignalHandler);

	// Set up resource path for data provider
	if (!config.appPath.empty())
	{
		gPathToResources = strdup(config.appPath.c_str());
	}

	// Create data provider
	string configPath = config.configPath;
	if (configPath.empty())
		configPath = string(gPathToResources) + "config.hjson";

	CNetGameDataProviderLocalFiles *dataProvider = new CNetGameDataProviderLocalFiles(configPath);

	// Create and configure auth server
	CAuthServiceServer *authServer = new CAuthServiceServer(dataProvider, config.port);
	authServer->rateLimitMaxAttempts = config.rateLimitMaxAttempts;
	authServer->rateLimitWindowSeconds = config.rateLimitWindowSeconds;

	// Start serving
	authServer->Start();

	LOGM("CAuthServiceProcess: auth service started, waiting for requests");

	// Main loop: wait for shutdown signal
	while (!sAuthShutdownRequested)
	{
		SYS_Sleep(100);
	}

	LOGM("CAuthServiceProcess: shutting down");

	authServer->Shutdown();
	delete authServer;
	delete dataProvider;

	LOGM("CAuthServiceProcess: shutdown complete");
}
