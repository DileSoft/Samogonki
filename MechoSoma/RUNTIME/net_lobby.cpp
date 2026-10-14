#include "StdAfx.h"

#include "net_socket.h"
#include "net_lobby.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ---------------------------- construction -------------------------------- */

netLobbyClient::netLobbyClient(void)
{
	_sock = NET_SOCKET_INVALID;
	_state = NET_LOBBY_IDLE;
	_status[0] = 0;
	_token[0] = 0;
	_server_ip = 0;
	_server_port = 0;
	_inlen = 0;
	_outlen = 0;
	_in[0] = 0;
	_out[0] = 0;
	_match_count = 0;
	_registered_id = 0;
	_peer_count = 0;
}

netLobbyClient::~netLobbyClient(void)
{
	close();
}

void netLobbyClient::set_status(const char* text)
{
	if(!text) return;

	strncpy(_status,text,sizeof(_status) - 1);
	_status[sizeof(_status) - 1] = 0;
}

/* ------------------------------- lifecycle -------------------------------- */

void netLobbyClient::close(void)
{
	if(_sock != NET_SOCKET_INVALID){
		netSocketClose(_sock);
		_sock = NET_SOCKET_INVALID;
	}

	_state = NET_LOBBY_IDLE;
	_token[0] = 0;
	_inlen = 0;
	_outlen = 0;
	_match_count = 0;
	_registered_id = 0;
	_peer_count = 0;
	_status[0] = 0;

	// Empty the buffers as well as their lengths: the parse walks them from
	// index 0 and only the length says where the live bytes end, so a buffer
	// left NUL-terminated nowhere could be read as a line that was never sent.
	_in[0] = 0;
	_out[0] = 0;
}

void netLobbyClient::dial(const char* host,unsigned short port)
{
	unsigned int ip = 0;

	close();

	if(!host || !*host){
		_state = NET_LOBBY_ERROR;
		set_status("No lobby address");
		return;
	}

	if(!netSocketResolve(host,&ip)){
		_state = NET_LOBBY_ERROR;
		set_status(netSocketError());
		return;
	}

	_sock = netSocketTcpConnect(ip,port);
	if(_sock == NET_SOCKET_INVALID){
		_state = NET_LOBBY_ERROR;
		set_status(netSocketError());
		return;
	}

	_server_ip = ip;
	_server_port = port;
	_state = NET_LOBBY_CONNECTING;
	set_status("Connecting to lobby");
}

void netLobbyClient::pump(void)
{
	if(_state == NET_LOBBY_CONNECTING){
		int st = netSocketTcpStatus(_sock);

		if(st < 0){
			_state = NET_LOBBY_ERROR;
			set_status("Lobby unreachable");
			return;
		}

		if(st == 0) return;

		char line[64];
		int n = snprintf(line,sizeof(line),"HELLO %d",LOBBY_PROTO_VERSION);
		(void)n;

		queue(line);
		_state = NET_LOBBY_HELLO;
		set_status("Saying hello to lobby");
		return;
	}

	if(_state != NET_LOBBY_HELLO && _state != NET_LOBBY_READY)
		return;

	flush_out();

	if(!read_in()){
		_state = NET_LOBBY_ERROR;
		set_status("Lobby closed the connection");
		return;
	}
}

/* ------------------------------- output ----------------------------------- */

void netLobbyClient::queue(const char* line)
{
	int len = (int)strlen(line);

	if(_outlen + len + 1 >= (int)sizeof(_out)) return;

	memcpy(_out + _outlen,line,len);
	_outlen += len;
	_out[_outlen ++] = '\n';
	_out[_outlen] = 0;
}

void netLobbyClient::flush_out(void)
{
	while(_outlen > 0){
		int n = netSocketTcpSend(_sock,_out,_outlen);

		if(n == NET_SOCKET_WOULDBLOCK) break;
		if(n <= 0) break;

		memmove(_out,_out + n,_outlen - n);
		_outlen -= n;
	}

	_out[_outlen] = 0;
}

/* -------------------------------- input ----------------------------------- */

