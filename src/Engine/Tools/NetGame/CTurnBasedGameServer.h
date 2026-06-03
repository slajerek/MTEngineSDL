#pragma once

#include "CNetGameServer.h"
#include <set>
#include <vector>
#include <random>

enum class TurnPhase { WAITING_FOR_PLAYERS, PLAYING, PAUSED };

// Generic turn-based round system for multiplayer games.
// Adds round management on top of CNetGameServer: random starting player
// for round 1, then rotating first-player each subsequent round,
// sequential turns, action/end-turn mechanics, state serialization for
// reconnect/restart, and pause/resume when no players are connected.
// Game servers that don't need rounds keep using plain CNetGameServer.
class CTurnBasedGameServer : public CNetGameServer
{
public:
	CTurnBasedGameServer(json serverConfig, int serverPort);
	virtual ~CTurnBasedGameServer();

	// Turn state
	int roundNumber;
	int currentPlayerIndex;
	std::vector<int> playerOrder;      // clientId order
	std::set<int> endedTurn;
	TurnPhase phase;

	// CNetGameServer overrides
	virtual void OnPlayerAuthenticated(CNetClientData *clientData) override;
	virtual bool OnCustomPacket(CNetClientData *clientData, const std::string &action, json &j) override;
	virtual void OnSaveState() override;
	virtual void NetServerCallbackClientDisconnected(CNetClientData *clientData) override;
	virtual void OnPlayerDeactivated(int clientId) override;

	// Turn management
	void StartRound();
	void AdvanceToNextPlayer();
	void CheckAllPlayersAuthenticated();
	void BroadcastRoundState();
	int GetCurrentClientId();
	bool IsClientsTurn(int clientId);

	// Serialization
	void SerializeTurnState(json &j);
	bool DeserializeTurnState(json &j);

	// Virtual hooks for game-specific behavior
	virtual void OnRoundStarted(int roundNumber) {}
	virtual void OnTurnStarted(int clientId) {}
	virtual void OnPlayerAction(CNetClientData *clientData, const std::string &actionType, json &actionData) {}
	virtual void OnTurnEnded(int clientId) {}
	virtual void OnAllTurnsEnded(int completedRound) {}
	virtual void OnGamePaused() {}
	virtual void OnGameResumed() {}

	// Override to allow specific actions from non-turn players (e.g. combat rolls)
	virtual bool ShouldAllowActionFromNonTurnPlayer(CNetClientData *clientData, const std::string &actionType) { return false; }

protected:
	std::mt19937 rng;
	void ShufflePlayerOrder();
	void RotatePlayerOrder();
	json BuildRoundStateJson(int forClientId = -1);

	static const char *GetPhaseName(TurnPhase phase);
};
