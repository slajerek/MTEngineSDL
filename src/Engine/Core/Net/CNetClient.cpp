/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#include "enet.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "NET_Main.h"
#include "NET_Includes.h"
#include "CNetPacket.h"
#include "NET_AuthPw2.h"

#include <array>

CNetClient::CNetClient(const char *serverConnectAddress, int serverConnectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash)
{
	this->Init(NULL, NULL, serverConnectAddress, serverConnectPort, serverId, clientLoginName, passwordHash);
}

CNetClient::CNetClient(CNetClientCallback *clientCallback, CNetPacketCallback *packetCallback, const char *serverConnectAddress, int serverConnectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash)
{
	this->Init(clientCallback, packetCallback, serverConnectAddress, serverConnectPort, serverId, clientLoginName, passwordHash);
}

void CNetClient::Init(CNetClientCallback *clientCallback, CNetPacketCallback *packetCallback, const char *serverConnectAddress, int serverConnectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash)
{
	this->packetMutex = new CSlrMutex("CNetClient");
	
	this->status = NET_CLIENT_STATUS_OFFLINE;

	this->AddClientCallback(clientCallback);
	this->AddPacketCallback(packetCallback);

	strncpy(this->serverAddress, serverConnectAddress, sizeof(this->serverAddress) - 1);
	this->serverAddress[sizeof(this->serverAddress) - 1] = '\0';
	this->serverPort = serverConnectPort;

	this->serverId = serverId;
	this->clientLoginName = clientLoginName;
	this->passwordHash = passwordHash;

	this->peer = NULL;

	this->reconnectDelay = 1000;

	byteBufferIn = new CByteBuffer();
	byteBufferReliableOut = new CByteBuffer();
	byteBufferNotReliableOut = new CByteBuffer();
}

CNetClient::~CNetClient()
{
	delete byteBufferIn;
	delete byteBufferReliableOut;
	delete byteBufferNotReliableOut;
}

void CNetClient::SetClientLoginDetails(std::string clientLoginName, vector<u8> passwordHash)
{
	LOGD("CNetClient::SetClientLoginDetails");
	LockMutex();
	
	this->clientLoginName = clientLoginName;
	
	this->passwordHash = passwordHash;
	
	if (status == NET_CLIENT_STATUS_OFFLINE)
	{
		status = NET_CLIENT_STATUS_CONNECTING;
	}
	else 
	{
		status = NET_CLIENT_STATUS_RECONNECT;
	}
	
	UnlockMutex();
}

void CNetClient::Connect()
{
	this->status = NET_CLIENT_STATUS_CONNECTING;

	LOGD("CNetClient::Connect");
	if (isRunning == false)
	{
		SYS_StartThread(this);
	}
}

