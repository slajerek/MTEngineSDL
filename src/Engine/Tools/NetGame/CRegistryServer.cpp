#include "CRegistryServer.h"
#include "NET_Main.h"
#include "SYS_Funct.h"
#include "SYS_Crypto.h"
#include "CNetPacket.h"
#include "CNetClientData.h"
#include "CNetGameInternalAuth.h"
#include "DBG_Log.h"

CRegistryServer::CRegistryServer(int port, const string &internalSecret)
{
	mutex = new CSlrMutex("CRegistryServerMutex");
	internalSecretHash = NetGameInternalSecretToHash(internalSecret);
	hasClusterConfig = false;
	adminCommandSeq = 0;
	adminCommandTimeoutSeconds = 20;
	adminAuthMaxAttempts = 5;
	heartbeatTimeoutSeconds = 15;
	heartbeatWarningSeconds = 8;
	gamePortRangeStart = 14701;
	gamePortRangeEnd = 14799;
	gameAllocationSeq = 0;
	crashSeqCounter = 0;
	registryExternalAddress = "localhost";
	shutdownPhase = ShutdownPhase::NONE;
	shutdownStartedAt = 0;
	shutdownDrainTimeoutSeconds = 1800;
	shutdownForceDrain = false;
	shutdownAdminClient = NULL;

	netServer = new CNetServer(port);
	netPackets = new CNetGamePackets();
	netServer->AddServerCallback(this);
	netServer->AddPacketCallback(netPackets);
}

CRegistryServer::~CRegistryServer()
{
	Shutdown();

	mutex->Lock();
	for (auto &pair : gameServers)
		delete pair.second;
	gameServers.clear();
	for (auto &pair : lobbies)
		delete pair.second;
	lobbies.clear();
	for (auto &pair : nodeAgentsByNodeId)
		delete pair.second;
	nodeAgentsByNodeId.clear();
	nodeAgents.clear();
	mutex->Unlock();

	delete mutex;
}

void CRegistryServer::Start()
{
	netServer->StartServer();
	if (internalSecretHash.empty())
		LOGWarning("CRegistryServer: internal secret not configured; all connections will be rejected");
	LOGM("CRegistryServer: started on port %d", netServer->serverPort);
}

void CRegistryServer::Shutdown()
{
	if (netServer)
	{
		netServer->RemoveServerCallback(this);
		netServer->status = NET_SERVER_STATUS_SHUTDOWN;

		for (int i = 0; i < 100; i++)
		{
			if (!netServer->isRunning)
				break;
			SYS_Sleep(20);
		}
	}
}

u8 CRegistryServer::NetServerAuthorize(CNetClientData *clientData, string userName, vector<u8> passwordHash)
{
	// Internal service — require shared secret.
	if (!NetGameInternalSecretMatches(internalSecretHash, passwordHash))
	{
		LOGWarning("CRegistryServer::NetServerAuthorize: rejecting %s (invalid internal secret)", userName.c_str());
		return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;
	}
	LOGD("CRegistryServer::NetServerAuthorize: accepted %s", userName.c_str());
	return NET_SERVER_CALLBACK_AUTHORIZE_CORRECT;
}

void CRegistryServer::NetServerCallbackClientConnected(CNetClientData *clientData)
{
	LOGD("CRegistryServer::NetServerCallbackClientConnected: %s", clientData->clientName.c_str());
	mutex->Lock();
	clientRoles[clientData] = ClientRole::UNKNOWN;
	mutex->Unlock();
}

void CRegistryServer::NetServerCallbackClientDisconnected(CNetClientData *clientData)
{
	LOGD("CRegistryServer::NetServerCallbackClientDisconnected: %s", clientData->clientName.c_str());

	CNetClientData *lobbyClientToNotify = NULL;
	string disconnectedGameId;
	string disconnectedServerId;
	vector<pair<CNetClientData *, json>> adminCompletions;

	mutex->Lock();

	// Remove from game servers if applicable
	for (auto it = gameServers.begin(); it != gameServers.end(); ++it)
	{
		if (it->second->clientData == clientData)
		{
			LOGM("CRegistryServer: game server disconnected gameId=%s serverId=%s",
				 it->second->gameId.c_str(), it->second->serverId.c_str());
			lobbyClientToNotify = FindLobbyForNotificationLocked();
			disconnectedGameId = it->second->gameId;
			disconnectedServerId = it->second->serverId;

			// Release allocated port back to the node's port pool
			if (!it->second->nodeId.empty() && it->second->listenPort > 0)
			{
				ReleasePortOnNode(it->second->nodeId, it->second->listenPort);
				LOGM("CRegistryServer: released port %d on node %s",
					 it->second->listenPort, it->second->nodeId.c_str());
			}

			// Decrement running game count on the node (for drain tracking)
			if (!it->second->nodeId.empty())
			{
				auto *nodeRecord = GetNodeAgentRecord(it->second->nodeId);
				if (nodeRecord && nodeRecord->runningGameCount > 0)
					nodeRecord->runningGameCount--;
			}

			delete it->second;
			gameServers.erase(it);
			break;
		}
	}

	// Remove from lobbies if applicable
	for (auto it = lobbies.begin(); it != lobbies.end(); ++it)
	{
		if (it->second->clientData == clientData)
		{
			LOGM("CRegistryServer: lobby disconnected lobbyId=%s", it->second->lobbyId.c_str());
			delete it->second;
			lobbies.erase(it);
			break;
		}
	}

	// Remove from node agents if applicable
	for (auto it = nodeAgentsByNodeId.begin(); it != nodeAgentsByNodeId.end(); ++it)
	{
		if (it->second->clientData == clientData)
		{
			LOGM("CRegistryServer: node agent disconnected nodeId=%s", it->second->nodeId.c_str());
			CRegistryNodeAgentRecord *record = it->second;
			string disconnectedNodeId = record->nodeId;
			record->status = "offline";
			record->lastHeartbeat = time(NULL);
			record->connectedProcessCount = 0;
			record->clientData = NULL;

			// Fail pending game allocations for this node
			for (auto allocIt = pendingGameAllocations.begin(); allocIt != pendingGameAllocations.end(); )
			{
				if (allocIt->second.nodeId == disconnectedNodeId)
				{
					LOGM("CRegistryServer: failing allocation gameId=%s (node %s disconnected)",
						 allocIt->second.gameId.c_str(), disconnectedNodeId.c_str());
					ReleasePortOnNode(disconnectedNodeId, allocIt->second.allocatedPort);

					// Notify lobby of failure
					if (allocIt->second.lobbyClient)
					{
						json failNotify;
						failNotify["action"] = "registry_allocateGameServerResult";
						failNotify["gameId"] = allocIt->second.gameId;
						failNotify["accepted"] = false;
						failNotify["reason"] = "node_disconnected";
						failNotify["nodeId"] = disconnectedNodeId;
						failNotify["address"] = "";
						failNotify["port"] = 0;
						adminCompletions.push_back({allocIt->second.lobbyClient, failNotify});
					}
					allocIt = pendingGameAllocations.erase(allocIt);
				}
				else
				{
					++allocIt;
				}
			}
			break;
		}
	}

	for (auto it = pendingAdminCommands.begin(); it != pendingAdminCommands.end(); )
	{
		PendingAdminCommand &pending = it->second;

		if (pending.adminClient == clientData)
		{
			it = pendingAdminCommands.erase(it);
			continue;
		}

		for (auto awaitIt = pending.awaitingNodeIds.begin(); awaitIt != pending.awaitingNodeIds.end(); )
		{
			auto nodeRecordIt = nodeAgentsByNodeId.find(*awaitIt);
			if (nodeRecordIt != nodeAgentsByNodeId.end() && nodeRecordIt->second
				&& nodeRecordIt->second->clientData == clientData)
			{
				pending.failedNodes++;
				json nodeResult;
				nodeResult["nodeId"] = *awaitIt;
				nodeResult["success"] = false;
				nodeResult["reason"] = "node_disconnected";
				pending.nodeResults.push_back(nodeResult);
				awaitIt = pending.awaitingNodeIds.erase(awaitIt);
			}
			else
			{
				++awaitIt;
			}
		}

		if (pending.awaitingNodeIds.empty() && pending.adminClient)
		{
			json completion;
			completion["action"] = "registry_adminCommandResult";
			completion["command"] = pending.commandName;
			completion["scope"] = pending.scope;
			completion["accepted"] = (pending.failedNodes == 0);
			completion["reason"] = (pending.failedNodes == 0) ? "ok" : "node_execution_failed";
			completion["commandId"] = pending.commandId;
			completion["targetedNodes"] = pending.targetedNodes;
			completion["dispatchedNodes"] = pending.dispatchedNodes;
			completion["completed"] = true;
			completion["successfulNodes"] = pending.successfulNodes;
			completion["failedNodes"] = pending.failedNodes;
			completion["nodeResults"] = pending.nodeResults;
			if (!pending.targetNodeId.empty())
				completion["nodeId"] = pending.targetNodeId;
			if (!pending.roleName.empty())
				completion["role"] = pending.roleName;
			if (pending.commandName == "stopManagedServices")
			{
				completion["graceful"] = pending.graceful;
				completion["includeRegistry"] = pending.includeRegistry;
			}
			adminCompletions.push_back({pending.adminClient, completion});
			it = pendingAdminCommands.erase(it);
			continue;
		}

		++it;
	}

	// Clean up pending game allocations for disconnected lobby
	for (auto allocIt = pendingGameAllocations.begin(); allocIt != pendingGameAllocations.end(); )
	{
		if (allocIt->second.lobbyClient == clientData)
		{
			LOGM("CRegistryServer: cleaning up pending game allocation gameId=%s (lobby disconnected)",
				 allocIt->second.gameId.c_str());
			ReleasePortOnNode(allocIt->second.nodeId, allocIt->second.allocatedPort);
			allocIt = pendingGameAllocations.erase(allocIt);
		}
		else
		{
			++allocIt;
		}
	}

	clientRoles.erase(clientData);
	adminAuthFailures.erase(clientData);
	mutex->Unlock();

	if (lobbyClientToNotify)
	{
		json j;
		j["action"] = "registry_gameServerDisconnected";
		j["gameId"] = disconnectedGameId;
		j["serverId"] = disconnectedServerId;
		SendJson(lobbyClientToNotify, j);
	}

	for (const auto &completion : adminCompletions)
	{
		SendJson(completion.first, completion.second);
	}
}

