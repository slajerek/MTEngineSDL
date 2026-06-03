#include "CNetGameServer.h"
#include "NET_Main.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "CNetPacket.h"
#include "CNetClientData.h"
#include "CSlrDate.h"
#include "CChatHistory.h"
#include "CLobbyLink.h"

#include <fstream>

using namespace nlohmann;
using namespace std;

// Constant-time string comparison to prevent timing side-channel attacks on tokens
static bool ConstantTimeStringEqual(const string &a, const string &b)
{
	if (a.size() != b.size())
		return false;
	volatile unsigned char result = 0;
	for (size_t i = 0; i < a.size(); i++)
		result |= (unsigned char)a[i] ^ (unsigned char)b[i];
	return result == 0;
}

CNetGameServer::CNetGameServer(json serverConfig, int serverPort)
{
	this->config = serverConfig;
	this->logSink = NULL;
	this->manager = NULL;
	this->lobbyLink = NULL;
	this->chatHistory = NULL;
	this->connectedPlayerCount = 0;
	this->isShuttingDown = false;
	this->tokenCreationTime = time(NULL);
	this->periodicSaveIntervalSeconds = serverConfig.value("periodicSaveIntervalSeconds", 60);
	this->lastPeriodicSaveTime = time(NULL);

	mutexLog = new CSlrMutex("CNetGameServerLogMutex");
	logDate = new CSlrDate();
	logBuf[0] = 0;

	this->serverPort = serverPort;
	netServer = new CNetServer(serverPort);
	Init(this->netServer);
}

CNetGameServer::~CNetGameServer()
{
}

void CNetGameServer::Init(CNetServer *netServer)
{
	this->netServer = netServer;
	this->netServer->AddServerCallback(this);

	this->netPackets = new CNetGamePackets();
	this->netServer->AddPacketCallback(this->netPackets);

	this->netServer->StartServer();
}

void CNetGameServer::NetServerCallbackClientConnected(CNetClientData *clientData)
{
	if (isShuttingDown) return;

	LOGD("CNetGameServer::NetServerCallbackClientConnected: %s", clientData->clientName.c_str());
	AddLog("Client connected: %s", clientData->clientName.c_str());

	// Check player whitelist (by clientId) — lock tokens for thread-safe name→id lookup
	{
		std::lock_guard<std::mutex> tokenLock(mutexTokens);
		auto nameIt = clientIdByClientName.find(clientData->clientName);
		if (!allowedPlayers.empty())
		{
			if (nameIt == clientIdByClientName.end() ||
				allowedPlayers.find(nameIt->second) == allowedPlayers.end())
			{
				AddLog("Client %s not in player whitelist, disconnecting", clientData->clientName.c_str());
				DisconnectWithError(clientData, "You are not a player in this game");
				return;
			}
		}
		if (nameIt != clientIdByClientName.end())
			clientData->clientId = nameIt->second;
	}

	connectedPlayerCount++;

	// Notify lobby via network link
	if (lobbyLink)
	{
		lobbyLink->SendPlayerEvent(clientData->clientName, "connected");
	}
	// Legacy: update CServerGame tracking via virtual callback
	OnUpdateGameTracking(connectedPlayerCount);
}

void CNetGameServer::NetServerCallbackClientDisconnected(CNetClientData *clientData)
{
	if (isShuttingDown) return;

	LOGD("CNetGameServer::NetServerCallbackClientDisconnected: %s", clientData->clientName.c_str());
	AddLog("Client disconnected: %s", clientData->clientName.c_str());

	authenticatedClients.erase(clientData->clientId);
	clientPacketRates.erase(clientData);

	if (connectedPlayerCount > 0)
		connectedPlayerCount--;

	// Notify lobby via network link
	if (lobbyLink)
	{
		lobbyLink->SendPlayerEvent(clientData->clientName, "disconnected");
	}
	// Legacy: update CServerGame tracking via virtual callback
	OnUpdateGameTracking(connectedPlayerCount);
}

