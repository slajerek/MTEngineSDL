# NetGame — MTEngineSDL Multiplayer Game System

Developer guide for integrating multiplayer lobby, game servers, and turn-based rounds into an MTEngineSDL project.

## Overview

The NetGame system provides a complete multiplayer stack:

1. **Lobby Server** — matchmaking, player auth, game creation
2. **Game Server** — per-game instance with client connections and token auth
3. **Games Manager** — game lifecycle orchestration, port allocation, server spawning
4. **Turn System** (optional) — round-based turns with player actions
5. **Registry** — routes messages between lobby and game servers (supports multi-VPS)

All classes live in `MTEngineSDL/src/Engine/Tools/NetGame/`. Your game subclasses the engine base classes and implements virtual hooks for game-specific logic.

```
Engine (MTEngineSDL)                    Your Game
─────────────────                       ─────────
CNetLobbyServer (abstract)      ←──    MyLobbyServer
CNetGameServer                  ←──    MyGameServer
  └── CTurnBasedGameServer      ←──    MyTurnBasedGameServer
CServerGamesManagerBase         ←──    MyGamesManager
CNetGameDataProvider            ←──    MyDataProvider
CNetGameUserProfile             ←──    MyPlayerProfile (optional)
CViewServerAdmin                ←──    MyViewServerAdmin (optional)
CViewGameServer                 ←──    MyViewGameServer (optional)
CViewGameClient                 ←──    MyViewGameClient (optional)
```

## Quick Start

Minimal multiplayer game setup:

```cpp
// 1. Data provider — player auth and persistence
class MyDataProvider : public CNetGameDataProviderLocalFiles
{
public:
    MyDataProvider() { Init("data/config.hjson"); }
};

// 2. Games manager — creates game server instances
class MyGamesManager : public CServerGamesManagerBase
{
public:
    MyGamesManager(MyDataProvider *dp) : CServerGamesManagerBase(dp) {}

    virtual CNetGameServer *CreateInProcessGameServer(
        CServerGame *game, json &serverConfig) override
    {
        // Create your game server here
        return new MyGameServer(serverConfig);
    }
};

// 3. Lobby server — handles matchmaking packets
class MyLobbyServer : public CNetLobbyServer
{
public:
    MyGamesManager *gamesManager;

    MyLobbyServer(int port, MyGamesManager *gm)
        : CNetLobbyServer(port), gamesManager(gm)
    {
        this->gamesManager = gm;
        CNetLobbyServer::gamesManager = gm;
    }

    // Required overrides — see "Lobby Server" section
    virtual void NetServerCallbackClientConnected(CNetClientData *cd) override;
    virtual void NetServerCallbackClientDisconnected(CNetClientData *cd) override;
    virtual void NetServerProcessPacket(CNetPacket *packet) override;
    virtual u8 NetServerAuthorize(CNetClientData *cd, string user,
                                  vector<u8> hash) override;
};

// 4. Game server — per-game instance
class MyGameServer : public CNetGameServer
{
public:
    MyGameServer(json config)
        : CNetGameServer(config, config.value("serverPort", 0)) {}

    virtual void OnPlayerAuthenticated(CNetClientData *cd) override
    {
        AddLog("Player %s joined", cd->clientName.c_str());
    }
};
```

## Architecture

### Network Topology

```
┌─────────────┐       ┌──────────────────┐       ┌──────────────┐
│ Game Client  │◄─────►│  Lobby Server    │◄─────►│ Game Client  │
│ (CNetLobby   │  UDP  │  :14500          │  UDP  │ (CNetLobby   │
│  Client)     │       │                  │       │  Client)     │
└──────┬───────┘       │  ┌────────────┐  │       └──────┬───────┘
       │               │  │GamesManager│  │              │
       │               │  └─────┬──────┘  │              │
       │               │        │         │              │
       │               │  ┌─────▼──────┐  │              │
       │               │  │ServiceSrv  │  │              │
       │               │  │  :14600    │  │              │
       │               │  └─────┬──────┘  │              │
       │               └────────┼─────────┘              │
       │                        │ heartbeat              │
       │               ┌───────▼──────────┐              │
       └──────────────►│  Game Server     │◄─────────────┘
          token auth   │  :14701-14799    │   token auth
                       │  (CLobbyLink)    │
                       └──────────────────┘
```

