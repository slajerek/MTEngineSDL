#pragma once


// CAPABILITY GUARD (MT_ENABLE_NET_GAME). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_NET_GAME implies NET_TRANSPORT and HTTP, so this whole stack rides on one flag -- which did not exist until 2026-09-06 (enables was an empty list).
#if MT_ENABLE_NET_GAME

#include "CGuiView.h"

class CNetLobbyServer;
class CServerGamesManagerBase;

class CViewServerAdmin : public CGuiView
{
public:
	CViewServerAdmin(const char *name, float posX, float posY, float sizeX, float sizeY, const char *titleI18nKey = NULL, const char *stableId = NULL);
	virtual ~CViewServerAdmin();

	virtual void RenderImGui();

	virtual bool HasContextMenuItems();
	virtual void RenderContextMenuItems();

	virtual void ActivateView();
	virtual void DeactivateView();

	// Virtual hook for subclasses to render game-specific lobby info.
	// Called inside the Lobby Server section, after connected clients count.
	virtual void RenderLobbyGameSpecificInfo() {}

	// Data sources — set after construction
	CNetLobbyServer *lobbyServer;
	CServerGamesManagerBase *gamesManager;
};

#endif // MT_ENABLE_NET_GAME
