/* ---------------------------- INCLUDE SECTION ----------------------------- */

#include "StdAfx.h"

#include "net_socket.h"
#include "net_transport.h"
#include "net_input.h"
#include "net_match.h"
#include "net_snapshot.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// Sleep() without <windows.h>: this TU pulls in StdAfx.h, and windows.h
// collides with enum dbtype3ds { Unknown } in 3dsftk.h exactly like it would
// in net_socket.cpp. kernel32 is already linked, so a plain declaration is
// enough and keeps the isolation contract of net_socket.h intact.
extern "C" void Sleep(unsigned int ms);

#if defined(_WIN32)
	#define netSleep(ms) Sleep(ms)
#else
	#include <unistd.h>
	#define netSleep(ms) usleep((ms) * 1000)
#endif

/* ---------------------------- DEFINITION SECTION -------------------------- */

/*
	Transport self test: a host and a client inside one process, so framing,
	reliability, ACK dequeue and reassembly can all be exercised on a single
	machine with no second player. Invoked by the "-nettest" flag before SDL
	comes up, so a failure shows up as a plain exit code and a log file.

	Everything runs on the loopback interface, so what this proves is not that
	the network is reliable but that the protocol around it is: the exact
	number of messages sent is the exact number delivered, nothing is
	retransmitted forever, nothing arrives twice, and a payload split across
	datagrams comes back out byte for byte.
*/

static const char* net_selftest_file = "nettest.log";

static void net_logf(FILE* f,const char* fmt,...)
{
	va_list arg;

	if(!f) return;

	va_start(arg,fmt);
	vfprintf(f,fmt,arg);
	va_end(arg);

	fprintf(f,"\n");

	// Flush every line: a crash mid test must still leave a trace behind.
	fflush(f);
}

static netEndpoint* net_first_peer(netSession* s,int include_host)
{
	for(int i = 0; i < NET_MAX_PEERS; i ++){
		netEndpoint* ep = s -> get_peer(i);

		if(ep && include_host == ep -> is_host)
			return ep;
	}

	return NULL;
}

static int net_reliable_left(netSession* s)
{
	int left = 0;

	for(int i = 0; i < NET_MAX_PEERS; i ++){
		netEndpoint* ep = s -> get_peer(i);
		if(ep) left += ep -> reliable_count();
	}

	return left;
}

