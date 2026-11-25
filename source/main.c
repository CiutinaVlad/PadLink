// PadLink - use a modded Nintendo Switch (Split Pad Pro / Joy-Cons) as a USB
// controller for a Switch 2, a Switch, or a PC.
//
// Threads: the main thread draws the SDL2 UI (vsync'd, 60 fps). While a mode
// runs, a separate higher-priority worker thread reads the controllers and feeds
// USB / Wi-Fi every 0.5 ms, so drawing never adds latency.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <SDL2/SDL.h>
#include "version.h"
#include "log.h"
#include "settings.h"
#include "input.h"
#include "procon.h"
#include "xinput.h"
#include "usb_hid.h"
#include "usb_vendor.h"
#include "wifi.h"
#include "motion.h"
#include "ui.h"

// ------------------------------------------------------------------ modes ---
typedef enum {
    MODE_SWITCH = 0,   // Pro Controller over USB (Switch 2 / Switch / Steam)
    MODE_XBOX,         // XInput over USB (Windows / Linux)
    MODE_GAMEPAD,      // generic USB HID gamepad
    MODE_WIFI,         // UDP to the PC receiver
    MODE_CEMU,         // Wi-Fi + motion + touch to the PC receiver, served to Cemu (Wii U)
    MODE_VENDOR,       // advanced: USB to the PC receiver
    MODE_HIDTEST,      // advanced: USB HID descriptors only (crash test)
    MODE_COUNT
} Mode;

#define NCARDS 6
static const UiCard k_cards[NCARDS] = {
    { "Switch 2 / Switch", "Wired Pro Controller with motion", "USB · Switch 2 · Steam",   UI_ICON_SWITCH,  0xff4554 },
    { "Xbox controller",   "Nothing to install on the PC",     "USB · Windows · Linux",    UI_ICON_XBOX,    0x3ccf4e },
    { "Wii U (Cemu)",      "GamePad with touch and motion",    "Wi-Fi · Cemu on PC",       UI_ICON_TABLET,  0x26c6da },
    { "Generic gamepad",   "Standard USB gamepad, no motion",  "USB · PC · Mac · Android", UI_ICON_GAMEPAD, 0x4c9dff },
    { "PC over Wi-Fi",     "No cable, needs the PC receiver",  "Wi-Fi · Windows · Linux",  UI_ICON_WIFI,    0xb57bff },
    { "Settings",          "Motion, latency, layout, tools",   "",                         UI_ICON_WRENCH,  0x9aa3b5 },
};
static const int k_card_mode[NCARDS] = { MODE_SWITCH, MODE_XBOX, MODE_CEMU, MODE_GAMEPAD, MODE_WIFI, -1 };
static const char *k_mode_title[MODE_COUNT] = {
    "Switch 2 / Switch", "Xbox controller", "Generic gamepad", "PC over Wi-Fi", "Wii U (Cemu)", "USB to PC receiver", "USB HID test",
};
static const UiIcon k_mode_icon[MODE_COUNT] = {
    UI_ICON_SWITCH, UI_ICON_XBOX, UI_ICON_GAMEPAD, UI_ICON_WIFI, UI_ICON_TABLET, UI_ICON_USB, UI_ICON_WRENCH,
};
static const uint32_t k_mode_accent[MODE_COUNT] = { 0xff4554, 0x3ccf4e, 0x4c9dff, 0xb57bff, 0x26c6da, 0x4cc2ff, 0xf5b942 };

static u64 now_us(void) { return armTicksToNs(armGetSystemTick()) / 1000ULL; }

// ---------------------------------------------------------- input helpers ---
static PadState s_pad;          // UI thread's pad (menus only)
static bool s_quit_app;         // HOME > close, or Quit

typedef struct { u64 down; int tap_x, tap_y; bool tap; } UiInput;

