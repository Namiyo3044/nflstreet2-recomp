#include "runtime/prof.h"
#include "gx/texture.h"
#include "gx/state.h"

#include <stdio.h>
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

/* Bumped on every write to TMEM (palette loads and texture preloads). A
   texture that reads TMEM is re-checked on its next use, even within a frame
   (games reload a palette between two draws of the same texture, e.g.
   outline then fill). */
static u32 g_tlut_generation = 1;

/* Change stamps for TMEM in 512-byte blocks: every preload or palette load
   takes a new stamp and marks the blocks it wrote. A cached texture can then
   tell whether its palette or preloaded data was touched without re-hashing. */
#define TMEM_BLOCK 512u
static u32 g_tmem_stamp = 1;
static u32 g_tmem_block[TMEM_SIZE / TMEM_BLOCK];

static void tmem_mark(u32 addr, u32 bytes) {
    if (!bytes || addr >= TMEM_SIZE)
        return;
    u32 end = addr + bytes > TMEM_SIZE ? TMEM_SIZE : addr + bytes;
    for (u32 b = addr / TMEM_BLOCK; b <= (end - 1) / TMEM_BLOCK; b++)
        g_tmem_block[b] = g_tmem_stamp;
}

static u32 tmem_region_stamp(u32 addr, u32 bytes) {
    if (!bytes || addr >= TMEM_SIZE)
        return 0;
    u32 end = addr + bytes > TMEM_SIZE ? TMEM_SIZE : addr + bytes;
    u32 m = 0;
    for (u32 b = addr / TMEM_BLOCK; b <= (end - 1) / TMEM_BLOCK; b++)
        if (g_tmem_block[b] > m) m = g_tmem_block[b];
    return m;
}

void gx_tex_preload(CPUState* cpu) {
    u32 src = (gx_bp[0x60] << 5) & 0x01FFFFE0u;
    u32 even = (gx_bp[0x61] & 0x7FFFu) * 32u, odd = (gx_bp[0x62] & 0x7FFFu) * 32u;
    u32 lines = gx_bp[0x63] & 0x7FFFu, type = (gx_bp[0x63] >> 15) & 3u;
    g_tmem_stamp++;
    if (type == 3) {   /* RGBA8: 64-byte tiles split into AR (even bank) and GB (odd bank) halves */
        tmem_mark(even, lines * 32u);
        tmem_mark(odd, lines * 32u);
        for (u32 i = 0; i < lines; i++, src += 64, even += 32, odd += 32) {
            if (src + 64 > cpu->ram_size || even + 32 > TMEM_SIZE || odd + 32 > TMEM_SIZE)
                break;
            memcpy(g_tmem + even, cpu->ram + src, 32);
            memcpy(g_tmem + odd, cpu->ram + src + 32, 32);
        }
    } else {
        u32 bytes = lines * 32u;
        tmem_mark(even, bytes);
        if (src < cpu->ram_size && bytes <= cpu->ram_size - src && even + bytes <= TMEM_SIZE)
            memcpy(g_tmem + even, cpu->ram + src, bytes);
    }
    g_tlut_generation++;
}

void gx_tex_load_tlut(CPUState* cpu, u32 src_reg, u32 dest_reg) {
    u32 src = (src_reg << 5) & 0x01FFFFE0u;
    u32 tmem = (dest_reg & 0x3FFu) << 9;
    u32 bytes = ((dest_reg >> 10) & 0x7FFu) * 32u;
    if (src >= cpu->ram_size || bytes > cpu->ram_size - src || tmem + bytes > TMEM_SIZE)
        return;
    memcpy(g_tmem + tmem, cpu->ram + src, bytes);
    g_tmem_stamp++;
    tmem_mark(tmem, bytes);
    g_tlut_generation++;               /* paletted textures re-check their palette on next use */
}

