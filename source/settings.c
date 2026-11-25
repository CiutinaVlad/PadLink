#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"

#define SETTINGS_PATH "sdmc:/switch/padlink/settings.ini"

Settings g_settings = {
    .motion = MOTION_AUTO, .invert = { false, false, false }, .gyro_scale = 1.0f,
    .low_latency = true, .xbox_layout = LAYOUT_POSITION, .autostart = false, .last_mode = 0,
};

const char *motionSourceName(MotionSource m) {
    static const char *n[] = { "Auto", "Controller", "Console", "Off" };
    return m < MOTION_COUNT ? n[m] : "?";
}

void settingsLoad(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line; int v = atoi(eq + 1);
        if      (!strcmp(k, "motion"))      g_settings.motion = (v >= 0 && v < MOTION_COUNT) ? (MotionSource)v : MOTION_AUTO;
        else if (!strcmp(k, "invert_x"))    g_settings.invert[0] = v != 0;
        else if (!strcmp(k, "invert_y"))    g_settings.invert[1] = v != 0;
        else if (!strcmp(k, "invert_z"))    g_settings.invert[2] = v != 0;
        else if (!strcmp(k, "gyro_scale_pct")) { if (v >= 25 && v <= 400) g_settings.gyro_scale = v / 100.0f; }
        else if (!strcmp(k, "low_latency")) g_settings.low_latency = v != 0;
        else if (!strcmp(k, "xbox_layout")) g_settings.xbox_layout = (v == 1) ? LAYOUT_LABEL : LAYOUT_POSITION;
        else if (!strcmp(k, "autostart"))   g_settings.autostart = v != 0;
        else if (!strcmp(k, "last_mode"))   g_settings.last_mode = v;
    }
    fclose(f);
}

void settingsSave(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/padlink", 0777);
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "motion=%d\ninvert_x=%d\ninvert_y=%d\ninvert_z=%d\ngyro_scale_pct=%d\n"
               "low_latency=%d\nxbox_layout=%d\nautostart=%d\nlast_mode=%d\n",
            g_settings.motion, g_settings.invert[0], g_settings.invert[1], g_settings.invert[2],
            (int)(g_settings.gyro_scale * 100.0f + 0.5f), g_settings.low_latency,
            g_settings.xbox_layout, g_settings.autostart, g_settings.last_mode);
    fclose(f);
}
