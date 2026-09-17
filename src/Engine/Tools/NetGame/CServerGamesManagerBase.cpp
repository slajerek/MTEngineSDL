// CAPABILITY GUARD (MT_ENABLE_NET_GAME). The file stays in all three build lists and
// compiles to nothing when the capability is off -- an Xcode PBXBuildFile
// cannot be conditioned on a build setting, so the condition lives here.
// MT_CAP_NET_GAME implies NET_TRANSPORT and HTTP, so this whole stack rides on one flag -- which did not exist until 2026-09-06 (enables was an empty list).
#if MT_ENABLE_NET_GAME

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

	if (game->status == CServerGame::STOPPING)
	{
		// A stop is in flight (gameServer==NULL but the record isn't free
		// yet -- see the STOPPING comment on CServerGame::Status). Refuse
		// rather than start a second server that the stopper's re-validate
		// block wouldn't expect. Normal callers go through
		// EnsureServerRunning(), which already waits this out; this guard
		// covers direct callers (e.g. a freshly created game never hits
		// this, but the check is here for defense in depth).
		LOGWarning("CServerGamesManagerBase::StartGameServer: gameId=%s is STOPPING; refusing to start a second server", gameId.c_str());
		mutex->Unlock();
		return NULL;
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

	if (game->status == CServerGame::STOPPING)
	{
		// Another StopGameServer()/UpdateIdleServers() call already owns
		// this record's stop and released the mutex for its blocking
		// network stop (gameServer==NULL here, but the record is not free).
		// Falling through to the tail below would clear a STOPPING marker
		// this call does not own -- overwriting status and ReleasePort()ing
		// a port that owning call may be about to RetainZombiePort(). Log
		// and back off; the owning call's own re-validate block finishes
		// this record.
		LOGWarning("CServerGamesManagerBase::StopGameServer: gameId=%s is already STOPPING (another call owns it); ignoring this call", gameId.c_str());
		mutex->Unlock();
		return;
	}

	// Captured under the same hold that found the record, before any branch
	// below overwrites status with STOPPING: the tail restores FINISHED rather
	// than SUSPENDED when this record had already finished (see the in-process
	// branch's own prevStatus comment). For the plain path (neither branch
	// taken) this is just today's `status != FINISHED` rule spelled out.
	CServerGame::Status prevStatus = game->status;

	if (game->isRemoteProcess && game->processId > 0)
	{
		// Separate process mode: send shutdown via registry, then kill if needed.
		//
		// This branch unlocks and relocks the manager mutex up to 50 times in
		// the wait loop below, so it gets the SAME discipline as the in-process
		// branch (final-review I-4): STOPPING marked under the hold that checked
		// the record (EnsureServerRunning()/StartGameServer() treat STOPPING as
		// busy, so nothing restarts this record while we are unlocked), and the
		// record re-looked-up BY ID after every relock -- `game` is not a stable
		// pointer across an unlock, and a record erased and deleted during those
		// up-to-5 seconds would leave it dangling for `game->processId = 0`.
		LOGM("CServerGamesManagerBase::StopGameServer: gameId=%s shutting down remote process pid=%d", gameId.c_str(), game->processId);

		int processId = game->processId;
		game->status = CServerGame::STOPPING;

		if (registry)
			registry->RequestShutdown(gameId, "stop_requested");

		// Wait for process to exit (up to 5 seconds)
		bool exited = false;
		bool recordGone = false;
		bool lostRace = false;
		for (int i = 0; i < 50; i++)
		{
			if (CProcessSpawner::TryWait(processId, nullptr))
			{
				exited = true;
				break;
			}
			mutex->Unlock();
			SYS_Sleep(100);
			mutex->Lock();

			it = games.find(gameId);
			if (it == games.end())
			{
				recordGone = true;
				break;
			}
			game = it->second;
			if (game->status != CServerGame::STOPPING)
			{
				// Only the call that set the marker may act on the record.
				lostRace = true;
				break;
			}
		}

		if (recordGone)
		{
			LOGWarning("CServerGamesManagerBase::StopGameServer: gameId=%s record disappeared while stopping the remote process; nothing left to update", gameId.c_str());
			mutex->Unlock();
			return;
		}

		if (lostRace)
		{
			LOGWarning("CServerGamesManagerBase::StopGameServer: gameId=%s status changed to %s while stopping outside the lock; leaving the record alone (a restart or FinishGame() likely won the race)", gameId.c_str(), game->GetStatusName());
			mutex->Unlock();
			return;
		}

		if (!exited)
		{
			LOGM("CServerGamesManagerBase::StopGameServer: gameId=%s process did not exit, killing", gameId.c_str());
			CProcessSpawner::Kill(processId);
			CProcessSpawner::TryWait(processId, nullptr);
		}

		game->processId = 0;
		game->isRemoteProcess = false;
	}
	else if (game->gameServer)
	{
		// In-process mode: snapshot and detach the server from `game` while
		// still holding the mutex, then release the mutex before the
		// blocking stop below. The game server's own network thread calls
		// back into GetGame()/FinishGame() (the app-side game server
		// implementation), which take this same `mutex` -- holding it here across
		// Shutdown()/StopServer() would be a lock inversion that guarantees
		// the 5s cap expires (that is what run A's single "leaking game
		// server" firing actually was, not scheduling luck).
		CNetGameServer *server = game->gameServer;
		int port = game->serverPort;
		game->gameServer = NULL;

		// Capture the pre-stop status under this same lock, before
		// overwriting it with STOPPING, so the re-validate block below can
		// restore FINISHED rather than SUSPENDED when this record had
		// already finished (FinishGame() sets FINISHED then defers the stop
		// via pendingStops -- ProcessPendingStops() calls us next). Without
		// this, ArchiveFinishedGame()'s "if (status == FINISHED)" gate
		// (ProcessPendingStops()) never passes for an in-process game, the
		// record stays visible as SUSPENDED, and a finished game would stay
		// rejoinable. (Captured at the top of this function now that the
		// remote branch above needs it too -- same value: nothing between
		// there and here touches game->status on this path.)

		// Mark the record STOPPING before unlocking: EnsureServerRunning()/
		// StartGameServer()/the remote-allocation path all treat STOPPING as
		// busy and refuse to (re)start a second server for this record while
		// gameServer==NULL but the record isn't actually free -- without
		// this a rejoin landing in the unlocked window below could start a
		// second live server that the re-validate block then clobbered.
		game->status = CServerGame::STOPPING;

		mutex->Unlock();

		LOGM("CServerGamesManagerBase::StopGameServer: gameId=%s saving state and stopping", gameId.c_str());

		// Shutdown lobbyLink first (stops network thread). CLobbyLink holds a
		// raw gameServer pointer its callbacks dereference (HandleUpdateTokens,
		// HandleSaveState, HandleKickPlayer) -- if its network thread refuses
		// to stop in time, it may still be executing one of those, so neither
		// lobbyLink nor gameServer may be deleted: the whole game server is
		// leaked instead of risking a use-after-free.
		bool lobbyLinkStopped = true;
		if (server->lobbyLink)
		{
			lobbyLinkStopped = server->lobbyLink->Shutdown();
			if (lobbyLinkStopped)
			{
				delete server->lobbyLink;
				server->lobbyLink = NULL;
			}
			else
			{
				LOGError("CServerGamesManagerBase::StopGameServer: gameId=%s lobbyLink network thread did not stop in time; leaking game server to avoid a use-after-free", gameId.c_str());
			}
		}

		// Always save and always ask the game server's own network thread to
		// stop, regardless of the lobbyLink outcome -- only the delete below
		// is withheld on failure. Gating these on lobbyLinkStopped would lose
		// the save and leave this thread running forever whenever only the
		// lobbyLink failed to stop.
		server->SaveState();
		bool serverStopped = server->Shutdown();
		if (!serverStopped)
		{
			LOGError("CServerGamesManagerBase::StopGameServer: gameId=%s network thread did not stop in time; leaking game server to avoid a use-after-free", gameId.c_str());
		}

		bool leaked = !lobbyLinkStopped || !serverStopped;
		if (!leaked)
		{
			delete server;
		}

		mutex->Lock();

		// Re-validate: the game may have been removed from `games` while we
		// were stopping its server outside the lock, or -- since gameServer
		// was NULL and status was STOPPING for the whole window -- a rejoin
		// could have raced EnsureServerRunning()/StartGameServer() into
		// starting a brand-new server for this record (they now refuse to
		// while STOPPING, but only mutate the record here if it is still
		// marked STOPPING by this very call: if something else already
		// claimed it, leave it alone and only do our own port bookkeeping
		// below, or we would clobber a live restart).
		it = games.find(gameId);
		if (it != games.end())
		{
			game = it->second;
			if (game->status == CServerGame::STOPPING)
			{
				game->serverPort = 0;
				// Restore FINISHED rather than SUSPENDED if that is what
				// this record was before we marked it STOPPING (see the
				// prevStatus comment above) -- never RUNNING, which
				// prevStatus can also be and which would make this record
				// look live with no server. ArchiveFinishedGame() needs to
				// see FINISHED; a finished game must not be resumable.
				game->status = (prevStatus == CServerGame::FINISHED) ? CServerGame::FINISHED : CServerGame::SUSPENDED;
				game->connectedPlayerCount = 0;
				game->connectionTokens.clear();
				SaveGameRecord(game);
			}
			else
			{
				// Lost the race (e.g. FinishGame() ran on the game server's
				// own thread during the unlocked window and overwrote
				// STOPPING with FINISHED, or -- in tests -- a second
				// concurrent stopper claimed it first). Do not touch status
				// or the other fields; whoever holds the record now owns
				// it. Still zero serverPort if it still names the port we
				// just detached, so a later fresh StopGameServer() call
				// falling through to its plain tail can't ReleasePort() a
				// port we may be about to RetainZombiePort() below --
				// erasing it from usedPorts while a leaked thread might
				// still hold its socket.
				if (game->serverPort == port)
				{
					game->serverPort = 0;
				}
				LOGWarning("CServerGamesManagerBase::StopGameServer: gameId=%s status changed to %s while stopping outside the lock; leaving the record alone (a restart or FinishGame() likely won the race)", gameId.c_str(), game->GetStatusName());
			}
		}

		if (leaked)
		{
			// A still-running thread may still hold this port's ENet socket
			// (CNetServer only calls enet_host_destroy() from inside its own
			// ThreadRun(), CNetServer.cpp:322) -- retain it instead of
			// handing it to the next AllocatePort() call.
			RetainZombiePort(port);
			LOGError("CServerGamesManagerBase::StopGameServer: gameId=%s port %d retained (zombie) until the leaked server's thread exits", gameId.c_str(), port);
		}
		else
		{
			ReleasePort(port);
		}

		mutex->Unlock();
		return;
	}

	ReleasePort(game->serverPort);
	game->serverPort = 0;
	// A game that FINISHED while (or before) it was being stopped stays
	// FINISHED so ArchiveFinishedGame() still picks it up and nobody can
	// rejoin it; everything else parks as SUSPENDED (task-3b re-review 3).
	// Read prevStatus, not the live field: the remote branch above legitimately
	// leaves STOPPING here (its own marker, still owned by this call), and a
	// bare `status != FINISHED` test would then downgrade a finished remote game
	// to SUSPENDED and leave it rejoinable (final-review I-4).
	game->status = (prevStatus == CServerGame::FINISHED) ? CServerGame::FINISHED : CServerGame::SUSPENDED;
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

	if (game->status == CServerGame::STOPPING)
	{
		// A stop is in flight for this record (StopGameServer()/
		// UpdateIdleServers() detached the server and released this mutex
		// for their blocking network stop, up to ~7s in the failure case
		// this task exists for). Starting a second server here would race
		// the stopper's re-validate block; wait for it to clear instead,
		// same shape as the STARTING wait above. Once it clears (normally
		// back to SUSPENDED), retry from the top -- a single recursive call
		// picks up whatever state the stop left behind.
		mutex->Unlock();
		for (int i = 0; i < 150; i++) // 150 * 50ms = 7.5s, covers StopServer()'s 5s cap plus lobbyLink's 2s plus margin
		{
			SYS_Sleep(50);
			mutex->Lock();
			auto it2 = games.find(gameId);
			if (it2 == games.end())
			{
				LOGWarning("CServerGamesManagerBase::EnsureServerRunning: gameId=%s removed while a stop was in flight", gameId.c_str());
				mutex->Unlock();
				return NULL;
			}
			game = it2->second;
			if (game->status != CServerGame::STOPPING)
			{
				mutex->Unlock();
				return EnsureServerRunning(gameId);
			}
			mutex->Unlock();
		}
		LOGWarning("CServerGamesManagerBase::EnsureServerRunning: gameId=%s stop still in flight after timeout; refusing to start (client can retry)", gameId.c_str());
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
			bool wasStopping = (game->status == CServerGame::STOPPING);
			mutex->Unlock();

			if (wasStopping)
			{
				// A concurrent caller (another thread's UpdateIdleServers(),
				// or ProcessPendingStops()/StopGameServer()) is already
				// hibernating this exact game -- gameServer==NULL and
				// status==STOPPING is not "reactivated", it is "already
				// being handled". Wait for that stop to clear instead of
				// silently racing ahead of it: a caller that identified
				// this game as idle should be able to rely on it actually
				// being hibernated by the time this function returns.
				for (int i = 0; i < 150; i++) // 150 * 50ms = 7.5s, matches EnsureServerRunning's STOPPING wait
				{
					SYS_Sleep(50);
					mutex->Lock();
					auto it2 = games.find(gameId);
					bool stillStopping = (it2 != games.end() && it2->second->status == CServerGame::STOPPING);
					mutex->Unlock();
					if (!stillStopping)
						break;
				}
			}

			continue;
		}

		LOGM("CServerGamesManagerBase::UpdateIdleServers: hibernating idle game %s", game->gameId.c_str());

		// Captured under the same hold that re-checked the record, before any
		// branch below overwrites status with STOPPING -- see the identical
		// comment in StopGameServer().
		CServerGame::Status prevStatus = game->status;

		if (game->isRemoteProcess && game->processId > 0)
		{
			// Same discipline as StopGameServer()'s remote branch
			// (final-review I-4): STOPPING marked before the first unlock so
			// nothing restarts this record while we wait, and the record
			// re-looked-up by id after every relock -- `game` is not a stable
			// pointer across an unlock.
			int processId = game->processId;
			game->status = CServerGame::STOPPING;

			// Send shutdown via registry
			if (registry)
				registry->RequestShutdown(gameId, "idle_hibernate");

			// Give process time to exit, then force kill
			bool exited = false;
			bool recordGone = false;
			bool lostRace = false;
			for (int i = 0; i < 30; i++)
			{
				if (CProcessSpawner::TryWait(processId, nullptr))
				{
					exited = true;
					break;
				}
				mutex->Unlock();
				SYS_Sleep(100);
				mutex->Lock();

				it = games.find(gameId);
				if (it == games.end())
				{
					recordGone = true;
					break;
				}
				game = it->second;
				if (game->status != CServerGame::STOPPING)
				{
					lostRace = true;
					break;
				}
			}

			if (recordGone)
			{
				LOGWarning("CServerGamesManagerBase::UpdateIdleServers: gameId=%s record disappeared while stopping the remote process; nothing left to update", gameId.c_str());
				mutex->Unlock();
				continue;
			}

			if (lostRace)
			{
				LOGWarning("CServerGamesManagerBase::UpdateIdleServers: gameId=%s status changed to %s while stopping outside the lock; leaving the record alone (a restart or FinishGame() likely won the race)", gameId.c_str(), game->GetStatusName());
				mutex->Unlock();
				continue;
			}

			if (!exited)
			{
				CProcessSpawner::Kill(processId);
				CProcessSpawner::TryWait(processId, nullptr);
			}

			game->processId = 0;
			game->isRemoteProcess = false;
		}
		else if (game->gameServer)
		{
			// In-process: snapshot and detach the server from `game` while
			// still holding the mutex, then release the mutex before the
			// blocking stop below -- same lock-inversion hazard as
			// StopGameServer(): the game server's own network thread calls
			// back into GetGame()/FinishGame(), which take this same
			// `mutex`, so holding it across Shutdown()/StopServer() would
			// guarantee the 5s cap expires.
			CNetGameServer *server = game->gameServer;
			int port = game->serverPort;
			game->gameServer = NULL;

			// Capture the pre-stop status under this same lock -- see the
			// identical comment in StopGameServer() -- so the re-validate
			// block below can restore FINISHED rather than SUSPENDED if
			// this record had already finished by the time we re-lock
			// (FinishGame() can race in on the game server's own thread
			// during the unlocked window below, same as it can for
			// StopGameServer()). Captured at the top of this iteration now
			// that the remote branch above needs it too -- same value:
			// nothing between there and here touches game->status on this
			// path.

			// Mark STOPPING before unlocking -- see the identical comment in
			// StopGameServer(): without this, a rejoin landing in the
			// unlocked window below could start a second live server for
			// this record that the re-validate block would then clobber.
			game->status = CServerGame::STOPPING;

			mutex->Unlock();

			// Shutdown lobbyLink first (stops network thread). CLobbyLink
			// holds a raw gameServer pointer its callbacks dereference, so
			// if its network thread refuses to stop in time, neither
			// lobbyLink nor gameServer may be deleted -- the whole game
			// server is leaked instead of risking a use-after-free.
			bool lobbyLinkStopped = true;
			if (server->lobbyLink)
			{
				lobbyLinkStopped = server->lobbyLink->Shutdown();
				if (lobbyLinkStopped)
				{
					delete server->lobbyLink;
					server->lobbyLink = NULL;
				}
				else
				{
					LOGError("CServerGamesManagerBase::UpdateIdleServers: gameId=%s lobbyLink network thread did not stop in time; leaking game server to avoid a use-after-free", gameId.c_str());
				}
			}

			// Always save and always ask the game server's own network
			// thread to stop, regardless of the lobbyLink outcome -- only
			// the delete below is withheld on failure.
			server->SaveState();
			bool serverStopped = server->Shutdown();
			if (!serverStopped)
			{
				LOGError("CServerGamesManagerBase::UpdateIdleServers: gameId=%s network thread did not stop in time; leaking game server to avoid a use-after-free", gameId.c_str());
			}

			bool leaked = !lobbyLinkStopped || !serverStopped;
			if (!leaked)
			{
				delete server;
			}

			mutex->Lock();

			// Re-validate: the game may have been removed from `games`
			// while we were stopping its server outside the lock, or --
			// since gameServer was NULL and status was STOPPING for the
			// whole window -- a rejoin could have raced a restart in. Only
			// mutate the record if it is still marked STOPPING by this very
			// call; otherwise leave it alone (a restart won) and only do
			// our own port bookkeeping. Capture locals -- `it`/`games` must
			// not be touched again once we unlock below.
			it = games.find(gameId);
			bool stillPresent = (it != games.end());
			bool wonRace = false;
			if (stillPresent)
			{
				game = it->second;
				if (game->status == CServerGame::STOPPING)
				{
					wonRace = true;

					// Upload snapshot to shared storage before marking SUSPENDED
					UploadSnapshot(game->gameId);

					game->serverPort = 0;
					// Restore FINISHED rather than SUSPENDED if that is what
					// this record was before we marked it STOPPING (see the
					// prevStatus comment above) -- never RUNNING.
					// ArchiveFinishedGame() needs to see FINISHED; a
					// finished game must not be resumable.
					game->status = (prevStatus == CServerGame::FINISHED) ? CServerGame::FINISHED : CServerGame::SUSPENDED;
					game->connectedPlayerCount = 0;
					game->connectionTokens.clear();
					SaveGameRecord(game);
				}
				else
				{
					// Lost the race -- see the identical comment in
					// StopGameServer(). Zero serverPort if it still names
					// the port we just detached, so a later fresh
					// StopGameServer() call's plain tail can't
					// ReleasePort() a port we may be about to
					// RetainZombiePort() below.
					if (game->serverPort == port)
					{
						game->serverPort = 0;
					}
					LOGWarning("CServerGamesManagerBase::UpdateIdleServers: gameId=%s status changed to %s while stopping outside the lock; leaving the record alone (a restart or FinishGame() likely won the race)", gameId.c_str(), game->GetStatusName());
				}
			}

			if (leaked)
			{
				// A still-running thread may still hold this port's ENet
				// socket -- retain it instead of handing it to the next
				// AllocatePort() call.
				RetainZombiePort(port);
				LOGError("CServerGamesManagerBase::UpdateIdleServers: gameId=%s port %d retained (zombie) until the leaked server's thread exits", gameId.c_str(), port);
			}
			else
			{
				ReleasePort(port);
			}

			mutex->Unlock();

			if (wonRace)
			{
				// Notify subclass outside mutex (lobby can update player game lists, etc.)
				OnGamePaused(game);
			}

			continue;
		}

		// Upload snapshot to shared storage before marking SUSPENDED
		UploadSnapshot(game->gameId);

		ReleasePort(game->serverPort);
		game->serverPort = 0;
		// Keep FINISHED, exactly as StopGameServer()'s plain tail does: an
		// unconditional SUSPENDED here left a finished remote game rejoinable
		// and out of ArchiveFinishedGame()'s reach (final-review I-4). Read
		// prevStatus, not the live field -- the remote branch above legitimately
		// leaves its own STOPPING marker on the record.
		game->status = (prevStatus == CServerGame::FINISHED) ? CServerGame::FINISHED : CServerGame::SUSPENDED;
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
	// Defense in depth: a record mid-stop (see CServerGame::Status) must
	// never be archived out from under the stop that is about to
	// re-validate and mutate it. The current caller (ProcessPendingStops())
	// already only calls this when status==FINISHED, which STOPPING can
	// never be at the same time, but this guard makes the function safe to
	// call from anywhere in the future.
	if (game->status == CServerGame::STOPPING)
	{
		LOGWarning("CServerGamesManagerBase::ArchiveFinishedGame: gameId=%s is STOPPING; refusing to archive", game->gameId.c_str());
		return;
	}

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

void CServerGamesManagerBase::RetainZombiePort(int port)
{
	// A leaked game server's network thread may still hold the ENet socket
	// bound to this port (CNetServer only calls enet_host_destroy() from
	// inside its own ThreadRun(), CNetServer.cpp:322-330-ish), so
	// AllocatePort() must keep treating it as in use. Re-insert into
	// usedPorts (not just assert it's already there) so this function is
	// correct standing alone, regardless of what any future caller did to
	// usedPorts before reaching here. zombiePorts is separate bookkeeping.
	if (port > 0)
	{
		usedPorts.insert(port);
		zombiePorts.insert(port);
	}
}

#endif // MT_ENABLE_NET_GAME
