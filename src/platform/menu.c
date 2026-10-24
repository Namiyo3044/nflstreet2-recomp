#include "platform/menu.h"
#include "platform/settings.h"
#include "runtime/uicolors.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "third_party/font_anton.h"

typedef uint32_t u32;
typedef uint8_t u8;

/* --------------------------------------------------------------- state --- */
static bool g_open;
static int g_sel;
static unsigned g_changes;
static bool g_dirty = true;
static MenuInfo g_info;
/* The controls page: rows are the GameCube inputs, then "reset" and "back";
   column 0 is the keyboard, 1 the controller. */
static bool g_controls;
static int g_crow, g_ccol, g_capture;
#define CROW_KBPLAYER GCI_COUNT
#define CROW_NINTENDO (GCI_COUNT + 1)
#define CROW_RESET (GCI_COUNT + 2)
#define CROW_BACK (GCI_COUNT + 3)
#define CROWS (GCI_COUNT + 4)
static char g_players[256];
static bool g_shape_locked;
static float g_aspect = 4.0f / 3.0f;   /* the window's shape now (for window sizes) */
static char g_gpu_name[48];            /* the graphics card's name ("": not known) */

void menu_set_gpu_name(const char* name) {
    snprintf(g_gpu_name, sizeof g_gpu_name, "%s", name ? name : "");
    g_dirty = true;
}

void menu_set_aspect(float aspect) {
    if (aspect == g_aspect) return;
    g_aspect = aspect;
    g_dirty = true;
}

void menu_set_shape_locked(bool locked) {
    if (g_shape_locked == locked) return;
    g_shape_locked = locked;
    if (locked && g_sel == MI_SHAPE) g_sel = MI_DISPLAY;   /* off the item that can't change */
    g_dirty = true;
}
/* The menu colours page: rows are the colours, then "reset" and "back";
   g_kedit while changing red / green / blue (g_kch) of the selected one. */
static bool g_colors_page, g_kedit;
static int g_krow, g_kch;
/* The quit page: 0 = YES, 1 = NO. */
static bool g_quit_page, g_quit_yes;
static int g_qsel;

void menu_set_players_text(const char* text) {
    if (strcmp(g_players, text) != 0) {
        snprintf(g_players, sizeof(g_players), "%s", text);
        g_dirty = true;
    }
}
static const char* (*g_key_name)(int);
static const char* (*g_pad_name)(int);

void menu_set_binding_names(const char* (*key_name)(int), const char* (*pad_name)(int)) {
    g_key_name = key_name;
    g_pad_name = pad_name;
}

int menu_capturing(void) { return g_open && g_controls ? g_capture : 0; }

void menu_bind(int code) {
    if (!g_capture) return;
    if (code) {
        int* b = g_capture == 1 ? g_settings.key_bind : g_settings.pad_bind;
        b[g_crow] = code;
        g_changes |= 1u << MI_CONTROLS;
    }
    g_capture = 0;
    g_dirty = true;
}

static bool item_enabled(int i) {
    switch (i) {
    case MI_SHAPE: return !g_shape_locked;
    case MI_WINSIZE: return !g_settings.fullscreen;
    case MI_RES: return g_settings.renderer == 0;
    case MI_TEXTURES: return g_info.textures_found && g_settings.renderer == 0;
    default: return true;
    }
}

void menu_set_info(const MenuInfo* info) { g_info = *info; }

void menu_open(const MenuInfo* info) {
    g_info = *info;
    g_open = true;
    g_quit_page = false;
    g_sel = g_shape_locked ? MI_DISPLAY : 0;
    g_controls = false;
    g_colors_page = false;
    g_capture = 0;
    g_dirty = true;
}

void menu_open_quit(void) {
    g_open = true;
    g_quit_page = true;
    g_qsel = 1;                        /* NO to start with */
    g_controls = false;
    g_colors_page = false;
    g_capture = 0;
    g_dirty = true;
}

bool menu_take_quit(void) {
    bool q = g_quit_yes;
    g_quit_yes = false;
    return q;
}

bool menu_is_open(void) { return g_open; }
bool menu_live(void) { return g_open && g_colors_page; }

unsigned menu_take_changes(void) {
    unsigned c = g_changes;
    g_changes = 0;
    return c;
}

static void change(int item, int dir) {
    Settings* s = &g_settings;
    switch (item) {
    case MI_SHAPE: s->screen_shape = (s->screen_shape - 1 + 4 + dir) % 4 + 1; break;   /* 4:3, 16:9, 21:9, 32:9 */
    case MI_DISPLAY: s->fullscreen = !s->fullscreen; break;
    case MI_WINSIZE: s->window_size = (s->window_size - 1 + SETTINGS_WINDOW_SIZES + dir) % SETTINGS_WINDOW_SIZES + 1; break;
    case MI_RENDERER: s->renderer = !s->renderer; break;
    case MI_RES: s->resolution = (s->resolution - 1 + 8 + dir) % 8 + 1; break;
    case MI_TEXTURES: s->custom_textures = !s->custom_textures; break;
    default: return;
    }
    g_changes |= 1u << item;
}

static bool needs_restart(int i);

/* The game's own Video Settings screen (uismenu.c): its five rows are the
   first five items, with values as numbers from 0. */
