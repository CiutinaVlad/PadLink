// PadLink - use a modded Nintendo Switch (Split Pad Pro / Joy-Cons) as a USB
// controller for a Switch 2, a Switch, or a PC.
// Run it as a full-RAM title (hold R while starting a game).
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "version.h"
#include "log.h"
#include "settings.h"
#include "input.h"
#include "procon.h"
#include "xinput.h"
#include "usb_hid.h"
#include "usb_vendor.h"
#include "wifi.h"

// ------------------------------------------------------------------ modes ---
typedef enum {
    MODE_SWITCH = 0,   // Pro Controller over USB (Switch 2 / Switch / Steam)
    MODE_XBOX,         // XInput over USB (Windows / Linux)
    MODE_GAMEPAD,      // generic USB HID gamepad
    MODE_WIFI,         // UDP to the PC receiver
    MODE_VENDOR,       // advanced: USB to the PC receiver
    MODE_HIDTEST,      // advanced: USB HID descriptors only (crash test)
    MODE_COUNT
} Mode;

static const char *k_mode_title[MODE_COUNT] = {
    "Switch 2 / Switch", "PC - Xbox controller", "PC / Mac - Generic gamepad",
    "PC - Wi-Fi", "USB to PC receiver", "USB HID crash test",
};

static const char *k_mode_help[MODE_COUNT] = {
    "Acts as a wired Pro Controller: buttons, sticks and motion.\n"
    "  On the Switch 2 turn on: Settings > Controllers & Accessories >\n"
    "  Pro Controller Wired Communication. Also works with Steam on a PC.",
    "Acts as a wired Xbox controller. Windows and Linux have the driver\n"
    "  built in: nothing to install. Works with Steam and most PC games.",
    "Acts as a standard USB gamepad. Basic support everywhere,\n"
    "  including macOS. No motion.",
    "No cable. Run the PadLink receiver on the PC (see the GitHub page)\n"
    "  and keep both on the same network.",
    "For testing: sends to padlink_receiver.py --usb on a PC.",
    "For troubleshooting USB crashes: descriptors only, no gamepad.",
};

// --------------------------------------------------------------------- UI ---
#define C_RESET  "\x1b[0m"
#define C_TITLE  "\x1b[1;37;44m"
#define C_SEL    "\x1b[30;46m"
#define C_DIM    "\x1b[37m"
#define C_OK     "\x1b[1;32m"
#define C_WARN   "\x1b[1;33m"
#define C_ACCENT "\x1b[1;36m"

static void ui_header(const char *sub) {
    printf("\x1b[2J\x1b[1;1H");
    printf(C_TITLE " PadLink %-6s%-65s" C_RESET "\n", PADLINK_APP_VERSION, sub);
    printf("\n");
}

static void ui_footer(const char *text) {
    printf("\x1b[44;1H" C_DIM " %s" C_RESET, text);
}

static void ui_flush(void) { logLock(); consoleUpdate(NULL); logUnlock(); }

static u64 now_us(void) { return armTicksToNs(armGetSystemTick()) / 1000ULL; }

// Generic vertical menu. Returns the chosen index, or -1 for B.
static int ui_menu(PadState *pad, const char *sub, const char **items, const char **help, int n, int sel) {
    bool redraw = true;
    while (appletMainLoop()) {
        if (redraw) {
            ui_header(sub);
            for (int i = 0; i < n; i++)
                printf("  %s %-40s " C_RESET "\n\n", i == sel ? C_SEL ">" : " ", items[i]);
            if (help && help[sel]) printf("\x1b[30;1H" C_ACCENT "  %s" C_RESET "\n", help[sel]);
            ui_footer("Up/Down: choose    A: select    B: back");
            ui_flush();
            redraw = false;
        }
        padUpdate(pad);
        u64 d = padGetButtonsDown(pad);
        if (d & (HidNpadButton_AnyUp))   { sel = (sel + n - 1) % n; redraw = true; }
        if (d & (HidNpadButton_AnyDown)) { sel = (sel + 1) % n; redraw = true; }
        if (d & HidNpadButton_A) return sel;
        if (d & HidNpadButton_B) return -1;
        svcSleepThread(16000000ULL);
    }
    return -1;
}

