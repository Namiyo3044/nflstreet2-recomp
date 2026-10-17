#include "gx/texture.h"
#include "gx/state.h"

#include <stdlib.h>
#include <string.h>

/* Texture memory, used here for colour tables (TLUTs). */
#define TMEM_SIZE (1024u * 1024u)
static u8 g_tmem[TMEM_SIZE];

enum {
    FMT_I4 = 0, FMT_I8 = 1, FMT_IA4 = 2, FMT_IA8 = 3, FMT_RGB565 = 4, FMT_RGB5A3 = 5,
    FMT_RGBA8 = 6, FMT_C4 = 8, FMT_C8 = 9, FMT_C14X2 = 10, FMT_CMPR = 14,
};

static u32 rd16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }

static u32 argb(u32 a, u32 r, u32 g, u32 b) { return (a << 24) | (r << 16) | (g << 8) | b; }

static u32 from_rgb565(u32 v) {
    u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return argb(255, (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

static u32 from_rgb5a3(u32 v) {
    if (v & 0x8000) {               /* opaque RGB555 */
        u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return argb(255, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
    }
    u32 a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;  /* ARGB3444 */
    return argb((a << 5) | (a << 2) | (a >> 1), r * 17, g * 17, b * 17);
}

static u32 from_ia8(u32 v) {        /* high byte alpha, low byte intensity */
    u32 a = v >> 8, i = v & 255;
    return argb(a, i, i, i);
}

static u32 palette(const u8* tlut, u32 tlut_fmt, u32 index) {
    if (!tlut)
        return argb(255, index & 255, index & 255, index & 255);
    u32 v = rd16(tlut + (index & 0x3FFFu) * 2u);
    switch (tlut_fmt) {
    case 0: return from_ia8(v);
    case 1: return from_rgb565(v);
    default: return from_rgb5a3(v);
    }
}

static void block_dims(u32 fmt, u32* bw, u32* bh) {
    switch (fmt) {
    case FMT_I4: case FMT_C4: case FMT_CMPR: *bw = 8; *bh = 8; break;
    case FMT_I8: case FMT_IA4: case FMT_C8: *bw = 8; *bh = 4; break;
    default: *bw = 4; *bh = 4; break;
    }
}

u32 gx_tex_size(u32 fmt, u32 w, u32 h) {
    u32 bw, bh;
    block_dims(fmt, &bw, &bh);
    u32 blocks = ((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    return blocks * (fmt == FMT_RGBA8 ? 64u : 32u);
}

static void decode_cmpr_sub(const u8* p, u32* out4x4) {
    u32 c0 = rd16(p), c1 = rd16(p + 2);
    u32 col[4];
    col[0] = from_rgb565(c0);
    col[1] = from_rgb565(c1);
    u32 r0 = (col[0] >> 16) & 255, g0 = (col[0] >> 8) & 255, b0 = col[0] & 255;
    u32 r1 = (col[1] >> 16) & 255, g1 = (col[1] >> 8) & 255, b1 = col[1] & 255;
    if (c0 > c1) {
        col[2] = argb(255, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
        col[3] = argb(255, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
    } else {
        col[2] = argb(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        col[3] = 0;   /* transparent */
    }
    for (u32 y = 0; y < 4; y++) {
        u8 bits = p[4 + y];
        for (u32 x = 0; x < 4; x++)
            out4x4[y * 4 + x] = col[(bits >> (6 - 2 * x)) & 3];
    }
}

bool gx_tex_decode(const u8* src, u32 fmt, u32 w, u32 h, const u8* tlut, u32 tlut_fmt, u32* out) {
    u32 bw, bh;
    block_dims(fmt, &bw, &bh);
    u32 bx_count = (w + bw - 1) / bw, by_count = (h + bh - 1) / bh;
    const u8* p = src;

    for (u32 by = 0; by < by_count; by++) {
        for (u32 bx = 0; bx < bx_count; bx++) {
            u32 block[64];
            switch (fmt) {
            case FMT_I4:
                for (u32 i = 0; i < 64; i++) {
                    u32 n = (p[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                    block[i] = argb(n * 17, n * 17, n * 17, n * 17);
                }
                p += 32;
                break;
            case FMT_I8:
                for (u32 i = 0; i < 32; i++)
                    block[i] = argb(p[i], p[i], p[i], p[i]);
                p += 32;
                break;
            case FMT_IA4:
                for (u32 i = 0; i < 32; i++) {
                    u32 a = (p[i] >> 4) * 17, v = (p[i] & 15) * 17;
                    block[i] = argb(a, v, v, v);
                }
                p += 32;
                break;
            case FMT_IA8:
                for (u32 i = 0; i < 16; i++)
                    block[i] = from_ia8(rd16(p + i * 2));
                p += 32;
                break;
            case FMT_RGB565:
                for (u32 i = 0; i < 16; i++)
                    block[i] = from_rgb565(rd16(p + i * 2));
                p += 32;
                break;
            case FMT_RGB5A3:
                for (u32 i = 0; i < 16; i++)
                    block[i] = from_rgb5a3(rd16(p + i * 2));
                p += 32;
                break;
            case FMT_RGBA8:   /* AR pairs for 16 texels, then GB pairs */
                for (u32 i = 0; i < 16; i++)
                    block[i] = argb(p[i * 2], p[i * 2 + 1], p[32 + i * 2], p[32 + i * 2 + 1]);
                p += 64;
                break;
            case FMT_C4:
                for (u32 i = 0; i < 64; i++)
                    block[i] = palette(tlut, tlut_fmt, (p[i / 2] >> ((i & 1) ? 0 : 4)) & 15);
                p += 32;
                break;
            case FMT_C8:
                for (u32 i = 0; i < 32; i++)
                    block[i] = palette(tlut, tlut_fmt, p[i]);
                p += 32;
                break;
            case FMT_C14X2:
                for (u32 i = 0; i < 16; i++)
                    block[i] = palette(tlut, tlut_fmt, rd16(p + i * 2) & 0x3FFFu);
                p += 32;
                break;
            case FMT_CMPR: {  /* 8x8 = four 4x4 DXT1 sub-blocks: TL, TR, BL, BR */
                u32 sub[16];
                for (u32 s = 0; s < 4; s++) {
                    decode_cmpr_sub(p + s * 8, sub);
                    u32 ox = (s & 1) * 4, oy = (s >> 1) * 4;
                    for (u32 y = 0; y < 4; y++)
                        for (u32 x = 0; x < 4; x++)
                            block[(oy + y) * 8 + ox + x] = sub[y * 4 + x];
                }
                p += 32;
                break;
            }
            default:
                return false;
            }
            for (u32 y = 0; y < bh; y++) {
                u32 ty = by * bh + y;
                if (ty >= h)
                    break;
                for (u32 x = 0; x < bw; x++) {
                    u32 tx = bx * bw + x;
                    if (tx < w)
                        out[ty * w + tx] = block[y * bw + x];
                }
            }
        }
    }
    return true;
}

void gx_tex_load_tlut(CPUState* cpu, u32 src_reg, u32 dest_reg) {
    u32 src = (src_reg << 5) & 0x01FFFFE0u;
    u32 tmem = (dest_reg & 0x3FFu) << 9;
    u32 bytes = ((dest_reg >> 10) & 0x7FFu) * 32u;
    if (src >= cpu->ram_size || bytes > cpu->ram_size - src || tmem + bytes > TMEM_SIZE)
        return;
    memcpy(g_tmem + tmem, cpu->ram + src, bytes);
}

/* ------------------------------------------------------------- cache --- */
typedef struct {
    u32 addr, fmt, w, h, tlut_addr, tlut_fmt;
    GxTexture tex;
    u32* data;
} CacheEntry;

#define CACHE_MAX 512
static CacheEntry g_cache[CACHE_MAX];
static u32 g_cache_count;

void gx_tex_flush_cache(void) {
    for (u32 i = 0; i < g_cache_count; i++)
        free(g_cache[i].data);
    g_cache_count = 0;
}

const GxTexture* gx_tex_get(CPUState* cpu, u32 map) {
    /* Register sets for maps 0-3 and 4-7 live 0x20 apart. */
    u32 bank = map < 4 ? 0u : 0x20u, i = map & 3u;
    u32 image0 = gx_bp[0x88 + bank + i];
    u32 image3 = gx_bp[0x94 + bank + i];
    u32 tlut = gx_bp[0x98 + bank + i];

    u32 w = (image0 & 0x3FFu) + 1, h = ((image0 >> 10) & 0x3FFu) + 1;
    u32 fmt = (image0 >> 20) & 0xFu;
    u32 addr = (image3 << 5) & 0x01FFFFE0u;
    bool paletted = fmt == FMT_C4 || fmt == FMT_C8 || fmt == FMT_C14X2;
    u32 tlut_addr = paletted ? (tlut & 0x3FFu) << 9 : 0;
    u32 tlut_fmt = paletted ? (tlut >> 10) & 3u : 0;

    for (u32 k = 0; k < g_cache_count; k++) {
        CacheEntry* e = &g_cache[k];
        if (e->addr == addr && e->fmt == fmt && e->w == w && e->h == h &&
            e->tlut_addr == tlut_addr && e->tlut_fmt == tlut_fmt)
            return &e->tex;
    }

    u32 size = gx_tex_size(fmt, w, h);
    if (addr >= cpu->ram_size || size > cpu->ram_size - addr || g_cache_count >= CACHE_MAX)
        return NULL;
    u32* data = (u32*)malloc((size_t)w * h * 4);
    if (!data)
        return NULL;
    if (!gx_tex_decode(cpu->ram + addr, fmt, w, h, paletted ? g_tmem + tlut_addr : NULL, tlut_fmt, data)) {
        free(data);
        return NULL;
    }
    CacheEntry* e = &g_cache[g_cache_count++];
    e->addr = addr; e->fmt = fmt; e->w = w; e->h = h;
    e->tlut_addr = tlut_addr; e->tlut_fmt = tlut_fmt;
    e->data = data;
    e->tex.width = w;
    e->tex.height = h;
    e->tex.texels = data;
    return &e->tex;
}
