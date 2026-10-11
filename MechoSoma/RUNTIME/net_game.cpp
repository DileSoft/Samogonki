#include "StdAfx.h"

// net_match.h -> net_transport.h holds a netSocket by value without including
// its definition, so the socket header comes first. See the same note in
// net_match.cpp.
#include "net_socket.h"
#include "net_match.h"
#include "net_snapshot.h"
#include "net_fx.h"
#include "net_spawn.h"
#include "net_event.h"

#include "mechosoma.h"
#include "race.h"
#include "Mechos.h"
#include "fxlabGeneral.h"
#include "fxlabInterface.h"

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

// Same sink, for the other network translation units (net_fx.cpp), which have
// no logger of their own. Kept as one function so both roles land in one file.
void mchNetWriteLog(const char* fmt,...)
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
/*
	-netrace: start the race without the menus.

	A net race normally begins on the multiplayer screen, which a machine with
	no human attached cannot reach, and the menu path in between is exactly
	what nobody has run before. An automated test needs the race and nothing
	else, so this flag walks the same code the F2 keypress does - ret == 2,
	MCH_SINGLE_GAME, onto the loading screen - with the menu Quant skipped.
*/
int mchNetRaceAuto = 0;
/*
	-netdrive: the client drives itself.

	A keyboard is not available to a test, and the input path is exactly what
	needs exercising: the client sends, the host applies. This makes the client
	send a fixed steering sample every tick instead of reading the keyboard.
	Test scaffolding, no effect on a normal race.
*/
int mchNetAutoDrive = 0;
char mchNetJoinAddr[128] = "127.0.0.1";
int mchNetPort = NET_DEFAULT_PORT;

/*
	The world a net race runs, and the only one until a lobby can pick one.

	The menus are what normally choose the world, and a net race has no screen
	that chooses it: ShowImageRTO::Init leaves mchCurrentWorld at -1, which is
	not a world anything can index - not the renderer, not the track loader,
	not fxlab's WorldData. Both sides have to agree on the number, so it is a
	constant here rather than something either side decides.
*/
const int NET_RACE_WORLD = 1;
const int NET_RACE_TRACK = 0;

extern int mchCurrentWorld;
extern int mchCurrentTrack;

void mchNetArmRaceWorld(void)
{
	mchCurrentWorld = NET_RACE_WORLD;
	mchCurrentTrack = NET_RACE_TRACK;
}

const char* mchNetStatusText = "";

static netMatch* net_match_S = NULL;
static netSession* net_session_S = NULL;

static unsigned int net_last_connect_ms = 0;
static unsigned int net_last_input_ms = 0;
static unsigned int net_last_snap_ms = 0;
static unsigned int net_last_fx_log_ms = 0;
static int net_client_ready = 0;
static int net_last_player_count = -1;
static int net_camera_set = 0;
static int net_start_logged = 0;

// Last sampled position per slot, so the periodic log can say whether the
// field is actually driving or just sitting there.
static Vect3f net_motion_last[NET_MAX_PLAYERS];
static int net_motion_have = 0;

// Client side: the host's authoritative pose for the local car, and when it was
// last checked against the local one. See net_reconcile_local.
static Vect3f net_local_auth;
static float net_local_auth_yaw = 0;
static int net_local_auth_valid = 0;
static unsigned int net_reconcile_ms = 0;

// Input counters. input_sent is the client speaking, input_recv the host
// hearing: two numbers that tell a silent client from a deaf host.
static unsigned int net_input_sent_count = 0;
static unsigned int net_input_recv_count = 0;

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

			// The destroy-assemble sequence is host simulation: a client does
			// not take damage, so without this it never leaves NORMAL_MECHOS
			// and a wrecked car just keeps sliding around unbroken.
			s -> mech_state = (unsigned char)mr -> state();

			// Vglobal() rather than V: the member is protected, and a
			// snapshot builder has no business reaching into it.
			s -> vel_x = net_q_speed(mr -> Vglobal().x);
			s -> vel_y = net_q_speed(mr -> Vglobal().y);
			s -> vel_z = (signed char)mr -> Vglobal().z;

			s -> speed = (unsigned short)mr -> Vglobal().norm();

			// Arcane mana per part. The host is the only machine whose arcane
			// drains, so this is what both a client bar and a client's own
			// cast check have to read.
			for(int i = 0; i < NET_ACTIVE_PARTS; i ++){
				cMesh* part = mr -> part_by_index(i);

				if(!part) continue;

				s -> part_power[i] = net_q_mana(part -> arcaneData.Power,
								part -> arcaneData.MaxPower);
				s -> part_status[i] = (unsigned char)(part -> arcaneData.Status ? 1 : 0);
				}
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