### Port Allocation

| Port | Service |
|------|---------|
| 14500 | Lobby server (configurable) |
| 14600 | Service server — internal game server ↔ lobby link |
| 14650 | Registry server (standalone multi-VPS mode) |
| 14701–14799 | Game server instances (auto-allocated) |

### Packet Format

All messages are JSON over ENet (UDP, reliable). The `CNetGamePackets` codec handles serialization. Every JSON message has an `"action"` field:

```json
{"action": "login", "username": "alice", "passwordHash": "..."}
{"action": "createGame", "scenarioId": 1}
{"action": "roundState", "roundNumber": 3, "currentPlayer": "alice"}
```

---

## Lobby Server

Subclass `CNetLobbyServer` and implement 4 pure virtual methods.

### Pure Virtuals (must implement)

```cpp
// Called when a client connects (after ENet handshake + authorization)
virtual void NetServerCallbackClientConnected(CNetClientData *clientData) override;

// Called when a client disconnects
virtual void NetServerCallbackClientDisconnected(CNetClientData *clientData) override;

// Called for every received JSON packet — dispatch by action
virtual void NetServerProcessPacket(CNetPacket *packet) override;

// Return 1 to authorize, 0 to reject (check credentials here)
virtual u8 NetServerAuthorize(CNetClientData *clientData,
                              string userName, vector<u8> passwordHash) override;
```

### Packet Handling Pattern

```cpp
void MyLobbyServer::NetServerProcessPacket(CNetPacket *packet)
{
    CNetClientData *cd = packet->clientData;
    json j = netPackets->DeserializeJson(packet);
    string action = j.value("action", "");

    if (action == "createGame")
    {
        int mapId = j.value("mapId", 1);
        // ... create game, notify players
    }
    else if (action == "joinGame")
    {
        string gameId = j.value("gameId", "");
        // ... add player to game
    }
    else if (action == "startGame")
    {
        // Create server via gamesManager
        string gameId = ...;
        CServerGame *game = gamesManager->StartGameServer(gameId);

        // Send connection info to players
        json ready;
        ready["action"] = "gameReady";
        ready["address"] = game->serverAddress;
        ready["port"] = game->serverPort;
        // Send token per player:
        ready["connectionToken"] = game->connectionTokens[playerName];
        SendJson(playerClientData, ready);
    }
    else if (action == "leaveGame")
    {
        string gameId = j.value("gameId", "");
        gamesManager->PlayerLeaveGame(gameId, cd->clientName);
    }
}
```

### Available Methods

```cpp
// Send JSON to one client
void SendJson(CNetClientData *clientData, json sendJson);

// Broadcast to all connected clients
void BroadcastJson(json sendJson);

// Disconnect with error message
void DisconnectWithError(CNetClientData *clientData, const char *fmt, ...);

// Send error without disconnecting
void SendError(CNetClientData *clientData, const char *fmt, ...);

// Logging
void AddLog(const char *fmt, ...);
```

### Optional Overrides

```cpp
// Load game-specific config from Hjson
virtual void InitFromHjson(Hjson::Value hjsonRoot) override;

// Wire gamesManager ↔ registry when registry is created
virtual void OnRegistryCreated(CGameServerRegistry *registry) override
{
    registry->gamesManager = myGamesManager;
}
```

### Key Members

```cpp
CNetServer *netServer;                  // ENet server
CNetGamePackets *netPackets;            // JSON codec
ILogSink *logSink;                      // Optional UI log
CServerGamesManagerBase *gamesManager;  // Set by your subclass
CLobbyServiceServer *serviceServer;     // Internal service (port 14600)
CGameServerRegistry *registry;          // EMBEDDED or STANDALONE
int serverPort;
```

---

## Games Manager

Subclass `CServerGamesManagerBase` and implement the factory method. The base class handles port allocation, server lifecycle, state persistence, idle shutdown, and pending stops.