void CRegistryServer::NetServerProcessPacket(CNetPacket *packet)
{
	if (packet->packetType != NET_PACKET_TYPE_JSON)
		return;

	CNetGamePacketJson *p = (CNetGamePacketJson *)packet;
	json j = p->jsonPayload;

	string actionForLog = "<missing>";
	if (j.contains("action") && j["action"].is_string())
		actionForLog = j["action"].get<string>();
	LOGD("CRegistryServer::NetServerProcessPacket: action=%s client=%s",
		 actionForLog.c_str(), packet->clientData->clientName.c_str());

	try
	{
		if (!j.contains("action") || !j["action"].is_string())
			return;

		string action = j["action"];

		// Enforce role-based actions (prevents a lobby from sending gs_* and vice versa).
		ClientRole role = ClientRole::UNKNOWN;
		mutex->Lock();
		auto roleIt = clientRoles.find(packet->clientData);
		if (roleIt != clientRoles.end())
			role = roleIt->second;
		mutex->Unlock();

		if (action == "lobby_register")
		{
			if (role != ClientRole::UNKNOWN && role != ClientRole::LOBBY)
			{
				LOGWarning("CRegistryServer: client %s attempted lobby_register with locked role; disconnect", packet->clientData->clientName.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}
		else if (action == "gs_register")
		{
			if (role != ClientRole::UNKNOWN && role != ClientRole::GAME_SERVER)
			{
				LOGWarning("CRegistryServer: client %s attempted gs_register with locked role; disconnect", packet->clientData->clientName.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}
		else if (action == "na_register")
		{
			if (role != ClientRole::UNKNOWN && role != ClientRole::NODE_AGENT)
			{
				LOGWarning("CRegistryServer: client %s attempted na_register with locked role; disconnect", packet->clientData->clientName.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}
		else if (action == "admin_auth")
		{
			if (role != ClientRole::UNKNOWN && role != ClientRole::ADMIN)
			{
				LOGWarning("CRegistryServer: client %s attempted admin_auth with locked role; disconnect", packet->clientData->clientName.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}
		else if (action.rfind("lobby_", 0) == 0)
		{
			if (role != ClientRole::LOBBY)
			{
				LOGWarning("CRegistryServer: client %s attempted %s without LOBBY role; disconnect",
						   packet->clientData->clientName.c_str(), action.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}
		else if (action.rfind("gs_", 0) == 0)
		{
			if (role != ClientRole::GAME_SERVER)
			{
				LOGWarning("CRegistryServer: client %s attempted %s without GAME_SERVER role; disconnect",
						   packet->clientData->clientName.c_str(), action.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}
		else if (action.rfind("na_", 0) == 0 && action != "na_register")
		{
			if (role != ClientRole::NODE_AGENT)
			{
				LOGWarning("CRegistryServer: client %s attempted %s without NODE_AGENT role; disconnect",
						   packet->clientData->clientName.c_str(), action.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}
		else if (action.rfind("admin_", 0) == 0 && action != "admin_auth")
		{
			if (role != ClientRole::ADMIN)
			{
				LOGWarning("CRegistryServer: client %s attempted %s without ADMIN role; disconnect",
						   packet->clientData->clientName.c_str(), action.c_str());
				netServer->Disconnect(packet->clientData);
				return;
			}
		}

		// Game server messages (gs_*)
		if (action == "gs_register")
			HandleGsRegister(packet->clientData, j);
		else if (action == "gs_heartbeat")
			HandleGsHeartbeat(packet->clientData, j);
		else if (action == "gs_playerEvent")
			HandleGsPlayerEvent(packet->clientData, j);
		else if (action == "gs_stateSaved")
			HandleGsStateSaved(packet->clientData, j);
		else if (action == "gs_shutdownComplete")
			HandleGsShutdownComplete(packet->clientData, j);
		// Lobby messages (lobby_*)
		else if (action == "lobby_register")
			HandleLobbyRegister(packet->clientData, j);
		else if (action == "lobby_updateTokens")
			HandleLobbyUpdateTokens(packet->clientData, j);
		else if (action == "lobby_saveState")
			HandleLobbySaveState(packet->clientData, j);
		else if (action == "lobby_shutdown")
			HandleLobbyShutdown(packet->clientData, j);
		else if (action == "lobby_kickPlayer")
			HandleLobbyKickPlayer(packet->clientData, j);
		else if (action == "lobby_queryGameServer")
			HandleLobbyQueryGameServer(packet->clientData, j);
		else if (action == "lobby_allocateGameServer")
			HandleLobbyAllocateGameServer(packet->clientData, j);
		else if (action == "lobby_queryGameStatus")
			HandleLobbyQueryGameStatus(packet->clientData, j);
		// Node Agent messages (na_*)
		else if (action == "na_register")
			HandleNodeAgentRegister(packet->clientData, j);
		else if (action == "na_heartbeat")
			HandleNodeAgentHeartbeat(packet->clientData, j);
		else if (action == "na_commandResult")
			HandleNodeAgentCommandResult(packet->clientData, j);
		else if (action == "na_gameProcessSpawned")
			HandleNodeAgentGameProcessSpawned(packet->clientData, j);
		else if (action == "na_crashReport")
			HandleNodeAgentCrashReport(packet->clientData, j);
		// Admin messages (admin_*)
		else if (action == "admin_auth")
			HandleAdminAuth(packet->clientData, j);
		else if (action == "admin_getClusterState")
			HandleAdminGetClusterState(packet->clientData, j);
		else if (action == "admin_startManagedServices")
			HandleAdminStartManagedServices(packet->clientData, j);
		else if (action == "admin_stopManagedServices")
			HandleAdminStopManagedServices(packet->clientData, j);
		else if (action == "admin_maintenanceNode")
			HandleAdminMaintenanceNode(packet->clientData, j);
		else if (action == "admin_gracefulShutdown")
			HandleAdminGracefulShutdown(packet->clientData, j);
		else
			LOGWarning("CRegistryServer: unknown action=%s", action.c_str());
	}
	catch (exception &ex)
	{
		LOGError("CRegistryServer::NetServerProcessPacket: %s", ex.what());
	}
}

// --- Lobby message handlers ---

void CRegistryServer::HandleLobbyRegister(CNetClientData *clientData, json &j)
{
	string lobbyId = j.value("lobbyId", "");
	if (lobbyId.empty())
	{
		LOGWarning("CRegistryServer::HandleLobbyRegister: missing lobbyId from %s", clientData->clientName.c_str());
		return;
	}

	LOGM("CRegistryServer::HandleLobbyRegister: lobbyId=%s", lobbyId.c_str());

	mutex->Lock();
	if (!AssignRoleLocked(clientData, ClientRole::LOBBY, "lobby_register"))
	{
		mutex->Unlock();
		netServer->Disconnect(clientData);
		return;
	}

	for (auto it = lobbies.begin(); it != lobbies.end(); )
	{
		if (it->second->clientData == clientData)
		{
			delete it->second;
			it = lobbies.erase(it);
		}
		else
		{
			++it;
		}
	}

	// Remove old record if exists
	auto old = lobbies.find(lobbyId);
	if (old != lobbies.end())
	{
		delete old->second;
		lobbies.erase(old);
	}

	CRegistryLobbyRecord *record = new CRegistryLobbyRecord();
	record->lobbyId = lobbyId;
	record->clientData = clientData;
	lobbies[lobbyId] = record;
	mutex->Unlock();

	// Send ack
	json jAck;
	jAck["action"] = "registry_lobbyRegistered";
	jAck["lobbyId"] = lobbyId;
	SendJson(clientData, jAck);
}

void CRegistryServer::HandleLobbyUpdateTokens(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");

	LOGD("CRegistryServer::HandleLobbyUpdateTokens: gameId=%s", gameId.c_str());

	mutex->Lock();

	// Always store as pending — ensures tokens survive if the game server connection is stale
	// (e.g. process was killed but ENet hasn't detected the disconnect yet).
	// When a new game server registers with the same gameId, it will get these tokens.
	PendingTokens pt;
	if (j.contains("tokens") && j["tokens"].is_object())
	{
		for (auto &el : j["tokens"].items())
			pt.tokens[el.key()] = el.value().get<string>();
	}
	if (j.contains("nameToId") && j["nameToId"].is_object())
	{
		for (auto &el : j["nameToId"].items())
			pt.nameToId[el.key()] = el.value().get<int>();
	}
	pt.tokenCreationTime = (time_t)j.value("tokenCreationTime", (int64_t)0);
	pendingTokens[gameId] = pt;

	// Also forward directly if game server is connected
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json fwd;
		fwd["action"] = "lobby_updateTokens";
		fwd["gameId"] = gameId;
		fwd["tokens"] = j["tokens"];
		if (j.contains("nameToId"))
			fwd["nameToId"] = j["nameToId"];
		fwd["tokenCreationTime"] = j.value("tokenCreationTime", (int64_t)0);
		SendJson(it->second->clientData, fwd);
	}
	else
	{
		LOGD("CRegistryServer: gameId=%s not registered, tokens stored as pending", gameId.c_str());
	}

	mutex->Unlock();
}

void CRegistryServer::HandleLobbySaveState(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json fwd;
		fwd["action"] = "lobby_saveState";
		fwd["gameId"] = gameId;
		SendJson(it->second->clientData, fwd);
	}
	mutex->Unlock();
}

void CRegistryServer::HandleLobbyShutdown(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	string reason = j.value("reason", "");

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json fwd;
		fwd["action"] = "lobby_shutdown";
		fwd["gameId"] = gameId;
		fwd["reason"] = reason;
		SendJson(it->second->clientData, fwd);
	}
	mutex->Unlock();
}

void CRegistryServer::HandleLobbyKickPlayer(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json fwd;
		fwd["action"] = "lobby_kickPlayer";
		fwd["gameId"] = gameId;
		fwd["playerName"] = j.value("playerName", "");
		fwd["reason"] = j.value("reason", "");
		SendJson(it->second->clientData, fwd);
	}
	mutex->Unlock();
}

void CRegistryServer::HandleLobbyQueryGameServer(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");

	mutex->Lock();
	json resp;
	resp["action"] = "registry_gameServerInfo";
	resp["gameId"] = gameId;

	auto it = gameServers.find(gameId);
	if (it != gameServers.end())
	{
		resp["found"] = true;
		resp["serverId"] = it->second->serverId;
		resp["listenAddress"] = it->second->listenAddress;
		resp["listenPort"] = it->second->listenPort;
		resp["connectedPlayers"] = it->second->connectedPlayers;
		resp["status"] = it->second->status;
		resp["isReachable"] = ((it->second->status == "running" || it->second->status == "warning") &&
							   difftime(time(NULL), it->second->lastHeartbeat) < heartbeatTimeoutSeconds);
	}
	else
	{
		resp["found"] = false;
	}
	mutex->Unlock();

	SendJson(clientData, resp);
}

// --- Game server message handlers ---

void CRegistryServer::HandleGsRegister(CNetClientData *clientData, json &j)
{
	string serverId = j.value("serverId", "");
	string gameId = j.value("gameId", "");
	int listenPort = j.value("listenPort", 0);
	string listenAddress = j.value("listenAddress", "localhost");
	if (serverId.empty() || gameId.empty() || listenPort <= 0)
	{
		LOGWarning("CRegistryServer::HandleGsRegister: invalid registration from %s", clientData->clientName.c_str());
		return;
	}

	LOGM("CRegistryServer::HandleGsRegister: serverId=%s gameId=%s port=%d",
		 serverId.c_str(), gameId.c_str(), listenPort);

	CNetClientData *lobbyClient = NULL;

	mutex->Lock();
	if (!AssignRoleLocked(clientData, ClientRole::GAME_SERVER, "gs_register"))
	{
		mutex->Unlock();
		netServer->Disconnect(clientData);
		return;
	}

	for (auto it = gameServers.begin(); it != gameServers.end(); )
	{
		if (it->second->clientData == clientData)
		{
			delete it->second;
			it = gameServers.erase(it);
		}
		else
		{
			++it;
		}
	}

	// Remove old record for this gameId if exists
	auto old = gameServers.find(gameId);
	if (old != gameServers.end())
	{
		delete old->second;
		gameServers.erase(old);
	}

	CRegistryGameServerRecord *record = new CRegistryGameServerRecord();
	record->serverId = serverId;
	record->gameId = gameId;
	record->listenAddress = listenAddress;
	record->listenPort = listenPort;
	record->connectedPlayers = 0;
	record->status = "running";
	record->lastHeartbeat = time(NULL);
	record->clientData = clientData;

	gameServers[gameId] = record;

	// Track running game count per node (for drain monitoring)
	if (!record->nodeId.empty())
	{
		auto *nodeRecord = GetNodeAgentRecord(record->nodeId);
		if (nodeRecord)
			nodeRecord->runningGameCount++;
	}

	// Send registration ack
	json jAck;
	jAck["action"] = "lobby_registered";
	jAck["serverId"] = serverId;
	jAck["gameId"] = gameId;
	SendJson(clientData, jAck);

	// Check for pending tokens
	auto pit = pendingTokens.find(gameId);
	if (pit != pendingTokens.end())
	{
		LOGM("CRegistryServer: pushing pending tokens for gameId=%s", gameId.c_str());

		json jTokens;
		jTokens["action"] = "lobby_updateTokens";
		jTokens["gameId"] = gameId;
		json tokensJson = json::object();
		for (const auto &pair : pit->second.tokens)
			tokensJson[pair.first] = pair.second;
		jTokens["tokens"] = tokensJson;
		if (!pit->second.nameToId.empty())
		{
			json nameToIdJson = json::object();
			for (const auto &pair : pit->second.nameToId)
				nameToIdJson[pair.first] = pair.second;
			jTokens["nameToId"] = nameToIdJson;
		}
		jTokens["tokenCreationTime"] = (int64_t)pit->second.tokenCreationTime;
		SendJson(clientData, jTokens);

		pendingTokens.erase(pit);
	}

	// Check if this registration completes a pending game allocation
	CNetClientData *allocationLobbyClient = NULL;
	string allocationNodeId;
	auto allocIt = pendingGameAllocations.find(gameId);
	if (allocIt != pendingGameAllocations.end())
	{
		allocationLobbyClient = allocIt->second.lobbyClient;
		allocationNodeId = allocIt->second.nodeId;
		// Store nodeId on game server record for port release on disconnect
		record->nodeId = allocIt->second.nodeId;
		pendingGameAllocations.erase(allocIt);
		LOGM("CRegistryServer::HandleGsRegister: completing pending allocation for gameId=%s", gameId.c_str());
	}

	lobbyClient = FindLobbyForNotificationLocked();
	mutex->Unlock();

	// Complete the allocation by notifying the lobby
	if (allocationLobbyClient)
	{
		json allocResponse;
		allocResponse["action"] = "registry_allocateGameServerResult";
		allocResponse["gameId"] = gameId;
		allocResponse["accepted"] = true;
		allocResponse["reason"] = "ok";
		allocResponse["nodeId"] = allocationNodeId;
		allocResponse["address"] = listenAddress;
		allocResponse["port"] = listenPort;
		SendJson(allocationLobbyClient, allocResponse);
	}

	// Also send the regular notification (used by both REMOTE_NODE and SEPARATE_PROCESS modes)
	if (lobbyClient)
	{
		json jNotify;
		jNotify["action"] = "registry_gameServerRegistered";
		jNotify["gameId"] = gameId;
		jNotify["serverId"] = serverId;
		jNotify["listenAddress"] = listenAddress;
		jNotify["listenPort"] = listenPort;
		SendJson(lobbyClient, jNotify);
	}
}

void CRegistryServer::HandleGsHeartbeat(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData == clientData)
	{
		// Recovery: if server was in warning state and heartbeat arrives, transition back to running
		if (it->second->status == "warning")
		{
			LOGM("CRegistryServer: heartbeat recovered for gameId=%s (was in warning state, back to running)",
				 gameId.c_str());
		}

		it->second->lastHeartbeat = time(NULL);
		it->second->connectedPlayers = j.value("connectedPlayers", 0);
		it->second->status = j.value("status", "running");
	}
	else if (it != gameServers.end())
	{
		LOGWarning("CRegistryServer::HandleGsHeartbeat: rejecting heartbeat for gameId=%s from non-owner client=%s",
				   gameId.c_str(), clientData->clientName.c_str());
	}
	mutex->Unlock();
}

void CRegistryServer::HandleGsPlayerEvent(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	string playerName = j.value("playerName", "");
	string event = j.value("event", "");

	LOGM("CRegistryServer: player event gameId=%s player=%s event=%s",
		 gameId.c_str(), playerName.c_str(), event.c_str());

	CNetClientData *lobbyClient = NULL;

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData == clientData)
	{
		it->second->lastHeartbeat = time(NULL);
		lobbyClient = FindLobbyForNotificationLocked();
	}
	else if (it != gameServers.end())
	{
		LOGWarning("CRegistryServer::HandleGsPlayerEvent: rejecting playerEvent for gameId=%s from non-owner client=%s",
				   gameId.c_str(), clientData->clientName.c_str());
	}
	mutex->Unlock();

	// Forward to the lobby
	if (lobbyClient)
	{
		json fwd;
		fwd["action"] = "registry_playerEvent";
		fwd["gameId"] = gameId;
		fwd["playerName"] = playerName;
		fwd["event"] = event;
		SendJson(lobbyClient, fwd);
	}
}

void CRegistryServer::HandleGsStateSaved(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	LOGM("CRegistryServer: state saved for gameId=%s", gameId.c_str());
	CNetClientData *lobbyClient = NULL;

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData == clientData)
		lobbyClient = FindLobbyForNotificationLocked();
	else if (it != gameServers.end())
		LOGWarning("CRegistryServer::HandleGsStateSaved: rejecting stateSaved for gameId=%s from non-owner client=%s",
				   gameId.c_str(), clientData->clientName.c_str());
	mutex->Unlock();

	// Forward to lobby
	if (lobbyClient)
	{
		json fwd;
		fwd["action"] = "registry_stateSaved";
		fwd["gameId"] = gameId;
		SendJson(lobbyClient, fwd);
	}
}

void CRegistryServer::HandleGsShutdownComplete(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	LOGM("CRegistryServer: shutdown complete for gameId=%s", gameId.c_str());
	CNetClientData *lobbyClient = NULL;

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData == clientData)
	{
		it->second->status = "shutdown";
		lobbyClient = FindLobbyForNotificationLocked();
	}
	else if (it != gameServers.end())
	{
		LOGWarning("CRegistryServer::HandleGsShutdownComplete: rejecting shutdownComplete for gameId=%s from non-owner client=%s",
				   gameId.c_str(), clientData->clientName.c_str());
	}
	mutex->Unlock();

	// Forward to lobby
	if (lobbyClient)
	{
		json fwd;
		fwd["action"] = "registry_shutdownComplete";
		fwd["gameId"] = gameId;
		SendJson(lobbyClient, fwd);
	}
}

// --- Utility ---

void CRegistryServer::CheckHeartbeatTimeouts()
{
	vector<pair<string, string>> unreachableGames;

	mutex->Lock();
	time_t now = time(NULL);

	for (auto &pair : gameServers)
	{
		CRegistryGameServerRecord *record = pair.second;
		double elapsed = difftime(now, record->lastHeartbeat);

		// Skip already-terminal states
		if (record->status == "shutdown" || record->status == "unreachable")
			continue;

		// Tier 2: Dead threshold — mark unreachable
		if (elapsed > heartbeatTimeoutSeconds)
		{
			LOGM("CRegistryServer: heartbeat timeout for gameId=%s (last heartbeat %ds ago)",
				 record->gameId.c_str(), (int)elapsed);
			record->status = "unreachable";
			unreachableGames.push_back({record->gameId, record->serverId});
		}
		// Tier 1: Warning threshold — trigger proactive save
		else if (elapsed > heartbeatWarningSeconds && record->status == "running")
		{
			LOGM("CRegistryServer: heartbeat warning for gameId=%s (last heartbeat %ds ago, warning threshold=%ds), sending proactive save",
				 record->gameId.c_str(), (int)elapsed, heartbeatWarningSeconds);
			record->status = "warning";

			// Send save command directly to the game server
			if (record->clientData)
			{
				json jSave;
				jSave["action"] = "lobby_saveState";
				jSave["gameId"] = record->gameId;
				jSave["reason"] = "heartbeat_warning";
				SendJson(record->clientData, jSave);
			}
		}
	}

	for (auto &pair : nodeAgentsByNodeId)
	{
		CRegistryNodeAgentRecord *record = pair.second;
		if (!record)
			continue;
		if (record->status != "offline" && record->status != "unreachable"
			&& record->clientData != NULL
			&& difftime(now, record->lastHeartbeat) > heartbeatTimeoutSeconds)
		{
			LOGM("CRegistryServer: node heartbeat timeout nodeId=%s (last heartbeat %ds ago)",
				 record->nodeId.c_str(), (int)difftime(now, record->lastHeartbeat));
			record->status = "unreachable";
			record->connectedProcessCount = 0;
		}
	}

	vector<pair<CNetClientData *, json>> adminCompletions;
	for (auto it = pendingAdminCommands.begin(); it != pendingAdminCommands.end(); )
	{
		PendingAdminCommand &pending = it->second;
		if (difftime(now, pending.createdAt) > adminCommandTimeoutSeconds)
		{
			for (const string &nodeId : pending.awaitingNodeIds)
			{
				pending.failedNodes++;
				json nodeResult;
				nodeResult["nodeId"] = nodeId;
				nodeResult["success"] = false;
				nodeResult["reason"] = "node_command_timeout";
				pending.nodeResults.push_back(nodeResult);
			}
			pending.awaitingNodeIds.clear();

			if (pending.adminClient)
			{
				json completion;
				completion["action"] = "registry_adminCommandResult";
				completion["command"] = pending.commandName;
				completion["scope"] = pending.scope;
				completion["accepted"] = false;
				completion["reason"] = "node_command_timeout";
				completion["commandId"] = pending.commandId;
				completion["targetedNodes"] = pending.targetedNodes;
				completion["dispatchedNodes"] = pending.dispatchedNodes;
				completion["completed"] = true;
				completion["successfulNodes"] = pending.successfulNodes;
				completion["failedNodes"] = pending.failedNodes;
				completion["nodeResults"] = pending.nodeResults;
				if (!pending.targetNodeId.empty())
					completion["nodeId"] = pending.targetNodeId;
				if (!pending.roleName.empty())
					completion["role"] = pending.roleName;
				if (pending.commandName == "stopManagedServices")
				{
					completion["graceful"] = pending.graceful;
					completion["includeRegistry"] = pending.includeRegistry;
				}
				adminCompletions.push_back({pending.adminClient, completion});
			}

			it = pendingAdminCommands.erase(it);
			continue;
		}

		++it;
	}

	// Check game allocation timeouts
	vector<pair<CNetClientData *, json>> allocationTimeouts;
	for (auto allocIt = pendingGameAllocations.begin(); allocIt != pendingGameAllocations.end(); )
	{
		if (difftime(now, allocIt->second.createdAt) > GAME_ALLOCATION_TIMEOUT_SECONDS)
		{
			LOGM("CRegistryServer: game allocation timeout for gameId=%s on node=%s",
				 allocIt->second.gameId.c_str(), allocIt->second.nodeId.c_str());

			ReleasePortOnNode(allocIt->second.nodeId, allocIt->second.allocatedPort);

			if (allocIt->second.lobbyClient)
			{
				json timeoutNotify;
				timeoutNotify["action"] = "registry_allocateGameServerResult";
				timeoutNotify["gameId"] = allocIt->second.gameId;
				timeoutNotify["accepted"] = false;
				timeoutNotify["reason"] = "allocation_timeout";
				timeoutNotify["nodeId"] = allocIt->second.nodeId;
				timeoutNotify["address"] = "";
				timeoutNotify["port"] = 0;
				allocationTimeouts.push_back({allocIt->second.lobbyClient, timeoutNotify});
			}
			allocIt = pendingGameAllocations.erase(allocIt);
		}
		else
		{
			++allocIt;
		}
	}

	CNetClientData *lobbyClient = FindLobbyForNotificationLocked();
	mutex->Unlock();

	if (lobbyClient)
	{
		for (const auto &entry : unreachableGames)
		{
			json j;
			j["action"] = "registry_gameServerUnreachable";
			j["gameId"] = entry.first;
			j["serverId"] = entry.second;
			SendJson(lobbyClient, j);
		}
	}

	for (const auto &completion : adminCompletions)
	{
		SendJson(completion.first, completion.second);
	}

	for (const auto &timeout : allocationTimeouts)
	{
		SendJson(timeout.first, timeout.second);
	}

	// Advance graceful shutdown sequence if active
	if (shutdownPhase != ShutdownPhase::NONE)
		AdvanceShutdownSequence();
}

bool CRegistryServer::GetGameServerRecordCopy(const string &gameId, CRegistryGameServerRecord &outRecord) const
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it == gameServers.end() || it->second == NULL)
	{
		mutex->Unlock();
		return false;
	}
	outRecord = *(it->second);
	mutex->Unlock();
	return true;
}

bool CRegistryServer::IsGameServerReady(const string &gameId)
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	bool ready = (it != gameServers.end()
				  && (it->second->status == "running" || it->second->status == "warning")
				  && difftime(time(NULL), it->second->lastHeartbeat) < heartbeatTimeoutSeconds);
	mutex->Unlock();
	return ready;
}

CNetClientData *CRegistryServer::FindLobbyForNotificationLocked()
{
	// For now: return the first connected lobby (single-lobby scenario)
	// In multi-lobby future: route based on gameId ownership
	for (auto &pair : lobbies)
	{
		if (pair.second->clientData)
			return pair.second->clientData;
	}
	return NULL;
}

void CRegistryServer::SendJson(CNetClientData *clientData, json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netServer->IssuePacket(clientData, packet);
	delete packet;
}

bool CRegistryServer::AssignRoleLocked(CNetClientData *clientData, ClientRole role, const char *action)
{
	auto it = clientRoles.find(clientData);
	if (it == clientRoles.end())
	{
		clientRoles[clientData] = role;
		return true;
	}

	if (it->second == ClientRole::UNKNOWN)
	{
		it->second = role;
		return true;
	}

	if (it->second == role)
		return true;

	LOGWarning("CRegistryServer::AssignRoleLocked: role switch blocked for client=%s action=%s",
			   clientData->clientName.c_str(), action ? action : "<unknown>");
	return false;
}

// ========== Phase 0: Node Agent and Admin Methods ==========

void CRegistryServer::SetAdminSecret(const string &adminSecret)
{
	mutex->Lock();
	if (adminSecret.empty())
	{
		adminSecretHash.clear();
		mutex->Unlock();
		return;
	}
	// Use HMAC-SHA256 with a domain separator to produce the hash
	const char *domainKey = "LightHeroes.AdminSecret.v1";
	auto digest = SYS_HmacSha256(
		(const uint8_t *)domainKey, strlen(domainKey),
		(const uint8_t *)adminSecret.data(), adminSecret.size());
	adminSecretHash = vector<u8>(digest.begin(), digest.end());
	mutex->Unlock();
}

bool CRegistryServer::LoadClusterConfig(const string &configPath)
{
	CClusterConfig loaded;
	if (!loaded.LoadFromFile(configPath))
	{
		LOGError("CRegistryServer::LoadClusterConfig: failed to parse config='%s'", configPath.c_str());
		return false;
	}

	mutex->Lock();
	clusterConfig = loaded;
	clusterConfigPath = configPath;
	if (!loaded.GetRegistry().address.empty())
		registryExternalAddress = loaded.GetRegistry().address;
	hasClusterConfig = true;
	adminCommandTimeoutSeconds = loaded.GetOperations().adminCommandTimeoutSec;
	adminAuthMaxAttempts = loaded.GetOperations().adminAuthMaxAttempts;
	heartbeatTimeoutSeconds = loaded.GetGameServer().heartbeatTimeoutSeconds;
	heartbeatWarningSeconds = loaded.GetGameServer().heartbeatWarningSeconds;
	gamePortRangeStart = loaded.GetGameServer().portRangeStart;
	gamePortRangeEnd = loaded.GetGameServer().portRangeEnd;
	mutex->Unlock();

	LOGM("CRegistryServer::LoadClusterConfig: loaded config='%s' (adminCommandTimeout=%ds, adminAuthMaxAttempts=%d, heartbeatTimeout=%ds, heartbeatWarning=%ds, portRange=%d-%d)",
		 configPath.c_str(), adminCommandTimeoutSeconds, adminAuthMaxAttempts,
		 heartbeatTimeoutSeconds, heartbeatWarningSeconds, gamePortRangeStart, gamePortRangeEnd);
	return true;
}

json CRegistryServer::GetClusterState() const
{
	mutex->Lock();
	json state;
	state["timestamp"] = (int64_t)time(NULL);
	state["configLoaded"] = hasClusterConfig;
	if (hasClusterConfig)
	{
		state["configPath"] = clusterConfigPath;
		state["clusterConfig"] = clusterConfig.ToJson();
	}

	// Node agents
	json nodesJson = json::array();
	for (const auto &record : nodeAgents)
	{
		json nodeJson;
		nodeJson["nodeId"] = record->nodeId;
		nodeJson["nodeIp"] = record->nodeIp;
		nodeJson["status"] = record->status;
		nodeJson["maintenanceMode"] = record->maintenanceMode;
		nodeJson["runningGameCount"] = record->runningGameCount;
		nodeJson["connectedProcessCount"] = record->connectedProcessCount;
		nodeJson["maxProcessCapacity"] = record->maxProcessCapacity;
		nodeJson["lastHeartbeat"] = (int64_t)record->lastHeartbeat;
		if (!record->nodeMetrics.is_null())
			nodeJson["metrics"] = record->nodeMetrics;
		// Include configured roles from cluster config
		if (hasClusterConfig)
		{
			const SNodeConfig *nodeConfig = clusterConfig.GetNode(record->nodeId);
			if (nodeConfig)
				nodeJson["roles"] = nodeConfig->roles;
		}
		nodesJson.push_back(nodeJson);
	}
	state["nodeAgents"] = nodesJson;

	// Game servers
	json gamesJson = json::array();
	for (const auto &pair : gameServers)
	{
		json gameJson;
		gameJson["gameId"] = pair.second->gameId;
		gameJson["serverId"] = pair.second->serverId;
		gameJson["status"] = pair.second->status;
		gameJson["connectedPlayers"] = pair.second->connectedPlayers;
		gameJson["listenAddress"] = pair.second->listenAddress;
		gameJson["listenPort"] = pair.second->listenPort;
		gameJson["nodeId"] = pair.second->nodeId;
		// Session status: higher-level concept (ACTIVE/PAUSED/LOST)
		string status = pair.second->status;
		if (status == "running") gameJson["sessionStatus"] = "ACTIVE";
		else if (status == "shutdown") gameJson["sessionStatus"] = "PAUSED";
		else if (status == "unreachable") gameJson["sessionStatus"] = "LOST";
		else gameJson["sessionStatus"] = "UNKNOWN";
		gamesJson.push_back(gameJson);
	}
	state["gameServers"] = gamesJson;

	// Lobbies
	json lobbiesJson = json::array();
	for (const auto &pair : lobbies)
	{
		json lobbyJson;
		lobbyJson["lobbyId"] = pair.second->lobbyId;
		lobbiesJson.push_back(lobbyJson);
	}
	state["lobbies"] = lobbiesJson;

	// Recent crashes (last N, oldest first)
	json crashesJson = json::array();
	int count = (int)crashHistory.size() < CRASH_HISTORY_IN_STATE
				? (int)crashHistory.size() : CRASH_HISTORY_IN_STATE;
	for (auto it = crashHistory.end() - count; it != crashHistory.end(); ++it)
	{
		const auto &cr = *it;
		json cj;
		cj["nodeId"] = cr.nodeId;
		cj["roleName"] = cr.roleName;
		cj["exitCode"] = cr.exitCode;
		cj["timestamp"] = (int64_t)cr.timestamp;
		cj["seq"] = cr.seq;
		if (!cr.gameId.empty())
			cj["gameId"] = cr.gameId;
		crashesJson.push_back(cj);
	}
	state["recentCrashes"] = crashesJson;

	// Shutdown state
	if (shutdownPhase != ShutdownPhase::NONE)
	{
		json shutdownJson;
		const char *phaseName = "unknown";
		switch (shutdownPhase)
		{
			case ShutdownPhase::STOP_NEW_GAMES: phaseName = "stop_new_games"; break;
			case ShutdownPhase::DRAIN_ACTIVE: phaseName = "drain_active"; break;
			case ShutdownPhase::STOP_GAME_SERVERS: phaseName = "stop_game_servers"; break;
			case ShutdownPhase::STOP_INFRASTRUCTURE: phaseName = "stop_infrastructure"; break;
			case ShutdownPhase::STOP_REGISTRY: phaseName = "stop_registry"; break;
			default: break;
		}
		shutdownJson["phase"] = phaseName;
		shutdownJson["startedAt"] = (int64_t)shutdownStartedAt;
		shutdownJson["drainTimeoutSec"] = shutdownDrainTimeoutSeconds;
		shutdownJson["forceDrain"] = shutdownForceDrain;
		state["shutdownInProgress"] = shutdownJson;
	}

	mutex->Unlock();
	return state;
}

CRegistryNodeAgentRecord *CRegistryServer::GetNodeAgentRecord(const string &nodeId)
{
	// CALLER MUST HOLD mutex
	auto it = nodeAgentsByNodeId.find(nodeId);
	if (it != nodeAgentsByNodeId.end())
		return it->second;
	return NULL;
}

void CRegistryServer::HandleNodeAgentRegister(CNetClientData *clientData, json &j)
{
	// { "action": "na_register", "nodeId": "vps-1", "nodeIp": "10.0.0.1", "agentPort": 14652, "maxProcessCapacity": 25 }
	string nodeId = j.value("nodeId", "");
	string nodeIp = j.value("nodeIp", "");
	int agentPort = j.value("agentPort", 0);
	int maxCapacity = j.value("maxProcessCapacity", 25);

	if (nodeId.empty() || nodeIp.empty() || agentPort <= 0)
	{
		LOGWarning("CRegistryServer::HandleNodeAgentRegister: invalid registration from %s",
				   clientData->clientName.c_str());
		return;
	}

	mutex->Lock();
	if (!AssignRoleLocked(clientData, ClientRole::NODE_AGENT, "na_register"))
	{
		mutex->Unlock();
		netServer->Disconnect(clientData);
		return;
	}

	// Update old record for this nodeId if it exists (re-registration).
	auto oldIt = nodeAgentsByNodeId.find(nodeId);
	if (oldIt != nodeAgentsByNodeId.end())
	{
		CRegistryNodeAgentRecord *record = oldIt->second;
		if (record->clientData && record->clientData != clientData)
		{
			mutex->Unlock();
			LOGWarning("CRegistryServer::HandleNodeAgentRegister: rejecting nodeId takeover nodeId=%s existingClient=%s newClient=%s",
				nodeId.c_str(), record->clientData->clientName.c_str(), clientData->clientName.c_str());
			netServer->Disconnect(clientData);
			return;
		}
		record->nodeIp = nodeIp;
		record->agentPort = agentPort;
		record->status = "online";
		record->lastHeartbeat = time(NULL);
		record->clientData = clientData;
		record->connectedProcessCount = 0;
		record->maxProcessCapacity = maxCapacity;

		mutex->Unlock();

		LOGM("CRegistryServer: node agent re-registered nodeId=%s ip=%s port=%d maxCapacity=%d",
			 nodeId.c_str(), nodeIp.c_str(), agentPort, maxCapacity);

		json response;
		response["action"] = "registry_naRegistered";
		response["nodeId"] = nodeId;
		SendJson(clientData, response);
		return;
	}

	CRegistryNodeAgentRecord *record = new CRegistryNodeAgentRecord();
	record->nodeId = nodeId;
	record->nodeIp = nodeIp;
	record->agentPort = agentPort;
	record->status = "online";
	record->lastHeartbeat = time(NULL);
	record->clientData = clientData;
	record->connectedProcessCount = 0;
	record->maxProcessCapacity = maxCapacity;

	nodeAgentsByNodeId[nodeId] = record;
	nodeAgents.push_back(record);

	mutex->Unlock();

	LOGM("CRegistryServer: node agent registered nodeId=%s ip=%s port=%d maxCapacity=%d",
		 nodeId.c_str(), nodeIp.c_str(), agentPort, maxCapacity);

	// Send acknowledgment
	json response;
	response["action"] = "registry_naRegistered";
	response["nodeId"] = nodeId;
	SendJson(clientData, response);
}

void CRegistryServer::HandleNodeAgentHeartbeat(CNetClientData *clientData, json &j)
{
	// { "action": "na_heartbeat", "nodeId": "vps-1", "processCount": 3, "metrics": {...} }
	string nodeId = j.value("nodeId", "");

	mutex->Lock();
	CRegistryNodeAgentRecord *record = GetNodeAgentRecord(nodeId);
	if (record && record->clientData == clientData)
	{
		record->lastHeartbeat = time(NULL);
		record->connectedProcessCount = j.value("processCount", 0);
		record->status = "online";
		if (j.contains("metrics"))
			record->nodeMetrics = j["metrics"];
	}
	mutex->Unlock();
}

void CRegistryServer::HandleNodeAgentCommandResult(CNetClientData *clientData, json &j)
{
	uint64_t commandId = j.value("commandId", (uint64_t)0);
	string nodeId = j.value("nodeId", "");
	bool success = j.value("success", false);
	string reason = j.value("reason", success ? "ok" : "failed");

	if (commandId == 0 || nodeId.empty())
		return;

	json completion;
	CNetClientData *adminClient = NULL;
	bool sendCompletion = false;

	mutex->Lock();
	CRegistryNodeAgentRecord *record = GetNodeAgentRecord(nodeId);
	if (!record || record->clientData != clientData)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleNodeAgentCommandResult: rejecting command result commandId=%llu nodeId=%s from non-owner client=%s",
			(unsigned long long)commandId, nodeId.c_str(), clientData->clientName.c_str());
		return;
	}

	auto pendingIt = pendingAdminCommands.find(commandId);
	if (pendingIt == pendingAdminCommands.end())
	{
		mutex->Unlock();
		LOGD("CRegistryServer::HandleNodeAgentCommandResult: no pending command commandId=%llu", (unsigned long long)commandId);
		return;
	}

	PendingAdminCommand &pending = pendingIt->second;
	auto awaitIt = pending.awaitingNodeIds.find(nodeId);
	if (awaitIt == pending.awaitingNodeIds.end())
	{
		mutex->Unlock();
		LOGD("CRegistryServer::HandleNodeAgentCommandResult: duplicate or non-target command result commandId=%llu nodeId=%s",
			(unsigned long long)commandId, nodeId.c_str());
		return;
	}

	pending.awaitingNodeIds.erase(awaitIt);
	if (success)
		pending.successfulNodes++;
	else
		pending.failedNodes++;

	json nodeResult;
	nodeResult["nodeId"] = nodeId;
	nodeResult["success"] = success;
	nodeResult["reason"] = reason;
	pending.nodeResults.push_back(nodeResult);

	if (pending.awaitingNodeIds.empty() && pending.adminClient)
	{
		completion["action"] = "registry_adminCommandResult";
		completion["command"] = pending.commandName;
		completion["scope"] = pending.scope;
		completion["accepted"] = (pending.failedNodes == 0);
		completion["reason"] = (pending.failedNodes == 0) ? "ok" : "node_execution_failed";
		completion["commandId"] = pending.commandId;
		completion["targetedNodes"] = pending.targetedNodes;
		completion["dispatchedNodes"] = pending.dispatchedNodes;
		completion["completed"] = true;
		completion["successfulNodes"] = pending.successfulNodes;
		completion["failedNodes"] = pending.failedNodes;
		completion["nodeResults"] = pending.nodeResults;
		if (!pending.targetNodeId.empty())
			completion["nodeId"] = pending.targetNodeId;
		if (!pending.roleName.empty())
			completion["role"] = pending.roleName;
		if (pending.commandName == "stopManagedServices")
		{
			completion["graceful"] = pending.graceful;
			completion["includeRegistry"] = pending.includeRegistry;
		}
		adminClient = pending.adminClient;
		sendCompletion = true;
		pendingAdminCommands.erase(pendingIt);
	}

	mutex->Unlock();

	if (sendCompletion)
		SendJson(adminClient, completion);
}

void CRegistryServer::HandleNodeAgentCrashReport(CNetClientData *clientData, json &j)
{
	string nodeId = j.value("nodeId", "");
	string roleName = j.value("roleName", "");
	int exitCode = j.value("exitCode", -1);
	string gameId = j.value("gameId", "");

	if (nodeId.empty() || roleName.empty())
		return;

	CNetClientData *lobbyToNotify = NULL;
	string crashedGameId;

	mutex->Lock();
	CRegistryNodeAgentRecord *nodeRecord = GetNodeAgentRecord(nodeId);
	if (!nodeRecord || nodeRecord->clientData != clientData)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleNodeAgentCrashReport: rejecting from non-owner client=%s nodeId=%s",
			clientData->clientName.c_str(), nodeId.c_str());
		return;
	}

	LOGM("CRegistryServer: CRASH REPORT from node=%s role=%s exitCode=%d gameId=%s",
		 nodeId.c_str(), roleName.c_str(), exitCode, gameId.c_str());

	// If this is a game process crash, update game server record
	if (!gameId.empty())
	{
		auto gsIt = gameServers.find(gameId);
		if (gsIt != gameServers.end())
		{
			CRegistryGameServerRecord *gsRecord = gsIt->second;
			gsRecord->status = "crashed";
			crashedGameId = gameId;
			lobbyToNotify = FindLobbyForNotificationLocked();
		}

		// Decrement running game count on the node
		if (nodeRecord->runningGameCount > 0)
			nodeRecord->runningGameCount--;
	}

	mutex->Unlock();

	// Note: between mutex->Unlock() above and RecordCrashReport() below,
	// GetClusterState() may briefly see status="crashed" on the game record
	// without a corresponding crashHistory entry. The next poll resolves this.
	RecordCrashReport(nodeId, roleName, exitCode, gameId);

	// Notify lobby about the crash so it can track game state
	if (lobbyToNotify && !crashedGameId.empty())
	{
		json notify;
		notify["action"] = "registry_gameServerCrashed";
		notify["gameId"] = crashedGameId;
		notify["nodeId"] = nodeId;
		notify["exitCode"] = exitCode;
		SendJson(lobbyToNotify, notify);
	}
}