/* --------------------------- interpolation (A) --------------------------- */

/*
	Remote racers arrive at twenty poses a second. Applying each one on
	arrival means twenty corrections a second, and at fifty milliseconds they
	are visible: the local physics carries the car between two of them and the
	next batch snaps it back.

	This buffer keeps the last few snapshots and draws the car at a fixed
	delay behind the newest one. Two ticks is the whole point of the delay: it
	is long enough that the next snapshot has landed by the time the draw
	reaches this one, so a pose is always an interpolation between two known
	points and never an extrapolation that a late packet would have to be
	yanked back from.

	The host tick is the timeline, not the local clock. netMatch::update_tick
	advances it once per NET_TICK_MS by construction, so it is uniform, while
	arrivals are not. The newest tick plus the time since it landed is what
	places "now" on the host's line.
*/
const int NET_INTERP_SAMPLES		= 3;
const int NET_INTERP_DELAY_TICKS	= 2;

struct netInterpSample
{
	unsigned int tick;
	Vect3f pos;
	float yaw;
	Vect3f vel;
};

struct netInterpSlot
{
	int count;
	unsigned int last_arrival_ms;
	netInterpSample sample[NET_INTERP_SAMPLES];	// oldest first
};

static netInterpSlot net_interp[NET_MAX_PLAYERS];

static void net_interp_reset(void)
{
	memset(net_interp,0,sizeof(net_interp));
}

static void net_interp_remember(int slot,unsigned int tick,const Vect3f& pos,float yaw,const Vect3f& vel)
{
	netInterpSlot* b;

	if(slot < 0 || slot >= NET_MAX_PLAYERS) return;

	b = &net_interp[slot];

	// A tick that is not newer is a reordered or duplicated datagram. A
	// repeat of the newest tick just refreshes it; anything older is dropped.
	// Both keep the samples ascending, which is what the draw below walks.
	if(b -> count && (int)(tick - b -> sample[b -> count - 1].tick) <= 0){
		if(tick == b -> sample[b -> count - 1].tick){
			b -> sample[b -> count - 1].pos = pos;
			b -> sample[b -> count - 1].yaw = yaw;
			b -> sample[b -> count - 1].vel = vel;
			b -> last_arrival_ms = (unsigned int)clocki();
			}
		return;
		}

	if(b -> count >= NET_INTERP_SAMPLES){
		for(int i = 1; i < NET_INTERP_SAMPLES; i ++)
			b -> sample[i - 1] = b -> sample[i];

		b -> count = NET_INTERP_SAMPLES - 1;
		}

	b -> sample[b -> count].tick = tick;
	b -> sample[b -> count].pos = pos;
	b -> sample[b -> count].yaw = yaw;
	b -> sample[b -> count].vel = vel;
	b -> count ++;

	b -> last_arrival_ms = (unsigned int)clocki();
}

static void net_interp_apply(int slot,mchMechosRacer* mr)
{
	netInterpSlot* b = &net_interp[slot];
	Vect3f pos,vel;
	float yaw,rt,a,d;
	int i;

	if(!b -> count) return;

	// One snapshot so far: nothing to interpolate yet, so use it as it is.
	if(b -> count == 1){
		mr -> setPosition(b -> sample[0].pos,b -> sample[0].yaw);
		mr -> setGlobalVelocity(b -> sample[0].vel);
		return;
		}

	// "Now" on the host's line: the newest tick plus however long it has been
	// since that tick landed, pushed back by the delay.
	rt = (float)b -> sample[b -> count - 1].tick
	   + (float)((unsigned int)clocki() - b -> last_arrival_ms) / (float)NET_TICK_MS
	   - (float)NET_INTERP_DELAY_TICKS;

	// Behind the oldest or ahead of the newest: hold the nearest end rather
	// than extrapolate. A stall shows as the car pausing, never as a snap.
	if(rt <= (float)b -> sample[0].tick){
		mr -> setPosition(b -> sample[0].pos,b -> sample[0].yaw);
		mr -> setGlobalVelocity(b -> sample[0].vel);
		return;
		}

	if(rt >= (float)b -> sample[b -> count - 1].tick){
		i = b -> count - 1;
		mr -> setPosition(b -> sample[i].pos,b -> sample[i].yaw);
		mr -> setGlobalVelocity(b -> sample[i].vel);
		return;
		}

	for(i = 0; i + 1 < b -> count; i ++)
		if(rt < (float)b -> sample[i + 1].tick)
			break;

	a = (rt - (float)b -> sample[i].tick)
	  / ((float)b -> sample[i + 1].tick - (float)b -> sample[i].tick);

	pos = b -> sample[i].pos + (b -> sample[i + 1].pos - b -> sample[i].pos) * a;
	vel = b -> sample[i].vel + (b -> sample[i + 1].vel - b -> sample[i].vel) * a;

	// Angle goes the short way, or a car crossing the 2*PI seam would spin
	// the long way round in the gap between two batches.
	yaw = b -> sample[i].yaw;
	d = b -> sample[i + 1].yaw - yaw;
	while(d > (float)M_PI) d -= 2.0f * (float)M_PI;
	while(d < -(float)M_PI) d += 2.0f * (float)M_PI;
	yaw += d * a;

	mr -> setPosition(pos,yaw);
	mr -> setGlobalVelocity(vel);
}

