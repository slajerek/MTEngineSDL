// CAPABILITY GUARD (MT_ENABLE_ENET). The file stays in all three build lists and
// compiles to nothing when the capability is off -- the guard-the-body,
// keep-the-file idiom, which exists because an Xcode PBXBuildFile cannot be
// conditioned on a build setting the way a CMake or MSBuild item can.
// MT_CAP_NET_TRANSPORT declared this removable and nothing enforced it: MT_ENABLE_ENET appeared in no source, so the code shipped whatever the manifest said.
#if MT_ENABLE_ENET

/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#include <string>
#include "enet.h"
#include "CNetClientData.h"
#include "CNetServer.h"
#include "CByteBuffer.h"

CNetClientData::CNetClientData(CNetServer *server, ENetPeer *peer, u32 peerId)
{
	this->state = NET_CLIENT_STATE_EMPTY;
	this->server = server;
	this->voidData = NULL;
	this->peer = peer;
	this->peerId = peerId;
	this->clientId = -1;
	this->totalNumReceived = 0;
	this->byteBufferReliableOut = new CByteBuffer();
	this->byteBufferNotReliableOut = new CByteBuffer();
	this->clientName = "<PEER#" + std::to_string(peerId) + ">";
}

void CNetClientData::SetClientName(std::string userName)
{
	this->clientName = userName;
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

#endif // MT_ENABLE_ENET