void CNetClient::ThreadRun(void *data)
{
	LOGD("CNetClient::ThreadRun: thread started");

	strcpy(threadName, "Net-Client");

	while (status != NET_CLIENT_STATUS_SHUTDOWN)
	{
		if (status == NET_CLIENT_STATUS_RECONNECT)
		{
			LOGD("NET_CLIENT_STATUS_RECONNECT sleep");
			SYS_Sleep(reconnectDelay);
			LOGD("NET_CLIENT_STATUS_RECONNECT sleep done");
		}

		if (status == NET_CLIENT_STATUS_OFFLINE)
		{
			SYS_Sleep(reconnectDelay);
			continue;
		}

		this->status = NET_CLIENT_STATUS_CONNECTING;

		ENetHost * client;
		client = enet_host_create (NULL,	// create a client host
								   1,		// only allow 1 outgoing connection
								   2,		// allow up 2 channels to be used, 0 and 1
								   0,		//57600 / 8 // 56K modem with 56 Kbps downstream bandwidth
								   0		//14400 / 8 // 56K modem with 14 Kbps upstream bandwidth
				);

		if (client == NULL)
		{
			LOGError("An error occurred while trying to create an ENet client host");
			this->status = NET_CLIENT_STATUS_RECONNECT;
			continue;
		}

		ENetAddress address;
		ENetEvent event;

		// connect
		enet_address_set_host (&address, serverAddress);
		int actualPort = SYS_ApplyPortOffset((int)serverPort);
		address.port = actualPort;

		// Initiate the connection, allocating the two channels 0 and 1.
		peer = enet_host_connect (client, &address, 2, 0);
		if (peer == NULL)
		{
			LOGError("No available peers for initiating an ENet connection");
			this->status = NET_CLIENT_STATUS_RECONNECT;
			continue;
		}

		// Wait up to 5 seconds for the connection attempt to succeed.
		LOGCFROM("Connecting to %s:%d (base %d)", serverAddress, actualPort, serverPort);
		if (enet_host_service(client, & event, 5000) > 0 &&
			event.type == ENET_EVENT_TYPE_CONNECT)
		{
			LOGCFROM("Connection to %s:%d (base %d) succeeded", serverAddress, actualPort, serverPort);
			this->status = NET_CLIENT_STATUS_CONNECTED;
		}
		else
		{
			// Either the 5 seconds are up or a disconnect event was
			// received. Reset the peer in the event the 5 seconds
			// had run out without any significant event.
			enet_peer_reset (peer);
			LOGError("Connection to %s:%d (base %d) failed", serverAddress, actualPort, serverPort);
			this->status = NET_CLIENT_STATUS_RECONNECT;
			continue;
		}

		this->status = NET_CLIENT_STATUS_CONNECTED;

		//enet_peer_ping_interval(peer, 1);

		LockMutex();
		
		// login / authorize packet
		byteBufferReliableOut->Reset();
		byteBufferReliableOut->PutByte(NET_PACKET_TYPE_AUTHORIZE);
		byteBufferReliableOut->PutU32(NET_PROTOCOL_VERSION);
		byteBufferReliableOut->PutU64(serverId);
		byteBufferReliableOut->PutStdString(clientLoginName);

		// Auth data: for PW2 challenge-response we only send the 4-byte magic.
		u16 authLen = (u16)passwordHash.size();
		u8 *authBytes = passwordHash.data();
		if (NET_AuthPw2_IsClientAuthData(passwordHash))
		{
			authLen = 4;
			authBytes = passwordHash.data();
		}
		byteBufferReliableOut->PutU16(authLen);
		if (authLen > 0)
			byteBufferReliableOut->PutBytes(authBytes, authLen);

		this->SendReliableBufferAsync(byteBufferReliableOut);
		
		UnlockMutex();

		// check login
		while (status == NET_CLIENT_STATUS_CONNECTED)
		{
			if(enet_host_service (client, & event, NET_SERVICE_EVENT_SLEEP_TIME) > 0)
			{
				switch (event.type)
				{
				case ENET_EVENT_TYPE_RECEIVE:
				{
					byteBufferIn->SetData((u8*)event.packet->data, (u32)event.packet->dataLength);

					u32 packetType = byteBufferIn->GetByte();
					bool isAuthorized = false;
					if (packetType == NET_PACKET_TYPE_AUTHORIZED)
					{
						isAuthorized = byteBufferIn->GetBool();
					}
					else if (packetType == NET_PACKET_TYPE_AUTHORIZE_CHALLENGE)
					{
						u16 challengeLen = byteBufferIn->getU16();
						u8 *challengeBytes = byteBufferIn->getBytes(challengeLen);
						std::vector<u8> challenge(challengeBytes, challengeBytes + challengeLen);

						// Snapshot auth data under lock (can be updated from UI thread).
						std::vector<u8> clientAuthData;
						std::string loginNameCopy;
						this->LockMutex();
						clientAuthData = passwordHash;
						loginNameCopy = clientLoginName;
						this->UnlockMutex();

						if (!NET_AuthPw2_IsClientAuthData(clientAuthData))
						{
							LOGError("CNetClient: received authorize challenge but client has no PW2 auth data");
							this->SetStatusDisconnectAndReconnect();
						}
						else
						{
							u8 hashVer = 0;
							u32 iters = 0;
							std::vector<u8> salt;
							std::vector<u8> nonce;
							if (!NET_AuthPw2_ParseChallenge(challenge, hashVer, iters, salt, nonce))
							{
								LOGError("CNetClient: invalid PW2 authorize challenge payload");
								this->SetStatusDisconnectAndReconnect();
							}
							else
							{
								const u8 *proofPtr = NET_AuthPw2_GetPasswordProofPtr(clientAuthData);
								std::vector<u8> proof(proofPtr, proofPtr + 32);
								std::vector<u8> key;
								if (!NET_AuthPw2_DeriveKeyFromProof(hashVer, proof, salt, iters, key))
								{
									LOGError("CNetClient: failed to derive PW2 key");
									this->SetStatusDisconnectAndReconnect();
								}
								else
								{
									auto hmac = NET_AuthPw2_ComputeHmacFromKey(key, nonce);
									std::vector<u8> responsePayload = NET_AuthPw2_BuildResponse(hmac);

									// Send authorize response.
									this->LockMutex();
									byteBufferReliableOut->Reset();
									byteBufferReliableOut->PutByte(NET_PACKET_TYPE_AUTHORIZE_RESPONSE);
									byteBufferReliableOut->PutU32(NET_PROTOCOL_VERSION);
									byteBufferReliableOut->PutU64(serverId);
									byteBufferReliableOut->PutStdString(loginNameCopy);
									byteBufferReliableOut->PutU16((u16)responsePayload.size());
									byteBufferReliableOut->PutBytes(responsePayload.data(), (u32)responsePayload.size());
									this->SendReliableBufferAsync(byteBufferReliableOut);
									this->UnlockMutex();
								}
							}
						}
					}

					// Clean up the packet now that we're done using it
					enet_packet_destroy (event.packet);
					byteBufferIn->data = NULL;

					if (byteBufferIn->error)
					{
						LOGError("FROM: parse error, disconnect");
						this->SetStatusDisconnectAndReconnect();
						break;
					}

					if (packetType == NET_PACKET_TYPE_AUTHORIZE_CHALLENGE)
					{
						// Challenge handled (or failed) above; stay in CONNECTED state.
						break;
					}
					else if (isAuthorized)
					{
						LOGCFROM("AUTHORIZED, go online");
						this->status = NET_CLIENT_STATUS_ONLINE;

						// Clear buffers from auth phase before firing callbacks,
						// so callbacks can safely queue new outgoing data
						this->LockMutex();
						byteBufferReliableOut->Reset();
						byteBufferNotReliableOut->Reset();
						this->UnlockMutex();

						{
							this->LockMutex();
							std::list<CNetClientCallback *> callbacksCopy = this->clientCallbacks;
							this->UnlockMutex();
							for (auto *callback : callbacksCopy)
							{
								callback->NetClientCallbackConnected(this);
							}
						}

						break;
					}
					else
					{
						LOGError("CONNECTED: not authorized");
						this->Disconnect();
						
						{
							this->LockMutex();
							std::list<CNetClientCallback *> callbacksCopy = this->clientCallbacks;
							this->UnlockMutex();
							for (auto *callback : callbacksCopy)
							{
								callback->NetClientCallbackNotAuthorized(this);
							}
						}

						break;
					}

//					LOGD("[c] CONNECTED: A packet of length %u containing %s was received from %s on channel %u.",
//							event.packet -> dataLength,
//							event.packet -> data,
//							event.peer -> data,
//							event.channelID);

					break;
				}
				case ENET_EVENT_TYPE_DISCONNECT:
					LOGCC("CONNECTED: disconnected from %s:%d", serverAddress, serverPort);
					event.peer->data = NULL;
					this->SetStatusDisconnectAndReconnect();
					break;

				default:
					LOGError("CONNECTED: enet_host_service: unknown event %d", event.type);
					this->SetStatusDisconnectAndReconnect();
					break;
				}
			}
		}

		while(status == NET_CLIENT_STATUS_ONLINE)
		{
			//LOGD("status == NET_CLIENT_STATUS_ONLINE");
			
			if(enet_host_service (client, & event, NET_SERVICE_EVENT_SLEEP_TIME) > 0)
			{
				switch (event.type)
				{
					case ENET_EVENT_TYPE_RECEIVE:
					{
						//LOGD("ENET_EVENT_TYPE_RECEIVE");
						u32 dataLength = (u32)event.packet->dataLength;
						//LOGD(": dataLength=%d", dataLength);
						
						byteBufferIn->SetData((u8*)event.packet->data, dataLength);
						this->ParseDataBuffer(byteBufferIn);

						// Clean up the packet now that we're done using it
						enet_packet_destroy (event.packet);
						byteBufferIn->data = NULL;

						if (byteBufferIn->error)
						{
							LOGError("FROM: parse error, disconnect");
							this->SetStatusDisconnectAndReconnect();
						}
					}
					break;

					case ENET_EVENT_TYPE_DISCONNECT:
					{
						LOGCC("FROM: EVENT DISCONNECT");

						// Reset the peer's client information.
						event.peer->data = NULL;
						this->SetStatusDisconnectAndReconnect();
						break;
					}
					default:
						LOGError("CNetClient::ThreadRun enet_host_service: unknown event %d", event.type);
						break;
				}
			}
			
			this->NetLogic();
		}

		LOGD("enet_host_destroy");
		this->LockMutex();
		enet_host_destroy(client);
		this->peer = NULL;
		this->UnlockMutex();
	}

	LOGD("CNetClient::ThreadRun: thread finished");
}

