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
#include "net_lobby.h"
#include "net_lobby_protocol.h"

#include "mechosoma.h"
#include "race.h"
#include "TrackDispatcher.h"
#include "Mechos.h"
#include "arcane.h"
#include "BodyDispatcher.h"
#include "BonusMan.h"
#include "fxlabGeneral.h"
#include "fxlabInterface.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// sound.h pulls the audio stack, which this file has no other use for. One
// declaration is enough for the pause undo below.
extern void mchResumeSound(void);

// The end-of-race screen, normally entered from the race dispatcher. The
// client reaches it from here when the host says the race is over.
void mchEndGame(void);

// race.cpp: the finish fanfare (sound + effect) a machine plays for a racer it
// did not simulate. Lives there because the audio headers already do.
void mchNetFinishVisual(mchRacer* p);

// race.cpp: the same, for a bonus pickup.
void mchNetBonusVisual(mchRacer* p);

/*
	Net races write a running log to net.log, next to the binary.

	A LAN session fails in ways nobody can see from the outside - a peer that
	never connects, a start tick nobody agrees on - and the only place that
	knows is this layer. One file, written while racing, is the difference
	between "it does not work" and something diagnosable.
*/

static FILE* net_log_file = NULL;
static FILE* net_trace_file = NULL;

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

	// Appended, not truncated: a lobby that is created and left several times
	// is exactly the run worth reading, and opening with "w" would throw away
	// every attempt but the last. Each session starts with its own
	// "net race start" line, so the attempts stay separable.
	net_log_file = fopen(name,"a");

	if(net_log_file){
		fprintf(net_log_file,"\n==== %s session ====\n",mchNetHost ? "host" : "client");
		fflush(net_log_file);
	}
}

static void net_log_close(void)
{
	if(!net_log_file) return;

	fclose(net_log_file);
	net_log_file = NULL;
}

/* ------------------------------ state trace ------------------------------ */

/*
	-netdump. See the note on mchNetTrace for what the three kinds of line mean.
	One file per role, same schema, keyed by the host tick so the two can be
	lined up row for row without knowing anything about wall clock.
*/

static void net_trace_open(void)
{
	if(!mchNetTrace) return;

	net_trace_file = fopen(mchNetHost ? "net-trace-host.csv" : "net-trace-client.csv","w");

	if(net_trace_file)
		fprintf(net_trace_file,
			"tick,kind,key,type,variant,px,py,pz,yaw,vx,vy,vz,speed,energy,"
			"flags,lap,cp,place,nstar,mech,feat,pw0,pw1,pw2,st0,st1,st2,model,alive\n");
}

static void net_trace_close(void)
{
	if(!net_trace_file) return;

	fclose(net_trace_file);
	net_trace_file = NULL;
}

static void net_trace_racer_line(unsigned int tick,char kind,int key,int type,
	float px,float py,float pz,float yaw,float vx,float vy,float vz,
	float speed,unsigned energy,unsigned flags,unsigned lap,unsigned cp,
	unsigned place,unsigned nstar,unsigned mech,unsigned feat,
	unsigned pw0,unsigned pw1,unsigned pw2,unsigned st0,unsigned st1,unsigned st2)
{
	if(!net_trace_file) return;

	// model and alive are entity-only; a racer has no replicated mesh id, so
	// they are written as 0 and 1 and never compare in the E section.
	fprintf(net_trace_file,
		"%u,%c,%d,%d,0,%.4f,%.4f,%.4f,%.6f,%.4f,%.4f,%.4f,%.2f,%u,"
		"%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,0,1\n",
		tick,kind,key,type,px,py,pz,yaw,vx,vy,vz,speed,energy,flags,lap,cp,
		place,nstar,mech,feat,pw0,pw1,pw2,st0,st1,st2);
}

// The wire state of every racer in a snapshot. Identical on both sides when
// the datagram arrived: this is what measures loss and reordering, not drift.
static void net_trace_racers_wire(unsigned int tick,const netRacerState* racers,int count)
{
	for(int i = 0; i < count; i ++){
		const netRacerState* s = &racers[i];

		net_trace_racer_line(tick,'R',(int)s -> slot,(int)s -> racer_id,
			net_uq_world(s -> pos_x),net_uq_world(s -> pos_y),net_uq_height(s -> pos_z),
			net_uq_yaw(s -> yaw),
			net_uq_speed(s -> vel_x),net_uq_speed(s -> vel_y),(float)s -> vel_z,
			(float)s -> speed,(unsigned)s -> energy,(unsigned)s -> flags,
			(unsigned)s -> lap,(unsigned)s -> checkpoint,(unsigned)s -> place,
			(unsigned)s -> nstar,(unsigned)s -> mech_state,
			(unsigned)(s -> features[0] | (s -> features[1] << 8) | (s -> features[2] << 16)),
			s -> part_power[0],s -> part_power[1],s -> part_power[2],
			s -> part_status[0],s -> part_status[1],s -> part_status[2]);
	}
}

// What is actually in this machine's world this tick. Client side that is the
// interpolated remote cars and the locally simulated own car.
static void net_trace_racers_world(unsigned int tick)
{
	mchRacer* rp = mch_raceD -> racerLst -> first();
	int slot = 0;

	while(rp && slot < NET_MAX_PLAYERS){
		mchMechosRacer* mr = dynamic_cast<mchMechosRacer*>(rp);

		if(mr){
			const Vect3f& p = mr -> R();
			const Vect3f& v = mr -> Vglobal();
			unsigned pw[NET_ACTIVE_PARTS],st[NET_ACTIVE_PARTS];
			unsigned lap = 0,cp = 0,place = 0,nstar = 0;

			for(int i = 0; i < NET_ACTIVE_PARTS; i ++){
				cMesh* part = mr -> part_by_index(i);

				pw[i] = part ? net_q_mana(part -> arcaneData.Power,part -> arcaneData.MaxPower) : 0;
				st[i] = part ? (part -> arcaneData.Status ? 1u : 0u) : 0u;
			}

			if(rp -> stPtr){
				lap = (unsigned)rp -> stPtr -> Lap;
				cp = (unsigned)rp -> stPtr -> Checkpoint;
				place = (unsigned)rp -> stPtr -> place;
				nstar = (unsigned)(rp -> NStar + rp -> stPtr -> NStar);
			}

			net_trace_racer_line(tick,'W',slot,rp -> ID,
				p.x,p.y,p.z,mr -> psi(),
				v.x,v.y,v.z,v.norm(),
				net_q_mana(mr -> Energy(),mr -> energy_max()),
				(unsigned)rp -> flags,lap,cp,place,nstar,
				(unsigned)mr -> state(),(unsigned)mr -> features(),
				pw[0],pw[1],pw[2],st[0],st[1],st[2]);
		}

		rp = rp -> next;
		slot ++;
	}
}

// mchNumLaps, mchNumRacers and mchNumRacersDef live in the setup/race headers
// rather than in mechosoma.h, and are declared extern in the sources that use
// them.
extern int mchNumLaps;
extern int mchNumRacers;
extern int mchNumRacersDef;

// The racer roster the race builds its cars from. Named here so the grid log
// below can say which slot drives which mechos figure.
extern xtList<mchRacerStats> mch_racerLst;

// How many mechos figures exist; a figure id is 0..acsMaxEssenceID-1.
extern int acsMaxEssenceID;

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
/*
	-netbots: run the client's own car as a bot, driven by the same AI the
	host uses for its spare slots, but with the AI's steering sent to the host
	as an ordinary input sample.

	The AI decides locally; point_control() output is captured instead of
	applied (see PointControlledObject::control), and both this machine and
	the host then apply it through net_control. That is what makes the bot's
	actions travel over the same INPUT message a player's would, and keeps the
	client's local copy and the host's copy on one code path.

	The host's own car and its spare slots are driven by its AI directly; only
	a seated client's car needs the round trip, so -netbots on both machines
	leaves a field of bots with the client's one transmitting. Test
	scaffolding, no effect on a normal race.
*/
int mchNetBots = 0;
/*
	-netdestroy: the host wrecks two adjacent bot slots a moment after the
	start, so the destroy-assemble sequence (and the part sync that follows it)
	runs in a test with nobody at the keyboard. Test scaffolding, no effect on
	a normal race.
*/
int mchNetDestroy = 0;
/*
	-netdump: write a per-tick CSV of every networked object, so a host and a
	client trace can be aligned on the host tick and compared field by field.

	Three kinds of line share one schema, told apart by the `kind` column:

	  R  a racer as it travels on the wire - host side the state it just packed
	     and sent, client side the state it just unpacked. Same host tick and
	     the same quantization on both machines, so R lines compare exactly.
	  W  a racer as it actually is in this machine's world. Client only (a
	     host's W would repeat its R): it is the interpolated remote cars and
	     the locally simulated own car, which is what the player sees.
	  E  a replicated body (class B projectiles, class C actors) as it is in
	     this machine's world.

	Diagnostic only. Off unless asked for, and when off it costs one branch.
*/
int mchNetTrace = 0;
char mchNetJoinAddr[128] = "127.0.0.1";
int mchNetPort = NET_DEFAULT_PORT;

/*
	The network menu's pick. Zero means "not chosen": a race armed from the
	command line only (-nethost / -netjoin / -netrace) leaves these empty and
	falls back to the old defaults, so the test entry point is unchanged.
*/
int mchNetMenuFigure = 0;
int mchNetMenuSpell[3] = { 0, 0, 0 };
int mchNetMenuWorld = -1;
int mchNetMenuTrack = -1;
char mchNetRacerConfig[30] = "";
char mchNetMenuName[32] = "";

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
	// The menu's pick wins when there is one; the -netrace path leaves it
	// unset and keeps the fixed world.
	mchCurrentWorld = (mchNetMenuWorld >= 0) ? mchNetMenuWorld : NET_RACE_WORLD;
	mchCurrentTrack = (mchNetMenuTrack >= 0) ? mchNetMenuTrack : NET_RACE_TRACK;
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
static int net_grid_logged = 0;

