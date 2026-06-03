#pragma once
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
