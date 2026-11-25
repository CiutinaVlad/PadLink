// motion.c - motion sensor (tilt / gyro) for the Pro Controller and Cemu modes.
// Uses the attached controller's sensor when it really has one (Joy-Cons), else the
// console's own sensor (Split Pad Pro has none). Settings come from settings.c.
#include <math.h>
#include <string.h>
#include <switch.h>
#include "motion.h"
#include "log.h"
#include "settings.h"

typedef struct { int src[3]; float sign[3]; } AxisMap;
static char  s_imu_mode[16] = "auto";
static const AxisMap s_map_joy = { {1, 0, 2}, {1.f, -1.f, 1.f} };   // controller sensor: y,-x,z
static const AxisMap s_map_con = { {2, 0, 1}, {1.f, -1.f, 1.f} };   // console sensor:    z,-x,y
static float s_accel_scale = 1.f, s_gyro_scale = 1.f;
static float s_invert[3] = { 1.f, 1.f, 1.f };

static void load_settings(void) {
    static const char *modes[] = { "auto", "joycon", "console", "off" };
    strcpy(s_imu_mode, modes[g_settings.motion < MOTION_COUNT ? g_settings.motion : 0]);
    s_gyro_scale = g_settings.gyro_scale;
    for (int i = 0; i < 3; i++) s_invert[i] = g_settings.invert[i] ? -1.f : 1.f;
    LOG("motion settings: %s, gyro x%.2f, invert %d%d%d", s_imu_mode, s_gyro_scale,
        g_settings.invert[0], g_settings.invert[1], g_settings.invert[2]);
}

// ------------------------------------------------------------------- motion --
// The first 1.5 s after starting are a calibration window (keep the Switch still):
//  - decide whether the attached controller REALLY has a motion sensor. A Split Pad
//    Pro has none: its readings are flat or missing, so we fall back to the console.
//  - work out the console sensor's units (m/s^2 + rad/s vs G + rotations/s),
//  - measure gyro bias so the view doesn't drift.
#define CAL_NS 1500000000ULL
typedef enum { SRC_NONE, SRC_JOYCON, SRC_CONSOLE } MotionSrc;
static HidSixAxisSensorHandle s_six;
static bool s_six_up, s_seven_up;
static MotionSrc s_src = SRC_NONE;
static bool s_cal_done;
static u64  s_cal_start;
static float s_accel[3], s_gyro[3];          // after unit conversion: G and rotations per second
static float s_gbias[3];
static float s_con_accel_div = 1.f, s_con_gyro_div = 1.f;

// calibration accumulators per source
typedef struct { u32 n; u64 last_sn; u32 sn_changes; double a[3], a2[3], g[3]; double mag; } CalAcc;
static CalAcc s_cj, s_cc;

static bool read_joycon(float a[3], float g[3], u64 *sn, bool *connected) {
    if (!s_six_up) return false;
    HidSixAxisSensorState st = {0};
    if (hidGetSixAxisSensorStates(s_six, &st, 1) < 1) return false;
    a[0] = st.acceleration.x; a[1] = st.acceleration.y; a[2] = st.acceleration.z;
    g[0] = st.angular_velocity.x; g[1] = st.angular_velocity.y; g[2] = st.angular_velocity.z;
    *sn = st.sampling_number;
    *connected = (st.attributes & HidSixAxisSensorAttribute_IsConnected) != 0;
    return true;
}

static bool read_console(float a[3], float g[3], u64 *sn) {
    if (!s_seven_up) return false;
    HidSevenSixAxisSensorState st;
    size_t total = 0;
    if (R_FAILED(hidGetSevenSixAxisSensorStates(&st, 1, &total)) || total == 0) return false;
    for (int i = 0; i < 3; i++) { a[i] = st.unk_x18[i]; g[i] = st.unk_x18[3 + i]; }   // accel, gyro, (quaternion)
    *sn = st.sampling_number;
    return true;
}

static void cal_add(CalAcc *c, const float a[3], const float g[3], u64 sn) {
    if (c->n && sn != c->last_sn) c->sn_changes++;
    c->last_sn = sn;
    for (int i = 0; i < 3; i++) { c->a[i] += a[i]; c->a2[i] += (double)a[i] * a[i]; c->g[i] += g[i]; }
    c->mag += sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
    c->n++;
}

static double cal_noise(const CalAcc *c) {     // summed accel variance: a real sensor is never perfectly flat
    double v = 0;
    for (int i = 0; i < 3; i++) { double m = c->a[i] / c->n; v += c->a2[i] / c->n - m * m; }
    return v;
}