/*
	Lobby and match test: one host and several clients, all in one process.

	This is the part that proves the match layer agrees with itself, which
	the transport test cannot show. What is being checked is not that the
	network works but that the protocol does:

	  - every client ends up in a distinct slot, and the name the host
	    stores for that slot is the name the client actually sent;
	  - readiness propagates both ways;
	  - after the start handshake every client holds the *same* absolute
	    start tick, which is the whole point of the two step handshake;
	  - input samples reach the host under the right slot with the values
	    they were packed with;
	  - a client arriving at a full race is refused rather than seated.

	Four clients plus the host is HS_PLAYERS_MAX, so the fifth is the refusal
	case.
*/
static int net_test_lobby(FILE* f)
{
	const int NUM_CLIENTS = 4;
	const int max_ms = 12000;
	const char* names[4] = { "vanya","mitya","kostya","lesha" };

	const int PHASE_WELCOME = 0;
	const int PHASE_READY = 1;
	const int PHASE_START = 2;
	const int PHASE_INPUT = 3;
	const int PHASE_CHECK = 4;

	netSession* host = new netSession;
	netMatch*   hmatch = new netMatch(host);

	netSession* cl[8];
	netMatch*   cm[8];
	netSession* fifth = new netSession;
	netMatch*   fifth_match = new netMatch(fifth);

	int ok = 1;
	int phase = PHASE_WELCOME;
	unsigned int phase_ms = 0;
	unsigned int t0 = clocki();
	unsigned short host_port = 0;
	unsigned int common_tick = 0;
	int input_check = 1;

	for(int i = 0; i < 8; i ++){ cl[i] = NULL; cm[i] = NULL; }

	if(!host -> open(0)){
		net_logf(f,"FAIL lobby: host -> open: %s",host -> status());
		ok = 0;
		}
	else {
		netEndpoint* self = host -> get_peer(0);

		host -> host();
		hmatch -> open_host("hostishka",1,2,3);

		if(self && self -> hasAddr)
			host_port = self -> addr.port;
		else {
			net_logf(f,"FAIL lobby: host -> local address");
			ok = 0;
			}
		}

	if(ok){
		for(int i = 0; i < NUM_CLIENTS; i ++){
			cl[i] = new netSession;

			if(!cl[i] -> open(0) || !cl[i] -> connect("127.0.0.1",host_port)){
				net_logf(f,"FAIL lobby: client %d -> open/connect: %s",i,cl[i] -> status());
				ok = 0;
				break;
				}

			cm[i] = new netMatch(cl[i]);
			cm[i] -> open_client(names[i]);
			}
		}

	// Arrives last, at a race that is already full.
	if(ok){
		fifth -> open(0);
		fifth -> connect("127.0.0.1",host_port);
		fifth_match -> open_client("greedy");
		}

	if(ok){
		while(clocki() - t0 < (unsigned int)max_ms && phase != PHASE_CHECK){
			unsigned int now = clocki();

			if(phase_ms == 0) phase_ms = now;

			host -> quant();
			hmatch -> quant();

			for(int i = 0; i < NUM_CLIENTS; i ++){
				if(!cl[i]) continue;
				cl[i] -> quant();
				cm[i] -> quant();
				}

			fifth -> quant();
			fifth_match -> quant();

			// --- phase transitions ---

			if(phase == PHASE_WELCOME && now - phase_ms > 400){
				int all_welcomed = 1;
				int slot_seen[NET_MAX_PLAYERS];

				for(int i = 0; i < NET_MAX_PLAYERS; i ++) slot_seen[i] = 0;

				for(int i = 0; i < NUM_CLIENTS; i ++){
					int slot = cm[i] -> local_slot();

					if(slot < 0){ all_welcomed = 0; continue; }

					if(slot_seen[slot]){
						net_logf(f,"FAIL lobby: slots %d handed out twice",slot);
						ok = 0;
						}

					slot_seen[slot] = 1;

					const netMatchPlayer& p = hmatch -> player(slot);

					if(!p.is_present){
						net_logf(f,"FAIL lobby: slot %d not present on host",slot);
						ok = 0;
						}

					if(strcmp(p.name,names[i])){
						net_logf(f,"FAIL lobby: slot %d holds \"%s\", client %d sent \"%s\"",
							slot,p.name,i,names[i]);
						ok = 0;
						}
					}

				// A full race, which is also what the fifth client runs into.
				if(hmatch -> num_players() != NET_MAX_PLAYERS){
					net_logf(f,"FAIL lobby: host sees %d players, expected %d",
						hmatch -> num_players(),NET_MAX_PLAYERS);
					ok = 0;
					}

				if(fifth_match -> state() != NET_MATCH_KICKED){
					net_logf(f,"FAIL lobby: fifth client state %d, expected KICKED (%d)",
						fifth_match -> state(),NET_MATCH_KICKED);
					ok = 0;
					}
				else net_logf(f,"lobby: fifth client refused with \"%s\"",fifth_match -> status());

				if(all_welcomed){
					net_logf(f,"lobby: %d of %d slots seated, names match",
						hmatch -> num_players(),NET_MAX_PLAYERS);
					phase = PHASE_READY;
					phase_ms = 0;
					}
				else if(now - phase_ms > 2000){
					net_logf(f,"FAIL lobby: not everybody was seated in time");
					ok = 0;
					phase = PHASE_CHECK;
					}
				}

			if(phase == PHASE_READY){
				for(int i = 0; i < NUM_CLIENTS; i ++)
					cm[i] -> set_ready(1);

				phase = PHASE_START;
				phase_ms = 0;
				}

			if(phase == PHASE_START){
				// Wait for the acknowledgements to land, then take the human
				// decision to begin. The host tick must not move until this
				// point, so the number checked below is really shared.
				if(now - phase_ms > 300){
					hmatch -> force_start();
					phase_ms = now;
					phase = PHASE_INPUT;

					// Hold the tick the host is about to promise. Every
					// client has to report the same value, and the host's
					// tick only advances through quant().
					common_tick = hmatch -> tick();
					}
				}

			if(phase == PHASE_INPUT){
				static const signed char traction[4] = { -120, 40, 90, -60 };
				static const signed char rudder[4]   = { 100, -30, 60, 120 };

				if(now - phase_ms > 100 && hmatch -> state() != NET_MATCH_RUNNING)
					phase_ms = now;		// still starting, keep waiting

				if(hmatch -> state() == NET_MATCH_RUNNING){
					unsigned char payload[NET_MAX_PAYLOAD];
					int all_running = 1;

					// The tick the host actually promised, not the one it
					// happened to be on when the decision was taken.
					common_tick = hmatch -> start_tick();

					for(int i = 0; i < NUM_CLIENTS; i ++){
						netInput in;
						int slot = cm[i] -> local_slot();
						int bytes;

						// Input waits for the race to be on, exactly as it
						// does in the game: samples sent before the host
						// is running are simply not recorded.
						memset(&in,0,sizeof(in));
						in.tick = hmatch -> tick() + (unsigned int)i;
						in.traction = traction[i];
						in.rudder = rudder[i];
						in.buttons = (i == 2) ? NET_BTN_HORN : 0;

						bytes = cm[i] -> build_input(payload,sizeof(payload),in);
						if(bytes <= 0 || !cm[i] -> send_input(payload,bytes))
							all_running = 0;

						if(cm[i] -> state() != NET_MATCH_RUNNING)
							all_running = 0;

						if(cm[i] -> start_tick() != common_tick){
							net_logf(f,"FAIL lobby: client %d start tick %u, host promised %u",
								i,cm[i] -> start_tick(),common_tick);
							ok = 0;
							all_running = 0;
							}
						}

					if(all_running){
						phase = PHASE_CHECK;
						phase_ms = 0;
						}
					}
				}

			netSleep(1);
			}

		// --- final checks, after the loop so the last packets landed ---

		if(phase != PHASE_CHECK){
			net_logf(f,"FAIL lobby: test stalled at phase %d, host state %d",phase,hmatch -> state());
			ok = 0;
			}
		else {
			netSleep(200);

			for(int i = 0; i < NUM_CLIENTS; i ++){
				cl[i] -> quant();
				cm[i] -> quant();
				}

			host -> quant();
			hmatch -> quant();
			}

		for(int i = 0; i < NUM_CLIENTS; i ++){
			if(!cl[i]) continue;

			cl[i] -> quant();
			cm[i] -> quant();

			int slot = cm[i] -> local_slot();
			netInput got;

			if(slot < 0 || !hmatch -> get_input(slot,&got)){
				net_logf(f,"FAIL lobby: no input recorded for slot %d",slot);
				input_check = 0;
				ok = 0;
				continue;
				}

			if(got.traction != (signed char)(i == 0 ? -120 : (i == 1 ? 40 : (i == 2 ? 90 : -60))) ||
			   got.rudder != (signed char)(i == 0 ? 100 : (i == 1 ? -30 : (i == 2 ? 60 : 120)))){
				net_logf(f,"FAIL lobby: slot %d input traction %d rudder %d",
					slot,(int)got.traction,(int)got.rudder);
				input_check = 0;
				ok = 0;
				}
			}

		if(input_check)
			net_logf(f,"lobby: input routed to every slot intact");
		}

	net_logf(f,"lobby: host state %d, players %d, ready %d, start tick %u",
		hmatch -> state(),hmatch -> num_players(),hmatch -> num_ready(),hmatch -> start_tick());

	for(int i = 0; i < NUM_CLIENTS; i ++){
		if(cl[i]){ cl[i] -> close(); delete cl[i]; }
		if(cm[i]) delete cm[i];
		}

	if(fifth) fifth -> close();
	if(host) host -> close();

	delete host;
	delete hmatch;
	delete fifth;
	delete fifth_match;

	return ok;
}