void CNetClient::NetLogic()
{
//	LOGD("CNetClient::NetLogic");

	// something in from clients?
	while(true)
	{
		this->LockMutex();
		if (this->receivedPackets.empty())
		{
			this->UnlockMutex();
			break;
		}

		std::list<CNetPacket *>::iterator it = this->receivedPackets.begin();
		CNetPacket *packet = (CNetPacket *)*it;
		this->receivedPackets.pop_front();
		this->UnlockMutex();
		
		{
			this->LockMutex();
			std::list<CNetClientCallback *> callbacksCopy = this->clientCallbacks;
			this->UnlockMutex();
			for (auto *callback : callbacksCopy)
			{
				callback->NetClientProcessPacket(packet);
			}
		}

		delete packet;
	}

//	LOGD("send out packets relOut=%d notRelOut=%d", this->byteBufferReliableOut->length, this->byteBufferNotReliableOut->length);

	this->LockMutex();
	
	// send out packets
	if (this->byteBufferReliableOut->length != 0)
	{
		SendReliableBufferAsync(byteBufferReliableOut);
		byteBufferReliableOut->Reset();
	}

	if (this->byteBufferNotReliableOut->length != 0)
	{
		SendNotReliableBufferAsync(byteBufferNotReliableOut);
		byteBufferNotReliableOut->Reset();
	}
	
	this->UnlockMutex();
	
	// net logic
	{
		std::list<CNetClientCallback *> callbacksCopy = this->clientCallbacks;
		for (auto *callback : callbacksCopy)
		{
			callback->NetClientLogic(this);
		}
	}

}

