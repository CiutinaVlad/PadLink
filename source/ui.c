// PadLink graphical UI. See ui.h.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include "ui.h"

// ---------------------------------------------------------------- palette ---
#define BG        0x0f1117
#define PANEL     0x1a1e2a
#define PANEL_HI  0x242a3a
#define LINE      0x2e3546
#define TEXT      0xf2f4f8
#define DIM       0x9aa3b5
#define FAINT     0x5d6578
#define OK_GREEN  0x34d17a
#define WARN      0xf5b942
#define BAD       0xff5a5a

static SDL_Renderer *R;

// ------------------------------------------------------------------ fonts ---
enum { F18, F22, F26, F32, F44, F56, NFS };
static const int k_fs[NFS] = { 18, 22, 26, 32, 44, 56 };
static TTF_Font *s_font[NFS][2];   // [size][bold]

typedef struct {
    uint32_t hash, color; int font; SDL_Texture *tex; int w, h; uint32_t used; char s[96];
} CacheEnt;
#define NCACHE 256
static CacheEnt s_cache[NCACHE];
static uint32_t s_frame;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static SDL_Texture *render_text(int font, const char *s, uint32_t rgb, int *w, int *h) {
    TTF_Font *f = s_font[font / 2][font & 1];
    SDL_Color c = { (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, surf);
    *w = surf->w; *h = surf->h;
    SDL_FreeSurface(surf);
    return t;
}

static CacheEnt *get_text(int font, const char *s, uint32_t rgb) {
    static CacheEnt tmp;
    uint32_t h = hash_str(s) ^ (uint32_t)font * 0x9e3779b9u;
    size_t len = strlen(s);
    if (len < sizeof tmp.s) {
        int lru = 0;
        for (int i = 0; i < NCACHE; i++) {
            CacheEnt *e = &s_cache[i];
            if (e->tex && e->hash == h && e->font == font && e->color == rgb && !strcmp(e->s, s)) {
                e->used = s_frame; return e;
            }
            if (!s_cache[i].tex || s_cache[i].used < s_cache[lru].used) lru = i;
            if (!s_cache[i].tex) { lru = i; break; }
        }
        CacheEnt *e = &s_cache[lru];
        if (e->tex) SDL_DestroyTexture(e->tex);
        memset(e, 0, sizeof *e);
        e->tex = render_text(font, s, rgb, &e->w, &e->h);
        if (!e->tex) return NULL;
        e->hash = h; e->font = font; e->color = rgb; e->used = s_frame;
        memcpy(e->s, s, len + 1);
        return e;
    }
    // too long to cache: render once (caller draws it right away)
    if (tmp.tex) SDL_DestroyTexture(tmp.tex);
    tmp.tex = render_text(font, s, rgb, &tmp.w, &tmp.h);
    return tmp.tex ? &tmp : NULL;
}

#define FONT(size, bold) ((size) * 2 + ((bold) ? 1 : 0))
enum { AL_LEFT, AL_CENTER, AL_RIGHT };

// y is the vertical center of the line. Returns the width drawn.
static int text(int font, const char *s, int x, int y, uint32_t rgb, int align) {
    if (!s || !*s) return 0;
    CacheEnt *e = get_text(font, s, rgb);
    if (!e) return 0;
    int dx = align == AL_CENTER ? x - e->w / 2 : align == AL_RIGHT ? x - e->w : x;
    SDL_Rect dst = { dx, y - e->h / 2, e->w, e->h };
    SDL_RenderCopy(R, e->tex, NULL, &dst);
    return e->w;
}

static int text_w(int font, const char *s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(s_font[font / 2][font & 1], s, &w, &h);
    return w;
}

// Draws lines separated by '\n'. Returns the number of lines.
static int text_lines(int font, const char *s, int x, int y, int line_h, uint32_t rgb, int align) {
    if (!s) return 0;
    char buf[160]; int n = 0;
    while (*s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, s, len); buf[len] = 0;
        text(font, buf, x, y + n * line_h, rgb, align);
        n++;
        if (!nl) break;
        s = nl + 1;
    }
    return n;
}

