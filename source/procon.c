// procon.c - emulate a wired Nintendo Switch Pro Controller over usb:ds.
//
// Protocol (public reverse-engineering notes, e.g. dekuNukem's
// Nintendo_Switch_Reverse_Engineering): the host talks over a HID interrupt OUT
// endpoint and the controller answers on interrupt IN.
//   80 01 -> 81 01 00 03 <MAC reversed>      status / controller type
//   80 02 -> 81 02, 80 03 -> 81 03           handshake / baud rate
//   80 04                                    start streaming 0x30 reports, no timeout
//   80 05                                    stop streaming
//   01 <cnt> <rumble x8> <subcmd> <args>    -> 21 <timer> <input...> <ack> <subcmd> <data>
//   10 <cnt> <rumble x8>                     rumble only (ignored)
// Input report 0x30 = buttons, two 12-bit sticks, then 3 IMU samples (accel + gyro).
//
// Everything here is written for PadLink; no code was copied from other projects.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <switch.h>
#include "procon.h"
#include "log.h"
#include "settings.h"

#define PC_VID 0x057E
#define PC_PID 0x2009

// HID report descriptor of a Pro Controller: input reports 0x30/0x21/0x81 and
// output reports 0x01/0x10/0x80/0x82, 63 bytes each (vendor page), plus a generic
// gamepad layout for report 0x30.
static const u8 k_report_desc[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01,
    0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0A, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03,
    0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00,
    0x0B, 0x30, 0x00, 0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00,
    0x0B, 0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01, 0x00,
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0,
    0x0B, 0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01,
    0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x34, 0x81, 0x03,
    0x06, 0x00, 0xFF,
    0x85, 0x21, 0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03,
    0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03,
    0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x10, 0x09, 0x04, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0xC0
};

static const u8 k_mac[6] = { 0x7C, 0xBB, 0x8A, 0x50, 0x4C, 0x4B };   // fixed, "PLK"

// --------------------------------------------------------------- SPI flash ---
// Only the regions the host reads. Everything else reads as 0xFF (= "not set").
static void pack12(u8 *o, u16 a, u16 b) {
    o[0] = a & 0xFF;
    o[1] = ((b & 0x0F) << 4) | ((a >> 8) & 0x0F);
    o[2] = (b >> 4) & 0xFF;
}

#define STICK_CENTER 0x800
#define STICK_RANGE  0x600     // what the calibration below promises in each direction

