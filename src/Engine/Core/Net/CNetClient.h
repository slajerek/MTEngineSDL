/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _CNETCLIENT_H_
#define _CNETCLIENT_H_

#include "enet.h"
#include "SYS_Defs.h"
#include "SYS_Threading.h"
#include <list>

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
};


#endif
//_CNETCLIENT_H_
