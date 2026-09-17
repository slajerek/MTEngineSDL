/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

// NET_Initialize() EXISTS IN BOTH BRANCHES, and that is the whole point of the
// #else below. Core/SYS_Startup.cpp calls it unconditionally on two separate
// paths -- service mode and normal mode -- so compiling this function out
// would break the link of every build with MT_CAP_NET_TRANSPORT=0, which is
// the configuration the capability is supposed to make possible.
#if MT_ENABLE_ENET

// enet ipv6 https://github.com/Ericson2314/enet
#include "enet.h"

void NET_Test();

void NET_Initialize()
{
	enet_initialize();
	//NET_Test();
}

#else

void NET_Initialize()
{
	// No transport in this build. Deliberately silent rather than an error:
	// startup calls this on every path, so a log line here would fire on every
	// launch of an app that never asked for networking.
}

#endif // MT_ENABLE_ENET

#if MT_ENABLE_ENET

void NET_Test()
{
	/*
	LOGM("NET_Test");
	if (enet_initialize () != 0)
    {
        SYS_FatalExit("An error occurred while initializing ENet");
    }
	
	CNetServer *server = new CNetServer(NULL);
	server->Host();

	SYS_Sleep(1000);
	
	CNetClient *client = new CNetClient();
	client->Connect();

	while(server->isRunning)
	{
		SYS_Sleep(100);
	}
	
	enet_deinitialize();
	LOGM("NET_Test finished");*/
}

#endif // MT_ENABLE_ENET
