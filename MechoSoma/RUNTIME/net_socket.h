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

// Network byte order address as a dotted quad, NUL terminated.
void netSocketIpToString(unsigned int ip,char* out,int sz);

int netSocketLocalAddr(netSocket s,unsigned int* ip,unsigned short* port);

/*
	TCP, used only by the lobby server and its client.

	The lobby talks over TCP because its traffic is a request/response list -
	match list, register, join - where a lost line is a stuck menu, not a
	stale frame. Hole punching needs the game's *UDP* public endpoint, which
	TCP cannot see, so each game also sends one UDP probe from its game
	socket to the lobby's UDP port and the server pairs that endpoint with the
	TCP connection by token. See net_lobby_protocol.h.
*/

// Listening socket, non-blocking. Port 0 lets the OS pick one.
netSocket netSocketTcpListen(unsigned short port);

// A pending connection on a listener, NET_SOCKET_INVALID when there is none.
// A listener must be polled; it never blocks.
netSocket netSocketTcpAccept(netSocket listener,unsigned int* ip,unsigned short* port);

// Starts a non-blocking connect. The socket comes back at once and is only
// usable once netSocketTcpStatus() reports 1.
netSocket netSocketTcpConnect(unsigned int ip,unsigned short port);

// 0 still connecting, 1 connected, -1 failed or closed.
int netSocketTcpStatus(netSocket s);

int netSocketTcpSend(netSocket s,const void* data,int len);

// Returns the bytes read, NET_SOCKET_WOULDBLOCK when nothing is pending, or
// NET_SOCKET_ERROR. A return of 0 means the peer closed the connection.
int netSocketTcpRecv(netSocket s,void* data,int len);

// Peer address of a connected socket. ip is network byte order.
int netSocketTcpPeerAddr(netSocket s,unsigned int* ip,unsigned short* port);

// Last platform error, never NULL.
const char* netSocketError(void);

#endif /* __NET_SOCKET_H__ */