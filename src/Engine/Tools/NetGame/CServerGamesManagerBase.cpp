#include "CServerGamesManagerBase.h"
#include "CServerGame.h"
#include "CNetGameServer.h"
#include "CNetGameDataProvider.h"
#include "CSnapshotStorage.h"
#include "SYS_FileSystem.h"
#include "DBG_Log.h"
#include "CLobbyLink.h"
#include "CLobbyServiceServer.h"
#include "CGameServerRegistry.h"
#include "CRegistryServer.h"
#include "CRegistryClient.h"
#include "CProcessSpawner.h"

#include <filesystem>
#include <fstream>

using namespace std;

CServerGamesManagerBase::CServerGamesManagerBase(CNetGameDataProvider *dataProvider)
{
	this->dataProvider = dataProvider;
	registry = NULL;
	snapshotStorage = NULL;
	launchMode = ServerLaunchMode::IN_PROCESS;
	maxGamesPerPlayer = 5;
	portRangeStart = 14701;
	portRangeEnd = 14799;
	periodicSaveIntervalSeconds = 60;
	mutex = new CSlrMutex("CServerGamesManagerBaseMutex");
	mutexPendingStops = new CSlrMutex("CServerGamesManagerPendingStops");
	LoadAllGameRecords();
}

CServerGamesManagerBase::~CServerGamesManagerBase()
{
	for (auto &pair : games)
	{
		CServerGame *game = pair.second;
		if (game->isRemoteProcess && game->processId > 0)
		{
			CProcessSpawner::Kill(game->processId);
			CProcessSpawner::TryWait(game->processId, nullptr);
		}
		else if (game->gameServer)
		{
			if (game->gameServer->lobbyLink)
			{
				game->gameServer->lobbyLink->Disconnect();
				delete game->gameServer->lobbyLink;
				game->gameServer->lobbyLink = NULL;
			}
			game->gameServer->Shutdown();
		}
		delete game;
	}
	games.clear();
	delete mutex;
	delete mutexPendingStops;
}