static void spi_read(u32 addr, u8 len, u8 *out) {
    memset(out, 0xFF, len);
    for (u32 i = 0; i < len; i++) {
        u32 a = addr + i;
        u8 v = 0xFF;
        if (a == 0x6012) v = 0x03;                     // device type: Pro Controller
        else if (a == 0x601B) v = 0x01;                // colors present
        else if (a >= 0x6020 && a < 0x6038) {          // factory IMU calibration
            static u8 imu[24];
            static bool init;
            if (!init) {
                memset(imu, 0, sizeof imu);            // accel origin 0,0,0
                for (int k = 0; k < 3; k++) { imu[6 + 2*k] = 0x00; imu[7 + 2*k] = 0x40; }   // accel sens 0x4000
                /* gyro origin 0 (bytes 12-17) */
                for (int k = 0; k < 3; k++) { imu[18 + 2*k] = 0x3B; imu[19 + 2*k] = 0x34; } // gyro sens 0x343B
                init = true;
            }
            v = imu[a - 0x6020];
        } else if (a >= 0x603D && a < 0x604F) {        // factory stick calibration
            static u8 st[18];
            static bool init;
            if (!init) {
                // left:  max-above-center, center, min-below-center
                pack12(&st[0], STICK_RANGE, STICK_RANGE);
                pack12(&st[3], STICK_CENTER, STICK_CENTER);
                pack12(&st[6], STICK_RANGE, STICK_RANGE);
                // right: center, min-below-center, max-above-center
                pack12(&st[9], STICK_CENTER, STICK_CENTER);
                pack12(&st[12], STICK_RANGE, STICK_RANGE);
                pack12(&st[15], STICK_RANGE, STICK_RANGE);
                init = true;
            }
            v = st[a - 0x603D];
        } else if (a >= 0x6050 && a < 0x605C) {        // colors: body, buttons, grips
            static const u8 col[12] = { 0x2D,0x2D,0x2D, 0xE6,0xE6,0xE6, 0x2D,0x2D,0x2D, 0x2D,0x2D,0x2D };
            v = col[a - 0x6050];
        } else if (a >= 0x6080 && a < 0x6098) {        // IMU horizontal offsets + stick params (left)
            static const u8 p[24] = { 0x50,0xFD,0x00,0x00,0xC6,0x0F,
                                      0x0F,0x30,0x61,0x96,0x30,0xF3,0xD4,0x14,0x54,0x41,0x15,0x54,0xC7,0x79,0x9C,0x33,0x36,0x63 };
            v = p[a - 0x6080];
        } else if (a >= 0x6098 && a < 0x60AA) {        // stick params (right)
            static const u8 p[18] = { 0x0F,0x30,0x61,0x96,0x30,0xF3,0xD4,0x14,0x54,0x41,0x15,0x54,0xC7,0x79,0x9C,0x33,0x36,0x63 };
            v = p[a - 0x6098];
        }
        // 0x8010.. user calibration: left as 0xFF = none, so the host uses factory data
        out[i] = v;
    }
}

// --------------------------------------------------------------- settings ----
// Comes from the in-app Settings screen (settings.c).
typedef struct { int src[3]; float sign[3]; } AxisMap;
static char  s_imu_mode[16] = "auto";
static const AxisMap s_map_joy = { {1, 0, 2}, {1.f, -1.f, 1.f} };   // controller sensor: y,-x,z
static const AxisMap s_map_con = { {2, 0, 1}, {1.f, -1.f, 1.f} };   // console sensor:    z,-x,y
static float s_accel_scale = 1.f, s_gyro_scale = 1.f;
static float s_invert[3] = { 1.f, 1.f, 1.f };
static u32   s_report_ms = 0;

