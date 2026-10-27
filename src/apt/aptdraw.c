#include "apt/aptdraw.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ fonts */
typedef struct { uint8_t w, h; uint16_t x, y; uint8_t adv; int8_t ox, oy; bool have; } Glyph;
typedef struct { uint16_t a, b; int8_t amt; } Kern;
typedef struct {
    int height;
    Glyph g[256];
    Kern* kern; int nkern;
    uint8_t* atlas; int aw, ah;
} Font;

static TerfPack* g_fontpack;
static char g_fontnames[16][64]; static int g_nfontnames;
static Font* g_fonts[16];

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static Font* font_parse(const uint8_t* d, uint32_t n) {
    if (!d || n < 0x20) return NULL;
    Font* f = (Font*)calloc(1, sizeof *f);
    int ng = rd16(d + 10);
    f->height = d[0x13] ? d[0x13] : 16;
    uint32_t kern = rd32(d + 0x18), tex = rd32(d + 0x1c);
    int maxx = 0;
    for (int i = 0; i < ng; i++) {
        uint32_t p = 0x20 + (uint32_t)i * 12;
        if (p + 12 > n) break;
        int c = rd16(d + p);
        Glyph g = {d[p + 2], d[p + 3], rd16(d + p + 4), rd16(d + p + 6), d[p + 8], (int8_t)d[p + 9], (int8_t)d[p + 10], true};
        if (c < 256) f->g[c] = g;
        if (g.x + g.w > maxx) maxx = g.x + g.w;
    }
    int W = 1;
    while (W < maxx) W *= 2;
    int H = kern > tex + 16 ? (int)(((kern - tex - 16) * 2) / (uint32_t)W) : 0;
    if (H > 0 && tex + 16 + (uint32_t)(W * H / 2) <= n) {
        f->aw = W; f->ah = H;
        f->atlas = (uint8_t*)malloc((size_t)W * H);
        const uint8_t* raw = d + tex + 16;
        for (int i = 0; i < W * H / 2; i++) { f->atlas[2 * i] = (uint8_t)((raw[i] & 15) * 17); f->atlas[2 * i + 1] = (uint8_t)((raw[i] >> 4) * 17); }
    }
    if (kern + 4 <= n) {
        uint32_t nk = rd32(d + kern);
        if (nk > 100000) nk = 0;
        f->kern = (Kern*)calloc(nk ? nk : 1, sizeof(Kern));
        for (uint32_t i = 0; i < nk; i++) {
            uint32_t p = kern + 4 + i * 8;
            if (p + 8 > n) break;
            f->kern[f->nkern].a = rd16(d + p); f->kern[f->nkern].b = rd16(d + p + 2); f->kern[f->nkern].amt = (int8_t)d[p + 4];
            f->nkern++;
        }
    }
    return f;
}

void aptdraw_set_fonts(TerfPack* fonts) {
    g_fontpack = fonts;
    g_nfontnames = 0;
    for (int i = 0; i < 16; i++) g_fonts[i] = NULL;       /* (a font set changes once per run at most) */
    if (!fonts || fonts->count < 1) return;
    const uint8_t* d = fonts->entries[0].data; uint32_t n = fonts->entries[0].size;
    if (!d || n < 2) return;
    int cnt = rd16(d); uint32_t p = 2;
    for (int i = 0; i < cnt && i < 16 && p < n; i++) {
        int ln = d[p];
        if (p + 1 + (uint32_t)ln > n) break;
        int k = ln < 63 ? ln : 63;
        memcpy(g_fontnames[i], d + p + 1, (size_t)k); g_fontnames[i][k] = 0;
        g_nfontnames++;
        p += 2 + (uint32_t)ln;
    }
}

static Font* font_named(const char* name) {
    if (!g_fontpack) return NULL;
    int idx = -1;
    size_t nl = strlen(name);
    for (int i = 0; i < g_nfontnames; i++) {
        size_t l = strlen(g_fontnames[i]);
        if (!strcmp(g_fontnames[i], name) || (l >= nl && !strcmp(g_fontnames[i] + l - nl, name))) idx = i;
    }
    if (idx < 0) idx = 0;
    if (!g_fonts[idx] && idx + 1 < g_fontpack->count)
        g_fonts[idx] = font_parse(g_fontpack->entries[idx + 1].data, g_fontpack->entries[idx + 1].size);
    return g_fonts[idx];
}