CServerGame *CServerGamesManagerBase::StartGameServer(const string &gameId)
{
	mutex->Lock();

	auto it = games.find(gameId);
	if (it == games.end())
	{
		LOGError("CServerGamesManagerBase::StartGameServer: gameId=%s not found", gameId.c_str());
		mutex->Unlock();
		return NULL;
	}

	CServerGame *game = it->second;

	if (game->gameServer != NULL || (game->isRemoteProcess && game->processId > 0))
	{
		LOGD("CServerGamesManagerBase::StartGameServer: gameId=%s already running on port %d", gameId.c_str(), game->serverPort);
		mutex->Unlock();
		return game;
	}

	// REMOTE_NODE mode: request allocation from Registry
	if (launchMode == ServerLaunchMode::REMOTE_NODE)
	{
		if (!registry)
		{
			LOGError("CServerGamesManagerBase::StartGameServer: REMOTE_NODE mode requires registry");
			mutex->Unlock();
			return NULL;
		}

		game->status = CServerGame::STARTING;
		game->lastActivityTime = time(NULL);
		game->isRemoteProcess = true;

		// Generate connection tokens now (they'll be pushed to the game server via registry when it registers)
		GenerateConnectionTokens(game);

		LOGM("CServerGamesManagerBase::StartGameServer: gameId=%s requesting remote allocation", gameId.c_str());
		mutex->Unlock();

		registry->RequestAllocateGameServer(gameId, game->mapName, game->GetActivePlayerCount(), false);
		return game;
	}

	int port = AllocatePort();
	if (port == 0)
	{
		LOGError("CServerGamesManagerBase::StartGameServer: no ports available");
		mutex->Unlock();
		return NULL;
	}

	game->serverPort = port;
	game->serverAddress = "localhost";

	if (launchMode == ServerLaunchMode::SEPARATE_PROCESS)
	{
		// Spawn as a separate process
		string execPath = CProcessSpawner::GetExecutablePath();
		vector<string> args = {
			execPath,
			"--game-server",
			"--game-id", gameId,
			"--port", to_string(port),
			"--lobby-address", "localhost",
			"--app-path", string(gPathToResources),
			"--headless"
		};

		// Internal services shared secret (required).
		string internalSecret;
		if (registry)
			internalSecret = registry->internalSecret;
		if (internalSecret.empty())
		{
			LOGError("CServerGamesManagerBase::StartGameServer: missing internal secret (set CGameServerRegistry::internalSecret)");
			ReleasePort(port);
			game->serverPort = 0;
			mutex->Unlock();
			return NULL;
		}
		// Internal secret passed via environment variable (avoids ps aux exposure).
		vector<string> envVars = {"LH_INTERNAL_SECRET=" + internalSecret};

		// Determine whether to connect to embedded service server or standalone registry
		if (registry && registry->mode == RegistryMode::STANDALONE && registry->registryClient)
		{
			// Get actual registry port from the client's connection
			int registryPort = registry->registryClient->netClient->serverPort;
			args.push_back("--registry-address");
			args.push_back("localhost");
			args.push_back("--registry-port");
			args.push_back(to_string(registryPort));
		}
		else
		{
			args.push_back("--lobby-service-port");
			args.push_back(to_string(CLobbyServiceServer::SERVICE_PORT));
		}

		pid_t pid = CProcessSpawner::SpawnWithEnv(args, envVars);
		if (pid <= 0)
		{
			LOGError("CServerGamesManagerBase::StartGameServer: failed to spawn game server process for gameId=%s", gameId.c_str());
			ReleasePort(port);
			game->serverPort = 0;
			mutex->Unlock();
			return NULL;
		}

		game->processId = pid;
		game->isRemoteProcess = true;
		game->gameServer = NULL;

		// Generate connection tokens (will be pushed via registry when game server registers)
		GenerateConnectionTokens(game);

		game->status = CServerGame::RUNNING;
		game->lastActivityTime = time(NULL);
		game->connectedPlayerCount = 0;

		LOGM("CServerGamesManagerBase::StartGameServer: gameId=%s spawned as pid=%d on port %d", gameId.c_str(), pid, port);

		mutex->Unlock();
		return game;
	}

	// IN_PROCESS mode: use virtual factory to create game server

	// Build server config
	json serverConfig;
	serverConfig["serverPort"] = port;
	serverConfig["periodicSaveIntervalSeconds"] = periodicSaveIntervalSeconds;

	// Create the game server via virtual factory
	CNetGameServer *server = CreateInProcessGameServer(game, serverConfig);
	if (server == NULL)
	{
		LOGError("CServerGamesManagerBase::StartGameServer: CreateInProcessGameServer returned NULL for gameId=%s", gameId.c_str());
		ReleasePort(port);
		game->serverPort = 0;
		mutex->Unlock();
		return NULL;
	}

	game->gameServer = server;
	server->gameId = gameId;
	server->manager = this;

	// Set up allowed players whitelist
	for (const auto &slot : game->playerSlots)
	{
		server->allowedPlayers.insert(slot.playerProfileId);
		server->clientIdByClientName[slot.playerName] = slot.playerProfileId;
	}

	// Let subclass do game-specific setup (lobby link, chat history, etc.)
	OnGameServerCreated(game, server);

	// Generate connection tokens
	GenerateConnectionTokens(game);

	game->status = CServerGame::RUNNING;
	game->lastActivityTime = time(NULL);
	game->connectedPlayerCount = 0;

	LOGM("CServerGamesManagerBase::StartGameServer: gameId=%s started in-process on port %d", gameId.c_str(), port);

	mutex->Unlock();
	return game;
}

