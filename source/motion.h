#pragma once
#include <switch.h>

// Call motionStart() when a mode starts and motionUpdate() every loop. The first
// 1.5 s calibrate (keep the Switch still) and pick the sensor.
void motionStart(void);
void motionStop(void);
void motionUpdate(void);
// Pro Controller axes, after the invert / sensitivity settings: accel in G, gyro in
// deg/s. Returns false (and a flat, still reading) while calibrating or with no sensor.
bool motionGet(float accel_g[3], float gyro_dps[3]);
void motionGetRaw(float accel[3], float gyro[3]);   // as read, for the on-screen view
const char *motionActiveSource(void);                 // "controller", "console", "none", "calibrating..."
