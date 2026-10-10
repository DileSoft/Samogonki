#ifndef __NET_SNAPSHOT_H__
#define __NET_SNAPSHOT_H__

/*
	Snapshot wire format and the quantization behind it.

	Deliberately free of any engine dependency, like net_input.h: the
	transport self test covers all of this before SDL and before a single
	mesh is loaded.

	Layout is [netSnapshotHeader][count * sizeof(netRacerState)]. The structs
	are declared packed, so they are copied as bytes rather than field by
	field; the wire is little endian everywhere (see net_protocol.h) and
	every platform this ships on already is.

	Bonuses are not carried in a snapshot. A bonus is a discrete thing that
	either is there or is not, and the roster already says which; sending the
	list per racer per tick would spend most of a datagram repeating it.
*/

#include "net_protocol.h"

#include <cstring>

/*
	Quantization.

	These are the only place in the protocol where precision is lost, so each
	one states what it costs. The numbers are chosen so that the worst case
	stays under one centimetre per unit of the game's own scale, which is
	far below what the camera can show but not so far that two vehicles
	drifted apart visibly over a lap.
*/

// World coordinate. The engine world is toroidal and wraps at
// NET_WORLD_SIZE, so folding first is what keeps the value in range: taking
// the raw coordinate and clamping would instead pin every vehicle that
// crosses the far edge of the map.
inline unsigned short net_q_world(float x)
{
	// Fold into [0, NET_WORLD_SIZE).
	x = x - (float)((int)(x / (float)NET_WORLD_SIZE) * NET_WORLD_SIZE);
	if(x < 0) x += (float)NET_WORLD_SIZE;

	float scaled = x * (float)NET_Q_WORLD;

	if(scaled < 0) scaled = 0;
	if(scaled > 65535.0f) scaled = 65535.0f;

	return (unsigned short)(scaled + 0.5f);
}

inline float net_uq_world(unsigned short p)
{
	return (float)p / (float)NET_Q_WORLD;
}

// Height, signed. Range is the full world height with room on both sides.
inline signed short net_q_height(float z)
{
	float scaled = z * (float)NET_Q_HEIGHT;

	if(scaled > 32767.0f) scaled = 32767.0f;
	if(scaled < -32768.0f) scaled = -32768.0f;

	return (signed short)(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
}

inline float net_uq_height(signed short z)
{
	return (float)z / (float)NET_Q_HEIGHT;
}

// Heading. Deliberately modular: a wrapping value would make the interpolator
// take the long way round the circle at the 2*PI seam.
inline unsigned short net_q_yaw(float a)
{
	float turns = a / (2.0f * 3.14159265358979323846f);

	// Wrap to [0, 1).
	turns = turns - (float)((int)turns);
	if(turns < 0) turns += 1.0f;

	float scaled = turns * 65536.0f;

	if(scaled < 0) scaled = 0;
	if(scaled >= 65536.0f) scaled = 0;

	return (unsigned short)(scaled + 0.5f);
}

inline float net_uq_yaw(unsigned short a)
{
	return (float)a / 65536.0f * 2.0f * 3.14159265358979323846f;
}

// Horizontal speed. 512 units per second is well past anything the vehicles
// reach, and the step is still under a millimetre per frame at 20 Hz.
inline signed short net_q_speed(float v)
{
	float scaled = v * (float)NET_Q_SPEED_XY;

	if(scaled > 32767.0f) scaled = 32767.0f;
	if(scaled < -32768.0f) scaled = -32768.0f;

	return (signed short)(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
}

inline float net_uq_speed(signed short v)
{
	return (float)v / (float)NET_Q_SPEED_XY;
}

/*
	Arcane mana, as a fraction of the arcane's own MaxPower rather than an
	absolute number. MaxPower comes from the arcane's data and is not
	guaranteed to be 100, but both machines build the car from the same data,
	so the fraction means the same thing on both sides. Eight bits is far
	finer than a bar on screen can show.

	The host is the only machine whose arcane drains: a client's copy never
	runs the process that spends Power, so without this every client bar
	would sit full and every spell would look ready.
*/
inline unsigned char net_q_mana(float power,float maxpower)
{
	if(maxpower <= 0.0f) return 0;

	float f = power / maxpower;

	if(f < 0.0f) f = 0.0f;
	if(f > 1.0f) f = 1.0f;

	return (unsigned char)(f * 255.0f + 0.5f);
}

inline float net_uq_mana(unsigned char q,float maxpower)
{
	return maxpower * (float)q / 255.0f;
}

/*
	Encode a whole snapshot. Returns the number of bytes written, or 0 if it
	would not fit in one datagram.

	Refused rather than fragmented, like input: a snapshot that arrives late
	is worth less than one, so splitting it across retransmits would only
	delay the state that is already stale.
*/
inline int net_snapshot_pack(unsigned char* buf,int buf_size,unsigned int server_tick,
			     const netRacerState* racers,int count)
{
	if(!buf || !racers || count <= 0 || count > NET_MAX_PLAYERS) return 0;

	int need = (int)sizeof(netSnapshotHeader) + count * (int)sizeof(netRacerState);

	if(need > buf_size || need > NET_MAX_PAYLOAD) return 0;

	netSnapshotHeader* hdr = (netSnapshotHeader*)buf;

	hdr -> server_tick = server_tick;
	hdr -> racer_count = (unsigned char)count;
	hdr -> delta_mask = 0;

	memcpy(buf + sizeof(netSnapshotHeader),(const unsigned char*)racers,
	       count * sizeof(netRacerState));

	return need;
}

/*
	Decode. Returns how many racers were read, never more than capacity even
	if the payload claims otherwise, and 0 if anything is off: a corrupt
	count must not run past the caller's array.
*/
inline int net_snapshot_unpack(netSnapshotHeader* hdr,netRacerState* out,int capacity,
			       const unsigned char* buf,int buf_size)
{
	if(!hdr || !out || !buf || buf_size < (int)sizeof(netSnapshotHeader)) return 0;

	int count = ((netSnapshotHeader*)buf) -> racer_count;
	int need = (int)sizeof(netSnapshotHeader) + count * (int)sizeof(netRacerState);

	if(count <= 0 || count > NET_MAX_PLAYERS || need > buf_size) return 0;

	*hdr = *(netSnapshotHeader*)buf;

	if(count > capacity) count = capacity;

	memcpy((unsigned char*)out,buf + sizeof(netSnapshotHeader),count * sizeof(netRacerState));

	return count;
}

#endif /* __NET_SNAPSHOT_H__ */