void CNetGameServer::NetServerProcessPacket(CNetPacket *packet)
{
	if (isShuttingDown) return;

	LOGD("CNetGameServer::NetServerProcessPacket: packetType=%4.4x", packet->packetType);

	// Per-client packet rate limiting
	{
		time_t now = time(NULL);
		auto &rate = clientPacketRates[packet->clientData];
		if (difftime(now, rate.windowStart) >= 1.0)
		{
			rate.packetCount = 1;
			rate.windowStart = now;
		}
		else
		{
			rate.packetCount++;
			if (rate.packetCount > MAX_PACKETS_PER_SECOND)
			{
				AddLog("Rate limit exceeded by %s (%d pkt/s), disconnecting",
					   packet->clientData->clientName.c_str(), rate.packetCount);
				clientPacketRates.erase(packet->clientData);
				DisconnectWithError(packet->clientData, "Packet rate limit exceeded");
				return;
			}
		}
	}

	if (packet->packetType == NET_PACKET_TYPE_JSON)
	{
		CNetGamePacketJson *p = (CNetGamePacketJson *)packet;

		// Packet size check
		string packetDump = p->jsonPayload.dump();
		if ((int)packetDump.size() > MAX_PACKET_SIZE)
		{
			AddLog("Oversized packet from %s (%d bytes), disconnecting", packet->clientData->clientName.c_str(), (int)packetDump.size());
			DisconnectWithError(packet->clientData, "Packet too large");
			return;
		}

		LOGD("CNetGameServer::NetServerProcessPacket: received json=%s", packetDump.c_str());

		json j = p->jsonPayload;

		try
		{
			if (!j.contains("action") || !j["action"].is_string())
			{
				DisconnectWithError(packet->clientData, "Missing or invalid action field");
				return;
			}

			string action = j["action"];

			// Protocol version handshake — optional, before authenticate
			if (action == "clientHello")
			{
				string clientProtocol = j.value("protocolVersion", "");
				string serverVersion, rejectReason;

				if (!CheckProtocolVersion(clientProtocol, serverVersion, rejectReason))
				{
					json jReject;
					jReject["action"] = "helloReject";
					jReject["reasonCode"] = "VERSION_MISMATCH";
					jReject["serverProtocolVersion"] = serverVersion;
					jReject["clientProtocolVersion"] = clientProtocol;
					jReject["messageKey"] = "server.version.mismatch";
					jReject["messageParams"] = { {"serverVersion", serverVersion}, {"clientVersion", clientProtocol} };
					jReject["fallbackText"] = "Protocol version mismatch. Please update your client.";
					jReject["localizedText"] = rejectReason;
					jReject["localeUsed"] = "en";
					SendJson(packet->clientData, jReject);
					AddLog("clientHello rejected: %s from %s", rejectReason.c_str(), packet->clientData->clientName.c_str());
					return;
				}

				// Admission gate — check maintenance status
				string admReasonCode, admMessageKey, admLocalizedText, admFallbackText;
				string clientLocale = j.value("locale", "en");
				if (!CheckAdmission(clientLocale, admReasonCode, admMessageKey, admLocalizedText, admFallbackText))
				{
					json jReject;
					jReject["action"] = "helloReject";
					jReject["reasonCode"] = admReasonCode;
					jReject["messageKey"] = admMessageKey;
					jReject["localizedText"] = admLocalizedText;
					jReject["fallbackText"] = admFallbackText;
					jReject["localeUsed"] = clientLocale;
					SendJson(packet->clientData, jReject);
					AddLog("clientHello admission rejected: %s from %s", admReasonCode.c_str(), packet->clientData->clientName.c_str());
					return;
				}

				json jOk;
				jOk["action"] = "helloOk";
				jOk["serverProtocolVersion"] = serverVersion;
				SendJson(packet->clientData, jOk);
				return;
			}

			// Token authentication — must be first action from client
			if (action == "authenticate")
			{
				string token = j.value("connectionToken", "");
				string clientName = packet->clientData->clientName;

				if (token.empty() || token.size() > 64)
				{
					DisconnectWithError(packet->clientData, "Invalid connection token");
					return;
				}

				// Lock tokens for thread-safe access (tokens may be set from lobby thread)
				std::lock_guard<std::mutex> tokenLock(mutexTokens);

				// Tokens remain valid for the entire game session to allow reconnection.
				// Note: tokens may arrive asynchronously via CLobbyLink/CRegistryClient,
				// so if expectedTokens is empty we defer the auth check.

				if (expectedTokens.empty())
				{
					// Tokens not yet delivered — queue for re-processing from NetServerLogic()
					AddLog("Client %s auth deferred — tokens not yet received", clientName.c_str());

					std::lock_guard<std::mutex> lock(pendingAuthMutex);
					PendingAuth pa;
					pa.authJson = j;
					pa.clientData = packet->clientData;
					pa.queuedAt = time(NULL);
					pendingAuthQueue.push_back(pa);
					return;
				}

				// Check token expiry
				if (TOKEN_EXPIRY_SECONDS > 0 && tokenCreationTime > 0)
				{
					double age = difftime(time(NULL), tokenCreationTime);
					if (age > TOKEN_EXPIRY_SECONDS)
					{
						AddLog("Client %s token expired (age=%.0fs, max=%ds)", clientName.c_str(), age, TOKEN_EXPIRY_SECONDS);
						DisconnectWithError(packet->clientData, "Connection token expired");
						return;
					}
				}

				// Resolve clientId from clientName
				auto nameIt2 = clientIdByClientName.find(clientName);
				if (nameIt2 == clientIdByClientName.end())
				{
					DisconnectWithError(packet->clientData, "Unknown client name");
					return;
				}
				int cid = nameIt2->second;

				auto it = expectedTokens.find(cid);
				if (it == expectedTokens.end() || !ConstantTimeStringEqual(it->second, token))
				{
					DisconnectWithError(packet->clientData, "Invalid connection token");
					return;
				}

				authenticatedClients.insert(cid);
				packet->clientData->clientId = cid;
				AddLog("Client %s (id=%d) authenticated successfully", clientName.c_str(), cid);

				// Notify lobby
				if (lobbyLink)
				{
					lobbyLink->SendPlayerEvent(clientName, "authenticated");
				}

				// Send welcome
				json jWelcome;
				jWelcome["action"] = "authenticated";
				jWelcome["gameId"] = gameId;
				SendJson(packet->clientData, jWelcome);

				// Send recent chat history
				if (chatHistory)
				{
					int totalCount = chatHistory->GetTotalCount();
					int fetchCount = min(50, totalCount);
					vector<ChatEntry> recent = chatHistory->GetRecent(fetchCount);
					int startIndex = max(0, totalCount - fetchCount);

					json jHistory;
					jHistory["action"] = "chatHistory";
					jHistory["messages"] = json::array();
					for (const auto &entry : recent)
					{
						jHistory["messages"].push_back(CChatHistory::EntryToJson(entry));
					}
					jHistory["startIndex"] = startIndex;
					jHistory["totalCount"] = totalCount;
					jHistory["hasMore"] = (startIndex > 0);
					SendJson(packet->clientData, jHistory);
				}

				// Notify subclass
				OnPlayerAuthenticated(packet->clientData);
				return;
			}

			// For all other actions, check that client is authenticated (if tokens are in use)
			if (!allowedPlayers.empty() && authenticatedClients.find(packet->clientData->clientId) == authenticatedClients.end())
			{
				if (!expectedTokens.empty() || !authenticatedClients.empty())
				{
					DisconnectWithError(packet->clientData, "Not authenticated");
					return;
				}
			}

			if (action == "chat")
			{
				string message = j.value("message", "");
				if (message.size() > 500) message = message.substr(0, 500);

				LOGD("message: %s", message.c_str());

				string clientName = packet->clientData->clientName;
				string str = "[" + clientName + "] " + message + "\n";
				AddLog(str);

				// Persist to chat history
				if (chatHistory)
				{
					chatHistory->Append(clientName, message);
				}

				j["player"] = clientName;
				j["message"] = message; // use truncated version
				j["timestamp"] = (int64_t)time(NULL);
				BroadcastJson(j);
			}
			else if (action == "requestChatHistory")
			{
				int64_t beforeIndex64 = j.value("beforeIndex", (int64_t)0);
				int count = j.value("count", 50);
				if (count > 200) count = 200;
				if (count < 1) count = 1;

				if (chatHistory)
				{
					int totalCount = chatHistory->GetTotalCount();
					if (beforeIndex64 < 0)
						beforeIndex64 = 0;
					if (beforeIndex64 > (int64_t)totalCount)
						beforeIndex64 = (int64_t)totalCount;
					int beforeIndex = (int)beforeIndex64;
					int startIndex = max(0, beforeIndex - count);
					int fetchCount = beforeIndex - startIndex;

					vector<ChatEntry> msgs;
					if (fetchCount > 0)
						msgs = chatHistory->GetRange(startIndex, fetchCount);

					json jHistory;
					jHistory["action"] = "chatHistory";
					jHistory["messages"] = json::array();
					for (const auto &entry : msgs)
					{
						jHistory["messages"].push_back(CChatHistory::EntryToJson(entry));
					}
					jHistory["startIndex"] = startIndex;
					jHistory["totalCount"] = totalCount;
					jHistory["hasMore"] = (startIndex > 0);
					SendJson(packet->clientData, jHistory);
				}
			}
			else
			{
				// Try game-specific handler
				if (!OnCustomPacket(packet->clientData, action, j))
				{
					LOGWarning("CNetGameServer::NetServerProcessPacket: unknown action=%s", action.c_str());
				}
			}
		}
		catch (exception &ex)
		{
			LOGWarning("CNetGameServer::NetServerProcessPacket: can't parse json error= %s", ex.what());
		}
	}
}