void CServerGamesManagerBase::StopGameServer(const string &gameId)
{
	mutex->Lock();

	auto it = games.find(gameId);
	if (it == games.end())
	{
		mutex->Unlock();
		return;
	}

	CServerGame *game = it->second;

	if (game->isRemoteProcess && game->processId > 0)
	{
		// Separate process mode: send shutdown via registry, then kill if needed
		LOGM("CServerGamesManagerBase::StopGameServer: gameId=%s shutting down remote process pid=%d", gameId.c_str(), game->processId);

		if (registry)
			registry->RequestShutdown(gameId, "stop_requested");

		// Wait for process to exit (up to 5 seconds)
		bool exited = false;
		for (int i = 0; i < 50; i++)
		{
			if (CProcessSpawner::TryWait(game->processId, nullptr))
			{
				exited = true;
				break;
			}
			mutex->Unlock();
			SYS_Sleep(100);
			mutex->Lock();
		}

		if (!exited)
		{
			LOGM("CServerGamesManagerBase::StopGameServer: gameId=%s process did not exit, killing", gameId.c_str());
			CProcessSpawner::Kill(game->processId);
			CProcessSpawner::TryWait(game->processId, nullptr);
		}

		game->processId = 0;
		game->isRemoteProcess = false;
	}
	else if (game->gameServer)
	{
		// In-process mode: direct shutdown
		LOGM("CServerGamesManagerBase::StopGameServer: gameId=%s saving state and stopping", gameId.c_str());

		// Shutdown lobbyLink first (stops network thread)
		if (game->gameServer->lobbyLink)
		{
			game->gameServer->lobbyLink->Shutdown();
			delete game->gameServer->lobbyLink;
			game->gameServer->lobbyLink = NULL;
		}

		game->gameServer->SaveState();
		game->gameServer->Shutdown();
		delete game->gameServer;
		game->gameServer = NULL;
	}

	ReleasePort(game->serverPort);
	game->serverPort = 0;
	game->status = CServerGame::SUSPENDED;
	game->connectedPlayerCount = 0;
	game->connectionTokens.clear();

	SaveGameRecord(game);

	mutex->Unlock();
}

CServerGame *CServerGamesManagerBase::EnsureServerRunning(const string &gameId)
{
	mutex->Lock();

	auto it = games.find(gameId);
	if (it == games.end())
	{
		LOGError("CServerGamesManagerBase::EnsureServerRunning: gameId=%s not found", gameId.c_str());
		mutex->Unlock();
		return NULL;
	}

	CServerGame *game = it->second;

	if (game->status == CServerGame::FINISHED)
	{
		LOGError("CServerGamesManagerBase::EnsureServerRunning: gameId=%s is FINISHED", gameId.c_str());
		mutex->Unlock();
		return NULL;
	}

	if (game->status == CServerGame::RUNNING && (game->gameServer != NULL || game->isRemoteProcess))
	{
		// Already running — generate fresh tokens
		GenerateConnectionTokens(game);
		mutex->Unlock();

		// Push refreshed tokens via registry
		if (registry)
		{
			time_t rejoinTokenTime = time(NULL);
			registry->PushTokens(game->gameId, game->connectionTokens, game->clientIdByClientName, rejoinTokenTime);
		}
		return game;
	}

	if (game->status == CServerGame::STARTING)
	{
		// Another thread is starting it — wait with safe re-lookup each iteration
		mutex->Unlock();
		for (int i = 0; i < 100; i++)
		{
			SYS_Sleep(50);
			mutex->Lock();
			auto it2 = games.find(gameId);
			if (it2 == games.end())
			{
				// Game was removed while we waited
				LOGError("CServerGamesManagerBase::EnsureServerRunning: gameId=%s removed during startup wait", gameId.c_str());
				mutex->Unlock();
				return NULL;
			}
			game = it2->second;
			if (game->status == CServerGame::RUNNING)
			{
				GenerateConnectionTokens(game);
				mutex->Unlock();
				return game;
			}
			if (game->status != CServerGame::STARTING)
			{
				// Status changed to something unexpected (SUSPENDED, FINISHED)
				LOGError("CServerGamesManagerBase::EnsureServerRunning: gameId=%s status changed to %d during startup", gameId.c_str(), (int)game->status);
				mutex->Unlock();
				return NULL;
			}
			mutex->Unlock();
		}
		LOGError("CServerGamesManagerBase::EnsureServerRunning: timeout waiting for gameId=%s to start", gameId.c_str());
		return NULL;
	}

	// SUSPENDED — mark STARTING to prevent duplicate starts from concurrent calls,
	// then download snapshot and start the server
	game->status = CServerGame::STARTING;
	mutex->Unlock();
	DownloadSnapshot(gameId);
	return StartGameServer(gameId);
}

