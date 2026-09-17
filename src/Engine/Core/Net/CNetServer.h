/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _CNETSERVER_H_
#define _CNETSERVER_H_


// CAPABILITY GUARD (MT_ENABLE_ENET). The header stays INCLUDABLE either way and simply
// declares nothing when the capability is off, so a translation unit that
// includes it conditionally still compiles. Same shape as
// Video/CVideoSourceFFmpeg.h.
// MT_CAP_NET_TRANSPORT declared this removable and nothing enforced it: MT_ENABLE_ENET appeared in no source, so the code shipped whatever the manifest said.
#if MT_ENABLE_ENET

#include "SYS_Defs.h"
#ifdef _WIN32
#include <winsock2.h>
#endif
#include "enet.h"
#include "SYS_Threading.h"
#include <atomic>
#include <list>
#include <string>
#include <vector>

class CNetConnection;
class CNetClient;
class CNetPacket;
class CNetPacketCallback;
class CNetClientData;
class CByteBuffer;

#define NET_MAX_CLIENTS	32
#define NET_AUTH_CHALLENGE_TIMEOUT_MS 30000

#define NET_SERVER_STATUS_OFFLINE	0
#define NET_SERVER_STATUS_DISCONNECTED	1
#define NET_SERVER_STATUS_SETUP		2
#define NET_SERVER_STATUS_ONLINE	3
#define NET_SERVER_STATUS_SHUTDOWN	4

#define NET_SERVER_CALLBACK_AUTHORIZE_NOT_AVAILABLE 0
#define NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD 1
#define NET_SERVER_CALLBACK_AUTHORIZE_CORRECT 2
// Server requests a challenge-response round-trip.
// CNetServer will send NET_PACKET_TYPE_AUTHORIZE_CHALLENGE and wait for NET_PACKET_TYPE_AUTHORIZE_RESPONSE.
#define NET_SERVER_CALLBACK_AUTHORIZE_CHALLENGE 3

class CNetServer;

class CNetServerCallback
{
public:
	virtual ~CNetServerCallback();
	virtual void NetServerCallbackClientConnected(CNetClientData *clientData);
	virtual void NetServerCallbackClientDisconnected(CNetClientData *clientData);
	virtual void NetServerProcessPacket(CNetPacket *packet);
	virtual void NetServerLogic(CNetServer *netServer);
	virtual u8 NetServerAuthorize(CNetClientData *clientData, std::string userName, std::vector<u8> passwordHash);
};

class CNetServer : public CSlrThread
{
public:
	CNetServer(int serverPort);
	CNetServer(CNetServerCallback *serverCallback, CNetPacketCallback *packetCallback, int serverPort);
	void Init(CNetServerCallback *serverCallback, CNetPacketCallback *packetCallback, int serverPort);

	virtual ~CNetServer();
	
	int serverPort;

	// Address to bind the listening socket to. Empty (the default) binds
	// ENET_HOST_ANY (all interfaces) -- the historical behaviour. A
	// production caller sets this to "127.0.0.1" before StartServer() to
	// keep the socket off the public interface (security review).
	std::string bindAddress;

	u32 currentFrame;

	// starts thread
	void StartServer();

	// Requests the network thread to stop and blocks (polling, up to 5000ms)
	// until it has actually exited (status == NET_SERVER_STATUS_OFFLINE).
	// No-op if the server was never started. Logs LOGError if the wait
	// times out -- that is an error path, not routine shutdown.
	void StopServer();

	virtual void ThreadRun(void *data);

	// triggered by frame logic
	bool Receive(u32 frameNum);
	void Send(u32 frameNum);

	volatile u8 status;

	// Set by StopServer() before flipping status to SHUTDOWN, and checked by
	// ThreadRun right after enet_host_create -- closes the race where
	// StopServer() lands between StartServer() and the bind completing
	// (status would otherwise be stomped back to ONLINE and never exit).
	std::atomic<bool> shutdownRequested{false};

	CNetClientData *clients[NET_MAX_CLIENTS];
	CNetClientData *ConnectPeer(ENetPeer *peer);

	// Guards clients[] state/name reads from a non-network thread (e.g. an
	// ImGui debug view) against ConnectPeer()/Disconnected() mutating it on
	// the network thread. Take it, copy what you need, release it, then render.
	CSlrMutex *clientsMutex;

	void ParseDataBuffer(CNetClientData *netClientData, CByteBuffer *byteBuffer);
	bool ParseAuthorize(CNetClientData *netClientData, CByteBuffer *byteBuffer);
	bool ParseAuthorizeResponse(CNetClientData *netClientData, CByteBuffer *byteBuffer);

	void Disconnect(CNetClientData *netClientData);
	void Disconnected(CNetClientData *netClientData);

	std::list<CNetServerCallback *> serverCallbacks;
	std::list<CNetPacketCallback *> packetCallbacks;

	CByteBuffer *byteBufferIn;
	CByteBuffer *byteBufferReliableOut;
	CByteBuffer *byteBufferNotReliableOut;

	const char *GetPacketNameFromType(u16 protocolType, u16 packetType);

	void SendReliableBufferAsync(CNetClientData *clientData, CByteBuffer *byteBuffer);
	void SendNotReliableBufferAsync(CNetClientData *clientData, CByteBuffer *byteBuffer);

	void AddServerCallback(CNetServerCallback *serverCallback);
	void RemoveServerCallback(CNetServerCallback *serverCallback);
	void AddPacketCallback(CNetPacketCallback *packetCallback);
	
	void IssuePacket(CNetClientData *clientData, CNetPacket *packet);
	void BroadcastPacket(CNetPacket *packet);

	std::list<CNetPacket *> receivedPackets;

	void NetLogic();

	const char *GetStatusName();

	CSlrMutex *packetMutex;
	void LockMutex();
	void UnlockMutex();
	
};

#endif // MT_ENABLE_ENET

#endif
//_CNETSERVER_H_
