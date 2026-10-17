#include "gx/raster.h"
#include "gx/state.h"
#include "gx/texture.h"
#include "runtime/interrupts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif

#define EFB_W 640
#define EFB_H 528
#define MAX_DUMPS 300

static u32 g_color[EFB_W * EFB_H];   /* 0xAARRGGBB */
static u32 g_depth[EFB_W * EFB_H];   /* 24-bit, larger = farther */

static char g_dir[512] = "frames";
static u32 g_every;          /* render every Nth displayed frame */
static bool g_save;          /* and save it as frame_N.bmp */
static u32 g_frames;        /* frames copied to the display so far */
static u32 g_dumped;
static unsigned long long g_tris_skipped, g_tris_recorded;
static CPUState* g_cpu;     /* for texture fetches during rasterisation */

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
typedef struct {
    float pos[3];
    u32 pos_mtx;                 /* row index into transform memory */
    u32 tex_mtx[8];
    u32 color[2];                /* 0xRRGGBBAA */
    bool has_color[2];
    float tc[8][2];              /* texture coordinates as given */
    bool has_tc[8];
} Vtx;

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

/* Decodes one vertex; returns bytes consumed. Must agree with vertex_size()
   in fifo.c. */
static u32 decode_vertex(CPUState* cpu, u32 vat, const u8* p, Vtx* v) {
    u32 lo = gx_cp[0x50], hi = gx_cp[0x60];
    u32 a = gx_cp[0x70 + vat], b = gx_cp[0x80 + vat], c = gx_cp[0x90 + vat];
    const u8* start = p;
    u32 mia = gx_xf_reg[0x18], mib = gx_xf_reg[0x19];

    memset(v, 0, sizeof(*v));
    v->pos_mtx = mia & 0x3Fu;
    for (u32 i = 0; i < 4; i++) v->tex_mtx[i] = (mia >> (6 + 6 * i)) & 0x3Fu;
    for (u32 i = 0; i < 4; i++) v->tex_mtx[4 + i] = (mib >> (6 * i)) & 0x3Fu;

    if (lo & 1u) v->pos_mtx = *p++ & 0x3Fu;
    for (u32 i = 0; i < 8; i++)
        if ((lo >> (1 + i)) & 1u) v->tex_mtx[i] = *p++ & 0x3Fu;

    u32 pos = (lo >> 9) & 3u;
    if (pos) {
        u32 n = (a & 1u) ? 3u : 2u, fmt = (a >> 1) & 7u, shift = (a >> 4) & 31u, cs = comp_size(fmt);
        const u8* d = attr_data(cpu, &p, pos, 0, n * cs);
        if (d)
            for (u32 k = 0; k < n; k++)
                v->pos[k] = read_comp(d + k * cs, fmt, shift);
    }

    u32 nrm = (lo >> 11) & 3u;
    if (nrm) {   /* normals: stepped over (lighting is approximated) */
        bool nbt = (a >> 9) & 1u, index3 = (a >> 31) & 1u;
        if (nrm == 1)
            p += (nbt ? 9u : 3u) * comp_size((a >> 10) & 7u);
        else
            p += ((nbt && index3) ? 3u : 1u) * (nrm == 2 ? 1u : 2u);
    }

    for (u32 ch = 0; ch < 2; ch++) {
        u32 t = (lo >> (13 + ch * 2)) & 3u;
        u32 fmt = (a >> (14 + ch * 4)) & 7u;
        if (!t)
            continue;
        const u8* d = attr_data(cpu, &p, t, 2 + ch, color_size(fmt));
        if (d) {
            v->color[ch] = read_color(d, fmt);
            v->has_color[ch] = true;
        }
    }

    const u32 tc_cnt[8] = {(a >> 21) & 1u, b & 1u, (b >> 9) & 1u, (b >> 18) & 1u,
                           (b >> 27) & 1u, (c >> 5) & 1u, (c >> 14) & 1u, (c >> 23) & 1u};
    const u32 tc_fmt[8] = {(a >> 22) & 7u, (b >> 1) & 7u, (b >> 10) & 7u, (b >> 19) & 7u,
                           (b >> 28) & 7u, (c >> 6) & 7u, (c >> 15) & 7u, (c >> 24) & 7u};
    const u32 tc_shift[8] = {(a >> 25) & 31u, (b >> 4) & 31u, (b >> 13) & 31u, (b >> 22) & 31u,
                             c & 31u, (c >> 9) & 31u, (c >> 18) & 31u, (c >> 27) & 31u};
    for (u32 i = 0; i < 8; i++) {
        u32 t = (hi >> (i * 2)) & 3u;
        if (!t)
            continue;
        u32 n = tc_cnt[i] ? 2u : 1u, cs = comp_size(tc_fmt[i]);
        const u8* d = attr_data(cpu, &p, t, 4 + i, n * cs);
        if (d) {
            v->tc[i][0] = read_comp(d, tc_fmt[i], tc_shift[i]);
            v->tc[i][1] = n == 2 ? read_comp(d + cs, tc_fmt[i], tc_shift[i]) : 0.0f;
            v->has_tc[i] = true;
        }
    }
    return (u32)(p - start);
}

