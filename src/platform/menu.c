#include "platform/menu.h"
#include "platform/settings.h"

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

static bool item_enabled(int i) {
    switch (i) {
    case MI_WINSIZE: return !g_settings.fullscreen;
    case MI_RES: return g_settings.renderer == 0;
    case MI_TEXTURES: return g_info.textures_found && g_settings.renderer == 0;
    default: return true;
    }
}

void menu_open(const MenuInfo* info) {
    g_info = *info;
    g_open = true;
    g_sel = 0;
    g_dirty = true;
}

bool menu_is_open(void) { return g_open; }

unsigned menu_take_changes(void) {
    unsigned c = g_changes;
    g_changes = 0;
    return c;
}

static void change(int item, int dir) {
    Settings* s = &g_settings;
    switch (item) {
    case MI_SHAPE: s->screen_shape = (s->screen_shape + 3 + dir) % 3; break;
    case MI_DISPLAY: s->fullscreen = !s->fullscreen; break;
    case MI_WINSIZE: s->window_size = (s->window_size - 1 + 4 + dir) % 4 + 1; break;
    case MI_RENDERER: s->renderer = !s->renderer; break;
    case MI_RES: s->resolution = (s->resolution - 1 + 8 + dir) % 8 + 1; break;
    case MI_TEXTURES: s->custom_textures = !s->custom_textures; break;
    default: return;
    }
    g_changes |= 1u << item;
}

void menu_action(int a) {
    if (!g_open) return;
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
        if (item_enabled(g_sel)) change(g_sel, a == MENU_LEFT ? -1 : 1);
        break;
    case MENU_OK:
        if (g_sel == MI_CLOSE) g_open = false;
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
    static const char* const k[MI_COUNT] = {"SCREEN SHAPE", "DISPLAY", "WINDOW SIZE", "RENDERER",
                                            "INTERNAL RESOLUTION", "CUSTOM TEXTURES", "BACK TO THE GAME"};
    return k[i];
}

static void item_value(int i, char* out, size_t cap) {
    const Settings* s = &g_settings;
    switch (i) {
    case MI_SHAPE: snprintf(out, cap, "%s", s->screen_shape == 1 ? "4:3" : s->screen_shape == 2 ? "16:9" : "GAME'S SETTING"); break;
    case MI_DISPLAY: snprintf(out, cap, "%s", s->fullscreen ? "FULL SCREEN" : "WINDOW"); break;
    case MI_WINSIZE: snprintf(out, cap, "%dX  (%d LINES)", s->window_size, 480 * s->window_size); break;
    case MI_RENDERER: snprintf(out, cap, "%s", s->renderer ? "SOFTWARE" : "GRAPHICS CARD"); break;
    case MI_RES:
        if (s->resolution == 1) snprintf(out, cap, "1X  (ORIGINAL)");
        else snprintf(out, cap, "%dX  (%dP)", s->resolution, 480 * s->resolution);
        break;
    case MI_TEXTURES:
        snprintf(out, cap, "%s", !g_info.textures_found ? "NONE FOUND" : s->custom_textures ? "ON" : "OFF");
        break;
    default: out[0] = 0; break;
    }
}

static const char* item_help(int i) {
    switch (i) {
    case MI_SHAPE: return "How the picture is shaped. Game's setting follows the widescreen option in the game's own "
                          "menu; 4:3 and 16:9 keep that shape whatever the game is set to.";
    case MI_DISPLAY: return "Play in a window or on the whole screen (F11 also switches).";
    case MI_WINSIZE: return "How big the window is when not playing full screen.";
    case MI_RENDERER: return "The graphics card draws faster and sharper than the software renderer. Takes effect "
                             "the next time the game starts.";
    case MI_RES: return "How sharply the graphics card draws the game, as a multiple of the GameCube's own picture. "
                        "Higher needs a faster card. Takes effect the next time the game starts.";
    case MI_TEXTURES: return g_info.textures_found
                                 ? "Replacement textures from the custom texture folder (Dolphin's format)."
                                 : "No custom textures were found. Put Dolphin-format texture packs in "
                                   "C:\\nfls2\\textures to use them.";
    default: return "Close this menu and carry on playing.";
    }
}

static bool needs_restart(int i) {
    const Settings* s = &g_settings;
    if (i == MI_RENDERER) return (s->renderer == 0) != g_info.gpu_running;
    if (i == MI_RES) return g_info.gpu_running && s->renderer == 0 && s->resolution != g_info.res_running;
    return false;
}

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
    fill_rect(0, 0, w, h, 0, 150);                     /* the game, dimmed */

    int pw = U(980), ph = U(820);
    int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 232);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);   /* bottom edge */

    /* Title on a slanted orange banner breaking out of the panel. */
    int by0 = py0 - U(36), by1 = py0 + U(78);
    fill_slant(px0 - U(24), by0, px0 + U(520), by1, U(28), COL_ORANGE, 255);
    fill_slant(px0 + U(530), by0 + U(18), px0 + U(560), by1, U(26), COL_YELLOW, 255);
    text("VIDEO SETTINGS", (float)(px0 + U(28)), by1 - U(24), U(78), 0x111111u, 255, 0);

    /* Rows */
    int row_h = U(74), ry = py0 + U(130), lx = px0 + U(60), vx = px0 + pw - U(70);
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