static const int k_game_item[MENU_GAME_ROWS] = {MI_SHAPE, MI_DISPLAY, MI_WINSIZE, MI_RES, MI_RENDERER};

int menu_game_row_count(int row) {
    switch (k_game_item[row]) {
    case MI_SHAPE: return 4;
    case MI_DISPLAY: case MI_RENDERER: return 2;
    case MI_WINSIZE: return SETTINGS_WINDOW_SIZES;
    default: return 8;
    }
}

int menu_game_row_get(int row) {
    const Settings* s = &g_settings;
    switch (k_game_item[row]) {
    case MI_SHAPE: return s->screen_shape >= 1 && s->screen_shape <= 4 ? s->screen_shape - 1 : 1;
    case MI_DISPLAY: return s->fullscreen ? 1 : 0;
    case MI_WINSIZE: return s->window_size - 1;
    case MI_RENDERER: return s->renderer ? 1 : 0;
    default: return s->resolution - 1;
    }
}

void menu_game_row_set(int row, int v) {
    Settings* s = &g_settings;
    int item = k_game_item[row];
    switch (item) {
    case MI_SHAPE: if (g_shape_locked) return; s->screen_shape = v + 1; break;
    case MI_DISPLAY: s->fullscreen = v != 0; break;
    case MI_WINSIZE: if (s->fullscreen) return; s->window_size = v + 1; break;
    case MI_RENDERER: s->renderer = v != 0; break;
    default: if (s->renderer) return; s->resolution = v + 1; break;
    }
    g_changes |= 1u << item;
    g_dirty = true;
}

bool menu_game_row_enabled(int row) {
    switch (k_game_item[row]) {
    case MI_SHAPE: return !g_shape_locked;
    case MI_WINSIZE: return !g_settings.fullscreen;
    case MI_RES: return g_settings.renderer == 0;   /* the graphics card's */
    default: return true;
    }
}

const char* menu_game_row_name(int row, int v) {
    static char buf[48];
    switch (k_game_item[row]) {
    case MI_SHAPE: return v == 3 ? "32:9" : v == 2 ? "21:9" : v ? "16:9" : "4:3";
    case MI_DISPLAY: return v ? "Full Screen" : "Window";
    case MI_WINSIZE: {
        int h = settings_window_height(v + 1);
        snprintf(buf, sizeof buf, "%d x %d", (int)((float)h * g_aspect + 0.5f), h);
        return buf;
    }
    case MI_RENDERER: return v ? "Software (slow)" : g_gpu_name[0] ? g_gpu_name : "Graphics Card";
    default: snprintf(buf, sizeof buf, "%dx", v + 1); return buf;
    }
}

const char* menu_game_row_help(int row) {
    /* Short: the help line has room for about 45 letters. */
    static const char k_restart[] = "Takes effect the next time the game starts.";
    switch (k_game_item[row]) {
    case MI_SHAPE: return "Change the screen aspect ratio.";
    case MI_DISPLAY: return "Play in a window or in full screen.";
    case MI_WINSIZE: return "Change the size of the window.";
    case MI_RENDERER: return needs_restart(MI_RENDERER) ? k_restart : "Change how the game is drawn.";
    default: return "Change how sharply the game is drawn.";
    }
}

/* The game's Recomp Settings screen (Options): the port's own additions. */
extern int g_rain_style;
enum { RR_RAIN, RR_TEXTURES, RR_LABELS };
const char* const k_recomp_label[MENU_RECOMP_ROWS] = {"PS2-Style Rain", "Custom Textures", "Button Labels"};

int menu_recomp_row_count(int row) { (void)row; return 2; }

int menu_recomp_row_get(int row) {
    switch (row) {
    case RR_RAIN: return g_settings.ps2_rain ? 1 : 0;
    case RR_TEXTURES: return g_settings.custom_textures ? 1 : 0;
    default: return g_settings.nintendo_labels ? 0 : 1;
    }
}

void menu_recomp_row_set(int row, int v) {
    switch (row) {
    case RR_RAIN: g_settings.ps2_rain = v != 0; g_rain_style = g_settings.ps2_rain; g_changes |= 1u << MI_COUNT; break;
    case RR_TEXTURES:
        if (!g_info.textures_found || g_settings.renderer != 0) return;
        g_settings.custom_textures = v != 0;
        g_changes |= 1u << MI_TEXTURES;
        break;
    default: g_settings.nintendo_labels = v == 0; g_changes |= 1u << MI_COUNT; break;
    }
    g_dirty = true;
}

bool menu_recomp_row_enabled(int row) {
    return row != RR_TEXTURES || (g_info.textures_found && g_settings.renderer == 0);
}

const char* menu_recomp_row_name(int row, int v) {
    switch (row) {
    case RR_RAIN: return v ? "On" : "Off";
    case RR_TEXTURES: return !g_info.textures_found ? "None Found" : v ? "On" : "Off";
    default: return v ? "By Position" : "By Label";
    }
}

const char* menu_recomp_row_help(int row) {
    switch (row) {
    case RR_RAIN: return "Rain with streaks and splashes, as on PS2.";
    case RR_TEXTURES: return "Use the replacement textures folder.";
    default: return "Nintendo-style pads: by label or position.";
    }
}