/* ---------------------------------------------------------- transform --- */
#define MAX_TC 8

typedef struct {
    float x, y, z;              /* screen x, y; depth 0..16777215 */
    float inv_w;                /* 1/w, for perspective-correct interpolation */
    float col[2][4];            /* colour channels 0/1, RGBA 0..1 */
    float tc[MAX_TC][3];        /* s, t, q (q = 1 unless projected) */
    bool valid;
} SVtx;

static float clamp01(float v);

static void unpack(u32 rgba, float out[4]) {
    out[0] = (float)(rgba >> 24) / 255.0f;
    out[1] = (float)((rgba >> 16) & 255) / 255.0f;
    out[2] = (float)((rgba >> 8) & 255) / 255.0f;
    out[3] = (float)(rgba & 255) / 255.0f;
}

/* Lighting channel: material colour (register or vertex), optionally lit.
   Lights aren't computed yet: a lit channel gets ambient plus a fixed
   diffuse term, so lit models are visible without being exactly shaded. */
static void channel(const Vtx* v, u32 ch, float out[4]) {
    u32 cctrl = gx_xf_reg[0x0E + ch], actrl = gx_xf_reg[0x10 + ch];
    float mat_c[4], mat_a[4], amb[4], vc[4];
    unpack(gx_xf_reg[0x0C + ch], mat_c);
    unpack(gx_xf_reg[0x0C + ch], mat_a);
    unpack(gx_xf_reg[0x0A + ch], amb);
    unpack(v->has_color[ch] ? v->color[ch] : 0xFFFFFFFFu, vc);
    if (cctrl & 1u) memcpy(mat_c, vc, sizeof(vc));
    if (actrl & 1u) mat_a[3] = vc[3];

    for (u32 k = 0; k < 3; k++) {
        float m = mat_c[k];
        if (cctrl & 2u) {
            float am = (cctrl & 0x40u) ? vc[k] : amb[k];
            float lit = (cctrl & 0x783Cu) ? 0.75f : 0.0f;   /* any of lights 0-7 enabled */
            m *= clamp01(am + lit);
        }
        out[k] = m;
    }
    float am = mat_a[3];
    if (actrl & 2u) {
        float aa = (actrl & 0x40u) ? vc[3] : amb[3];
        am *= clamp01(aa + 0.75f);
    }
    out[3] = am;
}

/* Texture coordinate generation (XF 0x1040+i): regular texgens multiply the
   chosen input (position or a texture coordinate) by a texture matrix. */
