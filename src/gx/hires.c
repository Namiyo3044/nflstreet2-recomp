#include "gx/hires.h"
#include "gx/thread.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <dirent.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "third_party/bcdec.h"

static char* dup_str(const char* s) {   /* (strdup, without Windows' deprecation warning) */
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ------------------------------------------------------------- XXH64 --- */
#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull

static u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static u64 rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }   /* little-endian PC */
static u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 xround(u64 acc, u64 in) { acc += in * P2; acc = rotl64(acc, 31); return acc * P1; }
static u64 xmerge(u64 acc, u64 v) { acc ^= xround(0, v); return acc * P1 + P4; }

u64 hires_xxh64(const void* data, size_t len) {
    const u8* p = (const u8*)data;
    const u8* end = p + len;
    u64 h;
    if (len >= 32) {
        u64 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
        const u8* limit = end - 32;
        do {
            v1 = xround(v1, rd64(p)); v2 = xround(v2, rd64(p + 8));
            v3 = xround(v3, rd64(p + 16)); v4 = xround(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xmerge(h, v1); h = xmerge(h, v2); h = xmerge(h, v3); h = xmerge(h, v4);
    } else {
        h = P5;
    }
    h += (u64)len;
    for (; p + 8 <= end; p += 8) {
        h ^= xround(0, rd64(p));
        h = rotl64(h, 27) * P1 + P4;
    }
    if (p + 4 <= end) {
        h ^= (u64)rd32(p) * P1;
        h = rotl64(h, 23) * P2 + P3;
        p += 4;
    }
    for (; p < end; p++) {
        h ^= (u64)(*p) * P5;
        h = rotl64(h, 11) * P1;
    }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

/* -------------------------------------------------------------- names --- */
enum { FMT_C4 = 8, FMT_C8 = 9, FMT_C14X2 = 10 };

/* Dolphin hashes only the part of the palette the texture's indices use. */
static void used_palette(u32 fmt, const u8* data, u32 size, const u8* tlut, const u8** pal, u32* pal_size) {
    *pal = NULL;
    *pal_size = 0;
    if (!tlut || (fmt != FMT_C4 && fmt != FMT_C8 && fmt != FMT_C14X2))
        return;
    u32 lo = 0xFFFF, hi = 0;
    if (fmt == FMT_C4) {
        for (u32 i = 0; i < size; i++) {
            u32 a = data[i] >> 4, b = data[i] & 15u;
            if (a < lo) lo = a;
            if (a > hi) hi = a;
            if (b < lo) lo = b;
            if (b > hi) hi = b;
        }
    } else if (fmt == FMT_C8) {
        for (u32 i = 0; i < size; i++) {
            if (data[i] < lo) lo = data[i];
            if (data[i] > hi) hi = data[i];
        }
    } else {
        for (u32 i = 0; i + 1 < size; i += 2) {
            u32 v = (((u32)data[i] << 8) | data[i + 1]) & 0x3FFFu;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (lo > hi)
        return;
    *pal = tlut + 2u * lo;
    *pal_size = 2u * (hi + 1u - lo);
}

static void make_name(char* out, size_t cap, u32 fmt, u32 w, u32 h, bool mipmaps, u64 th, bool paletted,
                      const char* tlut_part) {
    if (paletted)
        snprintf(out, cap, "tex1_%ux%u%s_%016llx_%s_%u", w, h, mipmaps ? "_m" : "", (unsigned long long)th,
                 tlut_part, fmt);
    else
        snprintf(out, cap, "tex1_%ux%u%s_%016llx_%u", w, h, mipmaps ? "_m" : "", (unsigned long long)th, fmt);
}

void hires_name(char* out, size_t cap, u32 fmt, u32 w, u32 h, bool mipmaps, const u8* data, u32 size,
                const u8* tlut) {
    const u8* pal;
    u32 pal_size;
    used_palette(fmt, data, size, tlut, &pal, &pal_size);
    char tp[24];
    snprintf(tp, sizeof(tp), "%016llx", (unsigned long long)(pal_size ? hires_xxh64(pal, pal_size) : 0));
    make_name(out, cap, fmt, w, h, mipmaps, hires_xxh64(data, size), pal_size != 0, tp);
}

/* -------------------------------------------------------------- table --- */
struct HiresEntry {
    char* key;                         /* file name without extension or _mipN, lower case */
    char* path;                        /* level 0 file (.png or .dds) */
    char* mip_path[GX_HIRES_MAX_LEVELS];
    volatile int state;                /* 0 not loaded, 1 queued, 2 ready, 3 failed */
    bool loading;                      /* the loader thread is reading it now */
    bool stale;                        /* left behind by a rescan: drop whatever is loaded */
    GxHires img;
    u32* buf;
    HiresEntry* next_old;              /* entries left behind by a rescan */
};

static HiresEntry** g_tab;             /* open addressing on the key */
static u32 g_tab_cap, g_tab_n;
static u32* g_sizes;                   /* (w << 16) | h of every file, for a quick "none this size" */
static u32 g_sizes_cap, g_sizes_n;
static bool g_active;
static unsigned g_files, g_dds, g_found, g_loaded, g_failed, g_dumped;
static char g_dir[1024];
static unsigned long long g_bytes;
static char g_dump_dir[1024];

static u32 str_hash(const char* s) {
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

static HiresEntry** tab_slot(const char* key) {
    u32 i = str_hash(key) & (g_tab_cap - 1);
    while (g_tab[i] && strcmp(g_tab[i]->key, key) != 0) i = (i + 1) & (g_tab_cap - 1);
    return &g_tab[i];
}

static HiresEntry* tab_get(const char* key, bool create) {
    if (!g_tab_cap) {
        if (!create) return NULL;
        g_tab_cap = 1024;
        g_tab = (HiresEntry**)calloc(g_tab_cap, sizeof(*g_tab));
    }
    HiresEntry** s = tab_slot(key);
    if (*s || !create) return *s;
    if ((g_tab_n + 1) * 2 > g_tab_cap) {
        HiresEntry** old = g_tab;
        u32 oc = g_tab_cap;
        g_tab_cap *= 2;
        g_tab = (HiresEntry**)calloc(g_tab_cap, sizeof(*g_tab));
        for (u32 i = 0; i < oc; i++)
            if (old[i]) *tab_slot(old[i]->key) = old[i];
        free(old);
        s = tab_slot(key);
    }
    HiresEntry* e = (HiresEntry*)calloc(1, sizeof(*e));
    e->key = dup_str(key);
    *s = e;
    g_tab_n++;
    return e;
}

static bool size_known(u32 w, u32 h) {
    u32 k = (w << 16) | h;
    for (u32 i = 0; i < g_sizes_n; i++)
        if (g_sizes[i] == k) return true;
    return false;
}

static bool is_dds(const char* path) {
    size_t n = strlen(path);
    return n > 4 && (path[n - 4] == '.') && tolower((unsigned char)path[n - 3]) == 'd' &&
           tolower((unsigned char)path[n - 2]) == 'd' && tolower((unsigned char)path[n - 1]) == 's';
}

static void add_file(const char* dir, const char* name) {
    size_t n = strlen(name);
    if (n < 10 || strncmp(name, "tex1_", 5) != 0)
        return;
    char low[512];
    if (n >= sizeof(low)) return;
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)name[i]);
    bool dds = strcmp(low + n - 4, ".dds") == 0;
    if (!dds && strcmp(low + n - 4, ".png") != 0) return;
    low[n - 4] = 0;
    int level = 0;
    char* m = strstr(low, "_mip");
    if (m && m[4] >= '0' && m[4] <= '9') {
        level = atoi(m + 4);
        *m = 0;
        if (level <= 0 || level >= GX_HIRES_MAX_LEVELS) return;
    }
    unsigned w, h;
    if (sscanf(low, "tex1_%ux%u", &w, &h) != 2) return;
    HiresEntry* e = tab_get(low, true);
    if (!e) return;
    size_t pl = strlen(dir) + n + 2;
    char* path = (char*)malloc(pl);
    snprintf(path, pl, "%s/%s", dir, name);
    /* With both a .png and a .dds of the same texture, the .dds is used. */
    char** slot = level == 0 ? &e->path : &e->mip_path[level];
    if (*slot && is_dds(*slot) && !dds) { free(path); return; }
    bool first = !*slot;
    free(*slot);
    *slot = path;
    if (level == 0 && first) {
        g_files++;
        if (dds) g_dds++;
        if (!size_known(w, h)) {
            if (g_sizes_n == g_sizes_cap) {
                g_sizes_cap = g_sizes_cap ? g_sizes_cap * 2 : 64;
                g_sizes = (u32*)realloc(g_sizes, g_sizes_cap * sizeof(u32));
            }
            g_sizes[g_sizes_n++] = (w << 16) | h;
        }
    }
}

static void scan(const char* dir, int depth) {
    if (depth > 8) return;
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[1024];
            snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
            scan(sub, depth + 1);
        } else {
            add_file(dir, fd.cFileName);
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan(sub, depth + 1);
        else add_file(dir, de->d_name);
    }
    closedir(d);
#endif
}

void hires_init(const char* dir) {
    if (!dir || !*dir) return;
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    scan(dir, 0);
    g_active = g_files > 0 || g_dump_dir[0];
    printf("custom textures: %u found in %s (%u .dds)\n", g_files, dir, g_dds);
}

bool hires_has_folder(void) { return g_dir[0] != 0; }

void hires_set_dump_dir(const char* dir) {
    if (!dir || !*dir) return;
    snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", dir);
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    g_active = true;
}

bool hires_active(void) { return g_active; }

static volatile bool g_enabled = true;
void hires_set_enabled(bool on) { g_enabled = on; }
bool hires_enabled(void) { return g_enabled; }
unsigned hires_file_count(void) { return g_files; }

/* ------------------------------------------------------------- loader --- */
static gx_mutex g_mx;
static gx_cond g_cv;
static HiresEntry** g_queue;
static u32 g_q_head, g_q_n, g_q_cap;
static bool g_thread_started;

static u32* load_png(const char* path, int* w, int* h) {
    int n;
    stbi_uc* px = stbi_load(path, w, h, &n, 4);
    if (!px) return NULL;
    size_t count = (size_t)(*w) * (size_t)(*h);
    u32* out = (u32*)malloc(count * 4);
    if (out)
        for (size_t i = 0; i < count; i++) {
            const stbi_uc* p = px + i * 4;
            out[i] = ((u32)p[3] << 24) | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
        }
    stbi_image_free(px);
    return out;
}

/* ---------------------------------------------------------------- DDS --- */
/* DirectDraw Surface files, as Dolphin reads them: BC1 (DXT1), BC2 (DXT3),
   BC3 (DXT5) and BC7, or uncompressed 32/24-bit, with their own mip levels.
   They're decoded to plain pixels here (on the loader thread). */
enum { DDS_BC1 = 1, DDS_BC2, DDS_BC3, DDS_BC7, DDS_RGBA32, DDS_RGB24 };

static u32 rd32le(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
#define FOURCC(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

static int mask_shift(u32 m) { int s = 0; if (!m) return 0; while (!(m & 1u)) { m >>= 1; s++; } return s; }
static u32 mask_value(u32 px, u32 m) {          /* a channel, scaled to 0-255 */
    if (!m) return 255;
    u32 v = (px & m) >> mask_shift(m), max = m >> mask_shift(m);
    return max == 255 ? v : (v * 255u + max / 2u) / max;
}

static u32 dds_level_bytes(int kind, u32 bpp, u32 w, u32 h) {
    u32 bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
    switch (kind) {
    case DDS_BC1: return bw * bh * 8u;
    case DDS_BC2: case DDS_BC3: case DDS_BC7: return bw * bh * 16u;
    default: return w * h * (bpp / 8u);
    }
}

/* Decodes one level into out (w * h pixels, 0xAARRGGBB). */
static void dds_decode(int kind, const u8* src, u32 w, u32 h, u32 bpp, const u32 masks[4], u32* out) {
    if (kind == DDS_RGBA32 || kind == DDS_RGB24) {
        u32 step = bpp / 8u;
        for (u32 i = 0; i < w * h; i++, src += step) {
            u32 px = step == 4 ? rd32le(src) : (u32)src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16);
            out[i] = (mask_value(px, masks[3]) << 24) | (mask_value(px, masks[0]) << 16) |
                     (mask_value(px, masks[1]) << 8) | mask_value(px, masks[2]);
        }
        return;
    }
    u32 bsize = kind == DDS_BC1 ? 8u : 16u;
    u8 blk[4 * 4 * 4];
    for (u32 by = 0; by < h; by += 4)
        for (u32 bx = 0; bx < w; bx += 4, src += bsize) {
            switch (kind) {
            case DDS_BC1: bcdec_bc1(src, blk, 16); break;
            case DDS_BC2: bcdec_bc2(src, blk, 16); break;
            case DDS_BC3: bcdec_bc3(src, blk, 16); break;
            default: bcdec_bc7(src, blk, 16); break;
            }
            for (u32 y = 0; y < 4 && by + y < h; y++)
                for (u32 x = 0; x < 4 && bx + x < w; x++) {
                    const u8* p = blk + y * 16u + x * 4u;
                    out[(by + y) * w + bx + x] = ((u32)p[3] << 24) | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
                }
        }
}

static u8* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* d = n > 0 ? (u8*)malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *len = d ? (size_t)n : 0;
    return d;
}

/* Loads a .dds: all its levels (up to max_levels) one after another. */
static u32* load_dds(const char* path, u32 max_levels, u32* w, u32* h, u32* levels, u32* lw, u32* lh, u32* off) {
    size_t len;
    u8* d = read_file(path, &len);
    if (!d) return NULL;
    u32* out = NULL;
    if (len < 128 || rd32le(d) != FOURCC('D', 'D', 'S', ' ') || rd32le(d + 4) != 124) goto done;
    u32 height = rd32le(d + 12), width = rd32le(d + 16), mips = rd32le(d + 28);
    u32 pf_flags = rd32le(d + 80), fourcc = rd32le(d + 84), bpp = rd32le(d + 88);
    u32 masks[4] = {rd32le(d + 92), rd32le(d + 96), rd32le(d + 100), rd32le(d + 104)};
    size_t data = 128;
    int kind = 0;
    if (pf_flags & 0x4u) {                                   /* DDPF_FOURCC */
        if (fourcc == FOURCC('D', 'X', 'T', '1')) kind = DDS_BC1;
        else if (fourcc == FOURCC('D', 'X', 'T', '2') || fourcc == FOURCC('D', 'X', 'T', '3')) kind = DDS_BC2;
        else if (fourcc == FOURCC('D', 'X', 'T', '4') || fourcc == FOURCC('D', 'X', 'T', '5')) kind = DDS_BC3;
        else if (fourcc == FOURCC('D', 'X', '1', '0') && len >= 148) {
            u32 dxgi = rd32le(d + 128);
            data = 148;
            if (dxgi == 71 || dxgi == 72) kind = DDS_BC1;
            else if (dxgi == 74 || dxgi == 75) kind = DDS_BC2;
            else if (dxgi == 77 || dxgi == 78) kind = DDS_BC3;
            else if (dxgi == 98 || dxgi == 99) kind = DDS_BC7;
            else if (dxgi == 28 || dxgi == 29) {             /* R8G8B8A8 */
                kind = DDS_RGBA32; bpp = 32;
                masks[0] = 0xFFu; masks[1] = 0xFF00u; masks[2] = 0xFF0000u; masks[3] = 0xFF000000u;
            } else if (dxgi == 87 || dxgi == 91 || dxgi == 88 || dxgi == 93) {   /* B8G8R8A8 / B8G8R8X8 */
                kind = DDS_RGBA32; bpp = 32;
                masks[0] = 0xFF0000u; masks[1] = 0xFF00u; masks[2] = 0xFFu;
                masks[3] = (dxgi == 87 || dxgi == 91) ? 0xFF000000u : 0;
            } else {
                printf("custom textures: %s: DXGI format %u isn't supported\n", path, dxgi);
            }
        } else {
            printf("custom textures: %s: format %.4s isn't supported\n", path, (const char*)(d + 84));
        }
    } else if ((pf_flags & 0x40u) && (bpp == 32 || bpp == 24)) {   /* DDPF_RGB */
        kind = bpp == 32 ? DDS_RGBA32 : DDS_RGB24;
        if (!(pf_flags & 0x1u)) masks[3] = 0;                /* no DDPF_ALPHAPIXELS */
    } else {
        printf("custom textures: %s: this kind of .dds isn't supported\n", path);
    }
    if (!kind || !width || !height || width > 16384 || height > 16384) goto done;
    if (!mips) mips = 1;
    if (mips > max_levels) mips = max_levels;
    u32 total = 0, n = 0;
    size_t pos = data;
    for (; n < mips; n++) {
        u32 a = width >> n, b = height >> n;
        if (!a) a = 1;
        if (!b) b = 1;
        size_t bytes = dds_level_bytes(kind, bpp, a, b);
        if (pos + bytes > len) break;                        /* truncated: keep the levels that are there */
        lw[n] = a; lh[n] = b; off[n] = total;
        total += a * b;
        pos += bytes;
    }
    if (!n) goto done;
    out = (u32*)malloc((size_t)total * 4);
    if (!out) goto done;
    pos = data;
    for (u32 l = 0; l < n; l++) {
        dds_decode(kind, d + pos, lw[l], lh[l], bpp, masks, out + off[l]);
        pos += dds_level_bytes(kind, bpp, lw[l], lh[l]);
    }
    *w = width; *h = height; *levels = n;
done:
    free(d);
    return out;
}

/* One image (level 0 of a .dds). */
static u32* load_image(const char* path, int* w, int* h) {
    if (!is_dds(path))
        return load_png(path, w, h);
    u32 lw[1], lh[1], off[1], uw, uh, n;
    u32* p = load_dds(path, 1, &uw, &uh, &n, lw, lh, off);
    if (p) { *w = (int)uw; *h = (int)uh; }
    return p;
}

static void load_entry(HiresEntry* e) {
    int w = 0, h = 0;
    GxHires* g = &e->img;
    u32 levels = 1, total = 0;
    u32* base = NULL;
    if (e->path && is_dds(e->path)) {
        u32 uw, uh;
        base = load_dds(e->path, GX_HIRES_MAX_LEVELS, &uw, &uh, &levels, g->lw, g->lh, g->off);
        if (base) { w = (int)uw; h = (int)uh; }
    } else if (e->path) {
        base = load_png(e->path, &w, &h);
    }
    if (!base) {
        printf("custom textures: couldn't read %s\n", e->path ? e->path : e->key);
        gx_mutex_lock(&g_mx);
        e->state = 3;
        g_failed++;
        gx_mutex_unlock(&g_mx);
        return;
    }
    if (levels > 1) {                  /* a .dds with its own mip levels */
        total = g->off[levels - 1] + g->lw[levels - 1] * g->lh[levels - 1];
        g->w = (u32)w; g->h = (u32)h; g->levels = levels; g->texels = base;
        e->buf = base;
        gx_mutex_lock(&g_mx);
        e->state = 2;
        g_loaded++;
        g_bytes += (unsigned long long)total * 4;
        gx_mutex_unlock(&g_mx);
        return;
    }
    /* Mip files, as long as each is half the size of the one before. */
    u32* lv[GX_HIRES_MAX_LEVELS] = {base};
    total = (u32)w * (u32)h;
    g->lw[0] = (u32)w; g->lh[0] = (u32)h; g->off[0] = 0;
    for (u32 l = 1; l < GX_HIRES_MAX_LEVELS && e->mip_path[l]; l++) {
        int mw, mh;
        u32 ew = (u32)w >> l, eh = (u32)h >> l;
        if (!ew) ew = 1;
        if (!eh) eh = 1;
        u32* p = load_image(e->mip_path[l], &mw, &mh);
        if (!p || (u32)mw != ew || (u32)mh != eh) { free(p); break; }
        lv[l] = p;
        g->lw[l] = ew; g->lh[l] = eh; g->off[l] = total;
        total += ew * eh;
        levels++;
    }
    u32* buf = base;
    if (levels > 1) {
        buf = (u32*)malloc((size_t)total * 4);
        if (!buf) { buf = base; levels = 1; total = (u32)w * (u32)h; }
        else for (u32 l = 0; l < levels; l++) memcpy(buf + g->off[l], lv[l], (size_t)g->lw[l] * g->lh[l] * 4);
        for (u32 l = buf == base ? 1 : 0; l < GX_HIRES_MAX_LEVELS; l++) free(lv[l]), lv[l] = NULL;
    }
    g->w = (u32)w; g->h = (u32)h; g->levels = levels; g->texels = buf;
    e->buf = buf;
    gx_mutex_lock(&g_mx);              /* the image is complete before it is seen as ready */
    e->state = 2;
    g_loaded++;
    g_bytes += (unsigned long long)total * 4;
    gx_mutex_unlock(&g_mx);
}

static void loader(void* arg) {
    (void)arg;
    for (;;) {
        gx_mutex_lock(&g_mx);
        while (!g_q_n) gx_cond_wait(&g_cv, &g_mx);
        HiresEntry* e = g_queue[g_q_head];
        g_q_head = (g_q_head + 1) % g_q_cap;
        g_q_n--;
        e->loading = true;
        gx_mutex_unlock(&g_mx);
        load_entry(e);
        gx_mutex_lock(&g_mx);
        e->loading = false;
        if (e->stale && e->buf) {      /* a rescan happened meanwhile: nothing uses it */
            free(e->buf);
            e->buf = NULL;
            e->state = 3;
        }
        gx_mutex_unlock(&g_mx);
    }
}

static void queue_load(HiresEntry* e) {
    if (!g_thread_started) {
        gx_mutex_init(&g_mx);
        gx_cond_init(&g_cv);
        gx_thread t;
        if (!gx_thread_create(&t, loader, NULL)) { e->state = 3; return; }
        g_thread_started = true;
    }
    gx_mutex_lock(&g_mx);
    if (g_q_n == g_q_cap) {            /* grow the ring, keeping its order */
        u32 cap = g_q_cap ? g_q_cap * 2 : 256;
        HiresEntry** q = (HiresEntry**)malloc(cap * sizeof(*q));
        for (u32 i = 0; i < g_q_n; i++) q[i] = g_queue[(g_q_head + i) % g_q_cap];
        free(g_queue);
        g_queue = q;
        g_q_cap = cap;
        g_q_head = 0;
    }
    g_queue[(g_q_head + g_q_n) % g_q_cap] = e;
    g_q_n++;
    e->state = 1;
    gx_cond_broadcast(&g_cv);
    gx_mutex_unlock(&g_mx);
}

/* ------------------------------------------------------------- rescan --- */
static HiresEntry* g_old;              /* left behind by the last rescan */

static void free_entry(HiresEntry* e) {
    free(e->key);
    free(e->path);
    for (int l = 0; l < GX_HIRES_MAX_LEVELS; l++) free(e->mip_path[l]);
    free(e->buf);
    free(e);
}

void hires_rescan(void (*retire)(u32* texels)) {
    if (!g_dir[0]) return;
    if (g_thread_started) gx_mutex_lock(&g_mx);
    /* Entries from the rescan before this one are long out of use (the
       texture cache was emptied then), except one the loader may still be
       reading. */
    HiresEntry* keep = NULL;
    while (g_old) {
        HiresEntry* e = g_old;
        g_old = e->next_old;
        if (e->loading) { e->next_old = keep; keep = e; }
        else free_entry(e);
    }
    g_old = keep;
    /* This scan's entries: the images that may be in use by frames still
       being drawn go back through the texture cache's delayed free. */
    g_q_n = 0;
    g_q_head = 0;
    for (u32 i = 0; i < g_tab_cap; i++) {
        HiresEntry* e = g_tab ? g_tab[i] : NULL;
        if (!e) continue;
        e->stale = true;
        if (e->state == 2 && e->buf) {
            if (retire) retire(e->buf);
            e->buf = NULL;
        }
        e->next_old = g_old;
        g_old = e;
    }
    if (g_thread_started) gx_mutex_unlock(&g_mx);
    free(g_tab);
    g_tab = NULL;
    g_tab_cap = g_tab_n = 0;
    g_sizes_n = 0;
    g_files = g_dds = g_found = g_loaded = g_failed = 0;
    g_bytes = 0;
    scan(g_dir, 0);
    g_active = g_files > 0 || g_dump_dir[0];
    printf("custom textures: reloaded, %u found in %s (%u .dds)\n", g_files, g_dir, g_dds);
}

/* ----------------------------------------------------------- dumping --- */
static char** g_dumped_set;
static u32 g_dumped_cap, g_dumped_n;

static bool dumped_add(const char* name) {
    if ((g_dumped_n + 1) * 2 > g_dumped_cap) {
        char** old = g_dumped_set;
        u32 oc = g_dumped_cap;
        g_dumped_cap = g_dumped_cap ? g_dumped_cap * 2 : 1024;
        g_dumped_set = (char**)calloc(g_dumped_cap, sizeof(char*));
        for (u32 i = 0; i < oc; i++) {
            if (!old[i]) continue;
            u32 k = str_hash(old[i]) & (g_dumped_cap - 1);
            while (g_dumped_set[k]) k = (k + 1) & (g_dumped_cap - 1);
            g_dumped_set[k] = old[i];
        }
        free(old);
    }
    u32 k = str_hash(name) & (g_dumped_cap - 1);
    while (g_dumped_set[k]) {
        if (strcmp(g_dumped_set[k], name) == 0) return false;
        k = (k + 1) & (g_dumped_cap - 1);
    }
    g_dumped_set[k] = dup_str(name);
    g_dumped_n++;
    return true;
}

static void dump(const char* name, u32 w, u32 h, const u32* px) {
    if (!dumped_add(name)) return;
    u8* rgba = (u8*)malloc((size_t)w * h * 4);
    if (!rgba) return;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        u32 v = px[i];
        rgba[i * 4 + 0] = (u8)(v >> 16); rgba[i * 4 + 1] = (u8)(v >> 8);
        rgba[i * 4 + 2] = (u8)v; rgba[i * 4 + 3] = (u8)(v >> 24);
    }
    char path[1400];
    snprintf(path, sizeof(path), "%s/%s.png", g_dump_dir, name);
    if (stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)w * 4)) g_dumped++;
    free(rgba);
}

