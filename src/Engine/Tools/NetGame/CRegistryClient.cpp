#include "CRegistryClient.h"
#include "MT_NetGameAuthDomains.h"
#include "CNetGameServer.h"
#include "CNetPacket.h"
#include "CNetClientData.h"
#include "CNetGameInternalAuth.h"
#include "DBG_Log.h"
#include "SYS_Funct.h"
#include "SYS_Crypto.h"
#include <cstring>

CRegistryClient::CRegistryClient(const string &registryAddress, int registryPort,
							 RegistryClientRole role, const string &clientId,
							 const string &internalSecret)
{
	this->role = role;
	this->clientId = clientId;
	this->isConnected = false;
	this->isRegistered = false;
	this->shutdownRequested = false;
	this->adminAuthReceived = false;
	this->adminAuthorized = false;
	this->adminAuthReason = "";
	this->adminClusterStateReceived = false;
	this->adminClusterState = json::object();
	this->adminCommandResultReceived = false;
	this->adminCommandAccepted = false;
	this->adminCommandName = "";
	this->adminCommandReason = "";
	this->adminCommandId = 0;
	this->adminCommandCompleted = false;
	this->adminCommandTargetedNodes = 0;
	this->adminCommandDispatchedNodes = 0;
	this->adminCommandSuccessfulNodes = 0;
	this->adminCommandFailedNodes = 0;
	this->adminMaintenanceResultReceived = false;
	this->adminMaintenanceAccepted = false;
	this->adminShutdownProgressReceived = false;
	this->adminShutdownPhase = "";
	this->adminShutdownMessage = "";
	this->adminShutdownComplete = false;
	this->lastHeartbeatTime = 0;
	this->startTime = time(NULL);
	this->lobbyCallback = NULL;
	this->gameServerCallback = NULL;
	this->nodeAgentCallback = NULL;
	this->gameServer.store(nullptr);
	this->listenPort = 0;
	this->nodeId = "";
	this->nodeIp = "";
	this->agentPort = 0;
	this->maxProcessCapacity = 0;
	this->nodeAgentProcessCount = 0;
	this->nodeAgentMetrics = json::object();

	// Connect using clientId as the login name
	string loginName = clientId;
	vector<u8> secretHash = NetGameInternalSecretToHash(internalSecret);

	netClient = new CNetClient(registryAddress.c_str(), registryPort, 0, loginName, secretHash);
	netPackets = new CNetGamePackets();
	netClient->AddClientCallback(this);
	netClient->AddPacketCallback(netPackets);
}

CRegistryClient::~CRegistryClient()
{
	Shutdown();
}

void CRegistryClient::Connect()
{
	const char *roleName = "UNKNOWN";
	if (role == RegistryClientRole::LOBBY)
		roleName = "LOBBY";
	else if (role == RegistryClientRole::GAME_SERVER)
		roleName = "GAME_SERVER";
	else if (role == RegistryClientRole::NODE_AGENT)
		roleName = "NODE_AGENT";
	else if (role == RegistryClientRole::ADMIN)
		roleName = "ADMIN";

	LOGM("CRegistryClient::Connect: connecting to registry as %s role=%s",
		 clientId.c_str(), roleName);
	netClient->Connect();
}

void CRegistryClient::Disconnect()
{
	if (netClient)
	{
		netClient->RemoveClientCallback(this);
		netClient->Disconnect();
	}
	isConnected = false;
	isRegistered = false;
}

void CRegistryClient::Shutdown()
{
	if (netClient)
	{
		netClient->RemoveClientCallback(this);
		netClient->Disconnect();
		netClient->status = NET_CLIENT_STATUS_SHUTDOWN;

		// Wait for network thread to finish
		for (int i = 0; i < 100; i++)
		{
			if (!netClient->isRunning)
				break;
			SYS_Sleep(20);
		}
	}
	isConnected = false;
	isRegistered = false;
}

