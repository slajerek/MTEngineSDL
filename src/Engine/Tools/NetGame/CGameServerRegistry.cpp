#include "CGameServerRegistry.h"
#include "CLobbyServiceServer.h"
#include "CRegistryClient.h"
#include "CRegistryServer.h"
#include "CServerGame.h"
#include "CServerGamesManagerBase.h"
#include "SYS_Funct.h"
#include "DBG_Log.h"

// EMBEDDED mode: uses local CLobbyServiceServer
CGameServerRegistry::CGameServerRegistry(CLobbyServiceServer *serviceServer, const string &internalSecret)
{
	this->mode = RegistryMode::EMBEDDED;
	this->internalSecret = internalSecret;
	this->serviceServer = serviceServer;
	this->registryClient = NULL;
	this->gamesManager = NULL;
	this->getGameCallback = nullptr;
	this->onAllocationResult = nullptr;

	// Wire up back-reference so CLobbyServiceServer can notify us of events
	serviceServer->registry = this;
}

// STANDALONE mode: connects to external CRegistryServer
CGameServerRegistry::CGameServerRegistry(const string &registryAddress, int registryPort, const string &lobbyId, const string &internalSecret)
{
	this->mode = RegistryMode::STANDALONE;
	this->internalSecret = internalSecret;
	this->serviceServer = NULL;
	this->gamesManager = NULL;
	this->getGameCallback = nullptr;
	this->onAllocationResult = nullptr;

	registryClient = new CRegistryClient(registryAddress, registryPort,
									 RegistryClientRole::LOBBY, lobbyId, internalSecret);
	registryClient->lobbyCallback = this;
	registryClient->Connect();

	LOGM("CGameServerRegistry: STANDALONE mode, connecting to registry at %s:%d as %s",
		 registryAddress.c_str(), registryPort, lobbyId.c_str());
}

CGameServerRegistry::~CGameServerRegistry()
{
	if (registryClient)
	{
		registryClient->lobbyCallback = NULL;
		registryClient->Shutdown();
		delete registryClient;
		registryClient = NULL;
	}
}

bool CGameServerRegistry::WaitForConnection(int timeoutMs)
{
	if (mode == RegistryMode::EMBEDDED)
		return true;

	int elapsed = 0;
	while (elapsed < timeoutMs)
	{
		if (registryClient && registryClient->isRegistered)
			return true;
		SYS_Sleep(50);
		elapsed += 50;
	}
	return registryClient && registryClient->isRegistered;
}

void CGameServerRegistry::PushTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime)
{
	LOGM("CGameServerRegistry::PushTokens: gameId=%s tokens=%d mode=%s",
		 gameId.c_str(), (int)tokens.size(), mode == RegistryMode::EMBEDDED ? "EMBEDDED" : "STANDALONE");

	if (mode == RegistryMode::EMBEDDED)
	{
		serviceServer->SendUpdateTokens(gameId, tokens, nameToId, tokenCreationTime);
	}
	else
	{
		registryClient->PushTokens(gameId, tokens, nameToId, tokenCreationTime);
	}
}

void CGameServerRegistry::RequestSaveState(const string &gameId)
{
	LOGM("CGameServerRegistry::RequestSaveState: gameId=%s", gameId.c_str());

	if (mode == RegistryMode::EMBEDDED)
		serviceServer->SendSaveState(gameId);
	else
		registryClient->RequestSaveState(gameId);
}

void CGameServerRegistry::RequestShutdown(const string &gameId, const string &reason)
{
	LOGM("CGameServerRegistry::RequestShutdown: gameId=%s reason=%s", gameId.c_str(), reason.c_str());

	if (mode == RegistryMode::EMBEDDED)
		serviceServer->SendShutdown(gameId, reason);
	else
		registryClient->RequestShutdown(gameId, reason);
}

void CGameServerRegistry::KickPlayer(const string &gameId, const string &playerName, const string &reason)
{
	LOGM("CGameServerRegistry::KickPlayer: gameId=%s player=%s", gameId.c_str(), playerName.c_str());

	if (mode == RegistryMode::EMBEDDED)
		serviceServer->SendKickPlayer(gameId, playerName, reason);
	else
		registryClient->KickPlayer(gameId, playerName, reason);
}

CGameServerInfo CGameServerRegistry::GetGameServerInfo(const string &gameId)
{
	CGameServerInfo info;
	info.gameId = gameId;

	if (mode == RegistryMode::EMBEDDED)
	{
		CGameServerRecord *record = serviceServer->GetRecord(gameId);
		if (record)
		{
			info.address = record->listenAddress;
			info.port = record->listenPort;
			info.connectedPlayers = record->connectedPlayers;
			info.status = record->status;
			info.isReachable = (record->status == "running");
		}
	}
	else
	{
		// In standalone mode, query the registry asynchronously
		// For now, we don't have a synchronous query — this can be expanded later
		// The registry notifies us via callbacks
	}

	return info;
}