/* ------------------------------------------------------------- cache --- */
/* Decoded textures are kept across frames. Once per frame, the first time an
   entry is used its source bytes (and colour table) are hashed; it is only
   decoded again if they changed. Least recently used entries are dropped
   when the cache is full. */
typedef struct {
    u32 addr, fmt, w, h, tlut_addr, tlut_fmt, levels;
    u64 hash;                          /* texture bytes (sampled for big RAM textures) */
    u64 full_hash;                     /* all the bytes, for big RAM textures */
    u64 tlut_hash;                     /* palette bytes (paletted formats) */
    u32 tlut_gen;                      /* g_tlut_generation when last checked */
    u32 pal_stamp, data_stamp;
    bool overwritten;                  /* RAM rewritten by a frame-buffer copy: decode again in place */         /* TMEM stamps when palette / preloaded data were last checked */
    u32 checked_frame, used_frame;
    GxTexture tex;
    u32* data;
} CacheEntry;

#define CACHE_MAX 2048
static CacheEntry g_cache[CACHE_MAX];
static u32 g_cache_count;
static u32 g_frame = 1;

/* Index from a texture's key to its cache entry (open addressing). Rebuilt
   whenever an entry is created or replaced. */
#define INDEX_SIZE 4096u
static int g_index[INDEX_SIZE];
static bool g_index_ready;

static u32 key_hash(u32 addr, u32 fmt, u32 w, u32 h, u32 tlut_addr, u32 tlut_fmt, u32 levels) {
    u32 k = addr * 2654435761u;
    k ^= (fmt | (tlut_fmt << 4) | (levels << 8)) * 2246822519u;
    k ^= (w | (h << 11)) * 3266489917u;
    k ^= tlut_addr * 668265263u;
    k ^= k >> 15;
    return k & (INDEX_SIZE - 1u);
}
static unsigned long long g_decodes, g_hits, g_variants, g_variant_hits;