// Pumps SDL events (needed for the applet loop and touch) and reads buttons,
// with key repeat on the D-pad / left stick.
static UiInput ui_poll(void) {
    static u64 held_since, last_rep;
    UiInput in = { 0 };
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) s_quit_app = true;
        else if (ev.type == SDL_FINGERDOWN) {
            in.tap = true; in.tap_x = (int)(ev.tfinger.x * UI_W); in.tap_y = (int)(ev.tfinger.y * UI_H);
        }
    }
    padUpdate(&s_pad);
    in.down = padGetButtonsDown(&s_pad);
    const u64 dirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
    u64 held = padGetButtons(&s_pad) & dirs, t = now_us();
    if (!held) held_since = 0;
    else if (in.down & dirs) { held_since = t; last_rep = t; }
    else if (held_since && t - held_since > 400000 && t - last_rep > 110000) { in.down |= held; last_rep = t; }
    return in;
}

// --------------------------------------------------------- worker thread ---
typedef struct {
    Mode mode;
    volatile bool stop;          // UI -> worker
    volatile bool started, done; // worker -> UI
    volatile Result start_rc;
    Mutex lock;
    InputState in;               // latest input, for the visualizer
    float quit_progress;
} Worker;
static Worker W;

static Result mode_start(Mode m) {
    switch (m) {
        case MODE_SWITCH:  return proconInit();
        case MODE_XBOX:    return xinputInit();
        case MODE_GAMEPAD: return usbHidInit(2);
        case MODE_WIFI:    return wifiInit();
        case MODE_CEMU: {
            Result rc = wifiInit();
            if (R_SUCCEEDED(rc)) motionStart();
            return rc;
        }
        case MODE_VENDOR:  return usbVendorInit();
        case MODE_HIDTEST: return usbHidInit(1);
        default:           return 0;
    }
}

static void mode_stop(Mode m) {
    switch (m) {
        case MODE_SWITCH:  proconExit(); break;
        case MODE_XBOX:    xinputExit(); break;
        case MODE_GAMEPAD: case MODE_HIDTEST: usbHidExit(); break;
        case MODE_WIFI:    wifiExit(); break;
        case MODE_CEMU:    motionStop(); wifiExit(); break;
        case MODE_VENDOR:  usbVendorExit(); break;
        default: break;
    }
}

static void worker_main(void *arg) {
    (void)arg;
    Mode m = W.mode;
    LOG("start mode %d (%s)", m, k_mode_title[m]);
    Result rc = mode_start(m);
    W.start_rc = rc;
    if (R_FAILED(rc)) {
        LOG_RC("mode start", rc);
        mode_stop(m);
        W.done = true;
        return;
    }
    W.started = true;

    PadState pad;
    padInitializeDefault(&pad);
    InputState in;
    PadlinkPacket pkt;
    PadlinkMotionPacket mpkt;
    u32 seq = 0;
    bool dirty = true;
    u64 last_send = 0, last_share = 0, quit_since = 0;
    Result last_err = 0;

    while (!W.stop) {
        bool changed = inputRead(&pad, &in);
        u64 t = now_us();
        float qp = 0;
        if ((in.buttons & HidNpadButton_Plus) && (in.buttons & HidNpadButton_Minus)) {
            if (!quit_since) quit_since = t;
            qp = (t - quit_since) / 2000000.0f;
            if (qp >= 1.0f) break;
        } else quit_since = 0;
        dirty |= changed;

        Result src = 0;
        switch (m) {
            case MODE_SWITCH: src = proconUpdate(&in); break;
            case MODE_XBOX:   src = xinputUpdate(&in); break;
            case MODE_CEMU:
                motionUpdate();
                if (dirty || t - last_send >= 4000) {     // 250 Hz motion, buttons right away
                    inputToPacket(&in, &mpkt.base, seq++);
                    mpkt.base.type = PADLINK_TYPE_STATE_MOTION;
                    float ma[3], mg[3];
                    motionGet(ma, mg);
                    memcpy(mpkt.accel_g, ma, sizeof ma); memcpy(mpkt.gyro_dps, mg, sizeof mg);
                    src = wifiSendRaw(&mpkt, sizeof mpkt);
                    last_send = t; dirty = false;
                    wifiPoll();
                }
                break;
            case MODE_WIFI:
                if (dirty || t - last_send >= 4000) {
                    inputToPacket(&in, &pkt, seq++);
                    src = wifiSend(&pkt);
                    last_send = t; dirty = false;
                    wifiPoll();
                }
                break;
            default: {
                if (t - last_send >= 8000) dirty = true;
                if (dirty) {
                    bool queued = false;
                    if (m == MODE_VENDOR) { inputToPacket(&in, &pkt, seq); src = usbVendorSend(&pkt, &queued); if (queued) seq++; }
                    else src = usbHidSendPad(&in, &queued);
                    if (queued) { dirty = false; last_send = t; }
                }
            }
        }
        if (R_FAILED(src) && src != last_err) LOG_RC("send", src);
        last_err = src;

        if (t - last_share >= 8000) {      // hand a copy to the UI ~120x/s
            mutexLock(&W.lock);
            W.in = in; W.quit_progress = qp;
            mutexUnlock(&W.lock);
            last_share = t;
        }
        svcSleepThread(500000ULL);
    }
    mode_stop(m);
    LOG("stopped mode %d", m);
    W.done = true;
}