void CNetClient::IssuePacket(u8 protocolType, bool isReliable, CNetPacket *packet)
{
	packet->protocolType = protocolType;
	this->IssuePacket(isReliable, packet);
}

void CNetClient::IssuePacket(bool isReliable, CNetPacket *packet)
{
	LOGD("CNetClient::IssuePacket: status=%s", GetStatusName());
	
	LOGCCTO(this->clientLoginName.c_str(), packet->protocolType, packet->packetType, "IssuePacket");

	this->LockMutex();

	if (this->status != NET_CLIENT_STATUS_ONLINE)
	{
		LOGError("CNetClient::IssuePacket: status is %s, skipping packet", GetStatusName());
		this->UnlockMutex();
		return;
	}
	
	if (isReliable)
	{
		byteBufferReliableOut->PutU8(packet->protocolType);
		byteBufferReliableOut->PutU16(packet->packetType);
		packet->Serialize(byteBufferReliableOut);
	}
	else
	{
		byteBufferNotReliableOut->PutU8(packet->protocolType);
		byteBufferNotReliableOut->PutU16(packet->packetType);
		packet->Serialize(byteBufferNotReliableOut);
	}
//	LOGD("byteBufferReliableOut len=%d", byteBufferReliableOut->length);
//	LOGD("byteBufferNotReliableOut len=%d", byteBufferNotReliableOut->length);
	this->UnlockMutex();
}