static u64 hash_bytes(const u8* p, u32 n) {
    u64 h = 0x9E3779B97F4A7C15ull ^ n;
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* Big textures in RAM are checked once a frame by sampling: 32-byte pieces
   spread over the whole texture plus its start and end, about 8 KB in all.
   Every 32 frames each one gets a full hash, so a change the samples miss
   still shows within about half a second. */
static u64 hash_sampled(const u8* p, u32 n) {
    if (n <= 16384u)
        return hash_bytes(p, n);
    u64 h = 0x9E3779B97F4A7C15ull ^ n;
    const u32 pieces = 192;
    u32 step = (n - 32u) / pieces;
    for (u32 k = 0; k < pieces; k++) {
        u64 w[4];
        memcpy(w, p + k * step, 32);
        for (int j = 0; j < 4; j++) { h = (h ^ w[j]) * 0xFF51AFD7ED558CCDull; h ^= h >> 29; }
    }
    h ^= hash_bytes(p, 1024) * 3u;
    h ^= hash_bytes(p + n - 1024, 1024) * 5u;
    return h;
}

static u32 tlut_bytes(u32 fmt) {
    return fmt == FMT_C4 ? 16u * 2u : fmt == FMT_C8 ? 256u * 2u : 16384u * 2u;
}

static u64 palette_hash(u32 fmt, u32 tlut_addr) {
    u32 n = tlut_bytes(fmt);
    return tlut_addr + n <= TMEM_SIZE ? hash_bytes(g_tmem + tlut_addr, n) : 0;
}


/* Decoded buffers that were replaced or evicted while a frame that uses them
   may still be drawing; freed by gx_tex_collect_garbage() once it is done. */
/* Two stages: buffers retired while recording the current frame may still be
   used by that frame's draws, so they wait one more frame before being freed. */
static u32** g_garbage;                /* retired during the frame being recorded */
static u32 g_garbage_n, g_garbage_cap;
static u32** g_garbage_old;            /* retired during the previous frame */
static u32 g_garbage_old_n, g_garbage_old_cap;

static void retire(u32* data) {
    if (!data)
        return;
    if (g_garbage_n == g_garbage_cap) {
        u32 cap = g_garbage_cap ? g_garbage_cap * 2 : 64;
        u32** g = (u32**)realloc(g_garbage, sizeof(u32*) * cap);
        if (!g) {
            return;                    /* leak rather than free memory in use */
        }
        g_garbage = g;
        g_garbage_cap = cap;
    }
    g_garbage[g_garbage_n++] = data;
}

static void (*g_free_hook)(const u32* texels);

void gx_tex_set_free_hook(void (*hook)(const u32* texels)) {
    g_free_hook = hook;
}

void gx_tex_collect_garbage(void) {
    /* Called when the previous frame has been drawn and the one just recorded
       is about to be: the older stage is no longer used by anything. */
    for (u32 i = 0; i < g_garbage_old_n; i++) {
        if (g_free_hook)
            g_free_hook(g_garbage_old[i]);
        free(g_garbage_old[i]);
    }
    u32** t = g_garbage_old; g_garbage_old = g_garbage; g_garbage = t;
    u32 tc = g_garbage_old_cap; g_garbage_old_cap = g_garbage_cap; g_garbage_cap = tc;
    g_garbage_old_n = g_garbage_n;
    g_garbage_n = 0;
}

/* RAM ranges frame-buffer copies have written: textures there are the
   game's own renders, never replaced or dumped (as in Dolphin). */
#define COPY_RANGES 64
static u32 g_copy_lo[COPY_RANGES], g_copy_hi[COPY_RANGES], g_copy_n, g_copy_next;

static bool copy_written(u32 addr, u32 size) {
    for (u32 i = 0; i < g_copy_n; i++)
        if (addr < g_copy_hi[i] && g_copy_lo[i] < addr + size) return true;
    return false;
}

void gx_tex_invalidate(u32 addr, u32 size) {
    addr &= 0x01FFFFFFu;
    bool known = false;
    for (u32 i = 0; i < g_copy_n && !known; i++)
        known = g_copy_lo[i] == addr && g_copy_hi[i] == addr + size;
    if (!known) {
        g_copy_lo[g_copy_next] = addr;
        g_copy_hi[g_copy_next] = addr + size;
        g_copy_next = (g_copy_next + 1) % COPY_RANGES;
        if (g_copy_n < COPY_RANGES) g_copy_n++;
    }
    for (u32 n = 0; n < g_cache_count; n++) {
        CacheEntry* e = &g_cache[n];
        if (e->addr & 0x80000000u)
            continue;                  /* TMEM-preloaded or unused */
        u32 bytes = e->tex.width && e->tex.height ? gx_tex_size(e->fmt, e->w, e->h) : 0;
        if (e->addr < addr + size && addr < e->addr + bytes)
            e->overwritten = true;
    }
}

/* Called once per displayed frame: starts a new validation round. */
static void retire_hires(u32* texels) {
    retire(texels);
}

void gx_tex_reload_hires(void) {
    hires_rescan(retire_hires);
    /* Every texture is decoded again on its next use, looking up its
       replacement afresh. */
    for (u32 n = 0; n < g_cache_count; n++) {
        CacheEntry* e = &g_cache[n];
        if (e->addr == 0xFFFFFFFFu) continue;
        retire(e->data);
        e->data = NULL;
        e->tex.texels = NULL;
        e->tex.hires = NULL;
        e->tex.hires_entry = NULL;
        e->addr = 0xFFFFFFFFu;
        e->used_frame = 0;
    }
    g_index_ready = false;
}

void gx_tex_flush_cache(void) {
    g_frame++;
}

void gx_tex_report(void) {
    printf("textures: %u cached, %llu decodes, %llu cache hits (%llu versions of a texture kept side by side, "
           "%llu switches between them)\n", g_cache_count, g_decodes, g_hits, g_variants, g_variant_hits);
    hires_report();
}

/* Lays out the mip chain: level l is (w >> l) x (h >> l), at least 1x1,
   stored in RAM right after level l-1. Returns the bytes in RAM and fills the
   level sizes and decoded offsets; *texels gets the decoded total. */
static u32 mip_layout(u32 fmt, u32 w, u32 h, u32 levels, GxTexture* t, u32* texels) {
    u32 bytes = 0, total = 0;
    for (u32 l = 0; l < levels; l++) {
        u32 lw = w >> l, lh = h >> l;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        t->lw[l] = lw;
        t->lh[l] = lh;
        t->off[l] = total;
        total += lw * lh;
        bytes += gx_tex_size(fmt, lw, lh);
    }
    *texels = total;
    return bytes;
}

static bool decode_levels(const u8* base, u32 fmt, const GxTexture* t, const u8* tlut, u32 tlut_fmt, u32* out) {
    u32 src = 0;
    for (u32 l = 0; l < t->levels; l++) {
        if (!gx_tex_decode(base + src, fmt, t->lw[l], t->lh[l], tlut, tlut_fmt, out + t->off[l]))
            return false;
        src += gx_tex_size(fmt, t->lw[l], t->lh[l]);
    }
    return true;
}

/* Preloaded RGBA8 lives in two TMEM banks; rebuild the RAM tile layout
   (32 bytes AR, then 32 bytes GB, per tile) so the normal decoder can read it. */
static u8* g_rgba8_scratch;
static u32 g_rgba8_scratch_size;

static const u8* rgba8_from_tmem(u32 even, u32 odd, u32 size) {
    if (size > g_rgba8_scratch_size) {
        u8* p = (u8*)realloc(g_rgba8_scratch, size);
        if (!p) return NULL;
        g_rgba8_scratch = p;
        g_rgba8_scratch_size = size;
    }
    for (u32 i = 0; i * 64 < size; i++) {
        u32 e = even + i * 32, o = odd + i * 32;
        if (e + 32 > TMEM_SIZE || o + 32 > TMEM_SIZE) return NULL;
        memcpy(g_rgba8_scratch + i * 64, g_tmem + e, 32);
        memcpy(g_rgba8_scratch + i * 64 + 32, g_tmem + o, 32);
    }
    return g_rgba8_scratch;
}

static void index_rebuild(void) {
    for (u32 i = 0; i < INDEX_SIZE; i++) g_index[i] = -1;
    for (u32 n = 0; n < g_cache_count; n++) {
        const CacheEntry* c = &g_cache[n];
        if (c->addr == 0xFFFFFFFFu)
            continue;
        u32 k = key_hash(c->addr, c->fmt, c->w, c->h, c->tlut_addr, c->tlut_fmt, c->levels);
        while (g_index[k] >= 0) k = (k + 1u) & (INDEX_SIZE - 1u);
        g_index[k] = (int)n;
    }
    g_index_ready = true;
}

/* Looks up a custom replacement for a freshly decoded texture (hashed the
   way Dolphin does: level 0 as it sits in RAM, or in TMEM when preloaded). */
static void attach_hires(CacheEntry* e, const u8* src, bool preloaded, u32 even, u32 mode0) {
    e->tex.hires = NULL;
    e->tex.hires_entry = NULL;
    if (!hires_active())
        return;
    u32 fmt = e->fmt, w = e->w, h = e->h;
    u32 size0 = gx_tex_size(fmt, w, h);
    if (!preloaded && copy_written(e->addr, size0))
        return;
    const u8* data = src;
    if (preloaded && fmt == FMT_RGBA8)
        data = even + size0 <= TMEM_SIZE ? g_tmem + even : NULL;
    if (!data)
        return;
    bool paletted = fmt == FMT_C4 || fmt == FMT_C8 || fmt == FMT_C14X2;
    const u8* tlut = paletted && e->tlut_addr + tlut_bytes(fmt) <= TMEM_SIZE ? g_tmem + e->tlut_addr : NULL;
    if (paletted && !tlut)
        return;
    bool mipmaps = ((mode0 >> 5) & 3u) != 0;
    e->tex.hires_entry = hires_find(fmt, w, h, mipmaps, data, size0, tlut, e->data);
    e->tex.hires = hires_ready(e->tex.hires_entry);
}

static const GxTexture* tex_get(CPUState* cpu, u32 map) {
    /* Register sets for maps 0-3 and 4-7 live 0x20 apart. */
    u32 bank = map < 4 ? 0u : 0x20u, i = map & 3u;
    u32 mode0 = gx_bp[0x80 + bank + i];
    u32 mode1 = gx_bp[0x84 + bank + i];
    u32 image0 = gx_bp[0x88 + bank + i];
    u32 image1 = gx_bp[0x8C + bank + i];
    u32 image2 = gx_bp[0x90 + bank + i];
    u32 image3 = gx_bp[0x94 + bank + i];
    u32 tlut = gx_bp[0x98 + bank + i];
    bool preloaded = (image1 >> 21) & 1u;      /* read from TMEM, not RAM */

    u32 w = (image0 & 0x3FFu) + 1, h = ((image0 >> 10) & 0x3FFu) + 1;
    u32 fmt = (image0 >> 20) & 0xFu;
    u32 addr = (image3 << 5) & 0x01FFFFE0u;
    bool paletted = fmt == FMT_C4 || fmt == FMT_C8 || fmt == FMT_C14X2;
    u32 tlut_addr = paletted ? (tlut & 0x3FFu) << 9 : 0;
    u32 tlut_fmt = paletted ? (tlut >> 10) & 3u : 0;

    /* Mip levels: only when the minification filter uses them; the count
       comes from the maximum LOD (4.4 fixed point) and the texture size. */
    u32 levels = 1;
    u32 min_filter = gx_tex_min_filter(mode0);
    if (min_filter >= 2) {
        u32 max_lod = ((mode1 >> 8) & 0xFFu) >> 4;
        u32 n = 1, m = w > h ? w : h;
        while ((m >> n) && n < GX_TEX_MAX_LEVELS) n++;
        levels = max_lod + 1 < n ? max_lod + 1 : n;
    }

    GxTexture layout;
    u32 texel_count;
    layout.levels = levels;
    u32 size = mip_layout(fmt, w, h, levels, &layout, &texel_count);
    u32 even = 0, odd = 0;
    if (preloaded) {
        even = (image1 & 0x7FFFu) * 32u;
        odd = (image2 & 0x7FFFu) * 32u;
        addr = 0x80000000u | even;             /* cache key: TMEM, not a RAM address */
        if (fmt == FMT_RGBA8 ? (even + size / 2 > TMEM_SIZE || odd + size / 2 > TMEM_SIZE) : even + size > TMEM_SIZE)
            return NULL;
    } else if (addr >= cpu->ram_size || size > cpu->ram_size - addr) {
        return NULL;
    }
    u32 pal_bytes = paletted ? tlut_bytes(fmt) : 0;

    /* Find the entry: the one this map used last time, else the index. */
    static u32 last_hit[8] = {0};
    CacheEntry* e = NULL;
    u32 slot_found = 0;
#define SAME(c) ((c)->addr == addr && (c)->fmt == fmt && (c)->w == w && (c)->h == h && \
                 (c)->tlut_addr == tlut_addr && (c)->tlut_fmt == tlut_fmt && (c)->levels == levels)
    CacheEntry* c = &g_cache[last_hit[map & 7u]];
    if (last_hit[map & 7u] < g_cache_count && SAME(c)) {
        e = c;
        slot_found = last_hit[map & 7u];
    } else {
        if (!g_index_ready)
            index_rebuild();
        for (u32 k = key_hash(addr, fmt, w, h, tlut_addr, tlut_fmt, levels), n = 0; n < INDEX_SIZE;
             k = (k + 1u) & (INDEX_SIZE - 1u), n++) {
            int idx = g_index[k];
            if (idx < 0)
                break;
            c = &g_cache[idx];
            if (SAME(c)) {
                e = c;
                slot_found = (u32)idx;
                last_hit[map & 7u] = (u32)idx;
                break;
            }
        }
    }
#undef SAME

    /* The texture's bytes, fetched only when they have to be hashed or decoded. */
    const u8* src = NULL;
#define SRC() (src ? src : (src = preloaded ? (fmt == FMT_RGBA8 ? rgba8_from_tmem(even, odd, size) : g_tmem + even) \
                                         : cpu->ram + addr))

    if (e) {
        e->used_frame = g_frame;
        bool data_ok, pal_ok;
        u64 hsh = e->hash, tsh = e->tlut_hash;
        if (preloaded) {
            /* Preloaded data changes only through TMEM loads. */
            u32 st = fmt == FMT_RGBA8 ? tmem_region_stamp(even, size / 2) : tmem_region_stamp(even, size);
            if (fmt == FMT_RGBA8) { u32 st2 = tmem_region_stamp(odd, size / 2); if (st2 > st) st = st2; }
            data_ok = st <= e->data_stamp;
            if (!data_ok) {
                if (!SRC()) return NULL;
                hsh = hash_bytes(src, size);
                e->data_stamp = g_tmem_stamp;
            }
        } else if (e->overwritten) {
            /* Rewritten by a frame-buffer copy: decode again into this entry. */
            e->overwritten = false;
            e->checked_frame = g_frame;
            if (!SRC()) return NULL;
            u32* fresh = (u32*)malloc((size_t)texel_count * 4);
            if (!fresh || !decode_levels(src, fmt, &e->tex, paletted ? g_tmem + tlut_addr : NULL, tlut_fmt, fresh)) {
                free(fresh);
                return NULL;
            }
            retire(e->data);
            e->data = fresh;
            e->tex.texels = fresh;
            e->hash = hash_sampled(src, size);
            e->full_hash = size > 16384u ? hash_bytes(src, size) : 0;
            e->tex.hires = NULL;           /* drawn by the GPU: never replaced */
            e->tex.hires_entry = NULL;
            g_decodes++;
            return &e->tex;
        } else {
            data_ok = e->checked_frame == g_frame;
            if (!data_ok) {
                /* Sampled hash every frame; the full hash every 32nd frame
                   (staggered across entries). */
                if (!SRC()) return NULL;
                hsh = hash_sampled(src, size);
                if (hsh == e->hash && ((g_frame + slot_found) & 31u) == 0 && size > 16384u &&
                    hash_bytes(src, size) != e->full_hash)
                    hsh = ~e->hash;            /* the samples missed a change */
                e->checked_frame = g_frame;
            }
        }
        if (paletted) {
            pal_ok = tmem_region_stamp(tlut_addr, pal_bytes) <= e->pal_stamp;
            if (!pal_ok) {
                tsh = palette_hash(fmt, tlut_addr);
                e->pal_stamp = g_tmem_stamp;
            }
        } else {
            pal_ok = true;
        }
        if ((data_ok && pal_ok) || (hsh == e->hash && tsh == e->tlut_hash)) {
            g_hits++;
            return &e->tex;
        }
        /* Different contents under the same address: often the same texture
           drawn with another palette (team colours loaded into one palette
           slot), or one buffer reused for several images. Keep each version
           as its own entry instead of decoding again on every switch. */
        bool sampled_only_change = !preloaded && hsh == ~e->hash;
        if (!sampled_only_change) {
            if (!g_index_ready)
                index_rebuild();
            for (u32 k = key_hash(addr, fmt, w, h, tlut_addr, tlut_fmt, levels), n = 0; n < INDEX_SIZE;
                 k = (k + 1u) & (INDEX_SIZE - 1u), n++) {
                int idx = g_index[k];
                if (idx < 0)
                    break;
                CacheEntry* v = &g_cache[idx];
                if (v == e || v->addr != addr || v->fmt != fmt || v->w != w || v->h != h ||
                    v->tlut_addr != tlut_addr || v->tlut_fmt != tlut_fmt || v->levels != levels)
                    continue;
                if (v->hash == hsh && v->tlut_hash == tsh) {
                    v->used_frame = v->checked_frame = g_frame;
                    v->pal_stamp = v->data_stamp = g_tmem_stamp;
                    last_hit[map & 7u] = (u32)idx;
                    g_hits++;
                    g_variant_hits++;
                    return &v->tex;
                }
            }
            if (g_cache_count < CACHE_MAX) {
                g_variants++;
                goto new_entry;
            }
        }
        /* Changed: decode into a new buffer; the old one may still be in use
           by the frame being drawn. */
        if (!SRC()) return NULL;
        u32* fresh = (u32*)malloc((size_t)texel_count * 4);
        if (!fresh || !decode_levels(src, fmt, &e->tex, paletted ? g_tmem + tlut_addr : NULL, tlut_fmt, fresh)) {
            free(fresh);
            return NULL;
        }
        retire(e->data);
        e->data = fresh;
        e->tex.texels = fresh;
        e->hash = preloaded ? hsh : hash_sampled(src, size);
        e->full_hash = !preloaded && size > 16384u ? hash_bytes(src, size) : 0;
        e->tlut_hash = tsh;
        g_decodes++;
        attach_hires(e, src, preloaded, even, mode0);
        return &e->tex;
    }