// ------------------------------------------------------------- primitives ---
#define RGBA(c, a) (Uint8)(((c) >> 16) & 255), (Uint8)(((c) >> 8) & 255), (Uint8)((c) & 255), (Uint8)(a)

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static void rbox(int x, int y, int w, int h, int rad, uint32_t c, int a) {
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    roundedBoxRGBA(R, x, y, x + w - 1, y + h - 1, rad, RGBA(c, a));
}

static void rframe(int x, int y, int w, int h, int rad, int thick, uint32_t c, int a) {
    for (int i = 0; i < thick; i++)
        roundedRectangleRGBA(R, x + i, y + i, x + w - 1 - i, y + h - 1 - i, rad > i ? rad - i : 0, RGBA(c, a));
}

static void disc(int x, int y, int r, uint32_t c, int a) {
    filledCircleRGBA(R, x, y, r, RGBA(c, a));
    aacircleRGBA(R, x, y, r, RGBA(c, a));
}

static void ring(int x, int y, int r, int thick, uint32_t c, int a) {
    for (int i = 0; i < thick; i++) aacircleRGBA(R, x, y, r - i, RGBA(c, a));
    if (thick > 2) for (int i = 1; i < thick - 1; i++) circleRGBA(R, x, y, r - i, RGBA(c, a));
}

static void tline(int x1, int y1, int x2, int y2, int w, uint32_t c, int a) {
    thickLineRGBA(R, x1, y1, x2, y2, w, RGBA(c, a));
    disc(x1, y1, w / 2, c, a);
    disc(x2, y2, w / 2, c, a);
}

static void thick_arc(int x, int y, int r, int thick, int a0, int a1, uint32_t c, int a) {
    for (int i = 0; i < thick; i++) arcRGBA(R, x, y, r - i, a0, a1, RGBA(c, a));
}

static void clear(uint32_t c) {
    SDL_SetRenderDrawColor(R, RGBA(c, 255));
    SDL_RenderClear(R);
}

// -------------------------------------------------------------- hit areas ---
typedef struct { SDL_Rect r; int id; } Hit;
static Hit s_hits[64];
static int s_nhits;
static void hit_reset(void) { s_nhits = 0; }
static void hit_add(int x, int y, int w, int h, int id) {
    if (s_nhits < 64) s_hits[s_nhits++] = (Hit){ { x, y, w, h }, id };
}
int ui_hit(int x, int y) {
    for (int i = s_nhits - 1; i >= 0; i--) {
        SDL_Rect *r = &s_hits[i].r;
        if (x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h) return s_hits[i].id;
    }
    return -1;
}