static void load_settings(void) {
    static const char *modes[] = { "auto", "joycon", "console", "off" };
    strcpy(s_imu_mode, modes[g_settings.motion < MOTION_COUNT ? g_settings.motion : 0]);
    s_gyro_scale = g_settings.gyro_scale;
    for (int i = 0; i < 3; i++) s_invert[i] = g_settings.invert[i] ? -1.f : 1.f;
    s_report_ms = g_settings.low_latency ? 0 : 8;
    LOG("settings: motion=%s gyro x%.2f invert %d%d%d report_ms=%lu", s_imu_mode, s_gyro_scale,
        g_settings.invert[0], g_settings.invert[1], g_settings.invert[2], (unsigned long)s_report_ms);
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

static s16 clamp16(float v) { return v > 32767.f ? 32767 : v < -32768.f ? -32768 : (s16)(v >= 0.f ? v + 0.5f : v - 0.5f); }

// Pro Controller raw units with the calibration in SPI: 4096 per G, 13371/936 per deg/s.
static void motion_raw(s16 out[6]) {
    if (s_src == SRC_NONE || !s_cal_done) {           // flat and still
        out[0] = 0; out[1] = 0; out[2] = 4096; out[3] = out[4] = out[5] = 0;
        return;
    }
    const AxisMap *m = (s_src == SRC_JOYCON) ? &s_map_joy : &s_map_con;
    for (int i = 0; i < 3; i++) {
        float a = s_accel[m->src[i]] * m->sign[i] * s_invert[i];
        float g = s_gyro[m->src[i]] * m->sign[i] * s_invert[i];
        out[i]     = clamp16(a * 4096.f * s_accel_scale);
        out[3 + i] = clamp16(g * 360.f * (13371.f / 936.f) * s_gyro_scale);
    }
}

static const char *motion_src_name(void) {
    if (!s_cal_done) return "calibrating (keep still)";
    return s_src == SRC_JOYCON ? "controller" : s_src == SRC_CONSOLE ? "console" : "none";
}

// -------------------------------------------------------------------- USB ----
static UsbDsInterface *s_intf;
static UsbDsEndpoint  *s_ep_in, *s_ep_out;
static u8 *s_in_buf, *s_out_buf, *s_ctrl_buf;   // 0x1000-aligned
static Thread s_rx_thread, s_ctrl_thread;
static volatile bool s_run;
static volatile bool s_streaming;
static bool s_in_pending, s_was_configured;
static u32 s_in_urb;
static u64 s_in_tick, s_last30;
static u8  s_timer;
static u32 s_reports, s_replies;

// Replies from the RX thread, sent by the main loop (the only IN sender).
#define QN 16
static u8 s_q[QN][64];
static int s_qh, s_qt;
static Mutex s_qm;
static InputState s_cur;     // latest input, for replies built on the RX thread
static Mutex s_curm;

static void q_push(const u8 *d) {
    mutexLock(&s_qm);
    int n = (s_qh + 1) % QN;
    if (n != s_qt) { memcpy(s_q[s_qh], d, 64); s_qh = n; }
    mutexUnlock(&s_qm);
}
static bool q_pop(u8 *d) {
    mutexLock(&s_qm);
    bool ok = s_qt != s_qh;
    if (ok) { memcpy(d, s_q[s_qt], 64); s_qt = (s_qt + 1) % QN; }
    mutexUnlock(&s_qm);
    return ok;
}

static u16 stick12(s32 v) {
    s32 r = STICK_CENTER + (v * STICK_RANGE) / 32767;
    return r < 0 ? 0 : r > 0xFFF ? 0xFFF : (u16)r;
}

// Bytes after the report id: timer, battery/connection, 3 button bytes, 2 sticks, vibrator.
static void fill_input(u8 *p, const InputState *in) {
    u64 b = in->buttons;
    bool minus = b & HidNpadButton_Minus;
    bool home = minus && (b & HidNpadButton_StickR);      // HOME/Capture are taken by the Switch 1 itself
    bool capture = minus && (b & HidNpadButton_StickL);
    if (home || capture) b &= ~(u64)(HidNpadButton_Minus | HidNpadButton_StickR | HidNpadButton_StickL);

    p[0] = s_timer++;
    p[1] = 0x91;                                        // battery full + charging, Pro, USB powered
    p[2] = ((b & HidNpadButton_Y) ? 0x01 : 0) | ((b & HidNpadButton_X) ? 0x02 : 0) |
           ((b & HidNpadButton_B) ? 0x04 : 0) | ((b & HidNpadButton_A) ? 0x08 : 0) |
           ((b & HidNpadButton_R) ? 0x40 : 0) | ((b & HidNpadButton_ZR) ? 0x80 : 0);
    p[3] = ((b & HidNpadButton_Minus) ? 0x01 : 0) | ((b & HidNpadButton_Plus) ? 0x02 : 0) |
           ((b & HidNpadButton_StickR) ? 0x04 : 0) | ((b & HidNpadButton_StickL) ? 0x08 : 0) |
           (home ? 0x10 : 0) | (capture ? 0x20 : 0);
    p[4] = ((b & HidNpadButton_Down) ? 0x01 : 0) | ((b & HidNpadButton_Up) ? 0x02 : 0) |
           ((b & HidNpadButton_Right) ? 0x04 : 0) | ((b & HidNpadButton_Left) ? 0x08 : 0) |
           ((b & HidNpadButton_L) ? 0x40 : 0) | ((b & HidNpadButton_ZL) ? 0x80 : 0);
    pack12(&p[5], stick12(in->ls.x), stick12(in->ls.y));
    pack12(&p[8], stick12(in->rs.x), stick12(in->rs.y));
    p[11] = 0x00;
}

static void reply_subcmd(u8 subcmd, u8 ack, const u8 *data, size_t n) {
    u8 r[64] = {0};
    r[0] = 0x21;
    mutexLock(&s_curm);
    InputState in = s_cur;
    mutexUnlock(&s_curm);
    fill_input(&r[1], &in);
    r[13] = ack;
    r[14] = subcmd;
    if (n > 49) n = 49;
    if (n) memcpy(&r[15], data, n);
    q_push(r);
}

static void handle_out(const u8 *b, u32 len) {
    if (len < 2) return;
    if (b[0] == 0x80) {
        u8 r[64] = {0};
        switch (b[1]) {
            case 0x01:
                r[0] = 0x81; r[1] = 0x01; r[2] = 0x00; r[3] = 0x03;
                for (int i = 0; i < 6; i++) r[4 + i] = k_mac[5 - i];
                q_push(r);
                break;
            case 0x02: case 0x03:
                r[0] = 0x81; r[1] = b[1]; q_push(r);
                break;
            case 0x04: s_streaming = true;  LOG("host: start streaming (80 04)"); break;
            case 0x05: s_streaming = false; LOG("host: stop streaming (80 05)"); break;
            default:   LOG("host: 80 %02X (ignored)", b[1]); break;
        }
        return;
    }
    if (b[0] == 0x01 && len >= 11) {
        u8 sc = b[10];
        const u8 *arg = &b[11];
        switch (sc) {
            case 0x01: { u8 d[] = { 0x03 }; reply_subcmd(sc, 0x81, d, sizeof d); break; }
            case 0x02: {                                   // device info
                u8 d[12] = { 0x04, 0x33, 0x03, 0x02 };     // firmware 4.33, Pro Controller
                memcpy(&d[4], k_mac, 6);
                d[10] = 0x01; d[11] = 0x01;                // colors from SPI
                reply_subcmd(sc, 0x82, d, sizeof d);
                break;
            }
            case 0x04: { u8 d[14] = {0}; reply_subcmd(sc, 0x83, d, sizeof d); break; }
            case 0x10: {                                   // SPI read
                u32 addr = arg[0] | (arg[1] << 8) | (arg[2] << 16) | ((u32)arg[3] << 24);
                u8 n = arg[4];
                if (n > 0x1D) n = 0x1D;
                u8 d[5 + 0x1D];
                memcpy(d, arg, 4); d[4] = n;
                spi_read(addr, n, &d[5]);
                reply_subcmd(sc, 0x90, d, 5 + n);
                break;
            }
            case 0x21: {                                   // NFC/IR MCU config
                u8 d[] = { 0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01 };
                reply_subcmd(sc, 0xA0, d, sizeof d);
                break;
            }
            default:                                       // 03 report mode, 08, 30 lights, 38 HOME light, 40/41 IMU, 48 rumble...
                reply_subcmd(sc, 0x80, NULL, 0);
                break;
        }
        s_replies++;
        return;
    }
    // 0x10 = rumble only, 0x00 = keepalive: nothing to answer
}

static void rx_thread(void *arg) {
    (void)arg;
    bool posted = false;
    u32 urb = 0;
    while (s_run) {
        if (!posted) {
            UsbState st;
            if (R_FAILED(usbDsGetState(&st)) || st != UsbState_Configured) { svcSleepThread(20000000ULL); continue; }
            if (R_FAILED(usbDsEndpoint_PostBufferAsync(s_ep_out, s_out_buf, 64, &urb))) { svcSleepThread(20000000ULL); continue; }
            posted = true;
        }
        if (R_FAILED(eventWait(&s_ep_out->CompletionEvent, 100000000ULL))) continue;   // 100 ms, re-check s_run
        eventClear(&s_ep_out->CompletionEvent);
        posted = false;
        UsbDsReportData rd; u32 req = 0, xfer = 0;
        if (R_SUCCEEDED(usbDsEndpoint_GetReportData(s_ep_out, &rd)) &&
            R_SUCCEEDED(usbDsParseReportData(&rd, urb, &req, &xfer)) && xfer > 0) {
            handle_out(s_out_buf, xfer);
        }
    }
}

// Control requests on our interface (report descriptor, SET_IDLE, SET_REPORT).
static Result ctrl_wait(Event *ev) { Result rc = eventWait(ev, 1000000000ULL); eventClear(ev); return rc; }

static void ctrl_thread(void *arg) {
    (void)arg;
    while (s_run) {
        if (R_FAILED(eventWait(&s_intf->SetupEvent, 100000000ULL))) continue;
        eventClear(&s_intf->SetupEvent);
        struct usb_control_setup su __attribute__((aligned(8))) = {0};
        if (R_FAILED(usbDsInterface_GetSetupPacket(s_intf, &su, sizeof su))) continue;
        u32 urb;
        if (su.bmRequestType == 0x81 && su.bRequest == 0x06 && (su.wValue >> 8) == USB_DT_REPORT) {
            u32 n = sizeof k_report_desc < su.wLength ? sizeof k_report_desc : su.wLength;
            memcpy(s_ctrl_buf, k_report_desc, n);
            if (R_SUCCEEDED(usbDsInterface_CtrlInPostBufferAsync(s_intf, s_ctrl_buf, n, &urb)) &&
                R_SUCCEEDED(ctrl_wait(&s_intf->CtrlInCompletionEvent)) &&
                R_SUCCEEDED(usbDsInterface_CtrlOutPostBufferAsync(s_intf, s_ctrl_buf, 0, &urb)))
                ctrl_wait(&s_intf->CtrlOutCompletionEvent);
        } else if (su.bmRequestType == 0x21 && (su.bRequest == 0x0A || su.bRequest == 0x0B)) {   // SET_IDLE / SET_PROTOCOL
            if (R_SUCCEEDED(usbDsInterface_CtrlInPostBufferAsync(s_intf, s_ctrl_buf, 0, &urb)))
                ctrl_wait(&s_intf->CtrlInCompletionEvent);
        } else if (su.bmRequestType == 0x21 && su.bRequest == 0x09 && su.wLength <= 64) {      // SET_REPORT
            u32 n = su.wLength;
            if (R_SUCCEEDED(usbDsInterface_CtrlOutPostBufferAsync(s_intf, s_ctrl_buf, n, &urb)) &&
                R_SUCCEEDED(ctrl_wait(&s_intf->CtrlOutCompletionEvent))) {
                u8 tmp[64]; memcpy(tmp, s_ctrl_buf, n);
                if (R_SUCCEEDED(usbDsInterface_CtrlInPostBufferAsync(s_intf, s_ctrl_buf, 0, &urb)))
                    ctrl_wait(&s_intf->CtrlInCompletionEvent);
                handle_out(tmp, n);
            }
        } else {
            LOG("ctrl %02X/%02X unhandled -> stall", su.bmRequestType, su.bRequest);
            usbDsInterface_StallCtrl(s_intf);
        }
    }
}

#define STEP(name, call) do { rc = (call); LOG_RC(name, rc); if (R_FAILED(rc)) goto fail; } while (0)

static Result set_device_descriptors(void) {
    Result rc;
    u8 iM = 0, iP = 0, iS = 0;
    static const u16 langs[1] = { 0x0409 };
    rc = usbDsAddUsbLanguageStringDescriptor(NULL, langs, 1);
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iM, "Nintendo Co., Ltd.");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iP, "Pro Controller");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iS, "000000000001");
    if (R_FAILED(rc)) return rc;
    struct usb_device_descriptor dev = {
        .bLength = USB_DT_DEVICE_SIZE, .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200, .bMaxPacketSize0 = 0x40,
        .idVendor = PC_VID, .idProduct = PC_PID, .bcdDevice = 0x0210,
        .iManufacturer = iM, .iProduct = iP, .iSerialNumber = iS, .bNumConfigurations = 1,
    };
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev))) return rc;
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev))) return rc;
    dev.bcdUSB = 0x0300; dev.bMaxPacketSize0 = 0x09;
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev))) return rc;
    static const u8 bos[0x16] = {
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00
    };
    return usbDsSetBinaryObjectStore(bos, sizeof bos);
}

