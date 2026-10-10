#ifndef __NET_EVENT_H__
#define __NET_EVENT_H__

/*
	Event wire codec, engine free like the rest of them.

	An event is not a state, it is something that happened once. That is the
	whole design: a spell goes off, a racer is destroyed, a horn is sounded.
	Nothing here is replicated, corrected or interpolated, because there is
	nothing to correct - it either reached the host or it did not.

	The reason events exist at all, rather than putting a button in the input
	sample, is that a button is a level and an event is an edge. A player holds
	a brake down and a hundred ticks say so; they press an arcane once and one
	tick should say so once. Getting that wrong is how a held button casts the
	same spell a hundred times.

	Reliable by choice, and rare enough to afford it. A dropped input costs a
	step of steering; a dropped event is a spell that never existed.
*/

#include "net_protocol.h"

#include <cstring>

inline int net_event_pack(unsigned char* buf,int buf_size,unsigned int server_tick,
			  const netEvent* events,int count)
{
	if(!buf || !events || count <= 0 || count > NET_MAX_EVENT_PER_PACKET) return 0;

	int need = (int)sizeof(netEventHeader) + count * (int)sizeof(netEvent);

	if(need > buf_size || need > NET_MAX_PAYLOAD) return 0;

	netEventHeader* hdr = (netEventHeader*)buf;

	hdr -> server_tick = server_tick;
	hdr -> count = (unsigned char)count;

	memcpy(buf + sizeof(netEventHeader),(const unsigned char*)events,
	       count * sizeof(netEvent));

	return need;
}

inline int net_event_unpack(netEventHeader* hdr,netEvent* out,int capacity,
			    const unsigned char* buf,int buf_size)
{
	if(!hdr || !out || !buf || buf_size < (int)sizeof(netEventHeader)) return 0;

	int count = ((netEventHeader*)buf) -> count;
	int need = (int)sizeof(netEventHeader) + count * (int)sizeof(netEvent);

	if(count <= 0 || count > NET_MAX_EVENT_PER_PACKET || need > buf_size) return 0;

	*hdr = *(netEventHeader*)buf;

	if(count > capacity) count = capacity;

	memcpy((unsigned char*)out,buf + sizeof(netEventHeader),count * sizeof(netEvent));

	return count;
}

#endif /* __NET_EVENT_H__ */