// ------------------------------------------------------------------ icons ---
static void icon(UiIcon ic, int cx, int cy, int s, uint32_t c) {
    switch (ic) {
        case UI_ICON_SWITCH: {      // console with two controller halves
            int h = s * 62 / 100, jw = s * 20 / 100, sw = s * 46 / 100;
            int x0 = cx - (jw * 2 + sw) / 2;
            rbox(x0, cy - h / 2, jw + 8, h, jw / 2 + 4, c, 255);
            rbox(x0 + jw + sw - 8, cy - h / 2, jw + 8, h, jw / 2 + 4, c, 255);
            rbox(x0 + jw, cy - h / 2, sw, h, 4, PANEL, 255);
            rframe(x0 + jw, cy - h / 2, sw, h, 4, 3, c, 255);
            disc(x0 + jw / 2 + 4, cy - h / 6, s / 22 + 1, PANEL, 255);
            disc(x0 + jw + sw + jw / 2 - 4, cy + h / 8, s / 22 + 1, PANEL, 255);
            break;
        }
        case UI_ICON_XBOX: {        // round button with an X
            ring(cx, cy, s * 36 / 100, s / 14, c, 255);
            int d = s * 15 / 100;
            tline(cx - d, cy - d, cx + d, cy + d, s / 11, c, 255);
            tline(cx - d, cy + d, cx + d, cy - d, s / 11, c, 255);
            break;
        }
        case UI_ICON_GAMEPAD: {     // generic gamepad silhouette
            int w = s * 74 / 100, h = s * 36 / 100;
            disc(cx - w / 2 + h / 2, cy + h / 5, h * 6 / 10, c, 255);
            disc(cx + w / 2 - h / 2, cy + h / 5, h * 6 / 10, c, 255);
            rbox(cx - w / 2, cy - h / 2, w, h, h / 2, c, 255);
            int dx = cx - w / 4, dl = s / 12, dt = s / 22 + 1;
            boxRGBA(R, dx - dl, cy - dt, dx + dl, cy + dt, RGBA(PANEL, 255));
            boxRGBA(R, dx - dt, cy - dl, dx + dt, cy + dl, RGBA(PANEL, 255));
            int bx = cx + w / 4, bo = s / 14, br = s / 30 + 1;
            disc(bx, cy - bo, br, PANEL, 255); disc(bx, cy + bo, br, PANEL, 255);
            disc(bx - bo, cy, br, PANEL, 255); disc(bx + bo, cy, br, PANEL, 255);
            break;
        }
        case UI_ICON_WIFI: {
            int base = cy + s * 26 / 100, t = s / 11;
            disc(cx, base, s / 14 + 1, c, 255);
            for (int i = 1; i <= 3; i++) thick_arc(cx, base, i * s * 15 / 100 + t / 2, t, 225, 315, c, 255);
            break;
        }
        case UI_ICON_USB: {
            int t = s / 12, top = cy - s * 34 / 100, bot = cy + s * 24 / 100;
            tline(cx, top + s / 10, cx, bot, t, c, 255);
            filledTrigonRGBA(R, cx, top, cx - s / 9, top + s / 6, cx + s / 9, top + s / 6, RGBA(c, 255));
            disc(cx, bot + s / 14, s / 10, c, 255);
            tline(cx, cy + s / 10, cx - s / 5, cy - s / 20, t, c, 255);
            tline(cx - s / 5, cy - s / 20, cx - s / 5, cy - s / 7, t, c, 255);
            disc(cx - s / 5, cy - s / 7 - 2, s / 14, c, 255);
            tline(cx, cy + s / 22, cx + s / 5, cy - s / 9, t, c, 255);
            boxRGBA(R, cx + s / 5 - s / 14, cy - s / 9 - s / 7, cx + s / 5 + s / 14, cy - s / 9 - 2, RGBA(c, 255));
            break;
        }
        case UI_ICON_TABLET: {      // GamePad-style tablet: wide body, screen, two sticks
            int w = s * 86 / 100, h = s * 50 / 100;
            rbox(cx - w / 2, cy - h / 2, w, h, h / 3, c, 255);
            rbox(cx - w / 4 - 2, cy - h / 2 + 6, w / 2 + 4, h - 12, 3, PANEL, 255);
            disc(cx - w / 2 + w / 9, cy - h / 6, s / 16 + 1, PANEL, 255);
            disc(cx + w / 2 - w / 9, cy - h / 6, s / 16 + 1, PANEL, 255);
            break;
        }
        case UI_ICON_WRENCH: {      // gear
            int ro = s * 34 / 100, ri = s * 14 / 100;
            for (int i = 0; i < 8; i++) {
                float a = i * (float)M_PI / 4;
                Sint16 vx[4], vy[4];
                float ca = cosf(a), sa = sinf(a), hw = s * 0.085f;
                float r0 = ro * 0.7f, r1 = ro * 1.18f;
                vx[0] = (Sint16)(cx + ca * r0 - sa * hw); vy[0] = (Sint16)(cy + sa * r0 + ca * hw);
                vx[1] = (Sint16)(cx + ca * r1 - sa * hw); vy[1] = (Sint16)(cy + sa * r1 + ca * hw);
                vx[2] = (Sint16)(cx + ca * r1 + sa * hw); vy[2] = (Sint16)(cy + sa * r1 - ca * hw);
                vx[3] = (Sint16)(cx + ca * r0 + sa * hw); vy[3] = (Sint16)(cy + sa * r0 - ca * hw);
                filledPolygonRGBA(R, vx, vy, 4, RGBA(c, 255));
            }
            disc(cx, cy, ro, c, 255);
            disc(cx, cy, ri, PANEL, 255);
            break;
        }
    }
}