/*
	Lobby server client state. Declared here, above mchNetLobbyQuant, because
	that function has to know whether a lobby connection owns the dial or the
	typed address does. The implementation is further down, next to the rest of
	the lobby API.
*/
static netLobbyClient* net_lobby_S = NULL;
static int net_lobby_mode = 0;		// the menu chose the server, not a direct address
static int net_lobby_browse = 0;	// client: the match list is up
static int net_lobby_joined = 0;	// client: the server introduced a host
static unsigned int net_lobby_probe_ms = 0;
static unsigned int net_lobby_list_ms = 0;
static int net_lobby_reg_players = -1;	// host: last player count registered
static int net_lobby_host_known = 0;
static unsigned int net_lobby_host_ip = 0;
static unsigned short net_lobby_host_port = 0;
static int net_lobby_join_id = -1;	// client: match id it asked to join
static unsigned int net_lobby_join_ms = 0;	// client: when the JOIN was last sent

// Client: what a re-connect needs. The rendezvous parameters are saved when
// the menu starts the join, because mchNetFinit frees the session that holds
// them; and when the server first introduced a host, so the re-connect watch
// can tell how long this client has been waiting for a slot.
static char net_lobby_addr_saved[128] = "";
static char net_lobby_name_saved[32] = "";
static int net_lobby_figure_saved = 0;
static int net_lobby_spells_saved[3] = { 0,0,0 };
static unsigned int net_lobby_joined_ms = 0;

// How long a client introduced to a host waits for a slot before starting the
// whole connection over. Long enough to ride out a slow world load or one lost
// retransmit, short enough not to leave the lobby sitting empty.
static const int NET_LOBBY_RECONNECT_MS = 6000;

// -netlobbyhost / -netlobbyjoin: drive the rendezvous path with no menu.
// Test scaffolding, see mchNetLobbyAutoQuant.
int mchNetLobbyAuto = 0;
char mchNetLobbyAutoAddr[128] = "";

/*
	Host side: the endpoints the lobby server has introduced. The client
	retries its JOIN, which brings a fresh introduction every second, but a
	client whose control connection dropped keeps dialling on its own. This
	list makes the host keep knocking at each introduced endpoint for a while
	instead of only in the instant the introduction lands, so the host's NAT
	mapping stays open for the client's retried hello.
*/
struct netLobbyKnock
{
	unsigned int ip;
	unsigned short port;
	unsigned int next_ms;
	int tries;
};

static netLobbyKnock net_lobby_knock[NET_MAX_PLAYERS];
static int net_lobby_knock_count = 0;
static int net_lobby_seated_logged = 0;
static int net_lobby_rebrowse = 0;	// client: a rejected pick sent it back to the list
static int net_lobby_last_matches = -1;	// client: last list size logged

// Last sampled position per slot, so the periodic log can say whether the
// field is actually driving or just sitting there.
static Vect3f net_motion_last[NET_MAX_PLAYERS];
static int net_motion_have = 0;

// Client side: the host's authoritative pose for the local car, and when it was
// last checked against the local one. See net_reconcile_local.
static Vect3f net_local_auth;
static Vect3f net_local_auth_vel;
static float net_local_auth_yaw = 0;
static int net_local_auth_valid = 0;

/*
	The host's race progress for the local car: Lap and Checkpoint as its
	authoritative copy has them. The client counts its own car's line crossings
	so its passes animate, but that count can run a checkpoint ahead of the
	host's for good, and the checkpoint a racer is entitled to is read off
	Lap/Checkpoint. The count on screen and the stars a pass is worth are
	therefore compared against these, not the local pointer - see
	mchNetRacerHostStanding.
*/
static int net_local_stand_lap = 0;
static int net_local_stand_cp = 0;
static int net_local_stand_valid = 0;

// A fingerprint of every racer's host standing, so the checkpoint count can be
// recomputed on the frames the standings actually moved rather than every
// snapshot. Remote cars no longer advance their own checkpoint on a client
// (CP_Quant leaves them to the host), so their passes no longer trigger the
// recompute; this is what does.
static unsigned net_stand_fingerprint = 0;
static int net_stand_dirty = 0;

// Per slot, whether the horn was held last tick. The host plays one beep per
// press, so the relay has to fire on the rising edge too.
static int net_horn_last[NET_MAX_PLAYERS];

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
			s -> energy = net_q_mana(mr -> Energy(),mr -> energy_max());

			// Race state for the table: time, stars and the finished flag. The
			// client cannot derive any of them for a car it only places by
			// pose, and the result screen reads all three.
			s -> flags = (unsigned short)rp -> flags;

			// The engine feature bits, 22 of them in three bytes. Only
			// STATIC_NULIFICATOR is read back today - it is what the teleport
			// trap sets on its victim to freeze and hide it during the charge -
			// but the field is here for the rest as they are needed.
			s -> features[0] = (unsigned char)(mr -> features() & 0xFF);
			s -> features[1] = (unsigned char)((mr -> features() >> 8) & 0xFF);
			s -> features[2] = (unsigned char)((mr -> features() >> 16) & 0xFF);

		// Race standing. The result table and the HUD read these off every
		// racer's own stats, and a client's remote cars are pose-driven - they
		// never cross a checkpoint locally, so without this every remote car
		// sits on lap one, checkpoint one, last place.
		if(rp -> stPtr){
			s -> lap = (unsigned char)rp -> stPtr -> Lap;
			s -> checkpoint = (unsigned char)rp -> stPtr -> Checkpoint;
			s -> place = (unsigned char)rp -> stPtr -> place;
			s -> time_ms = (unsigned int)(rp -> stPtr -> time + rp -> stPtr -> cur_time);
			s -> nstar = (unsigned short)(rp -> NStar + rp -> stPtr -> NStar);
			}

			// Which part sits at each index, as M3D_KIND. A car that is
			// destroyed next to another picks up the nearest loose part on
			// reassembly, so its three spell parts and the arcanes baked into
			// them can differ from the ones it started with - and can differ
			// between the two machines, whose loose parts scatter on their own
			// RNG. The host owns the car, so it names the parts and every
			// client rebuilds to match. 0 means "no part": a client leaves
			// that slot alone, which is also what a broken-up car reports.
			//
			// The morph/activated bits are dropped: a spell flips them on and
			// off for its own visual, they are not a different part, and a
			// client rebuilding its whole car on every morph would reset the
			// energy and features registerGeometry() resets.
			for(int i = 0; i < NET_ACTIVE_PARTS; i ++){
				cMesh* part = mr -> part_by_index(i);

				if(part)
					s -> part_kind[i] = (unsigned char)
						(M3D_KIND(part -> Type) & ~(Mechos::MORPH_KIND | Mechos::ACTIVATED_KIND));
				}

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

		count ++;
		rp = rp -> next;
		}

	if(!count) return;

	// -netdump: the authoritative state, exactly as it goes on the wire.
	net_trace_racers_wire(net_match_S -> tick(),racers,count);
	net_fx_trace(net_trace_file,net_match_S -> tick());

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
	The host's Lap/Checkpoint for a racer, when this machine is a client and
	that racer is its own car. Returns 0 for every other racer and on the
	host; the caller then keeps the racer's own stPtr values.

	Used by getCheckpointNStar/setCheckpointNStar: the checkpoint a racer is
	entitled to is read off Lap/Checkpoint, and the client's own count can sit
	a checkpoint ahead of the host's for the rest of the race. Comparing
	against the host's numbers keeps the stars on screen and the stars a pass
	is worth the same as the host's, while the local pointer is left alone so
	its passes still animate.
*/
int mchNetRacerHostStanding(const mchRacer* r,int& lap,int& checkpoint)
{
	if(mchNetHost || !mchNetGame || !net_match_S) return 0;
	if(!net_local_stand_valid) return 0;
	if(r != net_racer_at(net_match_S -> local_slot())) return 0;

	lap = net_local_stand_lap;
	checkpoint = net_local_stand_cp;

	return 1;
}

/*
	A car this client only poses: everything but its own. A remote car's lap
	and checkpoint arrive in the snapshot, and the client must not count its
	line crossings itself - its own count would be overwritten by the next
	snapshot and the two standings would flicker against each other. See
	CP_Quant, which leaves remote cars to the host.
*/
int mchNetIsRemoteRacer(const mchRacer* r)
{
	if(mchNetHost || !mchNetGame || !net_match_S) return 0;
	if(net_match_S -> local_slot() < 0) return 0;

	return r != net_racer_at(net_match_S -> local_slot());
}