void CRegistryServer::RecordCrashReport(const string &nodeId, const string &roleName,
										int exitCode, const string &gameId)
{
	mutex->Lock();
	crashSeqCounter++;
	crashHistory.push_back(SCrashReport(nodeId, roleName, exitCode, gameId, time(NULL), crashSeqCounter));
	if ((int)crashHistory.size() > MAX_CRASH_HISTORY)
		crashHistory.pop_front();
	mutex->Unlock();
}

void CRegistryServer::HandleAdminAuth(CNetClientData *clientData, json &j)
{
	// { "action": "admin_auth", "secretHash": [byte array] }
	// The client computes HMAC-SHA256("LightHeroes.AdminSecret.v1", rawSecret) and sends the hash.

	mutex->Lock();
	bool hasAdminSecret = !adminSecretHash.empty();
	vector<u8> expectedHash = adminSecretHash;
	int maxAttempts = adminAuthMaxAttempts;

	// Rate limiting: check prior failures for this client
	int priorFailures = 0;
	auto failIt = adminAuthFailures.find(clientData);
	if (failIt != adminAuthFailures.end())
		priorFailures = failIt->second;
	mutex->Unlock();

	if (priorFailures >= maxAttempts)
	{
		LOGM("[AUDIT] admin_auth result=rejected_rate_limit client=%s failures=%d",
			 clientData->clientName.c_str(), priorFailures);
		netServer->Disconnect(clientData);
		return;
	}

	if (!hasAdminSecret)
	{
		LOGM("[AUDIT] admin_auth result=rejected_not_configured client=%s",
			 clientData->clientName.c_str());
		json response;
		response["action"] = "registry_adminAuthResult";
		response["authorized"] = false;
		response["reason"] = "admin_not_configured";
		SendJson(clientData, response);
		return;
	}

	if (!j.contains("secretHash") || !j["secretHash"].is_array())
	{
		LOGM("[AUDIT] admin_auth result=rejected_missing_credentials client=%s",
			 clientData->clientName.c_str());
		json response;
		response["action"] = "registry_adminAuthResult";
		response["authorized"] = false;
		response["reason"] = "missing_credentials";
		SendJson(clientData, response);
		return;
	}

	vector<u8> providedHash = j["secretHash"].get<vector<u8>>();

	// CRITICAL: Constant-time comparison to prevent timing attacks
	bool authorized = SYS_ConstantTimeEquals(expectedHash, providedHash);

	if (authorized)
	{
		mutex->Lock();
		if (!AssignRoleLocked(clientData, ClientRole::ADMIN, "admin_auth"))
		{
			mutex->Unlock();
			netServer->Disconnect(clientData);
			return;
		}
		adminAuthFailures.erase(clientData);
		mutex->Unlock();
		LOGM("[AUDIT] admin_auth result=success client=%s", clientData->clientName.c_str());
	}
	else
	{
		mutex->Lock();
		adminAuthFailures[clientData] = priorFailures + 1;
		int newCount = priorFailures + 1;
		mutex->Unlock();
		LOGM("[AUDIT] admin_auth result=failed client=%s attempt=%d/%d",
			 clientData->clientName.c_str(), newCount, maxAttempts);

		if (newCount >= maxAttempts)
		{
			json response;
			response["action"] = "registry_adminAuthResult";
			response["authorized"] = false;
			response["reason"] = "too_many_attempts";
			SendJson(clientData, response);
			netServer->Disconnect(clientData);
			return;
		}
	}

	json response;
	response["action"] = "registry_adminAuthResult";
	response["authorized"] = authorized;
	SendJson(clientData, response);
}