// Client side, once per frame: put every remote racer on the interpolated pose
// for now. The local car is driven by local input and is skipped.
static void net_apply_interpolation(void)
{
	mchRacer* rp;
	int local = net_match_S -> local_slot();
	int slot = 0;

	if(mchNetHost) return;

	rp = mch_raceD -> racerLst -> first();

	while(rp){
		mchMechosRacer* mr = dynamic_cast<mchMechosRacer*>(rp);

		if(mr && slot != local)
			net_interp_apply(slot,mr);

		rp = rp -> next;
		slot ++;
		}
}

/*
	The racer that belongs to a roster slot.

	The race builds racers in slot order and numbers them ID = slot + 1, so
	xtList::search() - which matches that ID - is off by one and hands back the
	neighbouring car. Every slot-to-racer lookup in this file goes through here;
	the list position is the slot.
*/
static mchRacer* net_racer_at(int slot)
{
	mchRacer* rp = mch_raceD -> racerLst -> first();

	while(rp && slot > 0){
		rp = rp -> next;
		slot --;
		}

	return slot == 0 ? rp : NULL;
}

/*
	Client side: check the locally simulated car against the host, once a
	second, and correct it only when the two have really come apart.

	The local car has to be simulated here or a keypress would wait a round
	trip, but it is the host's copy that everyone else sees, and the two do not
	step identically. Left alone they drift until the two players are watching
	their own car in different places. A per-frame nudge was tried and broke
	the car's turning - it rewrote the pose the dynamics integrate from - so
	the correction is a rare, hard snap that only fires past a threshold.
*/
static void net_reconcile_local(void)
{
	mchMechosRacer* mr;
	const float half = (float)NET_WORLD_SIZE * 0.5f;
	unsigned int now = (unsigned int)clocki();
	Vect3f r;
	float dx,dy,d2;

	if(!net_local_auth_valid) return;

	if((int)(now - net_reconcile_ms) < 1000) return;
	net_reconcile_ms = now;

	mr = dynamic_cast<mchMechosRacer*>(net_racer_at(net_match_S -> local_slot()));
	if(!mr) return;

	r = mr -> R();

	dx = net_local_auth.x - r.x;
	dy = net_local_auth.y - r.y;

	// The world is toroidal; the short way round is the one that matters.
	if(dx >  half) dx -= (float)NET_WORLD_SIZE;
	if(dx < -half) dx += (float)NET_WORLD_SIZE;
	if(dy >  half) dy -= (float)NET_WORLD_SIZE;
	if(dy < -half) dy += (float)NET_WORLD_SIZE;

	d2 = dx*dx + dy*dy;

	// Twenty metres is the threshold: under it the local simulation is
	// trusted, over it the host wins outright. Rare enough that the snap is
	// a blink, and only when the drift has become visible anyway.
	if(d2 < 20.0f * 20.0f) return;

	net_log("client: local car %.0f units from the host, snapping",
		Vect3f(dx,dy,0.0f).norm());

	mr -> setPosition(net_local_auth,net_local_auth_yaw);
}