void CRegistryClient::Update()
{
	if (!isConnected || !isRegistered)
		return;

	// Game servers send heartbeats
	if (role == RegistryClientRole::GAME_SERVER)
	{
		time_t now = time(NULL);
		if (difftime(now, lastHeartbeatTime) >= HEARTBEAT_INTERVAL_SECONDS)
		{
			json j;
			j["action"] = "gs_heartbeat";
			j["serverId"] = serverId;
			j["gameId"] = gameId;
			CNetGameServer *gs = gameServer.load();
			j["connectedPlayers"] = gs ? gs->connectedPlayerCount : 0;
			j["status"] = "running";
			j["uptimeSeconds"] = (int64_t)difftime(time(NULL), startTime);
			SendJson(j);

			lastHeartbeatTime = now;
		}
	}
	else if (role == RegistryClientRole::NODE_AGENT)
	{
		time_t now = time(NULL);
		if (difftime(now, lastHeartbeatTime) >= HEARTBEAT_INTERVAL_SECONDS)
		{
			json metrics = nodeAgentMetrics.empty() ? json::object() : nodeAgentMetrics;
			if (!metrics.contains("uptimeSeconds"))
				metrics["uptimeSeconds"] = (int64_t)difftime(now, startTime);
			SendNodeAgentHeartbeat(nodeAgentProcessCount, metrics);
			lastHeartbeatTime = now;
		}
	}
}

// --- Admin-side operations ---

void CRegistryClient::AdminAuthenticate(const string &adminSecret)
{
	const char *domainKey = MTNetGameAdminDomainRef();
	auto digest = SYS_HmacSha256((const uint8_t *)domainKey, strlen(domainKey),
							 (const uint8_t *)adminSecret.data(), adminSecret.size());
	vector<u8> hash(digest.begin(), digest.end());

	json j;
	j["action"] = "admin_auth";
	j["secretHash"] = hash;
	SendJson(j);
}

void CRegistryClient::AdminGetClusterState()
{
	json j;
	j["action"] = "admin_getClusterState";
	SendJson(j);
}

void CRegistryClient::AdminStartManagedServices(const string &scope, const string &nodeId, const string &roleName)
{
	adminCommandResultReceived = false;
	adminCommandCompleted = false;
	adminCommandAccepted = false;
	adminCommandReason = "";
	adminCommandSuccessfulNodes = 0;
	adminCommandFailedNodes = 0;
	adminCommandId = 0;

	json j;
	j["action"] = "admin_startManagedServices";
	j["scope"] = scope;
	if (!nodeId.empty())
		j["nodeId"] = nodeId;
	if (!roleName.empty())
		j["role"] = roleName;
	SendJson(j);
}

void CRegistryClient::AdminStopManagedServices(const string &scope, const string &nodeId, const string &roleName,
							   bool graceful, bool includeRegistry)
{
	adminCommandResultReceived = false;
	adminCommandCompleted = false;
	adminCommandAccepted = false;
	adminCommandReason = "";
	adminCommandSuccessfulNodes = 0;
	adminCommandFailedNodes = 0;
	adminCommandId = 0;

	json j;
	j["action"] = "admin_stopManagedServices";
	j["scope"] = scope;
	j["graceful"] = graceful;
	j["includeRegistry"] = includeRegistry;
	if (!nodeId.empty())
		j["nodeId"] = nodeId;
	if (!roleName.empty())
		j["role"] = roleName;
	SendJson(j);
}

void CRegistryClient::AdminMaintenanceNode(const string &nodeId, bool enable)
{
	adminMaintenanceResultReceived = false;
	adminMaintenanceAccepted = false;
	adminMaintenanceMessage = "";

	json j;
	j["action"] = "admin_maintenanceNode";
	j["nodeId"] = nodeId;
	j["enable"] = enable;
	SendJson(j);
}

void CRegistryClient::AdminGracefulShutdown(int drainTimeoutSec, bool forceDrain)
{
	adminShutdownProgressReceived = false;
	adminShutdownPhase = "";
	adminShutdownMessage = "";
	adminShutdownComplete = false;

	json j;
	j["action"] = "admin_gracefulShutdown";
	j["drainTimeoutSec"] = drainTimeoutSec;
	j["forceDrain"] = forceDrain;
	SendJson(j);
}

// --- Lobby-side operations ---

void CRegistryClient::RegisterAsLobby()
{
	json j;
	j["action"] = "lobby_register";
	j["lobbyId"] = clientId;
	SendJson(j);
	LOGM("CRegistryClient::RegisterAsLobby: lobbyId=%s", clientId.c_str());
}

