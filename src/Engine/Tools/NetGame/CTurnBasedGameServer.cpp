#include "CTurnBasedGameServer.h"
#include "CNetClientData.h"
#include "DBG_Log.h"

#include <algorithm>

using namespace nlohmann;
using namespace std;

CTurnBasedGameServer::CTurnBasedGameServer(json serverConfig, int serverPort)
: CNetGameServer(serverConfig, serverPort)
{
	roundNumber = 0;
	currentPlayerIndex = 0;
	phase = TurnPhase::WAITING_FOR_PLAYERS;

	std::random_device rd;
	rng.seed(rd());
}

CTurnBasedGameServer::~CTurnBasedGameServer()
{
}

const char *CTurnBasedGameServer::GetPhaseName(TurnPhase phase)
{
	switch (phase)
	{
		case TurnPhase::WAITING_FOR_PLAYERS: return "waiting";
		case TurnPhase::PLAYING: return "playing";
		case TurnPhase::PAUSED: return "paused";
		default: return "unknown";
	}
}

void CTurnBasedGameServer::OnPlayerAuthenticated(CNetClientData *clientData)
{
	AddLog("CTurnBasedGameServer: Player %s authenticated, phase=%s", clientData->clientName.c_str(), GetPhaseName(phase));

	if (phase == TurnPhase::PAUSED)
	{
		// Resuming from pause — send current round state to reconnecting player
		json j = BuildRoundStateJson(clientData->clientId);
		SendJson(clientData, j);

		// Check if enough players reconnected to resume
		if ((int)authenticatedClients.size() >= (int)allowedPlayers.size())
		{
			phase = TurnPhase::PLAYING;
			AddLog("CTurnBasedGameServer: All players reconnected, resuming game");
			OnGameResumed();
			BroadcastRoundState();
		}
	}
	else if (phase == TurnPhase::WAITING_FOR_PLAYERS)
	{
		CheckAllPlayersAuthenticated();
	}
	else if (phase == TurnPhase::PLAYING)
	{
		// Late reconnect during play — send current state
		json j = BuildRoundStateJson(clientData->clientId);
		SendJson(clientData, j);
	}
}

void CTurnBasedGameServer::CheckAllPlayersAuthenticated()
{
	if (allowedPlayers.empty())
		return;

	if ((int)authenticatedClients.size() >= (int)allowedPlayers.size())
	{
		AddLog("CTurnBasedGameServer: All %d players authenticated, starting round 1", (int)allowedPlayers.size());
		StartRound();
	}
}

void CTurnBasedGameServer::StartRound()
{
	roundNumber = 1;
	currentPlayerIndex = 0;
	endedTurn.clear();
	phase = TurnPhase::PLAYING;

	ShufflePlayerOrder();

	AddLog("CTurnBasedGameServer: Round %d started, player order:", roundNumber);
	for (int i = 0; i < (int)playerOrder.size(); i++)
	{
		AddLog("  %d: %d%s", i, playerOrder[i], (i == currentPlayerIndex ? " (current)" : ""));
	}

	OnRoundStarted(roundNumber);
	OnTurnStarted(GetCurrentClientId());
	BroadcastRoundState();
}

void CTurnBasedGameServer::ShufflePlayerOrder()
{
	playerOrder.clear();
	for (int cid : allowedPlayers)
	{
		playerOrder.push_back(cid);
	}
	std::shuffle(playerOrder.begin(), playerOrder.end(), rng);
}

void CTurnBasedGameServer::RotatePlayerOrder()
{
	// Move the first player to the end so the next player starts the new round
	if (playerOrder.size() > 1)
	{
		int first = playerOrder.front();
		playerOrder.erase(playerOrder.begin());
		playerOrder.push_back(first);
	}
}

int CTurnBasedGameServer::GetCurrentClientId()
{
	if (currentPlayerIndex >= 0 && currentPlayerIndex < (int)playerOrder.size())
	{
		return playerOrder[currentPlayerIndex];
	}
	return -1;
}

bool CTurnBasedGameServer::IsClientsTurn(int clientId)
{
	return phase == TurnPhase::PLAYING && GetCurrentClientId() == clientId;
}