// Button glyph for hints: a filled circle with a letter.
static int hint_glyph(int x, int y, const char *label, uint32_t c) {
    int r = 15;
    if (strlen(label) > 1) {   // pill for "+" "-" combos and words
        int w = text_w(FONT(F18, 1), label) + 18;
        rbox(x, y - r, w, r * 2, r, c, 255);
        text(FONT(F18, 1), label, x + w / 2, y, BG, AL_CENTER);
        return w;
    }
    disc(x + r, y, r, c, 255);
    text(FONT(F18, 1), label, x + r, y, BG, AL_CENTER);
    return r * 2;
}

// Right-aligned footer hints: pairs of {glyph, label}.
static void hints(const char *const *pairs, int n, int right, int y) {
    int x = right;
    for (int i = n - 1; i >= 0; i--) {
        int lw = text_w(FONT(F22, 0), pairs[i * 2 + 1]);
        x -= lw;
        text(FONT(F22, 0), pairs[i * 2 + 1], x, y, DIM, AL_LEFT);
        int gw = strlen(pairs[i * 2]) > 1 ? text_w(FONT(F18, 1), pairs[i * 2]) + 18 : 30;
        x -= gw + 10;
        hint_glyph(x, y, pairs[i * 2], TEXT);
        x -= 36;
    }
}

static void footer_bar(void) {
    boxRGBA(R, 0, UI_H - 72, UI_W, UI_H, RGBA(0x0b0d12, 255));
    hlineRGBA(R, 0, UI_W, UI_H - 72, RGBA(LINE, 255));
}

static void header(const char *title, const char *version, UiIcon ic, uint32_t accent, bool show_icon) {
    int x = 64;
    if (show_icon) {
        disc(x + 26, 56, 28, mix(PANEL, accent, 0.25f), 255);
        icon(ic, x + 26, 56, 40, accent);
        x += 72;
    }
    text(FONT(F44, 1), title, x, 56, TEXT, AL_LEFT);
    if (version) {
        char v[32]; snprintf(v, sizeof v, "v%s", version);
        int w = text_w(FONT(F18, 0), v) + 28;
        rbox(UI_W - 64 - w, 40, w, 32, 16, PANEL, 255);
        text(FONT(F18, 0), v, UI_W - 64 - w / 2, 56, DIM, AL_CENTER);
    }
}

// ------------------------------------------------------------------- home ---
void ui_draw_home(const UiHome *h) {
    s_frame++; hit_reset();
    clear(BG);
    // brand
    disc(64 + 30, 64, 30, 0xe8394d, 255);
    icon(UI_ICON_GAMEPAD, 64 + 30, 64, 44, TEXT);
    text(FONT(F44, 1), "PadLink", 140, 50, TEXT, AL_LEFT);
    text(FONT(F22, 0), "Use this Switch as a controller", 142, 88, DIM, AL_LEFT);
    char v[32]; snprintf(v, sizeof v, "v%s", h->version);
    int vw = text_w(FONT(F18, 0), v) + 28;
    rbox(UI_W - 64 - vw, 48, vw, 32, 16, PANEL, 255);
    text(FONT(F18, 0), v, UI_W - 64 - vw / 2, 64, DIM, AL_CENTER);

    // 2 columns x 3 rows of cards
    const int cw = 566, ch = 136, gx = 20, gy = 14, x0 = 64, y0 = 124;
    for (int i = 0; i < h->ncards && i < 6; i++) {
        const UiCard *c = &h->cards[i];
        int x = x0 + (i % 2) * (cw + gx), y = y0 + (i / 2) * (ch + gy);
        bool sel = h->sel == i;
        rbox(x, y, cw, ch, 22, sel ? PANEL_HI : PANEL, 255);
        if (sel) rframe(x - 4, y - 4, cw + 8, ch + 8, 26, 4, c->accent, 255);
        disc(x + 74, y + ch / 2, 46, mix(PANEL, c->accent, sel ? 0.35f : 0.22f), 255);
        icon(c->icon, x + 74, y + ch / 2, 58, c->accent);
        text(FONT(F32, 1), c->title, x + 146, y + 36, TEXT, AL_LEFT);
        text(FONT(F22, 0), c->subtitle, x + 146, y + 72, DIM, AL_LEFT);
        if (c->tag && *c->tag) {
            int tw = text_w(FONT(F18, 1), c->tag) + 24;
            rbox(x + 144, y + 92, tw, 30, 15, mix(PANEL, c->accent, 0.18f), 255);
            text(FONT(F18, 1), c->tag, x + 144 + tw / 2, y + 107, mix(c->accent, TEXT, 0.35f), AL_CENTER);
        }
        hit_add(x, y, cw, ch, i);
    }

    footer_bar();
    if (h->toast) text(FONT(F22, 0), h->toast, 64, UI_H - 36, WARN, AL_LEFT);
    static const char *const hp[] = { "A", "Select", "Y", "Settings", "+", "Quit" };
    hints(hp, 3, UI_W - 64, UI_H - 36);
    hit_add(UI_W - 150, UI_H - 72, 150, 72, 6);
    SDL_RenderPresent(R);
}