CServerGame *CServerGamesManagerBase::GetGame(const string &gameId)
{
	mutex->Lock();
	auto it = games.find(gameId);
	CServerGame *game = (it != games.end()) ? it->second : NULL;
	mutex->Unlock();
	return game;
}

vector<CServerGame *> CServerGamesManagerBase::GetAllGames()
{
	vector<CServerGame *> result;
	mutex->Lock();
	for (auto &pair : games)
	{
		result.push_back(pair.second);
	}
	mutex->Unlock();
	return result;
}

vector<CServerGame *> CServerGamesManagerBase::GetGamesForPlayer(int playerProfileId)
{
	vector<CServerGame *> result;
	mutex->Lock();
	for (auto &pair : games)
	{
		CServerGame *game = pair.second;
		for (const auto &slot : game->playerSlots)
		{
			if (slot.playerProfileId == playerProfileId)
			{
				result.push_back(game);
				break;
			}
		}
	}
	mutex->Unlock();
	return result;
}

void CServerGamesManagerBase::UpdateIdleServers()
{
	// Collect idle game IDs under mutex, then stop each outside the mutex
	// to avoid holding the mutex during blocking I/O (file writes, process waits)
	vector<string> idleGameIds;

	mutex->Lock();
	time_t now = time(NULL);

	for (auto &pair : games)
	{
		CServerGame *game = pair.second;
		bool isRunning = (game->status == CServerGame::RUNNING)
			&& (game->gameServer != NULL || game->isRemoteProcess);
		bool isIdle = (game->connectedPlayerCount == 0)
			&& (difftime(now, game->lastActivityTime) > IDLE_GRACE_PERIOD_SECONDS);

		if (isRunning && isIdle)
		{
			idleGameIds.push_back(pair.first);
		}
	}
	mutex->Unlock();

	// Now hibernate each idle game without holding the main mutex
	for (const string &gameId : idleGameIds)
	{
		mutex->Lock();
		auto it = games.find(gameId);
		if (it == games.end())
		{
			mutex->Unlock();
			continue;
		}

		CServerGame *game = it->second;

		// Re-check: game may have been reactivated between collection and now
		bool stillIdle = (game->status == CServerGame::RUNNING)
			&& (game->connectedPlayerCount == 0)
			&& (game->gameServer != NULL || game->isRemoteProcess);
		if (!stillIdle)
		{
			mutex->Unlock();
			continue;
		}

		LOGM("CServerGamesManagerBase::UpdateIdleServers: hibernating idle game %s", game->gameId.c_str());

		if (game->isRemoteProcess && game->processId > 0)
		{
			// Send shutdown via registry
			if (registry)
				registry->RequestShutdown(game->gameId, "idle_hibernate");

			// Give process time to exit, then force kill
			bool exited = false;
			for (int i = 0; i < 30; i++)
			{
				if (CProcessSpawner::TryWait(game->processId, nullptr))
				{
					exited = true;
					break;
				}
				mutex->Unlock();
				SYS_Sleep(100);
				mutex->Lock();
			}

			if (!exited)
			{
				CProcessSpawner::Kill(game->processId);
				CProcessSpawner::TryWait(game->processId, nullptr);
			}

			game->processId = 0;
			game->isRemoteProcess = false;
		}
		else if (game->gameServer)
		{
			// In-process: direct shutdown
			if (game->gameServer->lobbyLink)
			{
				game->gameServer->lobbyLink->Disconnect();
				delete game->gameServer->lobbyLink;
				game->gameServer->lobbyLink = NULL;
			}

			game->gameServer->SaveState();
			game->gameServer->Shutdown();
			delete game->gameServer;
			game->gameServer = NULL;
		}

		// Upload snapshot to shared storage before marking SUSPENDED
		UploadSnapshot(game->gameId);

		ReleasePort(game->serverPort);
		game->serverPort = 0;
		game->status = CServerGame::SUSPENDED;
		game->connectedPlayerCount = 0;
		game->connectionTokens.clear();

		SaveGameRecord(game);

		mutex->Unlock();

		// Notify subclass outside mutex (lobby can update player game lists, etc.)
		OnGamePaused(game);
	}
}

