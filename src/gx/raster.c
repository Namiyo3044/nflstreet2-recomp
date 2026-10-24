#include "runtime/prof.h"
#include "gx/raster.h"
#include "gx/state.h"
#include "gx/texture.h"
#include "gx/hires.h"
#include "gx/raster_internal.h"
#include "gx/raster_gl.h"
#include "runtime/interrupts.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif

#define MAX_DUMPS 300

u32 g_color[EFB_W * EFB_H];          /* 0xAARRGGBB */
u32 g_depth[EFB_W * EFB_H];          /* 24-bit, larger = farther */

static char g_dir[512] = "frames";
static u32 g_every;          /* render every Nth displayed frame */
static bool g_save;          /* and save it as frame_N.bmp */
static u32 g_frames;        /* frames copied to the display so far */
static u32 g_dumped;
static unsigned long long g_tris_skipped, g_tris_recorded;

/* Widescreen detection. With its widescreen option on, the game squeezes a
   16:9 picture into the same 640-pixel frame (anamorphic) and expects the TV
   to stretch it. It shows in the 3D projection: the field of view is set for
   a 16:9 screen. Each frame's full-screen perspective draws vote, and the
   result only changes after several frames agree. */
static u32 g_wide_draws, g_narrow_draws;      /* this frame */
static int g_wide_candidate = -1, g_wide_streak, g_widescreen = -1;
static float g_last_aspect;

static void note_projection(void) {
    if (gx_xf_reg[0x26] != 0)
        return;                                /* orthographic: 2D, says nothing */
    float p0 = fabsf(gx_f32(gx_xf_reg[0x20])), p2 = fabsf(gx_f32(gx_xf_reg[0x22]));
    float vw = fabsf(gx_f32(gx_xf_reg[0x1A])) * 2.0f, vh = fabsf(gx_f32(gx_xf_reg[0x1B])) * 2.0f;
    if (!(p0 > 1e-6f) || !(p2 > 1e-6f) || vw < 480.0f || vh < 300.0f)
        return;                                /* split screen, render-to-texture, ... */
    /* The shape of the screen the projection was made for, given that the
       640-pixel frame fills a 4:3 TV. */
    float aspect = p2 / p0 * (vh / vw) * (4.0f / 3.0f);
    g_last_aspect = aspect;
    if (aspect > 1.55f) g_wide_draws++;
    else g_narrow_draws++;
}

/* A frame only votes when it has a real 3D scene: loading screens and boot
   screens draw a few small 3D pieces (a spinning logo) at 4:3 whatever the
   game's setting, while menus and matches draw hundreds. */
#define WIDE_MIN_DRAWS 40u

static void vote_widescreen(void) {
    u32 total = g_wide_draws + g_narrow_draws;
    if (total < WIDE_MIN_DRAWS) {
        g_wide_draws = g_narrow_draws = 0;
        return;                                /* little or no 3D this frame: keep what we had */
    }
    int vote = g_wide_draws > g_narrow_draws;
    g_wide_draws = g_narrow_draws = 0;
    if (vote != g_wide_candidate) {
        g_wide_candidate = vote;
        g_wide_streak = 0;
    }
    if (++g_wide_streak >= 10 && g_widescreen != vote) {
        g_widescreen = vote;
        printf("screen shape: game is drawing for %s (%u 3D draws in the deciding frame, projection %.2f:1)\n",
               vote ? "16:9" : "4:3", total, g_last_aspect);
    }
}

void gx_raster_widescreen_reset(void) {
    g_widescreen = -1;
    g_wide_candidate = -1;
    g_wide_streak = 0;
}

int gx_raster_widescreen(void) {
    return g_widescreen;
}
static CPUState* g_cpu;     /* for texture fetches during rasterisation */

/* The transform unit state that vertices are transformed with: the live
   registers (software renderer, transforming as the game draws), or the GPU
   thread's own copy, kept up to date by replaying the recorded writes (GPU
   renderer, where transforming is deferred to that thread). */
static u32 g_xf_mem_local[0x800], g_xf_reg_local[0x100];
static const u32* XFM = gx_xf_mem;
static const u32* XFR = gx_xf_reg;
static bool g_defer;                   /* GPU renderer: transform on the GPU thread */

/* Which backend draws the recorded frames: the software rasteriser (worker
   threads) or the graphics card (raster_gl.c, on the game thread). */
static bool g_gpu;

static void texture_freed(const u32* texels) {
    glr_texture_freed(texels);
}

const char* gx_raster_gpu_name(void) { return g_gpu ? glr_gpu_name() : ""; }

void gx_raster_set_gpu_scale(int scale) {
    if (g_gpu) glr_set_scale(scale);
}

bool gx_raster_use_gpu(void* (*getproc)(const char*), int scale, void (*make_current)(void)) {
    if (!glr_init(getproc, scale, make_current))
        return false;
    g_gpu = true;
    gx_tex_set_free_hook(texture_freed);
    /* Transform on the GPU thread, from its own copy of the transform unit
       registers (starting from the current ones). */
    memcpy(g_xf_mem_local, gx_xf_mem, sizeof(g_xf_mem_local));
    memcpy(g_xf_reg_local, gx_xf_reg, sizeof(g_xf_reg_local));
    XFM = g_xf_mem_local;
    XFR = g_xf_reg_local;
    g_defer = true;
    return true;
}

/* The software frame buffer must hold the picture (a copy reads it).
   With the GPU, the read-back runs on the GPU thread (it owns the GL
   context); defined with the threads below. */
static void efb_to_memory(bool depth);
static void gpu_tex_copy(const TexCopy* info);
static const CmdList* expand_list(const CmdList* l);
static u32 gpu_tex_copy_for(u32 map);
static bool copy_dest_is(u32 map);

void gx_raster_config(const char* dir, u32 every) {
    if (dir) {
        strncpy(g_dir, dir, sizeof(g_dir) - 1);
        g_dir[sizeof(g_dir) - 1] = '\0';
    }
    g_every = every;
    g_save = every != 0;
    if (every)
        make_dir(g_dir);
}

bool gx_raster_active(void) {
    return g_every && ((g_frames + 1) % g_every) == 0;
}

/* ------------------------------------------------------ vertex decode --- */

static u32 rd16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 rd32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static u32 comp_size(u32 fmt) { return fmt <= 1 ? 1u : fmt <= 3 ? 2u : 4u; }

static float read_comp(const u8* p, u32 fmt, u32 shift) {
    float scale = 1.0f / (float)(1u << shift);
    switch (fmt) {
    case 0: return (float)p[0] * scale;
    case 1: return (float)(s8)p[0] * scale;
    case 2: return (float)rd16(p) * scale;
    case 3: return (float)(s16)rd16(p) * scale;
    default: return gx_f32(rd32(p));
    }
}

