/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _CNETCLIENT_H_
#define _CNETCLIENT_H_


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
#include <functional>
#include <list>
#include <vector>
#include <string>
#include <thread>

#define NET_CLIENT_STATUS_SHUTDOWN		0
#define NET_CLIENT_STATUS_OFFLINE		1
#define NET_CLIENT_STATUS_RECONNECT		2
#define NET_CLIENT_STATUS_CONNECTING	3
#define NET_CLIENT_STATUS_CONNECTED		4
#define NET_CLIENT_STATUS_ONLINE		5

class CNetConnection;
class CNetServer;
class CNetClient;
class CNetPacket;
class CNetPacketCallback;
class CByteBuffer;

class CNetClientCallback
{
public:
	virtual ~CNetClientCallback();
	virtual void NetClientCallbackConnected(CNetClient *netClient);
	virtual void NetClientCallbackDisconnected(CNetClient *netClient);
	virtual void NetClientCallbackNotAuthorized(CNetClient *netClient);
	virtual void NetClientProcessPacket(CNetPacket *packet);
	virtual void NetClientLogic(CNetClient *netClient);
};

class CNetClient : public CSlrThread
{
public:
	CNetClient(const char *serverConnectAddress, int serverConnectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash);
	CNetClient(CNetClientCallback *clientCallback, CNetPacketCallback *packetCallback, const char *serverConnectAddress, int serverConntectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash);
	void Init(CNetClientCallback *clientCallback, CNetPacketCallback *packetCallback, const char *serverConnectAddress, int serverConntectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash);

	virtual ~CNetClient();
	
	// starts thread
	void Connect();

	virtual void ThreadRun(void *data);

	u64 serverId;
	std::string clientLoginName;
	std::vector<u8> passwordHash;

	// Optional hook, consulted on the network thread immediately before every
	// authorize packet is built (initial connect and each RECONNECT) --
	// OUTSIDE packetMutex, since it is expected to be a blocking round trip
	// (e.g. an HTTPS call fetching a fresh single-use lobby ticket). When set
	// and it returns a non-empty vector, that value replaces passwordHash
	// before the authorize packet is assembled. An empty return means "no
	// fresh data available"; the previous passwordHash is reused so the
	// lobby simply rejects it (caller logs why).
	//
	// Must only be set via SetAuthDataProvider() (below) -- never assign the
	// private member directly. ThreadRun copies the std::function under
	// packetMutex before invoking it outside the lock, so an unsynchronized
	// direct assignment from another thread (e.g. the UI thread calling
	// Connect() right after) would race that copy: assigning a std::function
	// while another thread mid-copies/invokes it is undefined behavior, not
	// just a stale read.
	void SetAuthDataProvider(std::function<std::vector<u8>()> provider);

	// Clears the provider AND waits for any provider call already in flight on
	// the network thread to return (bounded, ~10s) before returning.
	//
	// Clearing alone is not enough for the caller that owns a `this`-capturing
	// lambda: ThreadRun copies the std::function under packetMutex and calls the
	// COPY outside the lock (deliberately -- it is a blocking HTTPS round trip),
	// so a plain SetAuthDataProvider(nullptr) returns immediately even while the
	// previous provider is mid-call with a 3s connect + 3s read budget. A
	// destructor that freed its object on that return had up to ~6s of exposure
	// (final-review I-2). Destructors and Disconnect() must use THIS.
	//
	// Called FROM the network thread (i.e. from inside a provider call) it would
	// deadlock on itself, so it detects that case by thread id, skips the wait
	// and logs a LOGWarning.
	void ClearAuthDataProvider();

	void SetClientLoginDetails(std::string clientLoginName, std::vector<u8> passwordHash);

	bool Receive(u32 frameNum);
	void Send(u32 frameNum);

	volatile u8 status;

	char serverAddress[256];
	u16 serverPort;

	u32 reconnectDelay;

	CByteBuffer *byteBufferIn;
	CByteBuffer *byteBufferReliableOut;
	CByteBuffer *byteBufferNotReliableOut;
	std::list<CNetPacket *> receivedPackets;

	ENetPeer *peer;

	std::list<CNetClientCallback *> clientCallbacks;
	std::list<CNetPacketCallback *> packetCallbacks;

	void SendNotReliableBufferAsync(CByteBuffer *byteBuffer);
	void SendReliableBufferAsync(CByteBuffer *byteBuffer);
	void AddClientCallback(CNetClientCallback *clientCallback);
	void RemoveClientCallback(CNetClientCallback *clientCallback);
	void AddPacketCallback(CNetPacketCallback *packetCallback);

	void ParseDataBuffer(CByteBuffer *byteBuffer);

	void NetLogic();

	CSlrMutex *packetMutex;
	void LockMutex();
	void UnlockMutex();
	
	bool IsOnline();
	void SetStatusDisconnectAndReconnect();
	void Disconnect();
	
	const char *GetStatusName();

	void IssuePacket(bool isReliable, CNetPacket *packet);
	void IssuePacket(u8 protocolType, bool isReliable, CNetPacket *packet);

private:
	// See SetAuthDataProvider() above -- always accessed under packetMutex
	// (set by SetAuthDataProvider, copied out by ThreadRun before invoking it
	// unlocked).
	std::function<std::vector<u8>()> authDataProvider;

	// Number of provider invocations currently executing outside the lock.
	// Incremented under packetMutex together with the copy, decremented under
	// packetMutex after the call returns; ClearAuthDataProvider() polls it.
	int authProviderCallsInFlight = 0;

	// ThreadRun's own thread id, published once at thread start under
	// packetMutex. ClearAuthDataProvider() compares against it so a call made
	// from inside a provider (i.e. on this very thread) never waits on itself.
	std::thread::id netThreadId;
	bool netThreadIdValid = false;
};


#endif // MT_ENABLE_ENET

#endif
//_CNETCLIENT_H_