/*
	Client side: put the host's view of a racer onto the local copy.

	Pose goes only to the other machines. The local racer is driven by local
	input and simulated locally, which is what keeps it responsive;
	overwriting it with a value that has been across the network would add a
	round trip of lag to the player's own car.

	Arcane state goes to everyone, the local car included - see the note in
	the loop. Remote racers are pose-driven from here on: they have no AI and
	no input, so the physics runs on from wherever the last snapshot put them.
*/
static void net_apply_snapshot(const void* data,unsigned int len)
{
	netSnapshotHeader hdr;
	netRacerState racers[NET_MAX_PLAYERS];
	int count = net_snapshot_unpack(&hdr,racers,NET_MAX_PLAYERS,(const unsigned char*)data,len);

	// The header's tick is the interpolation timeline. See the buffer below.
	for(int i = 0; i < count; i ++){
		mchRacer* rp = net_racer_at(racers[i].slot);
		mchMechosRacer* mr;

		if(!rp) continue;

		mr = dynamic_cast<mchMechosRacer*>(rp);

		if(!mr) continue;

		/*
			Arcane state goes to every racer, the local one included. A
			client never runs the arcane process - mchArcaneData::fxlabStart()
			returns at the net hook - so its own Power only charges and its
			Status never clears. The host's numbers are the true ones, and a
			keypress on this machine reads exactly these fields before it lets
			a spell out.
		*/
		for(int p = 0; p < NET_ACTIVE_PARTS; p ++){
			cMesh* part = mr -> part_by_index(p);

			if(!part) continue;

			part -> arcaneData.Status = racers[i].part_status[p] ? 1 : 0;
			part -> arcaneData.Power = net_uq_mana(racers[i].part_power[p],
							       part -> arcaneData.MaxPower);
			}

		// The destroy-assemble sequence, kicked off on the transition out of
		// NORMAL. It goes to the local car too: the client drives its own car
		// but never damages it, so without this it would keep driving through
		// its own explosion on every other machine.
		mr -> net_apply_state(racers[i].mech_state);

		/*
			The local car keeps its own simulation: a keypress that waited a
			round trip would feel dead, and that is the whole reason it is
			simulated here. The host's pose is kept only to check it against,
			once a second - see net_reconcile_local.
		*/
		if(racers[i].slot == net_match_S -> local_slot()){
			net_local_auth.set(net_uq_world(racers[i].pos_x),
					   net_uq_world(racers[i].pos_y),
					   net_uq_height(racers[i].pos_z));
			net_local_auth_yaw = net_uq_yaw(racers[i].yaw);
			net_local_auth_valid = 1;
			continue;
			}

		// Buffered, not applied: the draw interpolates between the last two
		// snapshots a fixed delay behind the newest. See
		// net_apply_interpolation.
		net_interp_remember(racers[i].slot,hdr.server_tick,
				    Vect3f(net_uq_world(racers[i].pos_x),
					   net_uq_world(racers[i].pos_y),
					   net_uq_height(racers[i].pos_z)),
				    net_uq_yaw(racers[i].yaw),
				    Vect3f(net_uq_speed(racers[i].vel_x),
					   net_uq_speed(racers[i].vel_y),
					   (float)racers[i].vel_z));
		}
}

/*
	Class D: the cosmetic RNG.

	The host sends a seed once, in the welcome, and every machine rolls
	fxlabClientRND from it. That is what makes dust, trail noise and every
	other purely local effect agree across machines without any of them being
	sent: they are drawn from the same stream in the same order.

	Deliberately not the gameplay RNG. That one feeds the same solver the
	physics does, and sharing it would mean every machine had to reproduce
	every frame exactly, which is the lockstep design this protocol is not.

	Seeded once per match, because the welcome arrives once per match.
*/
static int net_rnd_seeded = 0;

/* ------------------------------- arcane events ---------------------------- */

/*
	Live event queue. Filled by mchNetArcaneCast, flushed once per tick in
	mchNetQuant(), next to the input batch - the same tick, so that an event
	and the input that produced it travel together.
*/
static netEvent net_event_q[NET_MAX_EVENT_PER_PACKET];
static int net_event_q_count = 0;

void mchNetArcaneCast(int ownerID,int part,int event,int arcane_id)
{
	netEvent* e;

	// Only a client sends. The host casts for real: mchArcaneData::fxlabStart()
	// calls in here only when this flag says so, and the host's copy runs
	// through the engine instead. An event arriving at a host is a bug, not a
	// thing to forward.
	if(!mchNetGame || mchNetHost) return;
	if(!net_session_S || !net_match_S) return;

	// part is which part the arcane sits on, resolved by the caller against the
	// racer's own part list. -1 means it could not be resolved, and casting a
	// spell on no part in particular would hit somebody else's car.
	if(part < 0){
		net_log("client: arcane %d dropped, part not resolved",arcane_id);
		return;
		}

	if(net_event_q_count >= NET_MAX_EVENT_PER_PACKET){
		net_log("client: event queue full, dropping one of %d",net_event_q_count);
		return;
		}

	e = &net_event_q[net_event_q_count ++];

	memset(e,0,sizeof(netEvent));

	e -> type = (unsigned char)event;
	e -> slot = (unsigned char)net_match_S -> local_slot();
	e -> part = (unsigned char)part;
	e -> arcane_id = (unsigned char)arcane_id;

	// Pos_x, pos_y and target_id are for event types that name a place or a
	// victim. An arcane cast is neither: the host already knows where the
	// racer is, because it is simulating that racer.
	e -> pos_x = 0;
	e -> pos_y = 0;
	e -> target_id = 0;
	e -> reserved = 0;
	e -> time_ms = 0;

	net_log("client: arcane %d cast queued on part %d, event %d",arcane_id,part,event);
}

