#include "runtime/lodfix.h"

#include <stdio.h>
#include <string.h>

/* The engine's level-of-detail manager (0x80234424, every frame) sizes each
   registered model by the area of its bounding box on screen, in -1..1
   units, and compares it with its detail table's thresholds (players: 0.30
   for the full model, 0.0725 for the medium one), then caps how many can use
   the top levels. With the camera made wider for 21:9 / 32:9, everything is
   that much narrower in those units, so players fell below the thresholds
   (and flickered as the cap moved them round). The thresholds are divided by
   the camera's shape over 16:9 while it is ultrawide, which gives the levels
   a 16:9 camera would, and put back otherwise.

   The registry: a count at 0x803EC0F0 (r13 - 0x61B0) and pointers from
   0x803A2648; each model's LOD block points at +0x18 to its table: a count
   at +0xC, entries from the pointer at +0x10, 0x50 bytes each, the threshold
   first. Tables are shared (one for all players); each threshold's own
   value is kept the first time it is seen. */
#define REG_COUNT 0x003EC0F0u
#define REG_LIST 0x003A2648u
#define CAMERA_ASPECT 0x003ECED4u      /* the match camera's shape (r13 - 0x53CC) */
#define LIMITS 0x002FB3F8u             /* how many models may use each level (7 words) */
#define NLIMITS 7
#define MAX_THR 64

typedef struct { u32 addr, orig, set; } Thr;
static Thr g_thr[MAX_THR];
static int g_nthr;
static float g_scale_now = 1.0f;

