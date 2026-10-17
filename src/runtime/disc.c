#include "disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t offset;
    uint64_t length;
    char* path;      /* host file holding these bytes */
} Range;

static Range* g_ranges;
static size_t g_count, g_cap;
static uint8_t* g_fst;
static size_t g_fst_size;
static unsigned long long g_reads, g_bytes, g_misses;

/* A few open file handles, so streaming reads don't reopen files each time. */
#define HANDLE_CACHE 16
static struct { const char* path; FILE* f; unsigned long long used; } g_handles[HANDLE_CACHE];
static unsigned long long g_tick;

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static char* join(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* s = (char*)malloc(la + lb + 2);
    memcpy(s, a, la);
    s[la] = '/';
    memcpy(s + la + 1, b, lb + 1);
    return s;
}

static long long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fclose(f);
    return n;
}

static void add_range(uint64_t offset, uint64_t length, char* path) {
    if (g_count == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 1024;
        g_ranges = (Range*)realloc(g_ranges, g_cap * sizeof(Range));
    }
    g_ranges[g_count].offset = offset;
    g_ranges[g_count].length = length;
    g_ranges[g_count].path = path;
    g_count++;
}

static void add_system_file(const char* disc_dir, const char* name, uint64_t offset) {
    char* sys = join(disc_dir, "sys");
    char* path = join(sys, name);
    free(sys);
    long long n = file_size(path);
    if (n > 0)
        add_range(offset, (uint64_t)n, path);
    else
        free(path);
}

static int cmp_range(const void* a, const void* b) {
    uint64_t x = ((const Range*)a)->offset, y = ((const Range*)b)->offset;
    return x < y ? -1 : x > y;
}

static uint8_t* read_whole(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

bool disc_open(const char* disc_dir) {
    char* sys = join(disc_dir, "sys");
    char* boot_path = join(sys, "boot.bin");
    char* fst_path = join(sys, "fst.bin");
    free(sys);

    size_t boot_size = 0;
    uint8_t* boot = read_whole(boot_path, &boot_size);
    g_fst = read_whole(fst_path, &g_fst_size);
    free(boot_path);
    free(fst_path);
    if (!boot || boot_size < 0x430 || !g_fst || g_fst_size < 12) {
        fprintf(stderr, "error: disc folder needs sys\\boot.bin and sys\\fst.bin\n");
        free(boot);
        return false;
    }

    /* System area. */
    add_system_file(disc_dir, "boot.bin", 0x0000);
    add_system_file(disc_dir, "bi2.bin", 0x0440);
    add_system_file(disc_dir, "apploader.img", 0x2440);
    add_system_file(disc_dir, "main.dol", be32(boot + 0x420));
    add_system_file(disc_dir, "fst.bin", be32(boot + 0x424));
    free(boot);

    /* File table: 12-byte entries, then the name strings. Entry 0 is the root
       directory and its third word is the total entry count. */
    uint32_t n = be32(g_fst + 8);
    if ((size_t)n * 12 > g_fst_size) {
        fprintf(stderr, "error: fst.bin looks corrupt (%u entries)\n", n);
        return false;
    }
    const char* names = (const char*)g_fst + (size_t)n * 12;
    size_t names_size = g_fst_size - (size_t)n * 12;

    char* files_root = join(disc_dir, "files");
    struct { uint32_t end; char* path; } stack[64];
    int depth = 0;
    stack[0].end = n;
    stack[0].path = files_root;

    for (uint32_t i = 1; i < n; i++) {
        while (depth > 0 && i >= stack[depth].end) {
            free(stack[depth].path);
            depth--;
        }
        const uint8_t* e = g_fst + (size_t)i * 12;
        uint32_t name_off = be32(e) & 0x00FFFFFFu;
        if (name_off >= names_size)
            continue;
        char* path = join(stack[depth].path, names + name_off);
        if (e[0] & 1) {                       /* directory: word 2 = index after its contents */
            if (depth + 1 < 64) {
                depth++;
                stack[depth].end = be32(e + 8);
                stack[depth].path = path;
            } else {
                free(path);
            }
        } else {                              /* file: word 1 = disc offset, word 2 = length */
            add_range(be32(e + 4), be32(e + 8), path);
        }
    }
    while (depth > 0)
        free(stack[depth--].path);
    free(files_root);

    qsort(g_ranges, g_count, sizeof(Range), cmp_range);
    printf("disc: %zu files mapped from %s\n", g_count, disc_dir);
    return true;
}

static FILE* open_cached(const char* path) {
    g_tick++;
    size_t victim = 0;
    for (size_t i = 0; i < HANDLE_CACHE; i++) {
        if (g_handles[i].path == path) {
            g_handles[i].used = g_tick;
            return g_handles[i].f;
        }
        if (g_handles[i].used < g_handles[victim].used)
            victim = i;
    }
    if (g_handles[victim].f)
        fclose(g_handles[victim].f);
    g_handles[victim].f = fopen(path, "rb");
    g_handles[victim].path = g_handles[victim].f ? path : NULL;
    g_handles[victim].used = g_tick;
    return g_handles[victim].f;
}

bool disc_read(uint64_t offset, void* dst, size_t len) {
    uint8_t* out = (uint8_t*)dst;
    memset(out, 0, len);
    g_reads++;
    g_bytes += len;
    bool ok = true;

    /* First range that could overlap: the last one starting at or before offset. */
    size_t lo = 0, hi = g_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_ranges[mid].offset <= offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    size_t i = lo ? lo - 1 : 0;

    uint64_t end = offset + len;
    for (; i < g_count && g_ranges[i].offset < end; i++) {
        const Range* r = &g_ranges[i];
        uint64_t a = offset > r->offset ? offset : r->offset;
        uint64_t b = end < r->offset + r->length ? end : r->offset + r->length;
        if (a >= b)
            continue;
        FILE* f = open_cached(r->path);
        if (!f || fseek(f, (long)(a - r->offset), SEEK_SET) != 0 ||
            fread(out + (a - offset), 1, (size_t)(b - a), f) != (size_t)(b - a)) {
            fprintf(stderr, "disc: failed reading %s\n", r->path);
            ok = false;
        }
    }
    if (!ok)
        g_misses++;
    return ok;
}

const uint8_t* disc_fst(size_t* size) {
    *size = g_fst_size;
    return g_fst;
}

void disc_report(void) {
    printf("disc: %llu reads, %llu bytes, %llu failed\n", g_reads, g_bytes, g_misses);
}
