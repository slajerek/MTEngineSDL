#pragma once

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
