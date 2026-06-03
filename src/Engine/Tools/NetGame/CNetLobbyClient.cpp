#include "CNetLobbyClient.h"
#include "CGuiViewMessages.h"
#include "CSlrDate.h"
using namespace std;

CNetLobbyClient::CNetLobbyClient(int componentId, const char *serverAddress, int serverConnectPort, string clientLoginName, vector<u8> passwordHash, CGuiViewMessages *messagesLog)
{
	this->messagesLog = messagesLog;

	mutexLog = new CSlrMutex("CNetLobbyClientLogMutex");
	logDate = new CSlrDate();
	memset(logBuf, 0, MAX_STRING_LENGTH);

	joinableGameId = -1;

	this->netClient = new CNetClient(NULL, NULL, serverAddress, serverConnectPort,
									 NET_PROTOCOL_TYPE_NET_GAME, clientLoginName, passwordHash);

	this->Init(netClient, componentId);
}

CNetLobbyClient::CNetLobbyClient(CNetClient *netClient, int componentId, CGuiViewMessages *messagesLog)
{
	mutexLog = new CSlrMutex("CNetLobbyClientLogMutex");
	logDate = new CSlrDate();
	memset(logBuf, 0, MAX_STRING_LENGTH);

	this->messagesLog = messagesLog;
	joinableGameId = -1;

	this->Init(netClient, componentId);
}

void CNetLobbyClient::Init(CNetClient *netClient, int componentId)
{
	LOGD("CNetLobbyClient::Init componentId=%d", componentId);
	this->clientId = componentId;

	this->netClient = netClient;
	this->netClient->AddClientCallback(this);

	this->netPackets = new CNetGamePackets();
	this->netClient->AddPacketCallback(this->netPackets);

	this->netClient->Connect();
}

void CNetLobbyClient::IssuePacket(bool isReliable, CNetPacket *packet)
{
	this->netClient->IssuePacket(isReliable, packet);
}