/* ------------------------------------------------------------- lookup --- */
HiresEntry* hires_find(u32 fmt, u32 w, u32 h, bool mipmaps, const u8* data, u32 size, const u8* tlut,
                       const u32* decoded) {
    bool want = g_files && size_known(w, h);
    if (!want && !g_dump_dir[0]) return NULL;
    const u8* pal;
    u32 pal_size;
    used_palette(fmt, data, size, tlut, &pal, &pal_size);
    u64 th = hires_xxh64(data, size);
    char tp[24];
    snprintf(tp, sizeof(tp), "%016llx", (unsigned long long)(pal_size ? hires_xxh64(pal, pal_size) : 0));
    char name[160];
    make_name(name, sizeof(name), fmt, w, h, mipmaps, th, pal_size != 0, tp);
    if (g_dump_dir[0] && decoded) dump(name, w, h, decoded);
    if (!want) return NULL;
    /* The exact name, then any palette ($), then the same without / with _m
       (a pack dumped with different mip settings). */
    HiresEntry* e = tab_get(name, false);
    for (int alt = 1; !e && alt < 4; alt++) {
        bool m = alt < 2 ? mipmaps : !mipmaps;
        bool any_pal = alt & 1;
        if (any_pal && !pal_size) continue;
        make_name(name, sizeof(name), fmt, w, h, m, th, pal_size != 0, any_pal ? "$" : tp);
        e = tab_get(name, false);
    }
    if (!e || !e->path) return NULL;
    if (e->state == 0) {
        g_found++;
        queue_load(e);
    }
    return e;
}

const GxHires* hires_ready(HiresEntry* e) {
    return e && e->state == 2 ? &e->img : NULL;
}

void hires_report(void) {
    if (!g_files && !g_dump_dir[0]) return;
    if (g_files)
        printf("custom textures: %u files, %u used so far (%u loaded, %.0f MB, %u failed)\n", g_files, g_found,
               g_loaded, (double)g_bytes / (1024.0 * 1024.0), g_failed);
    if (g_dump_dir[0])
        printf("custom textures: %u textures dumped to %s\n", g_dumped, g_dump_dir);
}