static Result setup_interface(void) {
    Result rc = usbDsRegisterInterface(&s_intf);
    if (R_FAILED(rc)) return rc;
    struct usb_interface_descriptor intf = {
        .bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = s_intf->interface_index, .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_HID,
    };
    struct usb_hid_descriptor hid = {
        .bLength = USB_DT_HID_SIZE, .bDescriptorType = USB_DT_HID, .bcdHID = 0x0111,
        .bNumDescriptors = 1, .bClassDescriptorType = USB_DT_REPORT,
        .wClassDescriptorLength = sizeof k_report_desc,
    };
    struct usb_endpoint_descriptor ein = {
        .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN | (s_intf->interface_index + 1),
        .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT, .wMaxPacketSize = 0x40, .bInterval = 8,
    };
    struct usb_endpoint_descriptor eout = ein;
    eout.bEndpointAddress = USB_ENDPOINT_OUT | (s_intf->interface_index + 1);
    struct usb_ss_endpoint_companion_descriptor ss = {
        .bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE, .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .wBytesPerInterval = 0x40,
    };
    UsbDeviceSpeed sp[3] = { UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super };
    for (int i = 0; i < 3; i++) {
        // full speed: 4 ms (a real Pro Controller says 8); high/super: 2^(4-1) x 125 us = 1 ms
        ein.bInterval = eout.bInterval = 4;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &intf, sizeof intf))) return rc;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &hid, sizeof hid))) return rc;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &ein, sizeof ein))) return rc;
        if (i == 2 && R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &ss, sizeof ss))) return rc;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &eout, sizeof eout))) return rc;
        if (i == 2 && R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &ss, sizeof ss))) return rc;
    }
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(s_intf, &s_ep_in, ein.bEndpointAddress))) return rc;
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(s_intf, &s_ep_out, eout.bEndpointAddress))) return rc;
    return usbDsInterface_EnableInterface(s_intf);
}