void CRegistryServer::HandleAdminGetClusterState(CNetClientData *clientData, json &j)
{
	// { "action": "admin_getClusterState" }

	// Check role using find() to avoid inserting a default UNKNOWN entry
	mutex->Lock();
	auto roleIt = clientRoles.find(clientData);
	bool isAdmin = (roleIt != clientRoles.end() && roleIt->second == ClientRole::ADMIN);
	mutex->Unlock();

	if (!isAdmin)
	{
		LOGWarning("CRegistryServer::HandleAdminGetClusterState: unauthorized from %s",
				   clientData->clientName.c_str());
		return;
	}

	LOGM("[AUDIT] admin_getClusterState client=%s", clientData->clientName.c_str());

	json clusterState = GetClusterState();

	json response;
	response["action"] = "registry_clusterState";
	response["clusterState"] = clusterState;
	SendJson(clientData, response);
}

void CRegistryServer::HandleAdminStartManagedServices(CNetClientData *clientData, json &j)
{
	string scope = j.value("scope", "all");
	string nodeId = j.value("nodeId", "");
	string roleName = j.value("role", "");

	json response;
	response["action"] = "registry_adminCommandResult";
	response["command"] = "startManagedServices";
	response["scope"] = scope;

	if (scope != "all" && scope != "node" && scope != "role")
	{
		response["accepted"] = false;
		response["reason"] = "invalid_scope";
		SendJson(clientData, response);
		return;
	}
	if (scope == "node" && nodeId.empty())
	{
		response["accepted"] = false;
		response["reason"] = "missing_node_id";
		SendJson(clientData, response);
		return;
	}
	if (scope == "role" && roleName.empty())
	{
		response["accepted"] = false;
		response["reason"] = "missing_role";
		SendJson(clientData, response);
		return;
	}

	vector<pair<string, CNetClientData *>> targets;
	uint64_t commandId = 0;

	mutex->Lock();
	commandId = ++adminCommandSeq;
	if (scope == "node")
	{
		auto it = nodeAgentsByNodeId.find(nodeId);
		if (it != nodeAgentsByNodeId.end() && it->second && it->second->clientData)
			targets.push_back({it->first, it->second->clientData});
	}
	else
	{
		for (const auto &pair : nodeAgentsByNodeId)
		{
			CRegistryNodeAgentRecord *record = pair.second;
			if (!record || !record->clientData)
				continue;
			targets.push_back({pair.first, record->clientData});
		}
	}
	mutex->Unlock();

	int dispatched = 0;
	for (const auto &target : targets)
	{
		json command;
		command["action"] = "registry_manageServices";
		command["operation"] = "start";
		command["commandId"] = (uint64_t)commandId;
		command["scope"] = scope;
		command["targetNodeId"] = target.first;
		if (!roleName.empty())
			command["role"] = roleName;
		SendJson(target.second, command);
		dispatched++;
	}

	response["accepted"] = (dispatched > 0);
	response["reason"] = (dispatched > 0) ? "ok" : "no_target_node_agents";
	response["commandId"] = (uint64_t)commandId;
	response["targetedNodes"] = (int)targets.size();
	response["dispatchedNodes"] = dispatched;
	response["completed"] = false;
	response["successfulNodes"] = 0;
	response["failedNodes"] = 0;
	if (!nodeId.empty())
		response["nodeId"] = nodeId;
	if (!roleName.empty())
		response["role"] = roleName;

	if (dispatched > 0)
	{
		mutex->Lock();
		PendingAdminCommand pending;
		pending.commandId = commandId;
		pending.commandName = "startManagedServices";
		pending.scope = scope;
		pending.targetNodeId = nodeId;
		pending.roleName = roleName;
		pending.targetedNodes = (int)targets.size();
		pending.dispatchedNodes = dispatched;
		pending.createdAt = time(NULL);
		pending.adminClient = clientData;
		for (const auto &target : targets)
			pending.awaitingNodeIds.insert(target.first);
		pendingAdminCommands[commandId] = pending;
		mutex->Unlock();
	}

	SendJson(clientData, response);
	LOGM("[AUDIT] admin_startManagedServices client=%s scope=%s nodeId=%s role=%s targeted=%d dispatched=%d commandId=%llu",
		 clientData->clientName.c_str(), scope.c_str(), nodeId.c_str(), roleName.c_str(),
		 (int)targets.size(), dispatched, (unsigned long long)commandId);
}

