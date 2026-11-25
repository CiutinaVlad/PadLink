#pragma once
#include <switch.h>
#include "padlink_proto.h"

typedef struct {
    u64 buttons;              // HidNpadButton mask
    HidAnalogStickState ls, rs;
    bool touch;
    u16 touch_x, touch_y;
} InputState;

void inputInit(PadState *pad);
// Reads the current state. Returns true if anything changed since the last call.
bool inputRead(PadState *pad, InputState *out);
void inputToPacket(const InputState *in, PadlinkPacket *pkt, u32 seq);
