#include "runtime/uicolors.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A UI screen's quad ("drawing record"): the word 0x40000000 (plain) or
   0x40030000 (textured, its texture's number next; a plain white texture
   tinted gives some of the solid bars), unused words of 0xFFFFFFFF, a zero
   word, then its four corners' colours as RGBA (the tint, when textured),
   then texture coordinates and the corners. */
#define REC_COLORS 0x14u
#define REC_SIZE 0x24u                 /* as far as this needs to read */

/* Where the game loads its UI screens (its upper heap; guest offsets),
   on to the end of memory (the port's own added menu parts live past the
   game's 24 MB). */
#define SCAN_LO 0x00700000u
#define SLICES 8u                      /* the whole range every 8 fields */

typedef struct {
    u32 from, to;                      /* RGB in the low 24 bits */
    int alpha;                         /* -1: keep the shape's own */
} Swap;

#define MAX_SWAPS 96
static Swap g_file[MAX_SWAPS];         /* the file's own lines (not the menu's colours) */
static int g_nfile;
static Swap g_swaps[MAX_SWAPS];        /* in use: the menu's colours, then the file's */
static int g_nswaps;
static u32 g_gen = 1;                  /* bumped whenever the swaps change */
static char g_path[512] = "ui-colors.txt";
static u32 g_slice;
static unsigned g_changed;             /* shapes recoloured so far (for the log) */
static bool g_any_done;

/* ------------------------------------------------ the menu's colours --- */
/* Named colours: the first is the one shown and picked; the rest (shading
   of the same shape) follow it, keeping how they differ from it. */
typedef struct {
    const char* name;
    const char* help;
    u32 col[4];
    int ncol;
} Slot;

static const Slot k_slots[] = {
    {"MAIN MENU BARS", "The red bars behind the main menu's buttons.", {0x500000}, 1},
    {"MAIN MENU LOGO BAR", "The red bar under the main menu's logo (and the panel behind the logo).", {0x640000}, 1},
    {"MUSIC TICKER BAR", "The orange bar of the music ticker that shows the song playing.", {0xDC3D00}, 1},
    {"MUSIC TICKER BORDER", "The gold border around the music ticker.", {0xDCA500}, 1},
    {"MUSIC TICKER TAB", "The red tab on the right of the music ticker, with its shading.",
     {0xA80000, 0x740000, 0xB45153}, 3},
};
#define NSLOTS (int)(sizeof(k_slots) / sizeof(k_slots[0]))
static u32 g_slot_to[NSLOTS];          /* the colour picked (== the original: unchanged) */
static int g_slot_alpha[NSLOTS];       /* -1: the shapes' own see-through-ness */

static void rgb_to_hsv(u32 c, float* h, float* s, float* v) {
    float r = (float)((c >> 16) & 255) / 255.0f, g = (float)((c >> 8) & 255) / 255.0f, b = (float)(c & 255) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) { *h = 0; return; }
    if (mx == r) *h = fmodf((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) *h = (b - r) / d + 2.0f;
    else *h = (r - g) / d + 4.0f;
    *h *= 60.0f;
}

static u32 hsv_to_rgb(float h, float s, float v) {
    h = fmodf(h + 360.0f, 360.0f);
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2.0f) - 1)), m = v - c, r, g, b;
    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    int R = (int)lroundf((r + m) * 255), G = (int)lroundf((g + m) * 255), B = (int)lroundf((b + m) * 255);
    return (u32)R << 16 | (u32)G << 8 | (u32)B;
}

/* Where one of a slot's colours goes when its first one goes from `main`
   to `to`: the same turn of hue and the same change of strength and
   brightness. */
static u32 follow(u32 member, u32 main, u32 to) {
    if (member == main) return to;
    float mh, ms, mv, th, ts, tv, h, s, v;
    rgb_to_hsv(main, &mh, &ms, &mv);
    rgb_to_hsv(to, &th, &ts, &tv);
    rgb_to_hsv(member, &h, &s, &v);
    float sr = ms > 0.01f ? ts / ms : 1.0f, vr = mv > 0.01f ? tv / mv : 1.0f;
    if (ms <= 0.01f) s = ts;           /* from grey: take the new colour's strength */
    return hsv_to_rgb(h + (th - mh), s * sr, v * vr);
}