static void controls_action(int a) {
    if (g_capture) return;             /* the window sends the key / button instead */
    switch (a) {
    case MENU_UP: g_crow = (g_crow + CROWS - 1) % CROWS; break;
    case MENU_DOWN: g_crow = (g_crow + 1) % CROWS; break;
    case MENU_LEFT: case MENU_RIGHT:
        if (g_crow == CROW_KBPLAYER) {
            g_settings.keyboard_player = (g_settings.keyboard_player + (a == MENU_LEFT ? 3 : 1)) % 4;
            g_changes |= 1u << MI_CONTROLS;
        } else if (g_crow == CROW_NINTENDO) {
            g_settings.nintendo_labels = !g_settings.nintendo_labels;
            g_changes |= 1u << MI_CONTROLS;
        } else {
            g_ccol ^= 1;
        }
        break;
    case MENU_OK:
        if (g_crow == CROW_BACK) g_controls = false;
        else if (g_crow == CROW_KBPLAYER) {
            g_settings.keyboard_player = (g_settings.keyboard_player + 1) % 4;
            g_changes |= 1u << MI_CONTROLS;
        } else if (g_crow == CROW_NINTENDO) {
            g_settings.nintendo_labels = !g_settings.nintendo_labels;
            g_changes |= 1u << MI_CONTROLS;
        }
        else if (g_crow == CROW_RESET) {
            for (int i = 0; i < GCI_COUNT; i++) g_settings.key_bind[i] = g_settings.pad_bind[i] = 0;
            g_changes |= 1u << MI_CONTROLS;
        } else {
            g_capture = g_ccol + 1;
        }
        break;
    case MENU_BACK: g_controls = false; break;
    }
    g_dirty = true;
}

static void colors_leave(void) {
    g_colors_page = false;
    g_kedit = false;
    uicolors_save();
}

static void colors_action(int a) {
    int n = uicolors_slot_count(), rows = n + 2;   /* + reset, back */
    if (g_kedit) {                     /* red / green / blue of row g_krow */
        u32 c = uicolors_slot_get(g_krow);
        int sh = 16 - 8 * g_kch, v = (int)((c >> sh) & 255);
        switch (a) {
        case MENU_UP: g_kch = (g_kch + 2) % 3; break;
        case MENU_DOWN: g_kch = (g_kch + 1) % 3; break;
        case MENU_LEFT: case MENU_RIGHT:
            v += a == MENU_LEFT ? -5 : 5;
            v = v < 0 ? 0 : v > 255 ? 255 : v;
            uicolors_slot_set(g_krow, (c & ~(0xFFu << sh)) | (u32)v << sh);
            break;
        case MENU_OK: case MENU_BACK: g_kedit = false; break;
        }
        g_dirty = true;
        return;
    }
    switch (a) {
    case MENU_UP: g_krow = (g_krow + rows - 1) % rows; break;
    case MENU_DOWN: g_krow = (g_krow + 1) % rows; break;
    case MENU_LEFT: case MENU_RIGHT:
        if (g_krow < n) {              /* the next / previous ready-made colour */
            int np = uicolors_preset_count(), p = uicolors_slot_preset(g_krow);
            if (p < 0) p = a == MENU_LEFT ? 1 : np - 1;   /* from a custom colour */
            u32 cur = uicolors_slot_get(g_krow);
            for (int k = 0; k < np; k++) {   /* skip ones that look the same (red on a red shape) */
                p = (p + (a == MENU_LEFT ? np - 1 : 1)) % np;
                if (uicolors_preset_color(g_krow, p) != cur) break;
            }
            uicolors_slot_set(g_krow, uicolors_preset_color(g_krow, p));
        }
        break;
    case MENU_OK:
        if (g_krow < n) { g_kedit = true; g_kch = 0; }
        else if (g_krow == n) uicolors_reset_all();
        else colors_leave();
        break;
    case MENU_BACK: colors_leave(); break;
    }
    g_dirty = true;
}

void menu_action(int a) {
    if (!g_open) return;
    if (g_quit_page) {
        switch (a) {
        case MENU_UP: case MENU_DOWN: case MENU_LEFT: case MENU_RIGHT: g_qsel ^= 1; break;
        case MENU_OK:
            if (g_qsel == 0) g_quit_yes = true;
            g_open = g_quit_page = false;
            break;
        case MENU_BACK: g_open = g_quit_page = false; break;
        }
        g_dirty = true;
        return;
    }
    if (g_controls) {
        controls_action(a);
        return;
    }
    if (g_colors_page) {
        colors_action(a);
        return;
    }
    switch (a) {
    case MENU_UP:
    case MENU_DOWN: {
        int d = a == MENU_UP ? -1 : 1;
        for (int n = 0; n < MI_COUNT; n++) {
            g_sel = (g_sel + d + MI_COUNT) % MI_COUNT;
            if (item_enabled(g_sel)) break;
        }
        break;
    }
    case MENU_LEFT:
    case MENU_RIGHT:
        if (g_sel == MI_CONTROLS || g_sel == MI_COLORS) { if (a == MENU_RIGHT) menu_action(MENU_OK); return; }
        if (item_enabled(g_sel)) change(g_sel, a == MENU_LEFT ? -1 : 1);
        break;
    case MENU_OK:
        if (g_sel == MI_CLOSE) g_open = false;
        else if (g_sel == MI_CONTROLS) { g_controls = true; g_crow = 0; g_ccol = 0; g_capture = 0; }
        else if (g_sel == MI_COLORS) { g_colors_page = true; g_krow = 0; g_kedit = false; }
        else if (item_enabled(g_sel)) change(g_sel, 1);
        break;
    case MENU_BACK:
        g_open = false;
        break;
    }
    g_dirty = true;
}

