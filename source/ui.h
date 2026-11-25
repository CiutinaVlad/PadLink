#pragma once
// PadLink graphical UI (SDL2 + SDL2_ttf + SDL2_gfx), 1280x720.
// Pure drawing: no libnx calls in here, so the same code renders preview
// screenshots on a PC (see tools/ui_preview.c).
#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

#define UI_W 1280
#define UI_H 720

// Button bits used by the visualizer (same order as HidNpadButton).
enum {
    UIB_A = 1 << 0, UIB_B = 1 << 1, UIB_X = 1 << 2, UIB_Y = 1 << 3,
    UIB_LS = 1 << 4, UIB_RS = 1 << 5, UIB_L = 1 << 6, UIB_R = 1 << 7,
    UIB_ZL = 1 << 8, UIB_ZR = 1 << 9, UIB_PLUS = 1 << 10, UIB_MINUS = 1 << 11,
    UIB_LEFT = 1 << 12, UIB_UP = 1 << 13, UIB_RIGHT = 1 << 14, UIB_DOWN = 1 << 15,
};

typedef enum { UI_ICON_SWITCH, UI_ICON_XBOX, UI_ICON_GAMEPAD, UI_ICON_WIFI, UI_ICON_USB, UI_ICON_WRENCH } UiIcon;

typedef enum { UI_ST_STARTING, UI_ST_WAITING, UI_ST_CONNECTING, UI_ST_CONNECTED } UiStatus;

// A big card on the home screen.
typedef struct {
    const char *title, *subtitle, *tag;   // tag: "Switch 2 · Steam", ...
    UiIcon icon;
    uint32_t accent;                       // 0xRRGGBB
} UiCard;

typedef struct {
    const char *version;
    const UiCard *cards; int ncards;       // 4
    int sel;                               // 0..ncards-1 cards, ncards = Settings, ncards+1 = Advanced
    const char *toast;                     // optional message line, NULL for none
} UiHome;

typedef struct {
    const char *title;
    UiIcon icon; uint32_t accent;
    UiStatus status;
    const char *status_text;
    const char *lines[4];                  // instructions, NULL-terminated
    // live input
    uint32_t buttons;                      // UIB_*
    int lsx, lsy, rsx, rsy;                // -32768..32767
    // motion (Switch mode)
    bool show_motion;
    const char *motion_text;               // "console", "controller", ...
    float accel[3];                        // in G, any orientation; drawn as tilt
    // footer
    const char *stat_text;                 // e.g. "1234 reports  ·  RTT 4.1 ms"
    float quit_progress;                   // 0..1 while + and - are held
} UiRun;

typedef struct {
    const char *title;
    const char **names, **values, **help;  // values may be NULL for plain items
    int n, sel;
    const char *hint;                      // footer hint
} UiList;

typedef struct {
    const char *title, *message, *detail;
} UiError;

// font_data must stay valid while the UI runs (TTF_OpenFontRW does not copy).
bool ui_init(SDL_Renderer *r, const void *font_data, size_t font_size);
void ui_exit(void);

void ui_draw_home(const UiHome *h);
void ui_draw_run(const UiRun *v, uint32_t ms);       // ms: animation clock
void ui_draw_list(const UiList *l);
void ui_draw_error(const UiError *e);

// Touch hit testing for the last drawn screen. Returns an item index or -1.
// Home: 0..3 cards, 4 Settings, 5 Advanced, 6 Quit.  Run: 0 = Stop button.
// List: row index, or 1000 + row for the "<" arrow, 2000 + row for ">", 9999 = Back.
int ui_hit(int x, int y);