Result proconInit(void) {
    Result rc;
    _Static_assert(sizeof k_report_desc == 203, "Pro Controller report descriptor is 203 bytes");
    load_settings();
    motion_init();
    s_in_buf = aligned_alloc(0x1000, 0x1000);
    s_out_buf = aligned_alloc(0x1000, 0x1000);
    s_ctrl_buf = aligned_alloc(0x1000, 0x1000);
    if (!s_in_buf || !s_out_buf || !s_ctrl_buf) { proconExit(); return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); }
    memset(s_in_buf, 0, 0x1000); memset(s_out_buf, 0, 0x1000); memset(s_ctrl_buf, 0, 0x1000);

    STEP("usbDsInitialize", usbDsInitialize());
    STEP("Pro Controller descriptors", set_device_descriptors());
    STEP("Pro Controller interface", setup_interface());
    s_run = true;
    STEP("threadCreate rx", threadCreate(&s_rx_thread, rx_thread, NULL, NULL, 0x4000, 0x2C, -2));
    STEP("threadStart rx", threadStart(&s_rx_thread));
    STEP("threadCreate ctrl", threadCreate(&s_ctrl_thread, ctrl_thread, NULL, NULL, 0x4000, 0x2C, -2));
    STEP("threadStart ctrl", threadStart(&s_ctrl_thread));
    STEP("usbDsEnable", usbDsEnable());
    return 0;
