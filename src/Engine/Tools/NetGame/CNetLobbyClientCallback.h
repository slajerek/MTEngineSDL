#pragma once

#include <string>
using namespace std;

class CNetLobbyClient;

class CNetLobbyClientCallback
{
public:
	virtual ~CNetLobbyClientCallback() {}
	virtual void LobbyClientCallbackConnected(CNetLobbyClient *client) {}
	virtual void LobbyClientCallbackConnectionFailed(CNetLobbyClient *client) {}
	virtual void LobbyClientCallbackJoinableGamesUpdated(CNetLobbyClient *client) {}
	virtual void LobbyClientCallbackGameIdSet(CNetLobbyClient *client, int gameId) {}
	virtual void LobbyClientCallbackGameCancelled(CNetLobbyClient *client) {}
	virtual void LobbyClientCallbackError(CNetLobbyClient *client, const char *error) {}
	virtual void LobbyClientCallbackDisconnected(CNetLobbyClient *client) {}
	virtual void LobbyClientCallbackActiveGamesUpdated(CNetLobbyClient *client) {}
	virtual void LobbyClientCallbackGameReady(CNetLobbyClient *client, const string &gameId, const string &address, int port, const string &connectionToken) {}
	virtual void LobbyClientCallbackLeaveGameResult(CNetLobbyClient *client, const string &gameId, bool success, const string &error) {}
	virtual void LobbyClientCallbackGameFinished(CNetLobbyClient *client, const string &gameId, const string &winnerName, const string &reason) {}
};