/*
	Host side: replay an event against the racer it names.

	Slot is the sender's, resolved against the roster order - which is the same
	order net_apply_host_inputs() uses to hand out input, so an event and the
	input that produced it land on the same car.
*/
static void net_apply_event(const netEvent* e)
{
	mchRacer* rp;
	mchMechosRacer* mr;
	cMesh* m;
	int slot = 0;

	if(e -> slot >= NET_MAX_PLAYERS) return;

	rp = mch_raceD -> racerLst -> first();

	while(rp && slot < e -> slot){
		rp = rp -> next;
		slot ++;
		}

	if(!rp){ net_log("host: event slot %d, no such racer",(int)e -> slot); return; }

	mr = dynamic_cast<mchMechosRacer*>(rp);
	if(!mr){ net_log("host: event slot %d is not a mechos",(int)e -> slot); return; }

	m = mr -> part_by_index(e -> part);
	if(!m){
		net_log("host: event slot %d part %d missing",(int)e -> slot,(int)e -> part);
		return;
		}

	// Both arcane paths check this before they cast, so a client that has not
	// yet spent an arcane and a host that already has are distinguishable here
	// - which is what stops a client from holding a button down and getting a
	// spell per press for free.
	if(!(e -> type == NET_EV_ARCANE_STOP || m -> arcaneData.Status)){
		net_log("host: event %d slot %d part %d dropped, status %d",
			(int)e -> type,(int)e -> slot,(int)e -> part,(int)m -> arcaneData.Status);
		return;
		}

	m -> arcaneData.ownerID = rp -> ID;

	// This re-enters mchArcaneData::fxlabStart(), which is fine on two counts:
	// the host never routes out to the net layer, so it cannot loop, and the
	// racer this lands on is the host's own simulation of that player's car.
	switch(e -> type){
		case NET_EV_ARCANE_CAST:
			m -> arcaneData.fxlabStart();
			break;

		case NET_EV_ARCANE_STOP:
			m -> arcaneData.fxlabStop();
			break;

		default: return;
		}

	net_log("host: event %d on slot %d part %d arcane %d",
		(int)e -> type,(int)e -> slot,(int)e -> part,(int)e -> arcane_id);
}

static void net_flush_events(void)
{
	unsigned char buf[NET_MAX_PAYLOAD];
	int bytes;

	if(!net_event_q_count){
		net_event_q_count = 0;
		return;
		}

	bytes = net_event_pack(buf,sizeof(buf),net_match_S -> tick(),net_event_q,net_event_q_count);

	// Reliable. A lost event is a spell that never happened, and spells are
	// rare enough that the queue never falls behind.
	if(bytes > 0)
		net_session_S -> broadcast(NET_MSG_EVENT,buf,(unsigned int)bytes,1);

	net_event_q_count = 0;
}

/*
	Host side: queue a visual broadcast for an arcane that just started here.

	Called from mchArcaneData::fxlabStart() on the host, so it covers both the
	host's own cast and a client cast the host has just replayed. Only the
	look travels - the client that receives it builds client FX objects and
	nothing else (fxlabClientArcaneStart).
*/
void mchNetArcaneBroadcast(int ownerID,int arcane_id)
{
	netEvent* e;
	mchRacer* rp;
	int slot = 0;

	if(!mchNetGame || !mchNetHost) return;
	if(!net_session_S || !net_match_S || !mch_raceD) return;
	if(net_match_S -> state() != NET_MATCH_RUNNING) return;

	if(net_event_q_count >= NET_MAX_EVENT_PER_PACKET) return;

	// The client resolves the racer by slot, the same order net_apply_event
	// and net_apply_host_inputs use.
	rp = mch_raceD -> racerLst -> first();

	while(rp && rp -> ID != ownerID){
		rp = rp -> next;
		slot ++;
	}

	if(!rp) return;

	e = &net_event_q[net_event_q_count ++];

	memset(e,0,sizeof(netEvent));

	e -> type = NET_EV_ARCANE_CAST;
	e -> slot = (unsigned char)slot;
	e -> arcane_id = (unsigned char)arcane_id;

	net_log("host: visual arcane %d queued for slot %d",arcane_id,slot);
}

