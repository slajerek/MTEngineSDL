/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _NET_MAIN_H_
#define _NET_MAIN_H_

// enet ipv6 https://github.com/Ericson2314/enet

#define NET_SERVICE_EVENT_SLEEP_TIME 20

#define NET_PROTOCOL_VERSION	1

#define NUM_FRAMES_AHEAD	2
#define NUM_FRAMES_AHEAD_BUFFER		1024

void NET_Initialize();

#define NET_SERVER_TYPE_SERVER_MAIN       0

#endif
