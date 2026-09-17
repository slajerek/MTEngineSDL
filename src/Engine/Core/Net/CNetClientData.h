/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _NET_CLIENT_DATA_
#define _NET_CLIENT_DATA_


// CAPABILITY GUARD (MT_ENABLE_ENET). The header stays INCLUDABLE either way and simply
// declares nothing when the capability is off, so a translation unit that
// includes it conditionally still compiles. Same shape as
// Video/CVideoSourceFFmpeg.h.
// MT_CAP_NET_TRANSPORT declared this removable and nothing enforced it: MT_ENABLE_ENET appeared in no source, so the code shipped whatever the manifest said.
#if MT_ENABLE_ENET

#include "SYS_Defs.h"
#include "NET_Main.h"
#include "enet.h"
#include <list>
#include <vector>
#include <string>

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

	int clientId;  // generic numeric identity, set during authentication. -1 = unknown

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

	// Pre-online authorize challenge state (for challenge-response auth).
	bool authorizePending = false;
	u64 authorizePendingServerId = 0;
	std::string authorizePendingUserName;
	std::vector<u8> authorizePendingChallenge;
	u64 authorizePendingIssuedAtMillis = 0;

	u8 state;

	bool Receive(u32 frameNum);
	void Send(u32 frameNum);

	const char *GetStateName();
};


#endif // MT_ENABLE_ENET

#endif