// ------------------------------------------------------------- run screen ---
static const char *motion_label(const char *src) {
    if (!src) return "Off";
    if (!strcmp(src, "console")) return "Console motion sensor";
    if (!strcmp(src, "controller")) return "Controller motion sensor";
    if (!strncmp(src, "calibrating", 11)) return "Calibrating - keep still";
    if (!strcmp(src, "none")) return "Motion off";
    return src;
}

static void fill_run_view(Mode m, UiRun *v, char *stat, size_t stat_n, char *st, size_t st_n) {
    v->title = k_mode_title[m]; v->icon = k_mode_icon[m]; v->accent = k_mode_accent[m];
    mutexLock(&W.lock);
    InputState in = W.in; v->quit_progress = W.quit_progress;
    mutexUnlock(&W.lock);
    v->buttons = (uint32_t)(in.buttons & 0xFFFF);   // UIB_* match HidNpadButton bits
    v->lsx = in.ls.x; v->lsy = in.ls.y; v->rsx = in.rs.x; v->rsy = in.rs.y;
    v->stat_text = stat; stat[0] = 0;
    if (!W.started) { v->status = UI_ST_STARTING; v->status_text = "Starting..."; return; }

    switch (m) {
        case MODE_SWITCH: {
            ProconStats p; proconGetStats(&p);
            v->show_motion = true;
            v->motion_text = motion_label(p.imu_source);
            for (int i = 0; i < 3; i++) v->accel[i] = p.accel[i];
            snprintf(stat, stat_n, "%u reports", (unsigned)p.reports);
            if (!proconIsConfigured()) {
                v->status = UI_ST_WAITING; v->status_text = "Plug in the USB cable";
                v->lines[0] = "Connect to the Switch 2 (or its dock) with a\nUSB-C cable that carries data";
                v->lines[1] = "On the Switch 2: Settings > Controllers & Accessories\n> Pro Controller Wired Communication: On";
            } else if (!proconIsStreaming()) {
                v->status = UI_ST_CONNECTING; v->status_text = "Connecting...";
                v->lines[0] = "If this takes long, check that Pro Controller\nWired Communication is On on the Switch 2";
            } else {
                v->status = UI_ST_CONNECTED; v->status_text = "Connected - play!";
                v->lines[0] = "HOME: hold - and click the right stick";
                v->lines[1] = "Capture: hold - and click the left stick";
                v->lines[2] = "Tilt goes the wrong way? Settings > Tilt axis";
            }
            break;
        }
        case MODE_XBOX:
            snprintf(stat, stat_n, "%u reports", (unsigned)xinputReports());
            if (xinputIsConfigured()) {
                v->status = UI_ST_CONNECTED; v->status_text = "Connected - play!";
                v->lines[0] = "Guide button: hold - and click the right stick";
                v->lines[1] = "Face buttons by position or by label:\nchange it in Settings";
            } else {
                v->status = UI_ST_WAITING; v->status_text = "Plug the cable into the PC";
                v->lines[0] = "Windows and Linux have the driver built in";
                v->lines[1] = "Works with Steam and most PC games";
            }
            break;
        case MODE_GAMEPAD: case MODE_HIDTEST:
            snprintf(stat, stat_n, "%u reports", (unsigned)usbHidCompleted());
            if (usbHidIsConfigured()) { v->status = UI_ST_CONNECTED; v->status_text = "Connected"; }
            else { v->status = UI_ST_WAITING; v->status_text = "Plug the cable into the computer"; }
            v->lines[0] = m == MODE_HIDTEST ? "Descriptors only: for checking USB stability"
                                            : "Works on PC, Mac and Android without drivers";
            break;
        case MODE_VENDOR:
            snprintf(stat, stat_n, "%u packets", (unsigned)usbVendorCompleted());
            if (usbVendorIsConfigured()) { v->status = UI_ST_CONNECTED; v->status_text = "Connected to the receiver"; }
            else { v->status = UI_ST_WAITING; v->status_text = "Plug in and start the receiver"; }
            v->lines[0] = "On the PC: python padlink_receiver.py --usb";
            break;
        case MODE_CEMU: {
            WifiStats w; wifiGetStats(&w);
            float g[3];
            v->show_motion = true;
            v->motion_text = motion_label(motionActiveSource());
            motionGetRaw(v->accel, g);
            v->touch_passthrough = true;
            v->touching = in.touch; v->touch_x = in.touch_x; v->touch_y = in.touch_y;
            if (w.peer_known) {
                (void)st; (void)st_n;
                v->status = UI_ST_CONNECTED; v->status_text = "Connected - play!";
                snprintf(stat, stat_n, "PC %s  ·  round trip %.1f ms", w.peer_ip, w.rtt_median_ms);
                v->lines[0] = "Touch this screen to touch the GamePad";
                v->lines[1] = "In Cemu: Options > Separate GamePad view,\nor swap screens so the GamePad is shown";
                v->lines[2] = "HOME is - plus a right-stick click";
            } else {
                v->status = UI_ST_WAITING; v->status_text = "Looking for the receiver...";
                v->lines[0] = "On the PC run: padlink_receiver.py --cemu";
                v->lines[1] = "In Cemu input settings pick DSUController,\n127.0.0.1 port 26760";
                v->lines[2] = "Keep the Switch and the PC on the same network";
            }
            break;
        }
        case MODE_WIFI: {
            WifiStats w; wifiGetStats(&w);
            if (w.peer_known) {
                v->status = UI_ST_CONNECTED; v->status_text = "Connected - play!";
                snprintf(stat, stat_n, "PC %s  ·  round trip %.1f ms (worst %.0f)", w.peer_ip, w.rtt_median_ms, w.rtt_max_ms);
            } else {
                v->status = UI_ST_WAITING; v->status_text = "Looking for the receiver...";
            }
            v->lines[0] = "Run PadLink Receiver on the PC\n(PadLink-PC-receiver.zip on the GitHub page)";
            v->lines[1] = "Keep the Switch and the PC on the same network";
            v->lines[2] = "Turn off Wi-Fi power saving on the PC";
            break;
        }
        default: break;
    }
}