void CNetGameServer::SaveState()
{
	OnSaveState();

	// Reset periodic save timer so the next periodic save doesn't fire right after a proactive/requested save
	lastPeriodicSaveTime = time(NULL);
	LOGD("CNetGameServer::SaveState: state saved for gameId=%s, periodic timer reset", gameId.c_str());
}

void CNetGameServer::Shutdown()
{
	isShuttingDown = true;

	if (netServer)
	{
		// Remove ourselves from the callback list first, so the server thread
		// won't call our virtual methods while we're shutting down
		netServer->RemoveServerCallback(this);

		// Disconnect all connected clients
		for (int i = 0; i < NET_MAX_CLIENTS; i++)
		{
			if (netServer->clients[i]->state == NET_CLIENT_STATE_ONLINE ||
				netServer->clients[i]->state == NET_CLIENT_STATE_CONNECTED)
			{
				netServer->Disconnect(netServer->clients[i]);
			}
		}

		// Signal the server thread to stop
		netServer->status = NET_SERVER_STATUS_SHUTDOWN;

		// Wait for the network thread to finish (max ~2 seconds)
		for (int i = 0; i < 100; i++)
		{
			if (!netServer->isRunning)
				break;
			SYS_Sleep(20);
		}

		if (netServer->isRunning)
		{
			LOGError("CNetGameServer::Shutdown: network thread did not stop in time for gameId=%s", gameId.c_str());
		}
	}

	if (chatHistory)
	{
		delete chatHistory;
		chatHistory = NULL;
	}

	LOGM("CNetGameServer::Shutdown: gameId=%s", gameId.c_str());
}