static const Glyph* glyph(Font* f, unsigned char c) {
    if (f->g[c].have) return &f->g[c];
    if (c >= 'a' && c <= 'z' && f->g[c - 32].have) return &f->g[c - 32];
    if (f->g['?'].have) return &f->g['?'];
    if (f->g[' '].have) return &f->g[' '];
    return NULL;
}
static int kern_of(Font* f, unsigned char a, unsigned char b) {
    for (int i = 0; i < f->nkern; i++) if (f->kern[i].a == a && f->kern[i].b == b) return f->kern[i].amt;
    return 0;
}
static float text_width(Font* f, const char* s, size_t n, float scale) {
    float w = 0; int prev = -1;
    for (size_t i = 0; i < n; i++) {
        const Glyph* g = glyph(f, (unsigned char)s[i]);
        if (!g) continue;
        if (prev >= 0) w += kern_of(f, (unsigned char)prev, (unsigned char)s[i]);
        w += g->adv; prev = (unsigned char)s[i];
    }
    return w * scale;
}

/* ================================================================ targets */
typedef struct {
    uint32_t* px; int w, h;
    int bx0, by0, bx1, by1;                          /* touched area */
} Target;

static void touch(Target* t, int x0, int y0, int x1, int y1) {
    if (x0 < t->bx0) t->bx0 = x0;
    if (y0 < t->by0) t->by0 = y0;
    if (x1 > t->bx1) t->bx1 = x1;
    if (y1 > t->by1) t->by1 = y1;
}

