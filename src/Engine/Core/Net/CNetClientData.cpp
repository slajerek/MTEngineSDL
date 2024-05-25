/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#include "enet.h"
#include "CNetClientData.h"
#include "CNetServer.h"
#include "CByteBuffer.h"

CNetClientData::CNetClientData(CNetServer *server, ENetPeer *peer, u32 peerId)
{
	this->state = NET_CLIENT_STATE_EMPTY;
	this->server = server;
	this->componentId = -1;
	this->voidData = NULL;
	this->peer = peer;
	this->peerId = peerId;
	this->totalNumReceived = 0;
	this->byteBufferReliableOut = new CByteBuffer();
	this->byteBufferNotReliableOut = new CByteBuffer();
	sprintf(this->clientName, "<PEER#%d>", peerId);
}

void CNetClientData::SetClientName(char *userName)
{
	memset(clientName, 0x00, 32);
	strncpy(clientName, userName, 30);
}

void CNetClientData::SetVoidData(void *voidData)
{
	this->voidData = voidData;
}

const char *CNetClientData::GetStateName()
{
	switch(this->state)
	{
		case NET_CLIENT_STATE_EMPTY: return "EMPTY";
		case NET_CLIENT_STATE_CONNECTING: return "CONNECTING";
		case NET_CLIENT_STATE_CONNECTED: return "CONNECTED";
		case NET_CLIENT_STATE_ONLINE: return "ONLINE";
		case NET_CLIENT_STATE_DISCONNECT: return "DISCONNECT";
		default: return "<UNKNOWN>";
	}
}

bool CNetClientData::Receive(u32 frameNum)
{
	return false;
}

void CNetClientData::Send(u32 frameNum)
{
}

