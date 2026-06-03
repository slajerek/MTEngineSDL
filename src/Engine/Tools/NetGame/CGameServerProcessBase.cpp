#include "CGameServerProcessBase.h"
#include "CNetGameServer.h"
#include "CLobbyLink.h"
#include "CRegistryClient.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "DBG_Log.h"
#include "json.hpp"
#include <cstring>
#include <csignal>

using namespace nlohmann;

static volatile bool sShutdownRequested = false;

static void GameServerSignalHandler(int sig)
{
	LOGM("CGameServerProcessBase: received signal %d, shutting down", sig);
	sShutdownRequested = true;
}

bool CGameServerProcessBase::ParseArgs(int argc, const char **argv, Config &config)
{
	bool isGameServer = false;

	for (int i = 0; i < argc; i++)
	{
		if (strcmp(argv[i], "--game-server") == 0)
			isGameServer = true;
		else if (strcmp(argv[i], "--game-id") == 0 && i + 1 < argc)
			config.gameId = argv[++i];
		else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
			config.port = atoi(argv[++i]);
		else if (strcmp(argv[i], "--lobby-address") == 0 && i + 1 < argc)
			config.lobbyAddress = argv[++i];
		else if (strcmp(argv[i], "--lobby-service-port") == 0 && i + 1 < argc)
			config.lobbyServicePort = atoi(argv[++i]);
		else if (strcmp(argv[i], "--state-file") == 0 && i + 1 < argc)
			config.stateFilePath = argv[++i];
		else if (strcmp(argv[i], "--app-path") == 0 && i + 1 < argc)
			config.appPath = argv[++i];
		else if (strcmp(argv[i], "--registry-address") == 0 && i + 1 < argc)
			config.registryAddress = argv[++i];
		else if (strcmp(argv[i], "--registry-port") == 0 && i + 1 < argc)
			config.registryPort = atoi(argv[++i]);
		else if (strcmp(argv[i], "--internal-secret") == 0 && i + 1 < argc)
			config.internalSecret = argv[++i];
	}

	// Environment variable fallback for internal secret (avoids ps aux exposure)
	if (config.internalSecret.empty())
	{
		const char *envSecret = getenv("LH_INTERNAL_SECRET");
		if (envSecret && envSecret[0] != '\0')
			config.internalSecret = envSecret;
	}

	return isGameServer;
}

void CGameServerProcessBase::RunServer(const Config &config, CNetGameServer *gameServer)
{
	LOGM("CGameServerProcessBase::RunServer: gameId=%s port=%d lobbyAddress=%s lobbyServicePort=%d",
		 config.gameId.c_str(), config.port, config.lobbyAddress.c_str(), config.lobbyServicePort);

	// Install signal handlers for graceful shutdown
	sShutdownRequested = false;
	signal(SIGTERM, GameServerSignalHandler);
	signal(SIGINT, GameServerSignalHandler);

	gameServer->gameId = config.gameId;

	string serverId = "gs-" + config.gameId.substr(0, 8);

	// Choose connection mode: standalone registry or embedded lobby service
	CLobbyLink *lobbyLink = NULL;
	CRegistryClient *registryClient = NULL;

	if (config.registryPort > 0)
	{
		// STANDALONE registry mode: connect to external CRegistryServer
		LOGM("CGameServerProcessBase: connecting to standalone registry at %s:%d",
			 config.registryAddress.c_str(), config.registryPort);

		registryClient = new CRegistryClient(config.registryAddress, config.registryPort,
								 RegistryClientRole::GAME_SERVER, "gameserver-" + serverId, config.internalSecret);
		registryClient->gameServer = gameServer;
		registryClient->serverId = serverId;
		registryClient->gameId = config.gameId;
		registryClient->listenPort = config.port;
		registryClient->listenAddress = "localhost";
		registryClient->Connect();
	}
	else
	{
		// EMBEDDED mode: connect to lobby's CLobbyServiceServer
		lobbyLink = new CLobbyLink(config.lobbyAddress, config.lobbyServicePort,
							   serverId, config.gameId,
							   config.port, gameServer, config.internalSecret);
		gameServer->lobbyLink = lobbyLink;
		lobbyLink->Connect();
	}

	LOGM("CGameServerProcessBase: game server started, waiting for registration");

	// Main loop: run until shutdown
	while (!sShutdownRequested)
	{
		if (lobbyLink && lobbyLink->shutdownRequested)
			break;
		if (registryClient && registryClient->shutdownRequested)
			break;

		// Standalone registry client heartbeats.
		if (registryClient)
			registryClient->Update();
		SYS_Sleep(50);
	}

	bool lobbyShutdown = (lobbyLink && lobbyLink->shutdownRequested) ||
						 (registryClient && registryClient->shutdownRequested);
	LOGM("CGameServerProcessBase: shutting down (signal=%d lobbyShutdown=%d)",
		 (int)sShutdownRequested, (int)lobbyShutdown);

	// Clean shutdown
	if (lobbyLink)
	{
		lobbyLink->Disconnect();
		gameServer->lobbyLink = NULL;
	}
	if (registryClient)
	{
		registryClient->Disconnect();
	}

	gameServer->Shutdown();

	delete lobbyLink;
	delete registryClient;

	LOGM("CGameServerProcessBase: shutdown complete");
}