// -------------------------------------------------------------------- run ---
static void stick(int cx, int cy, int r, int sx, int sy, bool click, uint32_t accent) {
    disc(cx, cy, r, 0x12151d, 255);
    ring(cx, cy, r, 3, click ? accent : LINE, 255);
    float fx = sx / 32767.0f, fy = sy / 32767.0f;
    float m = sqrtf(fx * fx + fy * fy);
    if (m > 1) { fx /= m; fy /= m; }
    int kx = cx + (int)(fx * (r - 22)), ky = cy - (int)(fy * (r - 22));
    if (m > 0.08f) tline(cx, cy, kx, ky, 4, mix(0x12151d, accent, 0.6f), 255);
    disc(kx, ky, 20, click ? accent : (m > 0.08f ? mix(TEXT, accent, 0.4f) : 0x6b7385), 255);
}

static void face(int cx, int cy, const char *l, bool on, uint32_t accent) {
    disc(cx, cy, 21, on ? accent : 0x2a3040, 255);
    text(FONT(F18, 1), l, cx, cy, on ? BG : DIM, AL_CENTER);
}

static void shoulder(int x, int y, int w, const char *l, bool on, uint32_t accent) {
    rbox(x, y, w, 30, 15, on ? accent : 0x2a3040, 255);
    text(FONT(F18, 1), l, x + w / 2, y + 15, on ? BG : DIM, AL_CENTER);
}

static void visualizer(const UiRun *v, int x, int y, int w, int h) {
    rbox(x, y, w, h, 22, PANEL, 255);
    text(FONT(F18, 1), "LIVE INPUT", x + 28, y + 30, FAINT, AL_LEFT);
    int cx = x + w / 2, cy = y + h / 2 + 18;
    uint32_t a = v->accent, b = v->buttons;
    shoulder(cx - 230, cy - 120, 70, "ZL", b & UIB_ZL, a);
    shoulder(cx - 150, cy - 120, 70, "L",  b & UIB_L,  a);
    shoulder(cx + 80,  cy - 120, 70, "R",  b & UIB_R,  a);
    shoulder(cx + 160, cy - 120, 70, "ZR", b & UIB_ZR, a);
    stick(cx - 170, cy - 22, 52, v->lsx, v->lsy, b & UIB_LS, a);
    stick(cx + 90, cy + 72, 52, v->rsx, v->rsy, b & UIB_RS, a);
    // d-pad
    int dx = cx - 90, dy = cy + 72, o = 30;
    face(dx, dy - o, "", b & UIB_UP, a);    face(dx, dy + o, "", b & UIB_DOWN, a);
    face(dx - o, dy, "", b & UIB_LEFT, a);  face(dx + o, dy, "", b & UIB_RIGHT, a);
    filledTrigonRGBA(R, dx, dy - o - 8, dx - 7, dy - o + 5, dx + 7, dy - o + 5, RGBA(b & UIB_UP ? BG : DIM, 255));
    filledTrigonRGBA(R, dx, dy + o + 8, dx - 7, dy + o - 5, dx + 7, dy + o - 5, RGBA(b & UIB_DOWN ? BG : DIM, 255));
    filledTrigonRGBA(R, dx - o - 8, dy, dx - o + 5, dy - 7, dx - o + 5, dy + 7, RGBA(b & UIB_LEFT ? BG : DIM, 255));
    filledTrigonRGBA(R, dx + o + 8, dy, dx + o - 5, dy - 7, dx + o - 5, dy + 7, RGBA(b & UIB_RIGHT ? BG : DIM, 255));
    // face buttons
    int fx = cx + 170, fy = cy - 22;
    face(fx, fy - 44, "X", b & UIB_X, a); face(fx, fy + 44, "B", b & UIB_B, a);
    face(fx - 44, fy, "Y", b & UIB_Y, a); face(fx + 44, fy, "A", b & UIB_A, a);
    // + / -
    disc(cx - 40, cy - 50, 16, b & UIB_MINUS ? a : 0x2a3040, 255);
    boxRGBA(R, cx - 47, cy - 52, cx - 33, cy - 48, RGBA(b & UIB_MINUS ? BG : DIM, 255));
    disc(cx + 40, cy - 50, 16, b & UIB_PLUS ? a : 0x2a3040, 255);
    boxRGBA(R, cx + 33, cy - 52, cx + 47, cy - 48, RGBA(b & UIB_PLUS ? BG : DIM, 255));
    boxRGBA(R, cx + 38, cy - 57, cx + 42, cy - 43, RGBA(b & UIB_PLUS ? BG : DIM, 255));
}

