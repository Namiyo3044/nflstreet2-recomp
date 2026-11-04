/* ps2movies: takes the movies out of the PS2 version's MOVIEDAT.DAT, for
 * the port's optional PS2 extras (the Previews trailers and the sharper
 * intro movies). The player gives their own PS2 disc; nothing of the
 * game's goes in the repo.
 *
 * Usage: ps2movies <PS2 disc image .iso, or its MOVIEDAT.DAT> <work folder>
 *
 * Writes the movies as EA video files (MPEG-2 with EA sound) into the work
 * folder: intro0-2.ea (EA SPORTS BIG, the NFL Street 2 intro, the last
 * logo, in the GameCube's MOVIEDAT order) and trailer00-10.ea (the Previews
 * trailers, in the PS2 file's order). setup.ps1 then converts them with
 * ffmpeg into the MPEG-1 files the port plays.
 *
 * MOVIEDAT.DAT is an EA "TERF" file, little-endian on PS2: a DIR1 table of
 * (offset, length) pairs from the DATA chunk at 0x100. NFL Street 2 (USA)
 * has 17 movies: 0-10 the trailers, 11-13 the intros, 14-16 unused clips.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define FSEEK64 _fseeki64
#else
#define FSEEK64 fseeko
#endif

static FILE* g_in;

static uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(uint64_t off, void* dst, size_t len) {
    if (FSEEK64(g_in, (long long)off, SEEK_SET) != 0) return 0;
    return fread(dst, 1, len, g_in) == len;
}

/* ISO 9660: finds a file by name anywhere on the disc (the first one met,
   folders searched in order). Returns its byte offset and size. */
static int iso_find(uint32_t extent, uint32_t size, const char* want, int depth, uint64_t* off, uint32_t* len) {
    if (depth > 8 || size > (64u << 20)) return 0;
    uint8_t* dir = (uint8_t*)malloc(size);
    if (!dir || !read_at((uint64_t)extent * 2048, dir, size)) { free(dir); return 0; }
    int found = 0;
    for (int pass = 0; pass < 2 && !found; pass++)        /* files first, then folders */
        for (uint32_t p = 0; p < size && !found;) {
            uint8_t rl = dir[p];
            if (rl == 0) { p = (p / 2048 + 1) * 2048; continue; }   /* (records don't cross sectors) */
            if (p + rl > size || rl < 34) break;
            const uint8_t* r = dir + p;
            uint8_t nl = r[32];
            int is_dir = r[25] & 2;
            if (nl > 1 || (nl == 1 && r[33] > 1)) {      /* (not "." or "..") */
                char name[256];
                int n = 0;
                for (int i = 0; i < nl && r[33 + i] != ';'; i++) name[n++] = (char)toupper(r[33 + i]);
                if (n && name[n - 1] == '.') n--;
                name[n] = 0;
                if (pass == 0 && !is_dir && strcmp(name, want) == 0) {
                    *off = (uint64_t)le32(r + 2) * 2048;
                    *len = le32(r + 10);
                    found = 1;
                } else if (pass == 1 && is_dir) {
                    found = iso_find(le32(r + 2), le32(r + 10), want, depth + 1, off, len);
                }
            }
            p += rl;
        }
    free(dir);
    return found;
}

static void copy_out(const char* path, uint64_t off, uint32_t len) {
    FILE* o = fopen(path, "wb");
    if (!o) { fprintf(stderr, "ps2movies: can't write %s\n", path); exit(1); }
    static uint8_t buf[1 << 20];
    while (len) {
        uint32_t n = len < sizeof buf ? len : (uint32_t)sizeof buf;
        if (!read_at(off, buf, n)) { fprintf(stderr, "ps2movies: the file ends early\n"); exit(1); }
        if (fwrite(buf, 1, n, o) != n) { fprintf(stderr, "ps2movies: can't write %s (disk full?)\n", path); exit(1); }
        off += n;
        len -= n;
    }
    fclose(o);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ps2movies <PS2 disc .iso | MOVIEDAT.DAT> <work folder>\n");
        return 1;
    }
    g_in = fopen(argv[1], "rb");
    if (!g_in) { fprintf(stderr, "ps2movies: can't open %s\n", argv[1]); return 1; }

    /* The movie file: the input itself, or found on the disc image. */
    uint64_t base = 0;
    uint8_t h[0x100];
    if (!read_at(0, h, 4) || memcmp(h, "TERF", 4) != 0) {
        uint8_t pvd[2048];
        if (!read_at(16 * 2048, pvd, sizeof pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
            fprintf(stderr, "ps2movies: %s isn't a PS2 DVD image (.iso) or MOVIEDAT.DAT\n", argv[1]);
            return 1;
        }
        uint32_t len = 0;
        if (!iso_find(le32(pvd + 156 + 2), le32(pvd + 156 + 10), "MOVIEDAT.DAT", 0, &base, &len)) {
            fprintf(stderr, "ps2movies: no MOVIEDAT.DAT on this disc (is it NFL Street 2 for PS2?)\n");
            return 1;
        }
    }
    if (!read_at(base, h, sizeof h) || memcmp(h, "TERF", 4) != 0 || memcmp(h + 0x40, "DIR1", 4) != 0) {
        fprintf(stderr, "ps2movies: MOVIEDAT.DAT isn't the movie archive expected\n");
        return 1;
    }
    unsigned count = h[0x0E] | h[0x0F] << 8;
    uint8_t data[8];
    if (count != 17 || !read_at(base + 0x100, data, 4) || memcmp(data, "DATA", 4) != 0) {
        fprintf(stderr, "ps2movies: MOVIEDAT.DAT has %u movies, not the 17 of NFL Street 2 (USA) for PS2\n", count);
        return 1;
    }

    static const struct { int entry; const char* name; } k_out[] = {
        {11, "intro0"}, {12, "intro1"}, {13, "intro2"},
        {0, "trailer00"}, {1, "trailer01"}, {2, "trailer02"}, {3, "trailer03"}, {4, "trailer04"}, {5, "trailer05"},
        {6, "trailer06"}, {7, "trailer07"}, {8, "trailer08"}, {9, "trailer09"}, {10, "trailer10"},
    };
    uint64_t total = 0;
    for (size_t i = 0; i < sizeof k_out / sizeof *k_out; i++) {
        const uint8_t* e = h + 0x48 + 8 * k_out[i].entry;
        uint64_t off = base + 0x100 + le32(e);
        uint32_t len = le32(e + 4);
        uint8_t tag[4];
        if (!read_at(off, tag, 4) || memcmp(tag, "MPCh", 4) != 0) {
            fprintf(stderr, "ps2movies: movie %d isn't an EA video\n", k_out[i].entry);
            return 1;
        }
        size_t pl = strlen(argv[2]) + 32;
        char* path = (char*)malloc(pl);
        snprintf(path, pl, "%s/%s.ea", argv[2], k_out[i].name);
        copy_out(path, off, len);
        free(path);
        total += len;
    }
    printf("ps2movies: 3 intros and 11 trailers taken out (%.0f MB)\n", total / 1048576.0);
    return 0;
}