static void error_screen(const char *title, const char *msg, const char *detail) {
    UiError e = { title, msg, detail };
    while (!s_quit_app) {
        UiInput in = ui_poll();
        if ((in.down & (HidNpadButton_B | HidNpadButton_A)) || in.tap) break;
        ui_draw_error(&e);
    }
}

static void run_mode(Mode m) {
    g_settings.last_mode = m;
    settingsSave();
    memset(&W, 0, sizeof W);
    mutexInit(&W.lock);
    W.mode = m;
    Thread th;
    // Higher priority than the UI (0x2C); it sleeps 0.5 ms per loop so the UI still runs.
    Result rc = threadCreate(&th, worker_main, NULL, NULL, 0x10000, 0x2B, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&th);
    if (R_FAILED(rc)) { LOG_RC("worker thread", rc); error_screen("Couldn't start", "Internal error.", NULL); return; }

    char stat[96], st[64];
    u32 t0 = SDL_GetTicks();
    while (!W.done) {
        UiInput in = ui_poll();
        if (s_quit_app) W.stop = true;
        if (in.tap && m != MODE_CEMU && ui_hit(in.tap_x, in.tap_y) == 0) W.stop = true;
        UiRun v; memset(&v, 0, sizeof v);
        fill_run_view(m, &v, stat, sizeof stat, st, sizeof st);
        ui_draw_run(&v, SDL_GetTicks() - t0);
    }
    threadWaitForExit(&th);
    threadClose(&th);

    if (R_FAILED(W.start_rc)) {
        char d[96];
        snprintf(d, sizeof d, "Error 0x%X (%04d-%04d)  ·  log: sdmc:/switch/padlink/padlink.log",
                 (unsigned)W.start_rc, 2000 + R_MODULE(W.start_rc), R_DESCRIPTION(W.start_rc));
        if (m == MODE_WIFI || m == MODE_CEMU) error_screen("Couldn't start Wi-Fi", "Is the Switch connected to a Wi-Fi network?", d);
        else error_screen("Couldn't start USB", "Close DBI, Goldleaf, Tinfoil or anything else\nthat uses USB, then try again.", d);
    }
}