void CRegistryClient::PushTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime)
{
	json j;
	j["action"] = "lobby_updateTokens";
	j["gameId"] = gameId;
	json tokensJson = json::object();
	for (const auto &pair : tokens)
		tokensJson[std::to_string(pair.first)] = pair.second;
	j["tokens"] = tokensJson;
	json nameToIdJson = json::object();
	for (const auto &pair : nameToId)
		nameToIdJson[pair.first] = pair.second;
	j["nameToId"] = nameToIdJson;
	j["tokenCreationTime"] = (int64_t)tokenCreationTime;
	SendJson(j);
}

void CRegistryClient::RequestSaveState(const string &gameId)
{
	json j;
	j["action"] = "lobby_saveState";
	j["gameId"] = gameId;
	SendJson(j);
}

void CRegistryClient::RequestShutdown(const string &gameId, const string &reason)
{
	json j;
	j["action"] = "lobby_shutdown";
	j["gameId"] = gameId;
	j["reason"] = reason;
	SendJson(j);
}

void CRegistryClient::KickPlayer(const string &gameId, const string &playerName, const string &reason)
{
	json j;
	j["action"] = "lobby_kickPlayer";
	j["gameId"] = gameId;
	j["playerName"] = playerName;
	j["reason"] = reason;
	SendJson(j);
}

void CRegistryClient::QueryGameServer(const string &gameId)
{
	json j;
	j["action"] = "lobby_queryGameServer";
	j["gameId"] = gameId;
	SendJson(j);
}

void CRegistryClient::RequestAllocateGameServer(const string &gameId, const string &mapName, int playerCount, bool resume)
{
	json j;
	j["action"] = "lobby_allocateGameServer";
	j["gameId"] = gameId;
	j["mapName"] = mapName;
	j["playerCount"] = playerCount;
	j["resume"] = resume;
	SendJson(j);

	LOGM("CRegistryClient::RequestAllocateGameServer: gameId=%s mapName=%s players=%d resume=%s",
		 gameId.c_str(), mapName.c_str(), playerCount, resume ? "true" : "false");
}

void CRegistryClient::QueryGameStatus(const string &gameId)
{
	json j;
	j["action"] = "lobby_queryGameStatus";
	j["gameId"] = gameId;
	SendJson(j);
}

// --- Game server-side operations ---

void CRegistryClient::RegisterAsGameServer(const string &serverId, const string &gameId,
										   int listenPort, const string &listenAddress)
{
	this->serverId = serverId;
	this->gameId = gameId;
	this->listenPort = listenPort;
	this->listenAddress = listenAddress;

	json j;
	j["action"] = "gs_register";
	j["serverId"] = serverId;
	j["gameId"] = gameId;
	j["listenPort"] = listenPort;
	j["listenAddress"] = listenAddress;
	CNetGameServer *gs = gameServer.load();
	j["maxPlayers"] = gs ? (int)gs->allowedPlayers.size() : 0;
	j["version"] = 1;
	SendJson(j);

	LOGM("CRegistryClient::RegisterAsGameServer: serverId=%s gameId=%s port=%d",
		 serverId.c_str(), gameId.c_str(), listenPort);
}

void CRegistryClient::SendPlayerEvent(const string &playerName, const string &event)
{
	json j;
	j["action"] = "gs_playerEvent";
	j["serverId"] = serverId;
	j["gameId"] = gameId;
	j["playerName"] = playerName;
	j["event"] = event;
	SendJson(j);
}

void CRegistryClient::SendStateSaved()
{
	json j;
	j["action"] = "gs_stateSaved";
	j["serverId"] = serverId;
	j["gameId"] = gameId;
	SendJson(j);
}

void CRegistryClient::SendShutdownComplete()
{
	json j;
	j["action"] = "gs_shutdownComplete";
	j["serverId"] = serverId;
	j["gameId"] = gameId;
	SendJson(j);
}

