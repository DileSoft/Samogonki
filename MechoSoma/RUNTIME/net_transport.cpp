/* ---------------------------- INCLUDE SECTION ----------------------------- */

#include "StdAfx.h"

#include "net_socket.h"
#include "net_transport.h"

#include <cstring>
#include <cstdio>

/* ----------------------------- STRUCT SECTION ---------------------------- */

// A reliable datagram waiting for its ACK. The full wire bytes are kept so a
// retransmit is a plain sendto() and cannot drift from what was queued.
struct netOutgoing
{
	netEndpoint* ep;
	unsigned short seq;
	unsigned int sendTime;
	int attempts;
	int type;
	int flags;
	int len;
	unsigned char data[NET_MAX_DATAGRAM];

	void* list;
	netOutgoing* prev;
	netOutgoing* next;

	netOutgoing(void){
		ep = NULL; seq = 0; sendTime = 0; attempts = 0;
		type = NET_MSG_NONE; flags = 0; len = 0;
		list = NULL; prev = next = NULL;
	}
	~netOutgoing(void){ }
};

// One datagram waiting for the game layer to consume it. The data buffer is
// sized for a fully reassembled message, not for a single datagram: a
// fragmented message is delivered to this queue once complete, so a
// NET_MAX_PAYLOAD buffer here would overflow on the first big packet.
struct netIncoming
{
	int peer_id;
	int type;
	int len;
	unsigned short seq;
	unsigned char data[NET_FRAG_MAX * NET_FRAG_PAYLOAD];

	void* list;
	netIncoming* prev;
	netIncoming* next;

	netIncoming(void){ peer_id = -1; type = 0; len = 0; seq = 0; list = NULL; prev = next = NULL; }
	~netIncoming(void){ }
};


/* ----------------------------- EXTERN SECTION ---------------------------- */

netSession* net_S = NULL;

/* ---------------------------- DEFINITION SECTION -------------------------- */

netEndpoint::netEndpoint(void)
{
	id = -1;
	slot = -1;
	status = NET_EP_DISCONNECTED;
	is_host = 0;

	addr.ip = 0;
	addr.port = 0;

	lastRecvTime = 0;
	lastPingTime = 0;
	rtt = 0;

	recvSeq = 0;
	sendSeq = 1;

	hasAddr = 0;

	outLst = new xtList<netOutgoing>;

	list = NULL;
	prev = next = NULL;
}

netEndpoint::~netEndpoint(void)
{
	outLst -> delete_all();
	delete outLst;
}

netSession::netSession(void)
{
	_sock = NET_SOCKET_INVALID;
	_state = NET_STATE_IDLE;
	_local_slot = 0;
	_tick = 0;

	_nextPeerId = 0;
	_numPeers = 0;

	_sentPackets = 0;
	_recvPackets = 0;
	_retransmits = 0;
	_dropped = 0;
	_droppedShort = 0;
	_droppedMagic = 0;
	_droppedLen = 0;
	_droppedDup = 0;

	_status[0] = 0;

	_peers = new xtList<netEndpoint>;
	_inbox = new xtList<netIncoming>;

	_lastFrom = NULL;

	for(int i = 0; i < NET_MAX_PEERS; i ++)
		_frag[i].clear();
}

netSession::~netSession(void)
{
	close();

	_peers -> delete_all();
	delete _peers;

	_inbox -> delete_all();
	delete _inbox;
}

void netSession::set_status(const char* text)
{
	if(!text) return;

	strncpy(_status,text,sizeof(_status) - 1);
	_status[sizeof(_status) - 1] = 0;
}

int netSession::open(int port)
{
	netEndpoint* self;

	if(_sock != NET_SOCKET_INVALID){
		set_status("Already open");
		return 1;
	}

	if(!netSocketStartup()){
		set_status(netSocketError());
		return 0;
	}

	_sock = netSocketOpen((unsigned short)port);
	if(_sock == NET_SOCKET_INVALID){
		set_status(netSocketError());
		return 0;
	}

	// Slot -1 is empty: the host itself is always reachable as a peer but has
	// no racer slot of its own.
	self = alloc_peer(netAddr(),1);
	if(self){
		self -> status = NET_EP_CONNECTED;
		self -> is_host = 1;
		netSocketLocalAddr(_sock,&(self -> addr.ip),&(self -> addr.port));
		self -> hasAddr = 1;
	}

	set_status("Socket open");

	return 1;
}

