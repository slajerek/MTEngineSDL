#include "CViewGameServer.h"
#include "CGuiMain.h"
#include "CNetGameServer.h"
#include "CNetServer.h"
#include "CNetClientData.h"
#include "CGuiViewMessages.h"

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

		// Connected clients count
		int connectedCount = 0;
		for (u16 i = 0; i < NET_MAX_CLIENTS; i++)
		{
			if (server->netServer->clients[i]->state == NET_CLIENT_STATE_ONLINE)
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

			for (u16 i = 0; i < NET_MAX_CLIENTS; i++)
			{
				if (server->netServer->clients[i]->state != NET_CLIENT_STATE_EMPTY)
				{
					TableNextRow();
					TableNextColumn(); Text("%d", i);
					TableNextColumn(); Text("%s", server->netServer->clients[i]->GetStateName());
					TableNextColumn(); Text("%s", server->netServer->clients[i]->clientName.c_str());
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
