#include "StdAfx.h"

// net_transport.h holds a netSocket by value and does not include its
// definition, so whoever uses it pulls in the socket header first, exactly as
// net_transport.cpp does.
#include "net_socket.h"
#include "net_match.h"

#include <cstring>

/*
	Host and client sides of the match state machine, in one file on purpose:
	the two are halves of one protocol and the interesting part is that they
	stay mirror images of each other.

	The awkward part of arcade multiplayer is agreeing when the race begins.
	A wall clock cannot be shared between two machines, and a start moment
	derived from receipt time drifts by however long the packet was stuck in
	a retransmit. So the decision is made in two steps:

	  1. the host sends a "prepare" START_GAME with no tick yet;
	  2. every client acknowledges, and once they all have, the host picks
	     the tick and re-sends it.

	The tick itself is absolute and shared, so the only thing still governed
	by the local clock is a cosmetic countdown. Guaranteeing that the message
	is in every peer's inbox before the tick is chosen is what removes the
	unbounded part of the drift.
*/

static const int NET_START_ACK_MS	= 500;

netMatch::netMatch(netSession* sess) : _sess(sess)
{
	_state = NET_MATCH_NONE;
	_is_host = 0;
	_local_slot = -1;
	_count = 0;
	_num_ready = 0;
	_max_players = NET_MAX_PLAYERS;

	_world_id = 0;
	_track_id = 0;
	_laps = 0;
	_game_name[0] = 0;

	_manual_start = 0;
	_defer_ack = 0;
	_loaded = 0;
	_pending_start_ack = 0;
	_start_ack_timeout_ms = NET_START_ACK_MS;
	_client_figure = 0;
	for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
		_client_spells[i] = 0;

	_start_tick = 0;
	_start_time_ms = 0;
	_start_request_ms = 0;

	_lobby_full_ms = 0;

	_last_roster_ms = 0;
	_roster_dirty = 0;

	_has_welcome = 0;
	memset(&_welcome,0,sizeof(_welcome));

	_handler = NULL;
	_user = NULL;

	_status[0] = 0;

	for(int i = 0; i < NET_MAX_PLAYERS; i ++){
		_has_input[i] = 0;
		memset(&_input[i],0,sizeof(netInput));
	}

	reset_roster();
}

void netMatch::set_status(const char* text)
{
	if(!text) text = "";

	strncpy(_status,text,sizeof(_status) - 1);
	_status[sizeof(_status) - 1] = 0;
}

void netMatch::reset_roster(void)
{
	for(int i = 0; i < NET_MAX_PLAYERS; i ++){
		_players[i].slot = i;
		_players[i].peer_id = -1;
		_players[i].is_present = 0;
		_players[i].ready = 0;
		_players[i].last_input_tick = 0;
		_players[i].figure = 0;
		for(int j = 0; j < NET_LOBBY_SPELLS; j ++)
			_players[i].spells[j] = 0;
		_players[i].name[0] = 0;
		}

	_count = 0;
	_num_ready = 0;
}

int netMatch::free_slot(void)
{
	// Slot 0 is the host and is never handed out here.
	for(int i = 1; i < _max_players; i ++)
		if(!_players[i].is_present)
			return i;

	return -1;
}

int netMatch::slot_by_peer(int peer_id)
{
	netEndpoint* ep = _sess -> get_peer(peer_id);

	if(!ep || ep -> slot < 0) return -1;

	for(int i = 0; i < _max_players; i ++)
		if(_players[i].slot == ep -> slot && _players[i].is_present)
			return i;

	return -1;
}

// The transport peer the server lives behind. On a client there is exactly
// one remote peer; -1 if the session is not connected yet.
int netMatch::server_peer(void)
{
	for(int i = 0; i < NET_MAX_PEERS; i ++){
		netEndpoint* ep = _sess -> get_peer(i);

		if(ep && !ep -> is_host)
			return ep -> id;
		}

	return -1;
}

void netMatch::mark_ready(int slot,int on)
{
	if(slot < 0 || slot >= _max_players) return;

	if(_players[slot].ready == !!on) return;

	_players[slot].ready = !!on;

	_num_ready = 0;
	for(int i = 0; i < _max_players; i ++)
		if(_players[i].is_present && _players[i].ready)
			_num_ready ++;

	roster_changed();
}