bool CGameServerRegistry::IsGameServerReady(const string &gameId)
{
	if (mode == RegistryMode::EMBEDDED)
		return serviceServer->IsGameServerReady(gameId);

	// In standalone mode, we don't have direct access — rely on registry notifications
	// For now, always return true if the game exists (the registry would have notified us if it's down)
	return true;
}

void CGameServerRegistry::OnPlayerEvent(const string &gameId, const string &playerName, const string &event)
{
	LOGM("CGameServerRegistry::OnPlayerEvent: gameId=%s player=%s event=%s",
		 gameId.c_str(), playerName.c_str(), event.c_str());

	// Update CServerGame records if a game lookup callback is provided
	if (!getGameCallback)
		return;

	CServerGame *game = getGameCallback(gameId);
	if (!game)
		return;

	if (event == "connected")
	{
		game->connectedPlayerCount++;
		game->lastActivityTime = time(NULL);
	}
	else if (event == "disconnected")
	{
		if (game->connectedPlayerCount > 0)
			game->connectedPlayerCount--;
		game->lastActivityTime = time(NULL);
	}
	else if (event == "authenticated")
	{
		game->lastActivityTime = time(NULL);
	}
}

void CGameServerRegistry::Update()
{
	if (mode == RegistryMode::EMBEDDED)
		serviceServer->CheckHeartbeatTimeouts();
	else if (registryClient)
		registryClient->Update();
}

// --- CRegistryClientLobbyCallback (STANDALONE mode) ---

void CGameServerRegistry::RegistryPlayerEvent(const string &gameId, const string &playerName, const string &event)
{
	OnPlayerEvent(gameId, playerName, event);
}

void CGameServerRegistry::RegistryGameServerRegistered(const string &gameId, const string &serverId,
													   const string &listenAddress, int listenPort)
{
	LOGM("CGameServerRegistry: game server registered via registry gameId=%s serverId=%s",
		 gameId.c_str(), serverId.c_str());
}

void CGameServerRegistry::RegistryGameServerDisconnected(const string &gameId)
{
	LOGM("CGameServerRegistry: game server disconnected via registry gameId=%s", gameId.c_str());
}

void CGameServerRegistry::RegistryGameServerUnreachable(const string &gameId)
{
	LOGM("CGameServerRegistry: game server unreachable via registry gameId=%s", gameId.c_str());
}

void CGameServerRegistry::RegistryShutdownComplete(const string &gameId)
{
	LOGM("CGameServerRegistry: shutdown complete via registry gameId=%s", gameId.c_str());
}

void CGameServerRegistry::RequestAllocateGameServer(const string &gameId, const string &mapName, int playerCount, bool resume)
{
	if (mode == RegistryMode::EMBEDDED)
	{
		LOGError("CGameServerRegistry::RequestAllocateGameServer: not supported in EMBEDDED mode");
		if (onAllocationResult)
			onAllocationResult(gameId, false, "", "", 0, "embedded_mode_not_supported");
		return;
	}

	if (!registryClient || !registryClient->isRegistered)
	{
		LOGError("CGameServerRegistry::RequestAllocateGameServer: registry client not connected");
		if (onAllocationResult)
			onAllocationResult(gameId, false, "", "", 0, "registry_not_connected");
		return;
	}

	LOGM("CGameServerRegistry::RequestAllocateGameServer: gameId=%s mapName=%s players=%d resume=%s",
		 gameId.c_str(), mapName.c_str(), playerCount, resume ? "true" : "false");

	registryClient->RequestAllocateGameServer(gameId, mapName, playerCount, resume);
}

void CGameServerRegistry::RegistryAllocateGameServerResult(const string &gameId, bool accepted,
	const string &nodeId, const string &address, int port, const string &reason)
{
	LOGM("CGameServerRegistry: allocation result for gameId=%s accepted=%s nodeId=%s address=%s port=%d reason=%s",
		 gameId.c_str(), accepted ? "true" : "false", nodeId.c_str(), address.c_str(), port, reason.c_str());

	// Update the game record under gamesManager mutex if available
	if (accepted && getGameCallback && gamesManager)
	{
		gamesManager->mutex->Lock();
		CServerGame *game = getGameCallback(gameId);
		if (game)
		{
			game->serverAddress = address;
			game->serverPort = port;
			game->isRemoteProcess = true;
			game->remoteNodeId = nodeId;
			game->status = CServerGame::RUNNING;
			game->lastActivityTime = time(NULL);
		}
		gamesManager->mutex->Unlock();
	}

	if (onAllocationResult)
		onAllocationResult(gameId, accepted, nodeId, address, port, reason);
}