/*
	Host side: one interrupt of an arcane that already has its cast visual on
	the wire. See the header. The place travels because a storm strike sits at
	the cast point while a dropped seed sits at the owner, and the client
	cannot tell those apart from the owner alone.
*/
void mchNetArcaneInterrupt(int ownerID,int arcane_id,float x,float y,float z)
{
	netEvent* e;
	mchRacer* rp;
	int slot = 0;

	if(!mchNetGame || !mchNetHost) return;
	if(!net_session_S || !net_match_S || !mch_raceD) return;
	if(net_match_S -> state() != NET_MATCH_RUNNING) return;

	if(net_event_q_count >= NET_MAX_EVENT_PER_PACKET) return;

	rp = mch_raceD -> racerLst -> first();

	while(rp && rp -> ID != ownerID){
		rp = rp -> next;
		slot ++;
	}

	if(!rp) return;

	e = &net_event_q[net_event_q_count ++];

	memset(e,0,sizeof(netEvent));

	e -> type = NET_EV_ARCANE_INTERRUPT;
	e -> slot = (unsigned char)slot;
	e -> arcane_id = (unsigned char)arcane_id;
	e -> pos_x = net_q_spawn_pos(x);
	e -> pos_y = net_q_spawn_pos(y);
	e -> pos_z = net_q_spawn_pos(z);
}

/*
	Host side: an arcane has ended; its cast visual should come down everywhere.

	Same addressing as a cast (slot + arcane id), and reliable for the same
	reason: a lost stop leaves a spell drawn on a client forever. Only the
	look travels - the host's own visual is killed by the arcane's Close().
*/
void mchNetArcaneVisualStop(int ownerID,int arcane_id)
{
	netEvent* e;
	mchRacer* rp;
	int slot = 0;

	if(!mchNetGame || !mchNetHost) return;
	if(!net_session_S || !net_match_S || !mch_raceD) return;
	if(net_match_S -> state() != NET_MATCH_RUNNING) return;

	if(net_event_q_count >= NET_MAX_EVENT_PER_PACKET) return;

	rp = mch_raceD -> racerLst -> first();

	while(rp && rp -> ID != ownerID){
		rp = rp -> next;
		slot ++;
	}

	if(!rp) return;

	e = &net_event_q[net_event_q_count ++];

	memset(e,0,sizeof(netEvent));

	e -> type = NET_EV_ARCANE_STOP_VISUAL;
	e -> slot = (unsigned char)slot;
	e -> arcane_id = (unsigned char)arcane_id;
}

/*
	Client side: an arcane somebody else cast. Build only its visual.

	The sender is skipped: it already built its own visual the moment it
	cast, before the event even left (see the client branch of
	mchArcaneData::fxlabStart()).
*/
static void net_apply_visual_event(const netEvent* e)
{
	mchRacer* rp;
	int slot = 0;

	if(e -> type != NET_EV_ARCANE_CAST && e -> type != NET_EV_ARCANE_INTERRUPT &&
	   e -> type != NET_EV_ARCANE_STOP_VISUAL) return;
	if(e -> slot >= NET_MAX_PLAYERS) return;

	net_log("client: visual arcane %d type %d slot %d",(int)e -> arcane_id,(int)e -> type,(int)e -> slot);

	rp = mch_raceD -> racerLst -> first();

	while(rp && slot < e -> slot){
		rp = rp -> next;
		slot ++;
	}

	if(!rp || rp -> Type != MCH_RACER_TYPE_MECHOS) return;

	if(e -> type == NET_EV_ARCANE_STOP_VISUAL){
		// The arcane is over. Kill the mirror visual this machine built for
		// it, and put back any feature it changed on the car.
		fxlabClientArcaneStop(e -> arcane_id,rp -> ID);
		return;
		}

	if(e -> type == NET_EV_ARCANE_INTERRUPT){
		/*
			The caster did not build this one. Interrupts run only on the host,
			so unlike a cast there is nothing local to skip here - the casting
			client is waiting for exactly this to see its own storm strike.
			The place rides on the event.
		*/
		fxlabClientArcaneInterrupt(e -> arcane_id,rp -> ID,
					   Vect3f(net_uq_spawn_pos(e -> pos_x),
						  net_uq_spawn_pos(e -> pos_y),
						  net_uq_spawn_pos(e -> pos_z)));
		return;
		}

	/*
		A cast. Every client builds it, the caster included.

		The caster used to be skipped because it built its own visual the
		moment it cast - but it built it with the ownerID sitting on the arcane
		data, which is only whatever the last local trigger set, and a client
		that casts without touching that path got nothing. The host names the
		racer on every event, so the caster draws from here like everybody
		else and the two cannot disagree.
	*/
	fxlabClientArcaneStart(e -> arcane_id,rp -> ID);
}

static void net_seed_cosmetic_rng(void)
{
	netWelcome w;
	unsigned int seed;

	// Seeded already, or no welcome yet. Both are normal: the second is just
	// a client that has not been told its slot.
	if(net_rnd_seeded || !net_match_S -> welcome(&w)) return;

	seed = (unsigned int)w.client_rnd[0]
	     | ((unsigned int)w.client_rnd[1] << 8)
	     | ((unsigned int)w.client_rnd[2] << 16)
	     | ((unsigned int)w.client_rnd[3] << 24);

	fxlabClientRND.Open(seed);
	net_rnd_seeded = 1;

	net_log("client: cosmetic rng seeded with %08x",seed);
}

