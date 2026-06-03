#include "CNetGameClient.h"
#include "CGuiViewMessages.h"
#include "CSlrDate.h"
using namespace std;

CNetGameClient::CNetGameClient(int componentId, const char *serverAddress, int serverConnectPort, const char *clientLoginName, vector<u8> passwordHash, CGuiViewMessages *messagesLog, const string &connectionToken)
{
	this->messagesLog = messagesLog;
	this->isAuthenticated = false;
	this->roundNumber = 0;
	this->isMyTurn = false;
	this->turnPhase = "waiting";
	this->oldestLoadedIndex = 0;
	this->totalChatCount = 0;
	this->hasMoreHistory = false;
	this->isLoadingHistory = false;

	mutexLog = new CSlrMutex("CNetGameClientLogMutex");
	logDate = new CSlrDate();
	memset(logBuf, 0, MAX_STRING_LENGTH);

	this->netClient = new CNetClient(NULL, NULL, serverAddress, serverConnectPort,
									 NET_PROTOCOL_TYPE_NET_GAME, clientLoginName, passwordHash);

	// Set token BEFORE Init() triggers Connect() — otherwise the network
	// thread can fire NetClientCallbackConnected before the token arrives,
	// and no authenticate packet is ever sent.
	this->connectionToken = connectionToken;
	this->Init(netClient, componentId);
}

CNetGameClient::CNetGameClient(CNetClient *netClient, int componentId, CGuiViewMessages *messagesLog, const string &connectionToken)
{
	this->messagesLog = messagesLog;
	this->isAuthenticated = false;
	this->roundNumber = 0;
	this->isMyTurn = false;
	this->turnPhase = "waiting";
	this->oldestLoadedIndex = 0;
	this->totalChatCount = 0;
	this->hasMoreHistory = false;
	this->isLoadingHistory = false;

	mutexLog = new CSlrMutex("CNetGameClientLogMutex");
	logDate = new CSlrDate();
	memset(logBuf, 0, MAX_STRING_LENGTH);

	this->connectionToken = connectionToken;
	this->Init(netClient, componentId);
}

void CNetGameClient::Init(CNetClient *netClient, int componentId)
{
	LOGD("CNetGameClient::Init componentId=%d", componentId);
	this->clientId = componentId;

	this->netClient = netClient;
	this->netClient->AddClientCallback(this);

	this->netPackets = new CNetGamePackets();
	this->netClient->AddPacketCallback(this->netPackets);

	this->netClient->Connect();
}

void CNetGameClient::IssuePacket(bool isReliable, CNetPacket *packet)
{
	this->netClient->IssuePacket(isReliable, packet);
}

void CNetGameClient::NetClientCallbackConnected(CNetClient *netClient)
{
	LOGM("CNetGameClient::NetClientCallbackConnected: clientId=%d", clientId);
	AddLog("Connected to game server");

	// Auto-send authentication if we have a connection token
	if (!connectionToken.empty())
	{
		json j;
		j["action"] = "authenticate";
		j["connectionToken"] = connectionToken;
		SendJson(j);
		AddLog("Sending authentication token...");
	}
}

void CNetGameClient::NetClientCallbackDisconnected(CNetClient *netClient)
{
	LOGM("CNetGameClient::NetClientCallbackDisconnected: clientId=%d", clientId);
	isAuthenticated = false;
	AddLog("Disconnected from game server");
}

void CNetGameClient::RequestMoreHistory()
{
	if (isLoadingHistory || !hasMoreHistory) return;

	isLoadingHistory = true;

	json j;
	j["action"] = "requestChatHistory";
	j["beforeIndex"] = oldestLoadedIndex;
	j["count"] = 50;
	SendJson(j);
}

void CNetGameClient::NetClientProcessPacket(CNetPacket *packet)
{
	if (packet->packetType == NET_PACKET_TYPE_JSON)
	{
		CNetGamePacketJson *p = (CNetGamePacketJson *)packet;
		LOGD("CNetGameClient::NetClientProcessPacket: received json=%s", p->jsonPayload.dump());
		json j = p->jsonPayload;

		string action = j["action"];
		if (action == "error")
		{
			string error = j["error"];
			string msg = "Error: " + error;
			AddLog(msg);
		}
		else if (action == "authenticated")
		{
			isAuthenticated = true;
			string receivedGameId = j.value("gameId", "");
			AddLog("Authenticated for game %s", receivedGameId.c_str());
		}
		else if (action == "chatHistory")
		{
			auto messages = j["messages"];
			int startIndex = j.value("startIndex", 0);
			int serverTotalCount = j.value("totalCount", 0);
			bool serverHasMore = j.value("hasMore", false);

			vector<ChatEntry> received;
			for (const auto &m : messages)
			{
				received.push_back(CChatHistory::JsonToEntry(m));
			}

			if (chatMessages.empty())
			{
				// Initial load
				chatMessages = received;
			}
			else
			{
				// Prepend older messages
				chatMessages.insert(chatMessages.begin(), received.begin(), received.end());
			}

			oldestLoadedIndex = startIndex;
			totalChatCount = serverTotalCount;
			hasMoreHistory = serverHasMore;
			isLoadingHistory = false;

			// Also show in system log
			for (const auto &entry : received)
			{
				AddLog("[%s] %s", entry.player.c_str(), entry.message.c_str());
			}
		}
		else if (action == "roundState")
		{
			roundNumber = j.value("roundNumber", 0);
			currentPlayerClientId = j.value("currentPlayer", -1);
			isMyTurn = j.value("isYourTurn", false);
			turnPhase = j.value("phase", "waiting");

			playerOrder.clear();
			if (j.contains("playerOrder") && j["playerOrder"].is_array())
			{
				for (const auto &p : j["playerOrder"])
				{
					playerOrder.push_back(p.get<int>());
				}
			}

			AddLog("Round %d | Current: %d | Phase: %s | MyTurn: %s",
				   roundNumber, currentPlayerClientId, turnPhase.c_str(),
				   isMyTurn ? "yes" : "no");
		}
		else if (action == "chat")
		{
			string player = j["player"];
			string message = j["message"];
			int64_t timestamp = j.value("timestamp", (int64_t)0);

			ChatEntry entry;
			entry.player = player;
			entry.message = message;
			entry.timestamp = timestamp;
			chatMessages.push_back(entry);
			totalChatCount++;

			string str = "[" + player + "] " + message + "\n";
			AddLog(str);
		}
	}
}

void CNetGameClient::SendJson(json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	IssuePacket(true, packet);
	delete packet;
}

void CNetGameClient::AddLog(string str)
{
	LOGD("CNetGameClient::AddLog: %s", str);
	if (messagesLog)
		messagesLog->AddLog(str.c_str());
}

void CNetGameClient::AddLog(const char* fmt, ...)
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
	logBuf[MAX_STRING_LENGTH-1] = 0x00;

	logDate->RefreshFromCurrentSystemTime();

	if (messagesLog)
	{
		messagesLog->AddLog("%s\n", logBuf);
	}

	LOGD("[C%d] %s", clientId, logBuf);

	mutexLog->Unlock();
}
