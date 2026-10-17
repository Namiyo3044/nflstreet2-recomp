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
static unsigned long long g_tris_drawn, g_tris_skipped;
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

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

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

/* ---------------------------------------------------------------- TEV --- */
typedef struct {
    u32 stages;
    u32 color_op[16], alpha_op[16];
    u32 texmap[16], texcoord[16], chan[16];
    bool tex_on[16];
    u32 kcsel[16], kasel[16];
    u32 swap[4][4];              /* swap tables: source channel for R, G, B, A */
    const GxTexture* tex[16];
    u32 alpha_test;
} TevSetup;

static TevSetup g_tev;

static void tev_prepare(void) {
    g_tev.stages = ((gx_bp[0x00] >> 10) & 15u) + 1;
    for (u32 i = 0; i < g_tev.stages; i++) {
        g_tev.color_op[i] = gx_bp[0xC0 + i * 2];
        g_tev.alpha_op[i] = gx_bp[0xC1 + i * 2];
        u32 order = gx_bp[0x28 + i / 2] >> ((i & 1) ? 12 : 0);
        g_tev.texmap[i] = order & 7u;
        g_tev.texcoord[i] = (order >> 3) & 7u;
        g_tev.tex_on[i] = (order >> 6) & 1u;
        g_tev.chan[i] = (order >> 7) & 7u;
        u32 ksel = gx_bp[0xF6 + i / 2];
        g_tev.kcsel[i] = (ksel >> ((i & 1) ? 14 : 4)) & 31u;
        g_tev.kasel[i] = (ksel >> ((i & 1) ? 19 : 9)) & 31u;
        g_tev.tex[i] = g_tev.tex_on[i] ? gx_tex_get(g_cpu, g_tev.texmap[i]) : NULL;
    }
    for (u32 t = 0; t < 4; t++) {
        u32 k0 = gx_bp[0xF6 + t * 2], k1 = gx_bp[0xF7 + t * 2];
        g_tev.swap[t][0] = k0 & 3u;
        g_tev.swap[t][1] = (k0 >> 2) & 3u;
        g_tev.swap[t][2] = k1 & 3u;
        g_tev.swap[t][3] = (k1 >> 2) & 3u;
    }
    g_tev.alpha_test = gx_bp[0xF3];
}

static float wrap_coord(float c, u32 size, u32 mode) {
    float f = c * (float)size;
    int i = (int)floorf(f);
    int n = (int)size;
    switch (mode) {
    case 0:  /* clamp */
        if (i < 0) i = 0;
        if (i >= n) i = n - 1;
        break;
    case 2: { /* mirror */
        int period = 2 * n;
        int m = ((i % period) + period) % period;
        i = m < n ? m : period - 1 - m;
        break;
    }
    default: /* repeat */
        i = ((i % n) + n) % n;
        break;
    }
    return (float)i;
}

static void sample(u32 stage, float s, float t, float out[4]) {
    const GxTexture* tex = g_tev.tex[stage];
    if (!tex) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    u32 map = g_tev.texmap[stage];
    u32 mode0 = gx_bp[(map < 4 ? 0x80u : 0xA0u) + (map & 3u)];
    u32 x = (u32)wrap_coord(s, tex->width, mode0 & 3u);
    u32 y = (u32)wrap_coord(t, tex->height, (mode0 >> 2) & 3u);
    u32 c = tex->texels[y * tex->width + x];
    out[0] = (float)((c >> 16) & 255) / 255.0f;
    out[1] = (float)((c >> 8) & 255) / 255.0f;
    out[2] = (float)(c & 255) / 255.0f;
    out[3] = (float)(c >> 24) / 255.0f;
}

static void konst(u32 sel, bool alpha, float out[4]) {
    static const float frac[8] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
    if (sel < 8) {
        out[0] = out[1] = out[2] = out[3] = frac[sel];
        return;
    }
    u32 k = sel & 3u;
    const u8* kc = gx_tev_konst[k];
    float rgba[4] = {kc[0] / 255.0f, kc[1] / 255.0f, kc[2] / 255.0f, kc[3] / 255.0f};
    if (sel >= 0x0C && sel <= 0x0F && !alpha) {
        memcpy(out, rgba, sizeof(rgba));
        return;
    }
    u32 comp = sel >= 0x1C ? 3 : sel >= 0x18 ? 2 : sel >= 0x14 ? 1 : 0;
    if (sel >= 0x0C && sel <= 0x0F) comp = 3;
    out[0] = out[1] = out[2] = out[3] = rgba[comp];
}