static u32 rd(const CPUState* c, u32 o) {
    const u8* p = c->ram + o;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static void wr(CPUState* c, u32 o, u32 v) {
    u8* p = c->ram + o;
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static bool ok(const CPUState* c, u32 ea, u32 n) {
    return ea >= 0x80000000u && ea - 0x80000000u + n <= c->ram_size;
}
static float asf(u32 v) { float f; memcpy(&f, &v, 4); return f; }
static u32 u32f(float f) { u32 v; memcpy(&v, &f, 4); return v; }

static Thr* thr_find(u32 addr, u32 cur) {
    for (int i = 0; i < g_nthr; i++)
        if (g_thr[i].addr == addr) {
            /* The game loaded new data there (not our value): take it as its own. */
            if (cur != g_thr[i].set) { g_thr[i].orig = cur; g_thr[i].set = cur; }
            return &g_thr[i];
        }
    if (g_nthr == MAX_THR) return NULL;
    g_thr[g_nthr] = (Thr){addr, cur, cur};
    return &g_thr[g_nthr++];
}

/* The caps: the most models each level may hold, the rest pushed down
   (smallest first). A wider view has more players on screen at once
   competing for the top levels, so they swapped in and out as the camera
   panned; the caps grow with the camera's width. */
static u32 g_lim_orig[NLIMITS], g_lim_set[NLIMITS];
static bool g_lim_have;

static void caps(CPUState* cpu, float scale) {
    for (int k = 0; k < NLIMITS; k++) {
        u32 a = LIMITS + (u32)k * 4u, cur = rd(cpu, a);
        if (!g_lim_have || cur != g_lim_set[k]) {   /* the game's own (or newly set) */
            g_lim_orig[k] = cur;
        }
        u32 o = g_lim_orig[k], want = o;
        if (scale > 1.0f && o > 0u && o < 1000u) {   /* 0: the game wants none there (left so) */
            want = (u32)((float)o * scale + 0.999f);
            if (want < 8u) want = 8u;                     /* a team's worth of players at least */
        }
        if (want != cur) wr(cpu, a, want);
        g_lim_set[k] = want;
    }
    g_lim_have = true;
}

/* Players too close to the camera: during play the game hides a player
   whose bounding box reaches past the camera's near plane (flag 0x2000 in
   his LOD block, checked in 0x801A28E8 while the byte at 0x803EB7E8 is set),
   so the camera never cuts through him. With a 16:9 view anyone that close
   is off the sides of the picture anyway; at 21:9 / 32:9 the extra width at
   the sides shows players that close, and they vanished and reappeared as
   the camera panned. While the camera is ultrawide the rule is off (the
   game's own setting is put back otherwise). */
#define NEAR_HIDE 0x003EB7E8u          /* r13 - 0x6AB8, a byte */
static u8 g_near_orig, g_near_set;
static bool g_near_have;

static void near_hide(CPUState* cpu, float scale) {
    u8 cur = cpu->ram[NEAR_HIDE];
    if (!g_near_have || cur != g_near_set) g_near_orig = cur;   /* the game's own (or newly set) */
    u8 want = scale > 1.0f ? 0 : g_near_orig;
    if (want != cur) cpu->ram[NEAR_HIDE] = want;
    g_near_set = want;
    g_near_have = true;
}

/* Rain (and snow): the game's weather particles (0x801A69A8 sets them up,
   0x801A6AF8 moves them: a drop reaching the ground leaves a splash there,
   fading, and starts again at the top). Weather setup (0x801A762C) gives
   rain 96 drops with splashes starting at alpha 100, except on one field
   (the field index at 0x8030E6E8 is 2: The Alley), where it cuts them to 10
   drops at alpha 20: the splashes then fail the alpha test (16) almost at
   once and never show. With PS2/Xbox rain on, the full settings are put
   back there. */
#define WEATHER_MODE 0x003ECC34u       /* r13 - 0x566C: 0 none, 1 snow, 2 rain */
#define WEATHER_COUNT 0x003EB858u      /* r13 - 0x6A48: drops moved and drawn */
#define WEATHER_SPLASH_A 0x003ECC3Du   /* r13 - 0x5663: a splash's starting alpha */
#define WEATHER_DROP_A 0x003ECC3Cu     /* r13 - 0x5664: a drop's alpha */
#define WEATHER_SPEED 0x003ECC38u      /* r13 - 0x5668: fall per frame (float) */
#define RAIN_SPEED 1.2f
extern int g_rain_style;

static void weather(CPUState* cpu) {
    if (!g_rain_style || rd(cpu, WEATHER_MODE) != 2u) return;
    if (rd(cpu, WEATHER_COUNT) == 10u && cpu->ram[WEATHER_SPLASH_A] == 0x14u && cpu->ram[WEATHER_DROP_A] == 0x14u) {
        wr(cpu, WEATHER_COUNT, 0x60u);
        cpu->ram[WEATHER_SPLASH_A] = 0x64u;
        cpu->ram[WEATHER_DROP_A] = 0x64u;
        printf("rain: full rain settings put back (splashes on)\n");
    }
    /* The fall speed (r13 - 0x5668, -0.3 a frame for rain): the PS2's rain
       falls a little faster. */
    u32 sp = rd(cpu, WEATHER_SPEED);
    if (sp == 0xBE99999Au) {
        float f = -0.3f * RAIN_SPEED;
        u32 v; memcpy(&v, &f, 4);
        wr(cpu, WEATHER_SPEED, v);
    }
}

/* Menu fonts kept loaded. The game's resource manager counts the users of
   each font loaded from uis_font.dat (a 16-bit count per font) and frees a
   font when the count drops to 0. In the port the big font's count keeps
   drifting down (more releases than uses) during the credits roll, so it
   reached 0 and was freed while the title and headings still used it, and
   they popped in and out. While a font is loaded its count is kept at
   least FONT_FLOOR, so it isn't freed by the drift; leaving a screen still
   frees them all (that sets the counts to 0). All of uis_font.dat is
   163 KB, within the manager's room for it.
   Resource manager 1 (r13 - 0x6364); its table of file types at +0xC,
   0x24 bytes each; type 2 is uis_font.dat (its name at +0xC); +0x1E the
   number of fonts, +0x20 the fonts (8 bytes: count, state, -, data). */
#define RES_MGR1 0x003EBF3Cu           /* 0x803F22A0 - 0x6364 */
#define FONT_NAME 0x802EBE98u          /* "uis_font.dat" */
#define FONT_FLOOR 64u
static void font_pin(CPUState* cpu) {
    u32 mgr = rd(cpu, RES_MGR1);
    if (!ok(cpu, mgr, 0x10)) return;
    u32 tab = rd(cpu, mgr - 0x80000000u + 0xC);
    if (!ok(cpu, tab, 3 * 0x24)) return;
    u32 e = tab - 0x80000000u + 2 * 0x24;
    if (rd(cpu, e + 0xC) != FONT_NAME) return;
    u32 items = rd(cpu, e + 0x20);
    int n = (int16_t)(rd(cpu, e + 0x1C) & 0xFFFFu);
    if (n <= 0 || n > 16 || !ok(cpu, items, (u32)n * 8)) return;
    for (int i = 0; i < n; i++) {
        u32 it = items - 0x80000000u + (u32)i * 8;
        u32 count = rd(cpu, it) >> 16;
        if (!rd(cpu, it + 4) || count == 0 || count >= FONT_FLOOR) continue;
        cpu->ram[it] = (u8)(FONT_FLOOR >> 8);
        cpu->ram[it + 1] = (u8)FONT_FLOOR;
    }
}

bool lodfix_frame(CPUState* cpu) {
    if (!cpu || !cpu->ram || cpu->ram_size < 0x01800000u) return false;
    float aspect = asf(rd(cpu, CAMERA_ASPECT));
    float scale = aspect > 1.9f && aspect < 8.0f ? aspect / (16.0f / 9.0f) : 1.0f;
    caps(cpu, scale);
    near_hide(cpu, scale);
    weather(cpu);
    font_pin(cpu);
    u32 n = rd(cpu, REG_COUNT);
    if (n > 512) return false;
    for (u32 i = 0; i < n; i++) {
        u32 obj = rd(cpu, REG_LIST + i * 4);
        if (!ok(cpu, obj, 0x20)) continue;
        u32 tbl = rd(cpu, obj - 0x80000000u + 0x18);
        if (!ok(cpu, tbl, 0x14)) continue;
        u32 cnt = rd(cpu, tbl - 0x80000000u + 0xC), ent = rd(cpu, tbl - 0x80000000u + 0x10);
        if (cnt == 0 || cnt > 8 || !ok(cpu, ent, cnt * 0x50)) continue;
        for (u32 k = 0; k < cnt; k++) {
            u32 a = ent - 0x80000000u + k * 0x50, cur = rd(cpu, a);
            float v = asf(cur);
            if (!(v >= 0.0f && v < 16.0f)) continue;   /* not a threshold */
            Thr* t = thr_find(a, cur);
            if (!t) return n >= 6;
            u32 want = scale == 1.0f ? t->orig : u32f(asf(t->orig) / scale);
            if (want != cur) wr(cpu, a, want);
            t->set = want;
        }
    }
    if (scale != g_scale_now) {
        g_scale_now = scale;
        printf("lod: detail thresholds and caps %s (camera shape %.3f; caps now %u %u %u)\n",
               scale == 1.0f ? "the game's own" : "matched to the wider camera", aspect, g_lim_set[0], g_lim_set[1],
               g_lim_set[2]);
    }
    return n >= 6;                     /* a match: both teams' players */
}
