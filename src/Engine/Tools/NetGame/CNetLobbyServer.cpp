#include "CNetLobbyServer.h"
#include "CChatHistory.h"
#include "NET_Main.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "CNetPacket.h"
#include "CNetClientData.h"
#include "CSlrDate.h"
#include "json.hpp"
#include "CLobbyServiceServer.h"
#include "CGameServerRegistry.h"
#include "CRegistryServer.h"
#include <algorithm>

using namespace nlohmann;
using namespace std;

// Out-of-class definition required for ODR (used as function argument)
const int CNetLobbyServer::CHAT_HISTORY_PAGE_SIZE;

CNetLobbyServer::CNetLobbyServer(int serverPort)
{
	this->serverPort = serverPort;
	this->config = NULL;
	this->netServer = NULL;
	this->netPackets = NULL;
	this->logSink = NULL;
	this->serviceServer = NULL;
	this->registry = NULL;
	this->gamesManager = NULL;
	this->internalSecret = "";

	mutexLog = new CSlrMutex("CNetLobbyServerLogMutex");
	logDate = new CSlrDate();
	logBuf[0] = 0;
}

CNetLobbyServer::~CNetLobbyServer()
{
	if (lobbyChatHistory)
	{
		delete lobbyChatHistory;
		lobbyChatHistory = nullptr;
	}
}

void CNetLobbyServer::Init(CNetServer *netServer)
{
	this->netServer = netServer;
	this->netServer->AddServerCallback(this);

	this->netPackets = new CNetGamePackets();
	this->netServer->AddPacketCallback(this->netPackets);

	this->netServer->StartServer();
}

void CNetLobbyServer::InitFromHjson(Hjson::Value hjsonRoot)
{
	// Shared secret for internal services
	Hjson::Value secretVal = hjsonRoot["internalSecret"];
	if (secretVal.defined())
	{
		internalSecret = secretVal.to_string();
		LOGM("CNetLobbyServer: internal secret configured (%d bytes)", (int)internalSecret.size());
	}

	// Parse registry mode
	Hjson::Value registryModeVal = hjsonRoot["registryMode"];
	if (registryModeVal.defined())
	{
		string mode = registryModeVal.to_string();
		if (mode == "standalone")
		{
			string registryAddress = "localhost";
			int registryPort = CRegistryServer::DEFAULT_PORT;

			Hjson::Value addrVal = hjsonRoot["registryAddress"];
			if (addrVal.defined())
				registryAddress = addrVal.to_string();

			Hjson::Value portVal = hjsonRoot["registryPort"];
			if (portVal.defined())
				registryPort = (int)portVal.to_int64();

			SwitchToStandaloneRegistry(registryAddress, registryPort);
		}
	}
}

void CNetLobbyServer::SwitchToStandaloneRegistry(const string &registryAddress, int registryPort)
{
	LOGM("CNetLobbyServer::SwitchToStandaloneRegistry: %s:%d", registryAddress.c_str(), registryPort);

	// Shut down embedded service server
	if (serviceServer)
	{
		serviceServer->Shutdown();
		delete serviceServer;
		serviceServer = NULL;
	}

	// Delete old EMBEDDED registry
	if (registry)
	{
		delete registry;
		registry = NULL;
	}

	// Create new STANDALONE registry
	registry = new CGameServerRegistry(registryAddress, registryPort, "lobby-main", internalSecret);

	// Let subclass wire up gamesManager linkage
	OnRegistryCreated(registry);

	// Wait for registry connection
	if (!registry->WaitForConnection(10000))
	{
		LOGError("CNetLobbyServer: failed to connect to standalone registry at %s:%d",
				 registryAddress.c_str(), registryPort);
	}
}

void CNetLobbyServer::StoreToHjson(Hjson::Value hjsonRoot)
{
}

void CNetLobbyServer::SendJson(CNetClientData *clientData, json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netServer->IssuePacket(clientData, packet);
	delete packet;
}

void CNetLobbyServer::BroadcastJson(json sendJson)
{
	CNetGamePacketJson *packet = new CNetGamePacketJson(sendJson);
	netServer->BroadcastPacket(packet);
	delete packet;
}

void CNetLobbyServer::SendError(CNetClientData *clientData, const char *format, ...)
{
	char *buf = SYS_GetCharBuf();

	va_list args;
	va_start(args, format);
	vsnprintf(buf, MAX_STRING_LENGTH, format, args);
	va_end(args);

	json j;
	j["action"] = "error";
	j["error"] = buf;

	LOGError("CNetLobbyServer::SendError: %s: %s", clientData->clientName.c_str(), buf);

	AddLog(clientData->clientName + " error: " + buf);

	SYS_ReleaseCharBuf(buf);

	SendJson(clientData, j);
}

void CNetLobbyServer::SendStartGameFailed(CNetClientData *clientData, const char *format, ...)
{
	char *buf = SYS_GetCharBuf();

	va_list args;
	va_start(args, format);
	vsnprintf(buf, MAX_STRING_LENGTH, format, args);
	va_end(args);

	json j;
	j["action"] = "startGameFailed";
	j["error"] = buf;

	LOGError("CNetLobbyServer::SendStartGameFailed: %s: %s", clientData->clientName.c_str(), buf);

	AddLog(clientData->clientName + " startGameFailed: " + buf);

	SYS_ReleaseCharBuf(buf);

	SendJson(clientData, j);
}

