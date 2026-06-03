#include "CAssetServerProcess.h"
#include "CAssetServer.h"

#include "SYS_Main.h"
#include "SYS_FileSystem.h"

#include "DBG_Log.h"

#include <csignal>
#include <cstring>

using namespace std;

static volatile sig_atomic_t sAssetShutdownRequested = 0;

static void AssetServerSignalHandler(int sig)
{
	(void)sig;
	sAssetShutdownRequested = 1;
}

bool CAssetServerProcess::ParseArgs(int argc, const char **argv, Config &config)
{
	bool found = false;
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--asset-server") == 0)
		{
			found = true;
		}
		else if (strcmp(argv[i], "--asset-port") == 0 && i + 1 < argc)
		{
			config.port = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--app-path") == 0 && i + 1 < argc)
		{
			config.appPath = argv[++i];
		}
		else if (strcmp(argv[i], "--assets-folder") == 0 && i + 1 < argc)
		{
			config.assetsFolder = argv[++i];
		}
		else if (strcmp(argv[i], "--projects-folder") == 0 && i + 1 < argc)
		{
			config.projectsFolder = argv[++i];
		}
		else if (strcmp(argv[i], "--auth-address") == 0 && i + 1 < argc)
		{
			config.authServiceAddress = argv[++i];
		}
		else if (strcmp(argv[i], "--auth-port") == 0 && i + 1 < argc)
		{
			config.authServicePort = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--ssl") == 0)
		{
			config.useSSL = true;
		}
		else if (strcmp(argv[i], "--ssl-cert") == 0 && i + 1 < argc)
		{
			config.sslCertPath = argv[++i];
		}
		else if (strcmp(argv[i], "--ssl-key") == 0 && i + 1 < argc)
		{
			config.sslKeyPath = argv[++i];
		}
	}
	return found;
}

void CAssetServerProcess::Run(const Config &config)
{
	signal(SIGTERM, AssetServerSignalHandler);
	signal(SIGINT, AssetServerSignalHandler);

	if (!config.appPath.empty())
	{
		char *dup = strdup(config.appPath.c_str());
		gPathToResources = dup;
	}

	CAssetServer *server = new CAssetServer(config.port);
	server->assetsFolder = config.assetsFolder;
	server->projectsFolder = config.projectsFolder;
	server->authServiceAddress = config.authServiceAddress;
	server->authServicePort = config.authServicePort;
	server->useSSL = config.useSSL;
	server->sslCertPath = config.sslCertPath;
	server->sslKeyPath = config.sslKeyPath;

	LOGM("CAssetServerProcess: starting on port %d", config.port);
	server->Start();

	while (!sAssetShutdownRequested)
	{
		SYS_Sleep(100);
	}

	LOGM("CAssetServerProcess: shutting down");
	server->Shutdown();
	delete server;
}