void CTurnBasedGameServer::AdvanceToNextPlayer()
{
	// Find next player who hasn't ended their turn
	int numPlayers = (int)playerOrder.size();
	for (int i = 1; i <= numPlayers; i++)
	{
		int nextIndex = (currentPlayerIndex + i) % numPlayers;
		if (endedTurn.find(playerOrder[nextIndex]) == endedTurn.end())
		{
			currentPlayerIndex = nextIndex;
			AddLog("CTurnBasedGameServer: Turn passed to %d", GetCurrentClientId());
			OnTurnStarted(GetCurrentClientId());
			return;
		}
	}

	// All players ended their turn — this shouldn't be reached from endTurn handler
	// since we check there, but just in case
	AddLog("CTurnBasedGameServer: All players have ended their turns");
}

bool CTurnBasedGameServer::OnCustomPacket(CNetClientData *clientData, const string &action, json &j)
{
	if (action == "playerAction")
	{
		int cid = clientData->clientId;
		string actionType = j.value("type", "");

		if (!IsClientsTurn(cid) && !ShouldAllowActionFromNonTurnPlayer(clientData, actionType))
		{
			AddLog("CTurnBasedGameServer: Rejected playerAction from %d — not their turn (current: %d)",
				   cid, GetCurrentClientId());
			json jErr;
			jErr["action"] = "error";
			jErr["error"] = "Not your turn";
			SendJson(clientData, jErr);
			return true;
		}

		AddLog("CTurnBasedGameServer: Player %d action: %s", cid, actionType.c_str());

		OnPlayerAction(clientData, actionType, j);
		BroadcastRoundState();
		return true;
	}
	else if (action == "endTurn")
	{
		int cid = clientData->clientId;

		if (!IsClientsTurn(cid))
		{
			AddLog("CTurnBasedGameServer: Rejected endTurn from %d — not their turn (current: %d)",
				   cid, GetCurrentClientId());
			json jErr;
			jErr["action"] = "error";
			jErr["error"] = "Not your turn";
			SendJson(clientData, jErr);
			return true;
		}

		AddLog("CTurnBasedGameServer: Player %d ended turn (round %d)",
			   cid, roundNumber);

		OnTurnEnded(cid);

		// Advance to next round and switch to next player
		int completedRound = roundNumber;
		OnAllTurnsEnded(completedRound);

		roundNumber++;
		endedTurn.clear();
		RotatePlayerOrder();
		currentPlayerIndex = 0;

		AddLog("CTurnBasedGameServer: Round %d started, player order:", roundNumber);
		for (int i = 0; i < (int)playerOrder.size(); i++)
		{
			AddLog("  %d: %d%s", i, playerOrder[i], (i == currentPlayerIndex ? " (current)" : ""));
		}

		OnRoundStarted(roundNumber);
		OnTurnStarted(GetCurrentClientId());

		BroadcastRoundState();
		return true;
	}

	return false;
}

void CTurnBasedGameServer::OnPlayerDeactivated(int clientId)
{
	AddLog("CTurnBasedGameServer::OnPlayerDeactivated: %d", clientId);

	// Remove from allowedPlayers whitelist
	allowedPlayers.erase(clientId);

	// Remove from playerOrder
	auto it = std::find(playerOrder.begin(), playerOrder.end(), clientId);
	if (it != playerOrder.end())
	{
		int removedIndex = (int)std::distance(playerOrder.begin(), it);
		playerOrder.erase(it);

		// Adjust currentPlayerIndex if needed
		if (playerOrder.empty())
		{
			currentPlayerIndex = 0;
		}
		else if (removedIndex < currentPlayerIndex)
		{
			currentPlayerIndex--;
		}
		else if (currentPlayerIndex >= (int)playerOrder.size())
		{
			currentPlayerIndex = 0;
		}
	}

	// Remove from endedTurn set
	endedTurn.erase(clientId);

	// If it was this player's turn, advance
	if (!playerOrder.empty() && phase == TurnPhase::PLAYING)
	{
		// Check if all remaining players ended turn
		if ((int)endedTurn.size() >= (int)playerOrder.size())
		{
			int completedRound = roundNumber;
			OnAllTurnsEnded(completedRound);

			roundNumber++;
			endedTurn.clear();
			RotatePlayerOrder();
			currentPlayerIndex = 0;

			OnRoundStarted(roundNumber);
			OnTurnStarted(GetCurrentClientId());
		}

		BroadcastRoundState();
	}
}