### Factory (must implement)

```cpp
virtual CNetGameServer *CreateInProcessGameServer(
    CServerGame *game, json &serverConfig) override
{
    // Create your game state
    MyGameState *state = new MyGameState(game->mapId);

    // Create players from slots
    for (const auto &slot : game->playerSlots)
    {
        state->AddPlayer(slot.playerName, slot.roleId);
    }

    // Create and return game server
    return new MyGameServer(serverConfig, state);
}
```

### Optional Hooks

```cpp
// Called after server is created and started
virtual void OnGameServerCreated(CServerGame *game, CNetGameServer *server) override
{
    // Set up lobby link for heartbeat
    CLobbyLink *link = new CLobbyLink("localhost",
        CLobbyServiceServer::SERVICE_PORT,
        "gs-" + game->gameId.substr(0, 8),
        game->gameId, game->serverPort, server);
    server->lobbyLink = link;
    link->Connect();

    // Set up chat history
    string chatPath = "data/games/" + game->gameId + "_chat.jsonl";
    server->chatHistory = new CChatHistory(chatPath);
    server->chatHistory->LoadFromFile();
}

// Called when a player leaves mid-game
virtual void OnPlayerLeftGame(CServerGame *game, CNetGameServer *server,
                              const string &playerName) override
{
    // Notify turn system
    if (server) server->OnPlayerDeactivated(playerName);
}

// Called when game finishes (last player standing, etc.)
virtual void OnGameFinished(CServerGame *game, const string &winnerName) override
{
    // Update player stats, notify lobby clients
}
```

### Game Lifecycle Methods

```cpp
// Start a game server (allocates port, creates server, runs it)
CServerGame *StartGameServer(const string &gameId);

// Stop and persist state
void StopGameServer(const string &gameId);

// Start server if suspended (for player rejoin)
CServerGame *EnsureServerRunning(const string &gameId);

// Player leaves game — marks slot inactive, checks finish condition
bool PlayerLeaveGame(const string &gameId, const string &playerName);

// End the game
void FinishGame(const string &gameId, const string &winnerName,
                const string &reason);

// Execute deferred server shutdowns (call from main thread!)
void ProcessPendingStops();

// Check for idle servers (call periodically)
void UpdateIdleServers();

// Max games per player check
bool CanPlayerJoinGame(int playerProfileId);
int GetActiveGameCountForPlayer(int playerProfileId);
```

### Important: ProcessPendingStops

