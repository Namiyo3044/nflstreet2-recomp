#include "gx/raster.h"
#include "gx/state.h"
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
static u32 g_every;
static u32 g_frames;        /* frames copied to the display so far */
static u32 g_dumped;
static unsigned long long g_tris_drawn, g_tris_skipped;

void gx_raster_config(const char* dir, u32 every) {
    if (dir) {
        strncpy(g_dir, dir, sizeof(g_dir) - 1);
        g_dir[sizeof(g_dir) - 1] = '\0';
    }
    g_every = every;
    if (every)
        make_dir(g_dir);
}

bool gx_raster_active(void) {
    return g_every && g_dumped < MAX_DUMPS && ((g_frames + 1) % g_every) == 0;
}

/* ------------------------------------------------------ vertex decode --- */
typedef struct {
    float x, y, z;          /* position, model space */
    u32 pos_mtx;            /* row index into transform memory */
    u32 color0;             /* 0xRRGGBBAA */
    bool has_color0;
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

/* Pointer to element `index` of attribute array `array` in guest RAM. */
static const u8* array_elem(CPUState* cpu, u32 array, u32 index, u32 size) {
    u32 addr = (gx_cp[0xA0 + array] & 0x03FFFFFFu) + index * gx_cp[0xB0 + array];
    addr &= 0x01FFFFFFu;
    if (addr + size > cpu->ram_size)
        return NULL;
    return cpu->ram + addr;
}

/* Decodes one vertex at p for vertex format vat; returns bytes consumed.
   Mirrors vertex_size() in fifo.c, which must agree with it exactly. */
static u32 decode_vertex(CPUState* cpu, u32 vat, const u8* p, Vtx* v) {
    u32 lo = gx_cp[0x50], hi = gx_cp[0x60];
    u32 a = gx_cp[0x70 + vat], b = gx_cp[0x80 + vat], c = gx_cp[0x90 + vat];
    const u8* start = p;
    (void)b; (void)c;

    v->pos_mtx = gx_xf_reg[0x18] & 0x3Fu;
    v->has_color0 = false;
    v->color0 = 0xFFFFFFFFu;

    if (lo & 1u) v->pos_mtx = *p++ & 0x3Fu;     /* position/normal matrix index */
    for (u32 i = 1; i < 9; i++)                 /* texture matrix indices: skipped for now */
        if ((lo >> i) & 1u) p++;

    /* Position */
    u32 pos = (lo >> 9) & 3u;
    u32 pcount = (a & 1u) ? 3u : 2u, pfmt = (a >> 1) & 7u, pshift = (a >> 4) & 31u;
    if (pos) {
        const u8* src = p;
        if (pos == 1) {
            p += pcount * comp_size(pfmt);
        } else {
            u32 idx = pos == 2 ? *p : rd16(p);
            p += pos == 2 ? 1 : 2;
            src = array_elem(cpu, 0, idx, pcount * comp_size(pfmt));
        }
        if (src) {
            u32 cs = comp_size(pfmt);
            v->x = read_comp(src, pfmt, pshift);
            v->y = read_comp(src + cs, pfmt, pshift);
            v->z = pcount == 3 ? read_comp(src + 2 * cs, pfmt, pshift) : 0.0f;
        } else {
            v->x = v->y = v->z = 0.0f;
        }
    }

    /* Normal: stepped over */
    u32 nrm = (lo >> 11) & 3u;
    if (nrm) {
        bool nbt = (a >> 9) & 1u;
        bool index3 = (a >> 31) & 1u;
        if (nrm == 1)
            p += (nbt ? 9u : 3u) * comp_size((a >> 10) & 7u);
        else
            p += ((nbt && index3) ? 3u : 1u) * (nrm == 2 ? 1u : 2u);
    }

    /* Colour 0 and 1 */
    for (u32 ch = 0; ch < 2; ch++) {
        u32 t = (lo >> (13 + ch * 2)) & 3u;
        u32 fmt = (a >> (14 + ch * 4)) & 7u;
        if (!t)
            continue;
        const u8* src = p;
        if (t == 1) {
            p += color_size(fmt);
        } else {
            u32 idx = t == 2 ? *p : rd16(p);
            p += t == 2 ? 1 : 2;
            src = array_elem(cpu, 2 + ch, idx, color_size(fmt));
        }
        if (ch == 0 && src) {
            v->color0 = read_color(src, fmt);
            v->has_color0 = true;
        }
    }

    /* Texture coordinates: stepped over (stage 2) */
    const u32 tc_cnt[8] = {(a >> 21) & 1u, b & 1u, (b >> 9) & 1u, (b >> 18) & 1u,
                           (b >> 27) & 1u, (c >> 5) & 1u, (c >> 14) & 1u, (c >> 23) & 1u};
    const u32 tc_fmt[8] = {(a >> 22) & 7u, (b >> 1) & 7u, (b >> 10) & 7u, (b >> 19) & 7u,
                           (b >> 28) & 7u, (c >> 6) & 7u, (c >> 15) & 7u, (c >> 24) & 7u};
    for (u32 i = 0; i < 8; i++) {
        u32 t = (hi >> (i * 2)) & 3u;
        if (t == 1) p += (tc_cnt[i] ? 2u : 1u) * comp_size(tc_fmt[i]);
        else if (t == 2) p += 1;
        else if (t == 3) p += 2;
    }
    return (u32)(p - start);
}

/* ---------------------------------------------------------- transform --- */
typedef struct {
    float x, y, z;          /* screen x, y; depth 0..16777215 */
    float r, g, b, a;       /* 0..255 */
    bool valid;
} SVtx;

static void transform(const Vtx* in, SVtx* out) {
    const u32* m = &gx_xf_mem[(in->pos_mtx * 4u) & 0x3FFu];
    float ex = gx_f32(m[0]) * in->x + gx_f32(m[1]) * in->y + gx_f32(m[2]) * in->z + gx_f32(m[3]);
    float ey = gx_f32(m[4]) * in->x + gx_f32(m[5]) * in->y + gx_f32(m[6]) * in->z + gx_f32(m[7]);
    float ez = gx_f32(m[8]) * in->x + gx_f32(m[9]) * in->y + gx_f32(m[10]) * in->z + gx_f32(m[11]);

    const u32* pr = &gx_xf_reg[0x20];
    float cx, cy, cz, cw;
    if (gx_xf_reg[0x26] == 0) {             /* perspective */
        cx = gx_f32(pr[0]) * ex + gx_f32(pr[1]) * ez;
        cy = gx_f32(pr[2]) * ey + gx_f32(pr[3]) * ez;
        cz = gx_f32(pr[4]) * ez + gx_f32(pr[5]);
        cw = -ez;
    } else {                                /* orthographic */
        cx = gx_f32(pr[0]) * ex + gx_f32(pr[1]);
        cy = gx_f32(pr[2]) * ey + gx_f32(pr[3]);
        cz = gx_f32(pr[4]) * ez + gx_f32(pr[5]);
        cw = 1.0f;
    }

    out->valid = cw > 1e-6f;
    if (!out->valid)
        return;
    float nx = cx / cw, ny = cy / cw, nz = cz / cw;
    const u32* vp = &gx_xf_reg[0x1A];       /* wd, ht, zRange, xOrig, yOrig, farZ */
    out->x = nx * gx_f32(vp[0]) + gx_f32(vp[3]) - 342.0f;
    out->y = ny * gx_f32(vp[1]) + gx_f32(vp[4]) - 342.0f;
    out->z = nz * gx_f32(vp[2]) + gx_f32(vp[5]);

    /* Colour channel 0: vertex colour or the material colour register. */
    u32 mat = gx_xf_reg[0x0C];
    u32 col = (gx_xf_reg[0x0E] & 1u) && in->has_color0 ? in->color0 : mat;
    u32 alp = (gx_xf_reg[0x10] & 1u) && in->has_color0 ? in->color0 : mat;
    if (!(gx_xf_reg[0x0E] & 1u) && !mat && in->has_color0)
        col = alp = in->color0;   /* channel not configured yet: show the vertex colour */
    out->r = (float)(col >> 24);
    out->g = (float)((col >> 16) & 255);
    out->b = (float)((col >> 8) & 255);
    out->a = (float)(alp & 255);
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
    case 2: return is_src ? dc : sc;          /* dst colour for src, src colour for dst */
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
    if ((gx_bp[0x00] >> 14 & 3u) == 3u) return;   /* cull all */

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
        color_update = true;   /* blend mode not set up yet: still show the geometry */

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

            float r = w0 * a->r + w1 * b->r + w2 * c->r;
            float g = w0 * a->g + w1 * b->g + w2 * c->g;
            float bl = w0 * a->b + w1 * b->b + w2 * c->b;
            float al = w0 * a->a + w1 * b->a + w2 * c->a;

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
            u32 out = dst;
            if (color_update)
                out = (out & 0xFF000000u) | ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
            if (alpha_update)
                out = (out & 0x00FFFFFFu) | ((u32)(al < 0 ? 0 : al > 255 ? 255 : al) << 24);
            g_color[i] = out;
            if (ztest && zupdate)
                g_depth[i] = z;
        }
    }
}