void netSession::close(void)
{
	if(_sock != NET_SOCKET_INVALID){
		netSocketClose(_sock);
		_sock = NET_SOCKET_INVALID;
	}

	_peers -> delete_all();
	_inbox -> delete_all();

	_nextPeerId = 0;
	_numPeers = 0;
	_state = NET_STATE_IDLE;

	for(int i = 0; i < NET_MAX_PEERS; i ++)
		_frag[i].clear();

	set_status("Closed");
}

void netSession::host(void)
{
	_state = NET_STATE_HOSTING;
	set_status("Hosting");
}

int netSession::connect(const char* server,int port)
{
	netAddr addr;

	if(!server || !*server){
		set_status("No server address");
		return 0;
	}

	addr.ip = 0;
	addr.port = (unsigned short)port;

	if(!netSocketResolve(server,&(addr.ip))){
		set_status(netSocketError());
		return 0;
	}

	return connect_addr(addr.ip,addr.port);
}

int netSession::connect_addr(unsigned int ip,unsigned short port)
{
	netEndpoint* ep;
	netAddr addr;

	if(_state == NET_STATE_HOSTING){
		set_status("Already hosting");
		return 0;
	}

	addr.ip = ip;
	addr.port = port;

	ep = get_peer_by_addr(addr);
	if(!ep)
		ep = alloc_peer(addr,0);

	if(!ep){
		set_status("Peer limit reached");
		return 0;
	}

	ep -> status = NET_EP_CONNECTING;

	// The address is known and explicit here, so it is usable for matching
	// right away. Leaving hasAddr clear would make get_peer_by_addr() miss
	// this record for every inbound datagram, spawning a second peer for the
	// same remote endpoint - two sendSeq counters feeding one receiver's
	// recvSeq, which silently eats the packets whose sequence numbers collide.
	ep -> hasAddr = 1;

	_state = NET_STATE_CONNECTING;
	set_status("Connecting");

	// A ping is the handshake bootstrap: retransmitted until the host answers
	// with a WELCOME, which is how this survives a dropped first packet.
	queue_reliable(ep,NET_MSG_PING,NULL,0);

	return 1;
}

int netSession::punch(unsigned int ip,unsigned short port)
{
	netAddr addr;
	netEndpoint* ep;

	addr.ip = ip;
	addr.port = port;

	ep = get_peer_by_addr(addr);
	if(!ep)
		ep = alloc_peer(addr,0);

	if(!ep) return 0;

	ep -> hasAddr = 1;
	ep -> status = NET_EP_CONNECTING;

	// Start the timeout clock, or a punch that never lands would keep a dead
	// peer in the roster forever (a zero lastRecvTime means "no timeout").
	ep -> lastRecvTime = (unsigned int)clocki();

	return raw_send(addr,NET_MSG_PING,0,ep -> sendSeq ++,ep -> recvSeq,NULL,0);
}

int netSession::send_raw(unsigned int ip,unsigned short port,const void* data,unsigned int len)
{
	if(_sock == NET_SOCKET_INVALID) return 0;
	if(!data || !len) return 0;

	if(netSocketSend(_sock,ip,port,data,(int)len) < 0)
		return 0;

	_sentPackets ++;

	return 1;
}

netEndpoint* netSession::alloc_peer(const netAddr& addr,int is_host)
{
	netEndpoint* ep;

	if(_numPeers >= NET_MAX_PEERS) return NULL;

	ep = new netEndpoint;

	ep -> id = _nextPeerId ++;
	ep -> addr = addr;
	ep -> is_host = is_host;

	_peers -> append(ep);

	_numPeers ++;

	return ep;
}

void netSession::kill_peer(netEndpoint* ep)
{
	if(!ep) return;

	_peers -> remove(ep);
	delete ep;

	_numPeers --;

	if(_lastFrom == ep)
		_lastFrom = NULL;
}

netEndpoint* netSession::get_peer(int id)
{
	netEndpoint* ep = _peers -> first();

	while(ep){
		if(ep -> id == id)
			return ep;
		ep = ep -> next;
	}

	return NULL;
}

netEndpoint* netSession::get_peer_by_addr(const netAddr& addr)
{
	netEndpoint* ep = _peers -> first();

	while(ep){
		if(ep -> hasAddr && ep -> addr.ip == addr.ip && ep -> addr.port == addr.port)
			return ep;
		ep = ep -> next;
	}

	return NULL;
}