void netMatch::roster_changed(void)
{
	_roster_dirty = 1;
	_last_roster_ms = (unsigned int)clocki();
}

void netMatch::update_tick(unsigned int now)
{
	_sess -> set_tick(now / (unsigned int)NET_TICK_MS);
}

/* ------------------------------- SETUP ---------------------------------- */

int netMatch::open_host(const char* server_name,unsigned char world_id,unsigned char track_id,unsigned char laps)
{
	_is_host = 1;
	_state = NET_MATCH_LOBBY;
	_max_players = NET_MAX_PLAYERS;

	_world_id = world_id;
	_track_id = track_id;
	_laps = laps;

	_manual_start = 0;
	_defer_ack = 0;
	_loaded = 0;
	_pending_start_ack = 0;
	_start_ack_timeout_ms = NET_START_ACK_MS;

	reset_roster();

	_players[NET_SLOT_HOST].is_present = 1;
	_players[NET_SLOT_HOST].ready = 1;	// the host is always ready
	_count ++;

	if(server_name){
		strncpy(_players[NET_SLOT_HOST].name,server_name,NET_NAME_LEN - 1);
		_players[NET_SLOT_HOST].name[NET_NAME_LEN - 1] = 0;

		// A game name of its own is set with set_game_name; until then the
		// host's own name is the best guess at what to show.
		if(!_game_name[0]){
			strncpy(_game_name,server_name,NET_SERVER_NAME_LEN - 1);
			_game_name[NET_SERVER_NAME_LEN - 1] = 0;
			}
		}

	_num_ready = 1;

	_sess -> set_state(NET_STATE_HOSTING);
	set_status("Hosting");

	send_roster(1);

	return 1;
}

int netMatch::open_client(const char* player_name)
{
	netHello hello;
	int peer = server_peer();

	if(peer < 0) return 0;

	_is_host = 0;
	_state = NET_MATCH_LOBBY;

	set_status("Connecting");

	memset(&hello,0,sizeof(hello));
	hello.proto_version = (unsigned short)NET_PROTO_VERSION;
	hello.client_build = (unsigned char)NET_PROTO_VERSION;
	hello.input_rate_hz = (unsigned char)NET_TICK_RATE_HZ;
	hello.figure = (unsigned char)_client_figure;

	for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
		hello.spells[i] = (unsigned char)_client_spells[i];

	if(player_name){
		strncpy((char*)hello.name,player_name,NET_NAME_LEN - 1);
		hello.name[NET_NAME_LEN - 1] = 0;
		}

	// Reliable: a lost HELLO must not cost the host its slot assignment.
	_sess -> send(peer,NET_MSG_HELLO,&hello,sizeof(hello),1);

	reset_roster();

	_loaded = 0;
	_pending_start_ack = 0;

	return 1;
}

/* ------------------------------- CLIENT --------------------------------- */

void netMatch::set_ready(int on)
{
	netReady r;
	int peer = server_peer();

	if(peer >= 0){
		memset(&r,0,sizeof(r));
		r.ready = on ? 1 : 0;

		_sess -> send(peer,NET_MSG_READY,&r,sizeof(r),1);
		}

	mark_ready(_local_slot,on);
}

void netMatch::leave(void)
{
	netLeave l;
	int peer;

	if(_is_host) return;

	peer = server_peer();

	if(peer >= 0){
		memset(&l,0,sizeof(l));
		l.reason = 0;

		// Reliable: the one send is worth a retransmit the transport will not
		// get to make if the socket closes straight after.
		_sess -> send(peer,NET_MSG_LEAVE,&l,sizeof(l),1);
		}
}

void netMatch::set_client_mechos(int figure,const int* spells)
{
	_client_figure = figure;

	for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
		_client_spells[i] = spells ? spells[i] : 0;
}

void netMatch::set_defer_ack(int on)
{
	_defer_ack = !!on;
}

void netMatch::set_manual_start(int on)
{
	_manual_start = !!on;
}

void netMatch::set_start_ack_timeout(int ms)
{
	_start_ack_timeout_ms = ms;
}

void netMatch::set_game_name(const char* name)
{
	if(!name) name = "";

	strncpy(_game_name,name,NET_SERVER_NAME_LEN - 1);
	_game_name[NET_SERVER_NAME_LEN - 1] = 0;

	// Push it now rather than waiting for the roster heartbeat.
	if(_is_host) roster_changed();
}