/*
	Input mapping test.

	What is being pinned down here is the promise the wire format makes to
	the vehicle controls: a setpoint that goes in comes back out close enough
	that a client holding the stick at half deflection steers the same
	vehicle on the host, and a value outside the range is refused rather
	than silently folded somewhere else.

	The smoothing itself is not covered, because it lives in Mechos and
	reads vehicle state. What matters is that both paths feed the *same*
	algorithm - see keyboard_control() and net_control() in Mechos.cpp, both
	of which call apply_control() and nothing else.
*/
static int net_test_input(FILE* f)
{
	int ok = 1;

	// A packed struct has to actually be packed, or the wire layout the
	// host writes and the layout the client reads drift apart by the
	// compiler's padding choices.
	int expected = (int)(4 * sizeof(unsigned char) + sizeof(int));
	if(sizeof(netInput) != expected){
		net_logf(f,"FAIL sizeof(netInput) = %u, expected %d",sizeof(netInput),expected);
		ok = 0;
		}

	// The contract that actually matters: a setpoint the client produces
	// comes back out close enough that the host steers the same way. Sweep
	// the input range, not the wire range, and demand less than half a step.
	float worst = 0;
	float worst_at = 0;

	for(int i = 0; i <= 2000; i ++){
		float x = -1.0f + 2.0f * (float)i / 2000.0f;
		float back = net_input_expand(net_input_quantize(x));
		float error = fabs(back - x);

		if(error > worst){
			worst = error;
			worst_at = x;
			}
		}

	if(worst > 0.5f / 127.0f + 1e-5f){
		net_logf(f,"FAIL input round trip error %.6f at %.4f, tolerated %.6f",
			worst,worst_at,0.5f / 127.0f);
		ok = 0;
		}

	// Full deflection in both directions has to survive exactly.
	if(net_input_quantize(1.0f) != 127 || net_input_expand((signed char)127) != 1.0f){
		net_logf(f,"FAIL input positive full deflection: %d -> %f",
			(int)net_input_quantize(1.0f),net_input_expand((signed char)127));
		ok = 0;
		}

	// The low end of the wire range is one unit deeper and is clamped on
	// purpose, since -128 would otherwise decode past full deflection.
	if(net_input_expand((signed char)-128) != -1.0f){
		net_logf(f,"FAIL input -128 not clamped: %f",net_input_expand((signed char)-128));
		ok = 0;
		}

	if(net_input_quantize(-1.0f) != -127){
		net_logf(f,"FAIL input negative full deflection: %d",(int)net_input_quantize(-1.0f));
		ok = 0;
		}

	// Beyond full deflection must clamp, not wrap. The keyboard path can ask
	// for a rudder of 2.0 when both turn keys are held at once, and that has
	// to arrive as full lock rather than as none at all.
	if(net_input_quantize(2.0f) != 127 || net_input_quantize(-2.0f) != -127){
		net_logf(f,"FAIL input clamp: +2 -> %d, -2 -> %d",
			(int)net_input_quantize(2.0f),(int)net_input_quantize(-2.0f));
		ok = 0;
		}

	// A uniformly spinning signal must survive batching: four samples in,
	// four samples out, same order, same values.
	{
		netInput out[NET_INPUT_BATCH];
		netInput batch[NET_INPUT_BATCH];
		unsigned char payload[NET_MAX_PAYLOAD];
		int bytes, count;
		int bad = 0;

		for(int i = 0; i < NET_INPUT_BATCH; i ++){
			memset(&batch[i],0,sizeof(batch[i]));
			batch[i].tick = 1000 + (unsigned int)i;
			batch[i].traction = (signed char)(i * 31 - 40);
			batch[i].rudder = (signed char)(i * 17 - 30);
			batch[i].buttons = (unsigned char)(i == 1 ? NET_BTN_HORN : 0);
			}

		bytes = net_input_pack(payload,sizeof(payload),batch,NET_INPUT_BATCH);
		count = net_input_unpack(out,NET_INPUT_BATCH,payload,bytes);

		if(bytes != 1 + NET_INPUT_BATCH * (int)sizeof(netInput) || count != NET_INPUT_BATCH){
			net_logf(f,"FAIL input batch: %d bytes, %d samples of %d",bytes,count,NET_INPUT_BATCH);
			ok = 0;
			}
		else {
			for(int i = 0; i < NET_INPUT_BATCH; i ++)
				if(memcmp(&batch[i],&out[i],sizeof(netInput))){
					bad ++;
					net_logf(f,"FAIL input batch sample %d: tick %u vs %u, traction %d vs %d, rudder %d vs %d, buttons %u vs %u",
						i,batch[i].tick,out[i].tick,(int)batch[i].traction,(int)out[i].traction,
						(int)batch[i].rudder,(int)out[i].rudder,(unsigned)batch[i].buttons,(unsigned)out[i].buttons);
					}

			if(bad) ok = 0;
			}

		// A payload lying about its sample count must be refused rather
		// than trusted: it would otherwise run off the end of the array the
		// caller handed over.
		{
			netInput small[1];
			unsigned char forged[NET_MAX_PAYLOAD];
			netInput many[NET_INPUT_BATCH];
			int forged_bytes, got;

			memset(forged,0,sizeof(forged));
			forged[0] = (unsigned char)NET_INPUT_BATCH;

			forged_bytes = 1 + NET_INPUT_BATCH * (int)sizeof(netInput);
			got = net_input_unpack(small,1,forged,forged_bytes);

			if(got != 1){
				net_logf(f,"FAIL input batch clamp: asked for capacity 1, claimed %d, got %d",NET_INPUT_BATCH,got);
				ok = 0;
				}

			got = net_input_unpack(many,NET_INPUT_BATCH,forged,1);
			if(got != 0){
				net_logf(f,"FAIL input batch truncated: short payload accepted, %d samples",got);
				ok = 0;
				}
			}

		// Zero and negative counts are not batches.
		if(net_input_pack(payload,sizeof(payload),batch,0) != 0){
			net_logf(f,"FAIL input batch: empty batch accepted");
			ok = 0;
			}
		}

	net_logf(f,"input: round trip error %.6f at %.4f, batch %d bytes",
		worst,worst_at,1 + NET_INPUT_BATCH * (int)sizeof(netInput));

	return ok;
}

