#ifndef __NET_MATCH_H__
#define __NET_MATCH_H__

/*
	Match state machine: the layer that turns a netSession into something a
	game can actually drive.

	It owns the roster, hands out slots, broadcasts readiness, agrees a
	single start tick, and afterwards does nothing but forward input samples
	for the rest of the match. Nothing here touches rendering, sound or
	physics - the self test runs a whole lobby exchange with two sessions in
	one process and no game loaded.

	The tick used to agree a start moment is derived from clocki(), not from
	the simulation clock: see the note in net_transport.h for why the
	virtual time is the wrong timebase for anything net related.
*/

#include "net_transport.h"
#include "net_input.h"

// netMatch::state
enum netMatchState
{
	NET_MATCH_NONE = 0,
	NET_MATCH_LOBBY,
	NET_MATCH_STARTING,
	NET_MATCH_RUNNING,
	NET_MATCH_FINISHED,
	NET_MATCH_KICKED
};

// netMatchPlayer mirrors netRosterStates through the presence and ready
// fields rather than carrying a fourth redundant state of its own.
struct netMatchPlayer
{
	int slot;
	int peer_id;			// transport peer, -1 on a client
	int is_present;
	int ready;
	unsigned int last_input_tick;	// newest sample seen, host side
	int figure;			// mechos figure, 1..N, 0 until told
	int spells[NET_LOBBY_SPELLS];	// arcane part id per spell slot
	char name[NET_NAME_LEN];
};

/*
	Called for every message the match layer does not consume itself.

	Snapshots and events both travel as their own message types and mean
	nothing to a roster, so the game layer installs this to see them. Without
	it those messages would be read out of the inbox and silently dropped by
	the match layer, which is the state this was written to fix.
*/
typedef void (*netMatchHandler)(int type,const void* data,unsigned int len,void* user);

class netMatch
{
public:
	netMatch(netSession* sess);

	// Pump everything. Called once per frame, after netSession::quant().
	void quant(void);

	void set_handler(netMatchHandler handler,void* user){ _handler = handler; _user = user; }

	// ---- host side ----
	int  open_host(const char* server_name,
		       unsigned char world_id,
		       unsigned char track_id,
		       unsigned char laps);

	// Begin before everybody is ready. Used by the host's own "begin" and
	// by the practice path.
	void force_start(void);

	// A lobby, not a race-in-waiting: the roster does not start the match on
	// its own, the host decides. See the menu lobby.
	void set_manual_start(int on);

	// How long the host waits for the remote acknowledgements before starting
	// anyway. The default is short because both sides are normally loaded
	// already; the menu lobby sets it long enough to cover a world load.
	void set_start_ack_timeout(int ms);

	// The host's game name, shown in the lobby and re-sent with the roster.
	void set_game_name(const char* name);
	const char* game_name(void) const { return _game_name; }

	// The world the next start will carry. open_host fixes an initial one;
	// the lobby may change it before the host presses start.
	void set_world(unsigned char world_id,unsigned char track_id,unsigned char laps);

	// The host's own row (slot 0). Name and mechos, updated live from the
	// lobby instead of only at open_host.
	void set_local_player(const char* name,int figure,const int* spells);

	// ---- client side ----
	int  open_client(const char* player_name);
	void set_ready(int on);

	// Tell the host this client is leaving, so its slot frees at once rather
	// than when the transport times the peer out.
	void leave(void);

	// The mechos this client brings. Kept so a later HELLO can carry it; set
	// before connecting, or call again after to re-tell the host.
	void set_client_mechos(int figure,const int* spells);

	// Lobby only: hold the start acknowledgement until the race has loaded,
	// so the host cannot begin before this machine is ready to run.
	void set_defer_ack(int on);
	void notify_loaded(void);