static void motion_panel(const UiRun *v, int x, int y, int w, int h) {
    rbox(x, y, w, h, 22, PANEL, 255);
    text(FONT(F18, 1), "MOTION", x + 28, y + 30, FAINT, AL_LEFT);
    // bubble level
    int r = 52, cx = x + w - 28 - r, cy = y + h / 2;
    disc(cx, cy, r, 0x12151d, 255);
    ring(cx, cy, r, 2, LINE, 255);
    ring(cx, cy, r / 3, 1, LINE, 255);
    hlineRGBA(R, cx - r + 6, cx + r - 6, cy, RGBA(LINE, 255));
    vlineRGBA(R, cx, cy - r + 6, cy + r - 6, RGBA(LINE, 255));
    float ax = v->accel[0], ay = v->accel[1], az = v->accel[2];
    float m = sqrtf(ax * ax + ay * ay + az * az);
    float roll = 0, pitch = 0;
    if (m > 0.2f) {
        ax /= m; ay /= m; az /= m;
        roll = atan2f(ay, az) * 57.2958f;
        pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
        float bx = ay, by = -ax;            // bubble moves with the tilt
        float bm = sqrtf(bx * bx + by * by);
        if (bm > 1) { bx /= bm; by /= bm; }
        disc(cx + (int)(bx * (r - 16)), cy + (int)(by * (r - 16)), 14, v->accent, 255);
    }
    text(FONT(F26, 1), v->motion_text ? v->motion_text : "-", x + 28, y + 72, TEXT, AL_LEFT);
    char t[64];
    if (m > 0.2f) snprintf(t, sizeof t, "Tilt %+4.0f°   Roll %+4.0f°", pitch, roll);
    else snprintf(t, sizeof t, "No motion data");
    text(FONT(F22, 0), t, x + 28, y + 110, DIM, AL_LEFT);
}