void CRegistryClient::RegisterAsNodeAgent(const string &nodeId, const string &nodeIp, int agentPort, int maxProcessCapacity)
{
	this->nodeId = nodeId;
	this->nodeIp = nodeIp;
	this->agentPort = agentPort;
	this->maxProcessCapacity = maxProcessCapacity;

	json j;
	j["action"] = "na_register";
	j["nodeId"] = nodeId;
	j["nodeIp"] = nodeIp;
	j["agentPort"] = agentPort;
	j["maxProcessCapacity"] = maxProcessCapacity;
	SendJson(j);

	LOGM("CRegistryClient::RegisterAsNodeAgent: nodeId=%s nodeIp=%s agentPort=%d maxProcessCapacity=%d",
		 nodeId.c_str(), nodeIp.c_str(), agentPort, maxProcessCapacity);
}

void CRegistryClient::SendNodeAgentHeartbeat(int processCount, const json &metrics)
{
	if (nodeId.empty())
		return;

	json j;
	j["action"] = "na_heartbeat";
	j["nodeId"] = nodeId;
	j["processCount"] = processCount;
	j["metrics"] = metrics;
	SendJson(j);
}

void CRegistryClient::SendNodeAgentCommandResult(uint64_t commandId, bool success, const string &reason)
{
	if (nodeId.empty() || commandId == 0)
		return;

	json j;
	j["action"] = "na_commandResult";
	j["commandId"] = commandId;
	j["nodeId"] = nodeId;
	j["success"] = success;
	j["reason"] = reason;
	SendJson(j);
}

void CRegistryClient::SendGameProcessSpawned(uint64_t requestId, const string &gameId, bool success, int pid, const string &reason)
{
	json j;
	j["action"] = "na_gameProcessSpawned";
	j["requestId"] = requestId;
	j["gameId"] = gameId;
	j["nodeId"] = nodeId;
	j["success"] = success;
	j["pid"] = pid;
	j["reason"] = reason;
	SendJson(j);

	LOGM("CRegistryClient::SendGameProcessSpawned: requestId=%llu gameId=%s success=%s pid=%d",
		 (unsigned long long)requestId, gameId.c_str(), success ? "true" : "false", pid);
}

void CRegistryClient::SendCrashReport(const string &roleName, int exitCode, const string &gameId)
{
	json j;
	j["action"] = "na_crashReport";
	j["nodeId"] = nodeId;
	j["roleName"] = roleName;
	j["exitCode"] = exitCode;
	j["timestamp"] = (int64_t)time(NULL);
	if (!gameId.empty())
		j["gameId"] = gameId;
	SendJson(j);

	LOGM("CRegistryClient::SendCrashReport: role=%s exitCode=%d gameId=%s",
		 roleName.c_str(), exitCode, gameId.c_str());
}

// --- CNetClientCallback ---

void CRegistryClient::NetClientCallbackConnected(CNetClient *netClient)
{
	LOGM("CRegistryClient::NetClientCallbackConnected: connected to registry");
	isConnected = true;

	// Auto-register based on role
	if (role == RegistryClientRole::LOBBY)
	{
		RegisterAsLobby();
	}
	else if (role == RegistryClientRole::GAME_SERVER)
	{
		if (!serverId.empty() && !gameId.empty())
			RegisterAsGameServer(serverId, gameId, listenPort, listenAddress);
	}
	else if (role == RegistryClientRole::NODE_AGENT)
	{
		if (!nodeId.empty() && !nodeIp.empty() && agentPort > 0 && maxProcessCapacity > 0)
			RegisterAsNodeAgent(nodeId, nodeIp, agentPort, maxProcessCapacity);
	}
}

void CRegistryClient::NetClientCallbackDisconnected(CNetClient *netClient)
{
	LOGM("CRegistryClient::NetClientCallbackDisconnected: disconnected from registry");
	isConnected = false;
	isRegistered = false;
}