int CServerGamesManagerBase::RemoveSuspendedGames()
{
	namespace fs = std::filesystem;
	mutex->Lock();

	string gamesFolder = string(gPathToResources) + "data/games/";

	vector<string> toRemove;
	for (auto &pair : games)
	{
		CServerGame *game = pair.second;
		if (game->status == CServerGame::SUSPENDED && game->gameServer == NULL)
		{
			toRemove.push_back(pair.first);
		}
	}

	int removed = 0;
	for (const string &gameId : toRemove)
	{
		CServerGame *game = games[gameId];

		// Delete game record file
		string recordPath = gamesFolder + gameId + ".json";
		fs::remove(recordPath);

		// Delete state file
		if (!game->stateFilePath.empty())
			fs::remove(game->stateFilePath);

		// Delete chat history file
		string chatPath = gamesFolder + gameId + "_chat.jsonl";
		fs::remove(chatPath);

		LOGM("CServerGamesManagerBase::RemoveSuspendedGames: removed gameId=%s", gameId.c_str());

		delete game;
		games.erase(gameId);
		removed++;
	}

	mutex->Unlock();
	return removed;
}

void CServerGamesManagerBase::LoadAllGameRecords()
{
	namespace fs = std::filesystem;

	string gamesFolder = string(gPathToResources) + "data/games/";

	if (!fs::exists(gamesFolder))
	{
		LOGD("CServerGamesManagerBase::LoadAllGameRecords: games folder does not exist, creating");
		SYS_CreateFolder(gamesFolder.c_str());
		return;
	}

	for (const auto &entry : fs::directory_iterator(gamesFolder))
	{
		if (!entry.is_regular_file()) continue;

		string filename = entry.path().filename().generic_string();

		// Skip state files and chat history files
		if (filename.find("_state.json") != string::npos) continue;
		if (filename.find("_chat.jsonl") != string::npos) continue;
		if (filename.find(".json") == string::npos) continue;

		try
		{
			ifstream file(entry.path());
			json j;
			file >> j;

			CServerGame *game = new CServerGame();
			if (game->Deserialize(j))
			{
				games[game->gameId] = game;
				LOGM("CServerGamesManagerBase: loaded game record %s (%s)",
					 game->gameId.c_str(), game->mapName.c_str());
			}
			else
			{
				delete game;
			}
		}
		catch (const exception &e)
		{
			LOGError("CServerGamesManagerBase: failed to load game record %s: %s",
					 filename.c_str(), e.what());
		}
	}

	LOGM("CServerGamesManagerBase: loaded %d game records", (int)games.size());
}

void CServerGamesManagerBase::SaveGameRecord(CServerGame *game)
{
	namespace fs = std::filesystem;

	string gamesFolder = string(gPathToResources) + "data/games/";
	SYS_CreateFolder(gamesFolder.c_str());

	string filePath = gamesFolder + game->gameId + ".json";

	ofstream outFile(filePath);
	if (!outFile.is_open())
	{
		LOGError("CServerGamesManagerBase::SaveGameRecord: failed to write %s", filePath.c_str());
		return;
	}

	json j;
	game->Serialize(j);
	outFile << j.dump(4);
}