int netLobbyClient::read_in(void)
{
	for(;;){
		int room = (int)sizeof(_in) - 1 - _inlen;

		if(room <= 1) break;

		int n = netSocketTcpRecv(_sock,_in + _inlen,room);

		if(n == NET_SOCKET_WOULDBLOCK) break;
		if(n <= 0) return 0;

		_inlen += n;
		_in[_inlen] = 0;
	}

	char* start = _in;

	for(;;){
		char* nl = strchr(start,'\n');
		if(!nl) break;

		*nl = 0;
		handle_line(start);
		start = nl + 1;
	}

	int rem = (int)strlen(start);
	if(start != _in) memmove(_in,start,rem + 1);
	_inlen = rem;

	return 1;
}

void netLobbyClient::handle_line(char* line)
{
	int n = (int)strlen(line);
	while(n > 0 && line[n - 1] == '\r') line[-- n] = 0;

	if(!n) return;

	if(!strncmp(line,"HELLO ",6)){
		int version = 0;
		char tok[LOBBY_TOKEN_LEN] = "";

		if(sscanf(line,"HELLO %d %11s",&version,tok) == 2){
			strncpy(_token,tok,sizeof(_token) - 1);
			_token[sizeof(_token) - 1] = 0;
			_state = NET_LOBBY_READY;
			set_status("In the lobby");
		}
		return;
	}

	if(!strncmp(line,"REGISTERED ",11)){
		_registered_id = (unsigned short)atoi(line + 11);
		return;
	}

	if(!strncmp(line,"LIST ",5)){
		_match_count = 0;
		return;
	}

	if(!strncmp(line,"MATCH ",6)){
		if(_match_count < LOBBY_MAX_MATCHES){
			netLobbyMatch* m = &_matches[_match_count];
			unsigned short id,players,max,world,laps;
			int off = 0;

			if(sscanf(line,"MATCH %hu %hu %hu %hu %hu %n",
				  &id,&players,&max,&world,&laps,&off) == 5){
				memset(m,0,sizeof(*m));

				m -> id = id;
				m -> players = players;
				m -> max_players = max;
				m -> world = world;
				m -> laps = laps;

				const char* name = line + off;
				while(*name == ' ') name ++;

				strncpy(m -> name,name,sizeof(m -> name) - 1);
				m -> name[sizeof(m -> name) - 1] = 0;

				_match_count ++;
			}
		}
		return;
	}

	if(!strcmp(line,"END")) return;

	if(!strncmp(line,"PEER ",5)){
		char ipstr[64];
		unsigned pport = 0;

		if(sscanf(line,"PEER %63s %u",ipstr,&pport) == 2 && _peer_count < 8){
			unsigned int ip = 0;

			if(netSocketResolve(ipstr,&ip)){
				_peers[_peer_count].ip = ip;
				_peers[_peer_count].port = (unsigned short)pport;
				_peer_count ++;
			}
		}
		return;
	}

	if(!strncmp(line,"ERROR ",6)){
		set_status(line + 6);
		return;
	}
}

/* ------------------------------- requests --------------------------------- */

void netLobbyClient::register_match(const char* name,int world,int laps,int players,int max)
{
	char line[LOBBY_MAX_LINE];

	if(!ready()) return;

	snprintf(line,sizeof(line),"REGISTER %d %d %d %d %s",
		 world,laps,players,max,name ? name : "");

	queue(line);
}

void netLobbyClient::unregister_match(void)
{
	if(ready()) queue("UNREGISTER");
}

void netLobbyClient::request_list(void)
{
	if(ready()) queue("LIST");
}

void netLobbyClient::join(unsigned short match_id)
{
	char line[64];

	if(!ready()) return;

	snprintf(line,sizeof(line),"JOIN %u",(unsigned)match_id);
	queue(line);
}

/* -------------------------------- views ----------------------------------- */

const netLobbyMatch* netLobbyClient::match(int index) const
{
	if(index < 0 || index >= _match_count) return NULL;
	return &_matches[index];
}

int netLobbyClient::pop_peer(netLobbyPeer* out)
{
	if(_peer_count <= 0) return 0;

	if(out) *out = _peers[0];

	for(int i = 1; i < _peer_count; i ++)
		_peers[i - 1] = _peers[i];

	_peer_count --;

	return 1;
}

void netLobbyClient::clear_peers(void)
{
	_peer_count = 0;
}