void CNetClient::ParseDataBuffer(CByteBuffer *byteBuffer)
{
//	char *hexStr = byteBuffer->toHexString();
//	LOGCFROM("ParseDataBuffer: %s", hexStr);
//	delete hexStr;

	u16 numPackets = 0;
	while(!byteBuffer->IsEof())
	{
		LOGCFROM("parse packet buffer");

		u8 protocolType = byteBuffer->GetU8();
		u16 packetType = byteBuffer->GetU16();

		LOGCFROM("packet header: %2.2x/%4.4x", protocolType, packetType);

		if (packetType == NET_PACKET_TYPE_NOTHING)
		{
			LOGError("CNetServer::ParseDataBuffer: NET_PACKET_TYPE_NOTHING received");
		}
		else
		{
			bool parsed = false;

//			LOGD("parsing...");
			for (std::list<CNetPacketCallback *>::iterator it = this->packetCallbacks.begin();
				it != this->packetCallbacks.end(); it++)
			{
				CNetPacketCallback *callback = (*it);

//				LOGD("parsing callback");
				CNetPacket *packet = callback->NetDeserializePacket(protocolType, packetType, byteBuffer);

				if (packet != NULL)
				{
					// parsed packets
					//packet->playerData = NULL;
					packet->protocolType = protocolType;
					packet->packetType = packetType;

					this->LockMutex();
					receivedPackets.push_back(packet);
					this->UnlockMutex();

					parsed = true;
					break;
				}
			}

			if (parsed == false)
			{
				char *hexStr = byteBuffer->toHexString();
				LOGError("CNetClient::ParseDataBuffer: unknown packet type=%4.4x data=%s index=%d",
					packetType, hexStr, byteBuffer->index);
				free(hexStr);
				this->SetStatusDisconnectAndReconnect();
				return;
			}
		}

		numPackets++;
	}

	//LOGCFROM("ParseDataBuffer: done, parsed %d packets", numPackets);
}


