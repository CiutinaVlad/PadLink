// Renders PadLink's screens to PNG files on a PC, for checking the layout.
//   cc tools/ui_preview.c source/ui.c -Isource $(pkg-config --cflags --libs sdl2 SDL2_ttf SDL2_gfx) -lm -o ui_preview
//   ./ui_preview /path/to/font.ttf out_dir
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "ui.h"

static void save(SDL_Renderer *r, SDL_Surface *s, const char *dir, const char *name) {
    (void)r;
    char p[512]; snprintf(p, sizeof p, "%s/%s.bmp", dir, name);
    SDL_SaveBMP(s, p);
    printf("%s\n", p);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s font.ttf outdir\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *font = malloc(n); fread(font, 1, n, f); fclose(f);
    SDL_Init(0);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, UI_W, UI_H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(s);
    if (!ui_init(r, font, n)) { fprintf(stderr, "ui_init failed\n"); return 1; }

    UiCard cards[6] = {
        { "Switch 2 / Switch", "Wired Pro Controller with motion", "USB · Switch 2 · Steam",   UI_ICON_SWITCH,  0xff4554 },
        { "Xbox controller",   "Nothing to install on the PC",     "USB · Windows · Linux",    UI_ICON_XBOX,    0x3ccf4e },
        { "Wii U (Cemu)",      "GamePad with touch and motion",    "Wi-Fi · Cemu on PC",       UI_ICON_TABLET,  0x26c6da },
        { "Generic gamepad",   "Standard USB gamepad, no motion",  "USB · PC · Mac · Android", UI_ICON_GAMEPAD, 0x4c9dff },
        { "PC over Wi-Fi",     "No cable, needs the PC receiver",  "Wi-Fi · Windows · Linux",  UI_ICON_WIFI,    0xb57bff },
        { "Settings",          "Motion, latency, layout, tools",   "",                         UI_ICON_WRENCH,  0x9aa3b5 },
    };
    UiHome h = { "1.2.0", cards, 6, 2, NULL };
    ui_draw_home(&h); save(r, s, argv[2], "1_home");

    UiRun v = { .title = "Switch 2 / Switch", .icon = UI_ICON_SWITCH, .accent = 0xff4554,
        .status = UI_ST_CONNECTED, .status_text = "Connected - play!",
        .lines = { "HOME: hold - and click the right stick", "Capture: hold - and click the left stick",
                   "Switch 2: turn on Pro Controller Wired\nCommunication in Controllers settings", NULL },
        .buttons = UIB_A | UIB_ZR | UIB_RS, .lsx = 18000, .lsy = 22000, .rsx = -9000, .rsy = -26000,
        .show_motion = true, .motion_text = "Console motion sensor", .accel = { -0.25f, 0.30f, 0.92f },
        .stat_text = "48211 reports", .quit_progress = 0.0f };
    ui_draw_run(&v, 0); save(r, s, argv[2], "2_run_switch");

    UiRun c = { .title = "Wii U (Cemu)", .icon = UI_ICON_TABLET, .accent = 0x26c6da,
        .status = UI_ST_CONNECTED, .status_text = "Connected - play!",
        .lines = { "Touch this screen to touch the GamePad", "In Cemu: Options > Separate GamePad view,\nor swap screens so the GamePad is shown", "HOME is - plus a right-stick click", NULL },
        .buttons = UIB_ZL, .lsx = -12000, .lsy = 25000,
        .show_motion = true, .motion_text = "Console motion sensor", .accel = { 0.1f, -0.2f, 0.97f },
        .touch_passthrough = true, .touching = true, .touch_x = 980, .touch_y = 210,
        .stat_text = "PC 192.168.1.20  ·  round trip 4.1 ms" };
    ui_draw_run(&c, 0); save(r, s, argv[2], "2b_run_cemu");

    UiRun w = { .title = "PC over Wi-Fi", .icon = UI_ICON_WIFI, .accent = 0xb57bff,
        .status = UI_ST_WAITING, .status_text = "Looking for the receiver...",
        .lines = { "Run PadLink Receiver on the PC", "Keep both on the same network", NULL },
        .stat_text = "RTT 4.2 ms typical", .quit_progress = 0.45f };
    ui_draw_run(&w, 300); save(r, s, argv[2], "3_run_wifi");

    const char *names[9] = { "Motion source", "Tilt axis X", "Tilt axis Y", "Tilt axis Z", "Gyro sensitivity", "Latency", "Xbox face buttons", "Start automatically", "Advanced tools" };
    const char *vals[9] = { "Auto", "Normal", "Inverted", "Normal", "1.00x", "Lowest", "By position", "Off", NULL };
    const char *help[9] = { "Auto uses the controller's motion sensor if it has one (Joy-Cons),\notherwise the console's own sensor (Split Pad Pro).", 0,0,0,0,0,0,0,0 };
    UiList l = { "Settings", names, vals, help, 9, 0, NULL };
    ui_draw_list(&l); save(r, s, argv[2], "4_settings");

    UiError e = { "Couldn't start USB", "Close DBI, Goldleaf, Tinfoil or anything else\nthat uses USB, then try again.", "Error 0x2A2  ·  details in sdmc:/switch/padlink/padlink.log" };
    ui_draw_error(&e); save(r, s, argv[2], "5_error");
    ui_exit();
    return 0;
}
