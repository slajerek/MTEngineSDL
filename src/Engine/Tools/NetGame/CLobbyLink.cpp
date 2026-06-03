#include "CLobbyLink.h"
#include "CNetGameServer.h"
#include "CNetPacket.h"
#include "CNetClientData.h"
#include "CNetGameInternalAuth.h"
#include "DBG_Log.h"
#include "SYS_Funct.h"

CLobbyLink::CLobbyLink(const string &lobbyAddress, int lobbyServicePort,
					   const string &serverId, const string &gameId,
					   int listenPort, CNetGameServer *gameServer,
					   const string &internalSecret)
{
	this->serverId = serverId;
	this->gameId = gameId;
	this->listenAddress = "localhost";
	this->listenPort = listenPort;
	this->gameServer = gameServer;
	this->isRegistered = false;
	this->isConnected = false;
	this->shutdownRequested = false;
	this->lastHeartbeatTime = 0;
	this->startTime = time(NULL);

	// Create a CNetClient connecting to the lobby service port
	// Use "gameserver-{serverId}" as the login name, shared secret
	string loginName = "gameserver-" + serverId;
	vector<u8> secretHash = NetGameInternalSecretToHash(internalSecret);

	netClient = new CNetClient(lobbyAddress.c_str(), lobbyServicePort, 0, loginName, secretHash);
	netPackets = new CNetGamePackets();
	netClient->AddClientCallback(this);
	netClient->AddPacketCallback(netPackets);
}

CLobbyLink::~CLobbyLink()
{
	Shutdown();
}

void CLobbyLink::Connect()
{
	LOGM("CLobbyLink::Connect: connecting to lobby service serverId=%s gameId=%s", serverId.c_str(), gameId.c_str());
	netClient->Connect();
}

void CLobbyLink::Disconnect()
{
	if (netClient)
	{
		netClient->RemoveClientCallback(this);
		netClient->Disconnect();
	}
	isConnected = false;
	isRegistered = false;
}