static void rebuild(void) {
    g_nswaps = 0;
    for (int i = 0; i < NSLOTS; i++) {
        const Slot* sl = &k_slots[i];
        if (g_slot_to[i] == sl->col[0] && g_slot_alpha[i] < 0) continue;
        for (int k = 0; k < sl->ncol && g_nswaps < MAX_SWAPS; k++)
            g_swaps[g_nswaps++] = (Swap){sl->col[k], follow(sl->col[k], sl->col[0], g_slot_to[i]), g_slot_alpha[i]};
    }
    for (int i = 0; i < g_nfile && g_nswaps < MAX_SWAPS; i++) g_swaps[g_nswaps++] = g_file[i];
    g_gen++;
}

/* The slot (and which of its colours) a colour belongs to, else -1. */
static int slot_of(u32 rgb, int* which) {
    for (int i = 0; i < NSLOTS; i++)
        for (int k = 0; k < k_slots[i].ncol; k++)
            if (k_slots[i].col[k] == rgb) { if (which) *which = k; return i; }
    return -1;
}

int uicolors_slot_count(void) { return NSLOTS; }
const char* uicolors_slot_name(int i) { return i >= 0 && i < NSLOTS ? k_slots[i].name : ""; }
const char* uicolors_slot_help(int i) { return i >= 0 && i < NSLOTS ? k_slots[i].help : ""; }
u32 uicolors_slot_original(int i) { return i >= 0 && i < NSLOTS ? k_slots[i].col[0] : 0; }
u32 uicolors_slot_get(int i) { return i >= 0 && i < NSLOTS ? g_slot_to[i] : 0; }

void uicolors_slot_set(int i, u32 rgb) {
    if (i < 0 || i >= NSLOTS) return;
    rgb &= 0xFFFFFFu;
    if (g_slot_to[i] == rgb) return;
    g_slot_to[i] = rgb;
    rebuild();
}

void uicolors_reset_all(void) {
    for (int i = 0; i < NSLOTS; i++) { g_slot_to[i] = k_slots[i].col[0]; g_slot_alpha[i] = -1; }
    rebuild();
}

/* Named hues for left / right, at the shape's own strength and brightness. */
static const struct { const char* name; float hue; } k_hues[] = {
    {"RED", 0}, {"ORANGE", 22}, {"GOLD", 42}, {"YELLOW", 56}, {"LIME", 90}, {"GREEN", 125}, {"TEAL", 170},
    {"SKY BLUE", 200}, {"BLUE", 225}, {"INDIGO", 250}, {"PURPLE", 275}, {"MAGENTA", 305}, {"PINK", 335},
};
#define NHUES (int)(sizeof(k_hues) / sizeof(k_hues[0]))

int uicolors_preset_count(void) { return NHUES + 3; }   /* + original, grey, white */

const char* uicolors_preset_name(int p) {
    if (p == 0) return "ORIGINAL";
    if (p <= NHUES) return k_hues[p - 1].name;
    return p == NHUES + 1 ? "GREY" : "WHITE";
}

u32 uicolors_preset_color(int slot, int p) {
    u32 o = uicolors_slot_original(slot);
    if (p <= 0) return o;
    float h, s, v;
    rgb_to_hsv(o, &h, &s, &v);
    if (p <= NHUES) return hsv_to_rgb(k_hues[p - 1].hue, s, v);
    if (p == NHUES + 1) return hsv_to_rgb(0, 0, v);
    return 0xFFFFFFu;
}

/* The preset the slot's colour is, else -1 (custom). */
int uicolors_slot_preset(int slot) {
    for (int p = 0; p < uicolors_preset_count(); p++)
        if (uicolors_preset_color(slot, p) == g_slot_to[slot]) return p;
    return -1;
}

/* ------------------------------------------------------------- file --- */
static bool parse_hex(const char** p, u32* v, int* digits) {
    const char* s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;                /* allow "#RRGGBB" */
    u32 x = 0;
    int n = 0;
    while (isxdigit((unsigned char)*s) && n < 8) {
        int c = tolower((unsigned char)*s);
        x = x << 4 | (u32)(c <= '9' ? c - '0' : c - 'a' + 10);
        s++;
        n++;
    }
    if (n != 6 && n != 8) return false;
    *v = x;
    *digits = n;
    *p = s;
    return true;
}

/* A line "from => to": true and the parts; *commented when it's a swap
   behind "# ". */