int netSession::num_peers(void)
{
	return _numPeers;
}

int netSession::build_packet(unsigned char* buf,netEndpoint* ep,int type,int flags,const void* data,unsigned int len)
{
	netHeader* h = (netHeader*)buf;

	h -> magic = NET_MAGIC;
	h -> type = (unsigned char)type;
	h -> flags = (unsigned char)flags;
	h -> seq = ep ? ep -> sendSeq : 0;
	h -> ack = ep ? ep -> recvSeq : 0;
	h -> len = (unsigned short)len;

	if(len && data)
		memcpy(buf + sizeof(netHeader),data,len);

	return (int)(sizeof(netHeader) + len);
}

int netSession::raw_send(const netAddr& addr,int type,int flags,unsigned short seq,unsigned short ack,const void* data,unsigned int len)
{
	unsigned char buf[NET_MAX_DATAGRAM];
	netHeader* h = (netHeader*)buf;
	int sz;

	if(_sock == NET_SOCKET_INVALID) return 0;

	if(sizeof(netHeader) + len > NET_MAX_DATAGRAM) return 0;

	h -> magic = NET_MAGIC;
	h -> type = (unsigned char)type;
	h -> flags = (unsigned char)flags;
	h -> seq = seq;
	h -> ack = ack;
	h -> len = (unsigned short)len;

	if(len && data)
		memcpy(buf + sizeof(netHeader),data,len);

	sz = (int)(sizeof(netHeader) + len);

	if(netSocketSend(_sock,addr.ip,addr.port,buf,sz) < 0)
		return 0;

	_sentPackets ++;

	return 1;
}

void netSession::queue_reliable(netEndpoint* ep,int type,const void* data,unsigned int len)
{
	netOutgoing* out;

	if(!ep) return;

	out = new netOutgoing;
	out -> ep = ep;

	out -> len = build_packet(out -> data,ep,type,NET_FLAG_RELIABLE,data,len);
	out -> type = type;
	out -> flags = NET_FLAG_RELIABLE;
	out -> seq = ep -> sendSeq ++;
	out -> sendTime = (unsigned int)clocki();
	out -> attempts = 1;

	ep -> outLst -> append(out);

	raw_send(ep -> addr,type,NET_FLAG_RELIABLE,out -> seq,ep -> recvSeq,data,len);
}

void netSession::deliver(int peer_id,int type,const void* data,int len,unsigned short seq)
{
	netIncoming* inc = new netIncoming;

	inc -> peer_id = peer_id;
	inc -> type = type;
	inc -> len = len;
	inc -> seq = seq;

	if(len < 0 || len > (int)sizeof(inc -> data)) len = (int)sizeof(inc -> data);
	if(len > 0 && data)
		memcpy(inc -> data,data,len);

	_inbox -> append(inc);
}

int netSession::send(int peer_id,int type,const void* data,unsigned int len,int reliable)
{
	netEndpoint* ep = get_peer(peer_id);

	if(!ep) return 0;

	if(ep -> is_host && !ep -> hasAddr)
		return 0;

	if(len > NET_MAX_PAYLOAD){
		// Layout: [type][frag_id][index][count][data...] so the data area is
		// NET_FRAG_PAYLOAD bytes, and the buffer must hold the four leading
		// bytes too - writing a full NET_FRAG_PAYLOAD here would run four
		// bytes past the end.
		unsigned char frag[NET_MAX_PAYLOAD];
		unsigned char frag_id = (unsigned char)((_tick ++) & 0xFF);
		unsigned int offset = 0;
		unsigned int count = (len + NET_FRAG_PAYLOAD - 1) / NET_FRAG_PAYLOAD;

		if(count > NET_FRAG_MAX) return 0;

		for(unsigned int i = 0; i < count; i ++){
			unsigned int chunk = len - offset;
			if(chunk > NET_FRAG_PAYLOAD) chunk = NET_FRAG_PAYLOAD;

			frag[0] = (unsigned char)type;
			frag[1] = frag_id;
			frag[2] = (unsigned char)i;
			frag[3] = (unsigned char)count;

			if(chunk && data)
				memcpy(frag + 4,(const unsigned char*)data + offset,chunk);

			raw_send(ep -> addr,NET_MSG_FRAGMENT,NET_FLAG_FRAGMENT,ep -> sendSeq ++,ep -> recvSeq,frag,chunk + 4);

			offset += chunk;
		}

		return 1;
	}

	if(reliable){
		queue_reliable(ep,type,data,len);
		return 1;
	}

	return raw_send(ep -> addr,type,0,ep -> sendSeq ++,ep -> recvSeq,data,len);
}