`ProcessPendingStops()` must be called from the **main thread** (e.g., in your lobby view's `RenderImGui()`), NOT from network callbacks. Calling it from a network callback would destroy ENet hosts mid-callback, causing crashes.

```cpp
void MyViewLobbyServer::RenderImGui()
{
    // Process deferred game server stops on the main thread
    server->GetGamesManager()->ProcessPendingStops();

    // ... render UI ...
}
```

---

## Game Server

Subclass `CNetGameServer` for a basic game server, or `CTurnBasedGameServer` for turn-based games.

### Basic Game Server

```cpp
class MyGameServer : public CNetGameServer
{
public:
    MyGameState *state;

    MyGameServer(json config, MyGameState *state)
        : CNetGameServer(config, config.value("serverPort", 0))
    {
        this->state = state;
    }

    // Called when a player passes token authentication
    virtual void OnPlayerAuthenticated(CNetClientData *cd) override
    {
        AddLog("Player %s authenticated", cd->clientName.c_str());
        // Send initial game state to this player
        json j;
        j["action"] = "initialState";
        state->Serialize(j);
        SendJson(cd, j);
    }

    // Called for game-specific packets (action != standard ones)
    virtual bool OnCustomPacket(CNetClientData *cd,
        const string &action, json &j) override
    {
        if (action == "moveUnit")
        {
            // Handle game logic
            return true;  // handled
        }
        return false;  // not handled
    }

    // Called when game state should be persisted
    virtual void OnSaveState() override
    {
        CServerGame *game = manager->GetGame(gameId);
        if (game && !game->stateFilePath.empty())
        {
            json j;
            state->Serialize(j);
            ofstream f(game->stateFilePath);
            f << j.dump(4);
        }
    }
};
```

### Available Methods

```cpp
void SendJson(CNetClientData *clientData, json sendJson);  // Send to one
void BroadcastJson(json sendJson);                          // Send to all
void AddLog(const char *fmt, ...);                          // Logging
void SaveState();                                           // Triggers OnSaveState
void Shutdown();                                            // Graceful shutdown
```

### Authentication Flow

```
Client                    Game Server                  Lobby
  │                           │                          │
  │◄──── gameReady ───────────┤◄── tokens ──────────────│
  │  (address, port, token)   │  (playerName→token map)  │
  │                           │                          │
  │──── connect ─────────────►│                          │
  │──── login(name, token) ──►│                          │
  │                           │ verify token             │
  │◄─── authenticated ───────│                          │
  │                           │ OnPlayerAuthenticated()  │
```

---

## Turn-Based Game Server

`CTurnBasedGameServer` extends `CNetGameServer` with round management. Inherit from this instead of `CNetGameServer` for turn-based games.

### Setup

```cpp
class MyTurnGameServer : public CTurnBasedGameServer
{
public:
    MyGameState *state;

    MyTurnGameServer(json config, MyGameState *state)
        : CTurnBasedGameServer(config, config.value("serverPort", 0))
    {
        this->state = state;
    }

    // Called when a new round begins
    virtual void OnRoundStarted(int roundNumber) override
    {
        AddLog("Round %d started", roundNumber);

        // Broadcast game state to all clients
        json j;
        j["action"] = "gameState";
        state->Serialize(j);
        BroadcastJson(j);
    }

    // Called when a player sends an action (not endTurn)
    virtual void OnPlayerAction(CNetClientData *cd,
        const string &actionType, json &actionData) override
    {
        AddLog("Player %s: %s", cd->clientName.c_str(), actionType.c_str());

        if (actionType == "moveUnit")
        {
            int unitId = actionData["unitId"];
            int q = actionData["q"];
            int r = actionData["r"];
            state->MoveUnit(unitId, q, r);

            // Broadcast updated state
            BroadcastGameState();
        }
    }

    // Optional: called when a player ends their turn
    virtual void OnTurnEnded(const string &playerName) override
    {
        AddLog("Player %s ended turn", playerName.c_str());
    }

    // Optional: called when all players have ended their turn
    virtual void OnAllTurnsEnded(int completedRound) override
    {
        AddLog("Round %d complete", completedRound);
        // Do end-of-round processing (resource income, etc.)
    }
};
```

### Turn Flow

```
Round 1: ShufflePlayerOrder() → random starting player
  │
  ▼
OnRoundStarted(1)
  │
  ▼
BroadcastRoundState() → clients receive:
  {"action": "roundState", "roundNumber": 1,
   "currentPlayer": "alice", "playerOrder": ["alice","bob"],
   "isYourTurn": true/false}
  │
  ▼
Alice's turn:
  Client sends: {"action": "moveUnit", "unitId": 5, "q": 2, "r": -1}
  Server calls: OnPlayerAction(alice, "moveUnit", data)
  │
  Client sends: {"action": "end_turn"}
  Server calls: OnTurnEnded("alice")
  Server calls: AdvanceToNextPlayer()
  │
  ▼
Bob's turn:
  BroadcastRoundState() → currentPlayer = "bob"
  ... same flow ...
  │
  Client sends: {"action": "end_turn"}
  Server calls: OnTurnEnded("bob")
  Server calls: OnAllTurnsEnded(1)
  │
  ▼
Round 2: RotatePlayerOrder() → bob goes first (rotation, not random)
  OnRoundStarted(2)
  ... repeat ...
```

### Round State JSON (sent to clients automatically)

```json
{
    "action": "roundState",
    "roundNumber": 3,
    "currentPlayer": "alice",
    "playerOrder": ["alice", "bob"],
    "endedTurn": ["bob"],
    "isYourTurn": true,
    "turnPhase": "playing"
}
```

### Player Order

- **Round 1**: `ShufflePlayerOrder()` — random order
- **Round 2+**: `RotatePlayerOrder()` — first player moves to end (ensures alternation)

### Pause/Resume

If all players disconnect, the game pauses:

```cpp
virtual void OnGamePaused() override
{
    AddLog("All players disconnected, game paused");
}

virtual void OnGameResumed() override
{
    AddLog("Players reconnected, resuming");
}
```

### State Serialization (for reconnect)

```cpp
// Save turn state alongside game state
virtual void OnSaveState() override
{
    json j;
    state->Serialize(j);

    json turnState;
    SerializeTurnState(turnState);  // Engine method
    j["turnState"] = turnState;

    // Write to disk...
}

// Restore on server restart
// Call DeserializeTurnState(j["turnState"]) after loading
```

### Available Virtual Hooks

```cpp
virtual void OnRoundStarted(int roundNumber) {}
virtual void OnTurnStarted(const string &playerName) {}
virtual void OnPlayerAction(CNetClientData *cd, const string &actionType,
                            json &actionData) {}
virtual void OnTurnEnded(const string &playerName) {}
virtual void OnAllTurnsEnded(int completedRound) {}
virtual void OnGamePaused() {}
virtual void OnGameResumed() {}
```

---

## Client-Side

### Lobby Client

```cpp
// Subclass for game-specific packet parsing
class MyLobbyClient : public CNetLobbyClient
{
public:
    MyLobbyClient(int componentId, const char *addr, int port,
                  string login, vector<u8> pwHash, CGuiViewMessages *log)
        : CNetLobbyClient(componentId, addr, port, login, pwHash, log) {}

    virtual void OnJoinableGamesPacket(json &j) override
    {
        // Parse game list specific to your game
    }

    virtual void OnPlayerProfilePacket(json &j) override
    {
        // Parse player profile
    }
};
```

### Lobby Client Callbacks

Register callbacks to react to lobby events:

```cpp
class MyLobbyView : public CGuiView, public CNetLobbyClientCallback
{
    // Called when game is ready to join
    virtual void LobbyClientCallbackGameReady(CNetLobbyClient *client,
        const string &gameId, const string &address, int port,
        const string &connectionToken) override
    {
        // Create game client and connect. The connectionToken MUST be
        // passed to the constructor — CNetClient::Connect() spawns a
        // network thread that can fire the Connected callback before
        // any post-construction field assignment is visible, causing
        // the authenticate packet to be silently skipped.
        CNetGameClient *gameClient = new CNetGameClient(
            componentId, address.c_str(), port,
            client->clientLoginName.c_str(),
            client->passwordHash, messagesLog,
            connectionToken);
    }

    virtual void LobbyClientCallbackGameFinished(CNetLobbyClient *client,
        const string &gameId, const string &winnerName,
        const string &reason) override
    {
        // Show game finished UI
    }
};
```

### Game Client

The engine `CNetGameClient` automatically tracks turn state:

```cpp
CNetGameClient *gameClient = ...;

// These fields are updated from roundState broadcasts:
gameClient->roundNumber;     // Current round
gameClient->currentPlayer;   // Who's turn
gameClient->playerOrder;     // Turn sequence
gameClient->isMyTurn;        // Is it this client's turn?
gameClient->turnPhase;       // "waiting", "playing", "paused"
```

### Sending Actions

```cpp
// Send a player action (only valid when isMyTurn == true)
json action;
action["action"] = "moveUnit";
action["unitId"] = 42;
action["q"] = 3;
action["r"] = -2;
gameClient->SendJson(action);

// End turn
json endTurn;
endTurn["action"] = "end_turn";
gameClient->SendJson(endTurn);
```

---

## Data Provider

### Player Profiles

```cpp
// Engine base profile has: name, passwordHash, joinedGameIds, gamesWon, gamesLost
// Extend for game-specific fields:

class MyProfile : public CNetGameUserProfile
{
public:
    int rating = 1000;

    virtual void Serialize(json &j) override
    {
        CNetGameUserProfile::Serialize(j);
        j["rating"] = rating;
    }

    virtual bool Deserialize(json &j) override
    {
        CNetGameUserProfile::Deserialize(j);
        rating = j.value("rating", 1000);
        return true;
    }
};

// Override factory in your data provider:
class MyDataProvider : public CNetGameDataProviderLocalFiles
{
    virtual CNetGameUserProfile *CreateProfileInstance() override
    {
        return new MyProfile();
    }
};
```

### Hjson Config (data/config.hjson)

```hjson
{
    playersFolder: data/players
    gamesFolder: data/games
    maxGamesPerPlayer: 5
    serverPort: 14500
}
```

---

## Standalone Game Server Process

For separate-process game servers (needed for multi-VPS deployment):

```cpp
// MyGameServerProcess.h
class MyGameServerProcess
{
public:
    static void Run(const CGameServerProcessBase::Config &config);
};

// MyGameServerProcess.cpp
void MyGameServerProcess::Run(const CGameServerProcessBase::Config &config)
{
    // Load your game data
    MyProject *project = new MyProject();
    project->Load();

    // Create game state
    MyGameState *state = new MyGameState(project);

    // Create game server
    json serverConfig;
    serverConfig["serverPort"] = config.port;
    MyGameServer *server = new MyGameServer(serverConfig, state);

    // Delegate to engine's generic server loop
    // (handles signal handling, lobby/registry link, main loop, shutdown)
    CGameServerProcessBase::RunServer(config, server);

    delete server;
    delete project;
}

// In main():
CGameServerProcessBase::Config config;
if (CGameServerProcessBase::ParseArgs(argc, argv, config))
{
    MyGameServerProcess::Run(config);
    return;
}
```

CLI arguments: `--game-server --game-id <id> --port <port> --lobby-address <addr> --lobby-service-port <port> --state-file <path> --app-path <path> --registry-port <port> --registry-address <addr>`

---

## Registry Modes

### Embedded (default, single machine)

Lobby runs `CLobbyServiceServer` internally on port 14600. Game servers connect directly via `CLobbyLink`.

```
Lobby ──► ServiceServer:14600 ◄── CLobbyLink ◄── GameServer
```

No extra configuration needed — this is the default.

### Standalone (multi-VPS)

A separate `CRegistryServer` process runs on a known address. Both lobby and game servers connect as clients.

```
Lobby ──► CRegistryClient ──► RegistryServer:14650 ◄── CRegistryClient ◄── GameServer
```

Switch mode in lobby:

```cpp
lobbyServer->SwitchToStandaloneRegistry("registry.example.com", 14650);
```

Game server CLI: `--registry-address registry.example.com --registry-port 14650`

---

## Thread Safety

- Network callbacks run on **background threads** (ENet)
- UI rendering runs on the **main thread**
- Use `CSlrMutex` for shared data
- `ProcessPendingStops()` — must run on main thread
- Callback lists — use copy-and-iterate pattern when removing during iteration

---

## Security

- **Rate limiting**: `MAX_PACKETS_PER_SECOND = 30` (lobby) / `60` (game)
- **Packet size**: `MAX_PACKET_SIZE = 65536` bytes
- **Token auth**: One-time tokens generated per game start, never persisted on game server
- **Heartbeat timeout**: 30 seconds (service server and registry)
- **Idle shutdown**: Game servers auto-stop after 60 seconds with no activity

---

## Logging

```cpp
// Engine classes use ILogSink for decoupled logging:
class MyLogSink : public ILogSink
{
    CGuiViewMessages *view;
public:
    MyLogSink(CGuiViewMessages *v) : view(v) {}

    virtual void AddLog(const char *fmt, ...) override
    {
        char buf[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        view->AddLog("%s", buf);
    }

    virtual void AddLogStr(const string &str) override
    {
        view->AddLog(str.c_str());
    }
};

// Assign to server:
server->logSink = new MyLogSink(view->messagesLog);
```

---

## CServerGame — Game Record

Each game instance has a `CServerGame` record:

```cpp
CServerGame *game = gamesManager->GetGame(gameId);

game->gameId;           // Unique UUID
game->mapName;          // Map name
game->mapId;            // Map numeric ID
game->playerSlots;      // vector<PlayerSlot> — name, roleId, isActive
game->serverPort;       // 0 = not running
game->gameServer;       // CNetGameServer* (NULL if remote/suspended)
game->status;           // STARTING, RUNNING, SUSPENDED, FINISHED
game->connectionTokens; // playerName → token
game->stateFilePath;    // Path to saved state JSON
```

### PlayerSlot

```cpp
struct PlayerSlot {
    int playerProfileId;
    string playerName;
    int roleId;       // Faction/role ID
    bool isActive;    // false = player left
};

// Helpers:
int count = game->GetActivePlayerCount();
string last = game->GetLastActivePlayerName();
```

---

## Complete Integration Example

### File Structure

```
MyGame/
├── src/
│   ├── Lobby/
│   │   ├── MyLobbyServer.h/.cpp          ← CNetLobbyServer
│   │   └── MyLobbyClient.h/.cpp          ← CNetLobbyClient
│   ├── Game/
│   │   ├── MyGameServer.h/.cpp           ← CTurnBasedGameServer
│   │   ├── MyGameState.h/.cpp            ← your game state
│   │   └── MyGameServerProcess.h/.cpp    ← standalone server
│   └── DataProvider/
│       ├── MyDataProvider.h/.cpp          ← CNetGameDataProviderLocalFiles
│       └── MyGamesManager.h/.cpp          ← CServerGamesManagerBase
└── data/
    ├── config.hjson
    ├── players/                           ← player JSON profiles
    └── games/                             ← game state + chat files
        └── finished/                      ← archived finished games
```

### Startup Flow

```cpp
// In your app init:

// 1. Create data provider
MyDataProvider *dataProvider = new MyDataProvider();

// 2. Create games manager
MyGamesManager *gamesManager = new MyGamesManager(dataProvider);
gamesManager->LoadAllGameRecords();  // Resume suspended games

// 3. Create lobby server
MyLobbyServer *lobbyServer = new MyLobbyServer(14500, gamesManager);
gamesManager->lobbyServer = lobbyServer;

// 4. Create network server and init
CNetServer *netServer = new CNetServer(...);
lobbyServer->Init(netServer);
netServer->Start(14500);

// 5. Main loop
while (running)
{
    gamesManager->ProcessPendingStops();   // Main thread!
    gamesManager->UpdateIdleServers();     // Check idle timeout
    // ... render UI ...
}
```

---

## Class Reference Summary

| Class | Role | Override? |
|-------|------|-----------|
| `CNetLobbyServer` | Lobby server base | **Yes** — 4 pure virtuals |
| `CNetGameServer` | Game server | Optional — virtual hooks |
| `CTurnBasedGameServer` | Turn-based game server | Optional — round hooks |
| `CServerGamesManagerBase` | Game lifecycle | **Yes** — factory method |
| `CNetGameDataProvider` | Player data base | Optional |
| `CNetGameDataProviderLocalFiles` | File-based player data | Optional — factory |
| `CNetGameUserProfile` | Player profile | Optional — extend |
| `CNetLobbyClient` | Lobby client | Optional — packet hooks |
| `CNetLobbyClientCallback` | Lobby event listener | Optional |
| `CNetGameClient` | Game client | Uses as-is |
| `CViewServerAdmin` | Admin dashboard | Optional — info hook |
| `CViewGameServer` | Game server view | Optional — info hook |
| `CViewGameClient` | Game client view | Optional |
| `CGameServerProcessBase` | Standalone server runner | Uses as-is |
| `CLobbyServiceServer` | Internal service port | Uses as-is |
| `CLobbyLink` | Game → lobby heartbeat | Uses as-is |
| `CGameServerRegistry` | Registry abstraction | Uses as-is |
| `CRegistryServer` | Standalone registry | Uses as-is |
| `CRegistryClient` | Registry client | Uses as-is |
| `CChatHistory` | Persistent chat | Uses as-is |
| `CServerGame` | Game record | Uses as-is |
| `ILogSink` | Log interface | **Yes** — implement for UI |