// ---------------------------------------------------------------- settings --
static void settings_screen(PadState *pad) {
    int sel = 0;
    const int n = 8;
    bool redraw = true;
    while (appletMainLoop()) {
        if (redraw) {
            char v[8][40];
            snprintf(v[0], 40, "%s", motionSourceName(g_settings.motion));
            snprintf(v[1], 40, "%s", g_settings.invert[0] ? "Inverted" : "Normal");
            snprintf(v[2], 40, "%s", g_settings.invert[1] ? "Inverted" : "Normal");
            snprintf(v[3], 40, "%s", g_settings.invert[2] ? "Inverted" : "Normal");
            snprintf(v[4], 40, "%.2fx", g_settings.gyro_scale);
            snprintf(v[5], 40, "%s", g_settings.low_latency ? "Lowest" : "Standard (8 ms)");
            snprintf(v[6], 40, "%s", g_settings.xbox_layout == LAYOUT_LABEL ? "By label (A = A)" : "By position");
            snprintf(v[7], 40, "%s", g_settings.autostart ? "Last used mode" : "Off");
            static const char *names[8] = { "Motion source", "Tilt axis X", "Tilt axis Y", "Tilt axis Z",
                                            "Gyro sensitivity", "Latency", "Xbox face buttons", "Start automatically" };
            static const char *help[8] = {
                "Auto uses the controller's motion sensor if it has one (Joy-Cons),\n  otherwise the console's own sensor (Split Pad Pro).",
                "Flip this if tilting goes the wrong way on one axis.",
                "Flip this if tilting goes the wrong way on one axis.",
                "Flip this if tilting goes the wrong way on one axis.",
                "How strongly turning the Switch turns the camera.",
                "Lowest sends a fresh report at every USB poll. Standard behaves\n  exactly like a real Pro Controller (use it if something acts up).",
                "By position: the bottom button is always Xbox A.\n  By label: the Switch A button is Xbox A.",
                "Skip this menu next time and start the last mode right away.\n  Hold B while PadLink starts to get the menu back.",
            };
            ui_header("Settings");
            for (int i = 0; i < n; i++)
                printf("  %s %-22s %-24s " C_RESET "\n\n", i == sel ? C_SEL ">" : " ", names[i], v[i]);
            printf("\x1b[30;1H" C_ACCENT "  %s" C_RESET "\n", help[sel]);
            ui_footer("Up/Down: choose    Left/Right or A: change    B: save and go back");
            ui_flush();
            redraw = false;
        }
        padUpdate(pad);
        u64 d = padGetButtonsDown(pad);
        int dir = (d & (HidNpadButton_AnyRight | HidNpadButton_A)) ? 1 : (d & HidNpadButton_AnyLeft) ? -1 : 0;
        if (d & HidNpadButton_AnyUp)   { sel = (sel + n - 1) % n; redraw = true; }
        if (d & HidNpadButton_AnyDown) { sel = (sel + 1) % n; redraw = true; }
        if (dir) {
            redraw = true;
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
        if (d & HidNpadButton_B) { settingsSave(); return; }
        svcSleepThread(16000000ULL);
    }
    settingsSave();
}

// ------------------------------------------------------------- running ------
static Result mode_start(Mode m) {
    switch (m) {
        case MODE_SWITCH:  return proconInit();
        case MODE_XBOX:    return xinputInit();
        case MODE_GAMEPAD: return usbHidInit(2);
        case MODE_WIFI:    return wifiInit();
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
        case MODE_VENDOR:  usbVendorExit(); break;
        default: break;
    }
}

static void line_status(bool ok, const char *good, const char *waiting) {
    printf("\x1b[K  %s%s" C_RESET "\n", ok ? C_OK : C_WARN, ok ? good : waiting);
}

static void draw_run(Mode m, const InputState *in) {
    logLock();
    printf("\x1b[4;1H");
    printf("\x1b[K  " C_ACCENT "%s" C_RESET "\n\x1b[K\n", k_mode_title[m]);
    switch (m) {
        case MODE_SWITCH: {
            ProconStats p; proconGetStats(&p);
            if (!proconIsConfigured()) line_status(false, "", "Plug the USB cable into the Switch 2 (or dock / PC).");
            else line_status(proconIsStreaming(), "Connected - play!", "Connecting... (on the Switch 2: Pro Controller Wired Communication ON)");
            printf("\x1b[K\n\x1b[K  Motion: %s\n", p.imu_source);
            printf("\x1b[K\n\x1b[K  HOME    = hold Minus + click right stick\n");
            printf("\x1b[K  Capture = hold Minus + click left stick\n");
            break;
        }
        case MODE_XBOX:
            line_status(xinputIsConfigured(), "Connected - play!", "Plug the USB cable into the PC.");
            printf("\x1b[K\n\x1b[K  Guide button = hold Minus + click right stick\n\x1b[K\n");
            break;
        case MODE_GAMEPAD: case MODE_HIDTEST:
            line_status(usbHidIsConfigured(), "Connected.", "Plug the USB cable into the computer.");
            printf("\x1b[K\n\x1b[K\n\x1b[K\n");
            break;
        case MODE_VENDOR:
            line_status(usbVendorIsConfigured(), "Connected to the receiver.", "Plug in and run: padlink_receiver.py --usb");
            printf("\x1b[K\n\x1b[K\n\x1b[K\n");
            break;
        case MODE_WIFI: {
            WifiStats w; wifiGetStats(&w);
            if (w.peer_known) {
                char t[96]; snprintf(t, sizeof t, "Connected to %s", w.peer_ip);
                line_status(true, t, "");
            } else line_status(false, "", "Looking for the PadLink receiver on your network...");
            printf("\x1b[K\n\x1b[K  Round trip (measured): %.1f ms typical, %.1f ms worst of last 512\n",
                   w.rtt_median_ms, w.rtt_max_ms);
            printf("\x1b[K\n");
            break;
        }
        default: break;
    }
    // small live input readout so you can see it reacting
    printf("\x1b[22;1H\x1b[K" C_DIM "  Input: buttons %06llX   L %+6d %+6d   R %+6d %+6d" C_RESET "\n",
           (unsigned long long)(in->buttons & 0xFFFFFF), in->ls.x, in->ls.y, in->rs.x, in->rs.y);
    consoleUpdate(NULL);
    logUnlock();
}

// Returns false if the app should exit (HOME > close).
static bool run_mode(PadState *pad, Mode m) {
    g_settings.last_mode = m;
    settingsSave();
    ui_header(k_mode_title[m]);
    printf("\x1b[4;1H  Starting...\n");
    ui_flush();
    LOG("start mode %d (%s)", m, k_mode_title[m]);
    Result rc = mode_start(m);
    if (R_FAILED(rc)) {
        ui_header(k_mode_title[m]);
        printf("\x1b[4;1H" C_WARN "  Couldn't start (error 0x%X)." C_RESET "\n\n", (unsigned)rc);
        if (m == MODE_WIFI) printf("  Is the Switch connected to Wi-Fi?\n");
        else printf("  Close DBI, Goldleaf, Tinfoil or anything else using USB, then try again.\n");
        printf("\n  Details: sdmc:/switch/padlink/padlink.log\n");
        ui_footer("Press B to go back");
        ui_flush();
        while (appletMainLoop()) {
            padUpdate(pad);
            if (padGetButtonsDown(pad) & HidNpadButton_B) break;
            svcSleepThread(16000000ULL);
        }
        mode_stop(m);
        return true;
    }

    ui_header(k_mode_title[m]);
    ui_footer("Stop: hold + and - for 2 seconds.   Don't press HOME while playing.");
    InputState in;
    PadlinkPacket pkt;
    u32 seq = 0;
    bool dirty = true, keep_app = true;
    u64 last_send = 0, last_draw = 0, quit_since = 0;
    Result last_err = 0;

    while (true) {
        if (!appletMainLoop()) { keep_app = false; break; }
        bool changed = inputRead(pad, &in);
        u64 t = now_us();
        if ((in.buttons & HidNpadButton_Plus) && (in.buttons & HidNpadButton_Minus)) {
            if (!quit_since) quit_since = t;
            else if (t - quit_since > 2000000) break;
        } else quit_since = 0;
        dirty |= changed;

        Result src = 0;
        switch (m) {
            case MODE_SWITCH: src = proconUpdate(&in); break;
            case MODE_XBOX:   src = xinputUpdate(&in); break;
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

        if (t - last_draw >= 250000) { draw_run(m, &in); last_draw = now_us(); }
        svcSleepThread(500000ULL);
    }
    mode_stop(m);
    LOG("stopped mode %d", m);
    return keep_app;
}

// ------------------------------------------------------------------- main ---
int main(void) {
    consoleInit(NULL);
    logOpen();
    logSetEcho(false);
    settingsLoad();
    PadState pad;
    inputInit(&pad);

    // Autostart: go straight to the last mode unless B is held at launch.
    padUpdate(&pad);
    bool running = true;
    if (g_settings.autostart && !(padGetButtons(&pad) & HidNpadButton_B) &&
        g_settings.last_mode >= 0 && g_settings.last_mode <= MODE_WIFI)
        running = run_mode(&pad, (Mode)g_settings.last_mode);

    static const char *main_items[] = {
        "Switch 2 / Switch", "PC - Xbox controller", "PC / Mac - Generic gamepad", "PC - Wi-Fi",
        "Settings", "Advanced", "Quit",
    };
    const char *main_help[] = {
        k_mode_help[0], k_mode_help[1], k_mode_help[2], k_mode_help[3],
        "Motion, tilt direction, sensitivity, latency, button layout, autostart.",
        "Testing and troubleshooting tools.",
        "Back to the homebrew menu.",
    };

    int sel = (g_settings.last_mode >= 0 && g_settings.last_mode <= MODE_WIFI) ? g_settings.last_mode : 0;
    while (running) {
        int c = ui_menu(&pad, "Use this Switch as a controller", main_items, main_help, 7, sel);
        if (c < 0 || c == 6) break;
        sel = c;
        if (c <= 3) running = run_mode(&pad, (Mode)c);
        else if (c == 4) settings_screen(&pad);
        else if (c == 5) {
            static const char *adv[] = { "USB to PC receiver", "USB HID crash test", "Back" };
            const char *advh[] = { k_mode_help[MODE_VENDOR], k_mode_help[MODE_HIDTEST], NULL };
            int a = ui_menu(&pad, "Advanced", adv, advh, 3, 0);
            if (a == 0) running = run_mode(&pad, MODE_VENDOR);
            else if (a == 1) running = run_mode(&pad, MODE_HIDTEST);
        }
    }

    logClose();
    consoleExit(NULL);
    return 0;
}