void CNetLobbyClient::NetClientProcessPacket(CNetPacket *packet)
{
	if (packet->packetType == NET_PACKET_TYPE_JSON)
	{
		try {
			CNetGamePacketJson *p = (CNetGamePacketJson *)packet;
			LOGD("CNetLobbyClient::NetClientProcessPacket: received json=%s", p->jsonPayload.dump());
			json j = p->jsonPayload;

			string action = j["action"];
			if (action == "error")
			{
				string error = j["error"];
				string msg = "Error: " + error;
				AddLog(msg);

				auto callbacksCopy = netLobbyClientCallbacks;
				for (auto callback : callbacksCopy)
				{
					callback->LobbyClientCallbackError(this, error.c_str());
				}
			}
			else if (action == "chat")
			{
				ChatEntry entry;
				entry.player = j.value("player", "");
				entry.message = j.value("message", "");
				entry.timestamp = j.value("timestamp", (int64_t)0);
				chatMessages.push_back(entry);

				std::string str = "[" + entry.player + "] " + entry.message + "\n";
				AddLog(str);
			}
			else if (action == "chatHistory")
			{
				std::vector<ChatEntry> received;
				if (j.contains("messages"))
				{
					for (auto &jm : j["messages"])
					{
						received.push_back(CChatHistory::JsonToEntry(jm));
					}
				}

				int startIndex = j.value("startIndex", 0);
				totalChatCount = j.value("totalCount", 0);
				bool hasMore = j.value("hasMore", false);

				if (chatMessages.empty())
				{
					// Initial load — just set messages
					chatMessages = received;
					oldestLoadedIndex = startIndex;
				}
				else
				{
					// Prepend older messages
					chatMessages.insert(chatMessages.begin(), received.begin(), received.end());
					oldestLoadedIndex = startIndex;
				}

				hasMoreHistory = hasMore;
				isLoadingHistory = false;
			}
			else if (action == "playerProfile")
			{
				OnPlayerProfilePacket(j);
			}
			else if (action == "joinableGames")
			{
				LOGD("CNetLobbyClient::NetClientProcessPacket: joinableGames");

				OnJoinableGamesPacket(j);

				auto callbacksCopy1 = netLobbyClientCallbacks;
				for (auto callback : callbacksCopy1)
				{
					callback->LobbyClientCallbackJoinableGamesUpdated(this);
				}
			}
			else if (action == "setJoinableGameId")
			{
				joinableGameId = j["joinableGameId"];

				auto callbacksCopy2 = netLobbyClientCallbacks;
				for (auto callback : callbacksCopy2)
				{
					callback->LobbyClientCallbackGameIdSet(this, joinableGameId);
				}
			}
			else if (action == "startGameFailed")
			{
				string error = j["error"];
				string msg = "Start game failed: " + error;
				AddLog(msg);

				OnStartGameFailed(error);

				auto callbacksCopy3 = netLobbyClientCallbacks;
				for (auto callback : callbacksCopy3)
				{
					callback->LobbyClientCallbackError(this, error.c_str());
				}
			}
			else if (action == "gameStarting")
			{
				LOGM("CNetLobbyClient: game is starting");
				AddLog("Game is starting...");

				OnGameStarting();
			}
			else if (action == "myActiveGames")
			{
				LOGD("CNetLobbyClient: received myActiveGames");
				activeGames.clear();

				if (j.contains("games"))
				{
					for (auto &jg : j["games"])
					{
						CActiveGameInfo info;
						info.gameId = jg.value("gameId", "");
						// Backward-compat: read both new and old field names
						if (jg.contains("mapName"))
							info.mapName = jg.value("mapName", "");
						else
							info.mapName = jg.value("scenarioName", "");
						info.status = jg.value("status", "");
						if (jg.contains("players"))
						{
							for (auto &jp : jg["players"])
							{
								info.playerNames.push_back(jp.get<string>());
							}
						}
						info.extraData = jg;
						activeGames.push_back(info);
					}
				}

				AddLog("Received %d active games", (int)activeGames.size());

				auto callbacksCopyAG = netLobbyClientCallbacks;
				for (auto callback : callbacksCopyAG)
				{
					callback->LobbyClientCallbackActiveGamesUpdated(this);
				}
			}
			else if (action == "gameReady")
			{
				string gameId = j.value("gameId", "");
				string serverAddress = j.value("serverAddress", "");
				int serverPort = j.value("serverPort", 0);
				string connectionToken = j.value("connectionToken", "");

				LOGM("CNetLobbyClient: gameReady gameId=%s address=%s port=%d", gameId.c_str(), serverAddress.c_str(), serverPort);
				AddLog("Game ready! Server: %s:%d", serverAddress.c_str(), serverPort);

				auto callbacksCopyGR = netLobbyClientCallbacks;
				for (auto callback : callbacksCopyGR)
				{
					callback->LobbyClientCallbackGameReady(this, gameId, serverAddress, serverPort, connectionToken);
				}
			}
			else if (action == "cancelGameJoin")
			{
				joinableGameId = -1;

				OnGameCancelled();

				auto callbacksCopy4 = netLobbyClientCallbacks;
				for (auto callback : callbacksCopy4)
				{
					callback->LobbyClientCallbackGameCancelled(this);
				}
			}
			else if (action == "leaveGameResult")
			{
				string lgGameId = j.value("gameId", "");
				bool success = j.value("success", false);
				string error = j.value("error", "");

				if (success)
					AddLog("Left game %s", lgGameId.c_str());
				else
					AddLog("Leave game failed: %s", error.c_str());

				auto callbacksCopyLG = netLobbyClientCallbacks;
				for (auto callback : callbacksCopyLG)
				{
					callback->LobbyClientCallbackLeaveGameResult(this, lgGameId, success, error);
				}
			}
			else if (action == "gameFinished")
			{
				string gfGameId = j.value("gameId", "");
				string winner = j.value("winner", "");
				string reason = j.value("reason", "");

				AddLog("Game %s finished — winner: %s (%s)", gfGameId.c_str(), winner.c_str(), reason.c_str());

				auto callbacksCopyGF = netLobbyClientCallbacks;
				for (auto callback : callbacksCopyGF)
				{
					callback->LobbyClientCallbackGameFinished(this, gfGameId, winner, reason);
				}
			}
			else
			{
				LOGError("CNetLobbyClient::NetClientProcessPacket: unknown action=%s", action.c_str());
				netClient->SetStatusDisconnectAndReconnect();
			}
		}
		catch (const json::parse_error& e)
		{
			LOGError("CNetLobbyClient::NetClientProcessPacket: parse error %s", e.what());
			netClient->SetStatusDisconnectAndReconnect();
		}
	}
}

