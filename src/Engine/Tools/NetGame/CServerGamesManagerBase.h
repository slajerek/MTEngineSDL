#pragma once

#include "SYS_Defs.h"
#include "SYS_Threading.h"
#include "json.hpp"
#include <string>
#include <vector>
#include <map>
#include <set>

using namespace std;
using namespace nlohmann;

class CServerGame;
class CGameServerRegistry;
class CNetGameServer;
class CNetGameDataProvider;
class ISnapshotStorage;

enum class ServerLaunchMode { IN_PROCESS, SEPARATE_PROCESS, REMOTE_NODE };

// Generic base class for game server orchestration.
// Provides the interface needed by engine-level views (CViewServerAdmin)
// and lobby servers (CNetLobbyServer). Contains all generic game lifecycle
// management: start/stop servers, port allocation, persistence, idle checks.
// Game-specific subclasses override CreateInProcessGameServer() and
// OnGameServerCreated() to add domain logic.
class CServerGamesManagerBase
{
public:
	CServerGamesManagerBase(CNetGameDataProvider *dataProvider);
	virtual ~CServerGamesManagerBase();

	// Max games per player (0 = unlimited)
	int maxGamesPerPlayer;

	// Start/stop server for a game
	CServerGame *StartGameServer(const string &gameId);
	void StopGameServer(const string &gameId);

	// Player rejoin — starts server if needed, returns connection info
	CServerGame *EnsureServerRunning(const string &gameId);

	// Find game by ID
	CServerGame *GetGame(const string &gameId);

	// Snapshot of all managed games (for admin view)
	virtual vector<CServerGame *> GetAllGames();

	// Get all games a player is in
	vector<CServerGame *> GetGamesForPlayer(int playerProfileId);

	// Player leave game — marks slot inactive, checks for game finish
	bool PlayerLeaveGame(const string &gameId, int playerProfileId);

	// Finish a game — broadcasts result, stops server, archives
	void FinishGame(const string &gameId, int winnerProfileId, const string &reason);

	// Active game count for max-games enforcement
	int GetActiveGameCountForPlayer(int playerProfileId);
	bool CanPlayerJoinGame(int playerProfileId);

	// Archive finished game record to finished/ subfolder
	void ArchiveFinishedGame(CServerGame *game);

	// Write JSON history log for finished game
	void WriteGameHistoryLog(CServerGame *game, int winnerProfileId, const string &reason);

	// Process deferred stops (game servers finished during packet processing)
	void ProcessPendingStops();

	// Phase 2: Callback when remote game allocation completes
	void OnRemoteAllocationResult(const string &gameId, bool accepted, const string &nodeId,
								  const string &address, int port, const string &reason);

	// Shared snapshot storage (MinIO for cluster, local-fs for dev). NULL = no upload/download.
	ISnapshotStorage *snapshotStorage;

	// Upload game snapshot to shared storage
	bool UploadSnapshot(const string &gameId);

	// Download game snapshot from shared storage
	bool DownloadSnapshot(const string &gameId);

	// Called when a game is paused (0 connected players, snapshot saved). Override for game-specific notifications.
	virtual void OnGamePaused(CServerGame *game) {}

	// Periodic check — shutdown idle servers
	void UpdateIdleServers();

	// Remove all suspended games, returns count removed
	virtual int RemoveSuspendedGames();

	// Persistence
	void LoadAllGameRecords();
	void SaveGameRecord(CServerGame *game);

	// Generate fresh connection tokens for a game
	void GenerateConnectionTokens(CServerGame *game);

	// --- Virtual factory for in-process game servers ---
	// Subclass creates the game-specific server (e.g., Cthe game appGameServer with CGameState)
	virtual CNetGameServer *CreateInProcessGameServer(CServerGame *game, json &serverConfig) = 0;

	// Called after server is created and basic setup done; subclass adds game-specific setup
	// (chat history, lobby link, etc.)
	virtual void OnGameServerCreated(CServerGame *game, CNetGameServer *server) {}

	// Called when a player leaves a game. Override for game-specific cleanup (resource release, etc.)
	virtual void OnPlayerLeftGame(CServerGame *game, CNetGameServer *server, int clientId) {}

	// Called when a game finishes. Override for game-specific finalization.
	virtual void OnGameFinished(CServerGame *game, int winnerProfileId) {}

	// Game data persistence
	CNetGameDataProvider *dataProvider;

	// Game records
	map<string, CServerGame *> games;   // gameId -> CServerGame
	CSlrMutex *mutex;

	// Inter-server communication: set by CNetLobbyServer
	CGameServerRegistry *registry;

	// Launch mode: IN_PROCESS (default) or SEPARATE_PROCESS
	ServerLaunchMode launchMode;

	int portRangeStart;
	int portRangeEnd;
	int periodicSaveIntervalSeconds;
	static const int IDLE_GRACE_PERIOD_SECONDS = 60;

	// Deferred stops (game servers to stop after packet processing completes)
	vector<string> pendingStops;
	CSlrMutex *mutexPendingStops;

private:
	int AllocatePort();
	void ReleasePort(int port);
	set<int> usedPorts;
};