static void motion_init(void) {
    memset(&s_cj, 0, sizeof s_cj); memset(&s_cc, 0, sizeof s_cc);
    s_cal_done = false; s_cal_start = 0; s_src = SRC_NONE;
    memset(s_gbias, 0, sizeof s_gbias);
    if (!strcmp(s_imu_mode, "off")) { s_cal_done = true; return; }
    if (strcmp(s_imu_mode, "console")) {
        Result rc = hidGetSixAxisSensorHandles(&s_six, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        if (R_SUCCEEDED(rc)) rc = hidStartSixAxisSensor(s_six);
        LOG_RC("Controller motion sensor (handheld)", rc);
        s_six_up = R_SUCCEEDED(rc);
    }
    if (strcmp(s_imu_mode, "joycon")) {
        Result rc = hidInitializeSevenSixAxisSensor();
        if (R_SUCCEEDED(rc)) rc = hidStartSevenSixAxisSensor();
        LOG_RC("Console motion sensor", rc);
        s_seven_up = R_SUCCEEDED(rc);
    }
    LOG("Keep the Switch still for 1.5 s: calibrating motion...");
}

static void motion_exit(void) {
    if (s_six_up) { hidStopSixAxisSensor(s_six); s_six_up = false; }
    if (s_seven_up) { hidStopSevenSixAxisSensor(); hidFinalizeSevenSixAxisSensor(); s_seven_up = false; }
}

static void motion_finish_cal(void) {
    // Controller sensor counts only if it is connected, updating, reads ~1 G and shows sensor noise.
    bool joy_ok = false;
    if (s_cj.n > 20) {
        double mag = s_cj.mag / s_cj.n, noise = cal_noise(&s_cj);
        joy_ok = s_cj.sn_changes > 10 && mag > 0.7 && mag < 1.3 && noise > 1e-9;
        LOG("controller motion: samples %lu updates %lu |a|=%.3f noise=%.2e -> %s",
            (unsigned long)s_cj.n, (unsigned long)s_cj.sn_changes, mag, noise, joy_ok ? "has a sensor" : "no sensor");
    }
    bool con_ok = false;
    if (s_cc.n > 20) {
        double mag = s_cc.mag / s_cc.n;
        if (mag > 6.0 && mag < 14.0) { s_con_accel_div = 9.80665f; s_con_gyro_div = 6.2831853f; con_ok = true; }   // m/s^2, rad/s
        else if (mag > 0.5 && mag < 1.5) { s_con_accel_div = 1.f; s_con_gyro_div = 1.f; con_ok = true; }        // G, rot/s
        LOG("console motion: samples %lu |a|=%.3f -> units %s", (unsigned long)s_cc.n, mag,
            !con_ok ? "unknown, not used" : s_con_accel_div > 1.f ? "m/s^2 + rad/s" : "G + rot/s");
    }
    bool force_joy = !strcmp(s_imu_mode, "joycon"), force_con = !strcmp(s_imu_mode, "console");
    if ((joy_ok || force_joy) && !force_con && s_six_up) {
        s_src = SRC_JOYCON;
        for (int i = 0; i < 3; i++) s_gbias[i] = s_cj.n ? (float)(s_cj.g[i] / s_cj.n) : 0.f;
    } else if ((con_ok || force_con) && s_seven_up) {
        s_src = SRC_CONSOLE;
        for (int i = 0; i < 3; i++) s_gbias[i] = s_cc.n ? (float)(s_cc.g[i] / s_cc.n) / s_con_gyro_div : 0.f;
    } else {
        s_src = SRC_NONE;
    }
    // A bias bigger than ~5 deg/s means the Switch was moving during calibration: don't trust it.
    for (int i = 0; i < 3; i++) if (fabsf(s_gbias[i]) > 0.014f) { memset(s_gbias, 0, sizeof s_gbias); break; }
    LOG("motion source: %s (gyro bias %.4f %.4f %.4f rot/s)",
        s_src == SRC_JOYCON ? "controller" : s_src == SRC_CONSOLE ? "console" : "none",
        s_gbias[0], s_gbias[1], s_gbias[2]);
    s_cal_done = true;
}

static void motion_read(void) {
    float a[3], g[3]; u64 sn; bool conn;
    if (!s_cal_done) {
        u64 now = armTicksToNs(armGetSystemTick());
        if (!s_cal_start) s_cal_start = now;
        if (read_joycon(a, g, &sn, &conn) && conn) cal_add(&s_cj, a, g, sn);
        if (read_console(a, g, &sn)) cal_add(&s_cc, a, g, sn);
        if (now - s_cal_start >= CAL_NS) motion_finish_cal();
        return;
    }
    bool ok = false;
    if (s_src == SRC_JOYCON) ok = read_joycon(a, g, &sn, &conn);
    else if (s_src == SRC_CONSOLE && read_console(a, g, &sn)) {
        for (int i = 0; i < 3; i++) { a[i] /= s_con_accel_div; g[i] /= s_con_gyro_div; }
        ok = true;
    }
    if (!ok) return;                                   // keep the previous reading
    for (int i = 0; i < 3; i++) { s_accel[i] = a[i]; s_gyro[i] = g[i] - s_gbias[i]; }
}

// ------------------------------------------------------------------- public --
void motionStart(void) { load_settings(); motion_init(); }
void motionStop(void)  { motion_exit(); }
void motionUpdate(void) { motion_read(); }

bool motionGet(float accel_g[3], float gyro_dps[3]) {
    if (s_src == SRC_NONE || !s_cal_done) {           // flat and still
        accel_g[0] = accel_g[1] = 0.f; accel_g[2] = 1.f;
        gyro_dps[0] = gyro_dps[1] = gyro_dps[2] = 0.f;
        return false;
    }
    const AxisMap *m = (s_src == SRC_JOYCON) ? &s_map_joy : &s_map_con;
    for (int i = 0; i < 3; i++) {
        accel_g[i]  = s_accel[m->src[i]] * m->sign[i] * s_invert[i] * s_accel_scale;
        gyro_dps[i] = s_gyro[m->src[i]] * m->sign[i] * s_invert[i] * 360.f * s_gyro_scale;
    }
    return true;
}

void motionGetRaw(float accel[3], float gyro[3]) {
    memcpy(accel, s_accel, sizeof s_accel);
    memcpy(gyro, s_gyro, sizeof s_gyro);
}

const char *motionActiveSource(void) {
    if (!s_cal_done) return "calibrating (keep still)";
    return s_src == SRC_JOYCON ? "controller" : s_src == SRC_CONSOLE ? "console" : "none";
}