fail:
    proconExit();
    return rc;
}

void proconExit(void) {
    if (s_run) {
        s_run = false;
        if (s_ep_out) usbDsEndpoint_Cancel(s_ep_out);
        threadWaitForExit(&s_rx_thread);   threadClose(&s_rx_thread);
        threadWaitForExit(&s_ctrl_thread); threadClose(&s_ctrl_thread);
    }
    if (s_ep_in && s_in_pending) usbDsEndpoint_Cancel(s_ep_in);
    s_in_pending = false;
    s_streaming = false;
    usbDsExit();
    s_intf = NULL; s_ep_in = s_ep_out = NULL;
    free(s_in_buf); free(s_out_buf); free(s_ctrl_buf);
    s_in_buf = s_out_buf = s_ctrl_buf = NULL;
    motion_exit();
    LOG("Pro Controller mode stopped");
}

bool proconIsConfigured(void) {
    UsbState st;
    return s_intf && R_SUCCEEDED(usbDsGetState(&st)) && st == UsbState_Configured;
}

bool proconIsStreaming(void) { return s_streaming; }

static Result post_in(const u8 *d) {
    memcpy(s_in_buf, d, 64);
    Result rc = usbDsEndpoint_PostBufferAsync(s_ep_in, s_in_buf, 64, &s_in_urb);
    if (R_SUCCEEDED(rc)) { s_in_pending = true; s_in_tick = armGetSystemTick(); }
    return rc;
}

