#ifndef __NET_TRANSPORT_H__
#define __NET_TRANSPORT_H__

/*
	UDP transport for the arcade multiplayer mode.

	Deliberately free of OS headers: this layer talks to net_socket.h, which
	is the only place in the build where <winsock2.h> appears. See the comment
	in net_socket.h for why that isolation is required.

	Timing is driven by clocki(), not by global_time(): global_time is the
	13 ms virtual clock and it stops dead whenever mchFreezeTime is set, which
	would freeze the retransmit and timeout machinery in exactly the states
	that need it most.
*/

#include "net_protocol.h"

const int NET_DEFAULT_PORT	= 8500;

// netEndpoint::status
enum netEndpointStatus
{
	NET_EP_DISCONNECTED = 0,
	NET_EP_CONNECTING,
	NET_EP_CONNECTED,
	NET_EP_TIMED_OUT
};

// netSession::state
enum netSessionState
{
	NET_STATE_IDLE = 0,
	NET_STATE_HOSTING,
	NET_STATE_CONNECTING,
	NET_STATE_LOBBY,
	NET_STATE_RUNNING,
	NET_STATE_FINISHED
};

struct netEndpoint
{
	int id;
	int slot;				// racer slot, -1 until assigned
	int status;
	int is_host;

	netAddr addr;

	unsigned int lastRecvTime;
	unsigned int lastPingTime;
	unsigned int rtt;		// round trip, milliseconds

	unsigned short recvSeq;	// highest seq seen from this peer
	unsigned short sendSeq;	// next seq we will use towards this peer

	int hasAddr;			// 0 until the first datagram arrives

	// Reliable datagrams still waiting for an ACK, per peer. Never shared:
	// retransmitting somebody else's traffic to everybody is how the first
	// draft of this file ended up.
	xtList<class netOutgoing>* outLst;

	void* list;
	netEndpoint* prev;
	netEndpoint* next;

	int reliable_count(void){ return outLst -> size(); }

	netEndpoint(void);
	~netEndpoint(void);
};

class netSession
{
public:
	netSession(void);
	~netSession(void);

	int open(int port);
	void close(void);

	// Host side: listen on the socket already opened by open().
	void host(void);

	// Client side: aim at a server and prime the handshake.
	int connect(const char* server,int port);

	void quant(void);

	int send(int peer_id,int type,const void* data,unsigned int len,int reliable = 0);

	// Pops one datagram from the inbox. Returns 0 when nothing is pending.
	// *seq receives the sender's packet sequence number, which the game layer
	// uses to drop duplicates of discrete events out of band from the
	// per-tick input stream.
	int receive(int* peer_id,int* type,void* data,unsigned int* len,unsigned short* seq = NULL);

	netEndpoint* get_peer(int id);
	netEndpoint* get_peer_by_addr(const netAddr& addr);
	int num_peers(void);

	int state(void) const { return _state; }
	void set_state(int st){ _state = st; }
	int local_slot(void) const { return _local_slot; }
	void set_local_slot(int slot){ _local_slot = slot; }
	unsigned int tick(void) const { return _tick; }
	void set_tick(unsigned int t){ _tick = t; }
	const char* status(void) const { return _status; }

	unsigned int sent_packets(void) const { return _sentPackets; }
	unsigned int recv_packets(void) const { return _recvPackets; }
	unsigned int retransmits(void) const { return _retransmits; }
	unsigned int dropped(void) const { return _dropped; }
	unsigned int dropped_short(void) const { return _droppedShort; }
	unsigned int dropped_magic(void) const { return _droppedMagic; }
	unsigned int dropped_len(void) const { return _droppedLen; }
	unsigned int dropped_dup(void) const { return _droppedDup; }

private:
	netSocket _sock;
	int _state;
	int _local_slot;
	unsigned int _tick;

	int _nextPeerId;
	int _numPeers;

	unsigned int _sentPackets;
	unsigned int _recvPackets;
	unsigned int _retransmits;
	unsigned int _dropped;
	unsigned int _droppedShort;
	unsigned int _droppedMagic;
	unsigned int _droppedLen;
	unsigned int _droppedDup;

	// One reassembly buffer per peer slot. Lives in the instance and not in a
	// file static, so two sessions in one process (self test, or host and
	// client running side by side) cannot corrupt each other.
	class netFragBuf
	{
	public:
		void clear(void){
			busy = 0; frag_id = 0; count = 0; received_mask = 0;
			original_type = 0; time = 0; size = 0;
		}

		int busy;
		unsigned char frag_id;
		unsigned char count;
		unsigned int received_mask;
		int original_type;
		unsigned int time;
		int size;
		unsigned char data[NET_FRAG_MAX * NET_FRAG_PAYLOAD];
	};

	netFragBuf _frag[NET_MAX_PEERS];

	char _status[128];

	xtList<netEndpoint>* _peers;
	xtList<class netIncoming>* _inbox;

	netEndpoint* _lastFrom;

	void set_status(const char* text);
	netEndpoint* alloc_peer(const netAddr& addr,int is_host);
	void kill_peer(netEndpoint* ep);
	void ack_peer(netEndpoint* ep,unsigned short seq);
	int  raw_send(const netAddr& addr,int type,int flags,unsigned short seq,unsigned short ack,const void* data,unsigned int len);
	int  build_packet(unsigned char* buf,netEndpoint* ep,int type,int flags,const void* data,unsigned int len);
	void handle_packet(const unsigned char* buf,int sz,const netAddr& from);
	void retransmit(unsigned int now);
	void check_timeouts(unsigned int now);
	void send_pings(unsigned int now);
	void queue_reliable(netEndpoint* ep,int type,const void* data,unsigned int len);
	void deliver(int peer_id,int type,const void* data,int len,unsigned short seq);
};

// The one and only session. Matches the singleton style of the rest of the
// runtime (ogQuant, wi_D, acsScrD and friends).
extern netSession* net_S;

#endif /* __NET_TRANSPORT_H__ */