void netMatch::set_world(unsigned char world_id,unsigned char track_id,unsigned char laps)
{
	_world_id = world_id;
	_track_id = track_id;
	_laps = laps;
}

void netMatch::set_local_player(const char* name,int figure,const int* spells)
{
	netMatchPlayer* p;

	if(_is_host){
		p = &_players[NET_SLOT_HOST];

		if(name){
			strncpy(p -> name,name,NET_NAME_LEN - 1);
			p -> name[NET_NAME_LEN - 1] = 0;
			}

		p -> figure = figure;
		for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
			p -> spells[i] = spells ? spells[i] : 0;

		roster_changed();
		return;
		}

	// A client's row is the host's to change, so this turns into a fresh
	// HELLO carrying the new mechos.
	set_client_mechos(figure,spells);

	{
		int peer = server_peer();

		if(peer >= 0){
			netHello hello;

			memset(&hello,0,sizeof(hello));
			hello.proto_version = (unsigned short)NET_PROTO_VERSION;
			hello.client_build = (unsigned char)NET_PROTO_VERSION;
			hello.input_rate_hz = (unsigned char)NET_TICK_RATE_HZ;
			hello.figure = (unsigned char)_client_figure;

			for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
				hello.spells[i] = (unsigned char)_client_spells[i];

			if(name){
				strncpy((char*)hello.name,name,NET_NAME_LEN - 1);
				hello.name[NET_NAME_LEN - 1] = 0;
				}

			_sess -> send(peer,NET_MSG_HELLO,&hello,sizeof(hello),1);
			}
		}
}

void netMatch::notify_loaded(void)
{
	_loaded = 1;

	// A prepare that arrived while the race was still loading can be
	// acknowledged now: this is the signal the host has been waiting for.
	if(_pending_start_ack && _state == NET_MATCH_STARTING){
		netReady ack;
		int peer = server_peer();

		_pending_start_ack = 0;

		if(peer >= 0){
			memset(&ack,0,sizeof(ack));
			ack.ready = 2;

			_sess -> send(peer,NET_MSG_READY,&ack,sizeof(ack),1);
			}

		mark_ready(_local_slot,1);
		}
}

int netMatch::build_input(unsigned char* buf,int buf_size,const netInput& in)
{
	return net_input_pack(buf,buf_size,&in,1);
}

int netMatch::send_input(unsigned char* buf,int payload)
{
	int peer = server_peer();

	if(payload <= 0 || peer < 0) return 0;

	return _sess -> send(peer,NET_MSG_INPUT,buf,payload,0);
}

int netMatch::get_input(int slot,netInput* out)
{
	if(slot < 0 || slot >= NET_MAX_PLAYERS || !_has_input[slot] || !out)
		return 0;

	*out = _input[slot];

	return 1;
}

int netMatch::ms_until_start(void) const
{
	// Not scheduled: nobody has decided to begin yet.
	if(_state == NET_MATCH_NONE || _state == NET_MATCH_LOBBY)
		return -1;

	if(_state == NET_MATCH_RUNNING)
		return 0;

	return (int)(_start_time_ms - (unsigned int)clocki());
}

int netMatch::welcome(netWelcome* out) const
{
	if(!_has_welcome || !out) return 0;

	*out = _welcome;

	return 1;
}

