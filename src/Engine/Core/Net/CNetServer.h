/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _CNETSERVER_H_
#define _CNETSERVER_H_

#include "enet.h"
#include "SYS_Defs.h"
#include "SYS_Threading.h"
#include <list>
#include <vector>

class CNetConnection;
class CNetClient;
class CNetPacket;
class CNetPacketCallback;
class CNetClientData;
class CByteBuffer;

#define NET_MAX_CLIENTS	32

#define NET_SERVER_STATUS_OFFLINE	0
#define NET_SERVER_STATUS_DISCONNECTED	1
#define NET_SERVER_STATUS_SETUP		2
#define NET_SERVER_STATUS_ONLINE	3
#define NET_SERVER_STATUS_SHUTDOWN	4

#define NET_SERVER_CALLBACK_AUTHORIZE_NOT_AVAILABLE 0
#define NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD 1
#define NET_SERVER_CALLBACK_AUTHORIZE_CORRECT 2

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
	
	u32 currentFrame;

	// starts thread
	void StartServer();

	virtual void ThreadRun(void *data);

	// triggered by frame logic
	bool Receive(u32 frameNum);
	void Send(u32 frameNum);

	volatile u8 status;

	CNetClientData *clients[NET_MAX_CLIENTS];
	CNetClientData *ConnectPeer(ENetPeer *peer);

	void ParseDataBuffer(CNetClientData *netClientData, CByteBuffer *byteBuffer);
	bool ParseAuthorize(CNetClientData *netClientData, CByteBuffer *byteBuffer);

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

#endif
//_CNETSERVER_H_