void CNetLobbyClient::SendJson(json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	IssuePacket(true, packet);
	delete packet;
}

void CNetLobbyClient::NetClientCallbackConnected(CNetClient *netClient)
{
	LOGD("CNetLobbyClient::NetClientCallbackConnected");
	AddLog("Connected successfully.");

	auto callbacksCopy = netLobbyClientCallbacks;
	for (auto callback : callbacksCopy)
	{
		callback->LobbyClientCallbackConnected(this);
	}
}

void CNetLobbyClient::NetClientCallbackDisconnected(CNetClient *netClient)
{
	LOGD("CNetLobbyClient::NetClientCallbackDisconnected");

	joinableGameId = -1;
	activeGames.clear();
	ClearChatState();

	OnDisconnectedFromLobby();

	AddLog("Disconnected from lobby server.");

	auto callbacksCopyDisc = netLobbyClientCallbacks;
	for (auto callback : callbacksCopyDisc)
	{
		callback->LobbyClientCallbackDisconnected(this);
	}
}

void CNetLobbyClient::NetClientCallbackNotAuthorized(CNetClient *netClient)
{
	LOGWarning("CNetLobbyClient::NetClientCallbackNotAuthorized");
	AddLog("Login failed. Wrong password.");

	auto callbacksCopyAuth = netLobbyClientCallbacks;
	for (auto callback : callbacksCopyAuth)
	{
		callback->LobbyClientCallbackConnectionFailed(this);
	}
}

void CNetLobbyClient::SendChatMessage(const std::string &msg)
{
	json j;
	j["action"] = "chat";
	j["message"] = msg;
	SendJson(j);
}

void CNetLobbyClient::RequestMoreChatHistory()
{
	if (isLoadingHistory || !hasMoreHistory) return;

	isLoadingHistory = true;

	json j;
	j["action"] = "requestChatHistory";
	j["beforeIndex"] = oldestLoadedIndex;
	j["count"] = 50;
	SendJson(j);
}

void CNetLobbyClient::ClearChatState()
{
	chatMessages.clear();
	oldestLoadedIndex = 0;
	totalChatCount = 0;
	hasMoreHistory = false;
	isLoadingHistory = false;
}

void CNetLobbyClient::AddLog(string str)
{
	LOGD("CNetLobbyClient::AddLog: %s", str.c_str());
	if (messagesLog)
	{
		messagesLog->AddLog(str.c_str());
		messagesLog->AddLog("\n");
	}
}

void CNetLobbyClient::AddLog(const char* fmt, ...)
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

	LOGD("[CL%d] %s", clientId, logBuf);

	mutexLog->Unlock();
}

void CNetLobbyClient::AddNetLobbyClientCallback(CNetLobbyClientCallback *callback)
{
	netLobbyClientCallbacks.push_back(callback);
}

void CNetLobbyClient::RemoveNetLobbyClientCallback(CNetLobbyClientCallback *callback)
{
	netLobbyClientCallbacks.remove(callback);
}