// --------------------------------------------------------------- settings ---
static void advanced_screen(void);
static void settings_screen(void) {
    enum { N = 9 };
    static const char *names[N] = { "Motion source", "Tilt axis X", "Tilt axis Y", "Tilt axis Z",
                                    "Gyro sensitivity", "Latency", "Xbox face buttons", "Start automatically",
                                    "Advanced tools" };
    static const char *help[N] = {
        "Auto uses the controller's motion sensor if it has one (Joy-Cons),\notherwise the console's own sensor (Split Pad Pro).",
        "Flip this if tilting goes the wrong way on this axis.",
        "Flip this if tilting goes the wrong way on this axis.",
        "Flip this if tilting goes the wrong way on this axis.",
        "How strongly turning the Switch turns the camera.",
        "Lowest sends a fresh report at every USB poll. Standard behaves exactly\nlike a real Pro Controller (8 ms). Use it if something acts up.",
        "By position: the bottom button is always Xbox A.\nBy label: the Switch A button is Xbox A.",
        "Skip the menu and start the last used mode right away.\nHold B while PadLink starts to get the menu back.",
        "USB to the PC receiver, and a USB test for troubleshooting crashes.",
    };
    int sel = 0;
    while (!s_quit_app) {
        char v[N][32];
        const char *vals[N];
        snprintf(v[0], 32, "%s", motionSourceName(g_settings.motion));
        for (int i = 0; i < 3; i++) snprintf(v[1 + i], 32, "%s", g_settings.invert[i] ? "Inverted" : "Normal");
        snprintf(v[4], 32, "%.2fx", g_settings.gyro_scale);
        snprintf(v[5], 32, "%s", g_settings.low_latency ? "Lowest" : "Standard");
        snprintf(v[6], 32, "%s", g_settings.xbox_layout == LAYOUT_LABEL ? "By label" : "By position");
        snprintf(v[7], 32, "%s", g_settings.autostart ? "Last used mode" : "Off");
        for (int i = 0; i < N - 1; i++) vals[i] = v[i];
        vals[N - 1] = NULL;

        UiList l = { "Settings", names, vals, help, N, sel, NULL };
        ui_draw_list(&l);

        UiInput in = ui_poll();
        int dir = (in.down & (HidNpadButton_AnyRight | HidNpadButton_A)) ? 1 : (in.down & HidNpadButton_AnyLeft) ? -1 : 0;
        if (in.down & HidNpadButton_AnyUp)   sel = (sel + N - 1) % N;
        if (in.down & HidNpadButton_AnyDown) sel = (sel + 1) % N;
        if (in.tap) {
            int h = ui_hit(in.tap_x, in.tap_y);
            if (h == 9999) break;
            if (h >= 2000) { sel = h - 2000; dir = 1; }
            else if (h >= 1000) { sel = h - 1000; dir = -1; }
            else if (h >= 0) { if (h == sel) dir = 1; sel = h; }
        }
        if (in.down & HidNpadButton_B) break;
        if (sel == N - 1) {
            if ((in.down & HidNpadButton_A) || (in.tap && dir)) advanced_screen();
            dir = 0;
        }
        if (dir) {
            switch (sel) {
                case 0: g_settings.motion = (MotionSource)((g_settings.motion + MOTION_COUNT + dir) % MOTION_COUNT); break;
                case 1: case 2: case 3: g_settings.invert[sel - 1] = !g_settings.invert[sel - 1]; break;
                case 4:
                    g_settings.gyro_scale += 0.25f * dir;
                    if (g_settings.gyro_scale < 0.25f) g_settings.gyro_scale = 0.25f;
                    if (g_settings.gyro_scale > 4.0f) g_settings.gyro_scale = 4.0f;
                    break;
                case 5: g_settings.low_latency = !g_settings.low_latency; break;
                case 6: g_settings.xbox_layout = g_settings.xbox_layout == LAYOUT_LABEL ? LAYOUT_POSITION : LAYOUT_LABEL; break;
                case 7: g_settings.autostart = !g_settings.autostart; break;
            }
        }
    }
    settingsSave();
}

