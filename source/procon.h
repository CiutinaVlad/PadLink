#pragma once
#include <switch.h>
#include "input.h"

// "Pro Controller (USB)" mode: the Switch shows up as a wired Nintendo Switch Pro
// Controller (057E:2009) to whatever it's plugged into, for example a Switch 2 with
// "Pro Controller Wired Communication" turned on. Includes motion (tilt/gyro) from
// rail Joy-Cons or, with a Split Pad Pro, from the console's own motion sensor.
Result proconInit(void);
void   proconExit(void);
bool   proconIsConfigured(void);
bool   proconIsStreaming(void);         // host finished the handshake
// Call every loop iteration (~1 ms). Reads motion, answers the host, sends reports.
Result proconUpdate(const InputState *in);

typedef struct {
    const char *imu_source;              // "Joy-Con", "console", "off"
    float accel[3], gyro[3];             // last values as read (libnx units)
    u32 reports, replies;
} ProconStats;
void proconGetStats(ProconStats *out);