void ui_draw_run(const UiRun *v, uint32_t ms) {
    s_frame++; hit_reset();
    clear(BG);
    header(v->title, NULL, v->icon, v->accent, true);

    // status card
    const int lx = 64, lw = 560;
    uint32_t sc = v->status == UI_ST_CONNECTED ? OK_GREEN : v->status == UI_ST_STARTING ? DIM : WARN;
    rbox(lx, 116, lw, 150, 22, mix(PANEL, sc, 0.10f), 255);
    rframe(lx, 116, lw, 150, 22, 2, mix(PANEL, sc, 0.45f), 255);
    float pulse = v->status == UI_ST_CONNECTED ? 1.0f : 0.55f + 0.45f * sinf(ms * 0.006f);
    disc(lx + 58, 191, (int)(16 + 12 * (1 - pulse)), sc, (int)(90 * pulse));
    disc(lx + 58, 191, 14, sc, 255);
    static const char *st_label[] = { "STARTING", "WAITING", "CONNECTING", "CONNECTED" };
    text(FONT(F18, 1), st_label[v->status], lx + 96, 160, sc, AL_LEFT);
    text(FONT(F32, 1), v->status_text, lx + 96, 200, TEXT, AL_LEFT);

    // instructions
    int y = 310;
    for (int i = 0; i < 4 && v->lines[i]; i++) {
        disc(lx + 12, y, 5, v->accent, 255);
        int n = text_lines(FONT(F22, 0), v->lines[i], lx + 32, y, 30, DIM, AL_LEFT);
        y += n * 30 + 22;
    }

    // right column
    const int rx = 656, rw = 560;
    if (v->show_motion) {
        visualizer(v, rx, 116, rw, 330);
        motion_panel(v, rx, 462, rw, 150);
    } else {
        visualizer(v, rx, 116, rw, 380);
    }

    // footer: hold-to-stop + stop button + stats
    footer_bar();
    int fy = UI_H - 36;
    hint_glyph(64, fy, "+", TEXT);
    text(FONT(F22, 0), "and", 104, fy, DIM, AL_LEFT);
    hint_glyph(150, fy, "-", TEXT);
    text(FONT(F22, 0), "hold 2 s to stop", 190, fy, DIM, AL_LEFT);
    int pbx = 370, pbw = 150;
    rbox(pbx, fy - 5, pbw, 10, 5, LINE, 255);
    if (v->quit_progress > 0.01f) rbox(pbx, fy - 5, (int)(pbw * (v->quit_progress > 1 ? 1 : v->quit_progress)), 10, 5, BAD, 255);
    if (v->stat_text) text(FONT(F18, 0), v->stat_text, v->touch_passthrough ? 860 : 900, fy, FAINT, AL_RIGHT);
    if (v->touch_passthrough) {
        text(FONT(F18, 1), "TOUCH GOES TO THE GAMEPAD", UI_W - 64, fy, v->touching ? v->accent : FAINT, AL_RIGHT);
        if (v->touching) {   // show where the GamePad screen is being touched
            ring(v->touch_x, v->touch_y, 34, 3, v->accent, 200);
            disc(v->touch_x, v->touch_y, 8, v->accent, 200);
        }
    } else {
        int sbw = 150;
        rbox(UI_W - 64 - sbw, fy - 24, sbw, 48, 24, mix(PANEL, BAD, 0.25f), 255);
        text(FONT(F22, 1), "Stop", UI_W - 64 - sbw / 2, fy, 0xffb3b3, AL_CENTER);
        hit_add(UI_W - 64 - sbw - 10, fy - 34, sbw + 20, 68, 0);
    }
    SDL_RenderPresent(R);
}

// ------------------------------------------------------------------- list ---
void ui_draw_list(const UiList *l) {
    s_frame++; hit_reset();
    clear(BG);
    // back arrow + title
    tline(84, 56, 70, 56, 5, TEXT, 255);
    filledTrigonRGBA(R, 62, 56, 76, 44, 76, 68, RGBA(TEXT, 255));
    hit_add(40, 20, 80, 72, 9999);
    text(FONT(F44, 1), l->title, 112, 56, TEXT, AL_LEFT);

    const int x = 64, w = UI_W - 128, rh = l->n > 8 ? 44 : 50, y0 = 108;
    for (int i = 0; i < l->n; i++) {
        int y = y0 + i * (rh + 6);
        bool sel = i == l->sel;
        rbox(x, y, w, rh, 16, sel ? PANEL_HI : PANEL, 255);
        if (sel) boxRGBA(R, x + 12, y + 12, x + 16, y + rh - 13, RGBA(0x4cc2ff, 255));
        text(FONT(F26, sel), l->names[i], x + 30, y + rh / 2, sel ? TEXT : mix(TEXT, DIM, 0.4f), AL_LEFT);
        hit_add(x, y, w, rh, i);
        if (l->values && l->values[i]) {
            int pw = 340, px = x + w - 12 - pw;
            rbox(px, y + 7, pw, rh - 14, (rh - 14) / 2, sel ? 0x0f1117 : 0x151924, 255);
            uint32_t ac = sel ? 0x4cc2ff : FAINT;
            filledTrigonRGBA(R, px + 18, y + rh / 2, px + 30, y + rh / 2 - 9, px + 30, y + rh / 2 + 9, RGBA(ac, 255));
            filledTrigonRGBA(R, px + pw - 18, y + rh / 2, px + pw - 30, y + rh / 2 - 9, px + pw - 30, y + rh / 2 + 9, RGBA(ac, 255));
            text(FONT(F22, 1), l->values[i], px + pw / 2, y + rh / 2, sel ? TEXT : DIM, AL_CENTER);
            hit_add(px, y, 70, rh, 1000 + i);
            hit_add(px + pw - 70, y, 70, rh, 2000 + i);
        } else {
            // chevron
            int cx = x + w - 40, cy = y + rh / 2;
            tline(cx - 6, cy - 10, cx + 4, cy, 4, sel ? TEXT : FAINT, 255);
            tline(cx + 4, cy, cx - 6, cy + 10, 4, sel ? TEXT : FAINT, 255);
        }
    }
    // help box
    if (l->help && l->help[l->sel]) {
        int hy = y0 + l->n * (rh + 6) + 10;
        int hh = UI_H - 72 - 16 - hy;
        if (hh > 40) {
            rbox(x, hy, w, hh, 16, 0x141824, 255);
            int lines = 1;
            for (const char *p = l->help[l->sel]; *p; p++) if (*p == '\n') lines++;
            int ty = hy + hh / 2 - (lines - 1) * 15;
            text_lines(FONT(F22, 0), l->help[l->sel], x + 30, ty, 30, DIM, AL_LEFT);
        }
    }
    footer_bar();
    static const char *const hv[] = { "A", "Select", "B", "Back" };
    static const char *const hs[] = { "<>", "Change", "B", "Save and back" };
    if (l->values) hints(hs, 2, UI_W - 64, UI_H - 36);
    else hints(hv, 2, UI_W - 64, UI_H - 36);
    if (l->hint) text(FONT(F22, 0), l->hint, 64, UI_H - 36, FAINT, AL_LEFT);
    SDL_RenderPresent(R);
}

