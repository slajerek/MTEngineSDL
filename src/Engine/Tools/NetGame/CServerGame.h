#pragma once

#include "SYS_Defs.h"
#include "json.hpp"
#include <string>
#include <vector>
#include <ctime>
#ifdef _WIN32
#include <process.h>
typedef int pid_t;
#else
#include <sys/types.h>
#endif

using namespace std;
using namespace nlohmann;

class CNetGameServer;

class CServerGame
{
public:
	CServerGame();
	virtual ~CServerGame();

	// Identity
	string gameId;
	string mapName;     // generic name (was scenarioName)
	int mapId;          // generic id (was scenarioId)
	string projectPath; // relative path to project file (e.g. "projects/project08.json")

	// Players in this game
	struct PlayerSlot {
		int playerProfileId;
		string playerName;
		int roleId;     // generic role (was factionId)
		bool isActive = true;  // false when player has left the game
	};
	vector<PlayerSlot> playerSlots;

	// Server state
	string serverAddress;   // "localhost" for local
	int serverPort;         // 0 = not running
	CNetGameServer *gameServer;  // NULL = suspended or remote process
	pid_t processId;        // non-zero when running as separate process
	bool isRemoteProcess;   // true when gameServer==NULL but process is running
	string remoteNodeId;    // set when running on a remote node (REMOTE_NODE mode)

	// Persistence
	string stateFilePath;   // "data/games/{gameId}_state.json"

	// Lifecycle
	// STOPPING: an in-flight stop (StopGameServer()/UpdateIdleServers())
	// detached the server and released the manager mutex for the blocking
	// network stop. It is set under the mutex before unlocking and cleared
	// under the mutex once the stop completes, ONLY if the record is still
	// STOPPING at that point -- if something else (a restart) changed it
	// away from STOPPING in the meantime, the stopper leaves the record
	// alone and only does its own port bookkeeping. Relies on the existing
	// invariant that StopGameServer()/UpdateIdleServers() are only ever
	// called from the main thread, never concurrently for the same record.
	// Any path that might (re)start or archive a record must treat STOPPING
	// as busy: refuse/wait, never start, never archive.
	enum Status { STARTING, RUNNING, STOPPING, SUSPENDED, FINISHED };
	Status status;
	bool isFinishing;	// Guard against concurrent FinishGame double-call
	time_t lastActivityTime;
	int connectedPlayerCount;

	// Connection tokens: clientId (playerProfileId) -> token
	map<int, string> connectionTokens;

	// Client name to ID mapping (for auth lookup on game server)
	map<string, int> clientIdByClientName;

	// Active player helpers
	int GetActivePlayerCount();
	string GetLastActivePlayerName();
	int GetLastActivePlayerProfileId();

	void Serialize(json &j);
	bool Deserialize(json &j);
	static string GenerateGameId();
	static string GenerateConnectionToken();

	const char *GetStatusName();
};