void CServerGamesManagerBase::GenerateConnectionTokens(CServerGame *game)
{
	game->connectionTokens.clear();
	game->clientIdByClientName.clear();
	for (const auto &slot : game->playerSlots)
	{
		if (slot.isActive)
		{
			game->connectionTokens[slot.playerProfileId] = CServerGame::GenerateConnectionToken();
			game->clientIdByClientName[slot.playerName] = slot.playerProfileId;
		}
	}
}

bool CServerGamesManagerBase::PlayerLeaveGame(const string &gameId, int playerProfileId)
{
	mutex->Lock();

	auto it = games.find(gameId);
	if (it == games.end())
	{
		LOGError("CServerGamesManagerBase::PlayerLeaveGame: gameId=%s not found", gameId.c_str());
		mutex->Unlock();
		return false;
	}

	CServerGame *game = it->second;

	if (game->status == CServerGame::FINISHED)
	{
		LOGError("CServerGamesManagerBase::PlayerLeaveGame: gameId=%s is already FINISHED", gameId.c_str());
		mutex->Unlock();
		return false;
	}

	// Mark player slot as inactive
	bool found = false;
	for (auto &slot : game->playerSlots)
	{
		if (slot.playerProfileId == playerProfileId && slot.isActive)
		{
			slot.isActive = false;
			found = true;
			break;
		}
	}

	if (!found)
	{
		LOGError("CServerGamesManagerBase::PlayerLeaveGame: profileId=%d not found or already inactive in game %s",
				 playerProfileId, gameId.c_str());
		mutex->Unlock();
		return false;
	}

	LOGM("CServerGamesManagerBase::PlayerLeaveGame: profileId=%d left game %s", playerProfileId, gameId.c_str());

	// Notify game server if running
	CNetGameServer *server = game->gameServer;
	if (server)
	{
		OnPlayerLeftGame(game, server, playerProfileId);
	}

	SaveGameRecord(game);

	// Check if game should finish (0 or 1 active player remaining)
	int activeCount = game->GetActivePlayerCount();
	if (activeCount <= 1)
	{
		int winnerProfileId = game->GetLastActivePlayerProfileId();
		string reason = (activeCount == 0) ? "all_players_left" : "last_player_standing";
		mutex->Unlock();
		FinishGame(gameId, winnerProfileId, reason);
		return true;
	}

	mutex->Unlock();
	return true;
}

void CServerGamesManagerBase::FinishGame(const string &gameId, int winnerProfileId, const string &reason)
{
	mutex->Lock();

	auto it = games.find(gameId);
	if (it == games.end())
	{
		mutex->Unlock();
		return;
	}

	CServerGame *game = it->second;
	if (game->status == CServerGame::FINISHED || game->isFinishing)
	{
		mutex->Unlock();
		return;
	}

	// Mark as finishing under mutex to prevent concurrent double-call
	game->isFinishing = true;

	LOGM("CServerGamesManagerBase::FinishGame: gameId=%s winnerProfileId=%d reason=%s",
		 gameId.c_str(), winnerProfileId, reason.c_str());

	OnGameFinished(game, winnerProfileId);

	WriteGameHistoryLog(game, winnerProfileId, reason);

	game->status = CServerGame::FINISHED;
	SaveGameRecord(game);

	// Defer server stop and archive — cannot call StopGameServer synchronously here
	// because FinishGame may be called from within packet processing (e.g., leaveGame handler).
	// Stopping the server synchronously would destroy network state mid-callback.
	// Archive must also be deferred because it erases the game from the map,
	// and StopGameServer needs the game entry to find and shut down the server.
	mutexPendingStops->Lock();
	pendingStops.push_back(gameId);
	mutexPendingStops->Unlock();

	mutex->Unlock();
}

