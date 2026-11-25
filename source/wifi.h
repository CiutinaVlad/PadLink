#pragma once
#include <switch.h>
#include "padlink_proto.h"

typedef struct {
    bool     peer_known;
    char     peer_ip[16];
    u32      sent, echoed, send_errors;
    float    rtt_median_ms, rtt_p99_ms, rtt_max_ms;   // over the last 512 echoes
} WifiStats;

Result wifiInit(void);
void   wifiExit(void);
Result wifiSend(const PadlinkPacket *pkt);
void   wifiPoll(void);                 // reads echoes, non-blocking
void   wifiGetStats(WifiStats *out);