static float tev_math(float a, float b, float c, float d, u32 op) {
    u32 bias = (op >> 16) & 3u, sub = (op >> 18) & 1u, clamp = (op >> 19) & 1u, scale = (op >> 20) & 3u;
    if (bias == 3)               /* comparison modes: not modelled, pass d through */
        return d;
    a = clamp01(a); b = clamp01(b); c = clamp01(c);
    float lerp = a * (1.0f - c) + b * c;
    float r = sub ? d - lerp : d + lerp;
    r += bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0.0f;
    r *= scale == 1 ? 2.0f : scale == 2 ? 4.0f : scale == 3 ? 0.5f : 1.0f;
    if (clamp)
        return clamp01(r);
    return r < -4.0f ? -4.0f : r > 4.0f ? 4.0f : r;
}

static bool alpha_compare(u32 func, float a, u32 ref) {
    u32 v = (u32)(clamp01(a) * 255.0f + 0.5f);
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

/* Runs the TEV stages for one pixel. Returns false if the alpha test
   discards it. */
static bool tev_pixel(float col[2][4], float tc[MAX_TC][2], float out[4]) {
    float reg[4][4];
    for (u32 r = 0; r < 4; r++)
        for (u32 k = 0; k < 4; k++)
            reg[r][k] = (float)gx_tev_reg[r][k] / 255.0f;

    for (u32 st = 0; st < g_tev.stages; st++) {
        float tex[4] = {0, 0, 0, 0}, ras[4] = {0, 0, 0, 0}, kc[4], ka[4];
        if (g_tev.tex_on[st]) {
            float raw[4];
            u32 tci = g_tev.texcoord[st];
            sample(st, tc[tci][0], tc[tci][1], raw);
            u32 tsw = (g_tev.alpha_op[st] >> 2) & 3u;
            for (u32 k = 0; k < 4; k++) tex[k] = raw[g_tev.swap[tsw][k]];
        }
        u32 ch = g_tev.chan[st];
        if (ch <= 1) {
            u32 rsw = g_tev.alpha_op[st] & 3u;
            for (u32 k = 0; k < 4; k++) ras[k] = col[ch][g_tev.swap[rsw][k]];
        }
        konst(g_tev.kcsel[st], false, kc);
        konst(g_tev.kasel[st], true, ka);

        u32 cop = g_tev.color_op[st];
        u32 csel[4] = {(cop >> 12) & 15u, (cop >> 8) & 15u, (cop >> 4) & 15u, cop & 15u}; /* a b c d */
        float cin[4][3];
        for (u32 j = 0; j < 4; j++) {
            for (u32 k = 0; k < 3; k++) {
                float vsrc;
                switch (csel[j]) {
                case 0: vsrc = reg[0][k]; break;
                case 1: vsrc = reg[0][3]; break;
                case 2: vsrc = reg[1][k]; break;
                case 3: vsrc = reg[1][3]; break;
                case 4: vsrc = reg[2][k]; break;
                case 5: vsrc = reg[2][3]; break;
                case 6: vsrc = reg[3][k]; break;
                case 7: vsrc = reg[3][3]; break;
                case 8: vsrc = tex[k]; break;
                case 9: vsrc = tex[3]; break;
                case 10: vsrc = ras[k]; break;
                case 11: vsrc = ras[3]; break;
                case 12: vsrc = 1.0f; break;
                case 13: vsrc = 0.5f; break;
                case 14: vsrc = kc[k]; break;
                default: vsrc = 0.0f; break;
                }
                cin[j][k] = vsrc;
            }
        }
        u32 aop = g_tev.alpha_op[st];
        u32 asel[4] = {(aop >> 13) & 7u, (aop >> 10) & 7u, (aop >> 7) & 7u, (aop >> 4) & 7u};
        float ain[4];
        for (u32 j = 0; j < 4; j++) {
            switch (asel[j]) {
            case 0: ain[j] = reg[0][3]; break;
            case 1: ain[j] = reg[1][3]; break;
            case 2: ain[j] = reg[2][3]; break;
            case 3: ain[j] = reg[3][3]; break;
            case 4: ain[j] = tex[3]; break;
            case 5: ain[j] = ras[3]; break;
            case 6: ain[j] = ka[3]; break;
            default: ain[j] = 0.0f; break;
            }
        }
        u32 cdst = (cop >> 22) & 3u, adst = (aop >> 22) & 3u;
        float res[3];
        for (u32 k = 0; k < 3; k++)
            res[k] = tev_math(cin[0][k], cin[1][k], cin[2][k], cin[3][k], cop);
        float ares = tev_math(ain[0], ain[1], ain[2], ain[3], aop);
        for (u32 k = 0; k < 3; k++) reg[cdst][k] = res[k];
        reg[adst][3] = ares;
    }

    /* Output is the PREV register after the last stage. */
    for (u32 k = 0; k < 4; k++) out[k] = clamp01(reg[0][k]);

    u32 at = g_tev.alpha_test;
    u32 ref0 = at & 255u, ref1 = (at >> 8) & 255u, f0 = (at >> 16) & 7u, f1 = (at >> 19) & 7u, logic = (at >> 22) & 3u;
    if (at == 0)
        return true;   /* not configured yet */
    bool r0 = alpha_compare(f0, out[3], ref0), r1 = alpha_compare(f1, out[3], ref1);
    switch (logic) {
    case 0: return r0 && r1;
    case 1: return r0 || r1;
    case 2: return r0 != r1;
    default: return r0 == r1;
    }
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

static void draw_triangle(const SVtx* a, const SVtx* b, const SVtx* c) {
    if (!a->valid || !b->valid || !c->valid) { g_tris_skipped++; return; }
    if ((gx_bp[0x00] >> 14 & 3u) == 3u) return;

    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-6f || !isfinite(area)) return;

    int sx0, sy0, sx1, sy1;
    scissor(&sx0, &sy0, &sx1, &sy1);
    float fx0 = fminf(a->x, fminf(b->x, c->x)), fx1 = fmaxf(a->x, fmaxf(b->x, c->x));
    float fy0 = fminf(a->y, fminf(b->y, c->y)), fy1 = fmaxf(a->y, fmaxf(b->y, c->y));
    if (fx1 < sx0 || fy1 < sy0 || fx0 > sx1 + 1 || fy0 > sy1 + 1) return;
    int x0 = fx0 < sx0 ? sx0 : (int)fx0, x1 = fx1 > sx1 ? sx1 : (int)fx1;
    int y0 = fy0 < sy0 ? sy0 : (int)fy0, y1 = fy1 > sy1 ? sy1 : (int)fy1;

    u32 zmode = gx_bp[0x40], cmode = gx_bp[0x41];
    bool ztest = zmode & 1u, zupdate = (zmode >> 4) & 1u;
    u32 zfunc = (zmode >> 1) & 7u;
    bool blend = cmode & 1u;
    bool color_update = (cmode >> 3) & 1u, alpha_update = (cmode >> 4) & 1u;
    u32 dfac = (cmode >> 5) & 7u, sfac = (cmode >> 8) & 7u;
    bool subtract = (cmode >> 11) & 1u;
    if (!(cmode & 0x18u))
        color_update = true;

    u32 ntex = gx_xf_reg[0x3F] & 15u;
    if (ntex > MAX_TC) ntex = MAX_TC;
    float inv = 1.0f / area;
    g_tris_drawn++;

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;

            float zf = w0 * a->z + w1 * b->z + w2 * c->z;
            u32 z = zf <= 0 ? 0u : zf >= 16777215.0f ? 16777215u : (u32)zf;
            size_t i = (size_t)y * EFB_W + (size_t)x;
            if (ztest && !depth_pass(zfunc, z, g_depth[i]))
                continue;

            /* Perspective-correct weights for colours and texture coordinates. */
            float p0 = w0 * a->inv_w, p1 = w1 * b->inv_w, p2 = w2 * c->inv_w;
            float ps = p0 + p1 + p2;
            if (ps <= 0) continue;
            p0 /= ps; p1 /= ps; p2 /= ps;

            float col[2][4];
            for (u32 ch = 0; ch < 2; ch++)
                for (u32 k = 0; k < 4; k++)
                    col[ch][k] = p0 * a->col[ch][k] + p1 * b->col[ch][k] + p2 * c->col[ch][k];
            float tc[MAX_TC][2];
            for (u32 t = 0; t < ntex; t++) {
                float s = p0 * a->tc[t][0] + p1 * b->tc[t][0] + p2 * c->tc[t][0];
                float tt = p0 * a->tc[t][1] + p1 * b->tc[t][1] + p2 * c->tc[t][1];
                float q = p0 * a->tc[t][2] + p1 * b->tc[t][2] + p2 * c->tc[t][2];
                if (fabsf(q) > 1e-9f) { s /= q; tt /= q; }
                tc[t][0] = s; tc[t][1] = tt;
            }
            for (u32 t = ntex; t < MAX_TC; t++) tc[t][0] = tc[t][1] = 0.0f;

            float out[4];
            if (!tev_pixel(col, tc, out))
                continue;
            float r = out[0] * 255.0f, g = out[1] * 255.0f, bl = out[2] * 255.0f, al = out[3] * 255.0f;

            u32 dst = g_color[i];
            if (blend) {
                float sa = al / 255.0f, da = (float)(dst >> 24) / 255.0f;
                float d[3] = {(float)((dst >> 16) & 255), (float)((dst >> 8) & 255), (float)(dst & 255)};
                float s[3] = {r, g, bl};
                for (int k = 0; k < 3; k++) {
                    float sf = blend_factor(sfac, true, sa, da, s[k] / 255.0f, d[k] / 255.0f);
                    float df = blend_factor(dfac, false, sa, da, s[k] / 255.0f, d[k] / 255.0f);
                    float o = subtract ? d[k] - s[k] : s[k] * sf + d[k] * df;
                    s[k] = o < 0 ? 0 : o > 255 ? 255 : o;
                }
                r = s[0]; g = s[1]; bl = s[2];
            }
            u32 o = dst;
            if (color_update)
                o = (o & 0xFF000000u) | ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
            if (alpha_update)
                o = (o & 0x00FFFFFFu) | ((u32)(al < 0 ? 0 : al > 255 ? 255 : al) << 24);
            g_color[i] = o;
            if (ztest && zupdate)
                g_depth[i] = z;
        }
    }
}

