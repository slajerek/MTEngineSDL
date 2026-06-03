#pragma once

#include <string>
using namespace std;

class CNetGameServer;

// Generic standalone game server process.
// Provides CLI argument parsing and a main loop that handles signal handling,
// lobby/registry link setup, event loop, and graceful shutdown.
// Game-specific code creates the server, then calls RunServer().
class CGameServerProcessBase
{
public:
	struct Config
	{
		string gameId;
		int port = 0;
		string lobbyAddress = "localhost";
		int lobbyServicePort = 14600;
		string stateFilePath;
		string appPath;
		string internalSecret; // shared secret for internal services (lobby-service / registry)

		// Standalone registry mode (if registryPort > 0, use CRegistryClient instead of CLobbyLink)
		string registryAddress;
		int registryPort = 0;
	};

	// Parse --game-server CLI args. Returns true if --game-server flag was found.
	static bool ParseArgs(int argc, const char **argv, Config &config);

	// Run the generic server event loop: installs signal handlers, sets up
	// lobby link or registry client, runs main loop, and performs clean shutdown.
	// The caller creates and owns the gameServer; this function blocks until shutdown.
	static void RunServer(const Config &config, CNetGameServer *gameServer);
};
