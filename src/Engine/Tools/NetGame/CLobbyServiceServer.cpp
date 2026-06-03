#include "CLobbyServiceServer.h"
#include "NET_Main.h"
#include "SYS_Funct.h"
#include "CNetPacket.h"
#include "CNetClientData.h"
#include "CGameServerRegistry.h"
#include "CNetGameInternalAuth.h"
#include "DBG_Log.h"

CLobbyServiceServer::CLobbyServiceServer(int port, const string &internalSecret)
{
	this->registry = NULL;
	this->heartbeatTimeoutSeconds = 15;
	this->heartbeatWarningSeconds = 8;
	mutex = new CSlrMutex("CLobbyServiceServerMutex");
	internalSecretHash = NetGameInternalSecretToHash(internalSecret);

	netServer = new CNetServer(port);
	netPackets = new CNetGamePackets();
	netServer->AddServerCallback(this);
	netServer->AddPacketCallback(netPackets);
}

CLobbyServiceServer::~CLobbyServiceServer()
{
	Shutdown();

	mutex->Lock();
	for (auto &pair : gameServers)
	{
		delete pair.second;
	}
	gameServers.clear();
	mutex->Unlock();

	delete mutex;
}

void CLobbyServiceServer::Start()
{
	netServer->StartServer();
	if (internalSecretHash.empty())
		LOGWarning("CLobbyServiceServer: internal secret not configured; all connections will be rejected");
	LOGM("CLobbyServiceServer: started on port %d", netServer->serverPort);
}

void CLobbyServiceServer::Shutdown()
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

u8 CLobbyServiceServer::NetServerAuthorize(CNetClientData *clientData, string userName, vector<u8> passwordHash)
{
	// Internal service — require shared secret and a game-server login name.
	if (userName.rfind("gameserver-", 0) != 0)
	{
		LOGWarning("CLobbyServiceServer::NetServerAuthorize: rejecting %s (invalid internal login)", userName.c_str());
		return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;
	}

	if (!NetGameInternalSecretMatches(internalSecretHash, passwordHash))
	{
		LOGWarning("CLobbyServiceServer::NetServerAuthorize: rejecting %s (invalid internal secret)", userName.c_str());
		return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;
	}

	LOGD("CLobbyServiceServer::NetServerAuthorize: accepted %s", userName.c_str());
	return NET_SERVER_CALLBACK_AUTHORIZE_CORRECT;
}

void CLobbyServiceServer::NetServerCallbackClientConnected(CNetClientData *clientData)
{
	LOGD("CLobbyServiceServer::NetServerCallbackClientConnected: %s", clientData->clientName.c_str());
}

void CLobbyServiceServer::NetServerCallbackClientDisconnected(CNetClientData *clientData)
{
	LOGD("CLobbyServiceServer::NetServerCallbackClientDisconnected: %s", clientData->clientName.c_str());

	// Find and remove the game server record associated with this connection
	mutex->Lock();
	for (auto it = gameServers.begin(); it != gameServers.end(); ++it)
	{
		if (it->second->clientData == clientData)
		{
			LOGM("CLobbyServiceServer: game server disconnected gameId=%s serverId=%s",
				 it->second->gameId.c_str(), it->second->serverId.c_str());
			delete it->second;
			gameServers.erase(it);
			break;
		}
	}
	mutex->Unlock();
}

void CLobbyServiceServer::NetServerProcessPacket(CNetPacket *packet)
{
	if (packet->packetType != NET_PACKET_TYPE_JSON)
		return;

	CNetGamePacketJson *p = (CNetGamePacketJson *)packet;
	json j = p->jsonPayload;

	LOGD("CLobbyServiceServer::NetServerProcessPacket: %s", j.dump().c_str());

	try
	{
		if (!j.contains("action") || !j["action"].is_string())
			return;

		string action = j["action"];

		if (action == "gs_register")
			HandleRegister(packet->clientData, j);
		else if (action == "gs_heartbeat")
			HandleHeartbeat(packet->clientData, j);
		else if (action == "gs_playerEvent")
			HandlePlayerEvent(packet->clientData, j);
		else if (action == "gs_stateSaved")
			HandleStateSaved(packet->clientData, j);
		else if (action == "gs_shutdownComplete")
			HandleShutdownComplete(packet->clientData, j);
		else
			LOGWarning("CLobbyServiceServer: unknown action=%s", action.c_str());
	}
	catch (exception &ex)
	{
		LOGError("CLobbyServiceServer::NetServerProcessPacket: %s", ex.what());
	}
}