	// ---- both sides ----
	int  state(void) const { return _state; }
	int  local_slot(void) const { return _local_slot; }
	int  num_players(void) const { return _count; }
	int  num_ready(void) const { return _num_ready; }
	int  max_players(void) const { return _max_players; }
	const netMatchPlayer& player(int index) const { return _players[index]; }
	int  is_host(void) const { return _is_host; }

	// Milliseconds until the race begins. -1 while nobody has scheduled a
	// start, 0 once it has happened.
	int  ms_until_start(void) const;

	unsigned int start_tick(void) const { return _start_tick; }
	unsigned int tick(void) const { return _sess -> tick(); }

	// Host only: newest input sample seen for a slot. Returns 0 if the slot
	// has never sent anything, which is not the same as having sent "no
	// input" - the caller steers for nobody in that case.
	int  get_input(int slot,netInput* out);

	// Client only: pack one sample into a datagram payload. Returns the
	// payload size, or 0 if it will not fit.
	int  build_input(unsigned char* buf,int buf_size,const netInput& in);

	// Client only: send a payload built by build_input().
	int  send_input(unsigned char* buf,int payload);

	// What the host advertised, and what the client was told.
	unsigned char world_id(void) const { return _world_id; }
	unsigned char track_id(void) const { return _track_id; }
	unsigned char laps(void) const { return _laps; }

	// Text of the last thing that went wrong, for the UI.
	const char* status(void) const { return _status; }

	// Client only: the welcome as it arrived, copied on receipt rather than
	// kept. The game layer needs the cosmetic RNG seed out of it, and should
	// not have to intercept a message this layer owns.
	int welcome(netWelcome* out) const;

	netSession* sess(void) const { return _sess; }

private:
	netSession* _sess;

	int _state;
	int _is_host;
	int _local_slot;
	int _count;
	int _num_ready;
	int _max_players;

	unsigned char _world_id;
	unsigned char _track_id;
	unsigned char _laps;
	char _game_name[NET_SERVER_NAME_LEN];

	int _manual_start;		// host: do not start on a full roster
	int _defer_ack;			// client: acknowledge a start only once loaded
	int _loaded;			// client: the race is up
	int _pending_start_ack;		// client: a prepare arrived before _loaded
	int _start_ack_timeout_ms;
	int _client_figure;		// client: mechos sent in its next HELLO
	int _client_spells[NET_LOBBY_SPELLS];
	char _client_name[NET_NAME_LEN];	// client: name sent in its HELLOs
	unsigned int _last_hello_ms;		// client: when the HELLO last went out

	unsigned int _start_tick;
	unsigned int _start_time_ms;
	unsigned int _start_request_ms;	// when the host began waiting for acks

	// Host only: when the roster last held two racers or more. The lobby counts
	// down from there, not from when the host itself sat down.
	unsigned int _lobby_full_ms;

	unsigned int _last_roster_ms;
	int _roster_dirty;

	netInput _input[NET_MAX_PLAYERS];
	int _has_input[NET_MAX_PLAYERS];

	netMatchPlayer _players[NET_MAX_PLAYERS];

	netWelcome _welcome;
	int _has_welcome;

	netMatchHandler _handler;
	void* _user;

	char _status[128];

	int  server_peer(void);
	int  free_slot(void);
	int  slot_by_peer(int peer_id);

	void set_status(const char* text);
	void reset_roster(void);
	void mark_ready(int slot,int on);
	void roster_changed(void);
	void update_tick(unsigned int now);
	void free_lost_slots(void);

	void send_hello(void);

	void send_welcome(int peer_id,int slot);
	void send_roster(int reliable);
	void send_start_game(void);
	void kick(int peer_id,int reason,const char* text);
	void begin_race(unsigned int start_tick);

	// Both return 1 when the message was theirs, so the handler installed
	// with set_handler() only sees what the roster left alone.
	int  handle_host_message(int peer_id,int type,const void* data,unsigned int len,unsigned short seq);
	int  handle_client_message(int type,const void* data,unsigned int len);
};

#endif /* __NET_MATCH_H__ */
