#pragma once
/* EA "TERF" packs (the PS2 version's .DAT files): a directory of entries, each
   stored as is or compressed (type 5, EA's "LZH1"), and often itself a pack. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct TerfEntry {
    uint8_t* data;                     /* the entry's bytes, unpacked */
    uint32_t size;
    struct TerfPack* pack;             /* if the entry is a pack itself, its entries */
} TerfEntry;

typedef struct TerfPack {
    int count;
    TerfEntry* entries;
} TerfPack;

/* Reads a pack from a file (NULL with a message if it isn't one). */
TerfPack* terf_load(const char* path);
TerfPack* terf_parse(const uint8_t* data, uint32_t size);
void terf_free(TerfPack* p);

/* EA's LZH1: out must hold `size` bytes; returns the bytes produced (-1 on bad data). */
long lzh1_decode(const uint8_t* in, size_t in_size, uint8_t* out, size_t size);