void gx_raster_draw(CPUState* cpu, u8 prim, u32 vat, u32 count, const u8* data) {
    if (!gx_raster_active() || count == 0)
        return;

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
    case 0x80:   /* quads */
    case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4) {
            draw_triangle(&sv[i], &sv[i + 1], &sv[i + 2]);
            draw_triangle(&sv[i], &sv[i + 2], &sv[i + 3]);
        }
        break;
    case 0x90:   /* triangles */
        for (u32 i = 0; i + 2 < count; i += 3)
            draw_triangle(&sv[i], &sv[i + 1], &sv[i + 2]);
        break;
    case 0x98:   /* triangle strip */
        for (u32 i = 0; i + 2 < count; i++)
            draw_triangle(&sv[i], &sv[i + 1], &sv[i + 2]);
        break;
    case 0xA0:   /* triangle fan */
        for (u32 i = 1; i + 1 < count; i++)
            draw_triangle(&sv[0], &sv[i], &sv[i + 1]);
        break;
    default:     /* lines and points: not drawn yet */
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
        u32 srcy = sy + (h - 1 - y);                    /* BMP rows go bottom-up */
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

void gx_raster_copy(u32 cmd) {
    bool to_xfb = (cmd >> 14) & 1u;
    bool clear = (cmd >> 11) & 1u;

    if (to_xfb) {
        if (gx_raster_active()) {
            u32 sx = gx_bp[0x49] & 0x3FFu, sy = (gx_bp[0x49] >> 10) & 0x3FFu;
            u32 w = (gx_bp[0x4A] & 0x3FFu) + 1, h = ((gx_bp[0x4A] >> 10) & 0x3FFu) + 1;
            char path[600];
            snprintf(path, sizeof(path), "%s/frame_%06u.bmp", g_dir, g_frames + 1);
            write_bmp(path, sx, sy, w, h);
            printf("  frame %u saved (game time %.2f s)\n", g_frames + 1, interrupts_game_seconds());
            g_dumped++;
        }
        g_frames++;
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

void gx_raster_report(void) {
    printf("renderer: %u frames shown, %u dumped to %s, %llu triangles drawn, %llu clipped\n",
           g_frames, g_dumped, g_every ? g_dir : "(off)", g_tris_drawn, g_tris_skipped);
}
