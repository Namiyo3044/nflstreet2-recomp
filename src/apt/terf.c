#include "apt/terf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---------------------------------------------------------------- LZH1 --- */
/* Blocks of: 1 bit (1 = end, then a 32-bit Adler-32), 285 + 30 code lengths
   as 4-bit values, then deflate-style literal/length and distance codes.
   Bits are read most significant first; codes are canonical (not reversed). */
static const uint8_t k_lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,0,0};
static const uint16_t k_lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,228};
static const uint8_t k_dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const uint16_t k_dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};

typedef struct { const uint8_t* d; size_t n; size_t bit; } Bits;
static uint32_t getbits(Bits* b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = b->bit >> 3;
        uint32_t x = byte < b->n ? (uint32_t)(b->d[byte] >> (7 - (b->bit & 7))) & 1u : 0u;
        b->bit++;
        v = v << 1 | x;
    }
    return v;
}

typedef struct { uint16_t count[16]; uint16_t first[16]; uint16_t sym[288]; } Huff;
static void huff_build(Huff* h, const uint8_t* lens, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    int k = 0;
    for (int L = 1; L < 16; L++) {
        h->first[L] = (uint16_t)k;
        for (int i = 0; i < n; i++) if (lens[i] == L) h->sym[k++] = (uint16_t)i;
    }
}
static int huff_sym(Bits* b, const Huff* h) {
    int code = 0, first = 0;
    for (int L = 1; L < 16; L++) {
        code |= (int)getbits(b, 1);
        int cnt = h->count[L];
        if (code - first < cnt) return h->sym[h->first[L] + code - first];
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

long lzh1_decode(const uint8_t* in, size_t in_size, uint8_t* out, size_t size) {
    Bits b = {in, in_size, 0};
    size_t o = 0;
    Huff lt, dt;
    for (;;) {
        if (b.bit / 8 > in_size + 4) return -1;
        if (getbits(&b, 1)) { getbits(&b, 32); return (long)o; }
        uint8_t ll[285], dl[30];
        for (int i = 0; i < 285; i++) ll[i] = (uint8_t)getbits(&b, 4);
        for (int i = 0; i < 30; i++) dl[i] = (uint8_t)getbits(&b, 4);
        huff_build(&lt, ll, 285);
        huff_build(&dt, dl, 30);
        for (;;) {
            int s = huff_sym(&b, &lt);
            if (s < 0) return -1;
            if (s < 256) { if (o >= size) return -1; out[o++] = (uint8_t)s; continue; }
            if (s == 256) break;
            int k = s - 257;
            if (k >= 29) return -1;
            size_t len = k <= 7 ? (size_t)k + 3 : k_lbase[k] + getbits(&b, k_lext[k]);
            int ds = huff_sym(&b, &dt);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = k_dbase[ds] + getbits(&b, k_dext[ds]);
            if (dist > o || o + len > size) return -1;
            for (size_t i = 0; i < len; i++, o++) out[o] = out[o - dist];
        }
    }
}

/* ---------------------------------------------------------------- packs --- */
TerfPack* terf_parse(const uint8_t* b, uint32_t n) {
    if (n < 16 || memcmp(b, "TERF", 4) != 0) return NULL;
    int cnt = rd16(b + 14);
    uint32_t p = rd32(b + 4);
    uint32_t dir = 0, dirsz = 0, comp = 0, datap = 0;
    bool have_data = false;
    while (p + 8 <= n) {
        uint32_t sz = rd32(b + p + 4);
        if (!memcmp(b + p, "DIR1", 4)) { dir = p; dirsz = sz; }
        else if (!memcmp(b + p, "COMP", 4)) comp = p;
        else if (!memcmp(b + p, "DATA", 4)) { datap = p; have_data = true; break; }
        if (sz == 0) break;
        p += sz;
    }
    if (!dir || !have_data || dir + 8 + (uint32_t)cnt * 8 > n || dirsz < 8) return NULL;
    TerfPack* pk = (TerfPack*)calloc(1, sizeof *pk);
    pk->count = cnt;
    pk->entries = (TerfEntry*)calloc((size_t)cnt, sizeof(TerfEntry));
    for (int i = 0; i < cnt; i++) {
        uint32_t off = rd32(b + dir + 8 + i * 8), sz = rd32(b + dir + 12 + i * 8);
        uint32_t type = 0, usz = sz;
        if (comp && comp + 16 + (uint32_t)i * 8 <= n) { type = rd32(b + comp + 8 + i * 8); usz = rd32(b + comp + 12 + i * 8); }
        if ((uint64_t)datap + off + sz > n) continue;
        const uint8_t* raw = b + datap + off;
        TerfEntry* e = &pk->entries[i];
        if (type == 5) {
            e->data = (uint8_t*)malloc(usz ? usz : 1);
            long got = lzh1_decode(raw, sz, e->data, usz);
            if (got != (long)usz) {
                fprintf(stderr, "terf: entry %d didn't unpack (%ld of %u bytes)\n", i, got, usz);
                free(e->data); e->data = NULL; continue;
            }
            e->size = usz;
        } else {
            e->data = (uint8_t*)malloc(sz ? sz : 1);
            memcpy(e->data, raw, sz);
            e->size = sz;
        }
        if (e->size >= 16 && !memcmp(e->data, "TERF", 4)) e->pack = terf_parse(e->data, e->size);
    }
    return pk;
}

TerfPack* terf_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t* b = (uint8_t*)malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    TerfPack* p = terf_parse(b, (uint32_t)n);
    free(b);
    if (!p) fprintf(stderr, "terf: %s isn't an EA pack\n", path);
    return p;
}

void terf_free(TerfPack* p) {
    if (!p) return;
    for (int i = 0; i < p->count; i++) { terf_free(p->entries[i].pack); free(p->entries[i].data); }
    free(p->entries);
    free(p);
}
