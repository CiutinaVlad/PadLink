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

    UiCard cards[4] = {
        { "Switch 2 / Switch", "Wired Pro Controller with motion", "Switch 2 · Switch · Steam", UI_ICON_SWITCH, 0xff4554 },
        { "Xbox controller", "Nothing to install on the PC", "Windows · Linux · Steam", UI_ICON_XBOX, 0x3ccf4e },
        { "Generic gamepad", "Standard USB gamepad, no motion", "PC · Mac · Android", UI_ICON_GAMEPAD, 0x4c9dff },
        { "PC over Wi-Fi", "No cable, needs the PC receiver", "Windows · Mac · Linux", UI_ICON_WIFI, 0xb57bff },
    };
    UiHome h = { "1.1.0", cards, 4, 0, NULL };
    ui_draw_home(&h); save(r, s, argv[2], "1_home");

    UiRun v = { .title = "Switch 2 / Switch", .icon = UI_ICON_SWITCH, .accent = 0xff4554,
        .status = UI_ST_CONNECTED, .status_text = "Connected - play!",
        .lines = { "HOME: hold - and click the right stick", "Capture: hold - and click the left stick",
                   "Switch 2: turn on Pro Controller Wired\nCommunication in Controllers settings", NULL },
        .buttons = UIB_A | UIB_ZR | UIB_RS, .lsx = 18000, .lsy = 22000, .rsx = -9000, .rsy = -26000,
        .show_motion = true, .motion_text = "Console motion sensor", .accel = { -0.25f, 0.30f, 0.92f },
        .stat_text = "48211 reports", .quit_progress = 0.0f };
    ui_draw_run(&v, 0); save(r, s, argv[2], "2_run_switch");

    UiRun w = { .title = "PC over Wi-Fi", .icon = UI_ICON_WIFI, .accent = 0xb57bff,
        .status = UI_ST_WAITING, .status_text = "Looking for the receiver...",
        .lines = { "Run PadLink Receiver on the PC", "Keep both on the same network", NULL },
        .stat_text = "RTT 4.2 ms typical", .quit_progress = 0.45f };
    ui_draw_run(&w, 300); save(r, s, argv[2], "3_run_wifi");

    const char *names[8] = { "Motion source", "Tilt axis X", "Tilt axis Y", "Tilt axis Z", "Gyro sensitivity", "Latency", "Xbox face buttons", "Start automatically" };
    const char *vals[8] = { "Auto", "Normal", "Inverted", "Normal", "1.00x", "Lowest", "By position", "Off" };
    const char *help[8] = { "Auto uses the controller's motion sensor if it has one (Joy-Cons),\notherwise the console's own sensor (Split Pad Pro).", 0,0,0,0,0,0,0 };
    UiList l = { "Settings", names, vals, help, 8, 0, NULL };
    ui_draw_list(&l); save(r, s, argv[2], "4_settings");

    UiError e = { "Couldn't start USB", "Close DBI, Goldleaf, Tinfoil or anything else\nthat uses USB, then try again.", "Error 0x2A2  ·  details in sdmc:/switch/padlink/padlink.log" };
    ui_draw_error(&e); save(r, s, argv[2], "5_error");
    ui_exit();
    return 0;
}
