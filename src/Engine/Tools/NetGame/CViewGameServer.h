#pragma once

// CAPABILITY GUARD (MT_ENABLE_NET_GAME). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_NET_GAME implies NET_TRANSPORT and HTTP, so this whole stack rides on one flag -- which did not exist until 2026-09-06 (enables was an empty list).
#if MT_ENABLE_NET_GAME

#include "CGuiView.h"

class CNetGameServer;
class CGuiViewMessages;

class CViewGameServer : public CGuiView
{
public:
	CViewGameServer(const char *name, float posX, float posY, float sizeX, float sizeY, CGuiViewMessages *messagesLog, const char *titleI18nKey = NULL, const char *stableId = NULL);
	virtual ~CViewGameServer();

	virtual void RenderImGui();

	virtual bool HasContextMenuItems();
	virtual void RenderContextMenuItems();

	virtual void ActivateView();
	virtual void DeactivateView();

	// Virtual hook for game-specific info rendered after Game ID
	virtual void RenderGameSpecificInfo() {}

	//
	CNetGameServer *server;
	CGuiViewMessages *messagesLog;
};

#endif // MT_ENABLE_NET_GAME