void CRegistryServer::HandleAdminStopManagedServices(CNetClientData *clientData, json &j)
{
	string scope = j.value("scope", "all");
	string nodeId = j.value("nodeId", "");
	string roleName = j.value("role", "");
	bool graceful = j.value("graceful", true);
	bool includeRegistry = j.value("includeRegistry", false);

	json response;
	response["action"] = "registry_adminCommandResult";
	response["command"] = "stopManagedServices";
	response["scope"] = scope;

	if (scope != "all" && scope != "node" && scope != "role")
	{
		response["accepted"] = false;
		response["reason"] = "invalid_scope";
		SendJson(clientData, response);
		return;
	}
	if (scope == "node" && nodeId.empty())
	{
		response["accepted"] = false;
		response["reason"] = "missing_node_id";
		SendJson(clientData, response);
		return;
	}
	if (scope == "role" && roleName.empty())
	{
		response["accepted"] = false;
		response["reason"] = "missing_role";
		SendJson(clientData, response);
		return;
	}

	vector<pair<string, CNetClientData *>> targets;
	uint64_t commandId = 0;

	mutex->Lock();
	commandId = ++adminCommandSeq;
	if (scope == "node")
	{
		auto it = nodeAgentsByNodeId.find(nodeId);
		if (it != nodeAgentsByNodeId.end() && it->second && it->second->clientData)
			targets.push_back({it->first, it->second->clientData});
	}
	else
	{
		for (const auto &pair : nodeAgentsByNodeId)
		{
			CRegistryNodeAgentRecord *record = pair.second;
			if (!record || !record->clientData)
				continue;
			targets.push_back({pair.first, record->clientData});
		}
	}
	mutex->Unlock();

	int dispatched = 0;
	for (const auto &target : targets)
	{
		json command;
		command["action"] = "registry_manageServices";
		command["operation"] = "stop";
		command["commandId"] = (uint64_t)commandId;
		command["scope"] = scope;
		command["targetNodeId"] = target.first;
		command["graceful"] = graceful;
		command["includeRegistry"] = includeRegistry;
		if (!roleName.empty())
			command["role"] = roleName;
		SendJson(target.second, command);
		dispatched++;
	}

	response["accepted"] = (dispatched > 0);
	response["reason"] = (dispatched > 0) ? "ok" : "no_target_node_agents";
	response["commandId"] = (uint64_t)commandId;
	response["targetedNodes"] = (int)targets.size();
	response["dispatchedNodes"] = dispatched;
	response["graceful"] = graceful;
	response["includeRegistry"] = includeRegistry;
	response["completed"] = false;
	response["successfulNodes"] = 0;
	response["failedNodes"] = 0;
	if (!nodeId.empty())
		response["nodeId"] = nodeId;
	if (!roleName.empty())
		response["role"] = roleName;

	if (dispatched > 0)
	{
		mutex->Lock();
		PendingAdminCommand pending;
		pending.commandId = commandId;
		pending.commandName = "stopManagedServices";
		pending.scope = scope;
		pending.targetNodeId = nodeId;
		pending.roleName = roleName;
		pending.graceful = graceful;
		pending.includeRegistry = includeRegistry;
		pending.targetedNodes = (int)targets.size();
		pending.dispatchedNodes = dispatched;
		pending.createdAt = time(NULL);
		pending.adminClient = clientData;
		for (const auto &target : targets)
			pending.awaitingNodeIds.insert(target.first);
		pendingAdminCommands[commandId] = pending;
		mutex->Unlock();
	}

	SendJson(clientData, response);
	LOGM("[AUDIT] admin_stopManagedServices client=%s scope=%s nodeId=%s role=%s targeted=%d dispatched=%d graceful=%s includeRegistry=%s commandId=%llu",
		 clientData->clientName.c_str(), scope.c_str(), nodeId.c_str(), roleName.c_str(),
		 (int)targets.size(), dispatched,
		 graceful ? "true" : "false", includeRegistry ? "true" : "false",
		 (unsigned long long)commandId);
}