int netSession::broadcast(int type,const void* data,unsigned int len,int reliable)
{
	netEndpoint* ep = _peers -> first();
	netEndpoint* nxt;
	int sent = 0;

	// The list is walked with the next link captured up front: queue_reliable
	// is free to touch the queue of the peer being sent to, and it must not
	// turn into walking a list that just changed shape.
	while(ep){
		nxt = (netEndpoint*)ep -> next;

		// The local record exists so the code can find the bound address, and
		// a peer that has never been heard from has no address to reach yet.
		// Sending to either is how a roster ends up in a black hole.
		if(!ep -> is_host && ep -> hasAddr && send(ep -> id,type,data,len,reliable))
			sent ++;

		ep = nxt;
	}

	return sent;
}

void netSession::ack_peer(netEndpoint* ep,unsigned short seq)
{
	netOutgoing* out = ep -> outLst -> first();

	while(out){
		netOutgoing* nxt = (netOutgoing*)out -> next;

		// A distance >= 0x8000 means seq is behind out -> seq, so the peer
		// has already seen this datagram and it can leave the queue.
		if((unsigned short)(seq - out -> seq) < 0x8000){
			ep -> outLst -> remove(out);
			delete out;
		}

		out = nxt;
	}
}

void netSession::handle_packet(const unsigned char* buf,int sz,const netAddr& from)
{
	netHeader* h = (netHeader*)buf;
	netEndpoint* ep;
	const unsigned char* payload;
	int len;

	if(sz < (int)sizeof(netHeader)){
		_droppedShort ++;
		return;
	}

	if(h -> magic != NET_MAGIC){
		_droppedMagic ++;
		return;
	}

	if(h -> len > NET_MAX_PAYLOAD || (int)sizeof(netHeader) + h -> len > sz){
		_droppedLen ++;
		return;
	}

	ep = get_peer_by_addr(from);

	if(!ep){
		if(_numPeers >= NET_MAX_PEERS){
			_dropped ++;
			return;
		}

		ep = alloc_peer(from,0);
		if(!ep){
			_dropped ++;
			return;
		}
	}

	ep -> hasAddr = 1;
	ep -> lastRecvTime = (unsigned int)clocki();

	if(ep -> status == NET_EP_DISCONNECTED || ep -> status == NET_EP_CONNECTING)
		ep -> status = NET_EP_CONNECTED;

	// The ACK is always acted on, duplicates included, otherwise a peer that
	// lost our ACK would retransmit that same packet forever.
	ack_peer(ep,h -> ack);

	// Then drop the payload itself if we have seen it before.
	if((short)(h -> seq - ep -> recvSeq) <= 0){
		_droppedDup ++;
		return;
	}

	ep -> recvSeq = h -> seq;

	_recvPackets ++;

	// Answer a reliable packet with an ACK straight away. Relying on the next
	// outgoing message to carry it lets the sender burn through its whole
	// retransmit budget before the ACK arrives, which is exactly what the
	// packet counters here used to show.
	if(h -> flags & NET_FLAG_RELIABLE)
		raw_send(ep -> addr,NET_MSG_NONE,NET_FLAG_ACK_ONLY,ep -> sendSeq ++,ep -> recvSeq,NULL,0);

	if(h -> flags & NET_FLAG_ACK_ONLY)
		return;

	payload = buf + sizeof(netHeader);
	len = h -> len;

	if(h -> flags & NET_FLAG_FRAGMENT){
		netSession::netFragBuf* fb = _frag + ep -> id % NET_MAX_PEERS;
		unsigned char frag_id,frag_index,frag_count;
		int chunk,dst;
		unsigned int full;

		if(len < 4) return;

		frag_id = payload[1];
		frag_index = payload[2];
		frag_count = payload[3];

		if(!frag_count || frag_count > NET_FRAG_MAX || frag_index >= frag_count)
			return;

		if(!fb -> busy || fb -> frag_id != frag_id){
			fb -> clear();
			fb -> busy = 1;
			fb -> frag_id = frag_id;
			fb -> count = frag_count;
			fb -> original_type = payload[0];
			fb -> time = (unsigned int)clocki();
		}

		fb -> received_mask |= 1u << frag_index;

		chunk = len - 4;
		dst = frag_index * NET_FRAG_PAYLOAD;

		if(chunk > NET_FRAG_PAYLOAD) chunk = NET_FRAG_PAYLOAD;
		if(dst + chunk > (int)sizeof(fb -> data)) return;

		memcpy(fb -> data + dst,payload + 4,chunk);

		if(dst + chunk > fb -> size)
			fb -> size = dst + chunk;

		full = (frag_count >= 32) ? 0xFFFFFFFFu : ((1u << frag_count) - 1);

		if((fb -> received_mask & full) == full){
			deliver(ep -> id,fb -> original_type,fb -> data,fb -> size,h -> seq);
			fb -> clear();
		}

		return;
	}

	deliver(ep -> id,h -> type,payload,len,h -> seq);
}