static void advanced_screen(void) {
    static const char *names[2] = { "USB to PC receiver", "USB HID test" };
    static const char *help[2] = {
        "Sends to padlink_receiver.py --usb on a PC.\nNeeds the WinUSB driver on Windows.",
        "Shows up as a USB device with no gamepad.\nFor troubleshooting USB crashes with other homebrew.",
    };
    int sel = 0;
    while (!s_quit_app) {
        UiList l = { "Advanced", names, NULL, help, 2, sel, NULL };
        ui_draw_list(&l);
        UiInput in = ui_poll();
        if (in.down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) sel ^= 1;
        if (in.down & HidNpadButton_B) return;
        int pick = (in.down & HidNpadButton_A) ? sel : -1;
        if (in.tap) {
            int h = ui_hit(in.tap_x, in.tap_y);
            if (h == 9999) return;
            if (h == 0 || h == 1) pick = h;
        }
        if (pick >= 0) { run_mode(pick == 0 ? MODE_VENDOR : MODE_HIDTEST); return; }
    }
}

// ------------------------------------------------------------------- main ---
int main(void) {
    logOpen();
    logSetEcho(false);
    settingsLoad();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&s_pad);
    hidInitializeTouchScreen();

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { LOG("SDL_Init: %s", SDL_GetError()); logClose(); return 1; }
    SDL_Window *win = SDL_CreateWindow("PadLink", 0, 0, UI_W, UI_H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    PlFontData font = { 0 };
    Result prc = plInitialize(PlServiceType_User);
    if (R_SUCCEEDED(prc)) prc = plGetSharedFontByType(&font, PlSharedFontType_Standard);
    if (!ren || R_FAILED(prc) || !ui_init(ren, font.address, font.size)) {
        LOG("UI init failed: %s rc=0x%X", SDL_GetError(), (unsigned)prc);
        SDL_Quit(); plExit(); logClose();
        return 1;
    }

    // Autostart: go straight to the last mode unless B is held at launch.
    padUpdate(&s_pad);
    if (g_settings.autostart && !(padGetButtons(&s_pad) & HidNpadButton_B) &&
        g_settings.last_mode >= 0 && g_settings.last_mode <= MODE_CEMU)
        run_mode((Mode)g_settings.last_mode);

    int sel = 0;
    for (int i = 0; i < NCARDS; i++) if (k_card_mode[i] == g_settings.last_mode) sel = i;
    while (!s_quit_app) {
        UiHome h = { PADLINK_APP_VERSION, k_cards, NCARDS, sel, NULL };
        ui_draw_home(&h);
        UiInput in = ui_poll();
        // grid: 0 1 / 2 3 / 4 5 (5 = Settings)
        if (in.down & HidNpadButton_AnyLeft)  sel = (sel % 2) ? sel - 1 : sel;
        if (in.down & HidNpadButton_AnyRight) sel = (sel % 2) ? sel : sel + 1;
        if (in.down & HidNpadButton_AnyUp)    sel = sel >= 2 ? sel - 2 : sel;
        if (in.down & HidNpadButton_AnyDown)  sel = sel + 2 < NCARDS ? sel + 2 : sel;
        int act = (in.down & HidNpadButton_A) ? sel : -1;
        if (in.down & HidNpadButton_Y) act = NCARDS - 1;
        if (in.down & HidNpadButton_Plus) act = 6;
        if (in.tap) { int t = ui_hit(in.tap_x, in.tap_y); if (t >= 0) { act = t; if (t < NCARDS) sel = t; } }
        if (act == 6) break;
        if (act >= 0 && act < NCARDS) {
            if (k_card_mode[act] >= 0) run_mode((Mode)k_card_mode[act]);
            else settings_screen();
        }
    }

    ui_exit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    plExit();
    logClose();
    return 0;
}