void CServerGamesManagerBase::ProcessPendingStops()
{
	mutexPendingStops->Lock();
	vector<string> toStop = std::move(pendingStops);
	pendingStops.clear();
	mutexPendingStops->Unlock();

	for (const string &gameId : toStop)
	{
		StopGameServer(gameId);

		// Archive finished game after server is stopped
		// ArchiveFinishedGame erases from the games map, so we hold the mutex
		mutex->Lock();
		auto it = games.find(gameId);
		if (it != games.end() && it->second->status == CServerGame::FINISHED)
		{
			ArchiveFinishedGame(it->second);
		}
		mutex->Unlock();
	}
}

int CServerGamesManagerBase::GetActiveGameCountForPlayer(int playerProfileId)
{
	int count = 0;
	mutex->Lock();
	for (auto &pair : games)
	{
		CServerGame *game = pair.second;
		if (game->status == CServerGame::FINISHED)
			continue;
		for (const auto &slot : game->playerSlots)
		{
			if (slot.playerProfileId == playerProfileId && slot.isActive)
			{
				count++;
				break;
			}
		}
	}
	mutex->Unlock();
	return count;
}

bool CServerGamesManagerBase::CanPlayerJoinGame(int playerProfileId)
{
	if (maxGamesPerPlayer <= 0)
		return true;
	return GetActiveGameCountForPlayer(playerProfileId) < maxGamesPerPlayer;
}

void CServerGamesManagerBase::ArchiveFinishedGame(CServerGame *game)
{
	namespace fs = std::filesystem;

	string gamesFolder = string(gPathToResources) + "data/games/";
	string finishedFolder = gamesFolder + "finished/";
	SYS_CreateFolder(finishedFolder.c_str());

	// Move game record
	string srcPath = gamesFolder + game->gameId + ".json";
	string dstPath = finishedFolder + game->gameId + ".json";
	if (fs::exists(srcPath))
	{
		fs::rename(srcPath, dstPath);
	}

	// Move state file
	if (!game->stateFilePath.empty() && fs::exists(game->stateFilePath))
	{
		string dstStatePath = finishedFolder + game->gameId + "_state.json";
		fs::rename(game->stateFilePath, dstStatePath);
	}

	// Move chat history
	string chatSrc = gamesFolder + game->gameId + "_chat.jsonl";
	if (fs::exists(chatSrc))
	{
		string chatDst = finishedFolder + game->gameId + "_chat.jsonl";
		fs::rename(chatSrc, chatDst);
	}

	// Remove from active games map
	games.erase(game->gameId);
	delete game;
}

void CServerGamesManagerBase::WriteGameHistoryLog(CServerGame *game, int winnerProfileId, const string &reason)
{
	namespace fs = std::filesystem;

	string finishedFolder = string(gPathToResources) + "data/games/finished/";
	SYS_CreateFolder(finishedFolder.c_str());

	string historyPath = finishedFolder + game->gameId + "_history.json";

	// Find winner name for the log (display only — authoritative ID is winnerProfileId)
	string winnerName;
	for (const auto &slot : game->playerSlots)
	{
		if (slot.playerProfileId == winnerProfileId)
		{
			winnerName = slot.playerName;
			break;
		}
	}

	json j;
	j["gameId"] = game->gameId;
	j["mapName"] = game->mapName;
	j["mapId"] = game->mapId;
	j["winnerProfileId"] = winnerProfileId;
	j["winnerName"] = winnerName;
	j["reason"] = reason;
	j["finishedAt"] = (long long)time(NULL);

	json jPlayers = json::array();
	for (const auto &slot : game->playerSlots)
	{
		json jp;
		jp["playerName"] = slot.playerName;
		jp["playerProfileId"] = slot.playerProfileId;
		jp["roleId"] = slot.roleId;
		jp["isActive"] = slot.isActive;
		jPlayers.push_back(jp);
	}
	j["players"] = jPlayers;

	ofstream outFile(historyPath);
	if (outFile.is_open())
	{
		outFile << j.dump(4);
	}
}