static bool parse_line(const char* line, Swap* w, bool* commented, bool* blank) {
    const char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    *commented = false;
    *blank = *s == '\n' || *s == '\r' || *s == 0;
    if (*blank) return false;
    if (*s == '#' && !isxdigit((unsigned char)s[1])) { *commented = true; s++; }
    u32 from, to;
    int nf, nt;
    if (!parse_hex(&s, &from, &nf)) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] != '=' || s[1] != '>') return false;
    s += 2;
    if (!parse_hex(&s, &to, &nt)) return false;
    w->from = nf == 8 ? from >> 8 : from;   /* an alpha on the left is ignored */
    w->to = nt == 8 ? to >> 8 : to;
    w->alpha = nt == 8 ? (int)(to & 0xFF) : -1;
    return true;
}

void uicolors_load(const char* path) {
    g_nfile = 0;
    for (int i = 0; i < NSLOTS; i++) { g_slot_to[i] = k_slots[i].col[0]; g_slot_alpha[i] = -1; }
    if (path) snprintf(g_path, sizeof(g_path), "%s", path);
    FILE* f = path ? fopen(path, "r") : NULL;
    if (!f) { rebuild(); return; }
    char line[256];
    int lineno = 0, off = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        Swap w;
        bool commented, blank;
        bool ok = parse_line(line, &w, &commented, &blank);
        if (blank) continue;
        if (commented || (!ok && line[strspn(line, " \t")] == '#')) {
            if (ok && w.from != w.to) off++;   /* a swap left switched off */
            continue;
        }
        if (!ok) {
            printf("ui colors: line %d isn't \"RRGGBB => RRGGBB\"; left out\n", lineno);
            continue;
        }
        int which, sl = slot_of(w.from, &which);
        if (sl >= 0) {                 /* a menu colour: its first sets it, the rest follow */
            if (which == 0) { g_slot_to[sl] = w.to; g_slot_alpha[sl] = w.alpha; }
            continue;
        }
        if (g_nfile == MAX_SWAPS) {
            printf("ui colors: more than %d lines; the rest are left out\n", MAX_SWAPS);
            break;
        }
        g_file[g_nfile++] = w;
    }
    fclose(f);
    rebuild();
    printf("ui colors: %d colour swap%s from %s\n", g_nswaps, g_nswaps == 1 ? "" : "s", path);
    if (off)
        printf("ui colors: %d changed line%s still start%s with \"# \" (switched off); remove the \"# \" to use %s\n",
               off, off == 1 ? "" : "s", off == 1 ? "s" : "", off == 1 ? "it" : "them");
}

#define MENU_HEADER "# Set in the game (Video Settings > Menu Colours):"

bool uicolors_save(void) {
    /* Keep the file's other lines (notes, switched-off examples, colours
       that aren't the menu's); the menu's colours go at the end. */
    char* keep = NULL;
    size_t nkeep = 0, cap = 0;
    FILE* f = fopen(g_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            Swap w;
            bool commented, blank;
            bool ok = parse_line(line, &w, &commented, &blank);
            if (ok && !commented && slot_of(w.from, NULL) >= 0) continue;
            if (strncmp(line, MENU_HEADER, strlen(MENU_HEADER)) == 0) continue;
            size_t n = strlen(line);
            if (nkeep + n + 2 > cap) {
                cap = (nkeep + n + 2) * 2;
                char* nk = (char*)realloc(keep, cap);
                if (!nk) { free(keep); fclose(f); return false; }
                keep = nk;
            }
            memcpy(keep + nkeep, line, n);
            nkeep += n;
            if (n && line[n - 1] != '\n') keep[nkeep++] = '\n';
        }
        fclose(f);
    }
    while (nkeep >= 2 && keep[nkeep - 1] == '\n' && keep[nkeep - 2] == '\n') nkeep--;   /* trailing blank lines */
    f = fopen(g_path, "w");
    if (!f) { free(keep); printf("ui colors: couldn't write %s\n", g_path); return false; }
    if (nkeep) fwrite(keep, 1, nkeep, f);
    free(keep);
    bool header = false;
    for (int i = 0; i < NSLOTS; i++) {
        const Slot* sl = &k_slots[i];
        if (g_slot_to[i] == sl->col[0] && g_slot_alpha[i] < 0) continue;
        if (!header) { fprintf(f, "\n%s\n", MENU_HEADER); header = true; }
        if (g_slot_alpha[i] < 0) fprintf(f, "%06X => %06X    # %s\n", sl->col[0], g_slot_to[i], sl->name);
        else fprintf(f, "%06X => %06X%02X    # %s\n", sl->col[0], g_slot_to[i], g_slot_alpha[i], sl->name);
    }
    fclose(f);
    printf("ui colors: saved to %s\n", g_path);
    return true;
}

