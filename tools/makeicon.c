/* makeicon: turns the game's own disc banner (files\opening.bnr, the 96x32
 * picture Dolphin shows in its game list) into a Windows icon (.ico) for
 * the port's program and desktop shortcut. The icon is made on the player's
 * PC from their own disc; nothing of the game's goes in the repo.
 *
 * Usage: makeicon <opening.bnr> <output.ico>
 *
 * The banner is wider than tall: each square icon size holds the whole
 * banner, scaled to the icon's width and centred, transparent above and
 * below.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BW 96
#define BH 32

static uint8_t g_img[BH][BW][4];       /* RGBA */

static void decode_banner(const uint8_t* p) {
    /* RGB5A3, in 4x4 tiles, big-endian */
    for (int ty = 0; ty < BH / 4; ty++)
        for (int tx = 0; tx < BW / 4; tx++)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    uint16_t v = (uint16_t)(p[0] << 8 | p[1]);
                    p += 2;
                    uint8_t* o = g_img[ty * 4 + y][tx * 4 + x];
                    if (v & 0x8000) {
                        o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                        o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                        o[2] = (uint8_t)((v & 31) * 255 / 31);
                        o[3] = 255;
                    } else {
                        o[0] = (uint8_t)(((v >> 8) & 15) * 17);
                        o[1] = (uint8_t)(((v >> 4) & 15) * 17);
                        o[2] = (uint8_t)((v & 15) * 17);
                        o[3] = (uint8_t)(((v >> 12) & 7) * 255 / 7);
                    }
                }
}

/* The banner at size n x n: scaled to width n (an area average when it
   shrinks, bilinear when it grows), alpha-weighted, centred vertically. */
static void render(int n, uint8_t* out /* n*n*4, top row first */) {
    memset(out, 0, (size_t)n * n * 4);
    double s = (double)BW / n;            /* banner pixels per icon pixel */
    int h = (int)lround(BH / s);
    int y0 = (n - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < n; x++) {
            double acc[4] = {0, 0, 0, 0};
            if (s >= 1.0) {
                double fx0 = x * s, fx1 = (x + 1) * s, fy0 = y * s, fy1 = (y + 1) * s;
                for (int by = (int)fy0; by < (int)ceil(fy1) && by < BH; by++)
                    for (int bx = (int)fx0; bx < (int)ceil(fx1) && bx < BW; bx++) {
                        double wx = fmin(fx1, bx + 1) - fmax(fx0, bx), wy = fmin(fy1, by + 1) - fmax(fy0, by);
                        double w = wx * wy, a = g_img[by][bx][3] / 255.0;
                        for (int c = 0; c < 3; c++) acc[c] += g_img[by][bx][c] * a * w;
                        acc[3] += a * w;
                    }
                double area = s * s;
                for (int c = 0; c < 4; c++) acc[c] /= area;
            } else {
                double fx = (x + 0.5) * s - 0.5, fy = (y + 0.5) * s - 0.5;
                int x0 = (int)floor(fx), yy0 = (int)floor(fy);
                double ax = fx - x0, ay = fy - yy0;
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        int bx = x0 + i < 0 ? 0 : x0 + i >= BW ? BW - 1 : x0 + i;
                        int by = yy0 + j < 0 ? 0 : yy0 + j >= BH ? BH - 1 : yy0 + j;
                        double w = (i ? ax : 1 - ax) * (j ? ay : 1 - ay), a = g_img[by][bx][3] / 255.0;
                        for (int c = 0; c < 3; c++) acc[c] += g_img[by][bx][c] * a * w;
                        acc[3] += a * w;
                    }
            }
            uint8_t* o = out + ((size_t)(y0 + y) * n + x) * 4;
            if (acc[3] > 0) {
                for (int c = 0; c < 3; c++) {
                    double v = acc[c] / acc[3];
                    o[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : lround(v));
                }
                o[3] = (uint8_t)lround(fmin(acc[3], 1.0) * 255);
            }
        }
}

static void put16(FILE* f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE* f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: makeicon <opening.bnr> <output.ico>\n");
        return 1;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "makeicon: can't open %s\n", argv[1]); return 1; }
    static uint8_t bnr[0x20 + BW * BH * 2];
    size_t got = fread(bnr, 1, sizeof bnr, f);
    fclose(f);
    if (got != sizeof bnr || (memcmp(bnr, "BNR1", 4) != 0 && memcmp(bnr, "BNR2", 4) != 0)) {
        fprintf(stderr, "makeicon: %s isn't a GameCube banner\n", argv[1]);
        return 1;
    }
    decode_banner(bnr + 0x20);

    static const int sizes[] = {256, 64, 48, 32, 24, 16};
    enum { N = sizeof sizes / sizeof *sizes };
    uint32_t len[N];
    for (int i = 0; i < N; i++) {
        int n = sizes[i];
        len[i] = 40 + (uint32_t)n * n * 4 + (uint32_t)((n + 31) / 32 * 4) * n;   /* header, pixels, AND mask */
    }
    FILE* o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "makeicon: can't write %s\n", argv[2]); return 1; }
    put16(o, 0); put16(o, 1); put16(o, N);
    uint32_t off = 6 + 16 * N;
    for (int i = 0; i < N; i++) {
        int n = sizes[i];
        fputc(n >= 256 ? 0 : n, o); fputc(n >= 256 ? 0 : n, o);
        fputc(0, o); fputc(0, o);
        put16(o, 1); put16(o, 32);
        put32(o, len[i]); put32(o, off);
        off += len[i];
    }
    for (int i = 0; i < N; i++) {
        int n = sizes[i];
        uint8_t* px = (uint8_t*)malloc((size_t)n * n * 4);
        render(n, px);
        put32(o, 40); put32(o, (unsigned)n); put32(o, (unsigned)n * 2);   /* (height counts the mask too) */
        put16(o, 1); put16(o, 32); put32(o, 0); put32(o, (unsigned)n * n * 4);
        put32(o, 0); put32(o, 0); put32(o, 0); put32(o, 0);
        for (int y = n - 1; y >= 0; y--)          /* bottom row first, BGRA */
            for (int x = 0; x < n; x++) {
                const uint8_t* p = px + ((size_t)y * n + x) * 4;
                fputc(p[2], o); fputc(p[1], o); fputc(p[0], o); fputc(p[3], o);
            }
        int stride = (n + 31) / 32 * 4;
        for (int y = n - 1; y >= 0; y--)          /* AND mask: 1 where see-through */
            for (int b = 0; b < stride; b++) {
                unsigned v = 0;
                for (int k = 0; k < 8; k++) {
                    int x = b * 8 + k;
                    if (x < n && px[((size_t)y * n + x) * 4 + 3] == 0) v |= 0x80u >> k;
                }
                fputc((int)v, o);
            }
        free(px);
    }
    fclose(o);
    printf("makeicon: %s from the disc's banner\n", argv[2]);
    return 0;
}