int netMatch::handle_client_message(int type,const void* data,unsigned int len)
{
	switch(type){
		case NET_MSG_WELCOME:{
			const netWelcome* w = (const netWelcome*)data;

			if(len < sizeof(netWelcome)){
				set_status("Bad welcome");
				return 1;
				}

			_local_slot = w -> your_slot;
			_max_players = w -> max_players ? w -> max_players : NET_MAX_PLAYERS;

			_world_id = w -> world_id;
			_track_id = w -> track_id;
			_laps = w -> laps;

			// w -> client_rnd seeds fxlabClientRND, so effects that are never
			// replicated still match what everyone else sees. That is wired
			// up where the game layer starts, not here.
			_welcome = *w;
			_has_welcome = 1;

			set_status("In lobby");
			return 1;
			}

		case NET_MSG_ROSTER:{
			const netRosterHeader* hdr = (const netRosterHeader*)data;
			const unsigned char* buf = (const unsigned char*)data;
			int off = sizeof(netRosterHeader);

			if(len < (unsigned int)sizeof(netRosterHeader)) return 1;

			strncpy(_game_name,(const char*)hdr -> game_name,NET_SERVER_NAME_LEN - 1);
			_game_name[NET_SERVER_NAME_LEN - 1] = 0;

			reset_roster();

			for(int i = 0; i < hdr -> count && off + (int)sizeof(netRosterEntry) <= (int)len; i ++){
				const netRosterEntry* e = (const netRosterEntry*)(buf + off);
				int slot = e -> slot;

				off += sizeof(netRosterEntry);

				if(slot < 0 || slot >= NET_MAX_PLAYERS) continue;

				_players[slot].slot = slot;
				_players[slot].is_present = (e -> state != NET_ROSTER_FREE);
				_players[slot].ready = (e -> state == NET_ROSTER_READY || e -> state == NET_ROSTER_RACING);
				_players[slot].figure = e -> figure;
				for(int j = 0; j < NET_LOBBY_SPELLS; j ++)
					_players[slot].spells[j] = e -> spells[j];
				strncpy(_players[slot].name,(const char*)e -> name,NET_NAME_LEN - 1);
				_players[slot].name[NET_NAME_LEN - 1] = 0;
				}

			for(int i = 0; i < NET_MAX_PLAYERS; i ++){
				if(_players[i].is_present) _count ++;
				if(_players[i].is_present && _players[i].ready) _num_ready ++;
				}

			// The local slot is only known to us until the host confirms it
			// in the welcome; keep it in step either way.
			if(_local_slot >= 0 && _local_slot < NET_MAX_PLAYERS)
				_players[_local_slot].is_present = 1;

			return 1;
			}

		case NET_MSG_START_GAME:{
			const netStartGame* sg = (const netStartGame*)data;

			if(len < sizeof(netStartGame)) return 1;

			// Step one of the handshake: acknowledge so the host knows the
			// message is safely in our inbox before it commits to a tick.
			if(sg -> flags & NET_START_PREPARE){
				_state = NET_MATCH_STARTING;

				// The prepare carries the final world. The client leaves the
				// menu on it and loads that world, so it has to be read here,
				// not only from the commit that follows - by then the client
				// is already inside the loader.
				_world_id = sg -> world_id;
				_track_id = sg -> track_id;
				_laps = sg -> laps;

				// In the menu lobby the race is loaded after this message, so
				// the acknowledgement has to wait for that: the host would
				// otherwise begin while this machine is still loading a world.
				if(_defer_ack && !_loaded){
					_pending_start_ack = 1;
					}
				else {
					netReady ack;
					int peer = server_peer();

					if(peer >= 0){
						memset(&ack,0,sizeof(ack));
						ack.ready = 2;	// "acknowledged the start"

						_sess -> send(peer,NET_MSG_READY,&ack,sizeof(ack),1);
						}
					}

				set_status("Starting");
				return 1;
				}

			// Step two: the tick is final.
			_start_tick = sg -> start_tick;
			_start_time_ms = (unsigned int)clocki() + NET_START_DELAY_MS;
			_state = NET_MATCH_RUNNING;

			_world_id = sg -> world_id;
			_track_id = sg -> track_id;
			_laps = sg -> laps;

			set_status("Racing");
			return 1;
			}

		case NET_MSG_KICK:{
			const netKick* k = (const netKick*)data;

			_state = NET_MATCH_KICKED;

			if(len >= sizeof(netKick)){
				char text[NET_KICK_TEXT_LEN + 1];

				memcpy(text,k -> text,NET_KICK_TEXT_LEN);
				text[NET_KICK_TEXT_LEN] = 0;
				set_status(text[0] ? text : "Kicked");
				}
			else set_status("Kicked");

			return 1;
			}
		}

	// Anything else is not the match layer's business: snapshots and events
	// belong to the game, and the handler set with set_handler() sees them.
	return 0;
	}

/* -------------------------------- HOST ---------------------------------- */