// ========== Phase 5: Maintenance Node ==========

void CRegistryServer::HandleAdminMaintenanceNode(CNetClientData *clientData, json &j)
{
	string nodeId = j.value("nodeId", "");
	bool enable = j.value("enable", true);

	json response;
	response["action"] = "registry_adminMaintenanceResult";

	mutex->Lock();

	// Check authorization
	auto roleIt = clientRoles.find(clientData);
	if (roleIt == clientRoles.end() || roleIt->second != ClientRole::ADMIN)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleAdminMaintenanceNode: unauthorized from %s",
				   clientData->clientName.c_str());
		response["accepted"] = false;
		response["reason"] = "not_authorized";
		SendJson(clientData, response);
		return;
	}

	if (nodeId.empty())
	{
		mutex->Unlock();
		response["accepted"] = false;
		response["reason"] = "missing_nodeId";
		SendJson(clientData, response);
		return;
	}

	auto *nodeRecord = GetNodeAgentRecord(nodeId);
	if (!nodeRecord)
	{
		mutex->Unlock();
		response["accepted"] = false;
		response["reason"] = "node_not_found";
		response["nodeId"] = nodeId;
		SendJson(clientData, response);
		return;
	}

	nodeRecord->maintenanceMode = enable;
	int runningGames = nodeRecord->runningGameCount;
	mutex->Unlock();

	response["accepted"] = true;
	response["nodeId"] = nodeId;
	response["maintenanceMode"] = enable;
	response["runningGameCount"] = runningGames;
	if (enable)
		response["message"] = runningGames > 0
			? "Node in maintenance mode, draining " + to_string(runningGames) + " games"
			: "Node in maintenance mode, fully drained";
	else
		response["message"] = "Node removed from maintenance mode";
	SendJson(clientData, response);

	LOGM("CRegistryServer: node '%s' maintenance=%s runningGames=%d",
		 nodeId.c_str(), enable ? "true" : "false", runningGames);
}

