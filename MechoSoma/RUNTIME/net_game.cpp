#include "StdAfx.h"

// net_match.h -> net_transport.h holds a netSocket by value without including
// its definition, so the socket header comes first. See the same note in
// net_match.cpp.
#include "net_socket.h"
#include "net_match.h"
#include "net_snapshot.h"

#include "mechosoma.h"
#include "race.h"
#include "Mechos.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

/*
	Net races write a running log to net.log, next to the binary.

	A LAN session fails in ways nobody can see from the outside - a peer that
	never connects, a start tick nobody agrees on - and the only place that
	knows is this layer. One file, written while racing, is the difference
	between "it does not work" and something diagnosable.
*/

static FILE* net_log_file = NULL;

static void net_log(const char* fmt,...)
{
	if(!net_log_file) return;

	va_list arg;

	va_start(arg,fmt);
	vfprintf(net_log_file,fmt,arg);
	va_end(arg);

	fprintf(net_log_file,"\n");
	fflush(net_log_file);
}

static void net_log_open(void)
{
	// Named after the role, not a fixed name: a LAN test runs two copies of
	// the game on one machine, and a shared filename would have them
	// overwrite each other.
	const char* name = mchNetHost ? "net-host.log" : "net-client.log";

	net_log_file = fopen(name,"w");
}

static void net_log_close(void)
{
	if(!net_log_file) return;

	fclose(net_log_file);
	net_log_file = NULL;
}

// mchNumLaps, mchNumRacers and mchNumRacersDef live in the setup/race headers
// rather than in mechosoma.h, and are declared extern in the sources that use
// them.
extern int mchNumLaps;
extern int mchNumRacers;
extern int mchNumRacersDef;

/*
	Glue between the match layer and the game. Everything above this file is
	protocol and below it is a running race; this is the seam.

	Two things live here that do not belong anywhere else:

	  - applying input the host receives to the racer of the right slot;
	  - deciding when the race actually begins, which is the host's tick and
	    not the local start counter.

	The rest of the engine is left alone. Nothing here changes behaviour when
	mchNetGame is zero, which is every other mode the game has.
*/

int mchNetGame = 0;
int mchNetHost = 0;
char mchNetJoinAddr[128] = "127.0.0.1";
int mchNetPort = NET_DEFAULT_PORT;

const char* mchNetStatusText = "";

static netMatch* net_match_S = NULL;
static netSession* net_session_S = NULL;

static unsigned int net_last_connect_ms = 0;
static unsigned int net_last_input_ms = 0;
static unsigned int net_last_snap_ms = 0;
static int net_client_ready = 0;
static int net_last_player_count = -1;
static int net_camera_set = 0;
static int net_start_logged = 0;

static const int NET_CONNECT_RETRY_MS = 500;

// The host sends one snapshot per tick. Net ticks run at NET_TICK_MS, so this
// is 1000 / NET_TICK_MS and nothing else: any other rate would either flood
// the socket or make every vehicle stutter.
static const int NET_SNAPSHOT_HZ = NET_TICK_RATE_HZ;
static const int NET_SNAPSHOT_PERIOD_MS = 1000 / NET_SNAPSHOT_HZ;

