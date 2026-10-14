#ifndef __NET_LOBBY_H__
#define __NET_LOBBY_H__

/*
	The game's side of the lobby: a non-blocking TCP connection to the lobby
	server, the match list it hands back, and the peer endpoints it introduces.

	Nothing here touches the race. It owns one TCP socket and a byte buffer,
	and is pumped once a frame from the menu. The UDP probe that teaches the
	server this game's public endpoint is deliberately NOT sent from here: it
	has to leave the game's own session socket, so net_game sends it, reading
	token() and server_ip()/server_port() from this object.

	See net_lobby_protocol.h for the wire format.
*/

#include "net_lobby_protocol.h"
#include "net_socket.h"

enum netLobbyState
{
	NET_LOBBY_IDLE = 0,
	NET_LOBBY_CONNECTING,		// TCP handshake in progress
	NET_LOBBY_HELLO,		// connected, HELLO sent, waiting for the token
	NET_LOBBY_READY,
	NET_LOBBY_ERROR
};

struct netLobbyMatch
{
	unsigned short id;
	unsigned short players;
	unsigned short max_players;
	unsigned short world;
	unsigned short laps;
	char name[LOBBY_MATCH_NAME_LEN];
};

struct netLobbyPeer
{
	unsigned int ip;
	unsigned short port;
};

class netLobbyClient
{
public:
	netLobbyClient(void);
	~netLobbyClient(void);

	// Starts a non-blocking connection. Replaces any previous one.
	void dial(const char* host,unsigned short port);
	void close(void);
	void pump(void);

	int state(void) const { return _state; }
	int ready(void) const { return _state == NET_LOBBY_READY; }
	const char* status(void) const { return _status; }
	const char* token(void) const { return _token; }
	unsigned int server_ip(void) const { return _server_ip; }
	unsigned short server_port(void) const { return _server_port; }

	// Host side.
	void register_match(const char* name,int world,int laps,int players,int max);
	void unregister_match(void);

	// Client side.
	void request_list(void);
	void join(unsigned short match_id);

	int match_count(void) const { return _match_count; }
	const netLobbyMatch* match(int index) const;

	// Endpoints the server introduced. Pop consumes one; they queue.
	int pop_peer(netLobbyPeer* out);
	void clear_peers(void);

	// 1 once, if the last JOIN came back "no match". Reading it clears it.
	int take_join_rejected(void);

private:
	netSocket _sock;
	int _state;
	char _status[128];
	char _token[LOBBY_TOKEN_LEN];

	unsigned int _server_ip;
	unsigned short _server_port;

	char _in[4096];
	int _inlen;
	char _out[1024];
	int _outlen;

	netLobbyMatch _matches[LOBBY_MAX_MATCHES];
	int _match_count;

	unsigned short _registered_id;

	// Set when the server answers a JOIN with "no match": the pick is stale
	// (the host re-created its game and the old id is gone), so the caller
	// must drop it and list again rather than keep asking for a dead id.
	int _join_rejected;

	netLobbyPeer _peers[8];
	int _peer_count;

	void set_status(const char* text);
	void queue(const char* line);
	void flush_out(void);
	int  read_in(void);
	void handle_line(char* line);
};

#endif /* __NET_LOBBY_H__ */
