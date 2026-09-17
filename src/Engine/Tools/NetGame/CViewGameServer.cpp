// CAPABILITY GUARD (MT_ENABLE_NET_GAME). The file stays in all three build lists and
// compiles to nothing when the capability is off -- an Xcode PBXBuildFile
// cannot be conditioned on a build setting, so the condition lives here.
// MT_CAP_NET_GAME implies NET_TRANSPORT and HTTP, so this whole stack rides on one flag -- which did not exist until 2026-09-06 (enables was an empty list).
#if MT_ENABLE_NET_GAME

#include "CViewGameServer.h"
#include "CGuiMain.h"
#include "CNetGameServer.h"
#include "CNetServer.h"
#include "CNetClientData.h"
#include "CGuiViewMessages.h"
#include <vector>

using namespace ImGui;

CViewGameServer::CViewGameServer(const char *name, float posX, float posY, float sizeX, float sizeY, CGuiViewMessages *messagesLog, const char *titleI18nKey, const char *stableId)
: CGuiView(name, posX, posY, -1, sizeX, sizeY, titleI18nKey, stableId)
{
	this->messagesLog = messagesLog;
	server = NULL;
}

CViewGameServer::~CViewGameServer()
{
}

void CViewGameServer::RenderImGui()
{
	PreRenderImGui();

	if (server != NULL)
	{
		// Game ID
		Text("Game ID:");
		SameLine();
		if (!server->gameId.empty())
		{
			string shortId = server->gameId.size() > 8 ? server->gameId.substr(0, 8) : server->gameId;
			TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", shortId.c_str());
			if (IsItemHovered())
				SetTooltip("%s", server->gameId.c_str());
		}
		else
		{
			TextDisabled("(none)");
		}

		// Game-specific info hook
		RenderGameSpecificInfo();

		// Port
		Text("Port: %d", server->serverPort);

		// Snapshot clients[] under the network thread's clientsMutex: ConnectPeer()
		// and Disconnected() mutate state/clientName on the network thread while
		// this render thread would otherwise read them unguarded (Phase A final
		// review, Minor 1). Copy what we need, release the lock, then render.
		struct SClientSnapshot
		{
			u16 id;
			u8 state;
			std::string stateName;
			std::string clientName;
		};
		std::vector<SClientSnapshot> clientsSnapshot;
		clientsSnapshot.reserve(NET_MAX_CLIENTS);

		server->netServer->clientsMutex->Lock();
		for (u16 i = 0; i < NET_MAX_CLIENTS; i++)
		{
			CNetClientData *clientData = server->netServer->clients[i];
			if (clientData == NULL)
				continue;

			SClientSnapshot snap;
			snap.id = i;
			snap.state = clientData->state;
			snap.stateName = clientData->GetStateName();
			snap.clientName = clientData->clientName;
			clientsSnapshot.push_back(snap);
		}
		server->netServer->clientsMutex->Unlock();

		// Connected clients count
		int connectedCount = 0;
		for (const SClientSnapshot &snap : clientsSnapshot)
		{
			if (snap.state == NET_CLIENT_STATE_ONLINE)
				connectedCount++;
		}
		Text("Connected: %d / %d", connectedCount, (int)server->allowedPlayers.size());

		Separator();

		// Clients table
		if (BeginTable("##GameServerClients", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
		{
			TableSetupColumn("ID");
			TableSetupColumn("Status");
			TableSetupColumn("Name");
			TableHeadersRow();

			for (const SClientSnapshot &snap : clientsSnapshot)
			{
				if (snap.state != NET_CLIENT_STATE_EMPTY)
				{
					TableNextRow();
					TableNextColumn(); Text("%d", snap.id);
					TableNextColumn(); Text("%s", snap.stateName.c_str());
					TableNextColumn(); Text("%s", snap.clientName.c_str());
				}
			}
			EndTable();
		}
	}
	else
	{
		TextDisabled("No game server running");
	}

	PostRenderImGui();
}

bool CViewGameServer::HasContextMenuItems()
{
	return false;
}

void CViewGameServer::RenderContextMenuItems()
{
}

void CViewGameServer::ActivateView()
{
	LOGG("CViewGameServer::ActivateView()");
}

void CViewGameServer::DeactivateView()
{
	LOGG("CViewGameServer::DeactivateView()");
}

#endif // MT_ENABLE_NET_GAME