void netMatch::send_welcome(int peer_id,int slot)
{
	netWelcome w;

	memset(&w,0,sizeof(w));
	w.your_slot = (unsigned char)slot;
	w.num_racers = (unsigned char)_count;
	w.max_players = (unsigned char)_max_players;
	w.tick_rate = (unsigned short)NET_TICK_RATE_HZ;
	w.tick_now = _sess -> tick();
	w.world_id = _world_id;
	w.track_id = _track_id;
	w.laps = _laps;

	// Seed for the cosmetic RNG. The point is not secrecy, it is that every
	// machine draws from the same stream: dust, trail noise and every other
	// effect that is rolled for locally then look identical everywhere
	// without the server ever having to send one of them.
	//
	// Mixed from the clock and the tick, which is enough here and no more.
	// The gameplay RNG is not shared and must not be: it feeds the same solver
	// the physics does, and reproducing it would mean reproducing every frame.
	{
		unsigned int seed = ((unsigned int)clocki() * 2654435761u) ^ (_sess -> tick() * 40503u) ^ ((unsigned int)slot * 2246822519u);

		w.client_rnd[0] = (unsigned char)(seed & 0xff);
		w.client_rnd[1] = (unsigned char)((seed >> 8) & 0xff);
		w.client_rnd[2] = (unsigned char)((seed >> 16) & 0xff);
		w.client_rnd[3] = (unsigned char)((seed >> 24) & 0xff);
		}

	strncpy((char*)w.server_name,_players[NET_SLOT_HOST].name,NET_NAME_LEN - 1);
	w.server_name[NET_NAME_LEN - 1] = 0;

	_sess -> send(peer_id,NET_MSG_WELCOME,&w,sizeof(w),1);
}

void netMatch::send_roster(int reliable)
{
	unsigned char buf[NET_MAX_PAYLOAD];
	netRosterHeader* hdr = (netRosterHeader*)buf;
	int off = sizeof(netRosterHeader);

	hdr -> count = (unsigned char)_count;
	hdr -> flags = 0;
	hdr -> server_tick = _sess -> tick();

	memset(hdr -> game_name,0,NET_SERVER_NAME_LEN);
	strncpy((char*)hdr -> game_name,_game_name,NET_SERVER_NAME_LEN - 1);

	for(int i = 0; i < NET_MAX_PLAYERS; i ++)
		hdr -> ping[i] = 0;

	for(int i = 0; i < NET_MAX_PLAYERS && off + (int)sizeof(netRosterEntry) <= (int)sizeof(buf); i ++){
		netRosterEntry* e = (netRosterEntry*)(buf + off);
		const netMatchPlayer& p = _players[i];

		if(!p.is_present) continue;

		e -> slot = (unsigned char)p.slot;
		e -> state = (unsigned char)(p.ready ? NET_ROSTER_READY : NET_ROSTER_CONNECTING);
		e -> racer_id = (unsigned char)i;
		e -> kind = 0;
		e -> figure = (unsigned char)(p.figure > 0 ? p.figure : 0);
		for(int j = 0; j < NET_LOBBY_SPELLS; j ++)
			e -> spells[j] = (unsigned char)(p.spells[j] > 0 ? p.spells[j] : 0);
		memset(e -> name,0,NET_NAME_LEN);
		strncpy((char*)e -> name,p.name,NET_NAME_LEN - 1);

		off += sizeof(netRosterEntry);
		}

	_sess -> broadcast(NET_MSG_ROSTER,buf,off,reliable);
}

void netMatch::send_start_game(void)
{
	unsigned char buf[NET_MAX_PAYLOAD];
	netStartGame* sg = (netStartGame*)buf;

	memset(sg,0,sizeof(netStartGame));
	sg -> rng_seed = (unsigned int)clocki();
	sg -> world_id = _world_id;
	sg -> track_id = _track_id;
	sg -> laps = _laps;
	sg -> bots_mask = 0;

	for(int i = 0; i < NET_MAX_PLAYERS; i ++)
		sg -> racer_kind[i] = 0;

	if(_start_tick)
		sg -> start_tick = _start_tick;
	else{
		// Still deciding: announce without a tick and collect acknowledgements.
		sg -> flags = NET_START_PREPARE;
		}

	_sess -> broadcast(NET_MSG_START_GAME,sg,sizeof(netStartGame),1);
}