/*
	Client side: correct the locally simulated car when it has really come off
	the host's copy.

	The local car has to be simulated here or a keypress would wait a round
	trip, but it is the host's copy that everyone else sees. The two do not step
	identically, yet left to themselves they stay within a few units (measured
	with matched input): the difference is frame timing and the odd collision,
	not a systematic drift. So the local sim is left completely alone in normal
	driving, which is what keeps the steering responsive.

	What it must not do is leave the car stranded away from the host, so past a
	wide threshold the host wins outright with a snap. The snap used to be the
	problem: setPosition() runs setPositionInit(), which zeroes the velocity and
	forces the car integrates from, so each snap left the car standing while the
	host drove on, it fell behind again at once, and the two were snapped over
	and over - the once-a-second jump. Resuming on the host's velocity after the
	snap is what makes it settle instead of repeating.

	An earlier version eased the car across with a small per-frame translation.
	That was worse than useless: shifting the pose without shifting the velocity
	rocked the car and drove the two copies *further* apart (measured 0-9 units
	untouched versus 0-80 with the easing), and the pull it took to hide that is
	exactly what felt like sluggish steering.

	Teleports (the trap that freezes its victim, Formicary, MassShifter) are not
	handled here: the trap freezes the local car through the synced feature and
	the arrival event drops it on the target, so the error never grows into a
	snap in the first place.
*/
static void net_reconcile_local(void)
{
	mchMechosRacer* mr;
	const float half = (float)NET_WORLD_SIZE * 0.5f;
	const float threshold = 40.0f;
	Vect3f r;
	float dx,dy,d2;

	if(!net_local_auth_valid) return;

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

	if(d2 < threshold * threshold) return;

	net_log("client: local car %.0f units from the host, snapping",
		Vect3f(dx,dy,0.0f).norm());

	mr -> setPosition(net_local_auth,net_local_auth_yaw);

	// setPosition() cleared the velocity; resume on the host's, or the car
	// sits still after the cut while the host's copy drives off.
	mr -> setGlobalVelocity(net_local_auth_vel);
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

	// -netdump: the state as it arrived, before any of it is applied. Compared
	// against the host's R lines for the same tick this is the pure wire view.
	net_trace_racers_wire(hdr.server_tick,racers,count);

	// The header's tick is the interpolation timeline. See the buffer below.
	for(int i = 0; i < count; i ++){
		mchRacer* rp = net_racer_at(racers[i].slot);
		mchMechosRacer* mr;

		if(!rp) continue;

		mr = dynamic_cast<mchMechosRacer*>(rp);

		if(!mr) continue;

		// The destroy-assemble sequence, kicked off on the transition out of
		// NORMAL. It goes to the local car too: the client drives its own car
		// but never damages it, so without this it would keep driving through
		// its own explosion on every other machine.
		mr -> net_apply_state(racers[i].mech_state);

		/*
			The host is the authority on which part sits where. Once it says
			the car is whole again, this copy is put on the host's parts: it
			may have reassembled from a different set of loose parts, and then
			its spells and their mana bars would not be the host's. Applied
			only for a whole car - a broken-up one has no parts to name - and
			it does nothing when the parts already match.

			Before the mana loop below, so a car this frame rebuilds takes the
			host's Power and Status on its new parts rather than on the meshes
			the rebuild throws away.
		*/
		if(racers[i].mech_state == Mechos::NORMAL_MECHOS){
			unsigned char before[NET_ACTIVE_PARTS];

			for(int p = 0; p < NET_ACTIVE_PARTS; p ++){
				cMesh* pm = mr -> part_by_index(p);
				before[p] = pm ? (unsigned char)M3D_KIND(pm -> Type) : 0;
				}

			if(mr -> net_sync_parts(racers[i].part_kind))
				net_log("client: slot %d parts synced %d %d %d %d -> %d %d %d %d",
					(int)racers[i].slot,
					(int)before[0],(int)before[1],(int)before[2],(int)before[3],
					(int)racers[i].part_kind[0],(int)racers[i].part_kind[1],
					(int)racers[i].part_kind[2],(int)racers[i].part_kind[3]);
			}

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

		/*
			The local car is put on the host's pose as soon as a snapshot
			arrives, and left to simulate itself until the next one. The host
			is authoritative, so it wins every 25 ms; the local simulation in
			between is what keeps a keypress instant instead of a round trip.
			net_reconcile_local only covers the case where snapshots stall.
		*/
		if(racers[i].slot == net_match_S -> local_slot()){
			net_local_auth.set(net_uq_world(racers[i].pos_x),
					   net_uq_world(racers[i].pos_y),
					   net_uq_height(racers[i].pos_z));
			net_local_auth_vel.set(net_uq_speed(racers[i].vel_x),
					       net_uq_speed(racers[i].vel_y),
					       (float)racers[i].vel_z);
			net_local_auth_yaw = net_uq_yaw(racers[i].yaw);
			net_local_auth_valid = 1;

			// The host's pose, applied now. setPositionExact holds the z the
			// host sent (a ground snap would drop a flying car every snapshot);
			// it still clears the velocity and forces the car integrates from,
			// so the host's velocity goes back on straight after - otherwise
			// the car would stall for a frame every snapshot.
			mr -> setPositionExact(net_local_auth,net_local_auth_yaw);
			mr -> setGlobalVelocity(net_local_auth_vel);
			// The same reset clears speed_avr, and the EMA that rebuilds it
			// (0.06/frame) never catches up between 25 ms snapshots - the
			// bonus pickup gate reads it, so the local car could not collect.
			mr -> setAverageSpeed(net_local_auth_vel.norm());

			// The teleport trap freezes its victim in place (and hides it)
			// while it charges, then drops it on the target. Without this the
			// local car would keep driving through the countdown and only be
			// yanked across by the reconcile at the end. Only this one bit is
			// applied today; the rest of the field is along for the ride.
			{
				int hf = (int)racers[i].features[0]
				       | ((int)racers[i].features[1] << 8)
				       | ((int)racers[i].features[2] << 16);
				int want = (hf & Mechos::STATIC_NULIFICATOR) != 0;
				int have = (mr -> features() & Mechos::STATIC_NULIFICATOR) != 0;

				if(want && !have){
					mr -> set_feature(Mechos::STATIC_NULIFICATOR);
					net_log("client: local car frozen (teleport trap)");
					}
				if(!want && have){
					mr -> reset_feature(Mechos::STATIC_NULIFICATOR);
					net_log("client: local car released");
					}
			}

			/*
				Race progress is host-authoritative, but the local pointer stays
				local. The client counts its own line crossings so a pass plays
				its animation and sound; overwriting the pointer from the host
				skipped a crossing the host's copy had already counted and the
				animation was lost. What must not stay local is the *standing*:
				the checkpoint on screen and the stars a pass is worth are read
				off Lap/Checkpoint, and a crossing the client counted a frame
				early stuck forever, leaving it a checkpoint ahead. So the
				host's Lap/Checkpoint are kept aside here and read back through
				mchNetRacerHostStanding, which getCheckpointNStar and
				setCheckpointNStar use in place of this car's own count.
			*/
			net_local_stand_lap = racers[i].lap;
			net_local_stand_cp = racers[i].checkpoint;
			net_local_stand_valid = 1;

			continue;
			}

		// Race standing, host-authoritative like the pose. Only the remote
		// cars need it: the local car computes its own as it drives, and the
		// host's copy of a remote car is the one that crossed the line.
		if(rp -> stPtr){
			rp -> stPtr -> Lap = racers[i].lap;
			rp -> stPtr -> Checkpoint = racers[i].checkpoint;
			rp -> stPtr -> place = racers[i].place;

			// The running time is stored as the total elapsed, with the local
			// segment cleared, so the table reads one number and the client's
			// own accumulation cannot add to it before the next snapshot.
			rp -> stPtr -> time = (int)racers[i].time_ms;
			rp -> stPtr -> cur_time = 0;

			rp -> stPtr -> NStar = racers[i].nstar;
			rp -> NStar = 0;
			}

		// Only the finished bit is applied. The rest of mchRacer::flags is
		// this machine's own bookkeeping - AI, active racer, net remote - and
		// copying it wholesale would change how this car is driven.
		if(racers[i].flags & MCH_FINISHED)
			rp -> flags |= MCH_FINISHED;
		else
			rp -> flags &= ~MCH_FINISHED;

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

	// On a client, note when the host's standings moved at all. The checkpoint
	// count is recomputed from these, and remote cars no longer trigger it
	// with a pass of their own (CP_Quant leaves them to the host), so this is
	// what does.
	if(!mchNetHost){
		unsigned fp = 0;

		for(int i = 0; i < count; i ++)
			fp = fp * 131u + (unsigned)racers[i].lap * 37u
			   + (unsigned)racers[i].checkpoint;

		if(fp != net_stand_fingerprint){
			net_stand_fingerprint = fp;
			net_stand_dirty = 1;
			}
		}

	// -netdump: this machine's world after the snapshot was applied. The
	// remote cars are on their interpolated pose and the local car on the
	// host's, which is what the next frame actually draws.
	net_trace_racers_world(hdr.server_tick);
	net_fx_trace(net_trace_file,hdr.server_tick);
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

// One pickup can reach the host twice: its own simulation collects the man
// a few ticks after the owner's machine already did, and both sides speak.
// The window only has to cover the replication latency (actor states are
// 10 Hz); no real second pickup of the same man fits inside it, because a
// collected man is thrown to random(2048) first.
#define NET_BONUS_DEDUP_TICKS 20 // 20 * 25 ms = 500 ms

// The pickup the host last applied per slot: its arcane id and the tick it
// ran on. Stamped by the host path of mchNetBonus, read by the report path
// of net_apply_event.
static int net_bonus_last_arcane[NET_MAX_PLAYERS];
static unsigned net_bonus_last_tick[NET_MAX_PLAYERS];

// How far the host's copy of a car may sit from the man its owner already
// collected: one snapshot of pose lag plus the man's own stride. Within it
// the report collects that very man; beyond it the effect is replayed
// without touching any body.
#define NET_BONUS_REPORT_RADIUS 300.f

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
	Host side: a client reports that its own car just collected a bonus.

	The owner's machine runs the pickup first - its local simulation of the
	car leads the host's - but the fields the snapshot carries are read from
	the host's copy. Until the pickup is replayed here, every synced value
	(part Power above all) keeps describing the pre-pickup car, which is why
	the owner's HUD showed nothing but the passive regeneration.

	The replay is unconditional - the speed and slot gates ran where the
	pickup happened - except for the dedup window: the host's own simulation
	collects the same man a few ticks later, before the actor state that
	removed it arrives, and that twin has to be dropped.
*/
static void net_apply_client_bonus(int slot,int arcane_id,mchMechosRacer* mr)
{
	BonusMan* man;
	mchBonusArcaneData* p;
	unsigned now = net_match_S -> tick();

	if(arcane_id == net_bonus_last_arcane[slot] &&
			net_bonus_last_tick[slot] && now - net_bonus_last_tick[slot] < NET_BONUS_DEDUP_TICKS){
		net_log("host: bonus %d on slot %d dropped, twin of a recent pickup",
			arcane_id,slot);
		return;
		}

	// The man the client took is still standing in the host's world.
	// Collecting it here is the whole job: the bonus lands on the host's
	// copy, and the throw to random(2048) is what replicates the vanish.
	// The radius accepts the pose lag between the two simulations.
	man = dynamic_cast<BonusMan*>(Mdisp -> searchBody(mr -> R(),Body::BONUS_MAN,mr));
	if(man && getDist(mr -> R(),man -> R()).norm() < man -> radius() + mr -> radius() + NET_BONUS_REPORT_RADIUS){
		net_log("host: slot %d reported bonus %d, collecting the man",slot,arcane_id);
		man -> collect(*mr);
		return;
		}

	// No man in reach: the pickup was of something else - a meteorite
	// bonus, a track box - or the man has already moved. The effect still
	// has to run against the host's copy, so the synced fields catch up.
	p = mchGetBonusData(arcane_id);
	if(!p){
		net_log("host: slot %d reported bonus %d, unknown arcane",slot,arcane_id);
		return;
		}

	net_log("host: slot %d reported bonus %d, replaying without the man",slot,arcane_id);
	mr -> AddBonus(p -> Type);
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

	// A bonus report is not an arcane event: there is no part to resolve
	// and no Status to check. The gates ran on the owner's machine, where
	// the pickup actually happened; this side only replays the effect.
	if(e -> type == NET_EV_BONUS){
		net_apply_client_bonus(e -> slot,e -> arcane_id,mr);
		return;
		}

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
	Host side: an arcane has latched onto a victim - a voodoo head reaching a
	racer. What the client has to draw is one head leaving its orbit and homing
	in, and it cannot work out the victim from the owner alone, so the victim's
	slot rides on target_id.

	Reliable, like a cast: a lost latch leaves the head orbiting as if it had
	missed, and the host's copy keeps damaging either way.
*/
void mchNetArcaneLatch(int ownerID,int arcane_id,int victimID)
{
	netEvent* e;
	mchRacer* rp;
	int slot = 0;
	int victim_slot = 0;

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

	rp = mch_raceD -> racerLst -> first();

	while(rp && rp -> ID != victimID){
		rp = rp -> next;
		victim_slot ++;
		}

	if(!rp) return;

	e = &net_event_q[net_event_q_count ++];

	memset(e,0,sizeof(netEvent));

	e -> type = NET_EV_ARCANE_LATCH;
	e -> slot = (unsigned char)slot;
	e -> arcane_id = (unsigned char)arcane_id;
	e -> target_id = (unsigned char)victim_slot;
}

/*
	Host side: the race is over, so every client should show the same end
	screen. The host is the only machine whose finish decides that - a client
	ends its own race only if its own car happened to finish - so it has to be
	told.

	The order and times ride along for a future results screen; nothing reads
	them yet. Reliable: a lost ending leaves a client racing a finished race.
*/
void mchNetGameOver(void)
{
	netGameOver msg;
	mchRacer* rp;
	int slot = 0;

	if(!mchNetGame || !mchNetHost) return;
	if(!net_session_S || !net_match_S || !mch_raceD) return;

	memset(&msg,0,sizeof(msg));

	rp = mch_raceD -> racerLst -> first();

	while(rp && slot < NET_MAX_PLAYERS){
		if(rp -> stPtr){
			msg.order[slot] = (unsigned char)rp -> stPtr -> place;
			msg.time[slot] = (unsigned short)(rp -> stPtr -> time / 1000);
			}
		rp = rp -> next;
		slot ++;
		}

	net_match_S -> sess() -> broadcast(NET_MSG_GAME_OVER,&msg,(unsigned int)sizeof(msg),1);
	net_log("host: race over, told the clients");
}

/*
	Host side: a racer just finished. Queue the fanfare so the clients can
	play it on their snapshot-driven copy of that car. Reliable, like the
	other events: the ending of a race is not something to drop.
*/
void mchNetRacerFinished(int ownerID)
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

	e -> type = NET_EV_RACER_FINISHED;
	e -> slot = (unsigned char)slot;
}

/*
	Net side of a pickup, both directions.

	On the host the pickup has already run against its copy, so this is the
	fanfare relay for the machines that only pose that car, plus the stamp
	that lets a twin report of the same pickup be dropped.

	On a client this is the report itself: the owner's machine runs the
	pickup for its own car - its local simulation leads the host's - but the
	host owns every replicated field of that car. Until it hears, the
	snapshot keeps describing the pre-pickup car, and the synced bars show
	nothing but the passive regeneration.
*/
void mchNetBonus(int ownerID,int arcane_id)
{
	netEvent* e;
	mchRacer* rp;
	int slot = 0;

	if(!mchNetGame) return;
	if(!net_session_S || !net_match_S || !mch_raceD) return;
	if(net_match_S -> state() != NET_MATCH_RUNNING) return;

	if(!mchNetHost){
		if(net_event_q_count >= NET_MAX_EVENT_PER_PACKET){
			net_log("client: event queue full, bonus %d dropped",arcane_id);
			return;
			}

		e = &net_event_q[net_event_q_count ++];

		memset(e,0,sizeof(netEvent));

		e -> type = NET_EV_BONUS;
		e -> slot = (unsigned char)net_match_S -> local_slot();
		e -> arcane_id = (unsigned char)arcane_id;

		net_log("client: bonus %d pickup queued",arcane_id);
		return;
		}

	if(net_event_q_count >= NET_MAX_EVENT_PER_PACKET) return;

	rp = mch_raceD -> racerLst -> first();

	while(rp && rp -> ID != ownerID){
		rp = rp -> next;
		slot ++;
		}

	if(!rp) return;

	net_bonus_last_arcane[slot] = arcane_id;
	net_bonus_last_tick[slot] = net_match_S -> tick();

	e = &net_event_q[net_event_q_count ++];

	memset(e,0,sizeof(netEvent));

	e -> type = NET_EV_BONUS;
	e -> slot = (unsigned char)slot;
	e -> arcane_id = (unsigned char)arcane_id;
}

/*
	Client side: the host says the race is over. mchEndGame is the same call
	the host makes - it only differs in skipping the single-player rewards
	when mchNetGame - and it is idempotent, so a client that already ended its
	own race does nothing here.
*/
static void net_apply_game_over(const netGameOver* msg,unsigned int len)
{
	(void)msg;

	if(mchNetHost) return;
	if(len < sizeof(netGameOver)) return;
	if(!mch_raceD) return;
	if(mchTimeMode == MCH_TIME_GAME_OVER) return;

	mchEndGame();
	net_log("client: host ended the race");
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
	   e -> type != NET_EV_ARCANE_STOP_VISUAL && e -> type != NET_EV_ARCANE_LATCH &&
	   e -> type != NET_EV_HORN && e -> type != NET_EV_RACER_FINISHED &&
	   e -> type != NET_EV_BONUS) return;
	if(e -> slot >= NET_MAX_PLAYERS) return;

	net_log("client: visual arcane %d type %d slot %d",(int)e -> arcane_id,(int)e -> type,(int)e -> slot);

	rp = mch_raceD -> racerLst -> first();

	while(rp && slot < e -> slot){
		rp = rp -> next;
		slot ++;
	}

	if(!rp || rp -> Type != MCH_RACER_TYPE_MECHOS) return;

	if(e -> type == NET_EV_HORN){
		// The honker sounds its own horn from its own controls; every other
		// machine hears it from here, at that car's position.
		if(e -> slot != net_match_S -> local_slot())
			static_cast<mchMechosRacer*>(rp) -> beep();
		return;
		}

	if(e -> type == NET_EV_RACER_FINISHED){
		// This machine poses the car and never runs its finish block, so the
		// fanfare comes from the event. The finisher's own machine already
		// played it locally.
		if(e -> slot != net_match_S -> local_slot()){
			mchNetFinishVisual(rp);
			net_log("client: racer %d finished",(int)e -> slot);
			}
		return;
		}

	if(e -> type == NET_EV_BONUS){
		// The relayed pickup ran on the host - its own, or its replay of the
		// owner's report. The picking machine already ran it locally (its own
		// car, simulated here), so only the others draw it.
		if(e -> slot != net_match_S -> local_slot()){
			mchNetBonusVisual(rp);
			net_log("client: racer %d took bonus %d",(int)e -> slot,(int)e -> arcane_id);
			}
		return;
		}

	if(e -> type == NET_EV_ARCANE_STOP_VISUAL){
		// The arcane is over. Kill the mirror visual this machine built for
		// it, and put back any feature it changed on the car.
		fxlabClientArcaneStop(e -> arcane_id,rp -> ID);
		return;
		}

	if(e -> type == NET_EV_ARCANE_INTERRUPT){
		Vect3f where(net_uq_spawn_pos(e -> pos_x),
			     net_uq_spawn_pos(e -> pos_y),
			     net_uq_spawn_pos(e -> pos_z));

		/*
			MassShifter, and the teleport trap built on it, lands its victim on
			"where". On the victim's own machine the local car is posed from
			the host, so drop it on the target here rather than letting the
			reconcile ease it across the map. Updating the reconcile target too
			keeps it from fighting the drop.

			The caster did not build this one: interrupts run only on the host,
			so unlike a cast there is nothing local to skip.
		*/
		if(e -> arcane_id == ARCANE058 && e -> slot == net_match_S -> local_slot()){
			mchMechosRacer* lm = dynamic_cast<mchMechosRacer*>(rp);

			if(lm){
				lm -> setPosition(where,lm -> psi());
				net_local_auth = where;
				net_local_auth_valid = 1;
				net_log("client: local car teleported to %d %d",where.xi(),where.yi());
				}
			}

		fxlabClientArcaneInterrupt(e -> arcane_id,rp -> ID,where);
		return;
		}

	if(e -> type == NET_EV_ARCANE_LATCH){
		// The victim is named by slot, the same way the caster is. It may be
		// gone by now; a latch onto a wrecked car is a no-op.
		mchRacer* vp = net_racer_at(e -> target_id);
		mchMechosRacer* vm = vp ? dynamic_cast<mchMechosRacer*>(vp) : NULL;

		if(vm)
			fxlabClientArcaneLatch(e -> arcane_id,rp -> ID,vm -> ID);

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

		case NET_MSG_GAME_OVER:
			net_apply_game_over((const netGameOver*)data,len);
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

/*
	Bring the session and the match layer up, once. Shared by mchNetStart - the
	command line and the -netrace path, which opens at load time - and by the
	menu lobby, which opens while the menu is still on screen. Idempotent: the
	second caller finds the session already there.
*/
static int net_boot(int host)
{
	netEndpoint* self;

	if(net_session_S) return 1;

	net_session_S = new netSession;
	net_match_S = new netMatch(net_session_S);
	net_S = net_session_S;

	// Snapshots mean nothing to the roster, so the match layer must hand them
	// on rather than read them and throw them away.
	net_match_S -> set_handler(net_game_handler,NULL);

	net_fx_set_session(net_session_S);
	net_interp_reset();

	// Zero asks for an ephemeral port, which is what a client wants; a host
	// listens on the configured one.
	if(!net_session_S -> open(host ? mchNetPort : 0)){
		mchNetStatusText = "Failed to open socket";

		delete net_match_S;
		delete net_session_S;
		net_match_S = NULL;
		net_session_S = NULL;
		net_S = NULL;
		net_fx_set_session(NULL);

		return 0;
		}

	// A host listens on the port above; the client reaches 127.0.0.1 by
	// default and whatever the menu holds. Nothing is sent yet.
	self = net_session_S -> get_peer(0);
	(void)self;

	net_log_open();
	net_trace_open();
	net_log("net race start, %s",host ? "host" : "client");

	return 1;
}

/* ------------------------------- menu lobby ------------------------------- */

/*
	The menu lobby. The session comes up while the creation screen is still
	up, so players see each other join before the race loads, instead of
	connecting after the flag drops.

	The match layer's usual auto-start is off here: the host presses start.
*/
int mchNetLobbyHost(const char* host_name,const char* game_name,int figure,const int* spells)
{
	mchNetHost = 1;

	if(!net_boot(1)) return 0;

	net_session_S -> host();
	net_match_S -> open_host(host_name && host_name[0] ? host_name : "host",
				 (unsigned char)(mchCurrentWorld >= 0 ? mchCurrentWorld : NET_RACE_WORLD),
				 (unsigned char)NET_RACE_TRACK,
				 (unsigned char)(mchNumLaps > 0 ? mchNumLaps : 5));

	net_match_S -> set_manual_start(1);

	// Long enough to cover a world load on every client.
	net_match_S -> set_start_ack_timeout(30000);

	if(game_name) net_match_S -> set_game_name(game_name);
	if(figure > 0) net_match_S -> set_local_player(host_name,figure,spells);

	mchNetStatusText = "Waiting for racers";

	return 1;
}

int mchNetLobbyJoin(const char* addr,const char* player_name,int figure,const int* spells)
{
	mchNetHost = 0;

	if(!net_boot(0)) return 0;

	if(addr && addr[0]){
		strncpy(mchNetJoinAddr,addr,sizeof(mchNetJoinAddr) - 1);
		mchNetJoinAddr[sizeof(mchNetJoinAddr) - 1] = 0;
		}

	if(player_name){
		strncpy(mchNetMenuName,player_name,sizeof(mchNetMenuName) - 1);
		mchNetMenuName[sizeof(mchNetMenuName) - 1] = 0;
		}

	net_match_S -> set_client_mechos(figure,spells);
	net_match_S -> set_defer_ack(1);
	net_match_S -> set_manual_start(1);

	net_last_connect_ms = 0;
	mchNetStatusText = "Connecting";

	return 1;
}

// Pump the session while the menu is up. The menu calls this once a frame.
void mchNetLobbyQuant(void)
{
	unsigned int now;

	if(!net_session_S || !net_match_S) return;

	now = (unsigned int)clocki();

	// A client retries the connection until the welcome hands it a slot. The
	// HELLO is reliable, so resending it on the retry is harmless and covers
	// a lost one. Not in lobby mode: there the host is not the typed address
	// but whatever the server introduces, and mchNetMenuPump dials it.
	if(!mchNetHost && !net_lobby_mode && net_match_S -> local_slot() < 0 &&
	   (int)(now - net_last_connect_ms) >= NET_CONNECT_RETRY_MS){
		net_last_connect_ms = now;

		if(net_session_S -> connect(mchNetJoinAddr,mchNetPort))
			net_match_S -> open_client(mchNetMenuName[0] ? mchNetMenuName : "player");
		}

	net_session_S -> quant();
	net_match_S -> quant();
}

void mchNetLobbyClose(void)
{
	if(!net_session_S) return;

	mchNetFinit();
}

// Host pressed start: fix the world, then hand the match its prepare, which
// tells every client to load. The race itself begins once each has loaded and
// acknowledged.
void mchNetLobbyHostStart(void)
{
	if(!net_match_S) return;

	mchNetGame = 1;
	mchNetHost = 1;

	mchNetArmRaceWorld();

	net_match_S -> set_world((unsigned char)mchCurrentWorld,
				 (unsigned char)mchCurrentTrack,
				 (unsigned char)(mchNumLaps > 0 ? mchNumLaps : 5));

	mchSplitScreenGame = 0;
	mchTurnBasedGame = 0;

	net_match_S -> force_start();

	// The race is starting; the match no longer needs advertising, and a
	// client that dialed in is already connected.
	mchNetLobbyServerHangup();
}

// Client saw the host's prepare: adopt the world the host chose so both sides
// load the same one, then leave the menu and load.
void mchNetLobbyClientArm(void)
{
	if(!net_match_S) return;

	mchNetGame = 1;
	mchNetHost = 0;

	mchCurrentWorld = net_match_S -> world_id();
	mchCurrentTrack = net_match_S -> track_id();
	mchNumLaps = net_match_S -> laps() > 0 ? net_match_S -> laps() : 5;

	mchSplitScreenGame = 0;
	mchTurnBasedGame = 0;
}

void mchNetLobbySetLocal(const char* name,int figure,const int* spells)
{
	if(net_match_S) net_match_S -> set_local_player(name,figure,spells);
}

void mchNetLobbySetGameName(const char* name)
{
	if(net_match_S && mchNetHost) net_match_S -> set_game_name(name);
}

const char* mchNetGameName(void)
{
	return net_match_S ? net_match_S -> game_name() : "";
}

int mchNetPlayerFigure(int slot)
{
	if(!net_match_S || slot < 0 || slot >= NET_MAX_PLAYERS) return 0;
	if(!net_match_S -> player(slot).is_present) return 0;

	return net_match_S -> player(slot).figure;
}

// The "figure s1 s2 s3" config the race loader wants, from the lobby roster.
// Returns 0 when that slot has no player or never picked a mechos.
int mchNetPlayerConfig(int slot,char* out,int sz)
{
	const netMatchPlayer* p;

	if(!net_match_S || !out || sz < 16) return 0;
	if(slot < 0 || slot >= NET_MAX_PLAYERS) return 0;

	p = &net_match_S -> player(slot);

	if(!p -> is_present || p -> figure <= 0) return 0;

	sprintf(out,"%d %d %d %d",p -> figure,
		p -> spells[0],p -> spells[1],p -> spells[2]);

	return 1;
}

/*
	One line in net.log naming every grid slot's mechos figure and whether a
	player or a bot drives it. The race builds its cars in slot order, so the
	list position is the slot, the same mapping mchNetPlayerConfig uses.
	Written once when the race starts, so a net game's cars can be told apart
	in the log.
*/
void mchNetLogGrid(void)
{
	mchRacerStats* rs;
	int slot = 0;
	char line[200];

	if(!mchNetGame) return;

	line[0] = 0;

	rs = mch_racerLst.first();
	while(rs){
		char item[40];
		int f = mchNetPlayerFigure(slot);

		sprintf(item,"%ss%d=f%d%s",slot ? " " : "",slot,rs -> essenceID,f > 0 ? "P" : "b");
		strcat(line,item);

		rs = rs -> next;
		slot ++;
	}

	net_log("grid %s",line);
}

/*
	The figure a bot slot should take: the first one no player holds and no
	earlier bot has taken, and that the track allows. taken_mask has bit f set
	for figure f. Returns -1 when every figure is spoken for, and the caller
	then leaves the slot with whatever it already had.
*/
int mchNetBotFigure(unsigned int taken_mask)
{
	for(int f = 0; f < acsMaxEssenceID; f ++){
		if(taken_mask & (1u << f)) continue;
		if(mch_trackD && !mch_trackD -> FigureEnabled(f)) continue;

		return f;
	}

	return -1;
}

// The host has pressed start; the client should be leaving the menu to load.
int mchNetLobbyStarted(void)
{
	if(!net_match_S) return 0;

	return net_match_S -> state() == NET_MATCH_STARTING ||
	       net_match_S -> state() == NET_MATCH_RUNNING;
}

/* --------------------------- lobby server client -------------------------- */

/*
	The rendezvous server, on top of the menu lobby.

	The session still comes up exactly as it does on a LAN - open_host or
	open_client - and the race is unchanged. What the server adds is the
	introduction: it learns each game's public UDP endpoint from a probe on
	the game's own socket, and when a client asks to join a match it hands each
	side the other's endpoint. Both then punch outward, the NATs open, and the
	ordinary HELLO/WELCOME handshake runs over the result.

	net_lobby_S owns only the TCP control channel. The probe leaves
	net_session_S's socket, so it is sent from here.
*/

// "host", "host:port" or a bare address. Returns the port, and copies the host
// into out.
static unsigned short net_lobby_split_addr(const char* addr,char* out,int out_sz)
{
	const char* colon = addr ? strrchr(addr,':') : NULL;
	unsigned short port = (unsigned short)LOBBY_DEFAULT_PORT;

	if(!addr || !addr[0]){
		strncpy(out,"127.0.0.1",out_sz - 1);
		out[out_sz - 1] = 0;
		return port;
	}

	if(colon && colon != addr){
		int n = (int)(colon - addr);
		if(n > out_sz - 1) n = out_sz - 1;
		memcpy(out,addr,n);
		out[n] = 0;
		port = (unsigned short)atoi(colon + 1);
	}
	else {
		strncpy(out,addr,out_sz - 1);
		out[out_sz - 1] = 0;
	}

	return port;
}

static void net_lobby_ensure(void)
{
	if(!net_lobby_S) net_lobby_S = new netLobbyClient;
}

// Host: open the host session as usual, then register it with the server.
int mchNetLobbyServerHost(const char* lobby_addr,const char* host_name,
			  const char* game_name,int figure,const int* spells)
{
	char host[128];
	unsigned short port;

	if(!mchNetLobbyHost(host_name,game_name,figure,spells)) return 0;

	net_lobby_mode = 1;
	net_lobby_browse = 0;
	net_lobby_joined = 0;
	net_lobby_reg_players = -1;
	net_lobby_join_id = -1;

	net_lobby_ensure();

	port = net_lobby_split_addr(lobby_addr,host,sizeof(host));
	net_lobby_S -> dial(host,port);

	return 1;
}

// Client: open the client session as usual so the game socket exists (the
// probe needs it), then connect to the server and browse.
int mchNetLobbyServerJoin(const char* lobby_addr,const char* player_name,
			  int figure,const int* spells)
{
	char host[128];
	unsigned short port;

	if(!mchNetLobbyJoin("",player_name,figure,spells)) return 0;

	// Kept for the re-connect watch: mchNetFinit frees the session these would
	// have lived in, and a retry needs them to rebuild exactly the same join.
	strncpy(net_lobby_addr_saved,lobby_addr ? lobby_addr : "",sizeof(net_lobby_addr_saved) - 1);
	net_lobby_addr_saved[sizeof(net_lobby_addr_saved) - 1] = 0;
	strncpy(net_lobby_name_saved,player_name ? player_name : "",sizeof(net_lobby_name_saved) - 1);
	net_lobby_name_saved[sizeof(net_lobby_name_saved) - 1] = 0;
	net_lobby_figure_saved = figure;
	for(int i = 0; i < 3; i ++)
		net_lobby_spells_saved[i] = spells ? spells[i] : 0;
	net_lobby_joined_ms = 0;

	net_lobby_mode = 1;
	net_lobby_browse = 1;
	net_lobby_joined = 0;
	net_lobby_host_known = 0;
	net_lobby_list_ms = 0;
	net_lobby_last_matches = -1;
	net_lobby_join_id = -1;

	net_lobby_ensure();

	port = net_lobby_split_addr(lobby_addr,host,sizeof(host));
	net_lobby_S -> dial(host,port);

	return 1;
}

void mchNetLobbyServerHangup(void)
{
	if(net_lobby_S) net_lobby_S -> close();

	net_lobby_mode = 0;
	net_lobby_browse = 0;
	net_lobby_joined = 0;
	net_lobby_host_known = 0;
	net_lobby_reg_players = -1;
	net_lobby_join_id = -1;
	net_lobby_knock_count = 0;
	net_lobby_seated_logged = 0;
	net_lobby_rebrowse = 0;
	net_lobby_last_matches = -1;
}

static void net_lobby_knock_remember(unsigned int ip,unsigned short port)
{
	for(int i = 0; i < net_lobby_knock_count; i ++){
		if(net_lobby_knock[i].ip == ip && net_lobby_knock[i].port == port){
			net_lobby_knock[i].next_ms = 0;
			return;
			}
		}

	if(net_lobby_knock_count >= NET_MAX_PLAYERS) return;

	net_lobby_knock[net_lobby_knock_count].ip = ip;
	net_lobby_knock[net_lobby_knock_count].port = port;
	net_lobby_knock[net_lobby_knock_count].next_ms = 0;
	net_lobby_knock[net_lobby_knock_count].tries = 0;
	net_lobby_knock_count ++;
}

static void net_lobby_knock_quant(unsigned int now)
{
	if(!net_session_S) return;

	for(int i = 0; i < net_lobby_knock_count; i ++){
		netLobbyKnock* k = &net_lobby_knock[i];

		if(k -> tries >= 60) continue;
		if(k -> next_ms && (int)(now - k -> next_ms) < 0) continue;

		// A few in a row: the first packet on a fresh mapping is the one most
		// likely to be dropped, on either NAT.
		for(int n = 0; n < 3; n ++)
			net_session_S -> punch(k -> ip,k -> port);

		k -> next_ms = now + 500;
		k -> tries ++;
		}
}

/*
	Client: the introduction happened but no slot followed, so the rendezvous
	did not survive the trip - the punch was dropped, or the HELLO never got
	through. Rebuild the whole connection: a fresh game socket gets a fresh
	public endpoint from the server, the host is introduced to it and punches
	it, and the ordinary handshake runs again. This is what makes an empty
	player list cure itself instead of sitting there.
*/
static void net_lobby_client_reconnect(void)
{
	int rejoin = net_lobby_join_id;
	char addr[128];
	char name[32];

	// Copy first: mchNetLobbyServerJoin writes these same saved strings back
	// from its arguments, and handing it its own buffer is an aliased copy.
	strncpy(addr,net_lobby_addr_saved,sizeof(addr) - 1);
	addr[sizeof(addr) - 1] = 0;
	strncpy(name,net_lobby_name_saved,sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;

	mchNetFinit();

	mchNetLobbyServerJoin(addr,name,
			      net_lobby_figure_saved,net_lobby_spells_saved);

	// The re-connect targets the same game, so keep the id; if it is gone by
	// now the server's "no match" reply sends this client back to the list.
	net_lobby_join_id = rejoin;
}

/*
	One frame of the server channel. Called while the network screen is up,
	whatever phase it is in, because the host registers and the client lists
	from the setup/browse phases, not only from the in-session lobby.
*/
void mchNetMenuPump(void)
{
	unsigned int now;

	if(!net_lobby_S) return;

	net_lobby_S -> pump();

	now = (unsigned int)clocki();

	// The probe, from the game's own socket. Repeated, because a lost UDP
	// probe would otherwise leave the server with no endpoint to hand out.
	if(net_lobby_S -> ready() && net_session_S){
		if((int)(now - net_lobby_probe_ms) >= 500){
			char buf[64];
			int n;

			net_lobby_probe_ms = now;
			n = snprintf(buf,sizeof(buf),"%s%s\n",
				     LOBBY_PROBE_PREFIX,net_lobby_S -> token());

			net_session_S -> send_raw(net_lobby_S -> server_ip(),
						  net_lobby_S -> server_port(),
						  buf,(unsigned int)n);
		}
	}

	// Host: keep the advertised player count current.
	if(mchNetHost && net_lobby_S -> ready() && net_match_S){
		int players = net_match_S -> num_players();

		if(players != net_lobby_reg_players){
			// The world is the menu's pick; mchCurrentWorld is only fixed
			// once the host presses start, which is later than this.
			int world = (mchNetMenuWorld >= 0) ? mchNetMenuWorld
				  : (mchCurrentWorld >= 0 ? mchCurrentWorld : NET_RACE_WORLD);

			net_lobby_reg_players = players;
			net_lobby_S -> register_match(mchNetGameName(),
						      world,
						      mchNumLaps > 0 ? mchNumLaps : 5,
						      players,NET_MAX_PLAYERS);
		}
	}

	// Client: refresh the list while browsing.
	if(!mchNetHost && net_lobby_browse && net_lobby_S -> ready()){
		if((int)(now - net_lobby_list_ms) >= 2000){
			net_lobby_list_ms = now;
			net_lobby_S -> request_list();
		}

		// Log the list whenever its shape changes, so a stale or duplicate
		// match - what a pick lands on when the host has re-created its game -
		// is visible in the log rather than only in the menu.
		if(net_lobby_S -> match_count() != net_lobby_last_matches){
			char buf[256];
			int i,n = 0,count = net_lobby_S -> match_count();

			net_lobby_last_matches = count;
			buf[0] = 0;

			for(i = 0; i < count && n < 220; i ++){
				const netLobbyMatch* m = net_lobby_S -> match(i);

				if(m) n += snprintf(buf + n,sizeof(buf) - n," %u:%s(%u)",
						    (unsigned)m -> id,m -> name,(unsigned)m -> players);
			}

			net_log("client: list now %d match(es):%s",count,buf);
		}
	}

	// Client: the pick is gone (the host re-created its game and the server no
	// longer has that id). Drop it and list again; re-asking a dead id is what
	// left the client waiting forever while the host saw nobody. Errors that
	// are worth retrying - the host has not probed yet - do not come through
	// here, so a transient one still gets its retries.
	if(!mchNetHost && net_lobby_S -> take_join_rejected()){
		net_log("client: no match %d, re-listing",net_lobby_join_id);
		net_lobby_join_id = -1;
		net_lobby_list_ms = 0;
		net_lobby_S -> request_list();

		// If the pick was made from the browser and it turned out stale, the
		// screen is already back on the list; if the client had left the
		// browser for the session, it now has no host and no seat, so ask the
		// screen to show the list again rather than sit on an empty roster.
		if(net_match_S && net_match_S -> local_slot() < 0){
			net_lobby_joined = 0;
			net_lobby_browse = 1;
			net_lobby_rebrowse = 1;
			}
	}

	// Client: the introduction is not a one-shot. A JOIN sent before this
	// game's probe reached the server - or a PEER lost on the way - would
	// otherwise strand both sides: the browser would never leave and the host
	// would never hear a HELLO. Keep asking once a second until the transport
	// has actually seated this client. Once seated, both sides see it.
	if(!mchNetHost && net_lobby_S -> ready() && net_lobby_join_id >= 0 &&
	   net_match_S && net_match_S -> local_slot() < 0){
		if((int)(now - net_lobby_join_ms) >= 1000){
			net_lobby_join_ms = now;
			net_log("client: re-asking to join match %d",net_lobby_join_id);
			net_lobby_S -> join((unsigned short)net_lobby_join_id);
		}
	}

	// Introductions.
	{
		netLobbyPeer p;

		while(net_lobby_S -> pop_peer(&p)){
			if(mchNetHost){
				// Punch outward so the client's HELLO can get in through
				// this side's NAT, and remember the endpoint so the knocking
				// continues while the client is still trying to seat.
				char ipstr[32];

				netSocketIpToString(p.ip,ipstr,sizeof(ipstr));
				net_log("lobby host: peer %s:%u, knocking",ipstr,(unsigned)p.port);

				net_lobby_knock_remember(p.ip,p.port);
			}
			else {
				char ipstr[32];

				netSocketIpToString(p.ip,ipstr,sizeof(ipstr));
				net_log("lobby client: host %s:%u",ipstr,(unsigned)p.port);

				net_lobby_host_ip = p.ip;
				net_lobby_host_port = p.port;
				net_lobby_host_known = 1;
				net_lobby_joined = 1;

				// Start the wait-for-a-slot clock when the first introduction
				// lands; the re-connect watch below reads it.
				if(!net_lobby_joined_ms)
					net_lobby_joined_ms = now;

				// Seated: no reason to keep listing matches.
				net_lobby_browse = 0;
			}
		}
	}

	// Keep knocking at every introduced endpoint until it answers.
	net_lobby_knock_quant(now);

	// Client: dial the host the server named, retrying until seated.
	if(net_lobby_mode && !mchNetHost && net_lobby_host_known &&
	   net_session_S && net_match_S && net_match_S -> local_slot() < 0){
		if((int)(now - net_last_connect_ms) >= NET_CONNECT_RETRY_MS){
			net_last_connect_ms = now;

			if(net_session_S -> connect_addr(net_lobby_host_ip,net_lobby_host_port))
				net_match_S -> open_client(mchNetMenuName[0] ? mchNetMenuName : "player");
		}
	}

	// A client that has a slot is in the lobby with everybody else. Logged
	// once, so a run that never gets there is obvious in the log.
	if(!mchNetHost && net_match_S && net_match_S -> local_slot() >= 0 && !net_lobby_seated_logged){
		net_lobby_seated_logged = 1;
		net_log("lobby client: seated in slot %d",net_match_S -> local_slot());
		}

	/*
		Client: introduced to a host, but no slot has come. The player list
		stays empty - the client does not even see itself, because it forces
		its own row only once it has a slot - and it will stay that way: the
		same socket keeps retrying the same punch. Start the whole connection
		over instead, which hands the server a fresh public endpoint and gets
		a fresh introduction.
	*/
	if(!mchNetHost && net_lobby_mode && net_lobby_joined && net_match_S &&
	   net_match_S -> local_slot() < 0 && net_lobby_joined_ms &&
	   (int)(now - net_lobby_joined_ms) >= NET_LOBBY_RECONNECT_MS){
		net_log("client: no slot in %d ms, restarting the connection",
			NET_LOBBY_RECONNECT_MS);
		net_lobby_client_reconnect();
		}
}

/* ------------------------- headless rendezvous test ----------------------- */

/*
	One frame of the rendezvous path with no menu. Returns 1 once the race may
	load: the host once at least one client is seated (or a timeout, so a test
	never hangs), the client once the host's start has arrived. Test
	scaffolding behind -netlobbyhost / -netlobbyjoin.
*/
static int net_lobby_auto_started = 0;
static unsigned int net_lobby_auto_ms = 0;

int mchNetLobbyAutoQuant(void)
{
	if(!net_lobby_auto_started){
		int sp[3] = { 1,1,1 };

		net_lobby_auto_started = 1;
		net_lobby_auto_ms = (unsigned int)clocki();
		mchNumLaps = 5;

		if(mchNetHost)
			mchNetLobbyServerHost(mchNetLobbyAutoAddr,"host","moonshine",1,sp);
		else
			mchNetLobbyServerJoin(mchNetLobbyAutoAddr,"player",1,sp);
		}

	mchNetMenuPump();

	if(!mchNetHost && mchNetLobbyMatchCount() > 0 && !mchNetLobbyClientJoined())
		mchNetLobbyBrowseJoin(0);

	mchNetLobbyQuant();

	if(mchNetHost){
		unsigned int now = (unsigned int)clocki();

		if((net_match_S && net_match_S -> num_players() >= 2) ||
		   (int)(now - net_lobby_auto_ms) > 8000){
			mchNetLobbyHostStart();
			return 1;
			}

		return 0;
		}

	if(mchNetLobbyStarted())
		mchNetLobbyClientArm();

	return mchNetGame && mchNetLobbyStarted();
}

/* ------------------------------ browse view ------------------------------- */

int mchNetLobbyBrowseState(void){ return net_lobby_S ? net_lobby_S -> state() : NET_LOBBY_IDLE; }
const char* mchNetLobbyBrowseStatus(void){ return net_lobby_S ? net_lobby_S -> status() : ""; }
int mchNetLobbyClientJoined(void){ return net_lobby_joined; }

// 1 once, if a rejected pick should put the screen back on the match list.
int mchNetLobbyTakeRebrowse(void)
{
	int r = net_lobby_rebrowse;
	net_lobby_rebrowse = 0;
	return r;
}

int mchNetLobbyMatchCount(void)
{
	return net_lobby_S ? net_lobby_S -> match_count() : 0;
}

const char* mchNetLobbyMatchName(int index)
{
	const netLobbyMatch* m = net_lobby_S ? net_lobby_S -> match(index) : NULL;
	return m ? m -> name : "";
}

int mchNetLobbyMatchId(int index)
{
	const netLobbyMatch* m = net_lobby_S ? net_lobby_S -> match(index) : NULL;
	return m ? (int)m -> id : -1;
}

int mchNetLobbyMatchPlayers(int index)
{
	const netLobbyMatch* m = net_lobby_S ? net_lobby_S -> match(index) : NULL;
	return m ? (int)m -> players : 0;
}

int mchNetLobbyMatchMax(int index)
{
	const netLobbyMatch* m = net_lobby_S ? net_lobby_S -> match(index) : NULL;
	return m ? (int)m -> max_players : 0;
}

int mchNetLobbyMatchWorld(int index)
{
	const netLobbyMatch* m = net_lobby_S ? net_lobby_S -> match(index) : NULL;
	return m ? (int)m -> world : -1;
}

void mchNetLobbyBrowseRefresh(void)
{
	net_lobby_list_ms = 0;
	if(net_lobby_S) net_lobby_S -> request_list();
}

void mchNetLobbyBrowseJoin(int index)
{
	const netLobbyMatch* m = net_lobby_S ? net_lobby_S -> match(index) : NULL;
	if(m && !net_lobby_joined){
		// Remember the pick: mchNetMenuPump re-sends it once a second until
		// the server introduces the host and the session seats this client.
		net_lobby_join_id = (int)m -> id;
		net_lobby_join_ms = (unsigned int)clocki();
		net_log("client: asking to join match %d",net_lobby_join_id);
		net_lobby_S -> join(m -> id);
	}
}

void mchNetStart(void)
{
	int fresh = 0;

	if(!net_session_S){
		fresh = 1;

		if(!net_boot(mchNetHost)) return;
		}

	net_S = net_session_S;
	net_match_S -> set_handler(net_game_handler,NULL);
	net_fx_set_session(net_session_S);
	net_interp_reset();

	if(fresh){
		if(mchNetHost){
			net_session_S -> host();
			net_match_S -> open_host(mchNetMenuName[0] ? mchNetMenuName : "host",
						 (unsigned char)mchCurrentWorld,
						 (unsigned char)mchCurrentTrack,
						 (unsigned char)(mchNumLaps > 0 ? mchNumLaps : 5));

			net_log("host on port %d, racers %d",mchNetPort,mchNumRacers);
			}
		else {
			// The first attempt happens in mchNetQuant(), so a client that
			// starts before the host comes up retries instead of failing.
			net_last_connect_ms = 0;

			net_log("client joining %s:%d",mchNetJoinAddr,mchNetPort);
			}
		}

	// The menu lobby opened the session and the match already. A client that
	// got here has its world loaded, which is exactly what its deferred start
	// acknowledgement was waiting for.
	if(!mchNetHost && net_match_S -> state() == NET_MATCH_STARTING)
		net_match_S -> notify_loaded();

	mchNetStatusText = mchNetHost ? "Waiting for racers" : "Connecting";
}

void mchNetFinit(void)
{
	// Say goodbye before the socket goes: a client's slot frees on the host
	// the moment this lands, instead of when the peer times out. A no-op on
	// the host and when the client is not connected.
	if(net_match_S) net_match_S -> leave();

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
	net_grid_logged = 0;
	net_rnd_seeded = 0;
	net_motion_have = 0;
	net_local_auth_valid = 0;
	net_local_stand_valid = 0;
	net_stand_fingerprint = 0;
	net_stand_dirty = 0;
	memset(net_horn_last,0,sizeof(net_horn_last));
	net_input_sent_count = 0;
	net_input_recv_count = 0;
	net_event_q_count = 0;

	mchNetStatusText = "";

	if(net_lobby_S){
		delete net_lobby_S;
		net_lobby_S = NULL;
	}

	net_lobby_mode = 0;
	net_lobby_browse = 0;
	net_lobby_joined = 0;
	net_lobby_host_known = 0;
	net_lobby_reg_players = -1;
	net_lobby_join_id = -1;
	net_lobby_knock_count = 0;
	net_lobby_seated_logged = 0;
	net_lobby_rebrowse = 0;
	net_lobby_last_matches = -1;
	net_lobby_joined_ms = 0;

	net_log("net race end");
	net_trace_close();
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
			int horn = (in.buttons & NET_BTN_HORN) ? 1 : 0;
			netEvent* e;

			// The host's own copy sounds one beep per press (apply_control
			// tracks the edge), and every other client should hear exactly
			// that, so the relay fires on the edge too.
			if(horn && !net_horn_last[slot] && net_event_q_count < NET_MAX_EVENT_PER_PACKET){
				e = &net_event_q[net_event_q_count ++];
				memset(e,0,sizeof(netEvent));
				e -> type = NET_EV_HORN;
				e -> slot = (unsigned char)slot;
				}

			net_horn_last[slot] = horn;

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
		int ai = (rp -> flags & MCH_AI) != 0;

		if(slot == NET_SLOT_HOST){
			// The host's own car is keyboard driven unless -netbots asks for
			// an unattended field, in which case it is a bot too.
			if(mchNetBots){
				if(!ai) mch_raceD -> AI_Toggle(rp,1);
				rp -> set_net_control(0);
				}
			}
		else {
			int taken = net_match_S -> player(slot).is_present;

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

	memset(&in,0,sizeof(in));

	if(mchNetBots){
		// Test scaffolding: the sample is the AI's decision, captured by
		// control() this frame. Sending it and applying it through
		// set_net_input() is exactly what a player's input does, so the host
		// reproduces the bot's driving from the packet alone.
		float br,bt;

		rp -> get_bot_decision(br,bt);
		in.traction = net_input_quantize(bt);
		in.rudder = net_input_quantize(br);

		rp -> set_net_input(in);
		}
	else {
		mechos = dynamic_cast<mchMechosRacer*>(rp);
		if(!mechos) return;

		mechos -> read_input(in);

		// -netdrive: pretend the player is holding down forward and turning.
		// The route to the host is exactly the real one from here on.
		if(mchNetAutoDrive){
			in.traction = 127;
			in.rudder = 48;
			}
		}

	in.tick = net_match_S -> tick();

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

	if(!mchNetHost && !net_client_ready &&
	   (net_match_S -> state() == NET_MATCH_NONE || net_match_S -> state() == NET_MATCH_LOBBY)){
		if((int)(now - net_last_connect_ms) >= NET_CONNECT_RETRY_MS){
			net_last_connect_ms = now;

			if(net_session_S -> connect(mchNetJoinAddr,mchNetPort))
				net_match_S -> open_client(mchNetMenuName[0] ? mchNetMenuName : "player");
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
			// AI is re-evaluated every frame in the host branch below; only
			// the actor resend needs the roster change itself.

			// A client that has just joined has never seen the world's
			// actors: their spawns went out before it was there. Send them
			// all again so the newcomer's track is not empty.
			net_fx_resend_actors();
			}

		net_log("%s: roster now %d player(s), state %d",
			mchNetHost ? "host" : "client",players,net_match_S -> state());
		}

	/*
		Once, on both machines, when the race is running: by now the race has
		built its cars and both logs are open, so one line names the mechos
		figure every slot actually got and whether a player drives it.
	*/
	if(!net_grid_logged && net_match_S -> state() == NET_MATCH_RUNNING){
		net_grid_logged = 1;
		mchNetLogGrid();
	}

	if(mchNetHost){
		// Re-evaluated every frame, not only when the roster count changes:
		// a client that connects just as the race starts can otherwise leave
		// its car under AI for the first seconds, driving away from the car
		// the client is already steering. Idempotent - it only toggles on a
		// real change.
		net_update_ai();
		net_apply_host_inputs();


		// Snapshots only exist on the host: it is the only machine that knows
		// where anything is. The actors go the same way, and only from here:
		// a pass before the match is running would spawn them to nobody.
		if(net_match_S -> state() == NET_MATCH_RUNNING){
			// -netdestroy: wreck two adjacent bot slots once, a moment after
			// the start, so the destroy-assemble path runs unattended. They
			// die on the same spot and reassemble from the same pile, which is
			// what makes their parts (and arcanes) mix.
			if(mchNetDestroy){
				static unsigned net_destroy_tick = 0;

				if(!net_destroy_tick)
					net_destroy_tick = net_match_S -> tick() + 120;	// ~3 s
				else if(net_match_S -> tick() >= net_destroy_tick){
					net_destroy_tick = 0xffffffffu;

					for(int s = 2; s <= 3; s ++){
						mchRacer* drp = net_racer_at(s);
						mchMechosRacer* dmr = drp ? dynamic_cast<mchMechosRacer*>(drp) : NULL;

						if(dmr){
							dmr -> damage(1000000.0f);
							net_log("host: -netdestroy wrecked slot %d",s);
							}
						}
					}
				}

			net_send_snapshot();
			net_fx_update_actors();
			}

		if(net_match_S -> state() != NET_MATCH_LOBBY)
			mchNetStatusText = "Racing";
		}

	if(!mchNetHost && net_match_S -> local_slot() >= 0 && !net_client_ready){
		int local = net_match_S -> local_slot();
		mchRacer* rp = net_racer_at(local);
		int slot = 0;

		net_client_ready = 1;

		net_log("client: assigned slot %d",local);

		// The client drives its own slot and nothing else. setActiveRacer also
		// points the camera and the control config at it.
		if(rp)
			mch_raceD -> setActiveRacer(rp);

		/*
			Every remote racer is pose-driven from the host and marked
			net-controlled so its own physics does not also steer it - two
			simulators on one car is jitter and a pose that disagrees with the
			host. The local car is otherwise simulated here from the keyboard.

			With -netbots the local car is the exception the other way: it runs
			the AI to decide (AI_Toggle on, seeds kept), but its steering is
			captured and sent as an input sample, then applied through
			net_control on both machines. See net_send_client_input.
		*/
		rp = mch_raceD -> racerLst -> first();

		while(rp){
			if(mchNetBots && slot == local){
				if(!(rp -> flags & MCH_AI))
					mch_raceD -> AI_Toggle(rp,1);
				rp -> set_bot_input(1);
				}
			else {
				mch_raceD -> AI_Toggle(rp,0);

				if(slot != local)
					rp -> set_net_control(1);
				}

			rp = rp -> next;
			slot ++;
			}
		}

	if(!mchNetHost && net_client_ready)
		net_send_client_input();

	// The host's standings moved, so the stars on the checkpoint the local car
	// is heading to may have changed. Remote cars do not drive this any more
	// (their own pass on a client is not simulated), so it is done here, once
	// per change rather than once per snapshot.
	if(!mchNetHost && net_stand_dirty){
		net_stand_dirty = 0;

		if(mch_raceD && mch_raceD -> activeRacer)
			mch_raceD -> setCheckpointNStar();
		}

	/*
		A net race cannot be paused, by VK_PAUSE or by anything else.

		The host is the only simulation there is, so a freeze on any machine
		stops the snapshots every machine draws from; a frozen client also
		stops answering. Whatever set mchFreezeTime during a running race is
		undone here, before the simulation step that would honour it.

		Before the start the freeze is deliberate: it is how the start tick
		holds every car still - so only a running match is unpaused.
	*/
	if(net_match_S -> state() == NET_MATCH_RUNNING && mchFreezeTime){
		mchFreezeTime = 0;
		mchResumeSound();
		net_log("%s: ignored a pause during the race",mchNetHost ? "host" : "client");
		}

	/* ---------------------------- start tick ---------------------------- */

	// The start moment comes from the host: mchSetTimeMode is called when the
	// agreed tick says so, and never from the local start counter.
	if(net_match_S -> state() == NET_MATCH_RUNNING &&
	   net_match_S -> ms_until_start() <= 0 &&
	   mchTimeMode != MCH_TIME_RUNNING){
		net_log("start tick %u reached, time running",net_match_S -> start_tick());
		net_log("%s: race world %d \"%s\"",
			mchNetHost ? "host" : "client",mchCurrentWorld,
			mch_trackD ? mch_trackD -> TrackName(mchCurrentWorld,0) : "?");

		mchSetTimeMode(MCH_TIME_RUNNING);
		mchNetStatusText = "Racing";
		}

	if(net_match_S -> state() == NET_MATCH_STARTING && !net_start_logged){
		net_start_logged = 1;
		net_log("start handshake, tick to be %u",net_match_S -> start_tick());
		}
	}