static void net_game_handler(int type,const void* data,unsigned int len,void* player)
{
	(void)player;

	switch(type){
		case NET_MSG_SNAPSHOT:
			net_apply_snapshot(data,len);
			return;

		case NET_MSG_SPAWN:
			net_fx_spawn_recv((const unsigned char*)data,len);
			return;

		case NET_MSG_DESPAWN:
			net_fx_despawn_recv((const unsigned char*)data,len);
			return;

		case NET_MSG_ACTOR_STATE:
			net_fx_actor_recv((const unsigned char*)data,len);
			return;

		// Events travel both ways now: a client sends its casts to the host,
		// and the host sends visuals to the clients.
		case NET_MSG_EVENT:
			{
			int count;
			netEventHeader hdr;
			netEvent batch[NET_MAX_EVENT_PER_PACKET];

			count = net_event_unpack(&hdr,batch,NET_MAX_EVENT_PER_PACKET,
						 (const unsigned char*)data,len);

			// A client's events are casts it wants the host to run; a host's
			// are visuals it wants clients to draw.
			for(int i = 0; i < count; i ++){
				if(mchNetHost)
					net_apply_event(&batch[i]);
				else
					net_apply_visual_event(&batch[i]);
				}
			}
			return;
		}

	// Anything else is the roster's business, or a message this build does not
	// know: a newer client talking to an older host sends types this has never
	// heard of, and ignoring them is the whole of the compatibility story.
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

	// Projectiles travel as their own message types and are tracked by entity
	// id, which is per match. The registry is NOT reset here: it was cleared
	// before the world was built (see GameQuantRTO::Init) so that the walking
	// actors the host placed during the load are still in it and get
	// replicated. Clearing it now would silently drop every one of them.
	net_fx_set_session(net_session_S);
	net_interp_reset();

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

	// Body ids restart from zero for the next race, so a table that still
	// points at last race's projectiles would despawn somebody else's.
	net_fx_set_session(NULL);
	net_fx_reset();
	net_interp_reset();

	net_client_ready = 0;
	net_camera_set = 0;
	net_last_player_count = -1;
	net_start_logged = 0;
	net_rnd_seeded = 0;
	net_motion_have = 0;
	net_local_auth_valid = 0;
	net_reconcile_ms = 0;
	net_input_sent_count = 0;
	net_input_recv_count = 0;
	net_event_q_count = 0;

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

		if(slot != NET_SLOT_HOST && net_match_S -> get_input(slot,&in)){
			rp -> set_net_input(in);
			net_input_recv_count ++;
			}

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
			int ai = (rp -> flags & MCH_AI) != 0;

			/*
				AI_Toggle() flips the flag, it does not set it: AI_Toggle(p,1)
				on a car that is already AI turns the AI *off* and drops its
				seeds. Calling it on every roster heartbeat therefore parked
				every bot - they were built as AI, and the second the roster
				was counted they stopped being AI. It is only called when the
				AI state actually has to change.

				A seated client takes its slot back from the AI; a slot nobody
				drives gets the AI back.
			*/
			if(taken){
				if(ai) mch_raceD -> AI_Toggle(rp,0);
				rp -> set_net_control(1);
				}
			else {
				if(!ai) mch_raceD -> AI_Toggle(rp,1);
				rp -> set_net_control(0);
				}
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
	rp = net_racer_at(net_match_S -> local_slot());
	if(!rp) return;

	mechos = dynamic_cast<mchMechosRacer*>(rp);
	if(!mechos) return;

	memset(&in,0,sizeof(in));
	mechos -> read_input(in);
	in.tick = net_match_S -> tick();

	// -netdrive: pretend the player is holding down forward and turning. The
	// route to the host is exactly the real one from here on.
	if(mchNetAutoDrive){
		in.traction = 127;
		in.rudder = 48;
		}

	bytes = net_match_S -> build_input(payload,sizeof(payload),in);
	if(bytes > 0)
		net_match_S -> send_input(payload,bytes);

	net_input_sent_count ++;
}