void netMatch::kick(int peer_id,int reason,const char* text)
{
	netKick k;

	memset(&k,0,sizeof(k));
	k.reason = (unsigned char)reason;

	if(text){
		strncpy((char*)k.text,text,NET_KICK_TEXT_LEN - 1);
		k.text[NET_KICK_TEXT_LEN - 1] = 0;
		}

	_sess -> send(peer_id,NET_MSG_KICK,&k,sizeof(k),1);
}

void netMatch::begin_race(unsigned int start_tick)
{
	_start_tick = start_tick;
	_start_time_ms = (unsigned int)clocki() + NET_START_DELAY_MS;
	_state = NET_MATCH_RUNNING;

	for(int i = 0; i < NET_MAX_PLAYERS; i ++)
		if(_players[i].is_present)
			_players[i].ready = 1;

	send_start_game();
	send_roster(1);

	set_status("Racing");
}

void netMatch::force_start(void)
{
	if(_state != NET_MATCH_LOBBY) return;

	_state = NET_MATCH_STARTING;
	_start_tick = 0;
	_start_request_ms = (unsigned int)clocki();
	_roster_dirty = 0;

	send_start_game();

	set_status("Starting");
}

void netMatch::free_lost_slots(void)
{
	for(int i = 1; i < _max_players; i ++){
		if(!_players[i].is_present) continue;

		// A peer the transport has given up on leaves a slot occupied for
		// the rest of the match, which is how a full race ends up with room
		// for nobody.
		if(_players[i].peer_id >= 0 && !_sess -> get_peer(_players[i].peer_id)){
			_players[i].is_present = 0;
			_players[i].ready = 0;
			_players[i].name[0] = 0;
			_has_input[i] = 0;
			_count --;

			roster_changed();
			}
		}
	}

int netMatch::handle_host_message(int peer_id,int type,const void* data,unsigned int len,unsigned short seq)
{
	(void)seq;

	switch(type){
		case NET_MSG_HELLO:{
			const netHello* h = (const netHello*)data;
			netEndpoint* ep = _sess -> get_peer(peer_id);

			if(!ep) return 1;

			if(!h || len < sizeof(netHello)){
				kick(peer_id,NET_KICK_BAD_VERSION,"Bad hello");
				return 1;
				}

			if(h -> proto_version != NET_PROTO_VERSION){
				kick(peer_id,NET_KICK_BAD_VERSION,"Wrong version");
				return 1;
				}

			if(ep -> slot < 0){
				int slot = free_slot();

				if(slot < 0){
					kick(peer_id,NET_KICK_ROOM_FULL,"Race is full");
					return 1;
					}

				ep -> slot = slot;
				_players[slot].is_present = 1;
				_players[slot].peer_id = peer_id;
				_players[slot].ready = 0;
				_players[slot].last_input_tick = 0;
				_players[slot].name[0] = 0;
				_players[slot].figure = 0;
				for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
					_players[slot].spells[i] = 0;

				if(h -> name[0])
					strncpy(_players[slot].name,(const char*)h -> name,NET_NAME_LEN - 1);

				_players[slot].figure = h -> figure;
				for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
					_players[slot].spells[i] = h -> spells[i];

				_count ++;
				}
			else {
				// Already seated: this is a retransmit, or a reconnect after a
				// lost welcome. Resend rather than hand out a second slot. A
				// fresh HELLO is also how a seated client updates its name and
				// mechos from the lobby.
				int slot = ep -> slot;

				if(slot >= 0 && slot < _max_players){
					_players[slot].is_present = 1;
					_players[slot].peer_id = peer_id;
					_players[slot].figure = h -> figure;
					for(int i = 0; i < NET_LOBBY_SPELLS; i ++)
						_players[slot].spells[i] = h -> spells[i];

					if(h -> name[0])
						strncpy(_players[slot].name,(const char*)h -> name,NET_NAME_LEN - 1);
					}
				}

			_sess -> set_state(NET_STATE_LOBBY);
			send_welcome(peer_id,ep -> slot);
			send_roster(1);
			return 1;
			}

		case NET_MSG_READY:{
			const netReady* r = (const netReady*)data;
			int slot = slot_by_peer(peer_id);

			if(!r || len < sizeof(netReady) || slot < 0) return 1;

			// ready == 2 is the acknowledgement of a prepare START_GAME.
			if(r -> ready == 2){
				mark_ready(slot,1);
				return 1;
				}

			mark_ready(slot,r -> ready ? 1 : 0);
			send_roster(1);
			return 1;
			}

		case NET_MSG_INPUT:{
			netInput samples[NET_INPUT_BATCH];
			int slot = slot_by_peer(peer_id);
			int got = net_input_unpack(samples,NET_INPUT_BATCH,(const unsigned char*)data,len);

			if(slot < 0 || got <= 0) return 1;

			for(int i = 0; i < got; i ++){
				// Stale samples are dropped rather than replayed: a buffered
				// packet resending ticks the host already processed is worth
				// less than one tick of lag.
				if(_has_input[slot] && (int)(samples[i].tick - _input[slot].tick) <= 0)
					continue;

				_input[slot] = samples[i];
				_has_input[slot] = 1;
				_players[slot].last_input_tick = samples[i].tick;
				}
			return 1;
			}

		case NET_MSG_LEAVE:{
			int slot = slot_by_peer(peer_id);
			netEndpoint* ep = _sess -> get_peer(peer_id);

			if(slot >= 0 && ep){
				_has_input[slot] = 0;
				_players[slot].is_present = 0;
				_players[slot].peer_id = -1;
				_players[slot].ready = 0;
				_players[slot].name[0] = 0;
				_count --;

				ep -> slot = -1;

				send_roster(1);
				}
			return 1;
			}
		}

	// Not a match message, so the game may want it.
	return 0;
	}

