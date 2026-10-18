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
    char* path;                        /* level 0 file */
    char* mip_path[GX_HIRES_MAX_LEVELS];
    volatile int state;                /* 0 not loaded, 1 queued, 2 ready, 3 failed */
    GxHires img;
    u32* buf;
};

static HiresEntry** g_tab;             /* open addressing on the key */
static u32 g_tab_cap, g_tab_n;
static u32* g_sizes;                   /* (w << 16) | h of every file, for a quick "none this size" */
static u32 g_sizes_cap, g_sizes_n;
static bool g_active;
static unsigned g_files, g_dds, g_found, g_loaded, g_failed, g_dumped;
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
    e->key = strdup(key);
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

static void add_file(const char* dir, const char* name) {
    size_t n = strlen(name);
    if (n < 10 || strncmp(name, "tex1_", 5) != 0)
        return;
    char low[512];
    if (n >= sizeof(low)) return;
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)name[i]);
    if (n > 4 && strcmp(low + n - 4, ".dds") == 0) { g_dds++; return; }
    if (n <= 4 || strcmp(low + n - 4, ".png") != 0) return;
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
    if (level == 0) {
        free(e->path);
        e->path = path;
        g_files++;
        if (!size_known(w, h)) {
            if (g_sizes_n == g_sizes_cap) {
                g_sizes_cap = g_sizes_cap ? g_sizes_cap * 2 : 64;
                g_sizes = (u32*)realloc(g_sizes, g_sizes_cap * sizeof(u32));
            }
            g_sizes[g_sizes_n++] = (w << 16) | h;
        }
    } else {
        free(e->mip_path[level]);
        e->mip_path[level] = path;
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
    scan(dir, 0);
    g_active = g_files > 0;
    printf("custom textures: %u found in %s", g_files, dir);
    if (g_dds) printf(" (%u .dds files skipped: only .png is read for now)", g_dds);
    printf("\n");
}

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

static void load_entry(HiresEntry* e) {
    int w, h;
    u32* base = e->path ? load_png(e->path, &w, &h) : NULL;
    if (!base) {
        printf("custom textures: couldn't read %s\n", e->path ? e->path : e->key);
        gx_mutex_lock(&g_mx);
        e->state = 3;
        g_failed++;
        gx_mutex_unlock(&g_mx);
        return;
    }
    /* Mip files, as long as each is half the size of the one before. */
    u32* lv[GX_HIRES_MAX_LEVELS] = {base};
    u32 levels = 1, total = (u32)w * (u32)h;
    GxHires* g = &e->img;
    g->lw[0] = (u32)w; g->lh[0] = (u32)h; g->off[0] = 0;
    for (u32 l = 1; l < GX_HIRES_MAX_LEVELS && e->mip_path[l]; l++) {
        int mw, mh;
        u32 ew = (u32)w >> l, eh = (u32)h >> l;
        if (!ew) ew = 1;
        if (!eh) eh = 1;
        u32* p = load_png(e->mip_path[l], &mw, &mh);
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
        gx_mutex_unlock(&g_mx);
        load_entry(e);
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
    g_dumped_set[k] = strdup(name);
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