void CLobbyLink::Shutdown()
{
	if (netClient)
	{
		netClient->RemoveClientCallback(this);
		netClient->Disconnect();
		netClient->status = NET_CLIENT_STATUS_SHUTDOWN;

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

void CLobbyLink::Update()
{
	if (!isConnected || !isRegistered)
		return;

	time_t now = time(NULL);
	if (difftime(now, lastHeartbeatTime) >= HEARTBEAT_INTERVAL_SECONDS)
	{
		SendHeartbeat();
		lastHeartbeatTime = now;
	}
}

void CLobbyLink::NetClientCallbackConnected(CNetClient *netClient)
{
	LOGM("CLobbyLink::NetClientCallbackConnected: connected to lobby service");
	isConnected = true;
	SendRegister();
}

void CLobbyLink::NetClientCallbackDisconnected(CNetClient *netClient)
{
	LOGM("CLobbyLink::NetClientCallbackDisconnected: disconnected from lobby service");
	isConnected = false;
	isRegistered = false;
}

void CLobbyLink::NetClientProcessPacket(CNetPacket *packet)
{
	if (packet->packetType != NET_PACKET_TYPE_JSON)
		return;

	CNetGamePacketJson *p = (CNetGamePacketJson *)packet;
	json j = p->jsonPayload;

	LOGD("CLobbyLink::NetClientProcessPacket: %s", j.dump().c_str());

	try
	{
		if (!j.contains("action") || !j["action"].is_string())
			return;

		string action = j["action"];

		if (action == "lobby_registered")
			HandleRegistered(j);
		else if (action == "lobby_updateTokens")
			HandleUpdateTokens(j);
		else if (action == "lobby_saveState")
			HandleSaveState(j);
		else if (action == "lobby_shutdown")
			HandleShutdown(j);
		else if (action == "lobby_kickPlayer")
			HandleKickPlayer(j);
		else
			LOGWarning("CLobbyLink: unknown action=%s", action.c_str());
	}
	catch (exception &ex)
	{
		LOGError("CLobbyLink::NetClientProcessPacket: %s", ex.what());
	}
}

void CLobbyLink::SendRegister()
{
	json j;
	j["action"] = "gs_register";
	j["serverId"] = serverId;
	j["gameId"] = gameId;
	j["listenPort"] = listenPort;
	j["listenAddress"] = listenAddress;
	j["maxPlayers"] = gameServer ? (int)gameServer->allowedPlayers.size() : 0;
	j["version"] = 1;
	SendJson(j);

	LOGM("CLobbyLink::SendRegister: serverId=%s gameId=%s listenPort=%d", serverId.c_str(), gameId.c_str(), listenPort);
}

void CLobbyLink::SendHeartbeat()
{
	json j;
	j["action"] = "gs_heartbeat";
	j["serverId"] = serverId;
	j["gameId"] = gameId;
	j["connectedPlayers"] = gameServer ? gameServer->connectedPlayerCount : 0;
	j["status"] = "running";
	j["uptimeSeconds"] = (int64_t)difftime(time(NULL), startTime);
	SendJson(j);
}

void CLobbyLink::SendPlayerEvent(const string &playerName, const string &event)
{
	json j;
	j["action"] = "gs_playerEvent";
	j["serverId"] = serverId;
	j["gameId"] = gameId;
	j["playerName"] = playerName;
	j["event"] = event;
	SendJson(j);
}

void CLobbyLink::HandleRegistered(json &j)
{
	isRegistered = true;
	lastHeartbeatTime = time(NULL);
	LOGM("CLobbyLink::HandleRegistered: registered with lobby for gameId=%s", gameId.c_str());
}

void CLobbyLink::HandleUpdateTokens(json &j)
{
	if (!gameServer)
		return;

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
		{
			nameToId[el.key()] = el.value().get<int>();
		}
	}

	gameServer->SetExpectedTokens(tokens, nameToId, tokenCreationTime);
	LOGM("CLobbyLink::HandleUpdateTokens: updated %d tokens for gameId=%s", (int)tokens.size(), gameId.c_str());
}

void CLobbyLink::HandleSaveState(json &j)
{
	if (!gameServer)
		return;

	LOGM("CLobbyLink::HandleSaveState: saving state for gameId=%s", gameId.c_str());
	gameServer->SaveState();

	// Confirm
	json jResp;
	jResp["action"] = "gs_stateSaved";
	jResp["serverId"] = serverId;
	jResp["gameId"] = gameId;
	SendJson(jResp);
}

void CLobbyLink::HandleShutdown(json &j)
{
	if (!gameServer)
		return;

	string reason = j.value("reason", "");
	LOGM("CLobbyLink::HandleShutdown: shutting down gameId=%s reason=%s", gameId.c_str(), reason.c_str());

	// Confirm receipt of shutdown command
	json jResp;
	jResp["action"] = "gs_shutdownComplete";
	jResp["serverId"] = serverId;
	jResp["gameId"] = gameId;
	SendJson(jResp);

	// Set flag — actual shutdown is driven by the owner (manager/test), not from the network callback thread
	shutdownRequested = true;
}

void CLobbyLink::HandleKickPlayer(json &j)
{
	if (!gameServer || !gameServer->netServer)
		return;

	string playerName = j.value("playerName", "");
	string reason = j.value("reason", "kicked");

	LOGM("CLobbyLink::HandleKickPlayer: kicking %s from gameId=%s", playerName.c_str(), gameId.c_str());

	// Find the client by name and disconnect them
	for (int i = 0; i < NET_MAX_CLIENTS; i++)
	{
		CNetClientData *client = gameServer->netServer->clients[i];
		if (client && client->state == NET_CLIENT_STATE_ONLINE && client->clientName == playerName)
		{
			gameServer->DisconnectWithError(client, "Kicked: %s", reason.c_str());
			break;
		}
	}
}

void CLobbyLink::SendJson(json sendJson)
{
	if (!netClient || !netClient->IsOnline())
	{
		LOGD("CLobbyLink::SendJson: not online, dropping packet");
		return;
	}

	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netClient->IssuePacket(true, packet);
	delete packet;
}
