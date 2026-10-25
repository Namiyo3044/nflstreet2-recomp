#include "runtime/credits.h"
#include "runtime/disc.h"

#include <stdio.h>
#include <string.h>

/* Streamed.db's CDRL table (the GameCube credits). Its place in the file
   holds exactly its 411 lines, and the game won't take more, so the port's
   lines replace a few of the game's blank spacer lines instead of adding
   to them. */
#define CDRL_LINES  0x8690u            /* first line (after the header and field list) */
#define CDRL_ORIG   411u               /* lines in the game's own table */
#define LINE_SIZE   72u                /* 70 bytes of text, the style, a pad byte */

enum { NORMAL = 1, HEADING = 2 };
typedef struct { unsigned char style; const char* text; } Line;

/* The port's lines, at the top of the roll (before the game's legal
   notices). The libraries and the fan-project notice are in the README. */
static const Line k_lines[] = {
    {HEADING, "NFL STREET 2 RECOMP"},
    {NORMAL, "Namiyo3044 - Port Developer"},
    {NORMAL, "DolRecomp (expansionpak) - Recompiler"},
    {NORMAL, "NFL Street 2 Decomp Team - Special Thanks"},
    {NORMAL, "github.com/Namiyo3044/nflstreet2-recomp"},
    {NORMAL, ""},
};
#define N_OURS (sizeof(k_lines) / sizeof(k_lines[0]))

/* The game's blank lines left out to make room, one per line of ours:
   those between its legal notices (4, 9, 13), the second blank before
   PROGRAMMING and before MUSIC (15, 211), and one of two after the music
   staff (218). The table keeps the game's 411 lines (all its slot holds),
   so its header stays as it is. */
static const unsigned k_dropped[] = {4, 9, 13, 15, 211, 218};
#define N_DROPPED (sizeof(k_dropped) / sizeof(k_dropped[0]))
#define N_TOTAL (CDRL_ORIG - N_DROPPED + N_OURS)
typedef char fits_the_games_slot[(N_TOTAL <= CDRL_ORIG) ? 1 : -1];

/* The GameCube credits table's header, as on the disc: 411 lines. */
#define CDRL_HEADER 0x8648u
static const uint8_t k_header[0x28] = {
    0x8C, 0x80, 0x57, 0x17, 0, 0, 0, 2, 0, 0, 0, 0x48, 0, 0, 0x02, 0x3F, 0, 0, 0, 0,
    0x01, 0x9B, 0x01, 0x9B, 0, 0, 0xFF, 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0x68, 0x3A, 0x30, 0x9E};

/* The table's field list (CRli: text, 560 bits; CRty: style, 8 bits),
   which comes between the header and the lines and is part of the check. */
static const uint8_t k_fields[32] = {0, 0, 0, 0, 0, 0, 0, 0, 'i', 'l', 'R', 'C', 0, 0, 0x02, 0x30,
                                     0, 0, 0, 3, 0, 0, 0x02, 0x30, 'y', 't', 'R', 'C', 0, 0, 0, 8};

/* The database's checks: CRC-32, most significant bit first, polynomial
   0x04C11DB7, starting from all ones, nothing XORed at the end. A table
   header's covers its bytes 4-0x23 (stored at 0x24); the data's covers the
   field list and the lines (stored right after the last line). */
static uint32_t crc_add(uint32_t c, const uint8_t* p, size_t n) {
    while (n--) {
        c ^= (uint32_t)*p++ << 24;
        for (int k = 0; k < 8; k++) c = c & 0x80000000u ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}
static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static bool is_streamed_db(const char* path) {
    static const char name[] = "streamed.db";
    size_t l = strlen(path), n = sizeof(name) - 1;
    if (l < n) return false;
    for (size_t k = 0; k < n; k++) {
        char c = path[l - n + k];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != name[k]) return false;
    }
    return l == n || path[l - n - 1] == '/' || path[l - n - 1] == '\\';
}

static bool blank(const uint8_t* line) {
    for (unsigned i = 0; i < 70 && line[i]; i++)
        if (line[i] != ' ') return false;
    return true;
}