void CRegistryClient::NetClientProcessPacket(CNetPacket *packet)
{
	if (packet->packetType != NET_PACKET_TYPE_JSON)
		return;

	CNetGamePacketJson *p = (CNetGamePacketJson *)packet;
	json j = p->jsonPayload;

	LOGD("CRegistryClient::NetClientProcessPacket: %s", j.dump().c_str());

	try
	{
		if (!j.contains("action") || !j["action"].is_string())
			return;

		string action = j["action"];

		// Registry responses to lobby
		if (action == "registry_lobbyRegistered")
			HandleRegistryLobbyRegistered(j);
		else if (action == "registry_gameServerInfo")
			HandleRegistryGameServerInfo(j);
		else if (action == "registry_playerEvent")
			HandleRegistryPlayerEvent(j);
		else if (action == "registry_gameServerRegistered")
			HandleRegistryGameServerRegistered(j);
		else if (action == "registry_gameServerDisconnected")
			HandleRegistryGameServerDisconnected(j);
		else if (action == "registry_gameServerUnreachable")
			HandleRegistryGameServerUnreachable(j);
		else if (action == "registry_shutdownComplete")
			HandleRegistryShutdownComplete(j);
		else if (action == "registry_stateSaved")
			HandleRegistryStateSaved(j);
		else if (action == "registry_allocateGameServerResult")
			HandleRegistryAllocateGameServerResult(j);
		else if (action == "registry_gameServerCrashed")
		{
			string gid = j.value("gameId", "");
			string nid = j.value("nodeId", "");
			int exitCode = j.value("exitCode", -1);
			if (lobbyCallback)
				lobbyCallback->RegistryGameServerCrashed(gid, nid, exitCode);
		}
		else if (action == "registry_naRegistered")
			HandleRegistryNaRegistered(j);
		else if (action == "registry_manageServices")
			HandleRegistryManageServices(j);
		else if (action == "registry_spawnGameServer")
			HandleRegistrySpawnGameServer(j);
		// Messages forwarded from lobby to game server (via registry)
		else if (action == "lobby_registered")
			HandleLobbyRegistered(j);
		else if (action == "lobby_updateTokens")
			HandleLobbyUpdateTokens(j);
		else if (action == "lobby_saveState")
			HandleLobbySaveState(j);
		else if (action == "lobby_shutdown")
			HandleLobbyShutdown(j);
		else if (action == "lobby_kickPlayer")
			HandleLobbyKickPlayer(j);
		else if (action == "registry_adminAuthResult")
			HandleRegistryAdminAuthResult(j);
		else if (action == "registry_clusterState")
			HandleRegistryClusterState(j);
		else if (action == "registry_adminCommandResult")
			HandleRegistryAdminCommandResult(j);
		else if (action == "registry_adminMaintenanceResult")
			HandleRegistryAdminMaintenanceResult(j);
		else if (action == "registry_shutdownProgress")
			HandleRegistryShutdownProgress(j);
		else
			LOGWarning("CRegistryClient: unknown action=%s", action.c_str());
	}
	catch (exception &ex)
	{
		LOGError("CRegistryClient::NetClientProcessPacket: %s", ex.what());
	}
}

// --- Lobby-side handlers ---

void CRegistryClient::HandleRegistryLobbyRegistered(json &j)
{
	isRegistered = true;
	lastHeartbeatTime = time(NULL);
	LOGM("CRegistryClient::HandleRegistryLobbyRegistered: registered as lobby %s", clientId.c_str());
}

void CRegistryClient::HandleRegistryGameServerInfo(json &j)
{
	// This is a response to QueryGameServer — for now just log
	LOGD("CRegistryClient: game server info for gameId=%s found=%s",
		 j.value("gameId", "").c_str(), j.value("found", false) ? "true" : "false");
}

void CRegistryClient::HandleRegistryPlayerEvent(json &j)
{
	string gameId = j.value("gameId", "");
	string playerName = j.value("playerName", "");
	string event = j.value("event", "");

	if (lobbyCallback)
		lobbyCallback->RegistryPlayerEvent(gameId, playerName, event);
}

void CRegistryClient::HandleRegistryGameServerRegistered(json &j)
{
	string gameId = j.value("gameId", "");
	string serverId = j.value("serverId", "");
	string listenAddress = j.value("listenAddress", "localhost");
	int listenPort = j.value("listenPort", 0);

	if (lobbyCallback)
		lobbyCallback->RegistryGameServerRegistered(gameId, serverId, listenAddress, listenPort);
}

void CRegistryClient::HandleRegistryGameServerDisconnected(json &j)
{
	string gameId = j.value("gameId", "");
	if (lobbyCallback)
		lobbyCallback->RegistryGameServerDisconnected(gameId);
}