Result proconUpdate(const InputState *in) {
    if (!s_ep_in) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    mutexLock(&s_curm); s_cur = *in; mutexUnlock(&s_curm);
    motion_read();

    bool cfg = proconIsConfigured();
    if (cfg != s_was_configured) {
        s_was_configured = cfg;
        LOG(cfg ? "Plugged in: host configured us" : "Unplugged");
        if (cfg) {                                     // announce ourselves like a freshly plugged controller
            u8 r[64] = { 0x81, 0x03 };            q_push(r);
            u8 r2[64] = { 0x81, 0x01, 0x00, 0x03 }; q_push(r2);
        } else {
            s_streaming = false;
        }
    }
    if (!cfg) return 0;

    if (s_in_pending) {
        if (R_FAILED(eventWait(&s_ep_in->CompletionEvent, 0))) {
            if (armTicksToNs(armGetSystemTick() - s_in_tick) < 100000000ULL) return 0;
            usbDsEndpoint_Cancel(s_ep_in);                 // host stopped polling: don't jam
            eventWait(&s_ep_in->CompletionEvent, 50000000ULL);
        }
        eventClear(&s_ep_in->CompletionEvent);
        UsbDsReportData rd;
        usbDsEndpoint_GetReportData(s_ep_in, &rd);
        s_in_pending = false;
    }

    u8 pkt[64];
    if (q_pop(pkt)) return post_in(pkt);

    u64 now = armTicksToNs(armGetSystemTick());
    // report_ms=0: a fresh report is queued as soon as the host has taken the last one,
    // so the data the host reads is never older than one poll interval.
    if (s_streaming && now - s_last30 >= (u64)s_report_ms * 1000000ULL) {
        s_last30 = now;
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 0x30;
        fill_input(&pkt[1], in);
        s16 m[6];
        motion_raw(m);
        for (int f = 0; f < 3; f++)                         // 3 IMU samples, same reading in each
            for (int k = 0; k < 6; k++) {
                pkt[13 + f * 12 + k * 2]     = (u8)(m[k] & 0xFF);
                pkt[13 + f * 12 + k * 2 + 1] = (u8)((u16)m[k] >> 8);
            }
        s_reports++;
        return post_in(pkt);
    }
    return 0;
}

void proconGetStats(ProconStats *o) {
    o->imu_source = motion_src_name();
    memcpy(o->accel, s_accel, sizeof s_accel);
    memcpy(o->gyro, s_gyro, sizeof s_gyro);
    o->reports = s_reports;
    o->replies = s_replies;
}
