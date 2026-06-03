#include "CViewGameClient.h"
#include "CGuiMain.h"
#include "CNetGameClient.h"
#include "CNetClient.h"
#include "CGuiViewMessages.h"
#include "CChatHistory.h"
#include "CImGuiChatRenderer.h"
#include "CI18nManager.h"
#include "json.hpp"

using namespace ImGui;
using namespace nlohmann;

CViewGameClient::CViewGameClient(const char *name, float posX, float posY, float sizeX, float sizeY, string playerName, int clientId, CGuiViewMessages *messagesLog,
							 const char *titleI18nKey, const char *stableId)
: CGuiView(name, posX, posY, -1, sizeX, sizeY, titleI18nKey, stableId)
{
	chatBuf[0] = 0;

	this->messagesLog = messagesLog;
	this->playerName = playerName;
	this->clientId = clientId;

	gameClient = NULL;
}

CViewGameClient::~CViewGameClient()
{
}

void CViewGameClient::Disconnect()
{
	if (gameClient && gameClient->netClient)
	{
		gameClient->netClient->Disconnect();
	}
	gameClient = NULL;
}

void CViewGameClient::RenderImGui()
{
	PreRenderImGui();

	if (gameClient)
	{
		Text("%s", playerName.c_str());
		SameLine();

		u8 clientStatus = gameClient->netClient->status;
		if (gameClient->isAuthenticated && clientStatus == NET_CLIENT_STATUS_ONLINE)
		{
			if (gameClient->turnPhase == "paused")
			{
				TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", _T("game.status.paused"));
			}
			else
			{
				TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", _T("game.status.online"));
			}
		}
		else if (clientStatus == NET_CLIENT_STATUS_RECONNECT || clientStatus == NET_CLIENT_STATUS_CONNECTING)
		{
			TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%s", _T("game.status.reconnecting"));
		}
		else
		{
			Text("%s", gameClient->netClient->GetStatusName());
		}
		
		RenderCustomStatusAttributes();
		
		const float btnWidth = 100.0f;
		float alignX = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - btnWidth;
		ImGui::SameLine(alignX);

		if (gameClient->netClient->status == NET_CLIENT_STATUS_ONLINE)
		{
			if (Button(_T("game.btn.disconnect"), ImVec2(btnWidth, 0)))
			{
				gameClient->netClient->Disconnect();
			}
		}
		else
		{
			if (Button(_T("game.btn.connect"), ImVec2(btnWidth, 0)))
			{
				gameClient->netClient->Connect();
			}
		}

		// Round info
		if (gameClient->roundNumber > 0)
		{
			Separator();

			Text(_T("game.round"), gameClient->roundNumber);
			SameLine();
			Text(_T("game.current_player"), gameClient->currentPlayerClientId);
			SameLine();

			if (gameClient->isMyTurn)
			{
				TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", _T("game.your_turn"));
			}
			else
			{
				TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), _T("game.waiting_for_player"), gameClient->currentPlayerClientId);
			}

			Separator();
		}

		// Chat input row — pass i18n label for Send button
		if (CImGuiChatRenderer::RenderChatInput(chatBuf, MAX_STRING_LENGTH, 80.0f, _T("game.btn.send")))
		{
			if (chatBuf[0] != 0)
			{
				json j;
				j["action"] = "chat";
				j["message"] = string(chatBuf);
				LOGD("CViewGameClient: sending chat json=%s", j.dump().c_str());
				gameClient->SendJson(j);
				chatBuf[0] = 0;
			}
		}

		// Chat area — pass i18n labels
		{
			chatState.messages = &gameClient->chatMessages;
			chatState.isLoadingHistory = &gameClient->isLoadingHistory;
			chatState.hasMoreHistory = &gameClient->hasMoreHistory;
			float btnH = (gameClient->roundNumber > 0) ? (GetFrameHeight() * 2.1f + GetStyle().ItemSpacing.y) : 0;
			float chatHeight = GetContentRegionAvail().y - btnH;
			CImGuiChatRenderer::Labels labels;
			labels.loading = _T("game.chat.loading");
			labels.beginningOfChat = _T("game.chat.beginning");
			CImGuiChatRenderer::RenderChatArea(chatState, chatHeight, [this]() {
				gameClient->RequestMoreHistory();
			}, labels);
		}

		// END TURN button — full width, 3x height, blue, disabled when not your turn
		if (gameClient->roundNumber > 0)
		{
			bool canAct = gameClient->isMyTurn && gameClient->isAuthenticated;
			if (!canAct) BeginDisabled();
			PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.45f, 0.8f, 1.0f));
			PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.60f, 1.0f, 1.0f));
			PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.1f, 0.35f, 0.7f, 1.0f));
			if (Button(_T("game.btn.end_turn"), ImVec2(-1, GetFrameHeight() * 2.1f)))
			{
				json j;
				j["action"] = "endTurn";
				gameClient->SendJson(j);
			}
			PopStyleColor(3);
			if (!canAct) EndDisabled();
		}
	}
	else
	{
		TextDisabled("%s", _T("game.status.not_connected"));
	}

	PostRenderImGui();
}

bool CViewGameClient::HasContextMenuItems()
{
	return false;
}

void CViewGameClient::RenderContextMenuItems()
{
}

void CViewGameClient::ActivateView()
{
	LOGG("CViewGameClient::ActivateView()");
}

void CViewGameClient::DeactivateView()
{
	LOGG("CViewGameClient::DeactivateView()");
}