void CNetGameServer::SendJson(CNetClientData *clientData, json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netServer->IssuePacket(clientData, packet);
	delete packet;
}

void CNetGameServer::BroadcastJson(json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netServer->BroadcastPacket(packet);
	delete packet;
}

void CNetGameServer::DisconnectWithError(CNetClientData *clientData, const char *format, ...)
{
	char *buf = SYS_GetCharBuf();

	va_list args;
	va_start(args, format);
	vsnprintf(buf, MAX_STRING_LENGTH, format, args);
	va_end(args);

	json j;
	j["action"] = "error";
	j["error"] = buf;

	LOGError("CNetGameServer::DisconnectWithError: %s: %s", clientData->clientName.c_str(), buf);

	AddLog(clientData->clientName + " error: " + buf);

	SYS_ReleaseCharBuf(buf);

	SendJson(clientData, j);
	netServer->Disconnect(clientData);
}

void CNetGameServer::AddLog(string str)
{
	LOGD("[game:%s] %s", gameId.c_str(), str.c_str());
	if (logSink)
		logSink->AddLogStr(str);
}

void CNetGameServer::AddLog(const char *fmt, ...)
{
	mutexLog->Lock();
	va_list args;

	va_start(args, fmt);
	vsnprintf(logBuf, MAX_STRING_LENGTH, fmt, args);
	va_end(args);

	size_t l = strlen(logBuf);
	for (int i = 0; i < l; i++)
	{
		unsigned char c = (unsigned char)logBuf[i];
		if (c < 32 && c != 0x0A && c != 0x0D && c != 0x09)
		{
			logBuf[i] = '?';
		}
	}
	logBuf[MAX_STRING_LENGTH - 1] = 0x00;

	logDate->RefreshFromCurrentSystemTime();

	if (logSink)
	{
		logSink->AddLog("%s\n", logBuf);
	}

	LOGD("[game:%s] %s", gameId.c_str(), logBuf);

	mutexLog->Unlock();
}