// ------------------------------------------------------------------ error ---
void ui_draw_error(const UiError *e) {
    s_frame++; hit_reset();
    clear(BG);
    int w = 820, h = 330, x = (UI_W - w) / 2, y = (UI_H - 72 - h) / 2;
    rbox(x, y, w, h, 28, PANEL, 255);
    int cx = UI_W / 2, ty = y + 70;
    filledTrigonRGBA(R, cx, ty - 34, cx - 38, ty + 30, cx + 38, ty + 30, RGBA(WARN, 255));
    aatrigonRGBA(R, cx, ty - 34, cx - 38, ty + 30, cx + 38, ty + 30, RGBA(WARN, 255));
    boxRGBA(R, cx - 3, ty - 12, cx + 3, ty + 12, RGBA(BG, 255));
    disc(cx, ty + 20, 4, BG, 255);
    text(FONT(F32, 1), e->title, cx, y + 140, TEXT, AL_CENTER);
    int n = text_lines(FONT(F22, 0), e->message, cx, y + 196, 32, DIM, AL_CENTER);
    if (e->detail) text(FONT(F18, 0), e->detail, cx, y + 196 + n * 32 + 20, FAINT, AL_CENTER);
    hit_add(0, 0, UI_W, UI_H, 9999);
    footer_bar();
    static const char *const hp[] = { "B", "Back" };
    hints(hp, 1, UI_W - 64, UI_H - 36);
    SDL_RenderPresent(R);
}

// ------------------------------------------------------------------- init ---
bool ui_init(SDL_Renderer *r, const void *font_data, size_t font_size) {
    R = r;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    if (TTF_Init() < 0) return false;
    for (int i = 0; i < NFS; i++)
        for (int b = 0; b < 2; b++) {
            s_font[i][b] = TTF_OpenFontRW(SDL_RWFromConstMem(font_data, (int)font_size), 1, k_fs[i]);
            if (!s_font[i][b]) return false;
            if (b) TTF_SetFontStyle(s_font[i][b], TTF_STYLE_BOLD);
        }
    return true;
}

void ui_exit(void) {
    for (int i = 0; i < NCACHE; i++) if (s_cache[i].tex) SDL_DestroyTexture(s_cache[i].tex);
    memset(s_cache, 0, sizeof s_cache);
    for (int i = 0; i < NFS; i++) for (int b = 0; b < 2; b++) if (s_font[i][b]) TTF_CloseFont(s_font[i][b]);
    TTF_Quit();
}