void CLobbyServiceServer::HandleRegister(CNetClientData *clientData, json &j)
{
	string serverId = j.value("serverId", "");
	string gameId = j.value("gameId", "");
	int listenPort = j.value("listenPort", 0);
	string listenAddress = j.value("listenAddress", "localhost");

	LOGM("CLobbyServiceServer::HandleRegister: serverId=%s gameId=%s port=%d",
		 serverId.c_str(), gameId.c_str(), listenPort);

	mutex->Lock();

	// Remove old record for this gameId if exists
	auto old = gameServers.find(gameId);
	if (old != gameServers.end())
	{
		delete old->second;
		gameServers.erase(old);
	}

	CGameServerRecord *record = new CGameServerRecord();
	record->serverId = serverId;
	record->gameId = gameId;
	record->listenAddress = listenAddress;
	record->listenPort = listenPort;
	record->connectedPlayers = 0;
	record->status = "running";
	record->lastHeartbeat = time(NULL);
	record->clientData = clientData;

	gameServers[gameId] = record;

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
		LOGM("CLobbyServiceServer: pushing pending tokens for gameId=%s", gameId.c_str());

		json jTokens;
		jTokens["action"] = "lobby_updateTokens";
		jTokens["gameId"] = gameId;
		json tokensJson = json::object();
		for (const auto &pair : pit->second.tokens)
		{
			tokensJson[std::to_string(pair.first)] = pair.second;
		}
		jTokens["tokens"] = tokensJson;
		json nameToIdJson = json::object();
		for (const auto &pair : pit->second.nameToId)
		{
			nameToIdJson[pair.first] = pair.second;
		}
		jTokens["nameToId"] = nameToIdJson;
		jTokens["tokenCreationTime"] = (int64_t)pit->second.tokenCreationTime;
		SendJson(clientData, jTokens);

		pendingTokens.erase(pit);
	}

	mutex->Unlock();
}

void CLobbyServiceServer::HandleHeartbeat(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end())
	{
		// Recovery: if server was in warning state and heartbeat arrives, transition back to running
		if (it->second->status == "warning")
		{
			LOGM("CLobbyServiceServer: heartbeat recovered for gameId=%s (was in warning state, back to running)",
				 gameId.c_str());
		}

		it->second->lastHeartbeat = time(NULL);
		it->second->connectedPlayers = j.value("connectedPlayers", 0);
		it->second->status = j.value("status", "running");
	}
	mutex->Unlock();
}

void CLobbyServiceServer::HandlePlayerEvent(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	string playerName = j.value("playerName", "");
	string event = j.value("event", "");

	LOGM("CLobbyServiceServer: player event gameId=%s player=%s event=%s",
		 gameId.c_str(), playerName.c_str(), event.c_str());

	// Update connected player count from the record
	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end())
	{
		it->second->lastHeartbeat = time(NULL);
	}
	mutex->Unlock();

	// Notify registry if set
	if (registry)
	{
		registry->OnPlayerEvent(gameId, playerName, event);
	}
}

void CLobbyServiceServer::HandleStateSaved(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	LOGM("CLobbyServiceServer: state saved for gameId=%s", gameId.c_str());
}

void CLobbyServiceServer::HandleShutdownComplete(CNetClientData *clientData, json &j)
{
	string gameId = j.value("gameId", "");
	LOGM("CLobbyServiceServer: shutdown complete for gameId=%s", gameId.c_str());

	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end())
	{
		it->second->status = "shutdown";
	}
	mutex->Unlock();
}

void CLobbyServiceServer::SendUpdateTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime)
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json j;
		j["action"] = "lobby_updateTokens";
		j["gameId"] = gameId;
		json tokensJson = json::object();
		for (const auto &pair : tokens)
		{
			tokensJson[std::to_string(pair.first)] = pair.second;
		}
		j["tokens"] = tokensJson;
		json nameToIdJson = json::object();
		for (const auto &pair : nameToId)
		{
			nameToIdJson[pair.first] = pair.second;
		}
		j["nameToId"] = nameToIdJson;
		j["tokenCreationTime"] = (int64_t)tokenCreationTime;
		SendJson(it->second->clientData, j);
		mutex->Unlock();
	}
	else
	{
		// Game server not yet registered — store as pending
		LOGD("CLobbyServiceServer::SendUpdateTokens: gameId=%s not registered, storing as pending", gameId.c_str());
		StorePendingTokens(gameId, tokens, nameToId, tokenCreationTime);
		mutex->Unlock();
	}
}

