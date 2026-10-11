#ifndef __LOBBY_SERVER_H__
#define __LOBBY_SERVER_H__

/*
	The lobby server core.

	No console, no blocking, no threads: quant() is one non-blocking step -
	accept new TCP connections, read their lines, read UDP probes, drop the
	dead - and the console front end calls it in a loop. That shape is what
	lets the self test drive the whole thing in one process.

	See net_lobby_protocol.h for the wire format.
*/

#include "net_lobby_protocol.h"

struct lobbyMatchInfo
{
	unsigned short id;
	unsigned short players;
	unsigned short max_players;
	unsigned short world;
	unsigned short laps;
	char name[LOBBY_MATCH_NAME_LEN];
};

// Log sink. The core never prints on its own, so it can be tested silently.
typedef void (*lobbyLogFn)(const char* text,void* user);

class lobbyServer
{
public:
	lobbyServer(void);
	~lobbyServer(void);

	// Binds TCP and UDP on the same port. 0 asks the OS for one. Returns the
	// chosen port, or 0 on failure.
	int start(unsigned short port);
	void stop(void);

	void quant(void);

	unsigned short port(void) const;
	int match_count(void) const;
	// Copy of one match, for the console and the self test. Returns 0 when the
	// index is out of range.
	int match(int index,lobbyMatchInfo* out) const;

	int connection_count(void) const;

	void set_log(lobbyLogFn fn,void* user);

private:
	struct Impl;
	Impl* _impl;
};

#endif /* __LOBBY_SERVER_H__ */