// ========== Phase 2: Game Allocation ==========

CRegistryNodeAgentRecord *CRegistryServer::SelectBestNode(int requiredCapacity)
{
	// Must be called with mutex locked.
	// Strategy: pick the online node with the most remaining capacity.
	// Account for pending allocations that haven't spawned yet (heartbeat
	// only reports actual running processes, so pending ones aren't counted).
	CRegistryNodeAgentRecord *best = NULL;
	int bestFreeCapacity = -1;

	// Count pending allocations per node
	map<string, int> pendingPerNode;
	for (const auto &pair : pendingGameAllocations)
		pendingPerNode[pair.second.nodeId]++;

	for (auto *record : nodeAgents)
	{
		if (!record || !record->clientData)
			continue;
		if (record->status != "online")
			continue;
		if (record->maintenanceMode)
			continue;

		int pending = 0;
		auto pit = pendingPerNode.find(record->nodeId);
		if (pit != pendingPerNode.end())
			pending = pit->second;

		int freeCapacity = record->maxProcessCapacity - record->connectedProcessCount - pending;
		if (freeCapacity < requiredCapacity)
			continue;

		if (freeCapacity > bestFreeCapacity)
		{
			bestFreeCapacity = freeCapacity;
			best = record;
		}
	}

	return best;
}

int CRegistryServer::AllocatePortOnNode(const string &nodeId)
{
	// Must be called with mutex locked.
	set<int> &usedPorts = nodeUsedPorts[nodeId];

	// Prefer per-node port range from cluster config, fall back to global gamePortRangeStart/End
	int rangeStart = gamePortRangeStart;
	int rangeEnd = gamePortRangeEnd;
	if (hasClusterConfig)
	{
		const SNodeConfig *nodeConfig = clusterConfig.GetNode(nodeId);
		if (nodeConfig && nodeConfig->portRangeStart > 0 && nodeConfig->portRangeEnd > 0)
		{
			rangeStart = nodeConfig->portRangeStart;
			rangeEnd = nodeConfig->portRangeEnd;
		}
	}

	for (int port = rangeStart; port <= rangeEnd; port++)
	{
		if (usedPorts.find(port) == usedPorts.end())
		{
			usedPorts.insert(port);
			return port;
		}
	}
	return 0; // no ports available
}

void CRegistryServer::ReleasePortOnNode(const string &nodeId, int port)
{
	// Must be called with mutex locked.
	if (port <= 0)
		return;
	auto it = nodeUsedPorts.find(nodeId);
	if (it != nodeUsedPorts.end())
		it->second.erase(port);
}

void CRegistryServer::HandleLobbyAllocateGameServer(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	string mapName = j.value("mapName", "");
	int playerCount = j.value("playerCount", 0);
	bool resume = j.value("resume", false);

	if (gameId.empty())
	{
		LOGWarning("CRegistryServer::HandleLobbyAllocateGameServer: empty gameId");
		json response;
		response["action"] = "registry_allocateGameServerResult";
		response["gameId"] = "";
		response["accepted"] = false;
		response["reason"] = "empty_game_id";
		response["nodeId"] = "";
		response["address"] = "";
		response["port"] = 0;
		SendJson(clientData, response);
		return;
	}

	mutex->Lock();

	// Check for duplicate allocation (gameId-scoped lease)
	if (pendingGameAllocations.find(gameId) != pendingGameAllocations.end())
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleLobbyAllocateGameServer: allocation already in progress for gameId=%s", gameId.c_str());
		json response;
		response["action"] = "registry_allocateGameServerResult";
		response["gameId"] = gameId;
		response["accepted"] = false;
		response["reason"] = "allocation_in_progress";
		response["nodeId"] = "";
		response["address"] = "";
		response["port"] = 0;
		SendJson(clientData, response);
		return;
	}

	// Check if game server is already active (fast path)
	auto gsIt = gameServers.find(gameId);
	if (gsIt != gameServers.end() && gsIt->second->status == "running")
	{
		string address = gsIt->second->listenAddress;
		int port = gsIt->second->listenPort;
		mutex->Unlock();

		LOGM("CRegistryServer::HandleLobbyAllocateGameServer: gameId=%s already active at %s:%d", gameId.c_str(), address.c_str(), port);
		json response;
		response["action"] = "registry_allocateGameServerResult";
		response["gameId"] = gameId;
		response["accepted"] = true;
		response["reason"] = "already_active";
		response["nodeId"] = "";
		response["address"] = address;
		response["port"] = port;
		SendJson(clientData, response);
		return;
	}

	// Select best node
	CRegistryNodeAgentRecord *node = SelectBestNode(1);
	if (!node)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleLobbyAllocateGameServer: no available nodes for gameId=%s", gameId.c_str());
		json response;
		response["action"] = "registry_allocateGameServerResult";
		response["gameId"] = gameId;
		response["accepted"] = false;
		response["reason"] = "no_available_nodes";
		response["nodeId"] = "";
		response["address"] = "";
		response["port"] = 0;
		SendJson(clientData, response);
		return;
	}

	auto isLoopbackAddress = [](const string &address) -> bool {
		if (address.empty())
			return false;
		if (address == "localhost" || address == "127.0.0.1" || address == "::1")
			return true;
		if (address.rfind("127.", 0) == 0)
			return true;
		return false;
	};

	if (isLoopbackAddress(registryExternalAddress) && !isLoopbackAddress(node->nodeIp))
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleLobbyAllocateGameServer: rejecting allocation gameId=%s nodeId=%s because registryExternalAddress=%s is loopback",
			gameId.c_str(), node->nodeId.c_str(), registryExternalAddress.c_str());
		json response;
		response["action"] = "registry_allocateGameServerResult";
		response["gameId"] = gameId;
		response["accepted"] = false;
		response["reason"] = "invalid_registry_external_address";
		response["nodeId"] = node->nodeId;
		response["address"] = "";
		response["port"] = 0;
		SendJson(clientData, response);
		return;
	}

	// Allocate port on the selected node
	int port = AllocatePortOnNode(node->nodeId);
	if (port == 0)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleLobbyAllocateGameServer: no ports available on node %s for gameId=%s",
				   node->nodeId.c_str(), gameId.c_str());
		json response;
		response["action"] = "registry_allocateGameServerResult";
		response["gameId"] = gameId;
		response["accepted"] = false;
		response["reason"] = "no_ports_available";
		response["nodeId"] = node->nodeId;
		response["address"] = "";
		response["port"] = 0;
		SendJson(clientData, response);
		return;
	}

	// Capacity is reserved implicitly via pendingGameAllocations count
	// (SelectBestNode subtracts pending allocations from free capacity)

	// Create pending allocation record
	uint64_t requestId = ++gameAllocationSeq;
	PendingGameAllocation allocation;
	allocation.gameId = gameId;
	allocation.nodeId = node->nodeId;
	allocation.requestId = requestId;
	allocation.allocatedPort = port;
	allocation.lobbyClient = clientData;
	allocation.mapName = mapName;
	allocation.playerCount = playerCount;
	allocation.resume = resume;
	allocation.createdAt = time(NULL);
	pendingGameAllocations[gameId] = allocation;

	// Build spawn command for node agent
	CNetClientData *nodeAgentClient = node->clientData;
	string nodeIp = node->nodeIp;

	json spawnCmd;
	spawnCmd["action"] = "registry_spawnGameServer";
	spawnCmd["requestId"] = requestId;
	spawnCmd["gameId"] = gameId;
	spawnCmd["port"] = port;
	spawnCmd["registryAddress"] = registryExternalAddress;
	spawnCmd["registryPort"] = netServer->serverPort;

	json extraArgs;
	extraArgs["mapName"] = mapName;
	extraArgs["playerCount"] = playerCount;
	extraArgs["resume"] = resume;
	spawnCmd["extraArgs"] = extraArgs;

	// Verify node agent is still connected before sending (prevents TOCTOU — node could
	// disconnect between SelectBestNode and here if mutex were released)
	auto roleIt = clientRoles.find(nodeAgentClient);
	if (roleIt == clientRoles.end() || roleIt->second != ClientRole::NODE_AGENT)
	{
		// Node disconnected while we were building the command — clean up allocation
		pendingGameAllocations.erase(gameId);
		ReleasePortOnNode(allocation.nodeId, port);
		mutex->Unlock();

		LOGWarning("CRegistryServer::HandleLobbyAllocateGameServer: node %s disconnected during allocation for gameId=%s",
				   allocation.nodeId.c_str(), gameId.c_str());
		json response;
		response["action"] = "registry_allocateGameServerResult";
		response["gameId"] = gameId;
		response["accepted"] = false;
		response["reason"] = "node_disconnected";
		response["nodeId"] = allocation.nodeId;
		response["address"] = "";
		response["port"] = 0;
		SendJson(clientData, response);
		return;
	}

	// Send spawn command while still holding mutex — SendJson just queues a packet (non-blocking),
	// so holding the mutex ensures nodeAgentClient remains valid
	SendJson(nodeAgentClient, spawnCmd);

	mutex->Unlock();

	LOGM("CRegistryServer::HandleLobbyAllocateGameServer: allocated gameId=%s on node=%s port=%d requestId=%llu",
		 gameId.c_str(), allocation.nodeId.c_str(), port, (unsigned long long)requestId);
}