/*
	Host side: build a snapshot out of the local racers and send it.

	Only the host knows where anything is. Every other machine gets a
	position and a heading and draws from that, which is the whole point of
	the server-authoritative design.
*/
static void net_send_snapshot(void)
{
	netRacerState racers[NET_MAX_PLAYERS];
	unsigned char payload[NET_MAX_PAYLOAD];
	unsigned int now = (unsigned int)clocki();
	mchRacer* rp;
	int count = 0;
	int bytes;

	if((int)(now - net_last_snap_ms) < NET_SNAPSHOT_PERIOD_MS) return;
	net_last_snap_ms = now;

	rp = mch_raceD -> racerLst -> first();

	while(rp && count < NET_MAX_PLAYERS){
		netRacerState* s = &racers[count];
		mchMechosRacer* mr = dynamic_cast<mchMechosRacer*>(rp);

		memset(s,0,sizeof(netRacerState));

		if(mr){
			const Vect3f& pos = mr -> R();

			s -> pos_x = net_q_world(pos.x);
			s -> pos_y = net_q_world(pos.y);
			s -> pos_z = net_q_height(pos.z);
			s -> yaw = net_q_yaw(mr -> psi());

			// Vglobal() rather than V: the member is protected, and a
			// snapshot builder has no business reaching into it.
			s -> vel_x = net_q_speed(mr -> Vglobal().x);
			s -> vel_y = net_q_speed(mr -> Vglobal().y);
			s -> vel_z = (signed char)mr -> Vglobal().z;

			s -> speed = (unsigned short)mr -> Vglobal().norm();
			}

		s -> slot = (unsigned char)count;
		s -> racer_id = (unsigned char)rp -> ID;
		s -> kind = 0;
		s -> place = 0;

		count ++;
		rp = rp -> next;
		}

	if(!count) return;

	bytes = net_snapshot_pack(payload,sizeof(payload),net_match_S -> tick(),racers,count);
	if(bytes > 0)
		net_match_S -> sess() -> broadcast(NET_MSG_SNAPSHOT,payload,(unsigned int)bytes,0);
}

/*
	Client side: put the host's view of a racer onto the local copy.

	The local racer is skipped. It is driven by local input and simulated
	locally, which is what keeps it responsive; overwriting it with a value
	that has been across the network would add a round trip of lag to the
	player's own car.

	Remote racers are pose-driven from here on: they have no AI and no input,
	so the physics runs on from wherever the last snapshot put them.
*/
static void net_apply_snapshot(const void* data,unsigned int len)
{
	netSnapshotHeader hdr;
	netRacerState racers[NET_MAX_PLAYERS];
	int local = net_match_S -> local_slot();
	int count = net_snapshot_unpack(&hdr,racers,NET_MAX_PLAYERS,(const unsigned char*)data,len);

	for(int i = 0; i < count; i ++){
		mchRacer* rp = mch_raceD -> racerLst -> search(racers[i].slot);
		mchMechosRacer* mr;

		if(!rp || racers[i].slot == local) continue;

		mr = dynamic_cast<mchMechosRacer*>(rp);
		if(!mr) continue;

		mr -> setPosition(Vect3f(net_uq_world(racers[i].pos_x),
					 net_uq_world(racers[i].pos_y),
					 net_uq_height(racers[i].pos_z)),
				  net_uq_yaw(racers[i].yaw));

		mr -> setGlobalVelocity(Vect3f(net_uq_speed(racers[i].vel_x),
					       net_uq_speed(racers[i].vel_y),
					       (float)racers[i].vel_z));
		}
}

static void net_game_handler(int type,const void* data,unsigned int len,void* user)
{
	(void)user;

	if(type != NET_MSG_SNAPSHOT) return;

	net_apply_snapshot(data,len);
}

void mchNetStart(void)
{
	if(net_session_S) return;

	net_session_S = new netSession;
	net_match_S = new netMatch(net_session_S);
	net_S = net_session_S;

	// Snapshots mean nothing to the roster, so the match layer must hand them
	// on rather than read them and throw them away.
	net_match_S -> set_handler(net_game_handler,NULL);

	// Zero asks for an ephemeral port, which is what a host wants: the
	// client is told where to go, not the other way round.
	if(!net_session_S -> open(mchNetHost ? mchNetPort : 0)){
		mchNetStatusText = "Failed to open socket";
		net_log("open: FAILED");
		return;
		}

	net_log_open();
	net_log("net race start, %s",mchNetHost ? "host" : "client");

	if(mchNetHost){
		net_session_S -> host();
		net_match_S -> open_host("host",
					 (unsigned char)mchCurrentWorld,
					 (unsigned char)mchCurrentTrack,
					 (unsigned char)mchNumLaps);

		net_log("host on port %d, racers %d",mchNetPort,mchNumRacers);
		}
	else {
		// The first attempt happens in mchNetQuant(), so a client that starts
		// before the host comes up retries instead of failing outright.
		net_last_connect_ms = 0;

		net_log("client joining %s:%d",mchNetJoinAddr,mchNetPort);
		}

	mchNetStatusText = mchNetHost ? "Waiting for racers" : "Connecting";
}