void netMatch::quant(void)
{
	unsigned int now = (unsigned int)clocki();
	int peer_id, type;
	unsigned int len;
	unsigned short seq;
	unsigned char buf[NET_MAX_PAYLOAD];

	if(!_sess) return;

	update_tick(now);

	// Nothing to do before the session is usable and the match is opened.
	if(_sess -> state() == NET_STATE_IDLE || _state == NET_MATCH_NONE)
		return;

	/* ---------------------------- messages --------------------------- */

	while(_sess -> receive(&peer_id,&type,buf,&len,&seq)){
		int handled = _is_host
			? handle_host_message(peer_id,type,buf,len,seq)
			: handle_client_message(type,buf,len);

		// Whatever the roster does not own is the game's: snapshots and
		// events arrive as their own message types and mean nothing here.
		if(!handled && _handler)
			_handler(type,buf,len,_user);
		}

	/* ------------------------------- host ---------------------------- */

	if(_is_host){
		free_lost_slots();

		// Roster on change, plus a heartbeat: a peer that misses one still
		// catches the next, and late arrivals are never invisible.
		if(_roster_dirty || (int)(now - _last_roster_ms) >= NET_ROSTER_MS){
			_roster_dirty = 0;
			_last_roster_ms = now;
			send_roster(0);
			}

		// The lobby has nobody to press "start" until the menu phase lands, so
		// the roster alone decides: two racers seated is a race. The clock runs
		// from when the roster last changed, so joining at the last second
		// restarts the countdown instead of starting on top of somebody.
		// A menu lobby turns this off: there the host presses start.
		if(_state == NET_MATCH_LOBBY && !_manual_start){
			if(_count >= 2){
				if(!_lobby_full_ms) _lobby_full_ms = now;
				}
			else
				_lobby_full_ms = 0;

			if(_lobby_full_ms && (int)(now - _lobby_full_ms) >= NET_LOBBY_START_MS)
				force_start();
			}

		if(_state == NET_MATCH_STARTING){
			// Either every remote slot has acknowledged or the wait ran out:
			// a single silent peer must not hold the race hostage. The timeout
			// is short by default and long in the menu lobby, where the
			// acknowledgement only comes once a peer has a world loaded.
			int remote = 0, acked = 0;

			for(int i = 1; i < _max_players; i ++)
				if(_players[i].is_present){
					remote ++;
					if(_players[i].ready) acked ++;
					}

			if(!remote || acked == remote ||
			   (int)(now - _start_request_ms) >= _start_ack_timeout_ms){
				unsigned int delay_ticks = (unsigned int)(NET_START_DELAY_MS / NET_TICK_MS);

				begin_race(_sess -> tick() + delay_ticks);
				}
			}

		return;
		}

	/* ------------------------------ client --------------------------- */

	// Nothing to run: the game layer reads ms_until_start() to know when to
	// call mchSetTimeMode(MCH_TIME_RUNNING), and nothing else here depends
	// on the local clock once the tick is agreed.
	}
