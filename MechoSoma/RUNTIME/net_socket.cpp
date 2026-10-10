/* ---------------------------- INCLUDE SECTION ----------------------------- */

#include "net_socket.h"

#if defined(_WIN32)

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif

	// Must stay ahead of every other include in this file, see net_socket.h.
	#include <winsock2.h>
	#include <ws2tcpip.h>

	typedef SOCKET netSocketHandle;
	#define NET_INVALID_SOCKET INVALID_SOCKET

#else

	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <netdb.h>
	#include <unistd.h>
	#include <fcntl.h>
	#include <cerrno>

	typedef int netSocketHandle;
	#define NET_INVALID_SOCKET (-1)

#endif

#include <cstdio>
#include <cstring>

/* ----------------------------- EXTERN SECTION ----------------------------- */

#if defined(_WIN32)
static char netSockError[256] = "";
#else
static char netSockError[256] = "";
#endif

/* --------------------------- PROTOTYPE SECTION ---------------------------- */

static netSocketHandle netSocketSetNonBlocking(netSocketHandle s);
static int netSocketLastError(void);

/* ---------------------------- DEFINITION SECTION -------------------------- */

#if defined(_WIN32)

int netSocketStartup(void)
{
	WSADATA wsa;

	if(netSocketLastError() == 0)
		return 1;

	if(WSAStartup(MAKEWORD(2,2),&wsa) != 0){
		sprintf(netSockError,"WSAStartup failed: %d",WSAGetLastError());
		return 0;
	}

	netSockError[0] = 0;

	return 1;
}

void netSocketFinit(void)
{
	WSACleanup();
}

netSocket netSocketOpen(unsigned short port)
{
	struct sockaddr_in sa;
	netSocketHandle s;

	s = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
	if(s == NET_INVALID_SOCKET){
		sprintf(netSockError,"socket() failed: %d",WSAGetLastError());
		return NET_SOCKET_INVALID;
	}

	memset(&sa,0,sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);

	if(bind(s,(struct sockaddr*)&sa,sizeof(sa)) == SOCKET_ERROR){
		sprintf(netSockError,"bind() failed on port %u: %d",port,WSAGetLastError());
		closesocket(s);
		return NET_SOCKET_INVALID;
	}

	s = netSocketSetNonBlocking(s);
	if(s == NET_INVALID_SOCKET)
		return NET_SOCKET_INVALID;

	return (netSocket)s;
}