void mchNetFinit(void)
{
	if(net_session_S) net_session_S -> close();

	delete net_match_S;
	delete net_session_S;

	net_match_S = NULL;
	net_session_S = NULL;
	net_S = NULL;

	net_client_ready = 0;
	net_camera_set = 0;
	net_last_player_count = -1;
	net_start_logged = 0;

	mchNetStatusText = "";

	net_log("net race end");
	net_log_close();
}

int mchNetIsActive(void)
{
	return (net_session_S != NULL);
}

int mchNetNumPlayers(void)
{
	return net_match_S ? net_match_S -> num_players() : 0;
}

int mchNetNumReady(void)
{
	return net_match_S ? net_match_S -> num_ready() : 0;
}

int mchNetState(void)
{
	return net_match_S ? net_match_S -> state() : NET_MATCH_NONE;
}

int mchNetLocalSlot(void)
{
	return net_match_S ? net_match_S -> local_slot() : -1;
}

const char* mchNetPlayerName(int slot)
{
	if(!net_match_S || slot < 0 || slot >= NET_MAX_PLAYERS) return "";
	if(!net_match_S -> player(slot).is_present) return "";

	return net_match_S -> player(slot).name;
}

const char* mchNetStatus(void)
{
	return mchNetStatusText;
}

/*
	Host side: hand every seated client's steering to the racer that carries
	their slot.

	Slot 0 is the host's own racer and stays keyboard driven. Slots are the
	order the race built its racers in, and the roster hands them out in the
	same order, which is what makes the two line up.
*/
static void net_apply_host_inputs(void)
{
	mchRacer* rp;
	int slot = 0;

	rp = mch_raceD -> racerLst -> first();

	while(rp){
		netInput in;

		if(slot != NET_SLOT_HOST && net_match_S -> get_input(slot,&in))
			rp -> set_net_input(in);

		rp = rp -> next;
		slot ++;
		}
}

/*
	Host side: racers nobody is driving have to be driven by something, or
	the field is a parking lot. A seated client takes their slot back.
*/
static void net_update_ai(void)
{
	mchRacer* rp;
	int slot = 0;

	rp = mch_raceD -> racerLst -> first();

	while(rp){
		if(slot != NET_SLOT_HOST){
			int taken = net_match_S -> player(slot).is_present;

			// AI_Toggle(,0) also removes the seeds the AI laid down. Without
			// that the vehicle would still have control points and
			// point_control() would win over the network input.
			mch_raceD -> AI_Toggle(rp,taken ? 0 : 1);

			if(taken)
				rp -> set_net_control(1);
			else
				rp -> set_net_control(0);
			}

		rp = rp -> next;
		slot ++;
		}
}

/*
	Client side: read the local controls and send them, once per tick.

	The sample is built from the raw controls rather than from what the
	vehicle actually did, so the host re-derives the same smoothed traction
	and rudder from the same starting state.
*/
static void net_send_client_input(void)
{
	unsigned char payload[NET_MAX_PAYLOAD];
	netInput in;
	unsigned int now = (unsigned int)clocki();
	mchRacer* rp;
	mchMechosRacer* mechos;
	int bytes;

	if((int)(now - net_last_input_ms) < NET_TICK_MS) return;
	net_last_input_ms = now;

	// mchRacer is only a PointControlledObject; reading controls is a Mechos
	// capability, so the cast is through the concrete racer type.
	rp = mch_raceD -> racerLst -> search(net_match_S -> local_slot());
	if(!rp) return;

	mechos = dynamic_cast<mchMechosRacer*>(rp);
	if(!mechos) return;

	memset(&in,0,sizeof(in));
	mechos -> read_input(in);
	in.tick = net_match_S -> tick();

	bytes = net_match_S -> build_input(payload,sizeof(payload),in);
	if(bytes > 0)
		net_match_S -> send_input(payload,bytes);
}

