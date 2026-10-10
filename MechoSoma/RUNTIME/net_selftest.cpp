/* ---------------------------- INCLUDE SECTION ----------------------------- */

#include "StdAfx.h"

#include "net_socket.h"
#include "net_transport.h"

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

	ok = (got_welcome && got_fragment && host_reliable == 1 &&
	      burst_sent == burst_total && burst_recv == burst_total && burst_gaps == 0 &&
	      net_reliable_left(host) == 0 && net_reliable_left(client) == 0);

	net_logf(f,"welcome=%d reliable=delivered %d of 1 fragment=%d",
		got_welcome,host_reliable,got_fragment);
	net_logf(f,ok ? "RESULT: PASS" : "RESULT: FAIL");

	if(f) fclose(f);

	delete host;
	delete client;

	return ok ? 0 : 1;
}