void CNetClient::SendReliableBufferAsync(CByteBuffer *byteBuffer)
{
#if defined(DEBUG_PRINT_PACKETS)
	char *hexStr = byteBuffer->toHexString();
	LOGCTO("SendReliableBufferAsync: %s", hexStr);
	delete hexStr;
#endif

	ENetPacket *packet = enet_packet_create (byteBuffer->data, byteBuffer->length, ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send (peer, 0, packet);
	enet_host_flush(peer->host);
}

void CNetClient::SendNotReliableBufferAsync(CByteBuffer *byteBuffer)
{
#if defined(DEBUG_PRINT_PACKETS)
	char *hexStr = byteBuffer->toHexString();
	LOGCTO("SendReliableBufferAsync: %s", hexStr);
	delete hexStr;
#endif

	ENetPacket *packet = enet_packet_create (byteBuffer->data, byteBuffer->length, ENET_PACKET_FLAG_UNSEQUENCED);
	enet_peer_send (peer, 0, packet);
	enet_host_flush(peer->host);
}


void CNetClient::AddClientCallback(CNetClientCallback *clientCallback)
{
	if (clientCallback != NULL)
	{
		LOGCC("CNetClient::AddClientCallback");
		this->LockMutex();
		this->clientCallbacks.push_back(clientCallback);
		this->UnlockMutex();
	}
}

void CNetClient::RemoveClientCallback(CNetClientCallback *clientCallback)
{
	this->LockMutex();
	this->clientCallbacks.remove(clientCallback);
	this->UnlockMutex();
}

void CNetClient::AddPacketCallback(CNetPacketCallback *packetCallback)
{
	if (packetCallback != NULL)
	{
		LOGCC("CNetClient::AddPacketCallback");
		this->packetCallbacks.push_back(packetCallback);
	}
}

bool CNetClient::IsOnline()
{
	return (this->status == NET_CLIENT_STATUS_ONLINE);
}

void CNetClient::SetStatusDisconnectAndReconnect()
{
	LOGCFROM("CNetClient::SetStatusDisconnectAndReconnect");
	if (this->status == NET_CLIENT_STATUS_ONLINE)
	{
		this->LockMutex();
		std::list<CNetClientCallback *> callbacksCopy = this->clientCallbacks;
		this->UnlockMutex();
		for (auto *callback : callbacksCopy)
		{
			callback->NetClientCallbackDisconnected(this);
		}
	}
	this->status = NET_CLIENT_STATUS_RECONNECT;
}

void CNetClient::Disconnect()
{
	LOGCFROM("CNetClient::Disconnect");
	if (this->status == NET_CLIENT_STATUS_ONLINE)
	{
		this->LockMutex();
		std::list<CNetClientCallback *> callbacksCopy = this->clientCallbacks;
		this->UnlockMutex();
		for (auto *callback : callbacksCopy)
		{
			callback->NetClientCallbackDisconnected(this);
		}
	}
	this->status = NET_CLIENT_STATUS_OFFLINE;
}

const char *CNetClient::GetStatusName()
{
	switch (this->status)
	{
		case NET_CLIENT_STATUS_SHUTDOWN:
			return "SHUTDOWN";
		case NET_CLIENT_STATUS_OFFLINE:
			return "OFFLINE";
		case NET_CLIENT_STATUS_RECONNECT:
			return "RECONNECT";
		case NET_CLIENT_STATUS_CONNECTING:
			return "CONNECTING";
		case NET_CLIENT_STATUS_CONNECTED:
			return "CONNECTED";
		case NET_CLIENT_STATUS_ONLINE:
			return "ONLINE";
		default:
			return "UNKNOWN";
	}
}

void CNetClient::LockMutex()
{
	//LOGD("CNetClient::LockMutex");
	packetMutex->Lock();
}

void CNetClient::UnlockMutex()
{
	//LOGD("CNetClient::UnlockMutex");
	packetMutex->Unlock();
}

CNetClientCallback::~CNetClientCallback()
{
}

void CNetClientCallback::NetClientCallbackConnected(CNetClient *netClient)
{
	LOGD("CNetClientCallback::NetClientCallbackConnected");
}

void CNetClientCallback::NetClientCallbackDisconnected(CNetClient *netClient)
{
	LOGD("CNetClientCallback::NetClientCallbackDisconnected");
}

void CNetClientCallback::NetClientCallbackNotAuthorized(CNetClient *netClient)
{
	LOGWarning("CNetClientCallback::NetClientCallbackNotAuthorized");
}

void CNetClientCallback::NetClientProcessPacket(CNetPacket *packet)
{
	LOGError("CNetClientCallback::NetClientProcessPacket");
}

void CNetClientCallback::NetClientLogic(CNetClient *netClient)
{
}