static u32 color_size(u32 fmt) {
    static const u32 sizes[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    return sizes[fmt & 7];
}

static u32 read_color(const u8* p, u32 fmt) {
    u32 r, g, b, a = 255;
    switch (fmt) {
    case 0: { u32 v = rd16(p); r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; break; }
    case 1: r = p[0]; g = p[1]; b = p[2]; break;
    case 2: r = p[0]; g = p[1]; b = p[2]; break;
    case 3: { u32 v = rd16(p); r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; break; }
    case 4: { u32 v = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
              r = ((v >> 18) & 63) * 255 / 63; g = ((v >> 12) & 63) * 255 / 63;
              b = ((v >> 6) & 63) * 255 / 63; a = (v & 63) * 255 / 63; break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    return (r << 24) | (g << 16) | (b << 8) | a;
}

static const u8* array_elem(CPUState* cpu, u32 array, u32 index, u32 size) {
    u32 addr = (gx_cp[0xA0 + array] & 0x03FFFFFFu) + index * gx_cp[0xB0 + array];
    addr &= 0x01FFFFFFu;
    if (addr + size > cpu->ram_size)
        return NULL;
    return cpu->ram + addr;
}

/* Reads one attribute that is either inline (type 1) or an 8/16-bit index
   into an array (types 2/3). Returns a pointer to its data, advancing p. */
static const u8* attr_data(CPUState* cpu, const u8** p, u32 type, u32 array, u32 size) {
    if (type == 1) {
        const u8* d = *p;
        *p += size;
        return d;
    }
    u32 idx = type == 2 ? **p : rd16(*p);
    *p += type == 2 ? 1 : 2;
    return array_elem(cpu, array, idx, size);
}

/* The layout of a draw's vertices, worked out once per draw from the
   vertex descriptor (CP 0x50/0x60) and format (CP 0x70-0x97). */
typedef struct {
    u32 lo, hi, mia, mib;
    u32 pos_type, pos_n, pos_fmt, pos_shift, pos_cs;
    u32 nrm_type, nrm_fmt, nrm_cs, nrm_shift;
    bool nbt, index3;
    u32 col_type[2], col_fmt[2];
    u32 tc_type[8], tc_n[8], tc_fmt[8], tc_shift[8], tc_cs[8];
} VtxFormat;

static void vtx_format(u32 vat, VtxFormat* f) {
    u32 lo = gx_cp[0x50], hi = gx_cp[0x60];
    u32 a = gx_cp[0x70 + vat], b = gx_cp[0x80 + vat], c = gx_cp[0x90 + vat];
    f->lo = lo; f->hi = hi;
    f->mia = gx_xf_reg[0x18]; f->mib = gx_xf_reg[0x19];
    f->pos_type = (lo >> 9) & 3u;
    f->pos_n = (a & 1u) ? 3u : 2u;
    f->pos_fmt = (a >> 1) & 7u;
    f->pos_shift = (a >> 4) & 31u;
    f->pos_cs = comp_size(f->pos_fmt);
    f->nrm_type = (lo >> 11) & 3u;
    f->nbt = (a >> 9) & 1u;
    f->index3 = (a >> 31) & 1u;
    f->nrm_fmt = (a >> 10) & 7u;
    f->nrm_cs = comp_size(f->nrm_fmt);
    f->nrm_shift = f->nrm_cs == 1 ? 6u : f->nrm_cs == 2 ? 14u : 0u;   /* s8 has 6 and s16 14 fraction bits */
    for (u32 ch = 0; ch < 2; ch++) {
        f->col_type[ch] = (lo >> (13 + ch * 2)) & 3u;
        f->col_fmt[ch] = (a >> (14 + ch * 4)) & 7u;
    }
    const u32 tc_cnt[8] = {(a >> 21) & 1u, b & 1u, (b >> 9) & 1u, (b >> 18) & 1u,
                           (b >> 27) & 1u, (c >> 5) & 1u, (c >> 14) & 1u, (c >> 23) & 1u};
    const u32 tc_fmt[8] = {(a >> 22) & 7u, (b >> 1) & 7u, (b >> 10) & 7u, (b >> 19) & 7u,
                           (b >> 28) & 7u, (c >> 6) & 7u, (c >> 15) & 7u, (c >> 24) & 7u};
    const u32 tc_shift[8] = {(a >> 25) & 31u, (b >> 4) & 31u, (b >> 13) & 31u, (b >> 22) & 31u,
                             c & 31u, (c >> 9) & 31u, (c >> 18) & 31u, (c >> 27) & 31u};
    for (u32 i = 0; i < 8; i++) {
        f->tc_type[i] = (hi >> (i * 2)) & 3u;
        f->tc_n[i] = tc_cnt[i] ? 2u : 1u;
        f->tc_fmt[i] = tc_fmt[i];
        f->tc_shift[i] = tc_shift[i];
        f->tc_cs[i] = comp_size(tc_fmt[i]);
    }
}

/* Decodes one vertex; returns bytes consumed. Must agree with vertex_size()
   in fifo.c. */
static u32 decode_vertex(CPUState* cpu, const VtxFormat* f, const u8* p, Vtx* v) {
    const u8* start = p;
    u32 lo = f->lo;

    memset(v, 0, sizeof(*v));
    v->pos_mtx = f->mia & 0x3Fu;
    for (u32 i = 0; i < 4; i++) v->tex_mtx[i] = (f->mia >> (6 + 6 * i)) & 0x3Fu;
    for (u32 i = 0; i < 4; i++) v->tex_mtx[4 + i] = (f->mib >> (6 * i)) & 0x3Fu;

    if (lo & 1u) v->pos_mtx = *p++ & 0x3Fu;
    if (lo & 0x1FEu)
        for (u32 i = 0; i < 8; i++)
            if ((lo >> (1 + i)) & 1u) v->tex_mtx[i] = *p++ & 0x3Fu;

    if (f->pos_type) {
        u32 cs = f->pos_cs;
        const u8* d = attr_data(cpu, &p, f->pos_type, 0, f->pos_n * cs);
        if (d)
            for (u32 k = 0; k < f->pos_n; k++)
                v->pos[k] = read_comp(d + k * cs, f->pos_fmt, f->pos_shift);
    }

    v->nrm[2] = 1.0f;
    if (f->nrm_type) {   /* normal (and binormal/tangent, which are skipped) */
        u32 cs = f->nrm_cs;
        const u8* d = NULL;
        if (f->nrm_type == 1) {
            d = p;
            p += (f->nbt ? 9u : 3u) * cs;
        } else {
            u32 n_idx = (f->nbt && f->index3) ? 3u : 1u, isz = f->nrm_type == 2 ? 1u : 2u;
            u32 idx = f->nrm_type == 2 ? *p : rd16(p);
            d = array_elem(cpu, 1, idx, 3 * cs);
            p += n_idx * isz;
        }
        if (d)
            for (u32 k = 0; k < 3; k++)
                v->nrm[k] = read_comp(d + k * cs, f->nrm_fmt, f->nrm_shift);
    }

    for (u32 ch = 0; ch < 2; ch++) {
        u32 t = f->col_type[ch];
        if (!t)
            continue;
        const u8* d = attr_data(cpu, &p, t, 2 + ch, color_size(f->col_fmt[ch]));
        if (d) {
            v->color[ch] = read_color(d, f->col_fmt[ch]);
            v->has_color[ch] = true;
        }
    }

    if (f->hi)
        for (u32 i = 0; i < 8; i++) {
            u32 t = f->tc_type[i];
            if (!t)
                continue;
            u32 n = f->tc_n[i], cs = f->tc_cs[i];
            const u8* d = attr_data(cpu, &p, t, 4 + i, n * cs);
            if (d) {
                v->tc[i][0] = read_comp(d, f->tc_fmt[i], f->tc_shift[i]);
                v->tc[i][1] = n == 2 ? read_comp(d + cs, f->tc_fmt[i], f->tc_shift[i]) : 0.0f;
                v->has_tc[i] = true;
            }
        }
    return (u32)(p - start);
}

/* ---------------------------------------------------------- transform --- */

static float clamp01(float v);
static float g_u8f[256];               /* i / 255 */

static void unpack(u32 rgba, float out[4]) {
    out[0] = (float)(rgba >> 24) / 255.0f;
    out[1] = (float)((rgba >> 16) & 255) / 255.0f;
    out[2] = (float)((rgba >> 8) & 255) / 255.0f;
    out[3] = (float)(rgba & 255) / 255.0f;
}

/* Lighting channel (XF 0x100E/0x1010 + ch): material colour from its
   register or the vertex colour; if lighting is on, it is multiplied by
   ambient (register or vertex colour) plus the enabled lights, each with
   its colour, diffuse function and attenuation, as the transform unit does.
   pos / nrm are the vertex position and normal in view space. Light n lives
   at XF 0x600 + 16n: colour +3, cos attenuation +4..6, distance attenuation
   +7..9, position +10..12, direction +13..15.

   Everything that is the same for the whole draw (the channel settings,
   material and ambient colours, each light's values) is worked out once per
   draw in draw_setup; only the per-vertex part is left here. */
typedef struct {
    float col[4], lp[3], ld[3], k[6];
    float sdir[3];                     /* specular: the position, normalised */
} LightSetup;

typedef struct {
    u32 cctrl[2], actrl[2];
    float mat[2][4], amb[2][4];
    LightSetup light[8];
    bool lit, chan1, persp;
    u32 ntex;
    float xs;                          /* horizontal correction of the projection (1 = none) */
} DrawSetup;
static DrawSetup g_setup;

/* The screen shape the port shows when it is fixed (1 = 4:3, 2 = 16:9; 0 =
   follow the game). Full-screen 3D the game still draws for the other shape
   (for a moment after the setting changes, until the game sets up its next
   screen) is narrowed or widened to fit, so it never looks stretched; only
   while the game is seen drawing for the other shape. */
static volatile int g_target_shape;

void gx_raster_set_target_shape(int shape) {
    g_target_shape = shape;
}

static volatile u32 g_side_fill;       /* 0xRRGGBB to clear frames to (0: as the game asks) */

void gx_raster_set_side_fill(u32 rgb) { g_side_fill = rgb; }

/* Test shapes wider than 16:9: 3 = 21:9, 4 = 32:9. */
static float ultrawide_aspect(int target) { return target == 4 ? 32.0f / 9.0f : 21.0f / 9.0f; }

static float projection_correction(void) {
    int target = g_target_shape;
    if (!target)
        return 1.0f;
    if (XFR[0x26] != 0) {
        /* 2D at 21:9 (test): full-screen 2D (menus, the scoreboard) kept at
           16:9 in the middle of the wider picture, not stretched. */
        if (target < 3) return 1.0f;
        float vw = fabsf(gx_f32(XFR[0x1A])) * 2.0f, vh = fabsf(gx_f32(XFR[0x1B])) * 2.0f;
        return vw >= 480.0f && vh >= 300.0f ? (16.0f / 9.0f) / ultrawide_aspect(target) : 1.0f;
    }
    /* Once the game draws its screens for the shape shown, a projection
       made for the other shape is meant (a created player's portrait is
       drawn 4:3 and read back from the frame buffer): leave it alone. */
    if (target >= 3) {
        /* 21:9 (test): the game draws for 16:9; its full-screen 3D is
           widened to fill the wider picture (more seen at the sides). */
        float p0 = fabsf(gx_f32(XFR[0x20])), p2 = fabsf(gx_f32(XFR[0x22]));
        float vw = fabsf(gx_f32(XFR[0x1A])) * 2.0f, vh = fabsf(gx_f32(XFR[0x1B])) * 2.0f;
        if (!(p0 > 1e-6f) || !(p2 > 1e-6f) || vw < 480.0f || vh < 300.0f)
            return 1.0f;
        float aspect = p2 / p0 * (vh / vw) * (4.0f / 3.0f);
        float t = ultrawide_aspect(target);
        /* Any wide camera (the game's 16:9, or the wider one the port has it
           make, with some extra width so things just off the sides aren't
           skipped): fitted to the shape shown. */
        /* The shapes cameras are made with: 16:9, and the ultrawide ones the
           port has the game make (with or without the extra width), for
           this shape or the other (a screen's camera keeps its shape until
           the next screen). The nearest one; measured shapes run a little
           narrow. */
        static const float k_made[5] = {16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f, 21.0f / 9.0f * 1.25f,
                                        32.0f / 9.0f * 1.25f};
        int best = -1;
        float best_d = 0.12f;
        for (int i = 0; i < 5; i++) {
            float d = fabsf(aspect / k_made[i] - 1.0f);
            if (d < best_d) { best_d = d; best = i; }
        }
        if (best >= 0) return k_made[best] / t;
        if (fabsf(aspect - 4.0f / 3.0f) < 0.12f && g_widescreen != 1) return (4.0f / 3.0f) / t;
        return 1.0f;
    }
    {
        /* A camera still made for an ultrawide shape (the port had the game
           make it wider; a screen's camera keeps its shape until the next
           screen) shown at 4:3 or 16:9: fitted to the shape shown. */
        float p0 = fabsf(gx_f32(XFR[0x20])), p2 = fabsf(gx_f32(XFR[0x22]));
        float vw = fabsf(gx_f32(XFR[0x1A])) * 2.0f, vh = fabsf(gx_f32(XFR[0x1B])) * 2.0f;
        if (p0 > 1e-6f && p2 > 1e-6f && vw >= 480.0f && vh >= 300.0f) {
            float aspect = p2 / p0 * (vh / vw) * (4.0f / 3.0f);
            float shown = target == 1 ? 4.0f / 3.0f : 16.0f / 9.0f;
            static const float k_wide[4] = {21.0f / 9.0f, 32.0f / 9.0f, 21.0f / 9.0f * 1.25f, 32.0f / 9.0f * 1.25f};
            int best = -1;
            float best_d = 0.12f;
            for (int i = 0; i < 4; i++) {
                float d = fabsf(aspect / k_wide[i] - 1.0f);
                if (d < best_d) { best_d = d; best = i; }
            }
            if (best >= 0) return k_wide[best] / shown;
        }
    }
    if ((target == 2 && g_widescreen == 1) || (target == 1 && g_widescreen == 0))
        return 1.0f;
    float p0 = fabsf(gx_f32(XFR[0x20])), p2 = fabsf(gx_f32(XFR[0x22]));
    float vw = fabsf(gx_f32(XFR[0x1A])) * 2.0f, vh = fabsf(gx_f32(XFR[0x1B])) * 2.0f;
    if (!(p0 > 1e-6f) || !(p2 > 1e-6f) || vw < 480.0f || vh < 300.0f)
        return 1.0f;                   /* not a full-screen view (as in note_projection) */
    float aspect = p2 / p0 * (vh / vw) * (4.0f / 3.0f);
    if (target == 1 && fabsf(aspect - 16.0f / 9.0f) < 0.15f) return 4.0f / 3.0f;   /* made for 16:9, shown 4:3 */
    if (target == 2 && fabsf(aspect - 4.0f / 3.0f) < 0.12f) return 3.0f / 4.0f;    /* made for 4:3, shown 16:9 */
    return 1.0f;
}

static void draw_setup(void) {
    DrawSetup* d = &g_setup;
    u32 mask = 0;
    for (u32 ch = 0; ch < 2; ch++) {
        d->cctrl[ch] = XFR[0x0E + ch];
        d->actrl[ch] = XFR[0x10 + ch];
        unpack(XFR[0x0C + ch], d->mat[ch]);
        unpack(XFR[0x0A + ch], d->amb[ch]);
        for (int k = 0; k < 2; k++) {
            u32 ctrl = k ? d->actrl[ch] : d->cctrl[ch];
            if (ctrl & 2u) mask |= ((ctrl >> 2) & 0xFu) | (((ctrl >> 11) & 0xFu) << 4);
        }
    }
    d->lit = ((XFR[0x0E] | XFR[0x0F] | XFR[0x10] | XFR[0x11]) & 2u) != 0;
    d->chan1 = (XFR[0x09] & 3u) >= 2u;
    d->persp = XFR[0x26] == 0;
    d->ntex = XFR[0x3F] & 15u;
    d->xs = projection_correction();
    for (u32 n = 0; n < 8; n++) {
        if (!(mask & (1u << n)))
            continue;
        const u32* L = &XFM[0x600u + n * 16u];
        LightSetup* ls = &d->light[n];
        u32 c = L[3];
        ls->col[0] = (float)(c >> 24) / 255.0f; ls->col[1] = (float)((c >> 16) & 255) / 255.0f;
        ls->col[2] = (float)((c >> 8) & 255) / 255.0f; ls->col[3] = (float)(c & 255) / 255.0f;
        for (int k = 0; k < 6; k++) ls->k[k] = gx_f32(L[4 + k]);
        for (int k = 0; k < 3; k++) { ls->lp[k] = gx_f32(L[10 + k]); ls->ld[k] = gx_f32(L[13 + k]); }
        float len = sqrtf(ls->lp[0] * ls->lp[0] + ls->lp[1] * ls->lp[1] + ls->lp[2] * ls->lp[2]);
        for (int k = 0; k < 3; k++) ls->sdir[k] = len > 1e-9f ? ls->lp[k] / len : 0.0f;
    }
}

static void light_sum(u32 ctrl, const float pos[3], const float nrm[3], float lit[4], bool alpha) {
    u32 mask = ((ctrl >> 2) & 0xFu) | (((ctrl >> 11) & 0xFu) << 4);
    u32 diff_fn = (ctrl >> 7) & 3u, attn_fn = (ctrl >> 9) & 3u;
    for (u32 n = 0; n < 8; n++) {
        if (!(mask & (1u << n)))
            continue;
        const LightSetup* L = &g_setup.light[n];
        const float* lp = L->lp;
        const float* ld = L->ld;
        float ldir[3], attn = 1.0f;
        if (attn_fn == 0) {               /* specular */
            ldir[0] = L->sdir[0]; ldir[1] = L->sdir[1]; ldir[2] = L->sdir[2];
            float nd = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            float h = nd >= 0.0f ? fmaxf(0.0f, nrm[0] * ld[0] + nrm[1] * ld[1] + nrm[2] * ld[2]) : 0.0f;
            float num = L->k[0] + L->k[1] * h + L->k[2] * h * h;
            float den = L->k[3] + L->k[4] * h + L->k[5] * h * h;
            attn = fabsf(den) > 1e-9f ? fmaxf(0.0f, num) / den : 0.0f;
        } else {
            float v[3] = {lp[0] - pos[0], lp[1] - pos[1], lp[2] - pos[2]};
            float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2], d = sqrtf(d2);
            for (int k = 0; k < 3; k++) ldir[k] = d > 1e-9f ? v[k] / d : 0.0f;
            if (attn_fn == 1) {           /* spot */
                float cs = fmaxf(0.0f, ldir[0] * ld[0] + ldir[1] * ld[1] + ldir[2] * ld[2]);
                float num = L->k[0] + L->k[1] * cs + L->k[2] * cs * cs;
                float den = L->k[3] + L->k[4] * d + L->k[5] * d2;
                attn = fabsf(den) > 1e-9f ? fmaxf(0.0f, num) / den : 0.0f;
            }
        }
        float dif = 1.0f;
        if (diff_fn != 0) {
            float nd = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            dif = diff_fn == 1 ? nd : fmaxf(0.0f, nd);
        }
        float f = dif * attn;
        if (alpha) {
            lit[3] += L->col[3] * f;
        } else {
            for (int k = 0; k < 3; k++) lit[k] += L->col[k] * f;
        }
    }
}

static inline void unpack_tab(u32 rgba, float out[4]) {   /* as unpack(), from the i/255 table */
    out[0] = g_u8f[rgba >> 24]; out[1] = g_u8f[(rgba >> 16) & 255];
    out[2] = g_u8f[(rgba >> 8) & 255]; out[3] = g_u8f[rgba & 255];
}

static void channel(const Vtx* v, u32 ch, const float pos[3], const float nrm[3], float out[4]) {
    u32 cctrl = g_setup.cctrl[ch], actrl = g_setup.actrl[ch];
    const float* mat = g_setup.mat[ch];
    const float* amb = g_setup.amb[ch];
    float vc[4];
    unpack_tab(v->has_color[ch] ? v->color[ch] : 0xFFFFFFFFu, vc);

    /* colour */
    const float* m = (cctrl & 1u) ? vc : mat;
    if (cctrl & 2u) {
        float lit[4];
        memcpy(lit, (cctrl & 0x40u) ? vc : amb, sizeof(lit));
        light_sum(cctrl, pos, nrm, lit, false);
        for (int k = 0; k < 3; k++) out[k] = m[k] * clamp01(lit[k]);
    } else {
        for (int k = 0; k < 3; k++) out[k] = m[k];
    }
    /* alpha */
    float ma = (actrl & 1u) ? vc[3] : mat[3];
    if (actrl & 2u) {
        float lit[4];
        lit[3] = (actrl & 0x40u) ? vc[3] : amb[3];
        light_sum(actrl, pos, nrm, lit, true);
        out[3] = ma * clamp01(lit[3]);
    } else {
        out[3] = ma;
    }
}

/* Texture coordinate generation (XF 0x1040+i): regular texgens multiply the
   chosen input (position or a texture coordinate) by a texture matrix. */
static void texgen(const Vtx* v, u32 i, float out[3]) {
    u32 ctl = XFR[0x40 + i];
    bool proj = (ctl >> 1) & 1u;
    u32 type = (ctl >> 4) & 7u;
    u32 src = (ctl >> 7) & 31u;
    float in[4] = {0, 0, 1, 1};

    out[0] = out[1] = 0.0f;
    out[2] = 1.0f;
    if (type != 0) {             /* emboss / colour texgens: not used for menus */
        if (type >= 2) { out[0] = 0.5f; out[1] = 0.5f; }
        return;
    }
    if (src == 0) {              /* position */
        in[0] = v->pos[0]; in[1] = v->pos[1]; in[2] = v->pos[2];
    } else if (src >= 5 && src <= 12) {
        u32 k = src - 5;
        in[0] = v->tc[k][0]; in[1] = v->tc[k][1];
        in[2] = 1.0f;            /* AB11 */
    }

    const u32* m = &XFM[(v->tex_mtx[i] * 4u) & 0x3FFu];
    float s = gx_f32(m[0]) * in[0] + gx_f32(m[1]) * in[1] + gx_f32(m[2]) * in[2] + gx_f32(m[3]) * in[3];
    float t = gx_f32(m[4]) * in[0] + gx_f32(m[5]) * in[1] + gx_f32(m[6]) * in[2] + gx_f32(m[7]) * in[3];
    float q = proj ? gx_f32(m[8]) * in[0] + gx_f32(m[9]) * in[1] + gx_f32(m[10]) * in[2] + gx_f32(m[11]) * in[3]
                   : 1.0f;

    /* Optional second ("dual") texture transform. */
    if (XFR[0x12] & 1u) {
        u32 post = XFR[0x50 + i];
        float vin[3] = {s, t, q};
        if ((post >> 8) & 1u) {
            float len = sqrtf(vin[0] * vin[0] + vin[1] * vin[1] + vin[2] * vin[2]);
            if (len > 1e-9f) { vin[0] /= len; vin[1] /= len; vin[2] /= len; }
        }
        const u32* pm = &XFM[0x500u + ((post & 0x3Fu) * 4u)];
        s = gx_f32(pm[0]) * vin[0] + gx_f32(pm[1]) * vin[1] + gx_f32(pm[2]) * vin[2] + gx_f32(pm[3]);
        t = gx_f32(pm[4]) * vin[0] + gx_f32(pm[5]) * vin[1] + gx_f32(pm[6]) * vin[2] + gx_f32(pm[7]);
        q = gx_f32(pm[8]) * vin[0] + gx_f32(pm[9]) * vin[1] + gx_f32(pm[10]) * vin[2] + gx_f32(pm[11]);
    }
    out[0] = s; out[1] = t; out[2] = q;
}

/* Clip space to screen: the divide by w and the viewport. */
static void project(SVtx* out) {
    float cw = out->clip[3];
    out->valid = cw > 1e-6f;
    if (!out->valid)
        return;
    out->inv_w = 1.0f / cw;
    const u32* vp = &XFR[0x1A];
    out->x = out->clip[0] * out->inv_w * gx_f32(vp[0]) + gx_f32(vp[3]) - 342.0f;
    out->y = out->clip[1] * out->inv_w * gx_f32(vp[1]) + gx_f32(vp[4]) - 342.0f;
    out->z = out->clip[2] * out->inv_w * gx_f32(vp[2]) + gx_f32(vp[5]);
}

/* Ultrawide (test): 2D is kept at 16:9 in the middle, except screen-wide
   overlays (a colour filter over the whole picture, a flat panel), which are
   stretched to the full width so they cover the sides too. Pictures (opaque
   textured 2D, such as a loading screen's) stay 16:9 so they aren't
   distorted. */
static bool state_textured(const RenderState* rs) {
    for (u32 i = 0; i < rs->stages && i < 16; i++)
        if (rs->st[i].tex_on) return true;
    return false;
}

/* A copy of the whole screen drawn back as a texture (a crossfade: the last
   picture drawn over the new one as it fades; the loading screen's curtain:
   strips of it sliding off the sides): the copy holds the whole wider
   picture, so anything drawn from it in 2D is laid across the full width
   (not kept to 16:9 in the middle, which squeezed it, and stopped the
   curtain's strips at the 16:9 edges). Small copies (a created player's
   portrait) are left alone. */
static bool state_capture(const RenderState* rs) {
    for (u32 i = 0; i < rs->stages && i < 16; i++)
        if (rs->st[i].tex_on && (rs->st[i].efb_copy || rs->st[i].from_copy) && rs->st[i].tw >= 300u && rs->st[i].th >= 200u)
            return true;
    return false;
}

#define HUD_LOW -0.25f                 /* the top HUD: objects placed above this (-1 bottom .. 1 top); low enough for a style popup sliding away as it fades */
#define HUD_SIDE 0.30f                 /* and this far to one side (-1..1 across 16:9) */
#define HUD_PANELS 0.55f               /* the team panels' row: placed above this */
static u32 g_tick_seq_now, g_tick_seq = ~0u;   /* draws seen; the last music-ticker piece */
static float g_tick_mn, g_tick_mx;     /* where that piece lay (across 16:9) */
#define TICK_PEND 512
static u32 g_tick_pend[TICK_PEND][2];  /* the run's pieces so far (first vertex, count), before its disc */
static int g_tick_np;
static bool g_tick_ok;                 /* this run is the ticker */
static CmdList* g_tick_list;
static CmdList* g_wl;                  /* the list (and first vertex) of the draw being widened */
static u32 g_wb;
static int g_ovl_cur, g_ovl_prev;      /* pieces placed low and to the side, this / last frame */
static u32 g_ovl_frame;
#define HUD_OVERLAY 4                  /* that many: the play-calling screen is up */
static int g_banner_cur, g_banner_prev;   /* a stadium-name banner seen this / last frame (its side) */
static u32 g_banner_frame;
#define HUD_SIDE_LOW 0.50f             /* below it, this far to one side (style point popups) */
static volatile int g_hud_match;       /* a match is on (set from the game thread) */
void gx_raster_set_match(bool on) { g_hud_match = on; }

static void lerp_vtx(const SVtx* a, const SVtx* b, float t, SVtx* out);

/* Pieces crossing a side, remembered by what they look like (side, height
   of their object's origin, size): one whose origin moved since the last
   frame is sliding. */
#define SLIDE_MAX 64
typedef struct { float oy, w, h, ox; u32 frame; int side; } Slide;
static Slide g_slide[SLIDE_MAX];

static bool slide_moving(const SVtx* v, u32 n, float mn, float mx, float frame) {
    float ylo = 1e9f, yhi = -1e9f;
    for (u32 i = 0; i < n; i++) {
        float ny = v[i].clip[1] / v[i].clip[3];
        ylo = fminf(ylo, ny); yhi = fmaxf(yhi, ny);
    }
    int side = mn < -1.0f ? -1 : 1;
    float oy = v[0].org[1], ox = v[0].org[0] / frame, w = mx - mn, h = yhi - ylo;
    Slide* e = NULL;
    Slide* old = &g_slide[0];
    for (int k = 0; k < SLIDE_MAX; k++) {
        Slide* q = &g_slide[k];
        if (q->side == side && fabsf(q->oy - oy) < 0.002f && fabsf(q->w - w) < 0.01f && fabsf(q->h - h) < 0.01f &&
            g_frames - q->frame <= 2u) { e = q; break; }
        if (q->frame < old->frame) old = q;
    }
    bool moving = false;
    if (e) {
        if (e->frame != g_frames) moving = fabsf(e->ox - ox) > 0.002f;
        else moving = false;
    } else {
        e = old;
    }
    e->oy = oy; e->w = w; e->h = h; e->side = side;
    if (e->frame != g_frames || !moving) e->ox = ox;
    e->frame = g_frames;
    return moving;
}

/* Cut a piece made of quads at the 16:9 picture's sides: each corner past a
   side moves along its quad's edge (the corner level with it) to the side,
   colours and texture following. */
static void crop_at_sides(SVtx* v, u32 n, float frame) {
    if (n % 4u) return;
    for (u32 q = 0; q < n; q += 4) {
        SVtx c[4];
        memcpy(c, &v[q], sizeof c);
        for (int i = 0; i < 4; i++) {
            float ui = c[i].clip[0] / (c[i].clip[3] * frame);
            if (ui >= -1.0f && ui <= 1.0f) continue;
            float edge = ui < 0.0f ? -1.0f : 1.0f;
            float yi = c[i].clip[1] / c[i].clip[3];
            int best = -1;
            for (int j = 0; j < 4; j++) {
                if (j == i) continue;
                float yj = c[j].clip[1] / c[j].clip[3], uj = c[j].clip[0] / (c[j].clip[3] * frame);
                if (fabsf(yj - yi) < 1e-3f && (edge < 0.0f ? uj > -1.0f : uj < 1.0f)) { best = j; break; }
            }
            if (best < 0) continue;
            float uj = c[best].clip[0] / (c[best].clip[3] * frame);
            lerp_vtx(&c[i], &c[best], (edge - ui) / (uj - ui), &v[q + (u32)i]);
        }
    }
}

static const RenderState* g_wstate;    /* the state of the draw being widened */
/* Screen filters (a Recomp setting): in a match the game lays a see-through
   colour over the whole picture (a tint, field by field). With filters off
   it isn't drawn. Told apart from the fades (to black, between shots), which
   cover the screen too, by holding still: a filter keeps its colour and
   strength frame after frame, a fade changes them. */
int g_screen_filters = 1;              /* 1: the game's filters drawn (settings) */
#define FILTER_STEADY 20               /* frames unchanged before an overlay counts as a filter */
#define FILTER_MAX 8
static struct { u32 key; u32 frame; int steady; } g_filter[FILTER_MAX];

static bool filter_skip(SVtx* v, u32 n, bool capture) {
    if (g_screen_filters || !g_hud_match || g_setup.persp || capture || n < 3) return false;
    float xs = g_setup.xs > 1e-6f ? g_setup.xs : 1.0f, mn = 1e9f, mx = -1e9f, ylo = 1e9f, yhi = -1e9f, a = 0.0f;
    for (u32 i = 0; i < n; i++) {
        if (!(v[i].clip[3] > 1e-6f)) return false;
        float x = v[i].clip[0] / (v[i].clip[3] * xs), y = v[i].clip[1] / v[i].clip[3];
        mn = fminf(mn, x); mx = fmaxf(mx, x); ylo = fminf(ylo, y); yhi = fmaxf(yhi, y);
        a += v[i].col[0][3];
    }
    a /= (float)n;
    if (mn > -0.97f || mx < 0.97f || ylo > -0.95f || yhi < 0.95f || a > 0.9f) return false;
    const Stage* st0 = g_wstate && g_wstate->stages ? &g_wstate->st[0] : NULL;
    u32 key = (st0 && st0->tex_on ? st0->tex_addr : 0u) ^ ((u32)(v[0].col[0][0] * 255.0f) << 24) ^
              ((u32)(v[0].col[0][1] * 255.0f) << 16) ^ ((u32)(v[0].col[0][2] * 255.0f) << 8) ^ (u32)(a * 255.0f);
    int slot = -1, old = 0;
    for (int k = 0; k < FILTER_MAX; k++) {
        if (g_filter[k].key == key && g_frames - g_filter[k].frame <= 2u) { slot = k; break; }
        if (g_filter[k].frame < g_filter[old].frame) old = k;
    }
    if (slot < 0) { slot = old; g_filter[slot].key = key; g_filter[slot].steady = 0; g_filter[slot].frame = g_frames; }
    if (g_filter[slot].frame != g_frames) { g_filter[slot].frame = g_frames; g_filter[slot].steady++; }
    if (g_filter[slot].steady < FILTER_STEADY) return false;
    for (u32 i = 0; i < n; i++) {      /* out of sight */
        v[i].clip[0] = 4.0f * v[i].clip[3];
        project(&v[i]);
    }
    return true;
}

static void widen_fullscreen_2d(SVtx* v, u32 n, bool textured, bool ztest, bool capture) {
    u32 seq = ++g_tick_seq_now;        /* every draw counts (the ticker's run is unbroken) */
    if (filter_skip(v, n, capture)) return;
    int target = g_target_shape;
    if (target < 3 || n < 2) return;   /* lines (2) too: the Gamebreaker bar's glowing outline */
    /* Positions across the 16:9 picture (-1 its left edge, 1 its right),
       which sits in the middle of the wider one. */
    float frame = (16.0f / 9.0f) / ultrawide_aspect(target);
    if (capture && !g_setup.persp && g_setup.xs < 1.0f) {
        for (u32 i = 0; i < n; i++) {
            if (!(v[i].clip[3] > 1e-6f)) return;
        }
        for (u32 i = 0; i < n; i++) {
            v[i].clip[0] /= g_setup.xs;
            project(&v[i]);
        }
        return;
    }
    float mn = 1e9f, mx = -1e9f, alpha = 0.0f;
    for (u32 i = 0; i < n; i++) {
        if (!(v[i].clip[3] > 1e-6f)) return;
        float u = v[i].clip[0] / (v[i].clip[3] * frame);
        if (u < mn) mn = u;
        if (u > mx) mx = u;
        alpha += v[i].col[0][3];
    }
    if (!g_setup.persp && g_setup.xs < 1.0f && mn <= -0.97f && mx >= 0.97f) {   /* screen-wide 2D */
        /* An opaque picture (a loading screen's: a big texture). A small
           texture spread over the screen is a fill (the fade to black as a
           match starts, 128 x 128): stretched like a flat overlay. */
        const Stage* st0 = g_wstate && g_wstate->stages ? &g_wstate->st[0] : NULL;
        bool picture = !st0 || (st0->tw >= 256u && st0->th >= 256u);
        if (textured && !capture && picture && alpha / (float)n > 0.95f) return;
        for (u32 i = 0; i < n; i++) {
            v[i].clip[0] /= g_setup.xs;
            project(&v[i]);
        }
        return;
    }
    /* A flat (untextured) overlay shape running off one side of the 16:9
       picture (the scoreboard's red bars: menu-system shapes drawn in 3D
       with the match camera, no depth test): it was meant to reach the
       edge, so that end is taken to the edge of the wider picture. Scenery
       (depth-tested) is left alone. */
    if (g_setup.persp && ztest) return;
    /* A fade over the whole picture drawn as a 3D shape (the fade as a match
       starts: the last shot, copied off, drawn over the new one with the
       match camera, no depth test; or a flat colour in a match): it covered
       the 16:9 picture; it is stretched across the wider one. (Menu
       backgrounds, textured pictures, are left alone.) */
    if (g_setup.persp && !ztest && mn <= -0.97f && mx >= 0.97f && (capture || (!textured && g_hud_match))) {
        float ylo = 1e9f, yhi = -1e9f;
        for (u32 i = 0; i < n; i++) {
            float ny = v[i].clip[1] / v[i].clip[3];
            ylo = fminf(ylo, ny); yhi = fmaxf(yhi, ny);
        }
        if (ylo <= -0.95f && yhi >= 0.95f) {
            float edge = 1.0f / frame;
            for (u32 i = 0; i < n; i++) {
                float u = v[i].clip[0] / (v[i].clip[3] * frame);
                if (u <= -0.97f) v[i].clip[0] = -edge * v[i].clip[3] * frame;
                else if (u >= 0.97f) v[i].clip[0] = edge * v[i].clip[3] * frame;
                project(&v[i]);
            }
            return;
        }
    }
    /* The music ticker (the song popup at the bottom left, in menus and
       matches) moves left like the HUD's left side. It is drawn in one
       run: its box (placed left of the middle, low down), then each piece
       drawn straight after that lies low (end caps, tab, disc, the song's
       lines). Plenty else sits low on the left (button bars, menu text), so
       a run only counts once its disc turns up (placed from the top of the
       screen though drawn at the bottom); the pieces before it are moved
       then, still waiting in this list. */
    if (g_setup.persp && !ztest && g_wl) {
        float tu = v[0].org[0] / frame, ty = v[0].org[1], ylo = 1e9f, yhi = -1e9f;
        for (u32 i = 0; i < n; i++) {
            float ny = v[i].clip[1] / v[i].clip[3];
            ylo = fminf(ylo, ny); yhi = fmaxf(yhi, ny);
        }
        bool low = yhi < -0.40f;
        /* its box is tall (a button bar placed the same way is thin) */
        bool start = low && v[0].org[0] != 0.0f && tu < -0.30f && ty < -0.60f && ty > ylo - 0.3f && ty < yhi + 0.3f &&
                     yhi - ylo > 0.20f;
        bool next = low && g_tick_seq == seq - 1u && g_tick_list == g_wl &&
                    (mn < 0.25f || (mx - mn < 0.25f && mn >= g_tick_mn - 0.05f && mn <= g_tick_mx + 0.15f));
        if (g_tick_ok && mx - mn >= 0.5f) next = false;   /* after the disc only small pieces (a bar drawn next isn't it) */
        /* the disc: big (a button icon at the bottom is placed the same way
           but is much smaller) */
        bool disc = low && ty > 0.9f && tu < -0.30f && yhi - ylo > 0.22f;
        if (!next && start) { g_tick_np = 0; g_tick_ok = false; }   /* a new run */
        if (next || start) {
            g_tick_seq = seq; g_tick_mn = mn; g_tick_mx = mx; g_tick_list = g_wl;
            float r = fminf(ultrawide_aspect(target), 21.0f / 9.0f) / (16.0f / 9.0f);
            float shift = -(r - 1.0f) * frame;
            if (disc && !g_tick_ok) {          /* it is the ticker: move what came before */
                g_tick_ok = true;
                for (int k = 0; k < g_tick_np; k++)
                    for (u32 i = 0; i < g_tick_pend[k][1]; i++) {
                        SVtx* w = &g_wl->verts[g_tick_pend[k][0] + i];
                        w->clip[0] += shift * w->clip[3];
                        project(w);
                    }
                g_tick_np = 0;
            }
            if (g_tick_ok) {
                for (u32 i = 0; i < n; i++) {
                    v[i].clip[0] += shift * v[i].clip[3];
                    project(&v[i]);
                }
                return;
            }
            if (g_tick_np < TICK_PEND) { g_tick_pend[g_tick_np][0] = g_wb; g_tick_pend[g_tick_np][1] = n; g_tick_np++; }
            /* not known yet: treated as usual for now */
        }
    }
    /* Menus: a menu piece lying wholly beyond the 16:9 picture's sides was
       off the screen as the game made it (a list parked outside, waiting to
       slide in), and nothing covers that space at the wider shapes, so it
       showed (and left trails). It stays out of sight. */
    if (g_setup.persp && !ztest && !g_hud_match && (mx < -1.0f || mn > 1.0f)) {
        (void)slide_moving(v, n, mn, mx, frame);   /* remembered: it may slide in next */
        for (u32 i = 0; i < n; i++) {
            v[i].clip[0] = (mx < -1.0f ? -4.0f : 4.0f) * v[i].clip[3];
            project(&v[i]);
        }
        return;
    }
    /* The match HUD along the top (team 1 on the left, team 2 on the right):
       its panels move out towards the sides, to the edges of a 21:9 area
       (the screen's edges at 21:9; at 32:9 they'd be too far out to read
       at a glance); bars that ran off the side still reach the screen's
       edge. Below the panels the style point popups go with their side.
       What moves is decided by where the piece's object sits (its
       matrix's origin), not by the piece itself: every letter of a word
       shares it (words stay whole), it doesn't shift as a piece grows or
       fades, and a message centred on the screen sits in the middle. Only
       in a match (menus are left alone). */
    if (g_setup.persp && !ztest && g_hud_match) {
        float ou = v[0].org[0] / frame, oy = v[0].org[1];
        for (u32 i = 1; i < n; i++)
            if (v[i].mtx != v[0].mtx) { ou = (mn + mx) * 0.5f; break; }   /* mixed: by the piece itself */
        float ylo = 1e9f, yhi = -1e9f;
        for (u32 i = 0; i < n; i++) {
            float ny = v[i].clip[1] / v[i].clip[3];
            ylo = fminf(ylo, ny); yhi = fmaxf(yhi, ny);
        }
        /* Drawn with the camera's own matrix, or placed from somewhere far
           off (the music popup's disc is placed from the top of the screen):
           by the piece itself. */
        if ((fabsf(ou) < 0.02f && fabsf(oy) < 0.02f) || oy > yhi + 0.3f || oy < ylo - 0.3f) {
            ou = (mn + mx) * 0.5f;
            oy = ylo;
        }
        /* Up among the panels: anything placed to one side. Below them only
           what is placed well out to the side (the style point popups):
           the big messages in the middle (some drawn a letter-object at a
           time, "Conversion Failed") and the game's logo are placed nearer
           the middle, and the pre-match panels further down. */
        float side = oy >= HUD_PANELS ? HUD_SIDE : HUD_SIDE_LOW;
        /* The play-calling screen fills the lower half with play cards and
           button bars on both sides; its cards reach up into the popups'
           space, so while it is up only the panel row moves. Counted over
           this frame and the last. */
        if (g_frames != g_ovl_frame) { g_ovl_prev = g_ovl_cur; g_ovl_cur = 0; g_ovl_frame = g_frames; }
        /* (menu-system pieces only: single quads placed with matrix 0; a
           player's body parts aren't cards) */
        /* (nor the music ticker's box, drawn low on the left too) */
        if (oy < -0.1f && fabsf(ou) > 0.4f && v[0].mtx == 0 && n == 4 && g_tick_seq != seq && g_ovl_cur < 1000)
            g_ovl_cur++;
        if (oy < HUD_PANELS && (g_ovl_cur >= HUD_OVERLAY || g_ovl_prev >= HUD_OVERLAY)) side = 1e9f;
        /* A long bar in that row placed nearer the middle but lying mostly
           to one side (a stadium's name banner before the match): with
           its side, so the name stays inside it. */
        if (g_frames != g_banner_frame) {   /* a new frame */
            g_banner_prev = g_banner_cur; g_banner_cur = 0; g_banner_frame = g_frames;
        }
        if (ylo >= HUD_PANELS && ou >= -side && ou <= side) {
            if (mn < -0.6f && mx < 0.5f) ou = -1.0f, g_banner_cur = -1;
            else if (mx > 0.6f && mn > -0.5f) ou = 1.0f, g_banner_cur = 1;
            else if (g_banner_cur || g_banner_prev)
                ou = (float)(g_banner_cur ? g_banner_cur : g_banner_prev);   /* the rest of the banner (its faint big name) */
        }
        if (oy >= HUD_LOW && (ou < -side || ou > side)) {
            float r = fminf(ultrawide_aspect(target), 21.0f / 9.0f) / (16.0f / 9.0f);
            float shift = (ou < 0.0f ? -1.0f : 1.0f) * (r - 1.0f), edge = 1.0f / frame;
            bool across = mn < 1.0f && mx > -1.0f;   /* partly on the 16:9 picture */
            for (u32 i = 0; i < n; i++) {
                float u = v[i].clip[0] / (v[i].clip[3] * frame);
                float nu = across && u < -1.0f ? -edge : across && u > 1.0f ? edge : u + shift;   /* off the side: to the edge */
                v[i].clip[0] = nu * v[i].clip[3] * frame;
                project(&v[i]);
            }
            return;
        }
    }
    if ((mn >= -1.0f && mx <= 1.0f) || mx < -1.0f || mn > 1.0f) return;
    /* Menus: a piece crossing the side of the 16:9 picture as it slides in
       or out (its place changes from frame to frame) isn't a bar meant to
       reach the edge: it is cut at the side, as the game's screen did,
       rather than stretched to the screen's edge. */
    if (!g_hud_match && g_setup.persp && slide_moving(v, n, mn, mx, frame)) {
        crop_at_sides(v, n, frame);
        return;
    }
    if (textured) {
        /* Textured: only long thin bars (the main menu's logo bar is a tinted
           plain texture), not text or pictures sliding in from the side. */
        float y0 = 1e9f, y1 = -1e9f, x0 = 1e9f, x1 = -1e9f;
        for (u32 i = 0; i < n; i++) {
            if (v[i].y < y0) y0 = v[i].y;
            if (v[i].y > y1) y1 = v[i].y;
            if (v[i].x < x0) x0 = v[i].x;
            if (v[i].x > x1) x1 = v[i].x;
        }
        if (mx - mn < 0.4f || (x1 - x0) < 4.0f * (y1 - y0)) return;
    }
    for (u32 i = 0; i < n; i++) {
        float u = v[i].clip[0] / (v[i].clip[3] * frame);
        if (u < -1.0f || u > 1.0f) {
            v[i].clip[0] = u < 0.0f ? -v[i].clip[3] : v[i].clip[3];
            project(&v[i]);
        }
    }
}

/* A point between two vertices (t from a to b), interpolated in clip space. */
static void lerp_vtx(const SVtx* a, const SVtx* b, float t, SVtx* out) {
    for (int k = 0; k < 4; k++) out->clip[k] = a->clip[k] + (b->clip[k] - a->clip[k]) * t;
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 4; k++) out->col[c][k] = a->col[c][k] + (b->col[c][k] - a->col[c][k]) * t;
    for (int i = 0; i < MAX_TC; i++)
        for (int k = 0; k < 3; k++) out->tc[i][k] = a->tc[i][k] + (b->tc[i][k] - a->tc[i][k]) * t;
    out->near_d = a->near_d + (b->near_d - a->near_d) * t;
    out->far_d = a->far_d + (b->far_d - a->far_d) * t;
    project(out);
}

/* Free camera (debug): a 3x4 matrix applied to view-space positions of 3D
   (perspective) draws just before projection. Set from the window thread;
   each frame's transforming uses a copy taken when it starts. */
static float g_cam[12];
static volatile int g_cam_on;
static float g_cam_cur[12];
static int g_cam_cur_on;

void gx_raster_set_free_camera(bool on, const float m[12]) {
    if (m) memcpy(g_cam, m, sizeof(g_cam));
    g_cam_on = on;
}

static void take_free_camera(void) {
    g_cam_cur_on = g_cam_on;
    memcpy(g_cam_cur, g_cam, sizeof(g_cam_cur));
}

static void transform(const Vtx* in, SVtx* out) {
    const u32* m = &XFM[(in->pos_mtx * 4u) & 0x3FFu];
    float ex = gx_f32(m[0]) * in->pos[0] + gx_f32(m[1]) * in->pos[1] + gx_f32(m[2]) * in->pos[2] + gx_f32(m[3]);
    float ey = gx_f32(m[4]) * in->pos[0] + gx_f32(m[5]) * in->pos[1] + gx_f32(m[6]) * in->pos[2] + gx_f32(m[7]);
    float ez = gx_f32(m[8]) * in->pos[0] + gx_f32(m[9]) * in->pos[1] + gx_f32(m[10]) * in->pos[2] + gx_f32(m[11]);

    const u32* pr = &XFR[0x20];
    float cx, cy, cz, cw;
    if (g_setup.persp) {
        float qx = ex, qy = ey, qz = ez;   /* where the (free) camera sees it; lighting keeps the game's view */
        if (g_cam_cur_on) {
            const float* c = g_cam_cur;
            qx = c[0] * ex + c[1] * ey + c[2] * ez + c[3];
            qy = c[4] * ex + c[5] * ey + c[6] * ez + c[7];
            qz = c[8] * ex + c[9] * ey + c[10] * ez + c[11];
        }
        cx = (gx_f32(pr[0]) * qx + gx_f32(pr[1]) * qz) * g_setup.xs;
        cy = gx_f32(pr[2]) * qy + gx_f32(pr[3]) * qz;
        cz = gx_f32(pr[4]) * qz + gx_f32(pr[5]);
        cw = -qz;
    } else {
        cx = (gx_f32(pr[0]) * ex + gx_f32(pr[1])) * g_setup.xs;
        cy = gx_f32(pr[2]) * ey + gx_f32(pr[3]);
        cz = gx_f32(pr[4]) * ez + gx_f32(pr[5]);
        cw = 1.0f;
    }
    out->clip[0] = cx; out->clip[1] = cy; out->clip[2] = cz; out->clip[3] = cw;
    out->mtx = (u8)in->pos_mtx;
    out->org[0] = out->org[1] = 0.0f;
    if (g_setup.persp && g_target_shape >= 3) {
        /* the matrix's origin: menu-system pieces (HUD text, panels) are
           placed by their object's matrix, each letter offset within it */
        float oz = gx_f32(m[11]);
        if (oz < -1e-6f) {
            out->org[0] = (gx_f32(pr[0]) * gx_f32(m[3]) + gx_f32(pr[1]) * oz) * g_setup.xs / -oz;
            out->org[1] = (gx_f32(pr[2]) * gx_f32(m[7]) + gx_f32(pr[3]) * oz) / -oz;
        }
    }
    /* Perspective: GX's near plane is z = -w. Orthographic draws are never
       near-clipped (2D and full-screen effects). */
    out->near_d = g_setup.persp ? cz + cw : 1.0f;
    /* GX clip space keeps -w <= z <= 0: z = 0 is the far plane, and the GPU
       drops what lies beyond it (distant scenery the game expects hidden). */
    out->far_d = g_setup.persp ? -cz : 1.0f;
    project(out);

    /* View-space position and normal for lighting. The normal matrix rows
       for position matrix row r start at XF 0x400 + 3r (three floats each). */
    float vpos[3] = {ex, ey, ez}, vn[3] = {0.0f, 0.0f, 1.0f};
    if (g_setup.lit) {
        const u32* nm = &XFM[0x400u + (in->pos_mtx & 31u) * 3u];
        for (int r = 0; r < 3; r++)
            vn[r] = gx_f32(nm[r * 3]) * in->nrm[0] + gx_f32(nm[r * 3 + 1]) * in->nrm[1] + gx_f32(nm[r * 3 + 2]) * in->nrm[2];
        float len = sqrtf(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
        if (len > 1e-9f) { vn[0] /= len; vn[1] /= len; vn[2] /= len; }
        else { vn[0] = 0.0f; vn[1] = 0.0f; vn[2] = 1.0f; }
    }
    channel(in, 0, vpos, vn, out->col[0]);
    if (g_setup.chan1)                         /* second colour channel in use */
        channel(in, 1, vpos, vn, out->col[1]);
    else
        out->col[1][0] = out->col[1][1] = out->col[1][2] = out->col[1][3] = 0.0f;
    u32 ntex = g_setup.ntex;
    for (u32 i = 0; i < MAX_TC; i++) {
        if (i < ntex)
            texgen(in, i, out->tc[i]);
        else
            out->tc[i][0] = out->tc[i][1] = 0.0f, out->tc[i][2] = 1.0f;
    }
}

/* ------------------------------------------------------- draw state --- */
/* Rendering is split in two. The game thread decodes and transforms vertices
   and records each triangle together with a snapshot of the GPU state it
   needs (RenderState). At the end of a frame the recorded command list is drawn
   by worker threads, each owning a horizontal band of the frame buffer,
   while the game thread already records the next frame.

   TEV inputs are read from a per-thread table of rows (RGBA floats):
     0-3  PREV, REG0, REG1, REG2   (reset from the start values per pixel)
     4    texture colour of the current stage (after swizzle)
     5    rasterised colour of the current stage (after swizzle)
     6-8  constants 1, 0.5, 0
     9+   konstant colour / alpha for each stage (two rows per stage) */




static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static void op_params(u32 op, bool* sub, bool* clamp, bool* cmp, float* bias, float* scale) {
    u32 b = (op >> 16) & 3u, sc = (op >> 20) & 3u;
    *cmp = b == 3;                     /* comparison modes: not modelled, pass d through */
    *sub = (op >> 18) & 1u;
    *clamp = (op >> 19) & 1u;
    *bias = b == 1 ? 0.5f : b == 2 ? -0.5f : 0.0f;
    *scale = sc == 1 ? 2.0f : sc == 2 ? 4.0f : sc == 3 ? 0.5f : 1.0f;
}

static void konst_rows(RenderState* ds, u32 st, u32 kcsel, u32 kasel) {
    static const float frac[8] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
    for (int pass = 0; pass < 2; pass++) {
        u32 sel = pass ? kasel : kcsel;
        float* out = ds->konst[st * 2 + (u32)pass];
        if (sel < 8) {
            out[0] = out[1] = out[2] = out[3] = frac[sel];
            continue;
        }
        const u8* k = gx_tev_konst[sel & 3u];
        float rgba[4] = {g_u8f[k[0]], g_u8f[k[1]], g_u8f[k[2]], g_u8f[k[3]]};
        if (sel >= 0x0C && sel <= 0x0F) {           /* whole colour (alpha uses its A) */
            if (pass) { out[0] = out[1] = out[2] = out[3] = rgba[3]; }
            else memcpy(out, rgba, sizeof(rgba));
            continue;
        }
        u32 comp = sel >= 0x1C ? 3 : sel >= 0x18 ? 2 : sel >= 0x14 ? 1 : 0;
        out[0] = out[1] = out[2] = out[3] = rgba[comp];
    }
}

static void scissor(int* x0, int* y0, int* x1, int* y1) {
    u32 tl = gx_bp[0x20], br = gx_bp[0x21];
    int top = (int)(tl & 0x7FF) - 342, left = (int)((tl >> 12) & 0x7FF) - 342;
    int bottom = (int)(br & 0x7FF) - 342, right = (int)((br >> 12) & 0x7FF) - 342;
    if (right < left || bottom < top) {
        left = 0; top = 0; right = EFB_W - 1; bottom = EFB_H - 1;
    }
    *x0 = left < 0 ? 0 : left;
    *y0 = top < 0 ? 0 : top;
    *x1 = right >= EFB_W ? EFB_W - 1 : right;
    *y1 = bottom >= EFB_H ? EFB_H - 1 : bottom;
}

/* Snapshot of everything a draw needs from the current register state. */
/* A snapshot only fills (and is compared over) what the draw uses: the
   fixed fields, the stages in use and their konstant rows. The parts of a
   RenderState around the stage and konst arrays: */
#define RS_HEAD_END offsetof(RenderState, st)
#define RS_MID_BEGIN offsetof(RenderState, reg_init)
#define RS_MID_END offsetof(RenderState, konst)
#define RS_TAIL_BEGIN (offsetof(RenderState, konst) + sizeof(((RenderState*)0)->konst))

static void snapshot_state(RenderState* ds) {
    memset(ds, 0, RS_HEAD_END);
    memset((u8*)ds + RS_TAIL_BEGIN, 0, sizeof(RenderState) - RS_TAIL_BEGIN);
    for (u32 r = 0; r < 4; r++)
        for (u32 k = 0; k < 4; k++)
            ds->reg_init[r][k] = (float)gx_tev_reg[r][k] / 255.0f;

    u8 swap[4][4];
    for (u32 t = 0; t < 4; t++) {
        u32 k0 = gx_bp[0xF6 + t * 2], k1 = gx_bp[0xF7 + t * 2];
        swap[t][0] = k0 & 3u; swap[t][1] = (k0 >> 2) & 3u;
        swap[t][2] = k1 & 3u; swap[t][3] = (k1 >> 2) & 3u;
    }

    static const u8 c_row[16] = {0, 0, 1, 1, 2, 2, 3, 3, ROW_TEX, ROW_TEX, ROW_RAS, ROW_RAS, ROW_ONE, ROW_HALF, 0, ROW_ZERO};
    static const u8 a_row[8] = {0, 1, 2, 3, ROW_TEX, ROW_RAS, 0, ROW_ZERO};

    ds->stages = ((gx_bp[0x00] >> 10) & 15u) + 1;
    for (u32 i = 0; i < ds->stages; i++) {
        Stage* s = &ds->st[i];
        memset(s, 0, sizeof(*s));
        u32 cop = gx_bp[0xC0 + i * 2], aop = gx_bp[0xC1 + i * 2];
        u32 order = gx_bp[0x28 + i / 2] >> ((i & 1) ? 12 : 0);
        u32 texmap = order & 7u;
        s->texcoord = (u8)((order >> 3) & 7u);
        s->tex_on = (order >> 6) & 1u;
        s->chan = (u8)((order >> 7) & 7u);
        u32 ksel = gx_bp[0xF6 + i / 2];
        konst_rows(ds, i, (ksel >> ((i & 1) ? 14 : 4)) & 31u, (ksel >> ((i & 1) ? 19 : 9)) & 31u);

        const u32 csel[4] = {(cop >> 12) & 15u, (cop >> 8) & 15u, (cop >> 4) & 15u, cop & 15u};
        for (int j = 0; j < 4; j++) {
            s->ci[j] = (u8)(csel[j] == 14 ? ROW_KONST + i * 2 : c_row[csel[j]]);
            s->cbc[j] = csel[j] < 12 && (csel[j] & 1u);
        }
        const u32 asel[4] = {(aop >> 13) & 7u, (aop >> 10) & 7u, (aop >> 7) & 7u, (aop >> 4) & 7u};
        for (int j = 0; j < 4; j++)
            s->ai[j] = (u8)(asel[j] == 6 ? ROW_KONST + i * 2 + 1 : a_row[asel[j]]);
        s->cdst = (u8)((cop >> 22) & 3u);
        s->adst = (u8)((aop >> 22) & 3u);
        op_params(cop, &s->csub, &s->cclamp, &s->ccmp, &s->cbias, &s->cscale);
        op_params(aop, &s->asub, &s->aclamp, &s->acmp, &s->abias, &s->ascale);
        memcpy(s->tsw, swap[(aop >> 2) & 3u], 4);
        memcpy(s->rsw, swap[aop & 3u], 4);

        if (s->tex_on && g_gpu)
            s->efb_copy = gpu_tex_copy_for(texmap);
        if (s->tex_on)
            s->from_copy = copy_dest_is(texmap);
        if (s->tex_on)
            s->tex_addr = (gx_bp[(texmap < 4 ? 0x94u : 0xB4u) + (texmap & 3u)] << 5) & 0x01FFFFE0u;
        if (s->tex_on) {
            const GxTexture* tex = gx_tex_get(g_cpu, texmap);
            if (tex) {
                u32 mode0 = gx_bp[(texmap < 4 ? 0x80u : 0xA0u) + (texmap & 3u)];
                u32 mode1 = gx_bp[(texmap < 4 ? 0x84u : 0xA4u) + (texmap & 3u)];
                s->texels = tex->texels;
                s->hires = g_gpu && hires_enabled() ? tex->hires : NULL;
                s->tw = tex->width;
                s->th = tex->height;
                s->wrap_s = mode0 & 3u;
                s->wrap_t = (mode0 >> 2) & 3u;
                s->levels = tex->levels;
                memcpy(s->lw, tex->lw, sizeof(s->lw));
                memcpy(s->lh, tex->lh, sizeof(s->lh));
                memcpy(s->loff, tex->off, sizeof(s->loff));
                s->mag_linear = (mode0 >> 4) & 1u;
                s->min_filter = (u8)gx_tex_min_filter(mode0);
                s->lod_bias = (float)(s8)((mode0 >> 9) & 0xFFu) / 32.0f;
                s->min_lod = (float)(mode1 & 0xFFu) / 16.0f;
                s->max_lod = (float)((mode1 >> 8) & 0xFFu) / 16.0f;
            }
        }
    }

    /* Recognise the standard one-stage modes: output to PREV, no bias/scale,
       clamped, identity swizzles, colour channel 0 or 1. */
    ds->fast = FAST_NONE;
    if (ds->stages == 1) {
        u32 cop = gx_bp[0xC0] & 0xFFFFFFu, aop = gx_bp[0xC1] & 0xFFFFFFu;
        const Stage* s = &ds->st[0];
        bool ident = memcmp(s->tsw, "\0\1\2\3", 4) == 0 && memcmp(s->rsw, "\0\1\2\3", 4) == 0;
        bool plain = ((cop >> 16) & 0xFFu) == 0x08u && ((aop >> 16) & 0xFFu) == 0x08u;  /* clamp only, PREV */
        bool aop_ok = (aop & 0xFu) == 0;    /* swap tables 0 */
        if (ident && plain && aop_ok) {
            u32 c = cop & 0xFFFFu, a = (aop >> 4) & 0xFFFu;
            if (c == 0xF8AFu && a == ((7u << 9) | (4u << 6) | (5u << 3) | 7u) && s->tex_on && s->chan <= 1)
                ds->fast = FAST_MODULATE;
            else if (c == 0xFFF8u && a == ((7u << 9) | (7u << 6) | (7u << 3) | 4u) && s->tex_on)
                ds->fast = FAST_REPLACE;
            else if (c == 0xFFFAu && a == ((7u << 9) | (7u << 6) | (7u << 3) | 5u) && s->chan <= 1)
                ds->fast = FAST_PASSCLR;
        }
    }

    u32 at = gx_bp[0xF3];
    ds->at_ref0 = at & 255u; ds->at_ref1 = (at >> 8) & 255u;
    ds->at_f0 = (at >> 16) & 7u; ds->at_f1 = (at >> 19) & 7u; ds->at_logic = (at >> 22) & 3u;
    /* "always pass": unconfigured, or both tests always with AND/OR */
    ds->at_always = at == 0 || (ds->at_f0 == 7 && ds->at_f1 == 7 && ds->at_logic <= 1);

    scissor(&ds->sx0, &ds->sy0, &ds->sx1, &ds->sy1);
    u32 zmode = gx_bp[0x40], cmode = gx_bp[0x41];
    ds->ztest = zmode & 1u;
    ds->zupdate = (zmode >> 4) & 1u;
    ds->zfunc = (zmode >> 1) & 7u;
    ds->blend = cmode & 1u;
    ds->color_update = (cmode >> 3) & 1u;
    ds->alpha_update = (cmode >> 4) & 1u;
    ds->dfac = (cmode >> 5) & 7u;
    ds->sfac = (cmode >> 8) & 7u;
    ds->subtract = (cmode >> 11) & 1u;
    if (!(cmode & 0x18u))
        ds->color_update = true;
    ds->blend_alpha = ds->blend && !ds->subtract && ds->sfac == 4 && ds->dfac == 5;  /* src*a + dst*(1-a) */
    ds->ntex = gx_xf_reg[0x3F] & 15u;
    if (ds->ntex > MAX_TC) ds->ntex = MAX_TC;
    ds->cull = (gx_bp[0x00] >> 14) & 3u;
}

/* ---------------------------------------------------------------- TEV --- */
static inline int wrap_int(int i, int n, u32 mode) {
    switch (mode) {
    case 0:                            /* clamp */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    case 2: {                          /* mirror */
        int period = 2 * n;
        int m = i % period;
        if (m < 0) m += period;
        return m < n ? m : period - 1 - m;
    }
    default:                           /* repeat */
        if ((n & (n - 1)) == 0)
            return i & (n - 1);
        i %= n;
        return i < 0 ? i + n : i;
    }
}

static inline int ifloor(float f) {
    int i = (int)f;
    return f < (float)i ? i - 1 : i;   /* floor without a libm call */
}

static inline void unpack_texel(u32 c, float out[4]) {
    out[0] = g_u8f[(c >> 16) & 255]; out[1] = g_u8f[(c >> 8) & 255];
    out[2] = g_u8f[c & 255]; out[3] = g_u8f[c >> 24];
}

/* One mip level, nearest or bilinear. */
static inline void fetch_level(const Stage* s, u32 l, float u, float v, bool linear, float out[4]) {
    const u32* tx = s->texels + s->loff[l];
    int w = (int)s->lw[l], h = (int)s->lh[l];
    if (!linear) {
        int x = wrap_int(ifloor(u * (float)w), w, s->wrap_s);
        int y = wrap_int(ifloor(v * (float)h), h, s->wrap_t);
        unpack_texel(tx[y * w + x], out);
        return;
    }
    float fu = u * (float)w - 0.5f, fv = v * (float)h - 0.5f;
    int x0 = ifloor(fu), y0 = ifloor(fv);
    float ax = fu - (float)x0, ay = fv - (float)y0;
    int xa = wrap_int(x0, w, s->wrap_s), xb = wrap_int(x0 + 1, w, s->wrap_s);
    int ya = wrap_int(y0, h, s->wrap_t), yb = wrap_int(y0 + 1, h, s->wrap_t);
    u32 c00 = tx[ya * w + xa], c10 = tx[ya * w + xb], c01 = tx[yb * w + xa], c11 = tx[yb * w + xb];
    if (c00 == c10 && c00 == c01 && c00 == c11) {   /* flat area: no blending needed */
        unpack_texel(c00, out);
        return;
    }
    float w00 = (1 - ax) * (1 - ay), w10 = ax * (1 - ay), w01 = (1 - ax) * ay, w11 = ax * ay;
    for (int sh = 0, k = 2; k >= 0; k--, sh += 8) {
        float v00 = (float)((c00 >> sh) & 255), v10 = (float)((c10 >> sh) & 255);
        float v01 = (float)((c01 >> sh) & 255), v11 = (float)((c11 >> sh) & 255);
        out[k] = (v00 * w00 + v10 * w10 + v01 * w01 + v11 * w11) * (1.0f / 255.0f);
    }
    out[3] = ((float)(c00 >> 24) * w00 + (float)(c10 >> 24) * w10 + (float)(c01 >> 24) * w01 +
              (float)(c11 >> 24) * w11) * (1.0f / 255.0f);
}

static bool alpha_compare(u32 func, u32 v, u32 ref) {
    switch (func) {
    case 0: return false;
    case 1: return v < ref;
    case 2: return v == ref;
    case 3: return v <= ref;
    case 4: return v > ref;
    case 5: return v != ref;
    case 6: return v >= ref;
    default: return true;
    }
}

static bool alpha_test(const RenderState* ds, float a01) {
    u32 a = (u32)(a01 * 255.0f + 0.5f);
    bool r0 = alpha_compare(ds->at_f0, a, ds->at_ref0), r1 = alpha_compare(ds->at_f1, a, ds->at_ref1);
    switch (ds->at_logic) {
    case 0: return r0 && r1;
    case 1: return r0 || r1;
    case 2: return r0 != r1;
    default: return r0 == r1;
    }
}

static inline float tev_combine(float a, float b, float c, float d, bool sub, float bias, float scale, bool clamp) {
    a = clamp01(a); b = clamp01(b); c = clamp01(c);
    float lerp = a + (b - a) * c;
    float r = ((sub ? d - lerp : d + lerp) + bias) * scale;
    if (clamp)
        return clamp01(r);
    return r < -4.0f ? -4.0f : r > 4.0f ? 4.0f : r;
}

/* Samples a stage's texture at its coordinates. lod: level of detail for
   this triangle (log2 of texels per pixel, bias included). Filters follow
   the game's settings: magnification nearest/linear; minification nearest,
   linear, or mipmapped with nearest/linear inside a level and between levels. */
static inline void sample_stage(const Stage* s, const float tc[][2], float lod, float out[4]) {
    if (!s->texels) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    float u = tc[s->texcoord][0], v = tc[s->texcoord][1];
    if (lod <= 0.0f || s->levels <= 1 || s->min_filter < 2) {
        bool linear = lod <= 0.0f ? s->mag_linear : (s->min_filter == 1 || s->min_filter == 3 || s->min_filter == 5);
        fetch_level(s, 0, u, v, linear, out);
        return;
    }
    bool linear = s->min_filter == 3 || s->min_filter == 5;      /* LIN_MIP_* */
    bool mip_linear = s->min_filter == 4 || s->min_filter == 5;  /* *_MIP_LIN */
    float maxl = (float)(s->levels - 1);
    if (lod > maxl) lod = maxl;
    if (!mip_linear) {
        fetch_level(s, (u32)(lod + 0.5f), u, v, linear, out);
        return;
    }
    u32 l0 = (u32)lod;
    float f = lod - (float)l0;
    fetch_level(s, l0, u, v, linear, out);
    if (f > 0.0f && l0 + 1 < s->levels) {
        float o2[4];
        fetch_level(s, l0 + 1, u, v, linear, o2);
        for (int k = 0; k < 4; k++) out[k] += (o2[k] - out[k]) * f;
    }
}

/* Runs the TEV stages for one pixel. Returns false if the alpha test
   discards it. rows: this thread's input table, konst rows already filled. */
static bool tev_pixel(const RenderState* ds, float (*rows)[4], const float col[2][4], const float tc[][2],
                      const float* lod, float out[4]) {
    if (ds->fast != FAST_NONE) {
        const Stage* s = &ds->st[0];
        switch (ds->fast) {
        case FAST_MODULATE: {
            float t[4];
            sample_stage(s, tc, lod[0], t);
            const float* c = col[s->chan];
            for (int k = 0; k < 4; k++) out[k] = t[k] * clamp01(c[k]);
            break;
        }
        case FAST_REPLACE:
            sample_stage(s, tc, lod[0], out);
            break;
        default: {
            const float* c = col[s->chan];
            for (int k = 0; k < 4; k++) out[k] = clamp01(c[k]);
            break;
        }
        }
        return ds->at_always || alpha_test(ds, out[3]);
    }
    memcpy(rows, ds->reg_init, sizeof(ds->reg_init));
    for (u32 n = 0; n < ds->stages; n++) {
        const Stage* s = &ds->st[n];
        if (s->tex_on) {
            float raw[4];
            float* tex = rows[ROW_TEX];
            sample_stage(s, tc, lod[n], raw);
            tex[0] = raw[s->tsw[0]]; tex[1] = raw[s->tsw[1]]; tex[2] = raw[s->tsw[2]]; tex[3] = raw[s->tsw[3]];
        }
        float* ras = rows[ROW_RAS];
        if (s->chan <= 1) {
            const float* c = col[s->chan];
            ras[0] = c[s->rsw[0]]; ras[1] = c[s->rsw[1]]; ras[2] = c[s->rsw[2]]; ras[3] = c[s->rsw[3]];
        } else {
            ras[0] = ras[1] = ras[2] = ras[3] = 0.0f;
        }

        float res[3], ares;
        const float* ci[4] = {rows[s->ci[0]], rows[s->ci[1]], rows[s->ci[2]], rows[s->ci[3]]};
        if (s->ccmp) {
            for (int k = 0; k < 3; k++) res[k] = s->cbc[3] ? ci[3][3] : ci[3][k];
        } else {
            for (int k = 0; k < 3; k++) {
                float in[4];
                for (int j = 0; j < 4; j++) in[j] = s->cbc[j] ? ci[j][3] : ci[j][k];
                res[k] = tev_combine(in[0], in[1], in[2], in[3], s->csub, s->cbias, s->cscale, s->cclamp);
            }
        }
        ares = s->acmp ? rows[s->ai[3]][3]
                       : tev_combine(rows[s->ai[0]][3], rows[s->ai[1]][3], rows[s->ai[2]][3], rows[s->ai[3]][3],
                                     s->asub, s->abias, s->ascale, s->aclamp);
        float* cd = rows[s->cdst];
        cd[0] = res[0]; cd[1] = res[1]; cd[2] = res[2];
        rows[s->adst][3] = ares;
    }

    /* Output is the PREV register after the last stage. */
    for (int k = 0; k < 4; k++) out[k] = clamp01(rows[ROW_PREV][k]);
    return ds->at_always || alpha_test(ds, out[3]);
}

/* --------------------------------------------------------- rasteriser --- */
static bool depth_pass(u32 func, u32 z, u32 stored) {
    switch (func) {
    case 0: return false;
    case 1: return z < stored;
    case 2: return z == stored;
    case 3: return z <= stored;
    case 4: return z > stored;
    case 5: return z != stored;
    case 6: return z >= stored;
    default: return true;
    }
}

static float blend_factor(u32 f, bool is_src, float sa, float da, float sc, float dc) {
    switch (f) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return is_src ? dc : sc;
    case 3: return 1.0f - (is_src ? dc : sc);
    case 4: return sa;
    case 5: return 1.0f - sa;
    case 6: return da;
    default: return 1.0f - da;
    }
}

static inline u32 to_u8(float v) {      /* v already in 0..255 */
    return (u32)(v + 0.5f);
}

/* Draws the rows [band_y0, band_y1] of one triangle. */
static void draw_triangle(const RenderState* ds, float (*rows)[4], const SVtx* a, const SVtx* b, const SVtx* c,
                          int band_y0, int band_y1) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-6f || !isfinite(area)) return;
    /* Face culling. With y pointing down the screen, a positive area means the
       vertices run clockwise; GX treats counter-clockwise as front-facing
       (confirmed by the Select Teams logos, which are drawn in both windings). */
    if ((ds->cull == 1 && area < 0.0f) || (ds->cull == 2 && area > 0.0f))
        return;

    int sy0 = ds->sy0 > band_y0 ? ds->sy0 : band_y0;
    int sy1 = ds->sy1 < band_y1 ? ds->sy1 : band_y1;
    float fx0 = fminf(a->x, fminf(b->x, c->x)), fx1 = fmaxf(a->x, fmaxf(b->x, c->x));
    float fy0 = fminf(a->y, fminf(b->y, c->y)), fy1 = fmaxf(a->y, fmaxf(b->y, c->y));
    if (fx1 < ds->sx0 || fy1 < sy0 || fx0 > ds->sx1 + 1 || fy0 > sy1 + 1) return;
    int x0 = fx0 < ds->sx0 ? ds->sx0 : (int)fx0, x1 = fx1 > ds->sx1 ? ds->sx1 : (int)fx1;
    int y0 = fy0 < sy0 ? sy0 : (int)fy0, y1 = fy1 > sy1 ? sy1 : (int)fy1;
    if (y0 > y1 || x0 > x1) return;

    u32 ntex = ds->ntex;
    float inv = 1.0f / area;

    /* Per-triangle shortcuts: flat 2D (no perspective), constant colours,
       texture coordinates that don't need the projective divide. */
    bool affine = a->inv_w == b->inv_w && b->inv_w == c->inv_w;
    bool flat_col = memcmp(a->col, b->col, sizeof(a->col)) == 0 && memcmp(b->col, c->col, sizeof(a->col)) == 0;
    bool no_q = true;
    for (u32 t = 0; t < ntex; t++)
        if (a->tc[t][2] != 1.0f || b->tc[t][2] != 1.0f || c->tc[t][2] != 1.0f) no_q = false;

    /* Level of detail per stage, from how fast its texture coordinates change
       across the screen (texels per pixel), worked out once per triangle. */
    float lod[16];
    for (u32 n = 0; n < ds->stages; n++) {
        const Stage* s = &ds->st[n];
        lod[n] = 0.0f;
        if (!s->tex_on || !s->texels)
            continue;
        u32 t = s->texcoord < MAX_TC ? s->texcoord : 0;
        float qa = a->tc[t][2], qb = b->tc[t][2], qc = c->tc[t][2];
        float sa = a->tc[t][0] / (fabsf(qa) > 1e-9f ? qa : 1.0f), ta = a->tc[t][1] / (fabsf(qa) > 1e-9f ? qa : 1.0f);
        float sb = b->tc[t][0] / (fabsf(qb) > 1e-9f ? qb : 1.0f), tb = b->tc[t][1] / (fabsf(qb) > 1e-9f ? qb : 1.0f);
        float sc = c->tc[t][0] / (fabsf(qc) > 1e-9f ? qc : 1.0f), tc_ = c->tc[t][1] / (fabsf(qc) > 1e-9f ? qc : 1.0f);
        float dsdx = ((sb - sa) * (c->y - a->y) - (sc - sa) * (b->y - a->y)) * inv * (float)s->tw;
        float dsdy = ((sc - sa) * (b->x - a->x) - (sb - sa) * (c->x - a->x)) * inv * (float)s->tw;
        float dtdx = ((tb - ta) * (c->y - a->y) - (tc_ - ta) * (b->y - a->y)) * inv * (float)s->th;
        float dtdy = ((tc_ - ta) * (b->x - a->x) - (tb - ta) * (c->x - a->x)) * inv * (float)s->th;
        float rx = dsdx * dsdx + dtdx * dtdx, ry = dsdy * dsdy + dtdy * dtdy;
        float rho2 = rx > ry ? rx : ry;
        float l = rho2 > 1e-12f ? 0.5f * log2f(rho2) : -16.0f;
        l += s->lod_bias;
        if (l > 0.0f) {                 /* the LOD clamp only applies when minifying */
            if (l < s->min_lod) l = s->min_lod;
            if (l > s->max_lod) l = s->max_lod;
        }
        lod[n] = isfinite(l) ? l : 0.0f;
    }

    /* Edge functions step linearly across a row. */
    float dw0dx = (b->y - c->y) * inv, dw1dx = (c->y - a->y) * inv;

    float col[2][4];
    float tc[MAX_TC][2];
    memset(tc, 0, sizeof(tc));
    if (flat_col)
        memcpy(col, a->col, sizeof(col));

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        float px0 = (float)x0 + 0.5f;
        float w0 = ((b->x - px0) * (c->y - py) - (b->y - py) * (c->x - px0)) * inv;
        float w1 = ((c->x - px0) * (a->y - py) - (c->y - py) * (a->x - px0)) * inv;
        u32* crow = &g_color[(size_t)y * EFB_W];
        u32* zrow = &g_depth[(size_t)y * EFB_W];
        for (int x = x0; x <= x1; x++, w0 += dw0dx, w1 += dw1dx) {
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;

            u32 z = 0;
            if (ds->ztest) {
                float zf = w0 * a->z + w1 * b->z + w2 * c->z;
                z = zf <= 0 ? 0u : zf >= 16777215.0f ? 16777215u : (u32)zf;
                if (!depth_pass(ds->zfunc, z, zrow[x]))
                    continue;
            }

            float p0 = w0, p1 = w1, p2 = w2;
            if (!affine) {             /* perspective-correct weights */
                p0 = w0 * a->inv_w; p1 = w1 * b->inv_w; p2 = w2 * c->inv_w;
                float ps = p0 + p1 + p2;
                if (ps <= 0) continue;
                float r = 1.0f / ps;
                p0 *= r; p1 *= r; p2 *= r;
            }
            if (!flat_col)
                for (u32 ch = 0; ch < 2; ch++)
                    for (u32 k = 0; k < 4; k++)
                        col[ch][k] = p0 * a->col[ch][k] + p1 * b->col[ch][k] + p2 * c->col[ch][k];
            for (u32 t = 0; t < ntex; t++) {
                float s = p0 * a->tc[t][0] + p1 * b->tc[t][0] + p2 * c->tc[t][0];
                float tt = p0 * a->tc[t][1] + p1 * b->tc[t][1] + p2 * c->tc[t][1];
                if (!no_q) {
                    float q = p0 * a->tc[t][2] + p1 * b->tc[t][2] + p2 * c->tc[t][2];
                    if (fabsf(q) > 1e-9f) { float r = 1.0f / q; s *= r; tt *= r; }
                }
                tc[t][0] = s; tc[t][1] = tt;
            }

            float out[4];
            if (!tev_pixel(ds, rows, col, tc, lod, out))
                continue;
            float r = out[0] * 255.0f, g = out[1] * 255.0f, bl = out[2] * 255.0f, al = out[3] * 255.0f;

            u32 dst = crow[x];
            if (ds->blend_alpha) {
                float sa = out[3], ia = 1.0f - sa;
                r = r * sa + (float)((dst >> 16) & 255) * ia;
                g = g * sa + (float)((dst >> 8) & 255) * ia;
                bl = bl * sa + (float)(dst & 255) * ia;
            } else if (ds->blend) {
                float sa = out[3], da = g_u8f[dst >> 24];
                float d[3] = {(float)((dst >> 16) & 255), (float)((dst >> 8) & 255), (float)(dst & 255)};
                float sv[3] = {r, g, bl};
                for (int k = 0; k < 3; k++) {
                    float sf = blend_factor(ds->sfac, true, sa, da, sv[k] / 255.0f, d[k] / 255.0f);
                    float df = blend_factor(ds->dfac, false, sa, da, sv[k] / 255.0f, d[k] / 255.0f);
                    float o = ds->subtract ? d[k] - sv[k] : sv[k] * sf + d[k] * df;
                    sv[k] = o < 0 ? 0 : o > 255 ? 255 : o;
                }
                r = sv[0]; g = sv[1]; bl = sv[2];
            }
            u32 o = dst;
            if (ds->color_update)
                o = (o & 0xFF000000u) | (to_u8(r) << 16) | (to_u8(g) << 8) | to_u8(bl);
            if (ds->alpha_update)
                o = (o & 0x00FFFFFFu) | (to_u8(al) << 24);
            crow[x] = o;
            if (ds->ztest && ds->zupdate)
                zrow[x] = z;
        }
    }
}