void credits_on_disc_read(uint64_t disc_offset, uint8_t* dst, uint32_t n) {
    if (!n) return;
    uint64_t fo = 0;
    const char* path = disc_file_at(disc_offset, &fo);
    if (!path || !is_streamed_db(path)) return;
    uint64_t end = fo + n;

    /* The table header (it may come in pieces): the game's own, given our
       line count (both copies) and the check that goes with it. */
    if (fo < CDRL_HEADER + sizeof(k_header) && end > CDRL_HEADER) {
        uint8_t ours[sizeof(k_header)];
        memcpy(ours, k_header, sizeof(ours));
        ours[0x14] = ours[0x16] = (uint8_t)(N_TOTAL >> 8);
        ours[0x15] = ours[0x17] = (uint8_t)(N_TOTAL & 0xFF);
        put_be32(ours + 0x24, crc_add(0xFFFFFFFFu, ours + 4, 0x20));
        uint64_t a = fo > CDRL_HEADER ? fo : CDRL_HEADER;
        uint64_t b = end < CDRL_HEADER + sizeof(k_header) ? end : CDRL_HEADER + sizeof(k_header);
        if (memcmp(dst + (a - fo), k_header + (a - CDRL_HEADER), (size_t)(b - a)) == 0) {
            memcpy(dst + (a - fo), ours + (a - CDRL_HEADER), (size_t)(b - a));
        } else {
            printf("credits: Streamed.db's credits table isn't the expected one; left alone\n");
        }
    }

    /* The lines and their check: built once from the game's own (read
       from the disc here) and given to every read that covers any of them;
       the game reads the lines in one go, then the check again on its own. */
    static uint8_t table[CDRL_ORIG * LINE_SIZE + 4];
    static int built;                  /* 0 not yet, 1 built, -1 not the expected table */
    uint64_t lo = CDRL_LINES, total = (uint64_t)N_TOTAL * LINE_SIZE;
    if (end <= lo || fo >= lo + total + 4) return;
    if (!built) {
        static uint8_t orig[CDRL_ORIG * LINE_SIZE + 4];
        uint64_t size = (uint64_t)CDRL_ORIG * LINE_SIZE;
        built = -1;
        if (!disc_read(disc_offset - fo + lo, orig, sizeof(orig))) return;
        uint32_t c = crc_add(crc_add(0xFFFFFFFFu, k_fields, sizeof(k_fields)), orig, (size_t)size);
        if (c != be32(orig + size)) { printf("credits: the credits lines aren't the expected ones\n"); return; }
        for (unsigned k = 0; k < N_DROPPED; k++)
            if (!blank(orig + k_dropped[k] * LINE_SIZE)) {
                printf("credits: Streamed.db's credits aren't the expected ones; left alone\n");
                return;
            }
        memset(table, 0, sizeof(table));
        for (unsigned i = 0; i < N_OURS; i++) {
            size_t len = strlen(k_lines[i].text);
            if (len > 69) len = 69;
            memcpy(table + i * LINE_SIZE, k_lines[i].text, len);
            table[i * LINE_SIZE + 70] = k_lines[i].style;
        }
        unsigned w = N_OURS;
        for (unsigned i = 0; i < CDRL_ORIG; i++) {
            bool drop = false;
            for (unsigned k = 0; k < N_DROPPED; k++) drop |= k_dropped[k] == i;
            if (!drop) memcpy(table + (w++) * LINE_SIZE, orig + i * LINE_SIZE, LINE_SIZE);
        }
        put_be32(table + total, crc_add(crc_add(0xFFFFFFFFu, k_fields, sizeof(k_fields)), table, (size_t)total));
        built = 1;
        printf("credits: the port's credits added at the top (%u lines, %u in all)\n", (unsigned)N_OURS,
               (unsigned)N_TOTAL);
    }
    if (built != 1) return;
    uint64_t a = fo > lo ? fo : lo, b = end < lo + total + 4 ? end : lo + total + 4;
    memcpy(dst + (a - fo), table + (a - lo), (size_t)(b - a));
}