void CTurnBasedGameServer::NetServerCallbackClientDisconnected(CNetClientData *clientData)
{
	// Call base to handle authenticated client tracking, lobby notification, etc.
	CNetGameServer::NetServerCallbackClientDisconnected(clientData);

	if (isShuttingDown) return;

	// Save state on any player disconnect (while still playing)
	if (phase == TurnPhase::PLAYING && connectedPlayerCount >= 0)
	{
		SaveState();
	}

	// Check if all players disconnected — hibernate
	if (phase == TurnPhase::PLAYING && connectedPlayerCount == 0)
	{
		phase = TurnPhase::PAUSED;
		AddLog("CTurnBasedGameServer: All players disconnected, game paused (hibernating)");
		SaveState();  // Final save before hibernation
		OnGamePaused();
	}
}

json CTurnBasedGameServer::BuildRoundStateJson(int forClientId)
{
	json j;
	j["action"] = "roundState";
	j["roundNumber"] = roundNumber;
	j["currentPlayer"] = GetCurrentClientId();
	j["playerOrder"] = playerOrder;
	j["phase"] = GetPhaseName(phase);

	json jEndedTurn = json::array();
	for (const auto &p : endedTurn)
	{
		jEndedTurn.push_back(p);
	}
	j["endedTurn"] = jEndedTurn;

	if (forClientId >= 0)
	{
		j["isYourTurn"] = IsClientsTurn(forClientId);
	}

	return j;
}

void CTurnBasedGameServer::BroadcastRoundState()
{
	// Send per-player messages with isYourTurn flag
	for (int i = 0; i < NET_MAX_CLIENTS; i++)
	{
		CNetClientData *clientData = netServer->clients[i];
		if (clientData->state == NET_CLIENT_STATE_ONLINE &&
			authenticatedClients.find(clientData->clientId) != authenticatedClients.end())
		{
			json j = BuildRoundStateJson(clientData->clientId);
			SendJson(clientData, j);
		}
	}
}

void CTurnBasedGameServer::OnSaveState()
{
	// Subclasses should call this first, then add their own game state
	// The turn state will be serialized by the subclass via SerializeTurnState()
}

void CTurnBasedGameServer::SerializeTurnState(json &j)
{
	j["roundNumber"] = roundNumber;
	j["currentPlayerIndex"] = currentPlayerIndex;
	j["playerOrder"] = playerOrder;
	j["phase"] = GetPhaseName(phase);

	json jEndedTurn = json::array();
	for (const auto &p : endedTurn)
	{
		jEndedTurn.push_back(p);
	}
	j["endedTurn"] = jEndedTurn;
}

bool CTurnBasedGameServer::DeserializeTurnState(json &j)
{
	if (!j.contains("roundNumber"))
		return false;

	roundNumber = j.value("roundNumber", 0);
	currentPlayerIndex = j.value("currentPlayerIndex", 0);

	if (j.contains("playerOrder") && j["playerOrder"].is_array())
	{
		playerOrder.clear();
		for (const auto &p : j["playerOrder"])
		{
			playerOrder.push_back(p.get<int>());
		}
	}

	string phaseStr = j.value("phase", "waiting");
	if (phaseStr == "playing") phase = TurnPhase::PLAYING;
	else if (phaseStr == "paused") phase = TurnPhase::PAUSED;
	else phase = TurnPhase::WAITING_FOR_PLAYERS;

	if (j.contains("endedTurn") && j["endedTurn"].is_array())
	{
		endedTurn.clear();
		for (const auto &p : j["endedTurn"])
		{
			endedTurn.insert(p.get<int>());
		}
	}

	return true;
}
