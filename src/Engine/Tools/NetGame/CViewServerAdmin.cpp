#include "CViewServerAdmin.h"
#include "CGuiMain.h"
#include "CNetLobbyServer.h"
#include "CNetServer.h"
#include "CNetClientData.h"
#include "CServerGamesManagerBase.h"
#include "CServerGame.h"
#include "CGameServerRegistry.h"
#include "CLobbyServiceServer.h"
#include "CRegistryClient.h"

#include <ctime>

using namespace ImGui;
using namespace std;

CViewServerAdmin::CViewServerAdmin(const char *name, float posX, float posY, float sizeX, float sizeY, const char *titleI18nKey, const char *stableId)
: CGuiView(name, posX, posY, -1, sizeX, sizeY, titleI18nKey, stableId)
{
	lobbyServer = NULL;
	gamesManager = NULL;
}

CViewServerAdmin::~CViewServerAdmin()
{
}

void CViewServerAdmin::RenderImGui()
{
	PreRenderImGui();

	// -- Section 1: Lobby Server --
	if (CollapsingHeader("Lobby Server", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (lobbyServer && lobbyServer->netServer)
		{
			TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "RUNNING");
			SameLine();
			Text("Port: %d", lobbyServer->serverPort);

			// Launch mode
			if (gamesManager)
			{
				SameLine();
				Spacing(); SameLine();
				const char *launchModeStr = (gamesManager->launchMode == ServerLaunchMode::IN_PROCESS)
					? "IN_PROCESS" : "SEPARATE_PROCESS";
				TextDisabled("Launch: %s", launchModeStr);
			}

			// Registry mode
			if (lobbyServer->registry)
			{
				const char *registryModeStr = (lobbyServer->registry->mode == RegistryMode::EMBEDDED)
					? "EMBEDDED" : "STANDALONE";
				SameLine();
				Spacing(); SameLine();
				TextDisabled("Registry: %s", registryModeStr);
			}

			// Count connected clients
			int connectedCount = 0;
			for (u16 i = 0; i < NET_MAX_CLIENTS; i++)
			{
				if (lobbyServer->netServer->clients[i]->state == NET_CLIENT_STATE_ONLINE)
				{
					connectedCount++;
				}
			}
			Text("Connected clients: %d / %d", connectedCount, NET_MAX_CLIENTS);

			// Virtual hook for game-specific lobby info
			RenderLobbyGameSpecificInfo();

			if (TreeNode("Connected Clients"))
			{
				if (BeginTable("##LobbyClients", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
				{
					TableSetupColumn("ID");
					TableSetupColumn("Status");
					TableSetupColumn("Name");
					TableHeadersRow();

					for (u16 i = 0; i < NET_MAX_CLIENTS; i++)
					{
						if (lobbyServer->netServer->clients[i]->state != NET_CLIENT_STATE_EMPTY)
						{
							TableNextRow();
							TableNextColumn(); Text("%d", i);
							TableNextColumn(); Text("%s", lobbyServer->netServer->clients[i]->GetStateName());
							TableNextColumn(); Text("%s", lobbyServer->netServer->clients[i]->clientName.c_str());
						}
					}

					EndTable();
				}
				TreePop();
			}
		}
		else
		{
			TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "NOT RUNNING");
		}
	}

	Separator();

	// -- Section 2: Service Server (EMBEDDED mode only) --
	if (lobbyServer && lobbyServer->registry && lobbyServer->registry->mode == RegistryMode::EMBEDDED)
	{
		if (CollapsingHeader("Service Server", ImGuiTreeNodeFlags_DefaultOpen))
		{
			CLobbyServiceServer *serviceServer = lobbyServer->serviceServer;
			if (serviceServer && serviceServer->netServer)
			{
				// Status
				if (serviceServer->netServer->isRunning)
				{
					TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "RUNNING");
				}
				else
				{
					TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "NOT RUNNING");
				}
				SameLine();
				Text("Port: %d", CLobbyServiceServer::SERVICE_PORT);

				// Counts
				int gsCount = serviceServer->GetGameServerCount();
				int pendingCount = serviceServer->GetPendingTokensCount();
				Text("Registered game servers: %d", gsCount);
				Text("Pending tokens: %d", pendingCount);

				// Registered game servers table
				if (gsCount > 0 && TreeNode("Registered Game Servers"))
				{
					vector<CGameServerRecord *> records = serviceServer->GetAllGameServerRecords();

					if (BeginTable("##ServiceGameServers", 5,
								   ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
								   ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
								   ImVec2(0, 200)))
					{
						TableSetupColumn("Game ID");
						TableSetupColumn("Address");
						TableSetupColumn("Players");
						TableSetupColumn("Status");
						TableSetupColumn("Last Heartbeat");
						TableHeadersRow();

						time_t now = time(NULL);

						for (const auto *record : records)
						{
							TableNextRow();

							// Game ID (short)
							TableNextColumn();
							string shortId = record->gameId.size() > 8 ? record->gameId.substr(0, 8) : record->gameId;
							Text("%s", shortId.c_str());
							if (IsItemHovered())
							{
								SetTooltip("%s", record->gameId.c_str());
							}

							// Address:Port
							TableNextColumn();
							Text("%s:%d", record->listenAddress.c_str(), record->listenPort);

							// Connected Players
							TableNextColumn();
							Text("%d", record->connectedPlayers);

							// Status (color-coded)
							TableNextColumn();
							if (record->status == "running")
								TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "running");
							else if (record->status == "saving")
								TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "saving");
							else if (record->status == "shutting_down" || record->status == "shutdown")
								TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", record->status.c_str());
							else if (record->status == "unreachable")
								TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "unreachable");
							else
								Text("%s", record->status.c_str());

							// Last Heartbeat (relative)
							TableNextColumn();
							int elapsed = (int)difftime(now, record->lastHeartbeat);
							if (elapsed < 60)
								Text("%ds ago", elapsed);
							else if (elapsed < 3600)
								Text("%dm ago", elapsed / 60);
							else
								Text("%dh ago", elapsed / 3600);
						}

						EndTable();
					}
					TreePop();
				}
			}
			else
			{
				TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "NOT RUNNING");
			}
		}

		Separator();
	}

	// -- Section 3: Registry (STANDALONE mode only) --
	if (lobbyServer && lobbyServer->registry && lobbyServer->registry->mode == RegistryMode::STANDALONE)
	{
		if (CollapsingHeader("Registry", ImGuiTreeNodeFlags_DefaultOpen))
		{
			CRegistryClient *regClient = lobbyServer->registry->registryClient;
			if (regClient)
			{
				// Connection status
				if (regClient->isConnected)
					TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "CONNECTED");
				else
					TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "DISCONNECTED");

				SameLine();
				Spacing(); SameLine();

				// Registration status
				if (regClient->isRegistered)
					TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "REGISTERED");
				else
					TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "NOT REGISTERED");

				// Client ID
				Text("Lobby ID: %s", regClient->clientId.c_str());
			}
			else
			{
				TextDisabled("Registry client not available");
			}
		}

		Separator();
	}

	// -- Section 4: Game Servers --
	if (CollapsingHeader("Game Servers", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (gamesManager)
		{
			vector<CServerGame *> allGames = gamesManager->GetAllGames();

			// Summary counters
			int runningCount = 0, startingCount = 0, suspendedCount = 0;
			for (const auto *game : allGames)
			{
				switch (game->status)
				{
					case CServerGame::RUNNING:   runningCount++;   break;
					case CServerGame::STARTING:  startingCount++;  break;
					case CServerGame::SUSPENDED: suspendedCount++; break;
				}
			}
			Text("%d running, %d starting, %d suspended", runningCount, startingCount, suspendedCount);

			// Port pool summary
			int usedPorts = 0;
			for (const auto *game : allGames)
			{
				if (game->serverPort > 0) usedPorts++;
			}
			int totalPorts = gamesManager->portRangeEnd - gamesManager->portRangeStart + 1;
			Text("Port pool: %d / %d used (range %d-%d)",
				 usedPorts, totalPorts,
				 gamesManager->portRangeStart,
				 gamesManager->portRangeEnd);

			// Progress bar for port utilization
			float utilization = totalPorts > 0 ? (float)usedPorts / totalPorts : 0.0f;
			ProgressBar(utilization, ImVec2(-1, 0), "");

			Text("Total games: %d", (int)allGames.size());

			if (suspendedCount > 0)
			{
				SameLine();
				char btnLabel[64];
				snprintf(btnLabel, sizeof(btnLabel), "Cleanup Suspended (%d)", suspendedCount);
				if (Button(btnLabel))
				{
					int removed = gamesManager->RemoveSuspendedGames();
					LOGM("CViewServerAdmin: cleaned up %d suspended games", removed);
				}
			}

			Separator();

			if (allGames.empty())
			{
				TextDisabled("No games");
			}
			else
			{
				// Determine if we need PID column
				bool showPid = (gamesManager->launchMode == ServerLaunchMode::SEPARATE_PROCESS);
				int numColumns = showPid ? 8 : 7;

				if (BeginTable("##GameServers", numColumns,
							   ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
							   ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
							   ImVec2(0, 300)))
				{
					TableSetupColumn("Game ID");
					TableSetupColumn("Scenario");
					TableSetupColumn("Status");
					TableSetupColumn("Port");
					if (showPid) TableSetupColumn("PID");
					TableSetupColumn("Players");
					TableSetupColumn("Connected");
					TableSetupColumn("Last Activity");
					TableHeadersRow();

					time_t now = time(NULL);

					for (const auto *game : allGames)
					{
						TableNextRow();

						// Game ID (short form)
						TableNextColumn();
						string shortId = game->gameId.size() > 8 ? game->gameId.substr(0, 8) : game->gameId;
						Text("%s", shortId.c_str());
						if (IsItemHovered())
						{
							SetTooltip("%s", game->gameId.c_str());
						}

						// Map
						TableNextColumn();
						Text("%s", game->mapName.c_str());

						// Status with color
						TableNextColumn();
						switch (game->status)
						{
							case CServerGame::STARTING:
								TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "STARTING");
								break;
							case CServerGame::RUNNING:
								TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "RUNNING");
								break;
							case CServerGame::SUSPENDED:
								TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "SUSPENDED");
								break;
						}

						// Port
						TableNextColumn();
						if (game->serverPort > 0)
							Text("%d", game->serverPort);
						else
							TextDisabled("--");

						// PID (SEPARATE_PROCESS mode only)
						if (showPid)
						{
							TableNextColumn();
							if (game->processId != 0)
								Text("%d", (int)game->processId);
							else
								TextDisabled("--");
						}

						// Players list
						TableNextColumn();
						{
							string playersStr;
							for (size_t p = 0; p < game->playerSlots.size(); p++)
							{
								if (p > 0) playersStr += ", ";
								playersStr += game->playerSlots[p].playerName;
							}
							Text("%s", playersStr.c_str());
						}

						// Connected count
						TableNextColumn();
						Text("%d/%d", game->connectedPlayerCount, (int)game->playerSlots.size());

						// Last activity (relative time)
						TableNextColumn();
						int elapsed = (int)difftime(now, game->lastActivityTime);
						if (elapsed < 60)
							Text("%ds ago", elapsed);
						else if (elapsed < 3600)
							Text("%dm ago", elapsed / 60);
						else
							Text("%dh ago", elapsed / 3600);
					}

					EndTable();
				}
			}
		}
		else
		{
			TextDisabled("Games manager not available");
		}
	}

	PostRenderImGui();
}

bool CViewServerAdmin::HasContextMenuItems()
{
	return false;
}

void CViewServerAdmin::RenderContextMenuItems()
{
}

void CViewServerAdmin::ActivateView()
{
	LOGG("CViewServerAdmin::ActivateView()");
}

void CViewServerAdmin::DeactivateView()
{
	LOGG("CViewServerAdmin::DeactivateView()");
}
