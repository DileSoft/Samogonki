#ifndef __NET_SOCKET_H__
#define __NET_SOCKET_H__

/*
	Raw UDP socket layer for the network transport.

	Kept in a dedicated translation unit that includes NO project headers on
	purpose: <winsock2.h> drags in <windows.h>, and windows.h collides with
	two things the game already declares:
		- enum dbtype3ds { ... Unknown ... } in 3dsftk3/include/3dsftk.h
		- WritePrivateProfileString() in XTool/port.h
	So this header only speaks primitives, and net_socket.cpp is the single
	place in the whole build where <winsock2.h> appears.
*/

#include <cstddef>

typedef int netSocket;

const netSocket NET_SOCKET_INVALID = -1;

const int NET_SOCKET_ERROR		= -1;
const int NET_SOCKET_WOULDBLOCK	= -2;

// One time per process, call before anything else.
int netSocketStartup(void);
void netSocketFinit(void);

// Binds a UDP socket to port (0 lets the OS pick one).
netSocket netSocketOpen(unsigned short port);

// Returns the size of the datagram written, NET_SOCKET_ERROR on failure.
int netSocketSend(netSocket s,unsigned int ip,unsigned short port,const void* data,int len);

// Returns the size of the datagram read, NET_SOCKET_WOULDBLOCK when the
// socket is empty. *ip / *port receive the sender in network byte order.
int netSocketRecv(netSocket s,unsigned int* ip,unsigned short* port,void* data,int len);

void netSocketClose(netSocket s);

// Resolves a host name (or dotted quad) to a network byte order address.
int netSocketResolve(const char* host,unsigned int* ip);

int netSocketLocalAddr(netSocket s,unsigned int* ip,unsigned short* port);

// Last platform error, never NULL.
const char* netSocketError(void);

#endif /* __NET_SOCKET_H__ */