static inline uint32_t over(uint32_t s, uint32_t d) {
    uint32_t sa = s >> 24;
    if (sa == 255) return s;
    uint32_t ia = 255 - sa;
    uint32_t rb = (d & 0x00FF00FF) * ia, ag = ((d >> 8) & 0x00FF00FF) * ia;
    rb = ((rb + 0x00800080 + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    ag = ((ag + 0x00800080 + ((ag >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    return s + (rb | ag << 8);                       /* premultiplied: never past 255 */
}
static inline uint32_t scale_px(uint32_t c, uint32_t a) {   /* c * a/255 (premultiplied) */
    if (a >= 255) return c;
    uint32_t rb = (c & 0x00FF00FF) * a, ag = ((c >> 8) & 0x00FF00FF) * a;
    rb = ((rb + 0x00800080 + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    ag = ((ag + 0x00800080 + ((ag >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    return rb | ag << 8;
}

/* colour transform: multiply (0-1) and add (0-255), as Flash, on straight colour */
typedef struct { float m[4], a[4]; } CX;
static bool cx_identity(const CX* c) { return c->m[0] == 1 && c->m[1] == 1 && c->m[2] == 1 && c->m[3] == 1 && !c->a[0] && !c->a[1] && !c->a[2] && !c->a[3]; }
static inline int clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }
static uint32_t cx_straight(const CX* c, uint32_t argb) {     /* straight in, premultiplied out */
    int a = clamp255((argb >> 24) * c->m[3] + c->a[3]);
    if (!a) return 0;
    int r = clamp255(((argb >> 16) & 255) * c->m[0] + c->a[0]);
    int g = clamp255(((argb >> 8) & 255) * c->m[1] + c->a[1]);
    int b = clamp255((argb & 255) * c->m[2] + c->a[2]);
    return (uint32_t)a << 24 | (uint32_t)(r * a / 255) << 16 | (uint32_t)(g * a / 255) << 8 | (uint32_t)(b * a / 255);
}
static uint32_t cx_premul(const CX* c, uint32_t pm) {         /* premultiplied in and out */
    uint32_t a = pm >> 24;
    if (!a) return cx_straight(c, 0);
    uint32_t r = ((pm >> 16) & 255) * 255 / a, g = ((pm >> 8) & 255) * 255 / a, b = (pm & 255) * 255 / a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return cx_straight(c, a << 24 | r << 16 | g << 8 | b);
}

/* a colour transform with nothing added: premultiplied channels just scale */
typedef struct { bool scale_only, identity; uint32_t f[4]; } CXFast;   /* f: 0-256, b g r a */
static void cx_fast(const CX* c, CXFast* o) {
    o->identity = !c || cx_identity(c);
    o->scale_only = !o->identity && !c->a[0] && !c->a[1] && !c->a[2] && !c->a[3];
    if (o->scale_only) {
        float ma = c->m[3] < 0 ? 0 : c->m[3] > 1 ? 1 : c->m[3];
        float m[3] = {c->m[2], c->m[1], c->m[0]};
        for (int i = 0; i < 3; i++) { float v = m[i] < 0 ? 0 : m[i] > 1 ? 1 : m[i]; o->f[i] = (uint32_t)(v * ma * 256 + 0.5f); }
        o->f[3] = (uint32_t)(ma * 256 + 0.5f);
    }
}
static inline uint32_t cx_apply_fast(const CXFast* f, uint32_t c) {
    return ((c & 255) * f->f[0] >> 8) | (((c >> 8) & 255) * f->f[1] >> 8) << 8 | (((c >> 16) & 255) * f->f[2] >> 8) << 16 | ((c >> 24) * f->f[3] >> 8) << 24;
}

/* ================================================================ triangles */
static void mul6(const float* a, const float* b, float* o) {
    float r[6] = {a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
                  a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]};
    memcpy(o, r, sizeof r);
}
static bool inv6(const float* m, float* o) {
    double det = (double)m[0] * m[3] - (double)m[1] * m[2];
    if (fabs(det) < 1e-12) return false;
    double a = m[3] / det, b = -m[1] / det, c = -m[2] / det, d = m[0] / det;
    o[0] = (float)a; o[1] = (float)b; o[2] = (float)c; o[3] = (float)d;
    o[4] = (float)(-(a * m[4] + c * m[5])); o[5] = (float)(-(b * m[4] + d * m[5]));
    return true;
}

/* bilinear, clamped at the edges; u, v in texels (16.16) */
static inline uint32_t sample_fx(const AptImage* im, int32_t u, int32_t v) {
    u -= 32768; v -= 32768;
    int x0 = u >> 16, y0 = v >> 16;
    uint32_t fx = (uint32_t)(u >> 8) & 255, fy = (uint32_t)(v >> 8) & 255;
    int W = im->w, H = im->h;
    int xa = x0 < 0 ? 0 : x0 >= W ? W - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= W ? W - 1 : x0 + 1;
    int ya = y0 < 0 ? 0 : y0 >= H ? H - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= H ? H - 1 : y0 + 1;
    const uint32_t* r0 = im->pm + ya * W; const uint32_t* r1 = im->pm + yb * W;
    uint32_t c00 = r0[xa], c10 = r0[xb], c01 = r1[xa], c11 = r1[xb];
    if (c00 == c10 && c00 == c01 && c00 == c11) return c00;
    /* two lerps per channel pair (r b and a g lanes) */
    uint32_t ix = 256 - fx, iy = 256 - fy;
    uint32_t t_rb = ((c00 & 0xFF00FF) * ix + (c10 & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
    uint32_t t_ag = (((c00 >> 8) & 0xFF00FF) * ix + ((c10 >> 8) & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
    uint32_t b_rb = ((c01 & 0xFF00FF) * ix + (c11 & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
    uint32_t b_ag = (((c01 >> 8) & 0xFF00FF) * ix + ((c11 >> 8) & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
    uint32_t rb = (t_rb * iy + b_rb * fy) >> 8 & 0xFF00FF;
    uint32_t ag = (t_ag * iy + b_ag * fy) >> 8 & 0xFF00FF;
    return rb | ag << 8;
}
static inline int32_t to_fx(float f) { return f < -30000.f ? -30000 * 65536 : f > 30000.f ? 30000 * 65536 : (int32_t)(f * 65536.f); }

/* a triangle in pixels: solid premultiplied colour, or an image through uvm (pixel -> texel) */
static void tri(Target* t, const float* P, uint32_t color, const AptImage* im, const float* uvm, const CX* cx) {
    float x0 = P[0], y0 = P[1], x1 = P[2], y1 = P[3], x2 = P[4], y2 = P[5];
    for (int i = 0; i < 6; i++) if (P[i] != P[i] || fabsf(P[i]) > 1e7f) return;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabsf(area) < 1e-6f) return;
    if (area < 0) { float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; area = -area; }
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > t->w) maxx = t->w;
    if (maxy > t->h) maxy = t->h;
    if (minx >= maxx || miny >= maxy) return;
    /* edge functions, top-left rule (so neighbouring triangles don't overlap) */
    float A0 = y1 - y2, B0 = x2 - x1, C0 = x1 * y2 - x2 * y1;
    float A1 = y2 - y0, B1 = x0 - x2, C1 = x2 * y0 - x0 * y2;
    float A2 = y0 - y1, B2 = x1 - x0, C2 = x0 * y1 - x1 * y0;
    float b0 = (A0 > 0 || (A0 == 0 && B0 < 0)) ? 0 : 1e-6f;
    float b1 = (A1 > 0 || (A1 == 0 && B1 < 0)) ? 0 : 1e-6f;
    float b2 = (A2 > 0 || (A2 == 0 && B2 < 0)) ? 0 : 1e-6f;
    bool any = false;
    CXFast cf;
    cx_fast(cx, &cf);
    int32_t du = im ? to_fx(uvm[0]) : 0, dv = im ? to_fx(uvm[1]) : 0;
    for (int y = miny; y < maxy; y++) {
        float py = y + 0.5f;
        /* the row's span from the edges, then exact tests near its ends */
        float k0 = B0 * py + C0, k1 = B1 * py + C1, k2 = B2 * py + C2;
        float xl = (float)minx, xr = (float)maxx;
        float As[3] = {A0, A1, A2}, Ks[3] = {k0, k1, k2}, Bs[3] = {b0, b1, b2};
        bool skip = false;
        for (int e = 0; e < 3; e++) {
            if (As[e] > 0) { float b = (Bs[e] - Ks[e]) / As[e] - 0.5f; if (b - 1 > xl) xl = b - 1; }
            else if (As[e] < 0) { float b = (Bs[e] - Ks[e]) / As[e] - 0.5f; if (b + 2 < xr) xr = b + 2; }
            else if (Ks[e] < Bs[e]) skip = true;
        }
        if (skip || xl >= xr) continue;
        int sx0 = xl < minx ? minx : (int)xl, sx1 = xr > maxx ? maxx : (int)xr;
        float px = sx0 + 0.5f;
        float e0 = A0 * px + k0, e1 = A1 * px + k1, e2 = A2 * px + k2;
        int32_t u = 0, v = 0;
        if (im) { u = to_fx(uvm[0] * px + uvm[2] * py + uvm[4]); v = to_fx(uvm[1] * px + uvm[3] * py + uvm[5]); }
        uint32_t* row = t->px + (size_t)y * t->w;
        for (int x = sx0; x < sx1; x++, e0 += A0, e1 += A1, e2 += A2, u += du, v += dv) {
            if (e0 < b0 || e1 < b1 || e2 < b2) continue;
            uint32_t c;
            if (im) {
                c = sample_fx(im, u, v);
                if (!cf.identity) c = cf.scale_only ? cx_apply_fast(&cf, c) : cx_premul(cx, c);
            } else c = color;
            if (!c) continue;
            row[x] = over(c, row[x]);
            any = true;
        }
    }
    if (any) touch(t, minx, miny, maxx, maxy);
}

static void quad_line(Target* t, float ax, float ay, float bx, float by, float wpx, uint32_t color) {
    float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-4f) return;
    float nx = -dy / l * wpx * 0.5f, ny = dx / l * wpx * 0.5f;
    float q1[6] = {ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny};
    float q2[6] = {ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny};
    tri(t, q1, color, NULL, NULL, NULL);
    tri(t, q2, color, NULL, NULL, NULL);
}

/* ================================================================ resources */
static AptGeom* geom_of(VM* vm, AptFile* f, int cid) {
    AptGeom* g = apt_geom(f, (uint32_t)cid);
    if (g) return g;
    AptFile* of = NULL; int oc = 0;
    if (vm_resolve_char(vm, f, cid, &of, &oc) && of != f) return apt_geom(of, (uint32_t)oc);
    return NULL;
}
static AptImage* image_of(VM* vm, AptFile* f, int cid) {
    AptImage* im = apt_image(f, (uint32_t)cid);
    if (im) return im;
    AptFile* of = NULL; int oc = 0;
    if (vm_resolve_char(vm, f, cid, &of, &oc) && of != f) return apt_image(of, (uint32_t)oc);
    return NULL;
}

/* ================================================================ bounds */
static void apply(const float* m, float x, float y, float* ox, float* oy) { *ox = m[0] * x + m[2] * y + m[4]; *oy = m[1] * x + m[3] * y + m[5]; }

void aptdraw_local_bounds(VM* vm, Obj* o, float out[4], bool* any) {
    *any = false;
    DispExt* d = o->d;
    if (o->kind == OK_SHAPE) {
        if (!d->ch || d->ch->type != CH_SHAPE) return;
        AptGeom* g = geom_of(vm, d->file, (int)d->ch->geom);
        float b[4] = {1e30f, 1e30f, -1e30f, -1e30f}; bool have = false;
        if (g)
            for (int r = 0; r < g->n; r++)
                for (int i = 0; i + 1 < g->recs[r].npts * 2; i += 2) {
                    float x = g->recs[r].pts[i], y = g->recs[r].pts[i + 1];
                    if (x < b[0]) b[0] = x;
                    if (y < b[1]) b[1] = y;
                    if (x > b[2]) b[2] = x;
                    if (y > b[3]) b[3] = y;
                    have = true;
                }
        if (!have) memcpy(b, d->ch->bounds, sizeof b);
        memcpy(out, b, sizeof b);
        *any = true;
        return;
    }
    if (o->kind == OK_TEXT) { memcpy(out, d->rect, 4 * sizeof(float)); *any = true; return; }
    if (o->kind != OK_CLIP) return;
    float bb[4] = {1e30f, 1e30f, -1e30f, -1e30f};
    for (int i = 0; i < d->nkids; i++) {
        Obj* c = d->kids[i].o;
        float b[4]; bool h;
        aptdraw_local_bounds(vm, c, b, &h);
        if (!h) continue;
        float xs[4] = {b[0], b[2], b[0], b[2]}, ys[4] = {b[1], b[1], b[3], b[3]};
        for (int k = 0; k < 4; k++) {
            float x, y;
            apply(c->d->m, xs[k], ys[k], &x, &y);
            if (x < bb[0]) bb[0] = x;
            if (y < bb[1]) bb[1] = y;
            if (x > bb[2]) bb[2] = x;
            if (y > bb[3]) bb[3] = y;
        }
        *any = true;
    }
    if (*any) memcpy(out, bb, sizeof bb);
}

/* ================================================================ drawing */
typedef struct {
    VM* vm;
    float scale;
    int w, h;
    Target* pool[8]; int npool;                      /* spare layers (zeroed) */
} Draw;

static Target* layer_get(Draw* dr) {
    if (dr->npool) return dr->pool[--dr->npool];
    Target* t = (Target*)calloc(1, sizeof *t);
    t->w = dr->w; t->h = dr->h;
    t->px = (uint32_t*)calloc((size_t)dr->w * dr->h, 4);
    t->bx0 = t->by0 = 1 << 30; t->bx1 = t->by1 = -1;
    return t;
}
static void layer_put(Draw* dr, Target* t) {
    if (t->bx1 > t->bx0)
        for (int y = t->by0; y < t->by1; y++) memset(t->px + (size_t)y * t->w + t->bx0, 0, (size_t)(t->bx1 - t->bx0) * 4);
    t->bx0 = t->by0 = 1 << 30; t->bx1 = t->by1 = -1;
    if (dr->npool < 8) dr->pool[dr->npool++] = t;
    else { free(t->px); free(t); }
}

static void cx_combine(const CX* parent, const DispExt* d, CX* out) {
    float om[4] = {d->cx[0] / 255.f, d->cx[1] / 255.f, d->cx[2] / 255.f, d->cx[3] / 255.f * (d->alpha / 100.f)};
    for (int i = 0; i < 4; i++) { out->m[i] = parent->m[i] * om[i]; out->a[i] = parent->m[i] * d->cxadd[i] + parent->a[i]; }
}

static void draw_geom(Draw* dr, Target* t, AptFile* f, AptGeom* g, const float* M, const CX* cx) {
    for (int r = 0; r < g->n; r++) {
        AptGeomRec* rec = &g->recs[r];
        const float* pts = rec->pts;
        if (rec->style == 3) {
            uint32_t c = rec->color;
            uint32_t col = cx_straight(cx, c);
            if (!col) continue;
            float wpx = rec->param * dr->scale;
            if (wpx < 1) wpx = 1;
            for (int i = 0; i + 3 < rec->npts * 2; i += 4) {
                float ax, ay, bx, by;
                apply(M, pts[i], pts[i + 1], &ax, &ay);
                apply(M, pts[i + 2], pts[i + 3], &bx, &by);
                quad_line(t, ax, ay, bx, by, wpx, col);
            }
            continue;
        }
        AptImage* im = (rec->style == 0 || rec->style == 1) ? image_of(dr->vm, f, (int)rec->image) : NULL;
        float uvm[6];
        uint32_t col = 0;
        if (im) {
            float iv[6];
            if (!inv6(M, iv)) continue;
            mul6(rec->mtx, iv, uvm);                 /* pixel -> shape -> texel */
        } else {
            col = cx_straight(cx, rec->color);
            if (!col) continue;
        }
        for (int i = 0; i + 5 < rec->npts * 2; i += 6) {
            float P[6];
            apply(M, pts[i], pts[i + 1], &P[0], &P[1]);
            apply(M, pts[i + 2], pts[i + 3], &P[2], &P[3]);
            apply(M, pts[i + 4], pts[i + 5], &P[4], &P[5]);
            tri(t, P, col, im, uvm, cx);
        }
    }
}

static void draw_glyphs(Target* t, Font* f, float x, float y, const char* s, size_t n, float scale, uint32_t col) {
    int prev = -1;
    uint32_t ca = col >> 24;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        const Glyph* g = glyph(f, ch);
        if (!g) continue;
        if (prev >= 0) x += kern_of(f, (unsigned char)prev, ch) * scale;
        if (g->w > 1 && g->h > 1 && f->atlas) {
            int nw = (int)lroundf(g->w * scale), nh = (int)lroundf(g->h * scale);
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
            int px0 = (int)lroundf(x + g->ox * scale), py0 = (int)lroundf(y + g->oy * scale);
            float sx = (float)g->w / nw, sy = (float)g->h / nh;
            int ss = sx > 1 ? (int)ceilf(sx) : 1;
            if (ss > 4) ss = 4;
            int x0 = px0 < 0 ? 0 : px0, y0 = py0 < 0 ? 0 : py0;
            int x1 = px0 + nw > t->w ? t->w : px0 + nw, y1 = py0 + nh > t->h ? t->h : py0 + nh;
            for (int yy = y0; yy < y1; yy++) {
                uint32_t* row = t->px + (size_t)yy * t->w;
                for (int xx = x0; xx < x1; xx++) {
                    /* the glyph's footprint averaged (down-scaled) or bilinear (up-scaled) */
                    float acc = 0;
                    for (int j = 0; j < ss; j++)
                        for (int k = 0; k < ss; k++) {
                            float u = g->x + ((xx - px0) + (k + 0.5f) / ss) * sx - 0.5f;
                            float v = g->y + ((yy - py0) + (j + 0.5f) / ss) * sy - 0.5f;
                            int ux = (int)floorf(u), vy = (int)floorf(v);
                            float fx = u - ux, fy = v - vy;
                            int gx0 = g->x, gy0 = g->y, gx1 = g->x + g->w - 1, gy1 = g->y + g->h - 1;
                            int xa = ux < gx0 ? gx0 : ux > gx1 ? gx1 : ux, xb = ux + 1 < gx0 ? gx0 : ux + 1 > gx1 ? gx1 : ux + 1;
                            int ya = vy < gy0 ? gy0 : vy > gy1 ? gy1 : vy, yb = vy + 1 < gy0 ? gy0 : vy + 1 > gy1 ? gy1 : vy + 1;
                            if (xb >= f->aw || yb >= f->ah) continue;
                            const uint8_t* A = f->atlas;
                            acc += (A[ya * f->aw + xa] * (1 - fx) + A[ya * f->aw + xb] * fx) * (1 - fy) +
                                   (A[yb * f->aw + xa] * (1 - fx) + A[yb * f->aw + xb] * fx) * fy;
                        }
                    uint32_t a = (uint32_t)(acc / (ss * ss) + 0.5f);
                    if (!a) continue;
                    if (a > 255) a = 255;
                    row[xx] = over(scale_px(col, a), row[xx]);
                }
            }
            if (x1 > x0 && y1 > y0) touch(t, x0, y0, x1, y1);
            (void)ca;
        }
        x += g->adv * scale;
        prev = ch;
    }
}

static void draw_text(Draw* dr, Target* t, Obj* o, const float* M, const CX* cx) {
    VM* vm = dr->vm;
    DispExt* d = o->d;
    const char* txt = d->text ? d->text->s : "";
    if (d->var && d->parent) {
        Val v = vm_lookup_var(vm, d->parent, d->var);
        if (v.t != V_UNDEF) txt = v_cstr(vm, v);
    }
    if (!txt[0] || !strcmp(txt, "undefined") || !strcmp(txt, "false") || !strcmp(txt, "null") || !strcmp(txt, "NaN")) return;
    const char* fname = "STREET_BodyFont";
    if (d->font) {
        AptFile* of = NULL; int oc = 0;
        AptChar* fc = vm_resolve_char(vm, d->file, d->font, &of, &oc);
        if (fc && fc->type == CH_FONT && fc->fontname) fname = fc->fontname;
    }
    Font* fnt = font_named(fname);
    if (!fnt) return;
    float sy = sqrtf(M[2] * M[2] + M[3] * M[3]), sx = sqrtf(M[0] * M[0] + M[1] * M[1]);
    float scale = d->height / (float)fnt->height * sy;
    if (scale * fnt->height < 3) return;
    uint32_t c = d->color;
    uint32_t col = cx_straight(cx, 0xFF000000u | (c & 0xFFFFFF));
    if (!col) return;
    float lx, ty, rx, by;
    apply(M, d->rect[0], d->rect[1], &lx, &ty);
    apply(M, d->rect[2], d->rect[3], &rx, &by);
    float width = rx - lx;
    if (width < 1) width = 1;
    float lh = fnt->height * scale;
    float y = ty + 2 * sy;
    bool wrap = d->wordwrap || d->multiline;
    const char* p = txt;
    for (;;) {
        const char* e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        /* one paragraph [p, e) */
        const char* ls = p;
        while (1) {
            const char* le = e;
            if (wrap) {
                /* the longest run of words that fits */
                const char* best = NULL;
                const char* q = ls;
                while (q <= e) {
                    const char* sp = q;
                    while (sp < e && *sp != ' ') sp++;
                    if (best && text_width(fnt, ls, (size_t)(sp - ls), scale) > width - 4 * sy) break;
                    best = sp;
                    if (sp >= e) break;
                    q = sp + 1;
                }
                le = best ? best : e;
            }
            float w = text_width(fnt, ls, (size_t)(le - ls), scale);
            float x = d->align == 2 ? lx + (width - w) / 2 : d->align == 1 ? rx - w - 2 * sx : lx + 2 * sx;
            draw_glyphs(t, fnt, x, y, ls, (size_t)(le - ls), scale, col);
            y += lh;
            if (le >= e) break;
            ls = le + 1;
        }
        if (!*e) break;
        p = e + 1;
    }
}

static void draw_obj(Draw* dr, Target* t, Obj* o, const float* PM, const CX* pcx);

static void composite_masked(Target* dst, Target* layer, Target* mask) {
    int x0 = layer->bx0 > mask->bx0 ? layer->bx0 : mask->bx0, y0 = layer->by0 > mask->by0 ? layer->by0 : mask->by0;
    int x1 = layer->bx1 < mask->bx1 ? layer->bx1 : mask->bx1, y1 = layer->by1 < mask->by1 ? layer->by1 : mask->by1;
    bool any = false;
    for (int y = y0; y < y1; y++) {
        uint32_t* l = layer->px + (size_t)y * layer->w;
        uint32_t* m = mask->px + (size_t)y * mask->w;
        uint32_t* d = dst->px + (size_t)y * dst->w;
        for (int x = x0; x < x1; x++) {
            uint32_t a = m[x] >> 24;
            if (!a || !l[x]) continue;
            d[x] = over(scale_px(l[x], a), d[x]);
            any = true;
        }
    }
    if (any) touch(dst, x0, y0, x1, y1);
}

static void draw_obj(Draw* dr, Target* t, Obj* o, const float* PM, const CX* pcx) {
    DispExt* d = o->d;
    if (!d->visible || d->removed) return;
    float M[6];
    mul6(PM, d->m, M);
    CX cx;
    cx_combine(pcx, d, &cx);
    if (cx.m[3] <= 0.002f && cx.a[3] <= 0) return;
    if (o->kind == OK_SHAPE) {
        if (d->ch && d->ch->type == CH_SHAPE) {
            AptGeom* g = geom_of(dr->vm, d->file, (int)d->ch->geom);
            if (g) draw_geom(dr, t, d->file, g, M, &cx);
        }
        return;
    }
    if (o->kind == OK_TEXT) { draw_text(dr, t, o, M, &cx); return; }
    if (o->kind != OK_CLIP) return;
    int n = d->nkids;
    if (!n) return;
    Obj** kids = (Obj**)malloc((size_t)n * sizeof(Obj*));
    int* depths = (int*)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) { kids[i] = d->kids[i].o; depths[i] = d->kids[i].depth; }
    for (int i = 0; i < n;) {
        Obj* c = kids[i];
        if (c->d->clipdepth) {
            /* a mask: draws nothing itself; children up to its clip depth are cut to it */
            Target* layer = layer_get(dr);
            int j = i + 1;
            while (j < n && depths[j] <= c->d->clipdepth) { draw_obj(dr, layer, kids[j], M, &cx); j++; }
            Target* mask = layer_get(dr);
            CX solid = {{1, 1, 1, 1}, {0, 0, 0, 255}};
            draw_obj(dr, mask, c, M, &solid);
            composite_masked(t, layer, mask);
            layer_put(dr, layer); layer_put(dr, mask);
            i = j;
            continue;
        }
        draw_obj(dr, t, c, M, &cx);
        i++;
    }
    free(kids); free(depths);
}

void aptdraw_levels(VM* vm, uint32_t* px, int w, int h, float scale, float ox, float oy, uint32_t bg) {
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) px[i] = bg;
    Draw dr;
    memset(&dr, 0, sizeof dr);
    dr.vm = vm; dr.scale = scale; dr.w = w; dr.h = h;
    Target t = {px, w, h, 0, 0, w, h};
    float M[6] = {scale, 0, 0, scale, ox, oy};
    CX id = {{1, 1, 1, 1}, {0, 0, 0, 0}};
    for (int lv = 0; lv < VM_LEVELS; lv++) if (vm->levels[lv]) draw_obj(&dr, &t, vm->levels[lv], M, &id);
    for (int i = 0; i < dr.npool; i++) { free(dr.pool[i]->px); free(dr.pool[i]); }
}