/* ---------------------------------------------------------- recolour --- */
static u32 rd(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* Records recoloured: address, the game's own colours and the ones written,
   so colours picked again start from the game's (and a colour swapped to
   another that is swapped too isn't changed twice). A screen loaded again
   brings the game's colours back, and gets done again. */
#define DONE_SIZE 8192u
#define DONE_PROBES 8
typedef struct { u32 addr, gen; u32 orig[4], col[4]; } Done;
static Done g_done[DONE_SIZE];

static Done* done_find(u32 addr, bool make) {
    u32 h = ((addr >> 2) * 2654435761u) >> 19;
    Done* empty = NULL;
    for (int i = 0; i < DONE_PROBES; i++) {
        Done* d = &g_done[(h + (u32)i) % DONE_SIZE];
        if (d->addr == addr && d->gen) return d;
        if (!d->gen && !empty) empty = d;
    }
    if (!make) return NULL;
    return empty ? empty : &g_done[h % DONE_SIZE];   /* full: reuse */
}

static bool map_colors(const u32* orig, u32* out) {
    bool any = false;
    for (int k = 0; k < 4; k++) {
        out[k] = orig[k];
        u32 rgb = orig[k] >> 8;
        for (int i = 0; i < g_nswaps; i++) {
            if (g_swaps[i].from != rgb) continue;
            u32 a = g_swaps[i].alpha < 0 ? (orig[k] & 0xFF) : (u32)g_swaps[i].alpha;
            if ((orig[k] & 0xFF) == 0) a = 0;   /* fade edges stay faded */
            out[k] = g_swaps[i].to << 8 | a;
            any = true;
            break;
        }
    }
    return any;
}

static void recolour(u8* ram, u32 off) {
    u8* p = ram + off + REC_COLORS;
    u32 col[4], orig[4], want[4];
    for (int k = 0; k < 4; k++) col[k] = rd(p + k * 4);
    Done* d = done_find(off, false);
    if (d && memcmp(d->col, col, sizeof(col)) == 0) {
        if (d->gen == g_gen) return;   /* done, nothing changed since */
        memcpy(orig, d->orig, sizeof(orig));
    } else {
        memcpy(orig, col, sizeof(orig));   /* the game's own (new, or loaded again) */
        d = NULL;
    }
    bool any = map_colors(orig, want);
    if (!any && !d) return;            /* not ours */
    if (memcmp(want, col, sizeof(col)) != 0) {
        for (int k = 0; k < 4; k++) wr(p + k * 4, want[k]);
        g_changed++;
    }
    if (!d) d = done_find(off, true);
    d->addr = off;
    d->gen = g_gen;
    memcpy(d->orig, orig, sizeof(orig));
    memcpy(d->col, want, sizeof(want));
    g_any_done = true;
}

void uicolors_frame(CPUState* cpu) {
    if ((!g_nswaps && !g_any_done) || !cpu->ram) return;
    u8* ram = cpu->ram;
    u32 hi = cpu->ram_size;
    if (hi <= SCAN_LO + REC_SIZE) return;
    u32 step = ((hi - SCAN_LO) / SLICES + 3) & ~3u;
    u32 lo = SCAN_LO + step * g_slice;
    u32 end = lo + step;
    if (end > hi - REC_SIZE) end = hi - REC_SIZE;
    g_slice = (g_slice + 1) % SLICES;
    unsigned before = g_changed;
    for (u32 o = lo; o < end; o += 4) {
        if (ram[o] != 0x40 || ram[o + 2] || ram[o + 3] || (ram[o + 1] != 0 && ram[o + 1] != 3)) continue;
        bool textured = ram[o + 1] == 3;
        if ((!textured && (rd(ram + o + 4) != 0xFFFFFFFFu || rd(ram + o + 12) != 0xFFFFFFFFu)) ||
            rd(ram + o + 8) != 0xFFFFFFFFu || rd(ram + o + 16) != 0)   /* textured: +12 can be a second texture */
            continue;
        recolour(ram, o);
    }
    static bool told;
    if (!told && g_changed > before) {
        told = true;
        printf("ui colors: recolouring shapes (first at 0x%08X)\n", 0x80000000u + lo);
    }
}