int netSocketSend(netSocket s,unsigned int ip,unsigned short port,const void* data,int len)
{
	struct sockaddr_in sa;
	int res;

	memset(&sa,0,sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = ip;
	sa.sin_port = htons(port);

	res = sendto((netSocketHandle)s,(const char*)data,len,0,(struct sockaddr*)&sa,sizeof(sa));
	if(res == SOCKET_ERROR){
		sprintf(netSockError,"sendto() failed: %d",WSAGetLastError());
		return NET_SOCKET_ERROR;
	}

	return res;
}

int netSocketRecv(netSocket s,unsigned int* ip,unsigned short* port,void* data,int len)
{
	struct sockaddr_in from;
	int from_sz = sizeof(from);
	int res;

	memset(&from,0,sizeof(from));

	res = recvfrom((netSocketHandle)s,(char*)data,len,0,(struct sockaddr*)&from,&from_sz);
	if(res == SOCKET_ERROR){
		int err = WSAGetLastError();
		if(err == WSAEWOULDBLOCK)
			return NET_SOCKET_WOULDBLOCK;

		sprintf(netSockError,"recvfrom() failed: %d",err);
		return NET_SOCKET_ERROR;
	}

	if(ip) *ip = from.sin_addr.s_addr;
	if(port) *port = ntohs(from.sin_port);

	return res;
}

void netSocketClose(netSocket s)
{
	if(s != NET_SOCKET_INVALID)
		closesocket((netSocketHandle)s);
}

int netSocketResolve(const char* host,unsigned int* ip)
{
	struct addrinfo hints;
	struct addrinfo* res = NULL;
	int rc;

	if(!host || !*host || !ip) return 0;

	memset(&hints,0,sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	rc = getaddrinfo(host,NULL,&hints,&res);
	if(rc != 0 || !res){
		sprintf(netSockError,"getaddrinfo() failed for '%s'",host);
		if(res) freeaddrinfo(res);
		return 0;
	}

	*ip = ((struct sockaddr_in*)res -> ai_addr) -> sin_addr.s_addr;
	freeaddrinfo(res);

	return 1;
}

int netSocketLocalAddr(netSocket s,unsigned int* ip,unsigned short* port)
{
	struct sockaddr_in sa;
	int sz = sizeof(sa);

	memset(&sa,0,sizeof(sa));

	if(getsockname((netSocketHandle)s,(struct sockaddr*)&sa,&sz) == SOCKET_ERROR){
		sprintf(netSockError,"getsockname() failed: %d",WSAGetLastError());
		return 0;
	}

	if(ip) *ip = sa.sin_addr.s_addr;
	if(port) *port = ntohs(sa.sin_port);

	return 1;
}

static int netSocketLastError(void)
{
	return netSockError[0] ? 0 : 1;
}

static netSocketHandle netSocketSetNonBlocking(netSocketHandle s)
{
	u_long mode = 1;

	if(ioctlsocket(s,FIONBIO,&mode) == SOCKET_ERROR){
		sprintf(netSockError,"ioctlsocket() failed: %d",WSAGetLastError());
		closesocket(s);
		return NET_INVALID_SOCKET;
	}

	return s;
}

#else /* !_WIN32 */

int netSocketStartup(void)
{
	return 1;
}

void netSocketFinit(void)
{
}

netSocket netSocketOpen(unsigned short port)
{
	struct sockaddr_in sa;
	netSocketHandle s;

	s = socket(AF_INET,SOCK_DGRAM,0);
	if(s == NET_INVALID_SOCKET){
		sprintf(netSockError,"socket() failed: %d",errno);
		return NET_SOCKET_INVALID;
	}

	memset(&sa,0,sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);

	if(bind(s,(struct sockaddr*)&sa,sizeof(sa)) < 0){
		sprintf(netSockError,"bind() failed on port %u: %d",port,errno);
		close(s);
		return NET_SOCKET_INVALID;
	}

	s = netSocketSetNonBlocking(s);
	if(s == NET_INVALID_SOCKET)
		return NET_SOCKET_INVALID;

	return (netSocket)s;
}

int netSocketSend(netSocket s,unsigned int ip,unsigned short port,const void* data,int len)
{
	struct sockaddr_in sa;
	ssize_t res;

	memset(&sa,0,sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = ip;
	sa.sin_port = htons(port);

	res = sendto((netSocketHandle)s,data,len,0,(struct sockaddr*)&sa,sizeof(sa));
	if(res < 0){
		sprintf(netSockError,"sendto() failed: %d",errno);
		return NET_SOCKET_ERROR;
	}

	return (int)res;
}

int netSocketRecv(netSocket s,unsigned int* ip,unsigned short* port,void* data,int len)
{
	struct sockaddr_in from;
	socklen_t from_sz = sizeof(from);
	ssize_t res;

	memset(&from,0,sizeof(from));

	res = recvfrom((netSocketHandle)s,data,len,0,(struct sockaddr*)&from,&from_sz);
	if(res < 0){
		if(errno == EAGAIN || errno == EWOULDBLOCK)
			return NET_SOCKET_WOULDBLOCK;

		sprintf(netSockError,"recvfrom() failed: %d",errno);
		return NET_SOCKET_ERROR;
	}

	if(ip) *ip = from.sin_addr.s_addr;
	if(port) *port = ntohs(from.sin_port);

	return (int)res;
}

void netSocketClose(netSocket s)
{
	if(s != NET_SOCKET_INVALID)
		close((netSocketHandle)s);
}

int netSocketResolve(const char* host,unsigned int* ip)
{
	struct addrinfo hints;
	struct addrinfo* res = NULL;

	if(!host || !*host || !ip) return 0;

	memset(&hints,0,sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	if(getaddrinfo(host,NULL,&hints,&res) != 0 || !res){
		sprintf(netSockError,"getaddrinfo() failed for '%s'",host);
		if(res) freeaddrinfo(res);
		return 0;
	}

	*ip = ((struct sockaddr_in*)res -> ai_addr) -> sin_addr.s_addr;
	freeaddrinfo(res);

	return 1;
}

int netSocketLocalAddr(netSocket s,unsigned int* ip,unsigned short* port)
{
	struct sockaddr_in sa;
	socklen_t sz = sizeof(sa);

	memset(&sa,0,sizeof(sa));

	if(getsockname((netSocketHandle)s,(struct sockaddr*)&sa,&sz) < 0){
		sprintf(netSockError,"getsockname() failed: %d",errno);
		return 0;
	}

	if(ip) *ip = sa.sin_addr.s_addr;
	if(port) *port = ntohs(sa.sin_port);

	return 1;
}

static int netSocketLastError(void)
{
	return 0;
}

static netSocketHandle netSocketSetNonBlocking(netSocketHandle s)
{
	int flags = fcntl(s,F_GETFL,0);

	if(flags < 0 || fcntl(s,F_SETFL,flags | O_NONBLOCK) < 0){
		sprintf(netSockError,"fcntl() failed: %d",errno);
		close(s);
		return NET_INVALID_SOCKET;
	}

	return s;
}

#endif /* _WIN32 */

const char* netSocketError(void)
{
	return netSockError;
}