bool CServerGamesManagerBase::UploadSnapshot(const string &gameId)
{
	if (!snapshotStorage) return false;

	auto it = games.find(gameId);
	if (it == games.end()) return false;

	CServerGame *game = it->second;
	if (game->stateFilePath.empty()) return false;

	if (!std::filesystem::exists(game->stateFilePath))
	{
		LOGError("CServerGamesManagerBase::UploadSnapshot: state file not found for gameId=%s at %s",
				 gameId.c_str(), game->stateFilePath.c_str());
		return false;
	}

	bool ok = snapshotStorage->Upload(gameId, game->stateFilePath);
	if (ok) {
		LOGM("CServerGamesManagerBase::UploadSnapshot: gameId=%s uploaded", gameId.c_str());
	} else {
		LOGError("CServerGamesManagerBase::UploadSnapshot: gameId=%s upload failed", gameId.c_str());
	}
	return ok;
}

bool CServerGamesManagerBase::DownloadSnapshot(const string &gameId)
{
	if (!snapshotStorage) return false;

	auto it = games.find(gameId);
	if (it == games.end()) return false;

	CServerGame *game = it->second;
	if (game->stateFilePath.empty())
	{
		// Generate state file path
		string gamesFolder = string(gPathToResources) + "data/games/";
		game->stateFilePath = gamesFolder + gameId + "_state.json";
	}

	if (!snapshotStorage->Exists(gameId))
	{
		LOGD("CServerGamesManagerBase::DownloadSnapshot: no snapshot in storage for gameId=%s", gameId.c_str());
		return false;
	}

	bool ok = snapshotStorage->Download(gameId, game->stateFilePath);
	if (ok) {
		LOGM("CServerGamesManagerBase::DownloadSnapshot: gameId=%s downloaded to %s", gameId.c_str(), game->stateFilePath.c_str());
	} else {
		LOGError("CServerGamesManagerBase::DownloadSnapshot: gameId=%s download failed", gameId.c_str());
	}
	return ok;
}

void CServerGamesManagerBase::OnRemoteAllocationResult(const string &gameId, bool accepted, const string &nodeId,
													   const string &address, int port, const string &reason)
{
	mutex->Lock();

	auto it = games.find(gameId);
	if (it == games.end())
	{
		LOGWarning("CServerGamesManagerBase::OnRemoteAllocationResult: gameId=%s not found", gameId.c_str());
		mutex->Unlock();
		return;
	}

	CServerGame *game = it->second;

	if (!accepted)
	{
		LOGError("CServerGamesManagerBase::OnRemoteAllocationResult: allocation failed for gameId=%s reason=%s",
				 gameId.c_str(), reason.c_str());
		game->status = CServerGame::SUSPENDED;
		game->isRemoteProcess = false;
		game->serverPort = 0;
		game->connectionTokens.clear();
		SaveGameRecord(game);
		mutex->Unlock();
		return;
	}

	// The CGameServerRegistry::RegistryAllocateGameServerResult already updated
	// game->serverAddress, serverPort, status. Just verify and log.
	LOGM("CServerGamesManagerBase::OnRemoteAllocationResult: gameId=%s allocated on node=%s at %s:%d",
		 gameId.c_str(), nodeId.c_str(), address.c_str(), port);

	mutex->Unlock();
}

int CServerGamesManagerBase::AllocatePort()
{
	for (int port = portRangeStart; port <= portRangeEnd; port++)
	{
		if (usedPorts.find(port) == usedPorts.end())
		{
			usedPorts.insert(port);
			return port;
		}
	}
	return 0; // no ports available
}

void CServerGamesManagerBase::ReleasePort(int port)
{
	usedPorts.erase(port);
}