/*
	How many racers actually moved since the last call.

	Positions, not speeds: on a client a remote racer's velocity is not derived
	from the snapshot, so a speed test would call a smooth interpolated car
	parked. Anything past four units between two reports means it is driving.
*/
static void net_log_motion(void)
{
	mchRacer* rp = mch_raceD -> racerLst -> first();
	char mask[NET_MAX_PLAYERS + 1];
	int slot = 0,moved = 0,total = 0;

	while(rp && slot < NET_MAX_PLAYERS){
		mchMechosRacer* mr = dynamic_cast<mchMechosRacer*>(rp);

		mask[slot] = '.';

		if(mr){
			const Vect3f& r = mr -> R();

			if(net_motion_have){
				float dx = r.x - net_motion_last[slot].x;
				float dy = r.y - net_motion_last[slot].y;

				if(dx*dx + dy*dy > 4.0f){ moved ++; mask[slot] = 'm'; }
				}

			net_motion_last[slot] = r;
			total ++;
			}

		rp = rp -> next;
		slot ++;
		}

	mask[slot] = 0;
	net_motion_have = 1;

	// The mask names which slots moved, in slot order: `m` is a car that went
	// somewhere in the last ten seconds. A parked car shows as `.`, which is
	// what tells a car that is not being driven from one that is not there.
	net_log("%s: %d of %d racers moved [%s]",
		mchNetHost ? "host" : "client",moved,total,mask);
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

	// Class D seed. After the pump because that is when the welcome lands.
	if(!mchNetHost)
		net_seed_cosmetic_rng();

	// Class B. Flushes whatever the host queued while bodies were being
	// attached, and drops entries for bodies that died in the last frame.
	net_fx_quant();

	// Class A. Client only: remote racers are drawn on an interpolated pose
	// between the last two snapshots. After the pump, so this frame's
	// snapshots are already in the buffer.
	net_apply_interpolation();

	// The local car is simulated here, but once a second it is checked against
	// the host's copy and pulled back if the two have really come apart.
	if(!mchNetHost)
		net_reconcile_local();

	// Events go out on the same tick as the input, so that a spell and the
	// steering that went with it are one story. A client queues them in
	// mchNetArcaneCast; a host queues visual broadcasts in
	// mchNetArcaneBroadcast. Both flush here, on whichever side this is.
	net_flush_events();

	/*
		The registry is the one part of the stack that has never once run, so
		it says so, on a timer, rather than leaving a live test to guess. Every
		ten seconds is plenty: what matters is whether the numbers move at all
		when an arcane goes off and a bonus man is picked up.
	*/
	if(now - net_last_fx_log_ms >= 10000){
		int projectiles = 0,actors = 0;

		net_last_fx_log_ms = now;
		net_fx_stats(&projectiles,&actors);

		net_log("%s: entities %d projectile, %d actor",
			mchNetHost ? "host" : "client",projectiles,actors);

		net_log_motion();

		net_log("%s: input sent %u, recv %u",
			mchNetHost ? "host" : "client",
			net_input_sent_count,net_input_recv_count);
		}

	/* --------------------------- drive the race ------------------------- */

	players = net_match_S -> num_players();

	if(players != net_last_player_count){
		// The roster changed: somebody joined, left, or was refused. AI and
		// camera both key off who is actually driving, so both are redone.
		net_last_player_count = players;
		net_camera_set = 0;

		if(mchNetHost){
			net_update_ai();

			// A client that has just joined has never seen the world's
			// actors: their spawns went out before it was there. Send them
			// all again so the newcomer's track is not empty.
			net_fx_resend_actors();
			}

		net_log("%s: roster now %d player(s), state %d",
			mchNetHost ? "host" : "client",players,net_match_S -> state());
		}

	if(mchNetHost){
		net_apply_host_inputs();

		// Snapshots only exist on the host: it is the only machine that knows
		// where anything is. The actors go the same way, and only from here:
		// a pass before the match is running would spawn them to nobody.
		if(net_match_S -> state() == NET_MATCH_RUNNING){
			net_send_snapshot();
			net_fx_update_actors();
			}

		if(net_match_S -> state() != NET_MATCH_LOBBY)
			mchNetStatusText = "Racing";
		}

	if(!mchNetHost && net_match_S -> local_slot() >= 0 && !net_client_ready){
		mchRacer* rp = net_racer_at(net_match_S -> local_slot());
		int slot = 0;

		net_client_ready = 1;

		net_log("client: assigned slot %d",net_match_S -> local_slot());

		// The client drives its own slot and nothing else, so the AI has to
		// let go of just that one. setActiveRacer also points the camera and
		// the control config at it.
		if(rp){
			mch_raceD -> AI_Toggle(rp,0);
			mch_raceD -> setActiveRacer(rp);
			}

		/*
			Every remote racer is pose-driven from the host and marked
			net-controlled so its own physics does not also steer it - two
			simulators on one car is jitter and a pose that disagrees with the
			host. The local car is the exception: it is simulated here, from
			the keyboard, and only checked against the host once a second.
		*/
		rp = mch_raceD -> racerLst -> first();

		while(rp){
			mch_raceD -> AI_Toggle(rp,0);

			if(slot != net_match_S -> local_slot())
				rp -> set_net_control(1);

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
