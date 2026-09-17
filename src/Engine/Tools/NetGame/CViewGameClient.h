#pragma once

// CAPABILITY GUARD (MT_ENABLE_NET_GAME). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_NET_GAME implies NET_TRANSPORT and HTTP, so this whole stack rides on one flag -- which did not exist until 2026-09-06 (enables was an empty list).
#if MT_ENABLE_NET_GAME

#include "CGuiView.h"
#include "CNetGameClient.h"
#include "CImGuiChatRenderer.h"

class CGuiViewMessages;

class CViewGameClient : public CGuiView
{
public:
	CViewGameClient(const char *name, float posX, float posY, float sizeX, float sizeY, string playerName, int clientId, CGuiViewMessages *messagesLog,
				 const char *titleI18nKey = NULL, const char *stableId = NULL);
	virtual ~CViewGameClient();

	virtual void RenderImGui();

	// Called after rendering the player's connection status but before the CONNECT/DISCONNECT button
	virtual void RenderCustomStatusAttributes() {}

	virtual bool HasContextMenuItems();
	virtual void RenderContextMenuItems();

	virtual void ActivateView();
	virtual void DeactivateView();

	CNetGameClient *gameClient;
	void Disconnect();

	string playerName;
	int clientId;

	CGuiViewMessages *messagesLog;

	char chatBuf[MAX_STRING_LENGTH];

private:
	CImGuiChatRenderer::ChatState chatState;
};

#endif // MT_ENABLE_NET_GAME