void CRegistryClient::HandleRegistryGameServerUnreachable(json &j)
{
	string gameId = j.value("gameId", "");
	if (lobbyCallback)
		lobbyCallback->RegistryGameServerUnreachable(gameId);
}

void CRegistryClient::HandleRegistryShutdownComplete(json &j)
{
	string gameId = j.value("gameId", "");
	if (lobbyCallback)
		lobbyCallback->RegistryShutdownComplete(gameId);
}

void CRegistryClient::HandleRegistryStateSaved(json &j)
{
	string gameId = j.value("gameId", "");
	if (lobbyCallback)
		lobbyCallback->RegistryStateSaved(gameId);
}

void CRegistryClient::HandleRegistryNaRegistered(json &j)
{
	string registeredNodeId = j.value("nodeId", "");
	if (!registeredNodeId.empty())
		nodeId = registeredNodeId;

	isRegistered = true;
	lastHeartbeatTime = time(NULL);
	LOGM("CRegistryClient::HandleRegistryNaRegistered: registered as node agent nodeId=%s", nodeId.c_str());
}

void CRegistryClient::HandleRegistryManageServices(json &j)
{
	string operation = j.value("operation", "");
	uint64_t commandId = j.value("commandId", (uint64_t)0);
	string scope = j.value("scope", "all");
	string targetNodeId = j.value("targetNodeId", "");
	string roleName = j.value("role", "");
	bool graceful = j.value("graceful", true);
	bool includeRegistry = j.value("includeRegistry", false);

	if (nodeAgentCallback)
	{
		nodeAgentCallback->RegistryManageServices(operation, commandId, scope, targetNodeId, roleName,
									 graceful, includeRegistry);
	}
}

void CRegistryClient::HandleRegistrySpawnGameServer(json &j)
{
	uint64_t requestId = j.value("requestId", (uint64_t)0);
	string gameId = j.value("gameId", "");
	int port = j.value("port", 0);
	string registryAddress = j.value("registryAddress", "");
	int registryPort = j.value("registryPort", 0);
	json extraArgs = j.value("extraArgs", json::object());

	LOGM("CRegistryClient::HandleRegistrySpawnGameServer: requestId=%llu gameId=%s port=%d",
		 (unsigned long long)requestId, gameId.c_str(), port);

	if (nodeAgentCallback)
	{
		nodeAgentCallback->RegistrySpawnGameServer(requestId, gameId, port, registryAddress, registryPort, extraArgs);
	}
}

void CRegistryClient::HandleRegistryAllocateGameServerResult(json &j)
{
	string gameId = j.value("gameId", "");
	bool accepted = j.value("accepted", false);
	string nodeId = j.value("nodeId", "");
	string address = j.value("address", "");
	int port = j.value("port", 0);
	string reason = j.value("reason", "");

	LOGM("CRegistryClient::HandleRegistryAllocateGameServerResult: gameId=%s accepted=%s nodeId=%s address=%s port=%d",
		 gameId.c_str(), accepted ? "true" : "false", nodeId.c_str(), address.c_str(), port);

	if (lobbyCallback)
		lobbyCallback->RegistryAllocateGameServerResult(gameId, accepted, nodeId, address, port, reason);
}

// --- Game server-side handlers ---

void CRegistryClient::HandleLobbyRegistered(json &j)
{
	isRegistered = true;
	lastHeartbeatTime = time(NULL);
	LOGM("CRegistryClient::HandleLobbyRegistered: registered as game server for gameId=%s", gameId.c_str());

	if (gameServerCallback)
		gameServerCallback->RegistryRegistered(gameId);
}

void CRegistryClient::HandleLobbyUpdateTokens(json &j)
{
	map<int, string> tokens;
	map<string, int> nameToId;
	time_t tokenCreationTime = (time_t)j.value("tokenCreationTime", (int64_t)time(NULL));

	if (j.contains("tokens") && j["tokens"].is_object())
	{
		for (auto &el : j["tokens"].items())
		{
			int cid = std::stoi(el.key());
			tokens[cid] = el.value().get<string>();
		}
	}

	if (j.contains("nameToId") && j["nameToId"].is_object())
	{
		for (auto &el : j["nameToId"].items())
			nameToId[el.key()] = el.value().get<int>();
	}

	// If gameServer is set, apply tokens directly (like CLobbyLink does)
	CNetGameServer *gsTokens = gameServer.load();
	if (gsTokens)
		gsTokens->SetExpectedTokens(tokens, nameToId, tokenCreationTime);

	if (gameServerCallback)
		gameServerCallback->RegistryUpdateTokens(tokens, nameToId, tokenCreationTime);

	LOGM("CRegistryClient::HandleLobbyUpdateTokens: updated %d tokens for gameId=%s",
		 (int)tokens.size(), gameId.c_str());
}

