/* discextract: unpacks a GameCube disc image (.iso / .gcm) into the folder
 * layout the port reads, the same one Dolphin's "Extract Entire Disc" makes:
 *
 *     <out>\sys\boot.bin, bi2.bin, apploader.img, main.dol, fst.bin
 *     <out>\files\...          every file in the disc's file table
 *
 * Usage: discextract <image.iso> <output folder> [expected game ID, e.g. GN7E69]
 * Files already there with the right size are left alone, so running it
 * again only fills in what's missing.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define FSEEK64 _fseeki64
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#define FSEEK64 fseeko
#endif

static FILE* g_img;
static unsigned long long g_written, g_skipped;

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int read_at(uint64_t offset, void* dst, size_t len) {
    if (FSEEK64(g_img, (long long)offset, SEEK_SET) != 0) return 0;
    return fread(dst, 1, len, g_img) == len;
}

static void make_dir(const char* path) {
    if (MKDIR(path) != 0 && errno != EEXIST) {
        fprintf(stderr, "discextract: can't make folder %s\n", path);
        exit(1);
    }
}

static long long existing_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    FSEEK64(f, 0, SEEK_END);
#ifdef _WIN32
    long long n = _ftelli64(f);
#else
    long long n = ftello(f);
#endif
    fclose(f);
    return n;
}

/* Copies len bytes at the disc offset to path (unless it's already there). */
static void extract(const char* path, uint64_t offset, uint64_t len) {
    if (existing_size(path) == (long long)len) {
        g_skipped++;
        return;
    }
    FILE* out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "discextract: can't write %s\n", path);
        exit(1);
    }
    static uint8_t buf[1 << 20];
    while (len) {
        size_t n = len < sizeof buf ? (size_t)len : sizeof buf;
        if (!read_at(offset, buf, n)) {
            fprintf(stderr, "discextract: the image ends early (reading %s)\n", path);
            exit(1);
        }
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "discextract: can't write %s (disk full?)\n", path);
            exit(1);
        }
        offset += n;
        len -= n;
        g_written += n;
    }
    fclose(out);
}

static char* join(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* s = (char*)malloc(la + lb + 2);
    memcpy(s, a, la);
    s[la] = '/';
    memcpy(s + la + 1, b, lb + 1);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: discextract <image.iso> <output folder> [game ID]\n");
        return 1;
    }
    g_img = fopen(argv[1], "rb");
    if (!g_img) {
        fprintf(stderr, "discextract: can't open %s\n", argv[1]);
        return 1;
    }
    uint8_t boot[0x440];
    if (!read_at(0, boot, sizeof boot) || be32(boot + 0x1C) != 0xC2339F3Du) {
        fprintf(stderr, "discextract: %s isn't a GameCube disc image (.iso / .gcm; compressed .rvz / .ciso "
                        "images need converting to .iso in Dolphin first)\n", argv[1]);
        return 1;
    }
    char id[7];
    memcpy(id, boot, 6);
    id[6] = 0;
    if (argc > 3 && strcmp(id, argv[3]) != 0) {
        fprintf(stderr, "discextract: this disc is %s, not %s\n", id, argv[3]);
        return 1;
    }
    uint32_t dol_off = be32(boot + 0x420), fst_off = be32(boot + 0x424), fst_size = be32(boot + 0x428);

    /* main.dol's size: the end of its furthest section (7 code, 11 data). */
    uint8_t dh[0x100];
    if (!read_at(dol_off, dh, sizeof dh)) { fprintf(stderr, "discextract: can't read main.dol\n"); return 1; }
    uint32_t dol_size = 0x100;
    for (int i = 0; i < 18; i++) {
        uint32_t end = be32(dh + i * 4) + be32(dh + 0x90 + i * 4);
        if (be32(dh + 0x90 + i * 4) && end > dol_size) dol_size = end;
    }
    /* The apploader: a 32-byte header, its code, then its trailer. */
    uint8_t ah[0x20];
    if (!read_at(0x2440, ah, sizeof ah)) { fprintf(stderr, "discextract: can't read the apploader\n"); return 1; }
    uint32_t app_size = 0x20 + be32(ah + 0x14) + be32(ah + 0x18);

    make_dir(argv[2]);
    char* sys = join(argv[2], "sys");
    char* files = join(argv[2], "files");
    make_dir(sys);
    make_dir(files);
    struct { const char* name; uint64_t off, len; } sysf[] = {
        {"boot.bin", 0, 0x440},
        {"bi2.bin", 0x440, 0x2000},
        {"apploader.img", 0x2440, app_size},
        {"main.dol", dol_off, dol_size},
        {"fst.bin", fst_off, fst_size},
    };
    for (size_t i = 0; i < sizeof sysf / sizeof *sysf; i++) {
        char* p = join(sys, sysf[i].name);
        extract(p, sysf[i].off, sysf[i].len);
        free(p);
    }

    /* The file table: 12-byte entries (entry 0 the root, its third word the
       entry count), then the names. */
    uint8_t* fst = (uint8_t*)malloc(fst_size);
    if (!fst || !read_at(fst_off, fst, fst_size)) { fprintf(stderr, "discextract: can't read the file table\n"); return 1; }
    uint32_t n = be32(fst + 8);
    if ((uint64_t)n * 12 > fst_size) { fprintf(stderr, "discextract: the file table looks damaged\n"); return 1; }
    const char* names = (const char*)fst + (size_t)n * 12;
    size_t names_size = fst_size - (size_t)n * 12;
    struct { uint32_t end; char* path; } stack[64];
    int depth = 0;
    stack[0].end = n;
    stack[0].path = files;
    unsigned nfiles = 0;
    for (uint32_t i = 1; i < n; i++) {
        while (depth > 0 && i >= stack[depth].end) {
            free(stack[depth].path);
            depth--;
        }
        const uint8_t* e = fst + (size_t)i * 12;
        uint32_t name_off = be32(e) & 0x00FFFFFFu;
        if (name_off >= names_size) continue;
        const char* name = names + name_off;
        if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) continue;   /* (no escaping the folder) */
        char* path = join(stack[depth].path, name);
        if (e[0] & 1) {
            make_dir(path);
            if (depth + 1 < 64) {
                depth++;
                stack[depth].end = be32(e + 8);
                stack[depth].path = path;
            } else {
                free(path);
            }
        } else {
            extract(path, be32(e + 4), be32(e + 8));
            nfiles++;
            free(path);
        }
    }
    printf("discextract: %s, %u files, %.0f MB written (%llu files already there)\n", id, nfiles,
           g_written / 1048576.0, g_skipped);
    return 0;
}