void CRegistryServer::HandleNodeAgentGameProcessSpawned(CNetClientData *clientData, json &j)
{
	uint64_t requestId = j.value("requestId", (uint64_t)0);
	string gameId = j.value("gameId", "");
	string nodeId = j.value("nodeId", "");
	bool success = j.value("success", false);
	int pid = j.value("pid", 0);
	string reason = j.value("reason", "");

	LOGM("CRegistryServer::HandleNodeAgentGameProcessSpawned: requestId=%llu gameId=%s success=%s pid=%d",
		 (unsigned long long)requestId, gameId.c_str(), success ? "true" : "false", pid);

	mutex->Lock();

	auto allocIt = pendingGameAllocations.find(gameId);
	if (allocIt == pendingGameAllocations.end())
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleNodeAgentGameProcessSpawned: no pending allocation for gameId=%s", gameId.c_str());
		return;
	}

	PendingGameAllocation allocation = allocIt->second;
	CRegistryNodeAgentRecord *nodeRecord = GetNodeAgentRecord(allocation.nodeId);
	if (!nodeRecord || nodeRecord->clientData != clientData)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleNodeAgentGameProcessSpawned: rejecting non-owner completion requestId=%llu gameId=%s ownerNode=%s sender=%s",
			(unsigned long long)requestId, gameId.c_str(), allocation.nodeId.c_str(), clientData->clientName.c_str());
		return;
	}

	if (allocation.requestId != requestId)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleNodeAgentGameProcessSpawned: requestId mismatch gameId=%s expected=%llu got=%llu",
			gameId.c_str(), (unsigned long long)allocation.requestId, (unsigned long long)requestId);
		return;
	}

	if (nodeId != allocation.nodeId)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleNodeAgentGameProcessSpawned: nodeId mismatch gameId=%s expected=%s got=%s",
			gameId.c_str(), allocation.nodeId.c_str(), nodeId.c_str());
		return;
	}

	if (!success)
	{
		// Spawn failed — clean up and notify lobby
		ReleasePortOnNode(allocation.nodeId, allocation.allocatedPort);

		CNetClientData *lobbyClient = allocation.lobbyClient;
		pendingGameAllocations.erase(allocIt);
		mutex->Unlock();

		if (lobbyClient)
		{
			json response;
			response["action"] = "registry_allocateGameServerResult";
			response["gameId"] = gameId;
			response["accepted"] = false;
			response["reason"] = "spawn_failed: " + reason;
			response["nodeId"] = allocation.nodeId;
			response["address"] = "";
			response["port"] = 0;
			SendJson(lobbyClient, response);
		}
		return;
	}

	// Spawn succeeded — keep the allocation active until gs_register arrives.
	// The game server process will connect back to us and send gs_register.
	// When gs_register arrives, we'll match it by gameId and complete the flow.
	mutex->Unlock();

	// Note: we don't send registry_allocateGameServerResult yet — we wait for the game server
	// to actually register (gs_register). This ensures the server is truly online before
	// telling the lobby.
}

void CRegistryServer::HandleLobbyQueryGameStatus(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");

	mutex->Lock();

	json response;
	response["action"] = "registry_gameStatusResult";
	response["gameId"] = gameId;

	// Check if game server is registered and running
	auto gsIt = gameServers.find(gameId);
	if (gsIt != gameServers.end())
	{
		response["found"] = true;
		response["status"] = gsIt->second->status;
		response["address"] = gsIt->second->listenAddress;
		response["port"] = gsIt->second->listenPort;
		response["connectedPlayers"] = gsIt->second->connectedPlayers;
	}
	else
	{
		// Check if allocation is pending
		auto allocIt = pendingGameAllocations.find(gameId);
		if (allocIt != pendingGameAllocations.end())
		{
			response["found"] = true;
			response["status"] = "allocating";
			response["address"] = "";
			response["port"] = 0;
			response["connectedPlayers"] = 0;
		}
		else
		{
			response["found"] = false;
			response["status"] = "unknown";
		}
	}

	mutex->Unlock();
	SendJson(clientData, response);
}

// ========== Graceful Shutdown Sequence ==========

void CRegistryServer::HandleAdminGracefulShutdown(CNetClientData *clientData, json &j)
{
	mutex->Lock();
	auto roleIt = clientRoles.find(clientData);
	if (roleIt == clientRoles.end() || roleIt->second != ClientRole::ADMIN)
	{
		mutex->Unlock();
		LOGWarning("CRegistryServer::HandleAdminGracefulShutdown: unauthorized");
		json response;
		response["action"] = "registry_shutdownProgress";
		response["phase"] = "rejected";
		response["message"] = "not_authorized";
		response["complete"] = false;
		SendJson(clientData, response);
		return;
	}

	if (shutdownPhase != ShutdownPhase::NONE)
	{
		mutex->Unlock();
		json response;
		response["action"] = "registry_shutdownProgress";
		response["phase"] = "already_in_progress";
		response["message"] = "Shutdown already in progress";
		response["complete"] = false;
		SendJson(clientData, response);
		return;
	}

	shutdownDrainTimeoutSeconds = j.value("drainTimeoutSec", 1800);
	shutdownForceDrain = j.value("forceDrain", false);
	shutdownAdminClient = clientData;
	shutdownStartedAt = time(NULL);
	shutdownPhase = ShutdownPhase::STOP_NEW_GAMES;
	mutex->Unlock();

	LOGM("[AUDIT] action=gracefulShutdown client=%s drainTimeout=%d forceDrain=%s",
		 clientData->clientName.c_str(), shutdownDrainTimeoutSeconds,
		 shutdownForceDrain ? "true" : "false");

	json ack;
	ack["action"] = "registry_shutdownProgress";
	ack["phase"] = "stop_new_games";
	ack["message"] = "Shutdown initiated — stopping new games and entering maintenance";
	ack["complete"] = false;
	SendJson(clientData, ack);

	// Phase 1 kicks off immediately in AdvanceShutdownSequence
}

void CRegistryServer::AdvanceShutdownSequence()
{
	// Called from CheckHeartbeatTimeouts with mutex already unlocked.
	// Each phase checks its completion condition and advances to the next.

	mutex->Lock();
	time_t now = time(NULL);
	CNetClientData *adminClient = shutdownAdminClient;

	switch (shutdownPhase)
	{
	case ShutdownPhase::STOP_NEW_GAMES:
	{
		// Put all nodes in maintenance — prevents new game allocation
		for (auto *record : nodeAgents)
		{
			if (record && !record->maintenanceMode)
			{
				record->maintenanceMode = true;
				LOGM("CRegistryServer: shutdown phase 1 — node %s set to maintenance", record->nodeId.c_str());
			}
		}
		shutdownPhase = ShutdownPhase::DRAIN_ACTIVE;
		mutex->Unlock();

		if (adminClient)
		{
			json progress;
			progress["action"] = "registry_shutdownProgress";
			progress["phase"] = "drain_active";
			progress["message"] = "All nodes in maintenance — draining active games";
			progress["complete"] = false;
			SendJson(adminClient, progress);
		}
		return;
	}

	case ShutdownPhase::DRAIN_ACTIVE:
	{
		// Count running game servers
		int totalRunning = 0;
		for (const auto &pair : gameServers)
		{
			if (pair.second->status == "running")
				totalRunning++;
		}

		bool timedOut = difftime(now, shutdownStartedAt) > shutdownDrainTimeoutSeconds;

		if (totalRunning == 0)
		{
			// All games drained — advance
			shutdownPhase = ShutdownPhase::STOP_GAME_SERVERS;
			mutex->Unlock();
			if (adminClient)
			{
				json progress;
				progress["action"] = "registry_shutdownProgress";
				progress["phase"] = "stop_game_servers";
				progress["message"] = "All games drained — stopping game servers";
				progress["complete"] = false;
				SendJson(adminClient, progress);
			}
		}
		else if (timedOut && shutdownForceDrain)
		{
			mutex->Unlock();
			LOGM("CRegistryServer: shutdown drain timeout — force-draining %d games", totalRunning);
			ForceDrainAllGames();
			// Will re-enter DRAIN_ACTIVE on next tick and find 0 running
		}
		else if (timedOut)
		{
			// Non-force drain timeout — proceed to stop anyway
			shutdownPhase = ShutdownPhase::STOP_GAME_SERVERS;
			mutex->Unlock();
			if (adminClient)
			{
				json progress;
				progress["action"] = "registry_shutdownProgress";
				progress["phase"] = "stop_game_servers";
				progress["message"] = "Drain timeout — " + to_string(totalRunning) + " games still running, proceeding to stop";
				progress["complete"] = false;
				SendJson(adminClient, progress);
			}
		}
		else
		{
			mutex->Unlock();
			// Still draining — wait for next tick
		}
		return;
	}

	case ShutdownPhase::STOP_GAME_SERVERS:
	{
		// Send save+shutdown to all running game servers
		for (auto &pair : gameServers)
		{
			CRegistryGameServerRecord *gs = pair.second;
			if (gs->status == "running" && gs->clientData)
			{
				json saveCmd;
				saveCmd["action"] = "lobby_saveState";
				SendJson(gs->clientData, saveCmd);

				json shutdownCmd;
				shutdownCmd["action"] = "lobby_shutdown";
				shutdownCmd["reason"] = "graceful_shutdown";
				SendJson(gs->clientData, shutdownCmd);

				gs->status = "shutting_down";
			}
		}
		shutdownPhase = ShutdownPhase::STOP_INFRASTRUCTURE;
		mutex->Unlock();

		if (adminClient)
		{
			json progress;
			progress["action"] = "registry_shutdownProgress";
			progress["phase"] = "stop_infrastructure";
			progress["message"] = "Game servers shutting down — stopping infrastructure services";
			progress["complete"] = false;
			SendJson(adminClient, progress);
		}
		return;
	}

	case ShutdownPhase::STOP_INFRASTRUCTURE:
	{
		// Send stop command to all node agents for non-game roles
		for (auto *record : nodeAgents)
		{
			if (record && record->clientData)
			{
				json stopCmd;
				stopCmd["action"] = "registry_manageServices";
				stopCmd["operation"] = "stop";
				stopCmd["commandId"] = (uint64_t)0;
				stopCmd["scope"] = "all";
				stopCmd["targetNodeId"] = record->nodeId;
				stopCmd["roleName"] = "";
				stopCmd["graceful"] = true;
				stopCmd["includeRegistry"] = false;
				SendJson(record->clientData, stopCmd);
			}
		}
		shutdownPhase = ShutdownPhase::STOP_REGISTRY;
		mutex->Unlock();

		if (adminClient)
		{
			json progress;
			progress["action"] = "registry_shutdownProgress";
			progress["phase"] = "stop_registry";
			progress["message"] = "Infrastructure services stopping — shutting down registry";
			progress["complete"] = true;
			SendJson(adminClient, progress);
		}

		LOGM("CRegistryServer: graceful shutdown complete");
		// Caller should check shutdownPhase == STOP_REGISTRY and exit
		return;
	}

	case ShutdownPhase::STOP_REGISTRY:
	case ShutdownPhase::NONE:
		mutex->Unlock();
		return;
	}

	mutex->Unlock();
}

void CRegistryServer::ForceDrainAllGames()
{
	mutex->Lock();
	for (auto &pair : gameServers)
	{
		CRegistryGameServerRecord *gs = pair.second;
		if (gs->status == "running" && gs->clientData)
		{
			// Save state first
			json saveCmd;
			saveCmd["action"] = "lobby_saveState";
			SendJson(gs->clientData, saveCmd);

			// Then shutdown
			json shutdownCmd;
			shutdownCmd["action"] = "lobby_shutdown";
			shutdownCmd["reason"] = "force_drain";
			SendJson(gs->clientData, shutdownCmd);

			gs->status = "shutting_down";
			LOGM("CRegistryServer: force-draining game %s", gs->gameId.c_str());
		}
	}
	mutex->Unlock();
}
