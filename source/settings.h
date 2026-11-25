#pragma once
#include <switch.h>

// User settings, edited on the Settings screen and saved to
// sdmc:/switch/padlink/settings.ini (plain key=value, safe to delete).
typedef enum { MOTION_AUTO, MOTION_CONTROLLER, MOTION_CONSOLE, MOTION_OFF, MOTION_COUNT } MotionSource;
typedef enum { LAYOUT_POSITION, LAYOUT_LABEL, LAYOUT_COUNT } ButtonLayout;

typedef struct {
    MotionSource motion;
    bool  invert[3];          // flip tilt axes X / Y / Z
    float gyro_scale;         // 0.5 .. 3.0
    bool  low_latency;        // true: report at every USB poll; false: every 8 ms like a real Pro Controller
    ButtonLayout xbox_layout; // how Switch A/B/X/Y map to Xbox buttons
    bool  autostart;          // skip the menu and start the last mode (hold B while launching to see the menu)
    int   last_mode;
} Settings;

extern Settings g_settings;

void settingsLoad(void);
void settingsSave(void);
const char *motionSourceName(MotionSource m);