static void texgen(const Vtx* v, u32 i, float out[3]) {
    u32 ctl = gx_xf_reg[0x40 + i];
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

    const u32* m = &gx_xf_mem[(v->tex_mtx[i] * 4u) & 0x3FFu];
    float s = gx_f32(m[0]) * in[0] + gx_f32(m[1]) * in[1] + gx_f32(m[2]) * in[2] + gx_f32(m[3]) * in[3];
    float t = gx_f32(m[4]) * in[0] + gx_f32(m[5]) * in[1] + gx_f32(m[6]) * in[2] + gx_f32(m[7]) * in[3];
    float q = proj ? gx_f32(m[8]) * in[0] + gx_f32(m[9]) * in[1] + gx_f32(m[10]) * in[2] + gx_f32(m[11]) * in[3]
                   : 1.0f;

    /* Optional second ("dual") texture transform. */
    if (gx_xf_reg[0x12] & 1u) {
        u32 post = gx_xf_reg[0x50 + i];
        float vin[3] = {s, t, q};
        if ((post >> 8) & 1u) {
            float len = sqrtf(vin[0] * vin[0] + vin[1] * vin[1] + vin[2] * vin[2]);
            if (len > 1e-9f) { vin[0] /= len; vin[1] /= len; vin[2] /= len; }
        }
        const u32* pm = &gx_xf_mem[0x500u + ((post & 0x3Fu) * 4u)];
        s = gx_f32(pm[0]) * vin[0] + gx_f32(pm[1]) * vin[1] + gx_f32(pm[2]) * vin[2] + gx_f32(pm[3]);
        t = gx_f32(pm[4]) * vin[0] + gx_f32(pm[5]) * vin[1] + gx_f32(pm[6]) * vin[2] + gx_f32(pm[7]);
        q = gx_f32(pm[8]) * vin[0] + gx_f32(pm[9]) * vin[1] + gx_f32(pm[10]) * vin[2] + gx_f32(pm[11]);
    }
    out[0] = s; out[1] = t; out[2] = q;
}

