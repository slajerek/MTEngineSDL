#pragma once
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