void CRegistryClient::HandleLobbySaveState(json &j)
{
	CNetGameServer *gsSave = gameServer.load();
	if (gsSave)
		gsSave->SaveState();

	// Confirm
	SendStateSaved();

	if (gameServerCallback)
		gameServerCallback->RegistrySaveState();
}

void CRegistryClient::HandleLobbyShutdown(json &j)
{
	string reason = j.value("reason", "");
	LOGM("CRegistryClient::HandleLobbyShutdown: gameId=%s reason=%s", gameId.c_str(), reason.c_str());

	// Confirm receipt
	SendShutdownComplete();

	// Set flag
	shutdownRequested = true;

	if (gameServerCallback)
		gameServerCallback->RegistryShutdown(reason);
}

void CRegistryClient::HandleLobbyKickPlayer(json &j)
{
	string playerName = j.value("playerName", "");
	string reason = j.value("reason", "kicked");

	CNetGameServer *gsKick = gameServer.load();
	if (gsKick && gsKick->netServer)
	{
		for (int i = 0; i < NET_MAX_CLIENTS; i++)
		{
			CNetClientData *client = gsKick->netServer->clients[i];
			if (client && client->state == NET_CLIENT_STATE_ONLINE && client->clientName == playerName)
			{
				gsKick->DisconnectWithError(client, "Kicked: %s", reason.c_str());
				break;
			}
		}
	}

	if (gameServerCallback)
		gameServerCallback->RegistryKickPlayer(playerName, reason);
}

void CRegistryClient::HandleRegistryAdminAuthResult(json &j)
{
	adminAuthReceived = true;
	adminAuthorized = j.value("authorized", false);
	adminAuthReason = j.value("reason", "");
}

void CRegistryClient::HandleRegistryClusterState(json &j)
{
	adminClusterStateReceived = true;
	if (j.contains("clusterState"))
		adminClusterState = j["clusterState"];
	else
		adminClusterState = json::object();
}

void CRegistryClient::HandleRegistryAdminCommandResult(json &j)
{
	adminCommandResultReceived = true;
	adminCommandAccepted = j.value("accepted", false);
	adminCommandName = j.value("command", "");
	adminCommandReason = j.value("reason", "");
	adminCommandId = j.value("commandId", (uint64_t)0);
	adminCommandCompleted = j.value("completed", false);
	adminCommandTargetedNodes = j.value("targetedNodes", 0);
	adminCommandDispatchedNodes = j.value("dispatchedNodes", 0);
	adminCommandSuccessfulNodes = j.value("successfulNodes", 0);
	adminCommandFailedNodes = j.value("failedNodes", 0);
}

void CRegistryClient::HandleRegistryAdminMaintenanceResult(json &j)
{
	adminMaintenanceResultReceived = true;
	adminMaintenanceAccepted = j.value("accepted", false);
	adminMaintenanceMessage = j.value("message", j.value("reason", ""));
}

void CRegistryClient::HandleRegistryShutdownProgress(json &j)
{
	adminShutdownProgressReceived = true;
	adminShutdownPhase = j.value("phase", "");
	adminShutdownMessage = j.value("message", "");
	adminShutdownComplete = j.value("complete", false);
	LOGM("CRegistryClient: shutdown progress phase=%s complete=%s msg=%s",
		 adminShutdownPhase.c_str(), adminShutdownComplete ? "true" : "false",
		 adminShutdownMessage.c_str());
}

// --- Utility ---

void CRegistryClient::SendJson(json sendJson)
{
	if (!netClient || !netClient->IsOnline())
	{
		LOGD("CRegistryClient::SendJson: not online, dropping packet");
		return;
	}

	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netClient->IssuePacket(true, packet);
	delete packet;
}