/* ------------------------------------------------------------- drawing --- */
static u32* g_px;
static int g_w, g_h;
static stbtt_fontinfo g_font;
static bool g_font_ok;

typedef struct { int size, cp, w, h, xoff, yoff; float adv; u8* bmp; } Glyph;
#define GLYPHS 1024
static Glyph g_glyphs[GLYPHS];
static int g_nglyphs;

static const Glyph* glyph(int size, int cp) {
    for (int i = 0; i < g_nglyphs; i++)
        if (g_glyphs[i].size == size && g_glyphs[i].cp == cp) return &g_glyphs[i];
    if (g_nglyphs == GLYPHS) {          /* full (many window sizes): start again */
        for (int i = 0; i < g_nglyphs; i++) stbtt_FreeBitmap(g_glyphs[i].bmp, NULL);
        g_nglyphs = 0;
    }
    Glyph* g = &g_glyphs[g_nglyphs++];
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
    g->size = size;
    g->cp = cp;
    g->adv = (float)adv * sc;
    g->bmp = stbtt_GetCodepointBitmap(&g_font, 0, sc, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

/* Premultiplied "over" of colour rgb at alpha a (0-255). */
static inline void blend(int x, int y, u32 rgb, int a) {
    if (a <= 0 || x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    if (a > 255) a = 255;
    u32* d = &g_px[(size_t)y * g_w + x];
    u32 dv = *d;
    int inv = 255 - a;
    int r = (int)((rgb >> 16) & 255) * a / 255 + (int)((dv >> 16) & 255) * inv / 255;
    int g = (int)((rgb >> 8) & 255) * a / 255 + (int)((dv >> 8) & 255) * inv / 255;
    int b = (int)(rgb & 255) * a / 255 + (int)(dv & 255) * inv / 255;
    int al = a + (int)(dv >> 24) * inv / 255;
    *d = ((u32)al << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

static void fill_rect(int x0, int y0, int x1, int y1, u32 rgb, int a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_w) x1 = g_w;
    if (y1 > g_h) y1 = g_h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) blend(x, y, rgb, a);
}

/* A band slanted like the game's own banners: left edge leaning right by
   `skew` pixels from bottom to top, right edge likewise. */
static void fill_slant(int x0, int y0, int x1, int y1, int skew, u32 rgb, int a) {
    for (int y = y0; y < y1; y++) {
        float t = (float)(y1 - y) / (float)(y1 - y0);      /* 0 at the bottom, 1 at the top */
        float l = (float)x0 + t * (float)skew, r = (float)x1 + t * (float)skew;
        int xl = (int)floorf(l), xr = (int)ceilf(r);
        for (int x = xl; x < xr; x++) {
            float cov = 1.0f;
            if (x < l) cov = 1.0f - (l - (float)x);
            if (x + 1 > r) cov = r - (float)x;
            blend(x, y, rgb, (int)((float)a * (cov < 0 ? 0 : cov)));
        }
    }
}

/* A small solid triangle pointing left (dir -1) or right (1), centred at cx, cy. */
static void arrow(int cx, int cy, int size, int dir, u32 rgb, int a) {
    for (int x = 0; x <= size; x++) {          /* x = 0: the flat side, size: the tip */
        int half = size - x;
        int px = cx + dir * (x - size / 2);
        for (int y = -half; y <= half; y++) blend(px, cy + y, rgb, a);
    }
}

static float text_width(const char* s, int size) {
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size), w = 0;
    for (const char* p = s; *p; p++) {
        w += glyph(size, (unsigned char)*p)->adv;
        if (p[1]) w += (float)stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)p[0], (unsigned char)p[1]) * sc;
    }
    return w;
}

/* Text with its baseline at y; align 0 left, 1 right (x is the right edge), 2 centred. */
static void text(const char* s, float x, int y, int size, u32 rgb, int a, int align) {
    if (!g_font_ok) return;
    if (align) x -= text_width(s, size) / (align == 1 ? 1.0f : 2.0f);
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size);
    for (const char* p = s; *p; p++) {
        const Glyph* g = glyph(size, (unsigned char)*p);
        int gx = (int)floorf(x + 0.5f) + g->xoff, gy = y + g->yoff;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int c = g->bmp[j * g->w + i];
                if (c) blend(gx + i, gy + j, rgb, c * a / 255);
            }
        x += g->adv;
        if (p[1]) x += (float)stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)p[0], (unsigned char)p[1]) * sc;
    }
}