new_entry:
    /* New entry: take a free slot, or the least recently used one. */
    if (!SRC()) return NULL;
    u32 slot = g_cache_count;
    if (slot >= CACHE_MAX) {
        slot = 0;
        for (u32 k = 1; k < CACHE_MAX; k++)
            if (g_cache[k].used_frame < g_cache[slot].used_frame)
                slot = k;
        if (g_cache[slot].used_frame == g_frame)
            return NULL;               /* every entry is in use this frame */
        retire(g_cache[slot].data);
    } else {
        g_cache_count++;
    }
    e = &g_cache[slot];
    memset(e, 0, sizeof(*e));
    g_index_ready = false;
    e->tex = layout;
    e->tex.width = w;
    e->tex.height = h;
    e->data = (u32*)malloc((size_t)texel_count * 4);
    if (!e->data || !decode_levels(src, fmt, &e->tex, paletted ? g_tmem + tlut_addr : NULL, tlut_fmt, e->data)) {
        free(e->data);             /* never handed out */
        e->data = NULL;
        e->addr = 0xFFFFFFFFu;     /* matches nothing; reused later */
        return NULL;
    }
    e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->levels = levels;
    e->tlut_addr = tlut_addr; e->tlut_fmt = tlut_fmt;
    e->hash = preloaded ? hash_bytes(src, size) : hash_sampled(src, size);
    e->full_hash = !preloaded && size > 16384u ? hash_bytes(src, size) : 0;
    e->tlut_hash = paletted ? palette_hash(fmt, tlut_addr) : 0;
    e->checked_frame = e->used_frame = g_frame;
    e->tlut_gen = g_tlut_generation;
    e->pal_stamp = e->data_stamp = g_tmem_stamp;
    e->tex.texels = e->data;
    last_hit[map & 7u] = slot;
    g_decodes++;
    attach_hires(e, src, preloaded, even, mode0);
    return &e->tex;
#undef SRC
}

const GxTexture* gx_tex_get(CPUState* cpu, u32 map) {
    int prev = prof_enter(PROF_GX_TEXTURE);
    GxTexture* t = (GxTexture*)tex_get(cpu, map);
    if (t && t->hires_entry && !t->hires)
        t->hires = hires_ready(t->hires_entry);   /* finished loading in the background */
    prof_leave(prev);
    return t;
}
