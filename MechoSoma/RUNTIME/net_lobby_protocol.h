#ifndef __NET_LOBBY_PROTOCOL_H__
#define __NET_LOBBY_PROTOCOL_H__

/*
	The lobby server's wire protocol, shared by the server and the game's
	lobby client. Deliberately dependency free, like net_protocol_types.h:
	the server is a separate console target that pulls this header in without
	pulling the game in with it.

	Two channels, deliberately:

	  TCP   the control channel. A game connects, says hello, then either
	        registers a match (the host) or lists and joins one (a client).
	        Line based and human readable - the server is a console program
	        and an operator should be able to read its traffic - with the one
	        free-form field (the match name) last on the line so it may carry
	        spaces.

	  UDP   one probe from the game's own socket to the server's UDP port. The
	        server needs the public UDP endpoint of the exact socket the game
	        will race on, and TCP cannot reveal it. The probe carries the token
	        the server handed out over TCP, so the observed endpoint is paired
	        with the right connection.

	The TCP port and the UDP port are the same number and a server listens on
	both.

	Client -> server lines:

	  HELLO <version>
	  REGISTER <world> <laps> <name...>
	  UNREGISTER
	  LIST
	  JOIN <match_id>
	  BYE

	Server -> client lines:

	  HELLO <version> <token>
	  REGISTERED <match_id>
	  LIST <count>                       followed by <count> MATCH lines and END
	  MATCH <id> <players> <max> <world> <laps> <name...>
	  END
	  PEER <ip> <udp_port>               the other side's public UDP endpoint
	  ERROR <text...>

	UDP probe, one datagram:

	  MRLBPROBE <token>\n
*/

const int LOBBY_PROTO_VERSION	= 1;

// One number for both channels. A well known default rather than a constant
// the game must carry: the lobby address is typed into the menu.
const int LOBBY_DEFAULT_PORT	= 8510;

// The token the server pairs a TCP connection with its UDP probe. Handed out
// in HELLO, small enough to sit in a line, large enough not to collide.
const int LOBBY_TOKEN_LEN	= 12;

const int LOBBY_MAX_LINE	= 512;
const int LOBBY_MAX_MATCHES	= 64;
const int LOBBY_MATCH_NAME_LEN	= 24;

// A connection that has sent nothing for this long is gone. The game probes
// and re-lists while its menu is up, so a live connection never goes quiet.
const int LOBBY_TIMEOUT_MS	= 15000;

// The UDP probe datagram, NUL terminated as well as newline terminated so it
// is a valid C string if the server ever wants to print it.
const char LOBBY_PROBE_PREFIX[]	= "MRLBPROBE ";

#endif /* __NET_LOBBY_PROTOCOL_H__ */