/* Word-wrapped text inside width `maxw`; returns the number of lines. */
static int text_wrapped(const char* s, float x, int y, float maxw, int size, int line_h, u32 rgb, int a) {
    char line[540] = "", word[128];
    int lines = 0;
    const char* p = s;
    while (*p) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < 127) word[n++] = *p++;
        word[n] = 0;
        if (!n) break;
        char trial[700];
        snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && text_width(trial, size) > maxw) {
            text(line, x, y + lines * line_h, size, rgb, a, 0);
            lines++;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", trial);
        }
    }
    if (line[0]) {
        text(line, x, y + lines * line_h, size, rgb, a, 0);
        lines++;
    }
    return lines;
}

static const char* item_label(int i) {
    static const char* const k[MI_COUNT] = {"ASPECT RATIO", "DISPLAY", "WINDOW RESOLUTION", "INTERNAL RESOLUTION",
                                            "RENDERER", "CUSTOM TEXTURES", "MENU COLOURS",
                                            "CONTROLS", "BACK TO THE GAME"};
    return k[i];
}

static void item_value(int i, char* out, size_t cap) {
    const Settings* s = &g_settings;
    switch (i) {
    case MI_SHAPE: snprintf(out, cap, "%s", s->screen_shape == 1 ? "4:3" : s->screen_shape == 3 ? "21:9 (TEST)" : s->screen_shape == 4 ? "32:9 (TEST)" : "16:9"); break;
    case MI_DISPLAY: snprintf(out, cap, "%s", s->fullscreen ? "FULL SCREEN" : "WINDOW"); break;
    case MI_WINSIZE: {
        int h = settings_window_height(s->window_size);
        snprintf(out, cap, "%d X %d", (int)((float)h * g_aspect + 0.5f), h);
        break;
    }
    case MI_RENDERER:
        if (s->renderer || !g_gpu_name[0]) snprintf(out, cap, "%s", s->renderer ? "SOFTWARE (SLOW)" : "GRAPHICS CARD");
        else {
            snprintf(out, cap, "%s", g_gpu_name);
            for (char* c = out; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        }
        break;
    case MI_RES:
        snprintf(out, cap, "%dX", s->resolution);
        break;
    case MI_TEXTURES:
        snprintf(out, cap, "%s", !g_info.textures_found ? "NONE FOUND" : s->custom_textures ? "ON" : "OFF");
        break;
    case MI_COLORS: {
        int changed = 0;
        for (int k = 0; k < uicolors_slot_count(); k++) changed += uicolors_slot_get(k) != uicolors_slot_original(k);
        if (changed) snprintf(out, cap, "%d CHANGED", changed);
        else snprintf(out, cap, "THE GAME'S OWN");
        break;
    }
    case MI_CONTROLS: snprintf(out, cap, "KEYBOARD AND CONTROLLER"); break;
    default: out[0] = 0; break;
    }
}

static const char* item_help(int i) {
    switch (i) {
    case MI_SHAPE: return "The picture's shape: 4:3 or 16:9 (F9 also switches; the game's own screen option is set "
                          "to match). Can't be changed during a match (the match lays out its scoreboard when it "
                          "starts); change it in the game's menus.";
    case MI_DISPLAY: return "Play in a window or on the whole screen (F11 also switches).";
    case MI_WINSIZE: return "How big the window is when not playing full screen.";
    case MI_RENDERER: return "The graphics card draws faster and sharper than the software renderer. Takes effect "
                             "the next time the game starts.";
    case MI_RES: return "How sharply the graphics card draws the game, as a multiple of the GameCube's own picture. "
                        "Higher needs a faster card.";
    case MI_TEXTURES: return g_info.textures_found
                                 ? "Replacement textures from the custom texture folder (Dolphin's format, "
                                   ".png or .dds). Turn off and on again (or press F5 twice) to load files added or changed since."
                                 : "No custom texture folder was found. Put Dolphin-format texture packs in "
                                   "C:\\nfls2\\textures to use them.";
    case MI_COLORS: return "Change the colours of the game's menu bars and music ticker (shapes custom textures can't "
                           "reach). The game carries on behind the page so you can see each change.";
    case MI_CONTROLS: return "Choose which keys and controller buttons work each GameCube button and stick.";
    default: return "Close this menu and carry on playing.";
    }
}

static bool needs_restart(int i) {
    const Settings* s = &g_settings;
    if (i == MI_RENDERER) return (s->renderer == 0) != g_info.gpu_running;
    if (i == MI_RES) return false;     /* changed straight away (the graphics card's) */
    return false;
}

static void draw_controls(int w, int h, float u);
static void draw_colors(int w, int h, float u);

#define COL_ORANGE 0xFF7A00u
#define COL_YELLOW 0xFFD21Fu
#define COL_WHITE 0xFFFFFFu
#define COL_GREY 0xB4B4BEu
#define COL_DIM 0x5A5A64u
#define COL_PANEL 0x0E0E12u

const uint32_t* menu_draw(int w, int h, bool* dirty) {
    if (!g_font_ok) {
        g_font_ok = stbtt_InitFont(&g_font, k_font_anton, stbtt_GetFontOffsetForIndex(k_font_anton, 0)) != 0;
        if (!g_font_ok) printf("menu: couldn't read the built-in font\n");
    }
    if (w != g_w || h != g_h || !g_px) {
        free(g_px);
        g_px = (u32*)malloc((size_t)w * (size_t)h * 4);
        g_w = w;
        g_h = h;
        g_dirty = true;
        if (!g_px) { g_w = g_h = 0; *dirty = false; return NULL; }
    }
    *dirty = g_dirty;
    if (!g_dirty) return g_px;
    g_dirty = false;
    memset(g_px, 0, (size_t)w * (size_t)h * 4);

    float u = (float)h / 1080.0f;              /* layout unit: 1 pixel at 1080 lines */
    if ((float)w / 1920.0f < u * 0.75f) u = (float)w / 1920.0f / 0.75f;
#define U(v) ((int)((v) * u + 0.5f))
    if (g_colors_page) {                               /* the game left bright, to see the colours */
        draw_colors(w, h, u);
        return g_px;
    }
    fill_rect(0, 0, w, h, 0, 150);                     /* the game, dimmed */
    if (g_quit_page) {
        int pw = U(760), ph = U(330);
        int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
        fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 232);
        fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);
        int by0 = py0 - U(36), by1 = py0 + U(78);
        fill_slant(px0 - U(24), by0, px0 + U(420), by1, U(28), COL_ORANGE, 255);
        fill_slant(px0 + U(430), by0 + U(18), px0 + U(460), by1, U(26), COL_YELLOW, 255);
        text("QUIT GAME?", (float)(px0 + U(28)), by1 - U(24), U(78), 0x111111u, 255, 0);
        text("Anything not saved will be lost.", (float)(w / 2), py0 + U(160), U(34), COL_GREY, 255, 2);
        static const char* const k_q[2] = {"YES", "NO"};
        for (int i = 0; i < 2; i++) {
            int cx = w / 2 + (i == 0 ? -U(150) : U(150));
            int y0 = py0 + U(205), y1 = y0 + U(76);
            bool sel = g_qsel == i;
            if (sel) {
                fill_rect(cx - U(120), y0, cx + U(120), y1, COL_ORANGE, 46);
                fill_rect(cx - U(120), y1 - U(6), cx + U(120), y1, COL_ORANGE, 255);
            }
            text(k_q[i], (float)cx, y0 + U(56), U(46), sel ? COL_YELLOW : COL_GREY, 255, 2);
        }
        text("LEFT / RIGHT: CHOOSE     A / ENTER: CONFIRM     B / ESC: BACK", (float)(w / 2), py0 + ph + U(50), U(26),
             COL_GREY, 230, 2);
        return g_px;
    }
    if (g_controls) {
        draw_controls(w, h, u);
        return g_px;
    }

    int pw = U(980), ph = U(950);
    int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 232);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);   /* bottom edge */

    /* Title on a slanted orange banner breaking out of the panel. */
    int by0 = py0 - U(36), by1 = py0 + U(78);
    fill_slant(px0 - U(24), by0, px0 + U(520), by1, U(28), COL_ORANGE, 255);
    fill_slant(px0 + U(530), by0 + U(18), px0 + U(560), by1, U(26), COL_YELLOW, 255);
    text("VIDEO SETTINGS", (float)(px0 + U(28)), by1 - U(24), U(78), 0x111111u, 255, 0);

    /* Rows */
    int row_h = U(70), ry = py0 + U(130), lx = px0 + U(60), vx = px0 + pw - U(70);
    for (int i = 0; i < MI_COUNT; i++) {
        int y0 = ry + i * row_h + (i == MI_CLOSE ? U(18) : 0);
        bool sel = i == g_sel, en = item_enabled(i);
        if (sel) {
            fill_rect(px0 + U(24), y0, px0 + pw - U(24), y0 + row_h - U(8), COL_ORANGE, 46);
            fill_rect(px0 + U(24), y0, px0 + U(34), y0 + row_h - U(8), COL_ORANGE, 255);
        }
        int base = y0 + U(50);
        u32 lc = !en ? COL_DIM : sel ? COL_YELLOW : COL_GREY;
        text(item_label(i), (float)lx, base, U(40), lc, 255, 0);
        char v[64];
        item_value(i, v, sizeof(v));
        if (v[0]) {
            u32 vc = !en ? COL_DIM : sel ? COL_WHITE : 0xE6E6E6u;
            int vw = (int)text_width(v, U(40));
            text(v, (float)vx, base, U(40), vc, 255, 1);
            if (sel && en) {
                arrow(vx - vw - U(26), base - U(15), U(10), -1, COL_YELLOW, 255);
                arrow(vx + U(22), base - U(15), U(10), 1, COL_YELLOW, 255);
            }
            if (needs_restart(i))
                text("AFTER RESTART", (float)(vx - vw - U(56)), base, U(24), COL_ORANGE, 255, 1);
            else if (i == MI_SHAPE && g_shape_locked)
                text("CHANGE IT IN THE MENUS", (float)(vx - vw - U(30)), base, U(24), COL_ORANGE, 255, 1);
        }
    }

    /* What the selected item does. */
    int hy = ry + MI_COUNT * row_h + U(48);
    fill_rect(px0 + U(60), hy - U(10), px0 + pw - U(60), hy - U(8), COL_DIM, 255);
    text_wrapped(item_help(g_sel), (float)(px0 + U(60)), hy + U(36), (float)(pw - U(120)), U(28), U(36), COL_GREY, 255);

    text("UP / DOWN: CHOOSE     LEFT / RIGHT: CHANGE     ESC, F1 OR B: CLOSE", (float)(w / 2), py0 + ph + U(50),
         U(26), COL_GREY, 230, 2);
