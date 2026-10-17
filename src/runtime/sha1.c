/* Minimal SHA-1, used only to verify that main.dol is the expected build. */
#include "sha1.h"

#include <string.h>

static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_hex(const void* data, size_t len, char out[41]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const uint8_t* p = (const uint8_t*)data;
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++)
        sha1_block(h, p + i * 64);

    uint8_t tail[128];
    size_t rem = len % 64;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    size_t tail_len = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(h, tail);
    if (tail_len == 128)
        sha1_block(h, tail + 64);

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        uint8_t byte = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
        out[i * 2] = hex[byte >> 4];
        out[i * 2 + 1] = hex[byte & 15];
    }
    out[40] = '\0';
}