void CLobbyServiceServer::SendSaveState(const string &gameId)
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json j;
		j["action"] = "lobby_saveState";
		j["gameId"] = gameId;
		SendJson(it->second->clientData, j);
	}
	mutex->Unlock();
}

void CLobbyServiceServer::SendShutdown(const string &gameId, const string &reason)
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json j;
		j["action"] = "lobby_shutdown";
		j["gameId"] = gameId;
		j["reason"] = reason;
		SendJson(it->second->clientData, j);
	}
	mutex->Unlock();
}

void CLobbyServiceServer::SendKickPlayer(const string &gameId, const string &playerName, const string &reason)
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	if (it != gameServers.end() && it->second->clientData)
	{
		json j;
		j["action"] = "lobby_kickPlayer";
		j["gameId"] = gameId;
		j["playerName"] = playerName;
		j["reason"] = reason;
		SendJson(it->second->clientData, j);
	}
	mutex->Unlock();
}

void CLobbyServiceServer::CheckHeartbeatTimeouts()
{
	mutex->Lock();
	time_t now = time(NULL);

	for (auto &pair : gameServers)
	{
		CGameServerRecord *record = pair.second;
		double elapsed = difftime(now, record->lastHeartbeat);

		// Skip already-terminal states
		if (record->status == "shutdown" || record->status == "unreachable")
			continue;

		// Tier 2: Dead threshold — mark unreachable
		if (elapsed > heartbeatTimeoutSeconds)
		{
			LOGM("CLobbyServiceServer: heartbeat timeout for gameId=%s (last heartbeat %ds ago)",
				 record->gameId.c_str(), (int)elapsed);
			record->status = "unreachable";
		}
		// Tier 1: Warning threshold — trigger proactive save
		else if (elapsed > heartbeatWarningSeconds && record->status == "running")
		{
			LOGM("CLobbyServiceServer: heartbeat warning for gameId=%s (last heartbeat %ds ago, warning threshold=%ds), sending proactive save",
				 record->gameId.c_str(), (int)elapsed, heartbeatWarningSeconds);
			record->status = "warning";

			// Send save command to the game server
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
	mutex->Unlock();
}

CGameServerRecord *CLobbyServiceServer::GetRecord(const string &gameId)
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	CGameServerRecord *record = (it != gameServers.end()) ? it->second : NULL;
	mutex->Unlock();
	return record;
}

bool CLobbyServiceServer::IsGameServerReady(const string &gameId)
{
	mutex->Lock();
	auto it = gameServers.find(gameId);
	bool ready = (it != gameServers.end()
				  && (it->second->status == "running" || it->second->status == "warning")
				  && difftime(time(NULL), it->second->lastHeartbeat) < heartbeatTimeoutSeconds);
	mutex->Unlock();
	return ready;
}

int CLobbyServiceServer::GetGameServerCount()
{
	mutex->Lock();
	int count = (int)gameServers.size();
	mutex->Unlock();
	return count;
}

vector<CGameServerRecord *> CLobbyServiceServer::GetAllGameServerRecords()
{
	vector<CGameServerRecord *> result;
	mutex->Lock();
	for (auto &pair : gameServers)
	{
		result.push_back(pair.second);
	}
	mutex->Unlock();
	return result;
}

int CLobbyServiceServer::GetPendingTokensCount()
{
	mutex->Lock();
	int count = (int)pendingTokens.size();
	mutex->Unlock();
	return count;
}

void CLobbyServiceServer::StorePendingTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime)
{
	PendingTokens pt;
	pt.tokens = tokens;
	pt.nameToId = nameToId;
	pt.tokenCreationTime = tokenCreationTime;
	pendingTokens[gameId] = pt;
}

void CLobbyServiceServer::SendJson(CNetClientData *clientData, json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netServer->IssuePacket(clientData, packet);
	delete packet;
}