void gx_raster_draw(CPUState* cpu, u8 prim, u32 vat, u32 count, const u8* data) {
    if (!gx_raster_active() || count == 0)
        return;
    g_cpu = cpu;
    tev_prepare();

    SVtx* sv = (SVtx*)malloc(sizeof(SVtx) * count);
    if (!sv)
        return;
    const u8* p = data;
    for (u32 i = 0; i < count; i++) {
        Vtx v;
        p += decode_vertex(cpu, vat, p, &v);
        transform(&v, &sv[i]);
    }

    switch (prim & 0xF8) {
    case 0x80:
    case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4) {
            draw_triangle(&sv[i], &sv[i + 1], &sv[i + 2]);
            draw_triangle(&sv[i], &sv[i + 2], &sv[i + 3]);
        }
        break;
    case 0x90:
        for (u32 i = 0; i + 2 < count; i += 3)
            draw_triangle(&sv[i], &sv[i + 1], &sv[i + 2]);
        break;
    case 0x98:
        for (u32 i = 0; i + 2 < count; i++)
            draw_triangle(&sv[i], &sv[i + 1], &sv[i + 2]);
        break;
    case 0xA0:
        for (u32 i = 1; i + 1 < count; i++)
            draw_triangle(&sv[0], &sv[i], &sv[i + 1]);
        break;
    default:
        break;
    }
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