void netSession::retransmit(unsigned int now)
{
	netEndpoint* ep = _peers -> first();

	while(ep){
		netOutgoing* out = ep -> outLst -> first();

		while(out){
			netOutgoing* nxt = (netOutgoing*)out -> next;

			if((int)(now - out -> sendTime) >= NET_RETRANSMIT_MS){
				if(out -> attempts >= NET_RETRANSMIT_MAX){
					ep -> outLst -> remove(out);
					delete out;
				}
				else {
					netHeader* h = (netHeader*)(out -> data + sizeof(netHeader));

					out -> attempts ++;
					out -> sendTime = now;

					// Reuse the type and flags that were queued, so a retry is
					// byte for byte the same packet.
					raw_send(ep -> addr,out -> type,out -> flags,out -> seq,ep -> recvSeq,
						h,(unsigned int)(out -> len - (int)sizeof(netHeader)));

					_retransmits ++;
				}
			}

			out = nxt;
		}

		ep = ep -> next;
	}
}

void netSession::check_timeouts(unsigned int now)
{
	netEndpoint* ep = _peers -> first();
	netEndpoint* nxt;

	while(ep){
		nxt = (netEndpoint*)ep -> next;

		// The host's own record is in the list but has no timeout: it exists
		// so the local code can find the bound address. Remote peers that
		// stopped answering are dropped.
		if(!ep -> is_host && ep -> lastRecvTime && (int)(now - ep -> lastRecvTime) > NET_PEER_TIMEOUT_MS)
			kill_peer(ep);

		ep = nxt;
	}
}

void netSession::send_pings(unsigned int now)
{
	netEndpoint* ep = _peers -> first();

	while(ep){
		if(!ep -> is_host && ep -> lastRecvTime &&
		   (int)(now - ep -> lastPingTime) >= NET_PING_PERIOD_MS){
			ep -> lastPingTime = now;
			raw_send(ep -> addr,NET_MSG_PING,0,ep -> sendSeq ++,ep -> recvSeq,NULL,0);
		}

		ep = ep -> next;
	}
}

void netSession::quant(void)
{
	unsigned char buf[NET_MAX_DATAGRAM];
	unsigned int now = (unsigned int)clocki();

	if(_sock == NET_SOCKET_INVALID) return;

	for(;;){
		netAddr from;
		int sz;

		from.ip = 0;
		from.port = 0;

		sz = netSocketRecv(_sock,&(from.ip),&(from.port),buf,sizeof(buf));

		if(sz == NET_SOCKET_WOULDBLOCK) break;
		if(sz == NET_SOCKET_ERROR) break;
		if(sz <= 0) continue;

		handle_packet(buf,sz,from);
	}

	retransmit(now);
	check_timeouts(now);
	send_pings(now);

	for(int i = 0; i < NET_MAX_PEERS; i ++){
		if(_frag[i].busy && (int)(now - _frag[i].time) > NET_FRAG_TIMEOUT_MS)
			_frag[i].clear();
	}
}

int netSession::receive(int* peer_id,int* type,void* data,unsigned int* len,unsigned short* seq)
{
	netIncoming* inc = _inbox -> first();

	if(!inc) return 0;

	if(peer_id) *peer_id = inc -> peer_id;
	if(type) *type = inc -> type;
	if(len) *len = (unsigned int)inc -> len;
	if(seq) *seq = inc -> seq;

	if(data && inc -> len > 0)
		memcpy(data,inc -> data,inc -> len);

	_inbox -> remove(inc);
	delete inc;

	return 1;
}