void CNetGameServer::SetExpectedTokens(const map<int, string> &tokens, const map<string, int> &nameToId, time_t creationTime)
{
	std::lock_guard<std::mutex> lock(mutexTokens);
	expectedTokens = tokens;
	clientIdByClientName = nameToId;
	tokenCreationTime = creationTime;
	LOGM("CNetGameServer::SetExpectedTokens: set %d tokens for gameId=%s", (int)tokens.size(), gameId.c_str());
}

void CNetGameServer::NetServerLogic(CNetServer *netServer)
{
	// Called from network thread loop — update lobbyLink for periodic heartbeats
	if (lobbyLink)
	{
		lobbyLink->Update();
	}

	// Process deferred auth queue
	ProcessPendingAuth();

	// Periodic checkpoint save
	if (periodicSaveIntervalSeconds > 0 && connectedPlayerCount > 0)
	{
		time_t now = time(NULL);
		if (difftime(now, lastPeriodicSaveTime) >= periodicSaveIntervalSeconds)
		{
			lastPeriodicSaveTime = now;
			SaveState();
			LOGD("CNetGameServer: periodic checkpoint save for gameId=%s", gameId.c_str());
		}
	}
}

void CNetGameServer::ProcessPendingAuth()
{
	std::lock_guard<std::mutex> tokenLock(mutexTokens);

	if (expectedTokens.empty())
		return; // still no tokens, nothing to process

	std::lock_guard<std::mutex> lock(pendingAuthMutex);

	vector<PendingAuth> stillPending;

	for (auto &pa : pendingAuthQueue)
	{
		// Check if client is still connected
		if (pa.clientData->state != NET_CLIENT_STATE_ONLINE)
			continue;

		// Check timeout (5 seconds max)
		if (difftime(time(NULL), pa.queuedAt) > 5.0)
		{
			DisconnectWithError(pa.clientData, "Authentication timed out — tokens never arrived");
			continue;
		}

		// Re-attempt auth
		string clientName = pa.clientData->clientName;
		string token = pa.authJson.value("connectionToken", "");

		// Check token expiry
		if (TOKEN_EXPIRY_SECONDS > 0 && tokenCreationTime > 0)
		{
			double age = difftime(time(NULL), tokenCreationTime);
			if (age > TOKEN_EXPIRY_SECONDS)
			{
				AddLog("Client %s token expired during deferred auth (age=%.0fs)", clientName.c_str(), age);
				DisconnectWithError(pa.clientData, "Connection token expired");
				continue;
			}
		}

		auto nameIt = clientIdByClientName.find(clientName);
		if (nameIt == clientIdByClientName.end())
		{
			DisconnectWithError(pa.clientData, "Unknown client name");
			continue;
		}
		int cid = nameIt->second;

		auto it = expectedTokens.find(cid);
		if (it == expectedTokens.end() || !ConstantTimeStringEqual(it->second, token))
		{
			DisconnectWithError(pa.clientData, "Invalid connection token");
			continue;
		}

		authenticatedClients.insert(cid);
		pa.clientData->clientId = cid;
		AddLog("Client %s (id=%d) authenticated successfully (deferred)", clientName.c_str(), cid);

		if (lobbyLink)
		{
			lobbyLink->SendPlayerEvent(clientName, "authenticated");
		}

		json jWelcome;
		jWelcome["action"] = "authenticated";
		jWelcome["gameId"] = gameId;
		SendJson(pa.clientData, jWelcome);

		if (chatHistory)
		{
			int totalCount = chatHistory->GetTotalCount();
			int fetchCount = min(50, totalCount);
			vector<ChatEntry> recent = chatHistory->GetRecent(fetchCount);
			int startIndex = max(0, totalCount - fetchCount);

			json jHistory;
			jHistory["action"] = "chatHistory";
			jHistory["messages"] = json::array();
			for (const auto &entry : recent)
			{
				jHistory["messages"].push_back(CChatHistory::EntryToJson(entry));
			}
			jHistory["startIndex"] = startIndex;
			jHistory["totalCount"] = totalCount;
			jHistory["hasMore"] = (startIndex > 0);
			SendJson(pa.clientData, jHistory);
		}

		OnPlayerAuthenticated(pa.clientData);
	}

	pendingAuthQueue.clear();
}