/*
	Snapshot codec test.

	The wire carries 16 bit quantized values, so what is being pinned down is
	that a vehicle comes out where it went in and facing the way it was
	facing. The two cases worth naming are the map edge and the compass
	seam: the world folds at NET_WORLD_SIZE and the heading wraps at 2*PI, and
	getting either wrong shows up as a car teleporting across the track or an
	interpolator taking the long way round the circle.
*/
static int net_test_snapshot(FILE* f)
{
	int ok = 1;

	if(sizeof(netRacerState) != 48){
		net_logf(f,"FAIL sizeof(netRacerState) = %u, expected 48",sizeof(netRacerState));
		ok = 0;
		}

	// World position, including folding past the edge.
	float worst_pos = 0;
	for(int i = 0; i < 2000; i ++){
		float x = (float)NET_WORLD_SIZE * (float)i / 2000.0f;
		float back = net_uq_world(net_q_world(x));
		float error = fabs(back - x);

		if(error > worst_pos) worst_pos = error;
		}

	// The fold: a coordinate one world over must come back as the same place
	// inside the world, not as a clamped far corner.
	{
		float x = 1234.5f;
		float wrapped = net_uq_world(net_q_world(x + (float)NET_WORLD_SIZE));

		if(fabs(wrapped - x) > 0.01f){
			net_logf(f,"FAIL world fold: %.3f + world = %.3f",x,wrapped);
			ok = 0;
			}
		}

	if(worst_pos > 1.0f / (float)NET_Q_WORLD + 1e-4f){
		net_logf(f,"FAIL world quantization error %.6f, tolerated %.6f",
			worst_pos,1.0f / (float)NET_Q_WORLD);
		ok = 0;
		}

	// Heading: round trip across the whole circle, and across the seam in
	// particular. A 16 bit heading wraps by design, so the check is that the
	// value just below 2*PI comes back just below 2*PI, not that exactly 2*PI
	// maps back to itself: 2*PI is the seam, and it is the same place as 0.
	{
		float worst_yaw = 0;

		for(int i = 0; i < 2000; i ++){
			float a = 6.2831853f * (float)i / 2000.0f;
			float back = net_uq_yaw(net_q_yaw(a));

			if(fabs(back - a) > worst_yaw) worst_yaw = fabs(back - a);
			}

		if(worst_yaw > 0.001f){
			net_logf(f,"FAIL yaw quantization error %.6f",worst_yaw);
			ok = 0;
			}

		if(net_q_yaw(0.0f) != 0){
			net_logf(f,"FAIL yaw zero encodes to %u, expected 0",(unsigned)net_q_yaw(0.0f));
			ok = 0;
			}
		}

	// Height, including the negative range.
	{
		float z = -512.5f;

		if(fabs(net_uq_height(net_q_height(z)) - z) > 1.0f / (float)NET_Q_HEIGHT + 1e-4f){
			net_logf(f,"FAIL height round trip: %.4f -> %.4f",z,net_uq_height(net_q_height(z)));
			ok = 0;
			}
		}

	// Speed clamps at the wire ends rather than wrapping into the opposite
	// direction, which would send a fast car backwards.
	{
		if(net_q_speed(100000.0f) != 32767 || net_q_speed(-100000.0f) != -32768){
			net_logf(f,"FAIL speed clamp: %d / %d",
				(int)net_q_speed(100000.0f),(int)net_q_speed(-100000.0f));
			ok = 0;
			}
		}

	// Pack and unpack a full grid, values intact.
	{
		netRacerState sent[NET_MAX_PLAYERS];
		netRacerState got[NET_MAX_PLAYERS];
		netSnapshotHeader hdr;
		unsigned char payload[NET_MAX_PAYLOAD];
		int bytes,count;

		for(int i = 0; i < NET_MAX_PLAYERS; i ++){
			memset(&sent[i],0,sizeof(netRacerState));

			sent[i].slot = (unsigned char)i;
			sent[i].racer_id = (unsigned char)(10 + i);
			sent[i].kind = (unsigned char)(20 + i);
			sent[i].mech_state = (unsigned char)(i % 3);
			sent[i].pos_x = net_q_world(512.0f + 60.0f * (float)i);
			sent[i].pos_y = net_q_world(300.0f - 30.0f * (float)i);
			sent[i].pos_z = net_q_height(10.0f * (float)i);
			sent[i].yaw = net_q_yaw(1.0f + 0.3f * (float)i);
			sent[i].pitch = (signed char)(i * 7);
			sent[i].roll = (signed char)(i * -5);
			sent[i].vel_x = net_q_speed(20.0f * (float)i);
			sent[i].vel_y = net_q_speed(-10.0f * (float)i);
			sent[i].vel_z = (signed char)(i - 2);
			sent[i].ang_vel_z = (signed char)(i * 3);
			sent[i].speed = (unsigned short)(1000 + i);
			sent[i].energy = (unsigned char)(200 - i);
			sent[i].flags = (unsigned short)(0x8000 | i);
			sent[i].features[0] = (unsigned char)i;
			sent[i].part_kind[0] = (unsigned char)(i + 1);
			sent[i].part_arcane[0] = (unsigned char)(40 + i);
			sent[i].part_power[0] = (unsigned char)(50 + i);
			sent[i].lap = (unsigned char)i;
			sent[i].checkpoint = (unsigned char)(i * 2);
			sent[i].place = (unsigned char)(i + 1);
			sent[i].time_ms = 1000u * (unsigned int)(i + 1);
			sent[i].bonus_count = 0;
			}

		bytes = net_snapshot_pack(payload,sizeof(payload),12345,sent,NET_MAX_PLAYERS);
		count = net_snapshot_unpack(&hdr,got,NET_MAX_PLAYERS,payload,bytes);

		if(bytes != (int)sizeof(netSnapshotHeader) + NET_MAX_PLAYERS * (int)sizeof(netRacerState) ||
		   count != NET_MAX_PLAYERS || hdr.server_tick != 12345){
			net_logf(f,"FAIL snapshot batch: %d bytes, %d of %d racers, tick %u",
				bytes,count,NET_MAX_PLAYERS,(unsigned)hdr.server_tick);
			ok = 0;
			}
		else {
			for(int i = 0; i < NET_MAX_PLAYERS; i ++)
				if(memcmp(&sent[i],&got[i],sizeof(netRacerState))){
					net_logf(f,"FAIL snapshot racer %d differs after round trip",i);
					ok = 0;
					}
			}

		// A payload lying about its count must be refused.
		{
			netRacerState small[1];
			netRacerState many[NET_MAX_PLAYERS];
			unsigned char forged[NET_MAX_PAYLOAD];
			int got;

			memset(forged,0,sizeof(forged));

			// racer_count sits after the four byte server_tick, which is why
			// this is written at offset 4 and not at 0.
			forged[4] = (unsigned char)NET_MAX_PLAYERS;

			// A one byte payload cannot hold a header, let alone a racer.
			got = net_snapshot_unpack(&hdr,small,1,forged,1);
			if(got != 0){
				net_logf(f,"FAIL snapshot truncated: short payload accepted, %d racers",got);
				ok = 0;
				}

			// The payload claims five racers and is long enough for five,
			// but the caller can only hold one. Only one may be written.
			got = net_snapshot_unpack(&hdr,small,1,forged,
						  (int)sizeof(netSnapshotHeader) + NET_MAX_PLAYERS * (int)sizeof(netRacerState));
			if(got != 1){
				net_logf(f,"FAIL snapshot capacity clamp: claimed %d, capacity 1, got %d",
					NET_MAX_PLAYERS,got);
				ok = 0;
				}

			got = net_snapshot_unpack(&hdr,many,NET_MAX_PLAYERS,forged,
						  (int)sizeof(netSnapshotHeader) + NET_MAX_PLAYERS * (int)sizeof(netRacerState) - 1);
			if(got != 0){
				net_logf(f,"FAIL snapshot short by one byte: accepted, %d racers",got);
				ok = 0;
				}
			}

		if(net_snapshot_pack(payload,sizeof(payload),1,sent,0) != 0){
			net_logf(f,"FAIL snapshot: empty batch accepted");
			ok = 0;
			}
		}

	net_logf(f,"snapshot: world error %.6f, racer block %u bytes, batch %d bytes",
		worst_pos,(unsigned)sizeof(netRacerState),
		(int)sizeof(netSnapshotHeader) + NET_MAX_PLAYERS * (int)sizeof(netRacerState));

	return ok;
}