#undef U
    return g_px;
}

/* ------------------------------------------------------ controls page --- */
static const char* const k_input_labels[GCI_COUNT] = {
    "A", "B", "X", "Y", "Z", "START", "L", "R",
    "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT",
    "STICK UP", "STICK DOWN", "STICK LEFT", "STICK RIGHT",
    "C-STICK UP", "C-STICK DOWN", "C-STICK LEFT", "C-STICK RIGHT",
};

static void draw_controls(int w, int h, float u) {
#define U(v) ((int)((v) * u + 0.5f))
    int pw = U(1100), ph = U(1010);
    int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 232);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);
    int by0 = py0 - U(30), by1 = py0 + U(70);
    fill_slant(px0 - U(24), by0, px0 + U(380), by1, U(24), COL_ORANGE, 255);
    fill_slant(px0 + U(390), by0 + U(16), px0 + U(416), by1, U(22), COL_YELLOW, 255);
    text("CONTROLS", (float)(px0 + U(28)), by1 - U(20), U(68), 0x111111u, 255, 0);

    int lx = px0 + U(60), c0 = px0 + U(560), c1 = px0 + U(880);
    int top = py0 + U(100), row_h = U(34), fs = U(25);
    text("KEYBOARD", (float)c0, top, U(26), COL_ORANGE, 255, 2);
    text("CONTROLLER", (float)c1, top, U(26), COL_ORANGE, 255, 2);
    int ry = top + U(14);
    for (int r = 0; r < CROWS; r++) {
        int y0 = ry + r * row_h + (r >= CROW_KBPLAYER ? U(12) : 0);
        int base = y0 + U(27);
        bool sel = r == g_crow;
        if (sel) fill_rect(px0 + U(24), y0, px0 + pw - U(24), y0 + row_h - U(4), COL_ORANGE, 30);
        if (r == CROW_KBPLAYER || r == CROW_NINTENDO) {
            text(r == CROW_KBPLAYER ? "KEYBOARD PLAYS AS" : "NINTENDO CONTROLLERS", (float)lx, base, fs,
                 sel ? COL_YELLOW : COL_GREY, 255, 0);
            char v[40];
            if (r == CROW_KBPLAYER) snprintf(v, sizeof(v), "PLAYER %d", g_settings.keyboard_player + 1);
            else snprintf(v, sizeof(v), "%s", g_settings.nintendo_labels ? "BY LABEL (A = A)" : "BY POSITION");
            text(v, (float)c0, base, fs, sel ? COL_WHITE : 0xD2D2D2u, 255, 2);
            if (sel) {
                int vw = (int)text_width(v, fs);
                arrow(c0 - vw / 2 - U(22), base - U(10), U(8), -1, COL_YELLOW, 255);
                arrow(c0 + vw / 2 + U(22), base - U(10), U(8), 1, COL_YELLOW, 255);
            }
            continue;
        }
        if (r >= CROW_RESET) {
            text(r == CROW_RESET ? "RESET ALL TO DEFAULTS" : "BACK", (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY,
                 255, 0);
            if (sel) fill_rect(px0 + U(24), y0, px0 + U(32), y0 + row_h - U(4), COL_ORANGE, 255);
            continue;
        }
        text(k_input_labels[r], (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY, 255, 0);
        for (int c = 0; c < 2; c++) {
            int cx = c ? c1 : c0;
            int code = c ? g_settings.pad_bind[r] : g_settings.key_bind[r];
            const char* name = code < 0 ? "-" : c ? (g_pad_name ? g_pad_name(code) : "?") : (g_key_name ? g_key_name(code) : "?");
            bool cell = sel && c == g_ccol;
            if (cell) {
                fill_rect(cx - U(150), y0 + U(2), cx + U(150), y0 + row_h - U(6), COL_ORANGE, g_capture ? 200 : 90);
                if (g_capture) name = c ? "PRESS A BUTTON" : "PRESS A KEY";
            }
            text(name, (float)cx, base, fs, cell ? COL_WHITE : 0xD2D2D2u, 255, 2);
        }
    }
    if (g_players[0])
        text(g_players, (float)(w / 2), py0 + ph - U(50), U(22), COL_ORANGE, 255, 2);
    const char* help = g_capture ? "ESC: CANCEL     DELETE: NO BINDING"
                                 : "UP / DOWN: CHOOSE     LEFT / RIGHT: KEYBOARD OR CONTROLLER     ENTER OR A: CHANGE     ESC OR B: BACK";
    text(help, (float)(w / 2), py0 + ph - U(24), U(22), COL_GREY, 230, 2);
#undef U
}

/* ------------------------------------------------- menu colours page --- */
static void draw_colors(int w, int h, float u) {
#define U(v) ((int)((v) * u + 0.5f))
    int n = uicolors_slot_count(), rows = n + 2;
    int pw = U(820), row_h = U(60), top = U(120);
    int ph = top + rows * row_h + U(30) + U(250);
    int px0 = w - pw - U(40), py0 = (h - ph) / 2;     /* to the right: the game shows beside it */
    if (px0 < U(16)) px0 = U(16);
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 225);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);
    int by0 = py0 - U(30), by1 = py0 + U(70);
    fill_slant(px0 - U(24), by0, px0 + U(470), by1, U(24), COL_ORANGE, 255);
    fill_slant(px0 + U(480), by0 + U(16), px0 + U(506), by1, U(22), COL_YELLOW, 255);
    text("MENU COLOURS", (float)(px0 + U(28)), by1 - U(20), U(68), 0x111111u, 255, 0);

    int lx = px0 + U(50), sx = px0 + pw - U(90), vx = sx - U(40), fs = U(28);
    for (int r = 0; r < rows; r++) {
        int y0 = py0 + top + r * row_h + (r >= n ? U(14) : 0), base = y0 + U(40);
        bool sel = r == g_krow;
        if (sel) {
            fill_rect(px0 + U(20), y0, px0 + pw - U(20), y0 + row_h - U(6), COL_ORANGE, g_kedit ? 70 : 40);
            fill_rect(px0 + U(20), y0, px0 + U(28), y0 + row_h - U(6), COL_ORANGE, 255);
        }
        if (r >= n) {
            text(r == n ? "RESET ALL TO THE GAME'S OWN" : "BACK", (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY,
                 255, 0);
            continue;
        }
        text(uicolors_slot_name(r), (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY, 255, 0);
        u32 c = uicolors_slot_get(r);
        int p = uicolors_slot_preset(r);
        char v[48];
        if (p >= 0) snprintf(v, sizeof(v), "%s", uicolors_preset_name(p));
        else snprintf(v, sizeof(v), "CUSTOM  %06X", c);
        int vw = (int)text_width(v, fs);
        text(v, (float)vx, base, fs, sel ? COL_WHITE : 0xD2D2D2u, 255, 1);
        if (sel && !g_kedit) {
            arrow(vx - vw - U(22), base - U(11), U(8), -1, COL_YELLOW, 255);
            arrow(vx + U(16), base - U(11), U(8), 1, COL_YELLOW, 255);
        }
        /* The colour itself, beside the game's own (small, behind). */
        int sy0 = y0 + U(10), sy1 = y0 + row_h - U(16);
        fill_rect(sx + U(34), sy0 + U(10), sx + U(56), sy1, uicolors_slot_original(r), 255);
        fill_rect(sx, sy0, sx + U(40), sy1, 0xFFFFFFu, 255);
        fill_rect(sx + U(2), sy0 + U(2), sx + U(38), sy1 - U(2), c, 255);
    }

    /* Below: red / green / blue of the selected colour, or what it is. */
    int iy = py0 + top + rows * row_h + U(40);
    fill_rect(px0 + U(50), iy - U(10), px0 + pw - U(50), iy - U(8), COL_DIM, 255);
    if (g_krow < n) {
        u32 c = uicolors_slot_get(g_krow);
        static const char* const k_ch[3] = {"RED", "GREEN", "BLUE"};
        static const u32 k_chc[3] = {0xE04040u, 0x40C040u, 0x4080F0u};
        for (int k = 0; k < 3; k++) {
            int y = iy + U(20) + k * U(46), v = (int)((c >> (16 - 8 * k)) & 255);
            bool on = g_kedit && g_kch == k;
            text(k_ch[k], (float)lx, y + U(26), U(26), on ? COL_YELLOW : g_kedit ? COL_GREY : COL_DIM, 255, 0);
            int bx0 = lx + U(130), bx1 = px0 + pw - U(150);
            fill_rect(bx0, y + U(10), bx1, y + U(22), 0x303038u, 255);
            fill_rect(bx0, y + U(10), bx0 + (bx1 - bx0) * v / 255, y + U(22), k_chc[k], g_kedit ? 255 : 120);
            char num[8];
            snprintf(num, sizeof(num), "%d", v);
            text(num, (float)(px0 + pw - U(60)), y + U(26), U(26), on ? COL_WHITE : COL_GREY, 255, 1);
            if (on) {
                arrow(bx0 - U(20), y + U(16), U(7), -1, COL_YELLOW, 255);
                arrow(bx1 + U(20), y + U(16), U(7), 1, COL_YELLOW, 255);
            }
        }
        text_wrapped(uicolors_slot_help(g_krow), (float)lx, iy + U(190), (float)(pw - U(100)), U(24), U(30),
                     COL_GREY, 255);
    } else {
        text_wrapped(g_krow == n ? "Put every colour on this page back to the game's own."
                                 : "Back to the video settings. Colours are saved to ui-colors.txt.",
                     (float)lx, iy + U(40), (float)(pw - U(100)), U(24), U(30), COL_GREY, 255);
    }
    const char* help = g_kedit ? "UP / DOWN: RED, GREEN OR BLUE     LEFT / RIGHT: CHANGE     A OR B: DONE"
                               : "LEFT / RIGHT: COLOUR     A / ENTER: FINE-TUNE     B / ESC: BACK";
    text(help, (float)(px0 + pw / 2), py0 + ph + U(40), U(22), COL_WHITE, 255, 2);
#undef U
}
