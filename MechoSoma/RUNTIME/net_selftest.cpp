/* ---------------------------- INCLUDE SECTION ----------------------------- */

#include "StdAfx.h"

#include "net_socket.h"
#include "net_transport.h"
#include "net_input.h"

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

	ok = (got_welcome && got_fragment && got_input && host_reliable == 1 &&
	      burst_sent == burst_total && burst_recv == burst_total && burst_gaps == 0 &&
	      net_reliable_left(host) == 0 && net_reliable_left(client) == 0);

	net_logf(f,"welcome=%d reliable=delivered %d of 1 fragment=%d input=%d",
		got_welcome,host_reliable,got_fragment,got_input);
	net_logf(f,ok ? "RESULT: PASS" : "RESULT: FAIL");

	if(f) fclose(f);

	delete host;
	delete client;

	return ok ? 0 : 1;
}