void CNetLobbyServer::DisconnectWithError(CNetClientData *clientData, const char *format, ...)
{
	char *buf = SYS_GetCharBuf();

	va_list args;
	va_start(args, format);
	vsnprintf(buf, MAX_STRING_LENGTH, format, args);
	va_end(args);

	json j;
	j["action"] = "error";
	j["error"] = buf;

	LOGError("CNetLobbyServer::DisconnectWithError: %s: %s", clientData->clientName.c_str(), buf);

	AddLog(clientData->clientName + " error: " + buf);

	SYS_ReleaseCharBuf(buf);

	SendJson(clientData, j);
	netServer->Disconnect(clientData);
}

void CNetLobbyServer::AddLog(string str)
{
	LOGD("CNetLobbyServer::AddLog: %s", str.c_str());
	if (logSink)
	{
		logSink->AddLogStr(str);
		logSink->AddLogStr("\n");
	}
}

void CNetLobbyServer::AddLog(const char *fmt, ...)
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

	LOGD("[SL ] %s", logBuf);

	mutexLog->Unlock();
}

void CNetLobbyServer::SetLobbyChatHistoryPath(const std::string &path)
{
	if (lobbyChatHistory)
	{
		delete lobbyChatHistory;
	}
	lobbyChatHistory = new CChatHistory(path);
	lobbyChatHistory->LoadFromFile();
}

std::string CNetLobbyServer::GetLobbyChatHistoryPath() const
{
	if (lobbyChatHistory)
		return lobbyChatHistory->GetFilePath();
	return "";
}

bool CNetLobbyServer::ProcessChatPacket(CNetClientData *clientData, nlohmann::json &j)
{
	std::string action = j.value("action", "");

	if (action == "chat")
	{
		std::string message = j.value("message", "");
		if (message.empty()) return true;
		if (message.size() > 500) message = message.substr(0, 500);

		// Get sender name from client data
		std::string senderName = clientData->clientName;

		// Persist
		if (lobbyChatHistory)
		{
			lobbyChatHistory->Append(senderName, message);
		}

		// Broadcast to all
		nlohmann::json jBroadcast;
		jBroadcast["action"] = "chat";
		jBroadcast["player"] = senderName;
		jBroadcast["message"] = message;
		jBroadcast["timestamp"] = (int64_t)time(NULL);
		BroadcastJson(jBroadcast);

		AddLog("[%s] %s", senderName.c_str(), message.c_str());
		return true;
	}
	else if (action == "requestChatHistory")
	{
		if (!lobbyChatHistory) return true;

		int64_t beforeIndex64 = j.value("beforeIndex", (int64_t)0);
		int count = j.value("count", CHAT_HISTORY_PAGE_SIZE);
		if (count > 200) count = 200;
		if (count < 1) count = 1;

		int totalCount = lobbyChatHistory->GetTotalCount();
		if (beforeIndex64 < 0) beforeIndex64 = 0;
		if (beforeIndex64 > (int64_t)totalCount) beforeIndex64 = (int64_t)totalCount;
		int beforeIndex = (int)beforeIndex64;
		int startIndex = std::max(0, beforeIndex - count);
		int fetchCount = beforeIndex - startIndex;

		std::vector<ChatEntry> msgs;
		if (fetchCount > 0)
			msgs = lobbyChatHistory->GetRange(startIndex, fetchCount);

		nlohmann::json jHistory;
		jHistory["action"] = "chatHistory";
		jHistory["messages"] = nlohmann::json::array();
		for (const auto &entry : msgs)
		{
			jHistory["messages"].push_back(CChatHistory::EntryToJson(entry));
		}
		jHistory["startIndex"] = startIndex;
		jHistory["totalCount"] = totalCount;
		jHistory["hasMore"] = (startIndex > 0);
		SendJson(clientData, jHistory);
		return true;
	}

	return false;
}

void CNetLobbyServer::SendChatHistoryToClient(CNetClientData *clientData)
{
	if (!lobbyChatHistory) return;

	int totalCount = lobbyChatHistory->GetTotalCount();
	int fetchCount = std::min(CHAT_HISTORY_PAGE_SIZE, totalCount);
	std::vector<ChatEntry> recent = lobbyChatHistory->GetRecent(fetchCount);
	int startIndex = std::max(0, totalCount - fetchCount);

	nlohmann::json jHistory;
	jHistory["action"] = "chatHistory";
	jHistory["messages"] = nlohmann::json::array();
	for (const auto &entry : recent)
	{
		jHistory["messages"].push_back(CChatHistory::EntryToJson(entry));
	}
	jHistory["startIndex"] = startIndex;
	jHistory["totalCount"] = totalCount;
	jHistory["hasMore"] = (startIndex > 0);
	SendJson(clientData, jHistory);
}