/* ----------------------------------------------------- command lists --- */

/* Room for count more vertices in the pool; returns the first index, or
   ~0 if memory ran out. The pool may move: hold indices, not pointers. */
static u32 list_verts(CmdList* l, u32 count) {
    if (l->nv + count > l->cap_v) {
        u32 cap = l->cap_v ? l->cap_v : 16384;
        while (cap < l->nv + count) cap *= 2;
        SVtx* v = (SVtx*)realloc(l->verts, sizeof(SVtx) * cap);
        if (!v)
            return ~0u;
        l->verts = v;
        l->cap_v = cap;
    }
    u32 base = l->nv;
    l->nv += count;
    return base;
}

static CmdList g_lists[2];
static u32 g_rec;                      /* list being recorded */

static bool g_peek_stale = true;       /* commands recorded since the last frame-buffer peek snapshot */
static unsigned long long g_peeks, g_peek_snapshots, g_peek_probes;
/* A probe: the game reading the same single pixel once a frame (the GPU
   renderer then reads that pixel back on its own thread at the end of each
   frame, and the game gets the last frame's value, instead of stopping to
   wait for the whole frame to be drawn and read back). */
#define PROBE_MAX 4
static u32 g_probe_ea[PROBE_MAX], g_nprobe;            /* the pixels read frame after frame */
static u32 g_frame_ea[PROBE_MAX], g_nframe_ea;         /* the ones read this frame */
static u32 g_probe_streak, g_peeks_frame;
static bool g_frame_ea_over;                           /* more than PROBE_MAX different ones this frame */
static volatile u32 g_probe_value[PROBE_MAX];
static volatile bool g_probe_on;        /* the GPU thread reads the probe pixels after each list */
#define PROBE_STREAK 3u

