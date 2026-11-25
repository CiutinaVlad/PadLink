#include <string.h>
#include "input.h"

static InputState s_prev;

void inputInit(PadState *pad) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    // Handheld + player 1. The Hori Split Pad Pro and rail Joy-Cons show up as handheld.
    padInitializeDefault(pad);
    hidInitializeTouchScreen();
    memset(&s_prev, 0, sizeof s_prev);
}

bool inputRead(PadState *pad, InputState *out) {
    padUpdate(pad);
    memset(out, 0, sizeof *out);
    out->buttons = padGetButtons(pad);
    out->ls = padGetStickPos(pad, 0);
    out->rs = padGetStickPos(pad, 1);

    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        out->touch = true;
        out->touch_x = (u16)ts.touches[0].x;
        out->touch_y = (u16)ts.touches[0].y;
    }
    bool changed = memcmp(out, &s_prev, sizeof *out) != 0;
    s_prev = *out;
    return changed;
}

void inputToPacket(const InputState *in, PadlinkPacket *pkt, u32 seq) {
    memset(pkt, 0, sizeof *pkt);
    pkt->magic0 = PADLINK_MAGIC0;
    pkt->magic1 = PADLINK_MAGIC1;
    pkt->version = PADLINK_VERSION;
    pkt->type = PADLINK_TYPE_STATE;
    pkt->seq = seq;
    pkt->buttons = (u32)(in->buttons & 0xFFFFFFFFull);
    pkt->lx = (s16)in->ls.x; pkt->ly = (s16)in->ls.y;
    pkt->rx = (s16)in->rs.x; pkt->ry = (s16)in->rs.y;
    if (in->touch) {
        pkt->flags |= PADLINK_FLAG_TOUCH;
        pkt->touch_x = in->touch_x;
        pkt->touch_y = in->touch_y;
    }
    pkt->t_send_us = armTicksToNs(armGetSystemTick()) / 1000ULL;
}
