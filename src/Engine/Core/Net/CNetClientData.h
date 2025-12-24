/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _NET_CLIENT_DATA_
#define _NET_CLIENT_DATA_

#include "SYS_Defs.h"
#include "NET_Main.h"
#include "enet.h"
#include <list>

class CNetServer;
class CNetPacket;
class CByteBuffer;

#define NET_CLIENT_STATE_EMPTY	0
#define NET_CLIENT_STATE_CONNECTING	1
#define NET_CLIENT_STATE_CONNECTED	2
#define NET_CLIENT_STATE_ONLINE		3
#define NET_CLIENT_STATE_DISCONNECT	4

class CNetClientData
{
public:
	// playerId is unique on server
	CNetClientData(CNetServer *server, ENetPeer *peer, u32 peerId);
	u32 peerId;
	u32 totalNumReceived;

	std::string clientName;
	void SetClientName(std::string userName);

	CNetServer *server;
	ENetPeer *peer;
	CByteBuffer *byteBufferReliableOut;
	CByteBuffer *byteBufferNotReliableOut;
	
	/// 1000         | 1001 | 1002 | 1003
	/// current turn |      |      | execute
	std::list<CNetPacket *> currentFramePackets;
	std::list<CNetPacket *> framePackets[NUM_FRAMES_AHEAD_BUFFER];

	void *voidData;
	void SetVoidData(void *voidData);

	u8 state;

	bool Receive(u32 frameNum);
	void Send(u32 frameNum);

	const char *GetStateName();
};


#endif