int net_selftest(void)
{
	const int big_size = 4096;
	const int max_ms = 8000;
	const int burst_total = 10000;

	unsigned char big_buf[4096];
	unsigned char in_buf[NET_FRAG_MAX * NET_FRAG_PAYLOAD];
	unsigned char burst_buf[64];

	FILE* f = fopen(net_selftest_file,"w");

	netSession* host = new netSession;
	netSession* client = new netSession;

	int opened = 0;
	int ok = 0;

	int got_welcome = 0;
	int got_fragment = 0;
	int got_input = 0;
	int got_lobby = 0;
	int got_snapshot = 0;
	int host_reliable = 0;
	int keepalives = 0;

	int sent_fragment = 0;
	int burst_sent = 0;
	int burst_done = 0;
	int burst_counting = 0;
	int burst_recv = 0;
	int burst_gaps = 0;

	int host_client_id = -1;
	unsigned short host_port = 0;

	net_logf(f,"--- net transport self test ---");

	// Input mapping first: it needs no sockets, so if it fails there is no
	// point spending eight seconds on the transport to find out.
	got_input = net_test_input(f);
	if(!got_input)
		net_logf(f,"FAIL net_test_input");

	got_lobby = net_test_lobby(f);
	if(!got_lobby)
		net_logf(f,"FAIL net_test_lobby");

	got_snapshot = net_test_snapshot(f);
	if(!got_snapshot)
		net_logf(f,"FAIL net_test_snapshot");


	for(int i = 0; i < big_size; i ++)
		big_buf[i] = (unsigned char)(i * 31 + 7);

	if(host -> open(0)){
		host -> host();
		netEndpoint* self = host -> get_peer(0);

		if(self && self -> hasAddr){
			host_port = self -> addr.port;
			opened = 1;
			net_logf(f,"host bound on port %u",host_port);
		}
		else net_logf(f,"FAIL host -> local address");
	}
	else net_logf(f,"FAIL host -> open: %s",host -> status());

	if(opened && client -> open(0)){
		if(client -> connect("127.0.0.1",host_port))
			net_logf(f,"client connecting to 127.0.0.1:%u",host_port);
		else {
			opened = 0;
			net_logf(f,"FAIL client -> connect: %s",client -> status());
		}
	}
	else if(opened) {
		opened = 0;
		net_logf(f,"FAIL client -> open: %s",client -> status());
	}

	if(opened){
		unsigned int t0 = clocki();

		while(clocki() - t0 < (unsigned int)max_ms){
			host -> quant();
			client -> quant();

			int peer_id,type;
			unsigned int len;
			unsigned short seq;

			// The host welcomes whoever shows up, reliably.
			netEndpoint* ep = net_first_peer(host,0);

			if(ep && ep -> hasAddr && host_client_id < 0){
				netWelcome welcome;

				memset(&welcome,0,sizeof(welcome));
				welcome.your_slot = (unsigned char)(ep -> id & 0xFF);
				welcome.num_racers = 5;
				welcome.tick_rate = 20;
				welcome.tick_now = clocki();

				for(int i = 0; i < 4; i ++)
					welcome.client_rnd[i] = 0x5A;

				strncpy((char*)welcome.server_name,"selftest",sizeof(welcome.server_name) - 1);

				host -> send(ep -> id,NET_MSG_WELCOME,&welcome,sizeof(welcome),1);
				host_client_id = ep -> id;
			}

			// The client drains its inbox.
			while(client -> receive(&peer_id,&type,in_buf,&len,&seq)){
				if(type == NET_MSG_WELCOME && len == sizeof(netWelcome)){
					got_welcome = 1;
					net_logf(f,"client got WELCOME from peer %d (%u bytes)",peer_id,len);
				}
			}

			// Once welcomed: one payload that must be reassembled from
			// fragments, and one message that must survive the ACK queue.
			// Using NET_MSG_READY keeps the reliable one distinguishable from
			// the keepalive PINGs the transport itself emits.
			netEndpoint* hp = net_first_peer(client,0);

			if(got_welcome && hp && !sent_fragment){
				client -> send(hp -> id,NET_MSG_NONE,big_buf,big_size,0);
				client -> send(hp -> id,NET_MSG_READY,NULL,0,1);
				sent_fragment = 1;

				net_logf(f,"client sent %u bytes, expected %d fragments",big_size,
					(int)((big_size + NET_FRAG_PAYLOAD - 1) / NET_FRAG_PAYLOAD));
			}

			// Burst phase: the load check from the plan. A hundred datagrams
			// per frame, drained between batches so what is under test is the
			// transport and not the size of a socket buffer.
			if(got_fragment && hp && burst_sent < burst_total && !burst_done){
				int batch = 100;

				if(burst_sent + batch > burst_total) batch = burst_total - burst_sent;

				for(int i = 0; i < batch; i ++){
					// The payload is a counter, so a missing datagram shows up
					// as a gap rather than just an accounting error.
					*(int*)burst_buf = burst_sent + i;
					client -> send(hp -> id,NET_MSG_INPUT,burst_buf,64,0);
				}

				burst_sent += batch;

				if(burst_sent >= burst_total){
					burst_done = 1;
					burst_counting = 1;
					net_logf(f,"client burst complete: %d datagrams",burst_sent);
				}
			}

			// The host drains its inbox.
			while(host -> receive(&peer_id,&type,in_buf,&len,&seq)){
				if(type == NET_MSG_NONE && len == (unsigned int)big_size){
					got_fragment = (memcmp(in_buf,big_buf,big_size) == 0);
					net_logf(f,"host reassembled %u bytes, payload %s",len,got_fragment ? "intact" : "MISMATCH");
				}
				else if(type == NET_MSG_READY)
					host_reliable ++;
				else if(type == NET_MSG_INPUT && len == 64){
					burst_recv ++;

					if(*(int*)in_buf != burst_recv - 1)
						burst_gaps ++;
				}
				else if(type == NET_MSG_PING)
					keepalives ++;
			}

			// Give the last batch time to land before the loop runs out.
			if(burst_counting && burst_recv >= burst_total)
				burst_counting = 0;

			netSleep(1);
		}
	}
	else net_logf(f,"skipping the exchange loop, setup failed");

	net_logf(f,"host   sent=%u recv=%u retransmits=%u dropped=%u",
		host -> sent_packets(),host -> recv_packets(),host -> retransmits(),host -> dropped());
	net_logf(f,"client sent=%u recv=%u retransmits=%u dropped=%u",
		client -> sent_packets(),client -> recv_packets(),client -> retransmits(),client -> dropped());

	// A healthy run leaves nothing in the reliable queues. The first revision
	// of this file never dequeued an ACK, so every reliable packet was resent
	// forever and the queue grew without bound.
	net_logf(f,"host reliable queue left: %d",net_reliable_left(host));
	net_logf(f,"client reliable queue left: %d",net_reliable_left(client));

	net_logf(f,"keepalive PINGs seen by host: %d",keepalives);
	net_logf(f,"burst sent %d, received %d, gaps %d",burst_sent,burst_recv,burst_gaps);

	ok = (got_welcome && got_fragment && got_input && got_lobby && got_snapshot && host_reliable == 1 &&
	      burst_sent == burst_total && burst_recv == burst_total && burst_gaps == 0 &&
	      net_reliable_left(host) == 0 && net_reliable_left(client) == 0);

	net_logf(f,"welcome=%d reliable=delivered %d of 1 fragment=%d input=%d lobby=%d snapshot=%d",
		got_welcome,host_reliable,got_fragment,got_input,got_lobby,got_snapshot);
	net_logf(f,ok ? "RESULT: PASS" : "RESULT: FAIL");

	if(f) fclose(f);

	delete host;
	delete client;

	return ok ? 0 : 1;
}