static void transform(const Vtx* in, SVtx* out) {
    const u32* m = &gx_xf_mem[(in->pos_mtx * 4u) & 0x3FFu];
    float ex = gx_f32(m[0]) * in->pos[0] + gx_f32(m[1]) * in->pos[1] + gx_f32(m[2]) * in->pos[2] + gx_f32(m[3]);
    float ey = gx_f32(m[4]) * in->pos[0] + gx_f32(m[5]) * in->pos[1] + gx_f32(m[6]) * in->pos[2] + gx_f32(m[7]);
    float ez = gx_f32(m[8]) * in->pos[0] + gx_f32(m[9]) * in->pos[1] + gx_f32(m[10]) * in->pos[2] + gx_f32(m[11]);

    const u32* pr = &gx_xf_reg[0x20];
    float cx, cy, cz, cw;
    if (gx_xf_reg[0x26] == 0) {
        cx = gx_f32(pr[0]) * ex + gx_f32(pr[1]) * ez;
        cy = gx_f32(pr[2]) * ey + gx_f32(pr[3]) * ez;
        cz = gx_f32(pr[4]) * ez + gx_f32(pr[5]);
        cw = -ez;
    } else {
        cx = gx_f32(pr[0]) * ex + gx_f32(pr[1]);
        cy = gx_f32(pr[2]) * ey + gx_f32(pr[3]);
        cz = gx_f32(pr[4]) * ez + gx_f32(pr[5]);
        cw = 1.0f;
    }
    out->valid = cw > 1e-6f;
    if (!out->valid)
        return;
    out->inv_w = 1.0f / cw;
    const u32* vp = &gx_xf_reg[0x1A];
    out->x = cx * out->inv_w * gx_f32(vp[0]) + gx_f32(vp[3]) - 342.0f;
    out->y = cy * out->inv_w * gx_f32(vp[1]) + gx_f32(vp[4]) - 342.0f;
    out->z = cz * out->inv_w * gx_f32(vp[2]) + gx_f32(vp[5]);

    channel(in, 0, out->col[0]);
    channel(in, 1, out->col[1]);
    u32 ntex = gx_xf_reg[0x3F] & 15u;
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
enum { ROW_PREV = 0, ROW_TEX = 4, ROW_RAS = 5, ROW_ONE = 6, ROW_HALF = 7, ROW_ZERO = 8, ROW_KONST = 9,
       ROW_COUNT = ROW_KONST + 32 };
static float g_u8f[256];               /* i / 255 */

typedef struct {
    u8 ci[4];                          /* colour input rows a, b, c, d */
    bool cbc[4];                       /* input is an alpha broadcast to RGB */
    u8 ai[4];                          /* alpha input rows a, b, c, d (A component) */
    u8 cdst, adst;
    bool csub, asub, cclamp, aclamp, ccmp, acmp;
    float cbias, abias, cscale, ascale;
    bool tex_on;
    u8 texcoord, chan;
    u8 tsw[4], rsw[4];
    const u32* texels;                 /* decoded texture; stays valid until the frame is drawn */
    u32 tw, th, wrap_s, wrap_t;
} Stage;

enum { FAST_NONE, FAST_MODULATE, FAST_REPLACE, FAST_PASSCLR };

typedef struct {
    u32 stages;
    u32 fast;                          /* one-stage standard modes (GXSetTevOp) */
    Stage st[16];
    float reg_init[4][4];
    float konst[32][4];
    u32 at_ref0, at_ref1, at_f0, at_f1, at_logic;
    bool at_always;
    /* rasteriser and pixel engine */
    int sx0, sy0, sx1, sy1;            /* scissor, inclusive */
    bool ztest, zupdate, blend, blend_alpha, subtract, color_update, alpha_update;
    u32 zfunc, sfac, dfac, ntex;
} RenderState;

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
static void snapshot_state(RenderState* ds) {
    memset(ds, 0, sizeof(*ds));
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

        if (s->tex_on) {
            const GxTexture* tex = gx_tex_get(g_cpu, texmap);
            if (tex) {
                u32 mode0 = gx_bp[(texmap < 4 ? 0x80u : 0xA0u) + (texmap & 3u)];
                s->texels = tex->texels;
                s->tw = tex->width;
                s->th = tex->height;
                s->wrap_s = mode0 & 3u;
                s->wrap_t = (mode0 >> 2) & 3u;
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
}

/* ---------------------------------------------------------------- TEV --- */
static inline int wrap_index(float c, u32 size, u32 mode) {
    float f = c * (float)size;
    int i = (int)f;
    if (f < (float)i) i--;             /* floor without a libm call */
    int n = (int)size;
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
        if ((size & (size - 1)) == 0)
            return i & (n - 1);
        i %= n;
        return i < 0 ? i + n : i;
    }
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

static inline void sample_stage(const Stage* s, const float tc[][2], float out[4]) {
    if (!s->texels) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    int x = wrap_index(tc[s->texcoord][0], s->tw, s->wrap_s);
    int y = wrap_index(tc[s->texcoord][1], s->th, s->wrap_t);
    u32 c = s->texels[(u32)y * s->tw + (u32)x];
    out[0] = g_u8f[(c >> 16) & 255]; out[1] = g_u8f[(c >> 8) & 255];
    out[2] = g_u8f[c & 255]; out[3] = g_u8f[c >> 24];
}

/* Runs the TEV stages for one pixel. Returns false if the alpha test
   discards it. rows: this thread's input table, konst rows already filled. */
static bool tev_pixel(const RenderState* ds, float (*rows)[4], const float col[2][4], const float tc[][2], float out[4]) {
    if (ds->fast != FAST_NONE) {
        const Stage* s = &ds->st[0];
        switch (ds->fast) {
        case FAST_MODULATE: {
            float t[4];
            sample_stage(s, tc, t);
            const float* c = col[s->chan];
            for (int k = 0; k < 4; k++) out[k] = t[k] * clamp01(c[k]);
            break;
        }
        case FAST_REPLACE:
            sample_stage(s, tc, out);
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
            sample_stage(s, tc, raw);
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
            if (!tev_pixel(ds, rows, col, tc, out))
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
typedef struct {
    u32 cmd, sx, sy, w, h;             /* copy command and source rectangle */
    u32 dest, stride, lines;           /* to_xfb: destination in RAM */
    float yscale;
    bool to_xfb, clear;
    u32 clear_color, clear_z;
} CopyCmd;

typedef struct {
    u32 type;                          /* CMD_TRI or CMD_COPY */
    u32 state;                         /* RenderState index (triangles) */
    union {
        SVtx v[3];
        CopyCmd copy;
    } u;
} Cmd;

enum { CMD_TRI, CMD_COPY };

typedef struct {
    Cmd* cmds;
    u32 n, cap;
    RenderState* states;
    u32 ns, cap_s;
} CmdList;

static CmdList g_lists[2];
static u32 g_rec;                      /* list being recorded */

static Cmd* list_push(CmdList* l) {
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

static u32 list_state(CmdList* l) {
    if (l->ns == l->cap_s) {
        u32 cap = l->cap_s ? l->cap_s * 2 : 256;
        RenderState* s = (RenderState*)realloc(l->states, sizeof(RenderState) * cap);
        if (!s)
            return 0;
        l->states = s;
        l->cap_s = cap;
    }
    RenderState tmp;
    snapshot_state(&tmp);
    /* Consecutive draws often share state: reuse the previous snapshot. */
    if (l->ns && memcmp(&l->states[l->ns - 1], &tmp, sizeof(tmp)) == 0)
        return l->ns - 1;
    l->states[l->ns] = tmp;
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
            draw_triangle(ds, rows, &c->u.v[0], &c->u.v[1], &c->u.v[2], y0, y1);
            continue;
        }
        const CopyCmd* cp = &c->u.copy;
        if (cp->to_xfb && cp->lines && g_cpu) {
            /* Each destination line is written by the worker that owns its
               source line, so no thread reads rows another is still drawing. */
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
        gx_mutex_unlock(&g_mx);

        int y0, y1;
        band(who, count, &y0, &y1);
        run_list(l, y0, y1);

        gx_mutex_lock(&g_mx);
        if (--g_busy == 0)
            gx_cond_broadcast(&g_cv_done);
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
    printf("renderer: %d drawing thread%s\n", g_workers, g_workers == 1 ? "" : "s");
}

/* Waits until the workers have finished the list they are drawing. */
static void wait_idle(void) {
    if (!g_workers)
        return;
    gx_mutex_lock(&g_mx);
    while (g_busy > 0)
        gx_cond_wait(&g_cv_done, &g_mx);
    gx_mutex_unlock(&g_mx);
}

/* Ends the recorded frame: hands its list to the workers (or draws it here
   when there are none) and starts recording into the other list. */
static void submit(void) {
    start_workers();
    wait_idle();                       /* the other list is free again */
    gx_tex_collect_garbage();          /* texture memory the finished frame used */
    CmdList* l = &g_lists[g_rec];
    if (!g_workers) {
        run_list(l, 0, EFB_H - 1);
    } else {
        gx_mutex_lock(&g_mx);
        g_drawing = l;
        g_busy = g_workers;
        g_generation++;
        gx_cond_broadcast(&g_cv_work);
        gx_mutex_unlock(&g_mx);
    }
    g_rec ^= 1u;
    g_lists[g_rec].n = 0;
    g_lists[g_rec].ns = 0;
}

void gx_raster_set_threads(int n) {
    g_workers_wanted = n;
}

void gx_raster_sync(void) {
    wait_idle();
}

/* ------------------------------------------------------------ record --- */
void gx_raster_draw(CPUState* cpu, u8 prim, u32 vat, u32 count, const u8* data) {
    if (!gx_raster_active() || count == 0)
        return;
    if ((gx_bp[0x00] >> 14 & 3u) == 3u)
        return;                        /* cull everything */
    g_cpu = cpu;
    if (g_u8f[255] == 0.0f)
        for (int i = 0; i < 256; i++) g_u8f[i] = (float)i / 255.0f;
    CmdList* l = &g_lists[g_rec];
    u32 state = list_state(l);

    SVtx* sv = (SVtx*)malloc(sizeof(SVtx) * count);
    if (!sv)
        return;
    const u8* p = data;
    for (u32 i = 0; i < count; i++) {
        Vtx v;
        p += decode_vertex(cpu, vat, p, &v);
        transform(&v, &sv[i]);
    }

#define EMIT(i0, i1, i2) do {                                                        \
        const SVtx *a_ = &sv[i0], *b_ = &sv[i1], *c_ = &sv[i2];                      \
        if (!a_->valid || !b_->valid || !c_->valid) { g_tris_skipped++; break; }     \
        Cmd* cmd_ = list_push(l);                                                    \
        if (!cmd_) break;                                                            \
        cmd_->type = CMD_TRI; cmd_->state = state;                                   \
        cmd_->u.v[0] = *a_; cmd_->u.v[1] = *b_; cmd_->u.v[2] = *c_;                  \
        g_tris_recorded++;                                                           \
    } while (0)

    switch (prim & 0xF8) {
    case 0x80:
    case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4) {
            EMIT(i, i + 1, i + 2);
            EMIT(i, i + 2, i + 3);
        }
        break;
    case 0x90:
        for (u32 i = 0; i + 2 < count; i += 3)
            EMIT(i, i + 1, i + 2);
        break;
    case 0x98:
        for (u32 i = 0; i + 2 < count; i++)
            EMIT(i, i + 1, i + 2);
        break;
    case 0xA0:
        for (u32 i = 1; i + 1 < count; i++)
            EMIT(0, i, i + 1);
        break;
    default:
        break;
    }
#undef EMIT
    free(sv);
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

static void setup_xfb_copy(CPUState* cpu, CopyCmd* cp) {
    u32 dest = (gx_bp[0x4B] & 0x00FFFFFFu) << 5;
    u32 stride = (gx_bp[0x4D] & 0x3FFu) << 5;
    u32 yscale_reg = gx_bp[0x4E] & 0x1FFu;
    bool invert = (cp->cmd >> 10) & 1u;
    double yscale = 1.0;
    if (yscale_reg)
        yscale = invert ? 256.0 / yscale_reg : yscale_reg / 256.0;
    u32 lines = (u32)(cp->h * yscale + 0.5);
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
    cp->yscale = (float)yscale;
}

void gx_raster_copy(u32 cmd) {
    bool to_xfb = (cmd >> 14) & 1u;
    bool clear = (cmd >> 11) & 1u;
    bool active = gx_raster_active();

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
    for (u32 y = 0; y < height; y++) {
        const u8* src = cpu->ram + addr + y * stride;
        u32* d = out + y * width;
        for (u32 x = 0; x + 1 < width; x += 2) {
            int y0 = src[x * 2], u = src[x * 2 + 1] - 128, y1 = src[x * 2 + 2], v = src[x * 2 + 3] - 128;
            int r = (int)(1.371f * v), g = (int)(-0.698f * v - 0.336f * u), b = (int)(1.732f * u);
            d[x] = 0xFF000000u | ((u32)clamp8(y0 + r) << 16) | ((u32)clamp8(y0 + g) << 8) | clamp8(y0 + b);
            d[x + 1] = 0xFF000000u | ((u32)clamp8(y1 + r) << 16) | ((u32)clamp8(y1 + g) << 8) | clamp8(y1 + b);
        }
    }
    *out_w = width;
    *out_h = height;
    return true;
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
    unsigned long long drawn = g_tris_recorded;
    printf("renderer: %u frames shown, %u dumped to %s, %llu triangles drawn, %llu clipped, %d drawing threads\n",
           g_frames, g_dumped, g_every ? g_dir : "(off)", drawn, g_tris_skipped, g_workers);
}