static Cmd* list_push(CmdList* l) {
    g_peek_stale = true;
    if (l->n == l->cap) {
        u32 cap = l->cap ? l->cap * 2 : 4096;
        Cmd* c = (Cmd*)realloc(l->cmds, sizeof(Cmd) * cap);
        if (!c)
            return NULL;
        l->cmds = c;
        l->cap = cap;
    }
    return &l->cmds[l->n++];
}

/* Hashing, comparing and copying the used part of a snapshot. */
static u64 hash_words(u64 h, const void* p, size_t n) {
    const u8* b = (const u8*)p;
    for (; n >= 8; n -= 8, b += 8) {
        u64 w;
        memcpy(&w, b, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h;
}

static u64 rs_hash(const RenderState* r) {
    u64 h = 0xCBF29CE484222325ull;
    h = hash_words(h, r, RS_HEAD_END);
    h = hash_words(h, r->st, sizeof(Stage) * r->stages);
    h = hash_words(h, (const u8*)r + RS_MID_BEGIN, RS_MID_END - RS_MID_BEGIN);
    h = hash_words(h, r->konst, sizeof(r->konst[0]) * 2u * r->stages);
    return hash_words(h, (const u8*)r + RS_TAIL_BEGIN, sizeof(RenderState) - RS_TAIL_BEGIN);
}

static bool rs_equal(const RenderState* a, const RenderState* b) {
    return a->stages == b->stages && memcmp(a, b, RS_HEAD_END) == 0 &&
           memcmp(a->st, b->st, sizeof(Stage) * a->stages) == 0 &&
           memcmp((const u8*)a + RS_MID_BEGIN, (const u8*)b + RS_MID_BEGIN, RS_MID_END - RS_MID_BEGIN) == 0 &&
           memcmp(a->konst, b->konst, sizeof(a->konst[0]) * 2u * a->stages) == 0 &&
           memcmp((const u8*)a + RS_TAIL_BEGIN, (const u8*)b + RS_TAIL_BEGIN, sizeof(RenderState) - RS_TAIL_BEGIN) == 0;
}

static void rs_copy(RenderState* d, const RenderState* s) {
    memcpy(d, s, RS_HEAD_END);
    memcpy(d->st, s->st, sizeof(Stage) * s->stages);
    memcpy((u8*)d + RS_MID_BEGIN, (const u8*)s + RS_MID_BEGIN, RS_MID_END - RS_MID_BEGIN);
    memcpy(d->konst, s->konst, sizeof(s->konst[0]) * 2u * s->stages);
    memcpy((u8*)d + RS_TAIL_BEGIN, (const u8*)s + RS_TAIL_BEGIN, sizeof(RenderState) - RS_TAIL_BEGIN);
}

/* The snapshots already in the list being recorded, by hash, so a draw that
   goes back to an earlier setup (models alternating between a few textures)
   reuses its snapshot instead of adding another. */
#define STATE_TABLE 4096u
static struct { u64 hash; u32 idx, gen; } g_stab[STATE_TABLE];
static u32 g_stab_gen = 1;

static u32 list_state(CmdList* l) {
    if (l->ns == l->cap_s) {
        u32 cap = l->cap_s ? l->cap_s * 2 : 256;
        RenderState* s = (RenderState*)realloc(l->states, sizeof(RenderState) * cap);
        if (!s)
            return 0;
        l->states = s;
        l->cap_s = cap;
    }
    /* Nothing that the snapshot reads has changed since the last draw. */
    static u32 last_serial;
    static const CmdList* last_list;
    static u32 last_idx;
    if (gx_state_serial == last_serial && l == last_list && last_idx < l->ns)
        return last_idx;
    last_serial = gx_state_serial;
    last_list = l;
    if (l->ns == 0)
        g_stab_gen++;                  /* a fresh list: forget the table */
    static RenderState tmp;
    snapshot_state(&tmp);
    u64 h = rs_hash(&tmp);
    u32 slot = (u32)(h ^ (h >> 32)) & (STATE_TABLE - 1u);
    for (u32 probe = 0; probe < 8; probe++, slot = (slot + 1) & (STATE_TABLE - 1u)) {
        if (g_stab[slot].gen != g_stab_gen)
            break;                     /* empty for this list */
        if (g_stab[slot].hash == h && g_stab[slot].idx < l->ns && rs_equal(&l->states[g_stab[slot].idx], &tmp))
            return last_idx = g_stab[slot].idx;
    }
    rs_copy(&l->states[l->ns], &tmp);
    last_idx = l->ns;
    /* remember it (in the first free slot of its run, or over the oldest) */
    slot = (u32)(h ^ (h >> 32)) & (STATE_TABLE - 1u);
    for (u32 probe = 0; probe < 8; probe++) {
        u32 sl = (slot + probe) & (STATE_TABLE - 1u);
        if (g_stab[sl].gen != g_stab_gen || probe == 7) {
            g_stab[sl].hash = h; g_stab[sl].idx = l->ns; g_stab[sl].gen = g_stab_gen;
            break;
        }
    }
    return l->ns++;
}

/* --------------------------------------------------------- workers --- */
#include "gx/thread.h"

#define MAX_WORKERS 16
static int g_workers;                  /* 0 = draw on the game thread */
static int g_workers_wanted = -1;      /* -1 = automatic */
static gx_mutex g_mx;
static gx_cond g_cv_work, g_cv_done;
static u32 g_generation;               /* bumped for each submitted list */
static int g_busy;                     /* workers still drawing the current list */
static const CmdList* g_drawing;

/* EFB rows [y0, y1] of a TV-buffer copy, written to RAM as YUYV. Each
   destination line is written by whoever owns its source line, so no
   drawing thread reads rows another is still drawing. */
void raster_xfb_write(const CopyCmd* cp, int y0, int y1) {
    if (!cp->lines || !g_cpu)
        return;
    for (u32 y = 0; y < cp->lines; y++) {
        u32 srcy = cp->sy + (u32)(y / cp->yscale);
        if (srcy >= EFB_H || srcy >= cp->sy + cp->h)
            srcy = cp->sy + cp->h - 1 < EFB_H ? cp->sy + cp->h - 1 : EFB_H - 1;
        if ((int)srcy < y0 || (int)srcy > y1)
            continue;
        u8* d = g_cpu->ram + cp->dest + y * cp->stride;
        const u32* src = &g_color[srcy * EFB_W];
        for (u32 x = 0; x + 1 < cp->w; x += 2) {
            u32 c0 = cp->sx + x < EFB_W ? src[cp->sx + x] : 0;
            u32 c1 = cp->sx + x + 1 < EFB_W ? src[cp->sx + x + 1] : 0;
            int r0 = (c0 >> 16) & 255, g0 = (c0 >> 8) & 255, b0 = c0 & 255;
            int r1 = (c1 >> 16) & 255, g1 = (c1 >> 8) & 255, b1 = c1 & 255;
            int r = (r0 + r1) / 2, g = (g0 + g1) / 2, b = (b0 + b1) / 2;
            d[x * 2 + 0] = (u8)((66 * r0 + 129 * g0 + 25 * b0 + 128) / 256 + 16);
            d[x * 2 + 1] = (u8)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
            d[x * 2 + 2] = (u8)((66 * r1 + 129 * g1 + 25 * b1 + 128) / 256 + 16);
            d[x * 2 + 3] = (u8)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
        }
    }
}

/* Executes the whole list for frame-buffer rows [y0, y1]. */
static void run_list(const CmdList* l, int y0, int y1) {
    float rows[ROW_COUNT][4];
    for (int k = 0; k < 4; k++) {
        rows[ROW_ONE][k] = 1.0f;
        rows[ROW_HALF][k] = 0.5f;
        rows[ROW_ZERO][k] = 0.0f;
    }
    u32 cur = 0xFFFFFFFFu;
    for (u32 i = 0; i < l->n; i++) {
        const Cmd* c = &l->cmds[i];
        if (c->type == CMD_TRI) {
            const RenderState* ds = &l->states[c->state];
            if (c->state != cur) {
                memcpy(rows[ROW_KONST], ds->konst, sizeof(ds->konst));
                cur = c->state;
            }
            draw_triangle(ds, rows, &l->verts[c->u.vi[0]], &l->verts[c->u.vi[1]], &l->verts[c->u.vi[2]], y0, y1);
            continue;
        }
        const CopyCmd* cp = &c->u.copy;
        if (cp->to_xfb)
            raster_xfb_write(cp, y0, y1);
        if (cp->clear) {
            for (int y = y0; y <= y1; y++) {
                u32* cr = &g_color[(size_t)y * EFB_W];
                u32* zr = &g_depth[(size_t)y * EFB_W];
                for (int x = 0; x < EFB_W; x++) {
                    cr[x] = cp->clear_color;
                    zr[x] = cp->clear_z;
                }
            }
        }
    }
}

static void band(int who, int count, int* y0, int* y1) {
    *y0 = EFB_H * who / count;
    *y1 = EFB_H * (who + 1) / count - 1;
}

/* Renderer timing for the performance log: how long the game thread waited
   for the workers, and how long each frame took to draw (submit to last
   band done), in seconds since the last gx_raster_take_perf. */
static double now_s(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
static double g_submit_at, g_perf_wait, g_perf_draw, g_perf_draw_max;
static u32 g_perf_frames;
static unsigned long long g_perf_tris_at;

void gx_raster_take_perf(double* wait_s, double* draw_avg_s, double* draw_max_s, u32* tris_per_frame) {
    if (!g_workers) {
        *wait_s = *draw_avg_s = *draw_max_s = 0.0;
        *tris_per_frame = 0;
        return;
    }
    gx_mutex_lock(&g_mx);
    *wait_s = g_perf_wait;
    *draw_avg_s = g_perf_frames ? g_perf_draw / g_perf_frames : 0.0;
    *draw_max_s = g_perf_draw_max;
    *tris_per_frame = g_perf_frames ? (u32)((g_tris_recorded - g_perf_tris_at) / g_perf_frames) : 0;
    g_perf_wait = g_perf_draw = g_perf_draw_max = 0.0;
    g_perf_frames = 0;
    g_perf_tris_at = g_tris_recorded;
    gx_mutex_unlock(&g_mx);
}

/* What the threads are asked to do: draw a list, or (GPU) read the frame
   buffer back into memory. */
enum { JOB_LIST, JOB_READBACK, JOB_TEXCOPY };
static int g_job;
static bool g_job_depth;
static const TexCopy* g_job_copy;

static void worker_main(void* arg) {
    int who = (int)(size_t)arg;
    u32 seen = 0;
    for (;;) {
        gx_mutex_lock(&g_mx);
        while (g_generation == seen)
            gx_cond_wait(&g_cv_work, &g_mx);
        seen = g_generation;
        const CmdList* l = g_drawing;
        int count = g_workers;
        int job = g_job;
        bool job_depth = g_job_depth;
        gx_mutex_unlock(&g_mx);

        if (g_gpu) {                   /* the one GPU thread */
            if (job == JOB_READBACK)
                glr_readback(job_depth);
            else if (job == JOB_TEXCOPY)
                glr_tex_copy(g_job_copy);
            else {
                glr_run_list(g_defer ? expand_list(l) : l);
                if (g_probe_on)
                    for (u32 k = 0; k < g_nprobe; k++) {
                        u32 ea = g_probe_ea[k], v = 0;
                        glr_read_point((ea >> 2) & 0x3FFu, (ea >> 12) & 0x3FFu, (ea & 0x00400000u) != 0, &v);
                        g_probe_value[k] = v;
                    }
            }
        } else {
            int y0, y1;
            band(who, count, &y0, &y1);
            run_list(l, y0, y1);
        }

        gx_mutex_lock(&g_mx);
        if (--g_busy == 0) {
            double d = now_s() - g_submit_at;
            g_perf_draw += d;
            if (d > g_perf_draw_max) g_perf_draw_max = d;
            g_perf_frames++;
            gx_cond_broadcast(&g_cv_done);
        }
        gx_mutex_unlock(&g_mx);
    }
}

static void start_workers(void) {
    static bool started;
    if (started)
        return;
    started = true;
    for (int i = 0; i < 256; i++) g_u8f[i] = (float)i / 255.0f;
    int n = g_workers_wanted;
    if (g_gpu)
        n = 1;                         /* one thread feeds the graphics card */
    if (n < 0) {
        n = gx_cpu_count() - 1;        /* leave one core for the game */
        if (n > 8) n = 8;
        if (n < 1) n = 1;
    }
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    gx_mutex_init(&g_mx);
    gx_cond_init(&g_cv_work);
    gx_cond_init(&g_cv_done);
    g_workers = 0;
    for (int i = 0; i < n; i++) {
        gx_thread t;
        if (!gx_thread_create(&t, worker_main, (void*)(size_t)i))
            break;
        g_workers++;
    }
    if (g_gpu)
        printf("renderer: graphics card, fed by its own thread\n");
    else
        printf("renderer: %d drawing thread%s\n", g_workers, g_workers == 1 ? "" : "s");
}

/* Waits until the workers have finished the list they are drawing. */
static void wait_idle(void) {
    if (!g_workers)
        return;
    gx_mutex_lock(&g_mx);
    if (g_busy > 0) {
        int prev = prof_enter(PROF_RENDER_WAIT);
        double t0 = now_s();
        while (g_busy > 0)
            gx_cond_wait(&g_cv_done, &g_mx);
        g_perf_wait += now_s() - t0;
        prof_leave(prev);
    }
    gx_mutex_unlock(&g_mx);
}

/* Ends the recorded frame: hands its list to the workers (or draws it here
   when there are none) and starts recording into the other list. */
static void submit(void) {
    gx_state_serial++;                 /* the next draw starts a new list: take a fresh snapshot */
    /* Peeks this frame: one at the same pixel as before makes a probe. */
    /* Peeks this frame: a few pixels, the same ones as the frames before,
       make probes (read for the game at the end of each frame). */
    if (g_peeks_frame > 0) {
        bool same = !g_frame_ea_over && g_nframe_ea == g_nprobe && g_peeks_frame <= 2u * PROBE_MAX;
        for (u32 i = 0; same && i < g_nframe_ea; i++) {
            bool found = false;
            for (u32 j = 0; j < g_nprobe; j++) found |= g_probe_ea[j] == g_frame_ea[i];
            same = found;
        }
        if (same) g_probe_streak++;
        else if (!g_frame_ea_over && g_peeks_frame <= 2u * PROBE_MAX) {
            /* a new set (only while no probe reads are in use) */
            if (!g_probe_on) {
                for (u32 i = 0; i < g_nframe_ea; i++) g_probe_ea[i] = g_frame_ea[i];
                g_nprobe = g_nframe_ea;
            }
            g_probe_streak = 1;
        } else g_probe_streak = 0;
    }
    g_peeks_frame = 0;
    g_nframe_ea = 0;
    g_frame_ea_over = false;
    g_probe_on = g_gpu && g_workers && g_probe_streak >= PROBE_STREAK;
    start_workers();
    wait_idle();                       /* the other list is free again */
    gx_tex_collect_garbage();          /* texture memory the finished frame used */
    CmdList* l = &g_lists[g_rec];
    if (g_gpu && !g_workers) {         /* no thread could be started: draw here */
        glr_run_list(g_defer ? expand_list(l) : l);
    } else if (!g_workers) {
        run_list(l, 0, EFB_H - 1);
    } else {
        gx_mutex_lock(&g_mx);
        g_job = JOB_LIST;
        g_drawing = l;
        g_busy = g_workers;
        g_submit_at = now_s();
        g_generation++;
        gx_cond_broadcast(&g_cv_work);
        gx_mutex_unlock(&g_mx);
    }
    g_rec ^= 1u;
    g_lists[g_rec].n = 0;
    g_lists[g_rec].ns = 0;
    g_lists[g_rec].nv = 0;
    g_lists[g_rec].nraw = 0;
    g_lists[g_rec].nxf = 0;
}

/* GPU mode: has the GPU thread make its own copy into a texture. */

static void gpu_tex_copy(const TexCopy* info) {
    start_workers();
    wait_idle();
    if (!g_workers) {
        glr_tex_copy(info);
        return;
    }
    gx_mutex_lock(&g_mx);
    g_job = JOB_TEXCOPY;
    g_job_copy = info;
    g_busy = 1;
    g_submit_at = now_s();
    g_generation++;
    gx_cond_broadcast(&g_cv_work);
    gx_mutex_unlock(&g_mx);
    wait_idle();                       /* info lives on the caller's stack */
}

static void efb_to_memory(bool depth) {
    if (!g_gpu)
        return;
    start_workers();
    wait_idle();
    if (!g_workers) {
        glr_readback(depth);
        return;
    }
    gx_mutex_lock(&g_mx);
    g_job = JOB_READBACK;
    g_job_depth = depth;
    g_busy = 1;
    g_submit_at = now_s();
    g_generation++;
    gx_cond_broadcast(&g_cv_work);
    gx_mutex_unlock(&g_mx);
    wait_idle();
}

void gx_raster_set_threads(int n) {
    g_workers_wanted = n;
}

void gx_raster_sync(void) {
    wait_idle();
}

/* ---------------------------------------------------------- draw log --- */
/* --draw-log-at: writes every draw of a couple of frames, with the full GPU
   setup each one uses, to drawlog.txt. For finding rendering bugs. */
static double g_dlog_at = -1.0;
static int g_dlog_frames;              /* frames still to log once started */
static FILE* g_dlog;
static FILE* g_dlog_short;             /* one line per draw */
static u32 g_dlog_draws;

void gx_raster_draw_log(double at_seconds, int frames) {
    g_dlog_at = at_seconds;
    g_dlog_frames = frames;
}

static const char* const k_csel[16] = {"PREV.rgb", "PREV.a", "REG0.rgb", "REG0.a", "REG1.rgb", "REG1.a",
                                       "REG2.rgb", "REG2.a", "TEX.rgb", "TEX.a", "RAS.rgb", "RAS.a",
                                       "1", "0.5", "KONST", "0"};
static const char* const k_asel[8] = {"PREV.a", "REG0.a", "REG1.a", "REG2.a", "TEX.a", "RAS.a", "KONST", "0"};
static const char* const k_dst[4] = {"PREV", "REG0", "REG1", "REG2"};
static const char* const k_fmt[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7",
                                      "C4", "C8", "C14X2", "?11", "?12", "?13", "CMPR", "?15"};
static const char* const k_blend[8] = {"0", "1", "SRCCLR", "1-SRCCLR", "SRCA", "1-SRCA", "DSTA", "1-DSTA"};
static const char* const k_cmp[8] = {"never", "<", "==", "<=", ">", "!=", ">=", "always"};

static void dlog_op(FILE* f, u32 op, bool color) {
    u32 bias = (op >> 16) & 3u, sub = (op >> 18) & 1u, clamp = (op >> 19) & 1u, scale = (op >> 20) & 3u;
    static const char* const k_bias[4] = {"", " +0.5", " -0.5", " COMPARE"};
    static const char* const k_scale[4] = {"", " x2", " x4", " x0.5"};
    if (color) {
        fprintf(f, "%s = %s %c lerp(%s, %s, %s)%s%s%s", k_dst[(op >> 22) & 3u], k_csel[op & 15u], sub ? '-' : '+',
                k_csel[(op >> 12) & 15u], k_csel[(op >> 8) & 15u], k_csel[(op >> 4) & 15u], k_bias[bias], k_scale[scale],
                clamp ? "" : " (no clamp)");
    } else {
        fprintf(f, "%s.a = %s %c lerp(%s, %s, %s)%s%s%s  swap ras%u tex%u", k_dst[(op >> 22) & 3u], k_asel[(op >> 4) & 7u],
                sub ? '-' : '+', k_asel[(op >> 13) & 7u], k_asel[(op >> 10) & 7u], k_asel[(op >> 7) & 7u], k_bias[bias],
                k_scale[scale], clamp ? "" : " (no clamp)", op & 3u, (op >> 2) & 3u);
    }
    if (bias == 3)
        fprintf(f, " [compare mode %u]", ((op >> 20) & 3u) | (sub << 2));
}

/* Saves a decoded texture (level 0) as tex_ADDRESS.bmp: colour on the left,
   alpha as grey on the right. Once per address per log. */
static u32 g_dlog_dumped[128];
static u32 g_dlog_ndumped;

static void dlog_texture(u32 map, u32 addr) {
    for (u32 i = 0; i < g_dlog_ndumped; i++)
        if (g_dlog_dumped[i] == addr) return;
    if (g_dlog_ndumped >= 128 || !g_cpu) return;
    g_dlog_dumped[g_dlog_ndumped++] = addr;
    const GxTexture* t = gx_tex_get(g_cpu, map);
    if (!t) return;
    char path[64];
    snprintf(path, sizeof(path), "tex_%08X.bmp", addr);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 w = t->width * 2, h = t->height, row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    u8 hdr[54] = {'B', 'M'};
    hdr[2] = (u8)size; hdr[3] = (u8)(size >> 8); hdr[4] = (u8)(size >> 16); hdr[5] = (u8)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (u8)w; hdr[19] = (u8)(w >> 8);
    hdr[22] = (u8)h; hdr[23] = (u8)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)calloc(1, row);
    for (u32 y = 0; y < h; y++) {
        const u32* src = t->texels + (h - 1 - y) * t->width;
        for (u32 x = 0; x < t->width; x++) {
            u32 c = src[x];
            line[x * 3] = (u8)c; line[x * 3 + 1] = (u8)(c >> 8); line[x * 3 + 2] = (u8)(c >> 16);
            u8 a = (u8)(c >> 24);
            u8* d = line + (t->width + x) * 3;
            d[0] = d[1] = d[2] = a;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

static void dlog_short(u8 prim, u32 count, const SVtx* sv) {
    FILE* f = g_dlog_short;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 i = 0; i < count; i++) {
        if (!sv[i].valid) continue;
        x0 = fminf(x0, sv[i].x); x1 = fmaxf(x1, sv[i].x);
        y0 = fminf(y0, sv[i].y); y1 = fmaxf(y1, sv[i].y);
    }
    u32 stages = ((gx_bp[0x00] >> 10) & 15u) + 1, cm = gx_bp[0x41], at = gx_bp[0xF3];
    fprintf(f, "#%-4u box %4.0f,%4.0f-%4.0f,%4.0f prim %02X n%-3u cull %u stages %u", g_dlog_draws, x0, y0, x1, y1, prim,
            count, (gx_bp[0x00] >> 14) & 3u, stages);
    /* how it was projected: 3D or 2D, the horizontal correction (wider
       shapes), the viewport's size, and the transform unit's texgen count */
    fprintf(f, " %s xs%.3f vp%.0fx%.0f ntex%u", XFR[0x26] ? "2D" : "3D", g_setup.xs, fabsf(gx_f32(XFR[0x1A])) * 2.0f,
            fabsf(gx_f32(XFR[0x1B])) * 2.0f, XFR[0x3F] & 15u);
    if (count) fprintf(f, " mtx%u org%.3f,%.3f%s%s", sv[0].mtx, sv[0].org[0], sv[0].org[1], g_hud_match ? " match" : "",
                       (gx_bp[0x40] & 1u) ? "" : " noz");
    for (u32 i = 0; i < stages && i < 4; i++) {
        u32 order = gx_bp[0x28 + i / 2] >> ((i & 1) ? 12 : 0);
        fprintf(f, " [c%06X a%06X", gx_bp[0xC0 + i * 2] & 0xFFFFFFu, gx_bp[0xC1 + i * 2] & 0xFFFFFFu);
        if ((order >> 6) & 1u) {
            u32 map = order & 7u, bank = map < 4 ? 0u : 0x20u, m = map & 3u;
            u32 img0 = gx_bp[0x88 + bank + m], mode0 = gx_bp[0x80 + bank + m], mode1 = gx_bp[0x84 + bank + m];
            u32 tlut = gx_bp[0x98 + bank + m], fmt = (img0 >> 20) & 15u;
            u32 addr = (gx_bp[0x94 + bank + m] << 5) & 0x01FFFFE0u;
            fprintf(f, " %s %ux%u@%08X tc%u f%u/%u lod%u-%u bias%d", k_fmt[fmt], (img0 & 0x3FFu) + 1,
                    ((img0 >> 10) & 0x3FFu) + 1, addr, (order >> 3) & 7u,
                    (mode0 >> 4) & 1u, (mode0 >> 5) & 7u, mode1 & 0xFFu, (mode1 >> 8) & 0xFFu, (int)(s8)((mode0 >> 9) & 0xFFu));
            if (fmt == 8 || fmt == 9 || fmt == 10)
                fprintf(f, " tlut %s@tmem%05X", ((const char* const[]){"IA8", "RGB565", "RGB5A3", "?3"})[(tlut >> 10) & 3u],
                        (tlut & 0x3FFu) << 9);
            dlog_texture(map, addr);
        }
        fprintf(f, "]");
    }
    fprintf(f, " blend %s %s/%s at %s%u/%s%u col0 %.2f,%.2f,%.2f,%.2f", (cm & 1u) ? "on" : "off",
            k_blend[(cm >> 8) & 7u], k_blend[(cm >> 5) & 7u], k_cmp[(at >> 16) & 7u], at & 255u, k_cmp[(at >> 19) & 7u],
            (at >> 8) & 255u, sv[0].col[0][0], sv[0].col[0][1], sv[0].col[0][2], sv[0].col[0][3]);
    /* every vertex (small draws): screen position and texture coordinate 0 */
    fprintf(f, " v:");
    for (u32 i = 0; i < count && i < 8; i++) {
        float q = fabsf(sv[i].tc[0][2]) > 1e-9f ? sv[i].tc[0][2] : 1.0f;
        fprintf(f, " (%.0f,%.0f %.3f,%.3f)", sv[i].x, sv[i].y, sv[i].tc[0][0] / q, sv[i].tc[0][1] / q);
    }
    fprintf(f, "\n");
}

static void dlog_draw(u8 prim, u32 count, const SVtx* first) {
    if (!g_dlog) {
        g_dlog = fopen("drawlog.txt", "w");
        if (!g_dlog) { g_dlog_frames = 0; return; }
        g_dlog_short = fopen("drawlog-short.txt", "w");
        g_dlog_ndumped = 0;
        fprintf(g_dlog, "draw log from %.2f s of game time, frame %u\n", interrupts_game_seconds(), g_frames + 1);
        if (g_dlog_short)
            fprintf(g_dlog_short, "draw log from %.2f s of game time, frame %u (box = screen x0,y0-x1,y1; "
                    "f = mag/min filter; c/a = TEV colour/alpha op words)\n", interrupts_game_seconds(), g_frames + 1);
    }
    FILE* f = g_dlog;
    g_dlog_draws++;
    if (g_dlog_short)
        dlog_short(prim, count, first);
    fprintf(f, "\n#%u frame %u prim 0x%02X count %u\n", g_dlog_draws, g_frames + 1, prim, count);
    u32 stages = ((gx_bp[0x00] >> 10) & 15u) + 1;
    fprintf(f, "  genmode 0x%06X (tev stages %u, texgens %u, chans %u, cull %u)  pe_control 0x%06X (pixel format %u)\n",
            gx_bp[0x00], stages, gx_bp[0x00] & 15u, (gx_bp[0x00] >> 4) & 7u, (gx_bp[0x00] >> 14) & 3u, gx_bp[0x43],
            gx_bp[0x43] & 7u);
    u32 cm = gx_bp[0x41];
    fprintf(f, "  blend: %s  src %s dst %s%s  color%s alpha%s  cmode1 0x%06X  zmode 0x%06X (test %u func %s update %u)\n",
            (cm & 1u) ? "on" : "off", k_blend[(cm >> 8) & 7u], k_blend[(cm >> 5) & 7u], (cm >> 11) & 1u ? " SUBTRACT" : "",
            (cm >> 3) & 1u ? "+" : "-", (cm >> 4) & 1u ? "+" : "-", gx_bp[0x42], gx_bp[0x40], gx_bp[0x40] & 1u,
            k_cmp[(gx_bp[0x40] >> 1) & 7u], (gx_bp[0x40] >> 4) & 1u);
    u32 at = gx_bp[0xF3];
    fprintf(f, "  alpha test: a %s %u %s a %s %u\n", k_cmp[(at >> 16) & 7u], at & 255u,
            ((const char* const[]){"AND", "OR", "XOR", "XNOR"})[(at >> 22) & 3u], k_cmp[(at >> 19) & 7u], (at >> 8) & 255u);
    fprintf(f, "  regs PREV %d,%d,%d,%d REG0 %d,%d,%d,%d REG1 %d,%d,%d,%d REG2 %d,%d,%d,%d\n",
            gx_tev_reg[0][0], gx_tev_reg[0][1], gx_tev_reg[0][2], gx_tev_reg[0][3], gx_tev_reg[1][0], gx_tev_reg[1][1],
            gx_tev_reg[1][2], gx_tev_reg[1][3], gx_tev_reg[2][0], gx_tev_reg[2][1], gx_tev_reg[2][2], gx_tev_reg[2][3],
            gx_tev_reg[3][0], gx_tev_reg[3][1], gx_tev_reg[3][2], gx_tev_reg[3][3]);
    fprintf(f, "  konst K0 %u,%u,%u,%u K1 %u,%u,%u,%u K2 %u,%u,%u,%u K3 %u,%u,%u,%u\n",
            gx_tev_konst[0][0], gx_tev_konst[0][1], gx_tev_konst[0][2], gx_tev_konst[0][3], gx_tev_konst[1][0],
            gx_tev_konst[1][1], gx_tev_konst[1][2], gx_tev_konst[1][3], gx_tev_konst[2][0], gx_tev_konst[2][1],
            gx_tev_konst[2][2], gx_tev_konst[2][3], gx_tev_konst[3][0], gx_tev_konst[3][1], gx_tev_konst[3][2],
            gx_tev_konst[3][3]);
    fprintf(f, "  swap tables: ");
    for (u32 t = 0; t < 4; t++) {
        u32 k0 = gx_bp[0xF6 + t * 2], k1 = gx_bp[0xF7 + t * 2];
        fprintf(f, "%u=%c%c%c%c ", t, "rgba"[k0 & 3u], "rgba"[(k0 >> 2) & 3u], "rgba"[k1 & 3u], "rgba"[(k1 >> 2) & 3u]);
    }
    fprintf(f, "\n  xf: chans %u  ch0 color ctrl 0x%X alpha ctrl 0x%X mat 0x%08X amb 0x%08X | ch1 color 0x%X alpha 0x%X mat 0x%08X amb 0x%08X\n",
            gx_xf_reg[0x09], gx_xf_reg[0x0E], gx_xf_reg[0x10], gx_xf_reg[0x0C], gx_xf_reg[0x0A], gx_xf_reg[0x0F],
            gx_xf_reg[0x11], gx_xf_reg[0x0D], gx_xf_reg[0x0B]);
    for (u32 i = 0; i < stages; i++) {
        u32 order = gx_bp[0x28 + i / 2] >> ((i & 1) ? 12 : 0);
        u32 ksel = gx_bp[0xF6 + i / 2];
        u32 kc = (ksel >> ((i & 1) ? 14 : 4)) & 31u, ka = (ksel >> ((i & 1) ? 19 : 9)) & 31u;
        fprintf(f, "  stage %u: tex %s map %u coord %u, ras chan %u, kcsel 0x%02X kasel 0x%02X\n", i,
                (order >> 6) & 1u ? "on" : "off", order & 7u, (order >> 3) & 7u, (order >> 7) & 7u, kc, ka);
        fprintf(f, "    ");
        dlog_op(f, gx_bp[0xC0 + i * 2], true);
        fprintf(f, "\n    ");
        dlog_op(f, gx_bp[0xC1 + i * 2], false);
        fprintf(f, "\n");
        if ((order >> 6) & 1u) {
            u32 map = order & 7u, bank = map < 4 ? 0u : 0x20u, m = map & 3u;
            u32 img0 = gx_bp[0x88 + bank + m], img3 = gx_bp[0x94 + bank + m], mode0 = gx_bp[0x80 + bank + m];
            u32 tlut = gx_bp[0x98 + bank + m];
            fprintf(f, "    texture: %s %ux%u at 0x%08X  wrap %u/%u  filter mag %u min %u  tlut 0x%06X\n",
                    k_fmt[(img0 >> 20) & 15u], (img0 & 0x3FFu) + 1, ((img0 >> 10) & 0x3FFu) + 1, (img3 << 5) & 0x01FFFFE0u,
                    mode0 & 3u, (mode0 >> 2) & 3u, (mode0 >> 4) & 1u, (mode0 >> 5) & 7u, tlut);
        }
    }
    if (first)
        fprintf(f, "  first vertex: screen %.1f,%.1f z %.0f  col0 %.3f,%.3f,%.3f,%.3f  col1 %.3f,%.3f,%.3f,%.3f  tc0 %.3f,%.3f,%.3f\n",
                first->x, first->y, first->z, first->col[0][0], first->col[0][1], first->col[0][2], first->col[0][3],
                first->col[1][0], first->col[1][1], first->col[1][2], first->col[1][3], first->tc[0][0], first->tc[0][1],
                first->tc[0][2]);
}

static void dlog_frame_end(void) {
    if (g_dlog_short && g_dlog)
        fprintf(g_dlog_short, "-- end of frame %u\n", g_frames + 1);
    if (g_dlog && --g_dlog_frames <= 0) {
        fclose(g_dlog);
        g_dlog = NULL;
        if (g_dlog_short) fclose(g_dlog_short);
        g_dlog_short = NULL;
        g_dlog_at = -1.0;
        printf("  draw log written to drawlog.txt (%u draws)\n", g_dlog_draws);
    }
}

/* ------------------------------------------------------------ record --- */
static void push_tri(CmdList* l, u32 state, u32 a, u32 b, u32 c) {
    if (!l->verts[a].valid || !l->verts[b].valid || !l->verts[c].valid) {
        g_tris_skipped++;
        return;
    }
    Cmd* cmd = list_push(l);
    if (!cmd)
        return;
    cmd->type = CMD_TRI;
    cmd->state = state;
    cmd->u.vi[0] = a; cmd->u.vi[1] = b; cmd->u.vi[2] = c;
    g_tris_recorded++;
}

/* Records a triangle (vertex pool indices), cutting off any part in front
   of the near plane or beyond the far plane, as the GPU's clipper does
   (dropping whole triangles made big pieces of scenery that reach past the
   camera vanish and flicker; not clipping at the far plane drew distant
   scenery the game expects hidden). */
static float plane_d(const SVtx* v, int plane) {
    return plane ? v->far_d : v->near_d;
}

static void emit_tri(CmdList* l, u32 state, u32 a, u32 b, u32 c) {
    u32 poly[8] = {a, b, c};
    int n = 3;
    for (int plane = 0; plane < 2; plane++) {
        int inside = 0;
        for (int i = 0; i < n; i++) inside += plane_d(&l->verts[poly[i]], plane) >= 0.0f;
        if (inside == n)
            continue;
        if (inside == 0) {
            g_tris_skipped++;
            return;
        }
        /* Sutherland-Hodgman against one plane, keeping the winding order;
           new points go into the pool. */
        u32 out[8];
        int m = 0;
        for (int i = 0; i < n && m < 7; i++) {
            u32 pi = poly[i], qi = poly[(i + 1) % n];
            float pd = plane_d(&l->verts[pi], plane), qd = plane_d(&l->verts[qi], plane);
            bool pin = pd >= 0.0f, qin = qd >= 0.0f;
            if (pin)
                out[m++] = pi;
            if (pin != qin) {
                u32 ni = list_verts(l, 1);       /* may move the pool */
                if (ni == ~0u)
                    return;
                lerp_vtx(&l->verts[pi], &l->verts[qi], pd / (pd - qd), &l->verts[ni]);
                out[m++] = ni;
            }
        }
        memcpy(poly, out, sizeof(u32) * (size_t)m);
        n = m;
    }
    for (int i = 1; i + 1 < n; i++)
        push_tri(l, state, poly[0], poly[i], poly[i + 1]);
}

/* Lines and points: GX draws them as screen-space quads, lines widened
   across (up/down for flat lines, sideways for steep ones) by the line width
   and points as squares of the point size (both in sixths of a pixel, BP
   0x22). Texture coordinates can spread across the quad (the "texture
   offset", per coordinate in BP 0x30 + 2i); otherwise every corner keeps
   the vertex's own. Rain, sparks and outlines are drawn this way. */
static const float k_tex_off[8] = {0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f, 1.0f, 1.0f};

static void emit_quad_from(CmdList* l, u32 state, const u32 src[4], const float off[4][2], const float toff[4][2],
                           u32 tc_mask) {
    u32 b = list_verts(l, 4);           /* may move the pool */
    if (b == ~0u) return;
    float sx = fabsf(gx_f32(XFR[0x1A])), sy = fabsf(gx_f32(XFR[0x1B]));
    if (!(sx > 0.0f) || !(sy > 0.0f)) return;
    for (int k = 0; k < 4; k++) {
        SVtx* o = &l->verts[b + (u32)k];
        *o = l->verts[src[k]];
        o->clip[0] += off[k][0] / sx * o->clip[3];
        o->clip[1] -= off[k][1] / sy * o->clip[3];   /* screen y runs down */
        for (int t = 0; t < MAX_TC; t++)
            if (tc_mask & (1u << t)) {
                o->tc[t][0] += toff[k][0] * o->tc[t][2];
                o->tc[t][1] += toff[k][1] * o->tc[t][2];
            }
        project(o);
    }
    /* GX never culls lines and points: wind the quad so the face culling
       set for triangles keeps it (2 keeps a negative area, 1 a positive). */
    const SVtx* p0 = &l->verts[b];
    const SVtx* p1 = &l->verts[b + 1];
    const SVtx* p2 = &l->verts[b + 2];
    float area = (p1->x - p0->x) * (p2->y - p0->y) - (p1->y - p0->y) * (p2->x - p0->x);
    u32 cull = l->states[state].cull;
    bool flip = (cull == 2 && area > 0.0f) || (cull == 1 && area < 0.0f);
    if (flip) {
        emit_tri(l, state, b, b + 2, b + 1);
        emit_tri(l, state, b, b + 3, b + 2);
    } else {
        emit_tri(l, state, b, b + 1, b + 2);
        emit_tri(l, state, b, b + 2, b + 3);
    }
}

static void emit_line(CmdList* l, u32 state, u32 a, u32 c, u32 lp, u32 toffs) {
    const SVtx* va = &l->verts[a];
    const SVtx* vc = &l->verts[c];
    if (!va->valid || !vc->valid) return;
    float half = (float)(lp & 0xFFu) / 12.0f;
    if (half <= 0.0f) half = 1.0f / 12.0f;
    float dx = vc->x - va->x, dy = vc->y - va->y;
    float ox = 0.0f, oy = 0.0f;
    if (fabsf(dx) >= fabsf(dy)) oy = half; else ox = half;
    float t = k_tex_off[(lp >> 16) & 7u];
    const u32 src[4] = {a, a, c, c};
    const float off[4][2] = {{-ox, -oy}, {ox, oy}, {ox, oy}, {-ox, -oy}};
    const float to[4][2] = {{0, 0}, {fabsf(dx) >= fabsf(dy) ? 0.0f : t, fabsf(dx) >= fabsf(dy) ? t : 0.0f},
                            {fabsf(dx) >= fabsf(dy) ? 0.0f : t, fabsf(dx) >= fabsf(dy) ? t : 0.0f}, {0, 0}};
    emit_quad_from(l, state, src, off, to, toffs & 0xFFu);
}

static void emit_point(CmdList* l, u32 state, u32 a, u32 lp, u32 toffs) {
    if (!l->verts[a].valid) return;
    float half = (float)((lp >> 8) & 0xFFu) / 12.0f;
    if (half <= 0.0f) half = 1.0f / 12.0f;
    float t = k_tex_off[(lp >> 19) & 7u];
    const u32 src[4] = {a, a, a, a};
    const float off[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    const float to[4][2] = {{0, 0}, {t, 0}, {t, t}, {0, t}};
    emit_quad_from(l, state, src, off, to, (toffs >> 8) & 0xFFu);
}

/* ------------------------------------------------------------- rain --- */
/* The rain (PS2 style, an option): the game draws each drop as a short line
   from where it is to where it was a moment before (two matrices on the
   same point), opaque (blend off, passed by the alpha test). Drawn here as
   a longer streak instead: the line carried on back the same way (in clip
   space, which keeps it straight in the scene), see-through and fading
   towards its far end. The game's own splashes are drawn as it draws them
   (with the full rain settings put back, they show: see lodfix.c). */
#define RAIN_FLAG 0x80000000u
#define RAIN_STRETCH 5.5f              /* streak length, in the game's drop lengths */
#define RAIN_SPLASH_ALPHA 0.3f         /* the game's splashes, this opaque */
#define RAIN_DROPS 36                  /* of each rain box's 96 drops, how many are drawn */
#define RAIN_ALPHA 0.12f               /* a streak's opacity at its lower end */
int g_rain_style = 1;                  /* 1: PS2-style rain (settings) */

static u32 rain_state(CmdList* l, u32 state) {
    if (l->ns == l->cap_s) {
        u32 cap = l->cap_s * 2;
        RenderState* ns = (RenderState*)realloc(l->states, sizeof(RenderState) * cap);
        if (!ns) return state;
        l->states = ns;
        l->cap_s = cap;
    }
    RenderState* d = &l->states[l->ns];
    rs_copy(d, &l->states[state]);
    d->blend = true; d->sfac = 4; d->dfac = 5; d->subtract = false; d->blend_alpha = true;
    d->at_always = true; d->at_f0 = 7; d->at_f1 = 7; d->at_logic = 0;
    d->zupdate = false;
    return l->ns++;
}

/* More rain: each drop also falls in RAIN_COPIES other places beside it,
   moved sideways in the scene (not up or down, so they land, and splash,
   on the ground with it). A sideways move is the same for every point, so
   it is worked out straight in clip space from the camera's rotation and
   the projection. Drops are counted through each frame so a drop's splash
   (drawn just after it) gets the same copies. */
#define SPLASH_FLAG 0x40000000u
#define RAIN_COPIES 0
#define RAIN_SPREAD 8.0f               /* how far beside it (scene units; the game's rain box is 16 across) */
#define RAIN_LEAN 0.15f                 /* how much of the drops' own slant the streaks keep */
static const CmdList* g_rain_list;
static u32 g_rain_nv, g_rain_k;

static float rain_hash(u32 k, u32 d) {
    u32 h = k * 0x9E3779B1u ^ (d + 1u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

/* clip-space change for a move of w in the scene (v's matrix's rotation) */
static void rain_clip_delta(const SVtx* v, const float w[3], float out[4]) {
    const u32* pr = &XFR[0x20];
    const u32* m = &XFM[(v->mtx * 4u) & 0x3FFu];
    float ex = gx_f32(m[0]) * w[0] + gx_f32(m[1]) * w[1] + gx_f32(m[2]) * w[2];
    float ey = gx_f32(m[4]) * w[0] + gx_f32(m[5]) * w[1] + gx_f32(m[6]) * w[2];
    float ez = gx_f32(m[8]) * w[0] + gx_f32(m[9]) * w[1] + gx_f32(m[10]) * w[2];
    out[0] = (gx_f32(pr[0]) * ex + gx_f32(pr[1]) * ez) * g_setup.xs;
    out[1] = gx_f32(pr[2]) * ey + gx_f32(pr[3]) * ez;
    out[2] = gx_f32(pr[4]) * ez;
    out[3] = -ez;
}

/* scene move for a clip-space change (the reverse) */
static bool rain_world_delta(const SVtx* v, const float d[4], float out[3]) {
    const u32* pr = &XFR[0x20];
    const u32* m = &XFM[(v->mtx * 4u) & 0x3FFu];
    float p0 = gx_f32(pr[0]), p1 = gx_f32(pr[1]), p2 = gx_f32(pr[2]), p3 = gx_f32(pr[3]);
    if (!(fabsf(p0) > 1e-9f) || !(fabsf(p2) > 1e-9f) || !(fabsf(g_setup.xs) > 1e-9f)) return false;
    float ez = -d[3], ex = (d[0] / g_setup.xs - p1 * ez) / p0, ey = (d[1] - p3 * ez) / p2;
    for (int i = 0; i < 3; i++) out[i] = gx_f32(m[i]) * ex + gx_f32(m[4 + i]) * ey + gx_f32(m[8 + i]) * ez;
    return true;
}

/* the scene's up axis: the one pointing most up the screen */
static int rain_up(const SVtx* v) {
    const u32* m = &XFM[(v->mtx * 4u) & 0x3FFu];
    int up = 0;
    for (int i = 1; i < 3; i++)
        if (fabsf(gx_f32(m[4 + i])) > fabsf(gx_f32(m[4 + up]))) up = i;
    return up;
}

static void rain_offset(u32 k, u32 d, int up, float w[3]) {
    int s1 = (up + 1) % 3, s2 = (up + 2) % 3;
    w[0] = w[1] = w[2] = 0.0f;
    w[s1] = (rain_hash(k, d * 2) - 0.5f) * 2.0f * RAIN_SPREAD;
    w[s2] = (rain_hash(k, d * 2 + 1) - 0.5f) * 2.0f * RAIN_SPREAD;
}

static void rain_streak(CmdList* l, u32 state, const SVtx* a_in, const float tip[4]) {
    SVtx a = *a_in, pe = *a_in;          /* the pool may move */
    memcpy(pe.clip, tip, sizeof pe.clip);
    if (!(pe.clip[3] > 1e-6f) || !a.valid) return;
    project(&pe);
    float sx = fabsf(gx_f32(XFR[0x1A])), sy = fabsf(gx_f32(XFR[0x1B]));
    float dx = pe.x - a.x, dy = pe.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (!(sx > 0.0f) || !(sy > 0.0f) || len < 1e-4f) return;
    float half = 0.4f, nx = -dy / len * half, ny = dx / len * half;
    u32 b = list_verts(l, 4);
    if (b == ~0u) return;
    const SVtx* src[4] = {&a, &a, &pe, &pe};
    const float sg[4] = {-1.0f, 1.0f, 1.0f, -1.0f}, al[4] = {RAIN_ALPHA, RAIN_ALPHA, 0.0f, 0.0f};
    for (int k = 0; k < 4; k++) {
        SVtx* o = &l->verts[b + (u32)k];
        *o = *src[k];
        o->clip[0] += sg[k] * nx / sx * o->clip[3];
        o->clip[1] -= sg[k] * ny / sy * o->clip[3];
        for (int ch = 0; ch < 3; ch++) o->col[0][ch] = fminf(1.0f, a.col[0][ch] * 1.1f + 0.05f);
        o->col[0][3] = al[k];
        project(o);
    }
    emit_tri(l, state, b, b + 1, b + 2);
    emit_tri(l, state, b, b + 2, b + 3);
}

/* A splash's state: as the game's, without its alpha test (16), so made
   fainter it still lasts as long. */
static u32 splash_state(CmdList* l, u32 state) {
    if (l->ns == l->cap_s) {
        u32 cap = l->cap_s * 2;
        RenderState* ns = (RenderState*)realloc(l->states, sizeof(RenderState) * cap);
        if (!ns) return state;
        l->states = ns;
        l->cap_s = cap;
    }
    RenderState* d = &l->states[l->ns];
    rs_copy(d, &l->states[state]);
    d->at_always = true; d->at_f0 = 7; d->at_f1 = 7; d->at_logic = 0;
    d->zupdate = false;                /* (its clear parts no longer cut out by the test) */
    return l->ns++;
}

static void emit_rain(CmdList* l, u32 state, u32 ia, u32 ic) {
    if (l != g_rain_list || l->nv < g_rain_nv) { g_rain_k = 0; g_rain_list = l; }   /* a new frame */
    g_rain_nv = l->nv;
    u32 k = g_rain_k++;
    SVtx a = l->verts[ia], c = l->verts[ic];   /* a: where it is; c: where it was */
    if (!a.valid || !c.valid) return;
    /* the streak: back along the drop's own line, its slant cut down
       (worked out in the scene, then back into clip space) */
    float d[4] = {c.clip[0] - a.clip[0], c.clip[1] - a.clip[1], c.clip[2] - a.clip[2], c.clip[3] - a.clip[3]};
    float w[3];
    int up = rain_up(&a);
    if (rain_world_delta(&a, d, w)) {
        float wl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        for (int i = 0; i < 3; i++) if (i != up) w[i] *= RAIN_LEAN;
        float nl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (nl > 1e-9f) for (int i = 0; i < 3; i++) w[i] *= wl / nl * RAIN_STRETCH;
        rain_clip_delta(&a, w, d);
    } else {
        for (int i = 0; i < 4; i++) d[i] *= RAIN_STRETCH;
    }
    for (u32 n = 0; n <= RAIN_COPIES; n++) {
        SVtx at = a;
        if (n) {
            float off[3], oc[4];
            rain_offset(k, n, up, off);
            rain_clip_delta(&a, off, oc);
            for (int i = 0; i < 4; i++) at.clip[i] += oc[i];
            if (!(at.clip[3] > 1e-6f)) continue;
            project(&at);
            at.near_d = at.clip[2] + at.clip[3]; at.far_d = -at.clip[2];
        }
        float tip[4] = {at.clip[0] + d[0], at.clip[1] + d[1], at.clip[2] + d[2], at.clip[3] + d[3]};
        rain_streak(l, state, &at, tip);
    }
}

/* A drop's splash (the game's quad), also where its copies land. */
static void emit_splash_copies(CmdList* l, u32 state, u32 q) {
    if (!g_rain_k) return;
    u32 k = g_rain_k - 1u;                       /* the drop just drawn */
    int up = rain_up(&l->verts[q]);
    for (u32 n = 1; n <= RAIN_COPIES; n++) {
        float off[3], oc[4];
        rain_offset(k, n, up, off);
        rain_clip_delta(&l->verts[q], off, oc);
        u32 b = list_verts(l, 4);
        if (b == ~0u) return;
        bool ok = true;
        for (u32 i = 0; i < 4; i++) {
            SVtx* o = &l->verts[b + i];
            *o = l->verts[q + i];
            for (int j = 0; j < 4; j++) o->clip[j] += oc[j];
            if (!(o->clip[3] > 1e-6f)) ok = false;
            o->near_d = o->clip[2] + o->clip[3]; o->far_d = -o->clip[2];
            project(o);
        }
        if (!ok) continue;
        emit_tri(l, state, b, b + 1, b + 2);
        emit_tri(l, state, b, b + 2, b + 3);
    }
}

/* BP state lines and points need (recorded with each draw). */
static void line_point_state(u32* lp, u32* toffs) {
    *lp = gx_bp[0x22] & 0xFFFFFFu;
    u32 m = 0;
    for (u32 i = 0; i < 8; i++) {
        u32 ss = gx_bp[0x30 + i * 2];
        if ((ss >> 18) & 1u) m |= 1u << i;
        if ((ss >> 19) & 1u) m |= 1u << (8 + i);
    }
    *toffs = m;
}

/* Triangles of a primitive whose transformed vertices are at base..base+count. */
static void emit_prim(CmdList* l, u32 state, u8 prim, u32 base, u32 count, u32 lp, u32 toffs) {
#define EMIT(i0, i1, i2) emit_tri(l, state, base + (i0), base + (i1), base + (i2))

    switch (prim & 0xF8) {
    case 0x80:
    case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4) {
            if (toffs & SPLASH_FLAG)   /* the game's splash, fainter */
                for (u32 j = 0; j < 4; j++) l->verts[base + i + j].col[0][3] *= RAIN_SPLASH_ALPHA;
            EMIT(i, i + 1, i + 2);
            EMIT(i, i + 2, i + 3);
            if (toffs & SPLASH_FLAG) emit_splash_copies(l, state, base + i);
        }
        break;
    case 0x90:
        for (u32 i = 0; i + 2 < count; i += 3)
            EMIT(i, i + 1, i + 2);
        break;
    case 0x98:   /* strip: every other triangle is reversed to keep one winding */
        for (u32 i = 0; i + 2 < count; i++) {
            if (i & 1u)
                EMIT(i + 1, i, i + 2);
            else
                EMIT(i, i + 1, i + 2);
        }
        break;
    case 0xA0:
        for (u32 i = 1; i + 1 < count; i++)
            EMIT(0, i, i + 1);
        break;
    case 0xA8:   /* lines */
        for (u32 i = 0; i + 1 < count; i += 2) {
            if (toffs & RAIN_FLAG) emit_rain(l, state, base + i, base + i + 1);
            else emit_line(l, state, base + i, base + i + 1, lp, toffs);
        }
        break;
    case 0xB0:   /* line strip */
        for (u32 i = 0; i + 1 < count; i++) emit_line(l, state, base + i, base + i + 1, lp, toffs);
        break;
    case 0xB8:   /* points */
        for (u32 i = 0; i < count; i++) emit_point(l, state, base + i, lp, toffs);
        break;
    default:
        break;
    }
#undef EMIT
}

/* Room for count more decoded vertices (deferred mode); ~0 if out of memory. */
static u32 list_raw(CmdList* l, u32 count) {
    if (l->nraw + count > l->cap_raw) {
        u32 cap = l->cap_raw ? l->cap_raw : 16384;
        while (cap < l->nraw + count) cap *= 2;
        Vtx* v = (Vtx*)realloc(l->raw, sizeof(Vtx) * cap);
        if (!v)
            return ~0u;
        l->raw = v;
        l->cap_raw = cap;
    }
    u32 base = l->nraw;
    l->nraw += count;
    return base;
}

/* Deferred mode: transform unit writes go into the list in stream order. */
void gx_raster_xf_write(u32 addr, const u32* values, u32 n) {
    if (!g_defer || !gx_raster_active() || !n)
        return;
    CmdList* l = &g_lists[g_rec];
    if (l->nxf + n > l->cap_xf) {
        u32 cap = l->cap_xf ? l->cap_xf : 65536;
        while (cap < l->nxf + n) cap *= 2;
        u32* d = (u32*)realloc(l->xfdata, sizeof(u32) * cap);
        if (!d)
            return;
        l->xfdata = d;
        l->cap_xf = cap;
    }
    memcpy(l->xfdata + l->nxf, values, sizeof(u32) * n);
    /* one command for a run of consecutive writes */
    Cmd* last = l->n ? &l->cmds[l->n - 1] : NULL;
    if (last && last->type == CMD_XF && last->u.xf.addr + last->u.xf.n == addr &&
        last->u.xf.off + last->u.xf.n == l->nxf) {
        last->u.xf.n += n;
    } else {
        Cmd* c = list_push(l);
        if (!c)
            return;
        c->type = CMD_XF;
        c->u.xf.addr = addr;
        c->u.xf.n = n;
        c->u.xf.off = l->nxf;
    }
    l->nxf += n;
}

/* GPU thread: turns a deferred list into one of triangles, replaying the
   transform unit writes into its own copy of the registers and transforming,
   lighting and clipping each draw with them. */
static CmdList g_expanded;

static const CmdList* expand_list(const CmdList* l) {
    CmdList* e = &g_expanded;
    take_free_camera();
    e->n = 0;
    e->nv = 0;
    e->states = l->states;
    e->ns = l->ns;
    for (u32 i = 0; i < l->n; i++) {
        const Cmd* c = &l->cmds[i];
        if (c->type == CMD_XF) {
            const u32* v = l->xfdata + c->u.xf.off;
            for (u32 k = 0, a = c->u.xf.addr; k < c->u.xf.n; k++, a++) {
                if (a < 0x800u) g_xf_mem_local[a] = v[k];
                else if (a >= 0x1000u && a < 0x1100u) g_xf_reg_local[a - 0x1000u] = v[k];
            }
        } else if (c->type == CMD_DRAW) {
            u32 count = c->u.draw.count;
            u32 base = list_verts(e, count);
            if (base == ~0u)
                continue;
            draw_setup();
            const Vtx* raw = l->raw + c->u.draw.first;
            for (u32 k = 0; k < count; k++)
                transform(&raw[k], &e->verts[base + k]);
            g_wl = e; g_wb = base; g_wstate = &l->states[c->state];
            widen_fullscreen_2d(&e->verts[base], count, state_textured(&l->states[c->state]), l->states[c->state].ztest,
                                state_capture(&l->states[c->state]));
            emit_prim(e, c->state, (u8)c->u.draw.prim, base, count, c->u.draw.lp, c->u.draw.toffs);
        } else {
            Cmd* d = list_push(e);
            if (d) *d = *c;
        }
    }
    return e;
}

static void raster_draw(CPUState* cpu, u8 prim, u32 vat, u32 count, const u8* data) {
    if (count)
        note_projection();
    if (!gx_raster_active() || count == 0)
        return;
    if ((gx_bp[0x00] >> 14 & 3u) == 3u && (prim & 0xF8) < 0xA8)
        return;                        /* cull everything (triangles; lines and points aren't culled) */
    g_cpu = cpu;
    if (g_u8f[255] == 0.0f)
        for (int i = 0; i < 256; i++) g_u8f[i] = (float)i / 255.0f;
    CmdList* l = &g_lists[g_rec];
    u32 state = list_state(l);
    bool rain = (prim & 0xF8) == 0xA8 && g_rain_style && g_hud_match && !l->states[state].blend;
    if (rain) state = rain_state(l, state);
    /* The game draws its rain a box at a time (16 x 16 x 8, tiled over the
       camera's view, 0x801A7880) but stops after 22 boxes in a frame (the
       count at 0x803ECC2C, bumped after each box drawn), so with a wide
       view the far or high boxes went missing and rain stopped in mid-air.
       While a box is being drawn the count is set back, so every box in
       view is drawn. */
    if (rain && cpu->ram_size > 0x003ECC30u) {
        u8* cnt = cpu->ram + 0x003ECC2Cu;
        cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
        /* How many of a box's 96 drops are drawn is worked out again each
           frame from the rain's strength (0x801A7784: strength x 96, at
           0x803EB858), which keeps The Alley at a handful; RAIN_DROPS of
           them are drawn from here on (all 96 always move). */
        u8* num = cpu->ram + 0x003EB858u;
        num[0] = num[1] = num[2] = 0; num[3] = RAIN_DROPS;
    }
    /* each drop's splash is drawn straight after its line (a quad) */
    static bool prev_rain;
    bool splash = !rain && prev_rain && (prim & 0xF8) == 0x80;
    prev_rain = rain;
    if (splash) state = splash_state(l, state);

    if (g_defer) {                     /* decode now (RAM may change), transform on the GPU thread */
        u32 first = list_raw(l, count);
        if (first == ~0u)
            return;
        VtxFormat fmt;
        vtx_format(vat, &fmt);
        const u8* p = data;
        for (u32 i = 0; i < count; i++)
            p += decode_vertex(cpu, &fmt, p, &l->raw[first + i]);
        Cmd* c = list_push(l);
        if (!c)
            return;
        c->type = CMD_DRAW;
        c->state = state;
        c->u.draw.prim = prim;
        c->u.draw.count = count;
        c->u.draw.first = first;
        line_point_state(&c->u.draw.lp, &c->u.draw.toffs);
        if (rain) c->u.draw.toffs |= RAIN_FLAG;
        if (splash) c->u.draw.toffs |= SPLASH_FLAG;
        return;
    }

    take_free_camera();
    /* Transform straight into the list's vertex pool; triangles refer to
       vertices by index, so strips and fans don't copy each vertex 3 times. */
    u32 base = list_verts(l, count);
    if (base == ~0u)
        return;
    SVtx* sv = &l->verts[base];
    const u8* p = data;
    VtxFormat fmt;
    vtx_format(vat, &fmt);
    draw_setup();
    for (u32 i = 0; i < count; i++) {
        Vtx v;
        p += decode_vertex(cpu, &fmt, p, &v);
        transform(&v, &sv[i]);
    }
    g_wl = l; g_wb = base; g_wstate = &l->states[state];
    widen_fullscreen_2d(sv, count, state_textured(&l->states[state]), l->states[state].ztest,
                        state_capture(&l->states[state]));
    if (g_dlog_at >= 0.0 && g_dlog_frames > 0 && interrupts_game_seconds() >= g_dlog_at)
        dlog_draw(prim, count, &sv[0]);

    u32 lp, toffs;
    line_point_state(&lp, &toffs);
    if (rain) toffs |= RAIN_FLAG;
    if (splash) toffs |= SPLASH_FLAG;
    emit_prim(l, state, prim, base, count, lp, toffs);
}

void gx_raster_draw(CPUState* cpu, u8 prim, u32 vat, u32 count, const u8* data) {
    int prev = prof_enter(PROF_GX_VERTEX);
    raster_draw(cpu, prim, vat, count, data);
    prof_leave(prev);
}

/* ------------------------------------------------------- copy & dump --- */
static void write_bmp(const char* path, u32 sx, u32 sy, u32 w, u32 h) {
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    u32 row = (w * 3 + 3) & ~3u;
    u32 size = 54 + row * h;
    u8 hdr[54] = {'B', 'M'};
    hdr[2] = (u8)size; hdr[3] = (u8)(size >> 8); hdr[4] = (u8)(size >> 16); hdr[5] = (u8)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (u8)w; hdr[19] = (u8)(w >> 8);
    hdr[22] = (u8)h; hdr[23] = (u8)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)calloc(1, row);
    for (u32 y = 0; y < h; y++) {
        u32 srcy = sy + (h - 1 - y);
        for (u32 x = 0; x < w; x++) {
            u32 c = (srcy < EFB_H && sx + x < EFB_W) ? g_color[srcy * EFB_W + sx + x] : 0;
            line[x * 3 + 0] = (u8)c;
            line[x * 3 + 1] = (u8)(c >> 8);
            line[x * 3 + 2] = (u8)(c >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

/* EFB -> external frame buffer in RAM, as YUYV 4:2:2 (what the TV shows).
   Destination BP 0x4B and stride BP 0x4D (both in 32-byte units). The number
   of lines written follows the vertical scale (BP 0x4E, 8.8 fixed point;
   copy command bit 10 says whether it is inverted), exactly as the hardware
   does, so the copy never runs past the buffer the game allocated. */
static u32 g_xfb_copy_log;

/* TV frame buffers filled by frame-buffer copies: how many lines each copy
   wrote. The picture shown can be taller than what the game copied (the
   video interface shows 480 lines, the game copies fewer); the rest of that
   memory holds leftovers, which showed as a strip of noise along the bottom.
   Those lines are shown black instead. A record lapses when the buffer is
   written another way (movies write frames straight into it) or when no
   copy has gone there for a few frames. */
typedef struct { u32 dest, stride, lines, frame; u32 sum; bool valid; } XfbCopy;
static XfbCopy g_xfb_copies[4];
/* Until the game copies its first picture to the screen, the picture memory
   holds whatever was there at boot (zeros and leftovers that show as green
   and lines of noise), so the screen stays black until then. */
static bool g_xfb_first_copy;

/* A quick fingerprint of a TV buffer in RAM (a few hundred words spread over
   it). When the GPU keeps the copies and RAM isn't written, a change means
   the game wrote the buffer itself (a movie frame). */
static u32 xfb_sample_sum(u32 dest, u32 stride, u32 lines) {
    if (!g_cpu || !stride || !lines || dest >= g_cpu->ram_size || (u64)stride * lines > g_cpu->ram_size - dest)
        return 0;
    u32 bytes = stride * lines, h = 2166136261u;
    for (u32 i = 0; i < 512; i++) {
        u32 off = (u32)((u64)bytes * i / 512u) & ~3u;
        u32 v;
        memcpy(&v, g_cpu->ram + dest + off, 4);
        h = (h ^ v) * 16777619u;
    }
    return h;
}

static void note_xfb_copy(u32 dest, u32 stride, u32 lines) {
    g_xfb_first_copy = true;
    XfbCopy* slot = &g_xfb_copies[0];
    for (int i = 0; i < 4; i++) {
        if (g_xfb_copies[i].valid && g_xfb_copies[i].dest == dest) { slot = &g_xfb_copies[i]; break; }
        if (!g_xfb_copies[i].valid || g_xfb_copies[i].frame < slot->frame) slot = &g_xfb_copies[i];
    }
    slot->dest = dest; slot->stride = stride; slot->lines = lines; slot->frame = g_frames; slot->valid = true;
    slot->sum = xfb_sample_sum(dest, stride, lines);
}

void gx_raster_note_ram_write(u32 addr) {
    for (int i = 0; i < 4; i++) {
        XfbCopy* c = &g_xfb_copies[i];
        if (c->valid && addr >= c->dest && addr < c->dest + c->stride * 576u)
            c->valid = false;
    }
}

static u32 xfb_copied_lines(u32 addr) {
    for (int i = 0; i < 4; i++) {
        const XfbCopy* c = &g_xfb_copies[i];
        if (c->valid && c->dest == addr && g_frames - c->frame <= 4u)
            return c->lines;
    }
    return ~0u;
}

static void setup_xfb_copy(CPUState* cpu, CopyCmd* cp) {
    u32 dest = (gx_bp[0x4B] & 0x00FFFFFFu) << 5;
    u32 stride = (gx_bp[0x4D] & 0x3FFu) << 5;
    u32 yscale_reg = gx_bp[0x4E] & 0x1FFu;
    bool invert = (cp->cmd >> 10) & 1u;
    double yscale = 1.0;
    if (yscale_reg)
        yscale = invert ? 256.0 / yscale_reg : yscale_reg / 256.0;
    /* The GPU writes 1 + (height - 1) * scale lines (as Dolphin computes it). */
    u32 lines = 1u + (u32)((double)(cp->h - 1u) * yscale);
    if (lines > 1024)
        lines = 1024;
    u32 w = cp->w;
    if (w * 2u > stride)
        w = stride / 2u;               /* a line never spills into the next one */
    if (g_xfb_copy_log < 8) {
        g_xfb_copy_log++;
        printf("  xfb copy: %ux%u from (%u,%u) -> 0x%08X stride %u, yscale 0x%03X%s = %u lines, cmd 0x%06X\n",
               w, cp->h, cp->sx, cp->sy, dest, stride, yscale_reg, invert ? " (inverted)" : "", lines, cp->cmd);
    }
    if (!cpu || !w || !lines || dest >= cpu->ram_size || (u64)lines * stride > cpu->ram_size - dest)
        lines = 0;
    cp->w = w;
    cp->dest = dest;
    cp->stride = stride;
    cp->lines = lines;
    if (lines)
        note_xfb_copy(dest, stride, lines);
    cp->yscale = (float)yscale;
}

/* ------------------------------------------------- EFB to texture copy --- */
/* GXCopyTex: the game copies a rectangle of the frame buffer into RAM in a
   texture format and then draws with it (screen transitions such as the
   crossfade and the curtain wipe, and other render-to-texture effects).
   The frame so far is drawn first, then the pixels are converted and
   written out in the GPU's tiled texture layout. */
static unsigned long long g_tex_copies;

static void efb_texel(u32 x, u32 y, bool half, bool depth, bool keep_alpha, u8 rgba[4]) {
    u32 r = 0, g = 0, b = 0, a = 0, n = half ? 2u : 1u;
    for (u32 dy = 0; dy < n; dy++)
        for (u32 dx = 0; dx < n; dx++) {
            u32 px = x + dx < EFB_W ? x + dx : EFB_W - 1, py = y + dy < EFB_H ? y + dy : EFB_H - 1;
            if (depth) {
                u32 z = g_depth[py * EFB_W + px] & 0xFFFFFFu;
                r += z >> 16; g += (z >> 8) & 255u; b += z & 255u; a += 255u;
            } else {
                u32 c = g_color[py * EFB_W + px];
                r += (c >> 16) & 255u; g += (c >> 8) & 255u; b += c & 255u;
                a += keep_alpha ? c >> 24 : 255u;
            }
        }
    u32 d = n * n;
    rgba[0] = (u8)(r / d); rgba[1] = (u8)(g / d); rgba[2] = (u8)(b / d); rgba[3] = (u8)(a / d);
}

/* Returns false if nothing was written. *info: what was copied (for the GPU's
   own copy of it); bytes: how much RAM it covers. */
static bool efb_copy_to_texture(u32 cmd, TexCopy* info, u32* dest_out, u32* bytes_out) {
    CPUState* cpu = g_cpu;
    if (!cpu)
        return false;
    u32 sx = gx_bp[0x49] & 0x3FFu, sy = (gx_bp[0x49] >> 10) & 0x3FFu;
    u32 w = (gx_bp[0x4A] & 0x3FFu) + 1, h = ((gx_bp[0x4A] >> 10) & 0x3FFu) + 1;
    u32 dest = (gx_bp[0x4B] & 0x00FFFFFFu) << 5;
    u32 stride = (gx_bp[0x4D] & 0x3FFu) << 5;        /* bytes from one row of tiles to the next */
    bool half = (cmd >> 9) & 1u;
    bool intensity = (cmd >> 15) & 1u;
    u32 tpf = (cmd >> 3) & 15u;
    u32 fmt = (tpf >> 1) | ((tpf & 1u) << 3);         /* the copy's texture format */
    u32 pixfmt = gx_bp[0x43] & 7u;
    bool depth = pixfmt == 3u;                        /* Z24 frame buffer: depth copy */
    bool keep_alpha = pixfmt == 1u;                   /* only RGBA6 keeps alpha */
    u32 src_w = w, src_h = h;
    if (half) { w = (w + 1) / 2; h = (h + 1) / 2; }

    /* Tile shape and bytes per texel for each format. */
    u32 tw, th, bits;
    switch (fmt) {
    case 0x0: tw = 8; th = 8; bits = 4; break;                    /* R4 / I4 */
    case 0x1: case 0x8: case 0x7: case 0x9: case 0xA:
    case 0x2: tw = 8; th = 4; bits = 8; break;                    /* R8, A8, G8, B8, RA4 */
    case 0x3: case 0x4: case 0x5: case 0xB: case 0xC:
              tw = 4; th = 4; bits = 16; break;                   /* RA8, RGB565, RGB5A3, RG8, GB8 */
    case 0x6: tw = 4; th = 4; bits = 32; break;                   /* RGBA8 */
    default: return false;
    }
    u32 tiles_x = (w + tw - 1) / tw, tiles_y = (h + th - 1) / th;
    u32 tile_bytes = tw * th * bits / 8;
    if (fmt == 0x6) tile_bytes = 64;
    if (!stride) stride = tiles_x * tile_bytes;
    if (dest >= cpu->ram_size || (u64)tiles_y * stride > cpu->ram_size - dest)
        return false;

    for (u32 ty = 0; ty < tiles_y; ty++)
        for (u32 tx = 0; tx < tiles_x; tx++) {
            u8* out = cpu->ram + dest + ty * stride + tx * tile_bytes;
            for (u32 iy = 0; iy < th; iy++)
                for (u32 ix = 0; ix < tw; ix++) {
                    u32 x = tx * tw + ix, y = ty * th + iy;
                    u8 c[4] = {0, 0, 0, 0};
                    if (x < w && y < h)
                        efb_texel(sx + (half ? x * 2 : x), sy + (half ? y * 2 : y), half, depth, keep_alpha, c);
                    u8 lum = intensity ? (u8)((66 * c[0] + 129 * c[1] + 25 * c[2] + 128) / 256 + 16)
                                       : c[0];
                    u32 i = iy * tw + ix;
                    switch (fmt) {
                    case 0x0: {                                   /* 4 bits: high nibble first */
                        u8 v = lum >> 4;
                        if (i & 1u) out[i / 2] = (u8)((out[i / 2] & 0xF0u) | v);
                        else out[i / 2] = (u8)(v << 4);
                        break;
                    }
                    case 0x1: case 0x8: out[i] = lum; break;      /* R8 / I8 */
                    case 0x7: out[i] = c[3]; break;               /* A8 */
                    case 0x9: out[i] = c[1]; break;               /* G8 */
                    case 0xA: out[i] = c[2]; break;               /* B8 */
                    case 0x2: out[i] = (u8)((c[3] & 0xF0u) | (lum >> 4)); break;   /* RA4: alpha high */
                    case 0x3: out[i * 2] = c[3]; out[i * 2 + 1] = lum; break;      /* RA8 */
                    case 0xB: out[i * 2] = c[0]; out[i * 2 + 1] = c[1]; break;     /* RG8 */
                    case 0xC: out[i * 2] = c[1]; out[i * 2 + 1] = c[2]; break;     /* GB8 */
                    case 0x4: {                                   /* RGB565 */
                        u32 v = ((u32)(c[0] >> 3) << 11) | ((u32)(c[1] >> 2) << 5) | (c[2] >> 3);
                        out[i * 2] = (u8)(v >> 8); out[i * 2 + 1] = (u8)v;
                        break;
                    }
                    case 0x5: {                                   /* RGB5A3 */
                        u32 v;
                        if (c[3] >= 0xE0u)
                            v = 0x8000u | ((u32)(c[0] >> 3) << 10) | ((u32)(c[1] >> 3) << 5) | (c[2] >> 3);
                        else
                            v = ((u32)(c[3] >> 5) << 12) | ((u32)(c[0] >> 4) << 8) | ((u32)(c[1] >> 4) << 4) | (c[2] >> 4);
                        out[i * 2] = (u8)(v >> 8); out[i * 2 + 1] = (u8)v;
                        break;
                    }
                    case 0x6:                                     /* RGBA8: AR half, then GB half */
                        out[i * 2] = c[3]; out[i * 2 + 1] = c[0];
                        out[32 + i * 2] = c[1]; out[32 + i * 2 + 1] = c[2];
                        break;
                    }
                }
        }
    gx_tex_invalidate(dest, tiles_y * stride);
    g_tex_copies++;
    if (info) {
        info->sx = sx; info->sy = sy; info->src_w = src_w; info->src_h = src_h;
        info->w = w; info->h = h; info->fmt = fmt;
        info->intensity = intensity; info->keep_alpha = keep_alpha;
        info->id = depth ? 0u : 1u;    /* depth copies stay in RAM only */
    }
    *dest_out = dest;
    *bytes_out = tiles_y * stride;
    return true;
}

/* GPU mode: frame-buffer copies into textures are also made on the GPU, at
   its resolution, and drawn with in place of the (native-size) RAM copy for
   as long as that RAM is unchanged (a fingerprint, as for TV buffers). */
#define TEX_COPY_RECORDS 16
typedef struct { u32 id, dest, bytes, w, h, sum; bool valid; } TexCopyRec;
static TexCopyRec g_tex_copy_recs[TEX_COPY_RECORDS];
static u32 g_tex_copy_next_id = 1;

static u32 ram_sample_sum(u32 addr, u32 bytes) {
    if (!g_cpu || !bytes || addr >= g_cpu->ram_size || bytes > g_cpu->ram_size - addr)
        return 0;
    u32 h = 2166136261u, n = bytes / 4u < 256u ? bytes / 4u : 256u;
    for (u32 i = 0; i < n; i++) {
        u32 off = (u32)((u64)bytes * i / n) & ~3u, v;
        memcpy(&v, g_cpu->ram + addr + off, 4);
        h = (h ^ v) * 16777619u;
    }
    return h;
}

/* The GPU copy to use for a texture map, or 0. */
/* Where the game has copied the frame buffer to (and how big), remembered
   apart from the copies the GPU keeps: one made a while ago (the shot faded
   out as a match starts) is still a copy of the whole picture after the GPU's
   own records have moved on. */
#define COPY_DESTS 16
static struct { u32 addr, w, h; } g_copy_dests[COPY_DESTS];
static int g_copy_dest_next;

static void copy_dest_note(u32 addr, u32 w, u32 h) {
    for (int k = 0; k < COPY_DESTS; k++)
        if (g_copy_dests[k].addr == addr) { g_copy_dests[k].w = w; g_copy_dests[k].h = h; return; }
    g_copy_dests[g_copy_dest_next].addr = addr;
    g_copy_dests[g_copy_dest_next].w = w;
    g_copy_dests[g_copy_dest_next].h = h;
    g_copy_dest_next = (g_copy_dest_next + 1) % COPY_DESTS;
}

static bool copy_dest_is(u32 map) {
    u32 bank = map < 4 ? 0u : 0x20u, i = map & 3u;
    u32 image0 = gx_bp[0x88 + bank + i], image3 = gx_bp[0x94 + bank + i];
    u32 addr = (image3 << 5) & 0x01FFFFE0u;
    u32 w = (image0 & 0x3FFu) + 1, h = ((image0 >> 10) & 0x3FFu) + 1;
    for (int k = 0; k < COPY_DESTS; k++)
        if (g_copy_dests[k].addr == addr && g_copy_dests[k].w == w && g_copy_dests[k].h == h && addr) return true;
    return false;
}

static u32 gpu_tex_copy_for(u32 map) {
    u32 bank = map < 4 ? 0u : 0x20u, i = map & 3u;
    u32 image0 = gx_bp[0x88 + bank + i], image3 = gx_bp[0x94 + bank + i];
    u32 addr = (image3 << 5) & 0x01FFFFE0u;
    u32 w = (image0 & 0x3FFu) + 1, h = ((image0 >> 10) & 0x3FFu) + 1;
    for (int k = 0; k < TEX_COPY_RECORDS; k++) {
        TexCopyRec* r = &g_tex_copy_recs[k];
        if (!r->valid || r->dest != addr || r->w != w || r->h != h)
            continue;
        if (ram_sample_sum(r->dest, r->bytes) != r->sum) {
            r->valid = false;          /* the game wrote something else there since */
            continue;
        }
        return r->id;
    }
    return 0;
}

void gx_raster_copy(u32 cmd) {
    bool to_xfb = (cmd >> 14) & 1u;
    bool clear = (cmd >> 11) & 1u;
    bool active = gx_raster_active();

    if (g_dlog)
        fprintf(g_dlog, "\n-- copy cmd 0x%06X (%s%s) src %u,%u %ux%u dest 0x%08X stride %u yscale 0x%03X clear color 0x%06X%04X z 0x%06X\n",
                cmd, to_xfb ? "to TV buffer" : "to texture", clear ? ", clear" : "", gx_bp[0x49] & 0x3FFu,
                (gx_bp[0x49] >> 10) & 0x3FFu, (gx_bp[0x4A] & 0x3FFu) + 1, ((gx_bp[0x4A] >> 10) & 0x3FFu) + 1,
                (gx_bp[0x4B] & 0x00FFFFFFu) << 5, (gx_bp[0x4D] & 0x3FFu) << 5, gx_bp[0x4E] & 0x1FFu, gx_bp[0x4F],
                gx_bp[0x50] & 0xFFFFu, gx_bp[0x51] & 0xFFFFFFu);

    if (active && !to_xfb) {
        /* Copy to a texture: draw everything recorded so far, then convert
           (before any clear this copy asks for, which is recorded below). */
        submit();
        wait_idle();
        bool depth_copy = (gx_bp[0x43] & 7u) == 3u;
        efb_to_memory(depth_copy);
        TexCopy info;
        u32 dest = 0, bytes = 0;
        bool made = efb_copy_to_texture(cmd, &info, &dest, &bytes);
        if (made) copy_dest_note(dest, info.w, info.h);
        if (made && g_gpu && info.id) {
            /* forget older copies at the same place, then keep this one on the GPU too */
            TexCopyRec* slot = &g_tex_copy_recs[0];
            for (int k = 0; k < TEX_COPY_RECORDS; k++) {
                TexCopyRec* r = &g_tex_copy_recs[k];
                if (r->valid && r->dest < dest + bytes && dest < r->dest + r->bytes) r->valid = false;
            }
            for (int k = 0; k < TEX_COPY_RECORDS; k++)
                if (!g_tex_copy_recs[k].valid) { slot = &g_tex_copy_recs[k]; break; }
            if (slot->valid)           /* all in use: replace the oldest */
                for (int k = 0; k < TEX_COPY_RECORDS; k++)
                    if (g_tex_copy_recs[k].id < slot->id) slot = &g_tex_copy_recs[k];
            info.id = g_tex_copy_next_id++;
            slot->id = info.id; slot->dest = dest; slot->bytes = bytes;
            slot->w = info.w; slot->h = info.h; slot->sum = ram_sample_sum(dest, bytes); slot->valid = true;
            gpu_tex_copy(&info);
        }
    }

    if (active && (to_xfb || clear)) {
        Cmd* c = list_push(&g_lists[g_rec]);
        if (c) {
            CopyCmd* cp = &c->u.copy;
            memset(c, 0, sizeof(*c));
            c->type = CMD_COPY;
            cp->cmd = cmd;
            cp->sx = gx_bp[0x49] & 0x3FFu;
            cp->sy = (gx_bp[0x49] >> 10) & 0x3FFu;
            cp->w = (gx_bp[0x4A] & 0x3FFu) + 1;
            cp->h = ((gx_bp[0x4A] >> 10) & 0x3FFu) + 1;
            cp->to_xfb = to_xfb;
            if (to_xfb)
                setup_xfb_copy(g_cpu, cp);
            cp->clear = clear;
            u32 a = (gx_bp[0x4F] >> 8) & 255u, r = gx_bp[0x4F] & 255u;
            u32 g = (gx_bp[0x50] >> 8) & 255u, b = gx_bp[0x50] & 255u;
            cp->clear_color = (a << 24) | (r << 16) | (g << 8) | b;
            /* Ultrawide menus: the frame cleared to a grey instead of black,
               so the sides the 16:9 backgrounds don't reach aren't black. */
            u32 fill = g_side_fill;
            if (fill && clear && to_xfb && r < 16 && g < 16 && b < 16)
                cp->clear_color = (a << 24) | (fill & 0x00FFFFFFu);
            cp->clear_z = gx_bp[0x51] & 0xFFFFFFu;
        }
    }

    if (to_xfb) {
        if (active) {
            u32 sx = gx_bp[0x49] & 0x3FFu, sy = (gx_bp[0x49] >> 10) & 0x3FFu;
            u32 w = (gx_bp[0x4A] & 0x3FFu) + 1, h = ((gx_bp[0x4A] >> 10) & 0x3FFu) + 1;
            bool save = g_save && g_dumped < MAX_DUMPS;
            if (save) {
                /* The picture is needed before the copy's clear: draw the
                   frame without its final command, save it, then finish. */
                CmdList* l = &g_lists[g_rec];
                Cmd last;
                bool have_last = l->n && l->cmds[l->n - 1].type == CMD_COPY;
                if (have_last)
                    last = l->cmds[--l->n];
                submit();
                wait_idle();
                efb_to_memory(false);
                char path[600];
                snprintf(path, sizeof(path), "%s/frame_%06u.bmp", g_dir, g_frames + 1);
                write_bmp(path, sx, sy, w, h);
                printf("  frame %u saved (game time %.2f s)\n", g_frames + 1, interrupts_game_seconds());
                g_dumped++;
                if (have_last) {
                    Cmd* c = list_push(&g_lists[g_rec]);
                    if (c) *c = last;
                }
            }
            submit();
        }
        dlog_frame_end();
        vote_widescreen();
        gx_state_serial++;          /* textures are re-checked each frame: take a fresh snapshot */
        g_frames++;
        gx_tex_flush_cache();   /* texture data in RAM may change between frames: re-check */
    }
}

/* ------------------------------------------------ TV frame buffer dump --- */
/* What the video interface is actually showing: the external frame buffer
   (XFB) in RAM, YUYV 4:2:2, two bytes per pixel. Movie players and some
   loading screens write it directly without going through the GPU, so these
   dumps catch screens the EFB-copy dumps miss. */
static u32 g_xfb_every, g_xfb_dumped;

void gx_raster_config_xfb(u32 every_fields) {
    g_xfb_every = every_fields;
    if (every_fields)
        make_dir(g_dir);
    if (every_fields && !g_every)
        g_every = 1;     /* render every frame so the TV buffer always has the GPU output */
}

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* Where the frame buffer the TV shows lives and how it is laid out, from the
   VI registers: top-field base (0x201C), picture configuration (0x2048) and
   vertical timing (0x2000, active lines per field). */
static bool xfb_geometry(CPUState* cpu, u32 tfbl, u32 picture_cfg, u32 vtr, u32* addr, u32* width, u32* height,
                         u32* stride) {
    u32 a = (tfbl & 0x10000000u) ? (tfbl & 0x00FFFFFFu) << 5 : (tfbl & 0x00FFFFFFu);
    u32 w = ((picture_cfg >> 8) & 0x7Fu) * 16u;       /* WPL: width in 16-pixel units */
    u32 st = (picture_cfg & 0xFFu) * 32u;             /* STD is per field line */
    if (w == 0 || w > 720)
        w = 640;
    u32 line_bytes = w * 2u;
    /* Interlaced: a field's stride skips every other line, so a frame line is stride / 2. */
    if (st >= line_bytes * 2u)
        st /= 2u;
    if (st < line_bytes)
        st = line_bytes;
    u32 acv = (vtr >> 4) & 0x3FFu;                    /* active lines per field */
    u32 h = (acv >= 100 && acv <= 288) ? acv * 2u : 480u;
    if (a == 0 || a + st * h > cpu->ram_size)
        return false;
    *addr = a; *width = w; *height = h; *stride = st;
    return true;
}

bool gx_raster_xfb_rgb(CPUState* cpu, u32 tfbl, u32 picture_cfg, u32 vtr, u32* out, u32 max_w, u32 max_h,
                       u32* out_w, u32* out_h) {
    u32 addr, width, height, stride;
    wait_idle();                       /* the frame being drawn may be the one shown */
    if (!xfb_geometry(cpu, tfbl, picture_cfg, vtr, &addr, &width, &height, &stride))
        return false;
    if (width > max_w) width = max_w;
    if (height > max_h) height = max_h;
    u32 copied = xfb_copied_lines(addr);
    {
        /* Log the shown picture's layout whenever it changes (for display bugs). */
        static u32 last_w, last_h, last_st, last_copied, logged;
        if ((width != last_w || height != last_h || stride != last_st || copied != last_copied) && logged < 20) {
            printf("  tv picture: %ux%u from 0x%08X, stride %u, game copied %d lines (VI 0x%08X 0x%04X 0x%04X)\n",
                   width, height, addr, stride, copied == ~0u ? -1 : (int)copied, tfbl, picture_cfg, vtr);
            last_w = width; last_h = height; last_st = stride; last_copied = copied;
            logged++;
        }
    }
    for (u32 y = 0; y < height; y++) {
        const u8* src = cpu->ram + addr + y * stride;
        u32* d = out + y * width;
        if (y >= copied || !g_xfb_first_copy) { /* below what the game copied: black, not leftovers */
            for (u32 x = 0; x < width; x++) d[x] = 0xFF000000u;
            continue;
        }
        for (u32 x = 0; x + 1 < width; x += 2) {
            /* Untouched (zeroed) memory, which is what the screen shows
               before the game draws its first picture, decodes as bright
               green. The frame-buffer copy never writes all-zero pixels
               (black is Y 16, U and V 128), so show these as black. */
            if (!(src[x * 2] | src[x * 2 + 1] | src[x * 2 + 2] | src[x * 2 + 3])) {
                d[x] = d[x + 1] = 0xFF000000u;
                continue;
            }
            /* Video-range YUV (black at Y 16, white at 235, as the frame-buffer
               copy writes it and TVs expect), expanded to full-range RGB
               (BT.601). Treating Y as full range made everything look
               grey and washed out. */
            int y0 = (src[x * 2] - 16) * 298, u = src[x * 2 + 1] - 128;
            int y1 = (src[x * 2 + 2] - 16) * 298, v = src[x * 2 + 3] - 128;
            int r = 409 * v + 128, g = -100 * u - 208 * v + 128, b = 516 * u + 128;
            d[x] = 0xFF000000u | ((u32)clamp8((y0 + r) >> 8) << 16) | ((u32)clamp8((y0 + g) >> 8) << 8) |
                   clamp8((y0 + b) >> 8);
            d[x + 1] = 0xFF000000u | ((u32)clamp8((y1 + r) >> 8) << 16) | ((u32)clamp8((y1 + g) >> 8) << 8) |
                       clamp8((y1 + b) >> 8);
        }
    }
    *out_w = width;
    *out_h = height;
    return true;
}

/* Shows the picture the TV shows, from the GPU (the frame copied there at
   the internal resolution) or, when the game wrote the TV buffer itself or
   nothing has been copied, from RAM. Window thread, GPU mode only. */
static u32 g_present_pixels[720 * 576];
static bool g_present_from_gpu;

void gx_raster_present_gpu(CPUState* cpu, u32 tfbl, u32 picture_cfg, u32 vtr, int win_w, int win_h, float aspect,
                           const char* shot_path) {
    wait_idle();
    GlrPicture pic;
    memset(&pic, 0, sizeof(pic));
    u32 addr, w, h, st;
    if (g_xfb_first_copy && xfb_geometry(cpu, tfbl, picture_cfg, vtr, &addr, &w, &h, &st)) {
        XfbCopy* rec = NULL;
        for (int i = 0; i < 4; i++)
            if (g_xfb_copies[i].valid && g_xfb_copies[i].dest == addr) rec = &g_xfb_copies[i];
        if (rec && g_present_from_gpu && xfb_sample_sum(rec->dest, rec->stride, rec->lines) != rec->sum)
            rec->valid = false;        /* written since by the game itself */
        if (rec && rec->valid && glr_xfb_on_gpu(addr)) {
            pic.kind = GLR_PIC_GPU;
            pic.dest = addr;
            pic.tv_w = w;
            pic.tv_h = h;
        } else if (gx_raster_xfb_rgb(cpu, tfbl, picture_cfg, vtr, g_present_pixels, 720, 576, &pic.w, &pic.h)) {
            pic.kind = GLR_PIC_PIXELS;
            pic.pixels = g_present_pixels;
        }
    }
    glr_present(&pic, 0, win_w, win_h, aspect, shot_path);
}

/* A picture from elsewhere (a replacement movie frame) in place of the TV
   picture. Window thread, GPU mode only. */
void gx_raster_present_pixels(const u32* px, u32 w, u32 h, int win_w, int win_h, float aspect) {
    wait_idle();
    GlrPicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.kind = GLR_PIC_PIXELS;
    pic.pixels = px;
    pic.w = w;
    pic.h = h;
    glr_present(&pic, 0, win_w, win_h, aspect, NULL);
}

/* The window shows frames straight from the GPU, so TV-buffer copies needn't
   be read back and written to RAM. */
void gx_raster_present_from_gpu(bool on) {
    g_present_from_gpu = on;
    glr_keep_ram_copies(!on || g_xfb_every);
}

void gx_raster_render_all(void) {
    if (!g_every)
        g_every = 1;
}

void gx_raster_xfb_field(CPUState* cpu, u64 field, u32 tfbl, u32 picture_cfg, u32 vtr) {
    if (!g_xfb_every || g_xfb_dumped >= MAX_DUMPS || field % g_xfb_every)
        return;
    static u32 pixels[720 * 576];
    u32 width, height;
    u32 addr, w0, h0, st0;
    if (!xfb_geometry(cpu, tfbl, picture_cfg, vtr, &addr, &w0, &h0, &st0))
        return;
    if (!gx_raster_xfb_rgb(cpu, tfbl, picture_cfg, vtr, pixels, 720, 576, &width, &height))
        return;

    char path[600];
    snprintf(path, sizeof(path), "%s/xfb_%06llu.bmp", g_dir, (unsigned long long)field);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    u32 row = (width * 3 + 3) & ~3u;
    u32 size = 54 + row * height;
    u8 hdr[54] = {'B', 'M'};
    hdr[2] = (u8)size; hdr[3] = (u8)(size >> 8); hdr[4] = (u8)(size >> 16); hdr[5] = (u8)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (u8)width; hdr[19] = (u8)(width >> 8);
    hdr[22] = (u8)height; hdr[23] = (u8)(height >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)calloc(1, row);
    for (u32 y = 0; y < height; y++) {
        const u32* src = pixels + (height - 1 - y) * width;
        for (u32 x = 0; x < width; x++) {
            line[x * 3 + 0] = (u8)src[x];
            line[x * 3 + 1] = (u8)(src[x] >> 8);
            line[x * 3 + 2] = (u8)(src[x] >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    g_xfb_dumped++;
    printf("  tv frame (field %llu) saved from 0x%08X (game time %.2f s)\n", (unsigned long long)field,
           addr, interrupts_game_seconds());
}

u32 gx_raster_frames(void) {
    return g_frames;
}

void gx_raster_report(void) {
    wait_idle();
    gx_tex_report();
    glr_report();
    unsigned long long drawn = g_tris_recorded;
    printf("renderer: %u frames shown, %u dumped to %s, %llu triangles drawn, %llu clipped, %d drawing threads\n",
           g_frames, g_dumped, g_every ? g_dir : "(off)", drawn, g_tris_skipped, g_workers);
    printf("screen shape: %s (last 3D projection made for %.2f:1)\n",
           g_widescreen == 1 ? "16:9 widescreen" : g_widescreen == 0 ? "4:3" : "not known (no 3D drawn)",
           g_last_aspect);
}

/* ------------------------------------------------- frame-buffer peeks --- */
/* The CPU reading the frame buffer directly (GXPeekARGB / GXPeekZ, at
   0xC8000000: x in bits 2-11, y in bits 12-21, bit 22 = depth). The game does
   this to make a created player's portrait: it draws the head, then reads the
   pixels back one by one. The first read after new drawing draws everything
   recorded so far and takes a snapshot of the frame buffer (read back from
   the graphics card when that draws); later reads come from the snapshot. */
u32 gx_raster_peek(u32 ea) {
    g_peeks++;
    if (!gx_raster_active())
        return 0;
    g_peeks_frame++;
    {
        bool known = false;
        for (u32 i = 0; i < g_nframe_ea; i++) known |= g_frame_ea[i] == ea;
        if (!known) {
            if (g_nframe_ea < PROBE_MAX) g_frame_ea[g_nframe_ea++] = ea;
            else g_frame_ea_over = true;
        }
    }
    int slot = -1;
    for (u32 k = 0; g_probe_on && k < g_nprobe; k++) if (g_probe_ea[k] == ea) slot = (int)k;
    if (slot >= 0 && g_peeks_frame <= 2u * PROBE_MAX) {
        g_peek_probes++;
        u32 c = g_probe_value[slot];
        if (ea & 0x00400000u) return c & 0x00FFFFFFu;
        if ((gx_bp[0x43] & 7u) != 1u) c |= 0xFF000000u;
        return c;
    }
    if (g_peek_stale) {
        submit();
        wait_idle();
        efb_to_memory(true);
        g_peek_stale = false;
        if (g_peek_snapshots++ == 0)
            printf("gx: the game is reading the frame buffer directly (peeks; first at 0x%08X)\n", ea);
    }
    u32 x = (ea >> 2) & 0x3FFu, y = (ea >> 12) & 0x3FFu;
    if (x >= EFB_W || y >= EFB_H)
        return 0;
    u32 i = y * EFB_W + x;
    if (ea & 0x00400000u)
        return g_depth[i] & 0x00FFFFFFu;
    u32 c = g_color[i];
    if ((gx_bp[0x43] & 7u) != 1u)      /* no alpha in the frame buffer's format: reads as opaque */
        c |= 0xFF000000u;
    return c;
}

void gx_raster_peek_report(void) {
    if (g_peeks)
        printf("gx: %llu frame-buffer peeks (%llu snapshots, %llu answered from the last frame)\n", g_peeks,
               g_peek_snapshots, g_peek_probes);
}
