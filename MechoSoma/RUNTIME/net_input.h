#ifndef __NET_INPUT_H__
#define __NET_INPUT_H__

/*
	Mapping between the wire input format and the values the vehicle
	controls expect, plus the batching that puts several ticks into one
	datagram.

	Deliberately free of any engine dependency: the transport self test
	exercises all of this before SDL is up and before a single mesh is
	loaded.

	The wire carries setpoints, never filtered controls. Turning a setpoint
	into traction and rudder reads the vehicle state (Vlocal().y) and runs
	on the fixed simulation step, so it has to happen where the simulation
	happens - on the host. A client that smoothed its own input before
	sending would emit numbers the host has no way to reproduce.
*/

#include "net_protocol.h"

#include <cstring>

/*
	Quantize a -1..1 setpoint to the 8 bit wire range.

	Clamped rather than wrapped: keyboard_control() adds the turn keys
	together, so holding both can push the rudder ask past full deflection,
	and that has to read as full deflection rather than folding to zero.

	Rounds to nearest rather than truncating, so a half deflected stick
	survives a round trip through the packet.
*/
inline signed char net_input_quantize(float value)
{
	if(value > 1.0f)
		value = 1.0f;
	if(value < -1.0f)
		value = -1.0f;

	float scaled = value * 127.0f;

	return scaled < 0 ? (signed char)(scaled - 0.5f) : (signed char)(scaled + 0.5f);
}

/*
	Inverse: wire setpoint back to the float the controls work with.

	Clamped because the negative end of an 8 bit range runs to -128, which
	would otherwise decode to slightly more than full deflection.
*/
inline float net_input_expand(signed char value)
{
	float result = (float)value / 127.0f;

	if(result > 1.0f)
		result = 1.0f;
	if(result < -1.0f)
		result = -1.0f;

	return result;
}

/*
	Batch packing for NET_MSG_INPUT.

	Layout: [0] count, [1..] count * sizeof(netInput). The structs are
	declared packed, so they are copied as bytes rather than field by field;
	the wire format is little endian everywhere (see net_protocol.h) and
	every platform this ships on already is.

	Returns the number of bytes written, or 0 on overflow. A payload that
	does not fit in one datagram is refused rather than fragmented: input
	is time critical and a fragmented sample is a dropped sample.
*/
inline int net_input_pack(unsigned char* buf, int buf_size, const netInput* in, int count)
{
	if(!buf || !in || count <= 0 || count > NET_INPUT_BATCH)
		return 0;

	int need = 1 + (int)(count * sizeof(netInput));

	if(need > buf_size || need > NET_MAX_PAYLOAD)
		return 0;

	buf[0] = (unsigned char)count;
	memcpy(buf + 1, (const unsigned char*)in, count * sizeof(netInput));

	return need;
}

/*
	Inverse. Returns how many samples were taken out of the payload, and
	never more than out_capacity even if the payload claims otherwise: a
	corrupt or hostile count must not run past the caller array.
*/
inline int net_input_unpack(netInput* out, int out_capacity, const unsigned char* buf, int buf_size)
{
	if(!out || !buf || buf_size < 1)
		return 0;

	int count = buf[0];
	int need = 1 + (int)(count * sizeof(netInput));

	if(count <= 0 || count > NET_INPUT_BATCH || need > buf_size)
		return 0;

	if(count > out_capacity)
		count = out_capacity;

	memcpy((unsigned char*)out, buf + 1, count * sizeof(netInput));

	return count;
}

#endif /* __NET_INPUT_H__ */