static void copy_to_xfb(u32 cmd, u32 sx, u32 sy, u32 w, u32 h) {
    if (!g_cpu)
        return;
    u32 dest = (gx_bp[0x4B] & 0x00FFFFFFu) << 5;
    u32 stride = (gx_bp[0x4D] & 0x3FFu) << 5;
    u32 yscale_reg = gx_bp[0x4E] & 0x1FFu;
    bool invert = (cmd >> 10) & 1u;
    double yscale = 1.0;
    if (yscale_reg)
        yscale = invert ? 256.0 / yscale_reg : yscale_reg / 256.0;
    u32 lines = (u32)(h * yscale + 0.5);
    if (lines > 1024)
        lines = 1024;
    if (w * 2u > stride)
        w = stride / 2u;          /* a line never spills into the next one */
    if (g_xfb_copy_log < 8) {
        g_xfb_copy_log++;
        printf("  xfb copy: %ux%u from (%u,%u) -> 0x%08X stride %u, yscale 0x%03X%s = %u lines, cmd 0x%06X\n",
               w, h, sx, sy, dest, stride, yscale_reg, invert ? " (inverted)" : "", lines, cmd);
    }
    if (!w || !lines || dest >= g_cpu->ram_size || (u64)lines * stride > g_cpu->ram_size - dest)
        return;
    for (u32 y = 0; y < lines; y++) {
        u32 srcy = sy + (u32)(y / yscale);
        if (srcy >= EFB_H || srcy >= sy + h)
            srcy = sy + h - 1 < EFB_H ? sy + h - 1 : EFB_H - 1;
        u8* d = g_cpu->ram + dest + y * stride;
        for (u32 x = 0; x + 1 < w; x += 2) {
            u32 c0 = sx + x < EFB_W ? g_color[srcy * EFB_W + sx + x] : 0;
            u32 c1 = sx + x + 1 < EFB_W ? g_color[srcy * EFB_W + sx + x + 1] : 0;
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

void gx_raster_copy(u32 cmd) {
    bool to_xfb = (cmd >> 14) & 1u;
    bool clear = (cmd >> 11) & 1u;

    if (to_xfb) {
        if (gx_raster_active()) {
            u32 sx = gx_bp[0x49] & 0x3FFu, sy = (gx_bp[0x49] >> 10) & 0x3FFu;
            u32 w = (gx_bp[0x4A] & 0x3FFu) + 1, h = ((gx_bp[0x4A] >> 10) & 0x3FFu) + 1;
            copy_to_xfb(cmd, sx, sy, w, h);
            if (g_save && g_dumped < MAX_DUMPS) {
                char path[600];
                snprintf(path, sizeof(path), "%s/frame_%06u.bmp", g_dir, g_frames + 1);
                write_bmp(path, sx, sy, w, h);
                printf("  frame %u saved (game time %.2f s)\n", g_frames + 1, interrupts_game_seconds());
                g_dumped++;
            }
        }
        g_frames++;
        gx_tex_flush_cache();   /* texture data in RAM may change between frames */
    }

    if (clear) {
        u32 a = (gx_bp[0x4F] >> 8) & 255u, r = gx_bp[0x4F] & 255u;
        u32 g = (gx_bp[0x50] >> 8) & 255u, b = gx_bp[0x50] & 255u;
        u32 color = (a << 24) | (r << 16) | (g << 8) | b;
        u32 z = gx_bp[0x51] & 0xFFFFFFu;
        for (size_t i = 0; i < (size_t)EFB_W * EFB_H; i++) {
            g_color[i] = color;
            g_depth[i] = z;
        }
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

void gx_raster_xfb_field(CPUState* cpu, u64 field, u32 tfbl, u32 picture_cfg) {
    if (!g_xfb_every || g_xfb_dumped >= MAX_DUMPS || field % g_xfb_every)
        return;
    u32 addr = (tfbl & 0x10000000u) ? (tfbl & 0x00FFFFFFu) << 5 : (tfbl & 0x00FFFFFFu);
    u32 width = ((picture_cfg >> 8) & 0x7Fu) * 16u;   /* WPL: width in 16-pixel units */
    u32 stride = (picture_cfg & 0xFFu) * 32u;          /* STD is per field line; a frame line is half */
    if (width == 0 || width > 720)
        width = 640;
    u32 line_bytes = width * 2u;
    /* Interlaced: a field's stride skips every other line, so a frame line is stride / 2. */
    if (stride >= line_bytes * 2u)
        stride /= 2u;
    if (stride < line_bytes)
        stride = line_bytes;
    u32 height = 480;
    if (addr == 0 || addr + stride * height > cpu->ram_size)
        return;

    FILE* f;
    char path[600];
    snprintf(path, sizeof(path), "%s/xfb_%06llu.bmp", g_dir, (unsigned long long)field);
    f = fopen(path, "wb");
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
        const u8* src = cpu->ram + addr + (height - 1 - y) * stride;
        for (u32 x = 0; x + 1 < width; x += 2) {
            int y0 = src[x * 2], u = src[x * 2 + 1] - 128, y1 = src[x * 2 + 2], v = src[x * 2 + 3] - 128;
            int r = (int)(1.371f * v), g = (int)(-0.698f * v - 0.336f * u), b = (int)(1.732f * u);
            u8* d = line + x * 3;
            d[0] = clamp8(y0 + b); d[1] = clamp8(y0 + g); d[2] = clamp8(y0 + r);
            d[3] = clamp8(y1 + b); d[4] = clamp8(y1 + g); d[5] = clamp8(y1 + r);
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
    printf("renderer: %u frames shown, %u dumped to %s, %llu triangles drawn, %llu clipped\n",
           g_frames, g_dumped, g_every ? g_dir : "(off)", g_tris_drawn, g_tris_skipped);
}