void mchNetQuant(void)
{
	unsigned int now;
	int players;

	if(!net_session_S || !net_match_S || !mch_raceD) return;

	now = (unsigned int)clocki();

	/* ------------------------- bring the session up ---------------------- */

	if(!mchNetHost && !net_client_ready){
		if((int)(now - net_last_connect_ms) >= NET_CONNECT_RETRY_MS){
			net_last_connect_ms = now;

			if(net_session_S -> connect(mchNetJoinAddr,mchNetPort))
				net_match_S -> open_client("player");
			}
		}

	/* ------------------------------- pump ------------------------------- */

	net_session_S -> quant();
	net_match_S -> quant();

	/* --------------------------- drive the race ------------------------- */

	players = net_match_S -> num_players();

	if(players != net_last_player_count){
		// The roster changed: somebody joined, left, or was refused. AI and
		// camera both key off who is actually driving, so both are redone.
		net_last_player_count = players;
		net_camera_set = 0;

		if(mchNetHost)
			net_update_ai();

		net_log("%s: roster now %d player(s), state %d",
			mchNetHost ? "host" : "client",players,net_match_S -> state());
		}

	if(mchNetHost){
		net_apply_host_inputs();

		// Snapshots only exist on the host: it is the only machine that knows
		// where anything is.
		if(net_match_S -> state() == NET_MATCH_RUNNING)
			net_send_snapshot();

		if(net_match_S -> state() != NET_MATCH_LOBBY)
			mchNetStatusText = "Racing";
		}

	if(!mchNetHost && net_match_S -> local_slot() >= 0 && !net_client_ready){
		mchRacer* rp = mch_raceD -> racerLst -> search(net_match_S -> local_slot());
		int slot = 0;

		net_client_ready = 1;

		net_log("client: assigned slot %d",net_match_S -> local_slot());

		// The client drives its own slot and nothing else, so the AI has to
		// let go of just that one.
		if(rp){
			mch_raceD -> AI_Toggle(rp,0);
			mch_raceD -> setActiveRacer(rp);
			}

		// Every other slot is driven by the host's snapshots. Leaving the AI
		// on them would have two simulators steering the same car and the
		// pose would be overwritten every frame while the forces kept
		// accumulating: jitter, and the wrong car on screen.
		rp = mch_raceD -> racerLst -> first();

		while(rp){
			if(slot != net_match_S -> local_slot()){
				mch_raceD -> AI_Toggle(rp,0);
				rp -> set_net_control(1);
				}

			rp = rp -> next;
			slot ++;
			}
		}

	if(!mchNetHost && net_client_ready)
		net_send_client_input();

	/* ---------------------------- start tick ---------------------------- */

	// The start moment comes from the host: mchSetTimeMode is called when the
	// agreed tick says so, and never from the local start counter.
	if(net_match_S -> state() == NET_MATCH_RUNNING &&
	   net_match_S -> ms_until_start() <= 0 &&
	   mchTimeMode != MCH_TIME_RUNNING){
		net_log("start tick %u reached, time running",net_match_S -> start_tick());

		mchSetTimeMode(MCH_TIME_RUNNING);
		mchNetStatusText = "Racing";
		}

	if(net_match_S -> state() == NET_MATCH_STARTING && !net_start_logged){
		net_start_logged = 1;
		net_log("start handshake, tick to be %u",net_match_S -> start_tick());
		}
	}
