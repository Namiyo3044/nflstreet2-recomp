#include "runtime/exi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* External interface (EXI): three serial channels. Channel 0 device 0 is
 * memory card slot A, channel 0 device 1 the IPL chip (boot ROM, real-time
 * clock and SRAM), channel 1 device 0 slot B. Per channel the registers are
 * CSR (+0x00), DMA address (+0x04), DMA length (+0x08), control (+0x0C) and
 * immediate data (+0x10).
 *
 * The memory card is emulated at the hardware level: the game's own CARD
 * library formats, reads and writes it, and the card's contents live in a
 * raw image file on the PC (the same format Dolphin uses, so cards can be
 * swapped between the two).
 */

#define CSR_EXIINTMASK 0x0001u
#define CSR_EXIINT     0x0002u
#define CSR_TCINTMASK  0x0004u
#define CSR_TCINT      0x0008u
#define CSR_CS_SHIFT   7
#define CSR_EXTINTMASK 0x0400u
#define CSR_EXTINT     0x0800u
#define CSR_EXT        0x1000u
#define CSR_W1C        (CSR_EXIINT | CSR_TCINT | CSR_EXTINT)

typedef struct {
    u32 csr, mar, len, cr, data;
} Channel;

static Channel g_ch[3];

/* ------------------------------------------------------------ the card --- */
#define SECTOR_SIZE 0x2000u    /* 8 KB erase sectors */
#define PAGE_SIZE   128u       /* program page */

typedef struct {
    bool present;
    char path[512];
    FILE* file;
    u8* data;
    u32 size;              /* bytes */
    u32 id;                /* EXI ID: size in Mbit (latency and sector size codes 0) */
    u8 status;             /* 0x40 unlocked | 0x01 ready; 0x08/0x10 program/erase errors */
    bool irq_enabled;
    /* the command in progress (since chip select) */
    u32 pos;
    u8 cmd;
    u8 args[4];
    u32 address;
    bool pending_irq;      /* raise the card interrupt at deselect (program/erase done) */
    unsigned long long reads, pages, erases;
} Card;

static Card g_card[2];

static u32 card_address(const Card* c) {
    return ((u32)(c->args[0] & 0x7Fu) << 17) | ((u32)c->args[1] << 9) | ((u32)(c->args[2] & 3u) << 7) |
           (c->args[3] & 0x7Fu);
}

static void card_save(Card* c, u32 offset, u32 bytes) {
    if (!c->file)
        return;
    if (fseek(c->file, (long)offset, SEEK_SET) != 0 || fwrite(c->data + offset, 1, bytes, c->file) != bytes)
        fprintf(stderr, "memory card: couldn't write %s\n", c->path);
    fflush(c->file);
}

bool exi_insert_memcard(int slot, const char* path) {
    if (slot < 0 || slot > 1 || !path || !*path)
        return false;
    Card* c = &g_card[slot];
    memset(c, 0, sizeof(*c));
    snprintf(c->path, sizeof(c->path), "%s", path);
    FILE* f = fopen(path, "r+b");
    long len = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    /* A card is 4 to 128 Mbit (59 to 2043 blocks). New cards: 16 Mbit
       (251 blocks), unformatted, so the game offers to format it. */
    u32 size = 0x200000u;
    if (f && len > 0) {
        u32 mbit = (u32)((unsigned long long)len * 8u / (1024u * 1024u));
        if (mbit == 4 || mbit == 8 || mbit == 16 || mbit == 32 || mbit == 64 || mbit == 128) {
            size = (u32)len;
        } else {
            fprintf(stderr, "memory card: %s is %ld bytes, not a memory card image\n", path, len);
            fclose(f);
            return false;
        }
    }
    c->data = (u8*)malloc(size);
    if (!c->data) {
        if (f) fclose(f);
        return false;
    }
    memset(c->data, 0xFF, size);
    if (f) {
        if (fread(c->data, 1, size, f) != size)
            fprintf(stderr, "memory card: short read from %s\n", path);
    } else {
        f = fopen(path, "w+b");
        if (!f) {
            fprintf(stderr, "memory card: couldn't create %s (does the folder exist?)\n", path);
            free(c->data);
            c->data = NULL;
            return false;
        }
        fwrite(c->data, 1, size, f);
        fflush(f);
        printf("memory card: created a new, unformatted %u-block card at %s\n", size / SECTOR_SIZE - 5, path);
    }
    c->file = f;
    c->size = size;
    c->id = size * 8u / (1024u * 1024u);
    c->status = 0x41;
    c->present = true;
    printf("memory card: slot %c = %s (%u blocks)\n", 'A' + slot, path, size / SECTOR_SIZE - 5);
    return true;
}

static void card_select(Card* c) {
    c->pos = 0;
    c->cmd = 0;
    c->pending_irq = false;
}

/* Deselect ends a command; erase and program finish now (instantly) and
   signal with the card's interrupt, which the CARD library waits for. */
static void card_deselect(Card* c, Channel* ch) {
    if (c->cmd == 0xF1 && c->pos >= 3) {             /* sector erase */
        u32 a = card_address(c) & ~(SECTOR_SIZE - 1u) & (c->size - 1u);
        memset(c->data + a, 0xFF, SECTOR_SIZE);
        card_save(c, a, SECTOR_SIZE);
        c->erases++;
        c->pending_irq = true;
    } else if (c->cmd == 0xF4 && c->pos >= 3) {      /* chip erase */
        memset(c->data, 0xFF, c->size);
        card_save(c, 0, c->size);
        c->pending_irq = true;
    }
    if (c->pending_irq && c->irq_enabled)
        ch->csr |= CSR_EXIINT;
    c->pending_irq = false;
    c->cmd = 0;
    c->pos = 0;
}

static u8 card_byte(Card* c, u8 in) {
    u8 out = 0xFF;
    u32 p = c->pos++;
    if (p == 0) {
        c->cmd = in;
        return 0xFF;
    }
    switch (c->cmd) {
    case 0x00:                                        /* ID: dummy byte, then 4 ID bytes */
        out = p >= 2 ? (u8)(c->id >> (8u * (3u - ((p - 2u) & 3u)))) : 0x80;
        break;
    case 0x83:                                        /* read status */
        out = c->status;
        break;
    case 0x89:                                        /* clear status */
        c->status &= (u8)~0x18u;
        break;
    case 0x81:                                        /* interrupt enable */
        if (p == 1) c->irq_enabled = in & 1u;
        break;
    case 0x52:                                        /* read array: 4 address bytes, then latency */
    case 0xF2:                                        /* page program: 4 address bytes */
    case 0xF1:                                        /* sector erase: 2 address bytes */
        if (p <= 4) c->args[p - 1] = in;
        if (p == 4 || (c->cmd == 0xF1 && p == 2)) {
            if (c->cmd == 0xF1) { c->args[2] = 0; c->args[3] = 0; }
            c->address = card_address(c);
        }
        break;
    default:                                          /* wake-up, sleep, others: ignored */
        break;
    }
    return out;
}

static void card_dma(Card* c, CPUState* cpu, u32 mar, u32 len, bool to_ram) {
    if (mar >= cpu->ram_size || len > cpu->ram_size - mar)
        return;
    u32 a = c->address & (c->size - 1u);
    if (a + len > c->size)
        len = c->size - a;
    if (c->cmd == 0x52 && to_ram) {                   /* read */
        memcpy(cpu->ram + mar, c->data + a, len);
        c->address += len;
        c->reads++;
    } else if (c->cmd == 0xF2 && !to_ram) {           /* program a page: bits can only go 1 -> 0 */
        for (u32 i = 0; i < len; i++)
            c->data[a + i] &= cpu->ram[mar + i];
        card_save(c, a, len);
        c->address += len;
        c->pages++;
        c->pending_irq = true;
    }
}

/* ------------------------------------------------------ IPL / RTC / SRAM --- */
static u8 g_sram[64];
static u32 g_ipl_cmd, g_ipl_pos;
static bool g_ipl_have_cmd;

static void sram_init(void) {
    memset(g_sram, 0, sizeof(g_sram));
    g_sram[0x13] = 0x04;                  /* flags: stereo sound */
    g_sram[0x3A] = 0xFF;                  /* flashIDCheckSum[0]: ~sum of an all-zero flash ID */
    g_sram[0x3B] = 0xFF;                  /* flashIDCheckSum[1] */
    /* checksums over counterBias..flags (bytes 0x0C-0x13), as u16 words */
    u16 sum = 0, inv = 0;
    for (u32 i = 0x0C; i < 0x14; i += 2) {
        u16 w = (u16)((g_sram[i] << 8) | g_sram[i + 1]);
        sum = (u16)(sum + w);
        inv = (u16)(inv + (u16)~w);
    }
    g_sram[0] = (u8)(sum >> 8); g_sram[1] = (u8)sum;
    g_sram[2] = (u8)(inv >> 8); g_sram[3] = (u8)inv;
}

/* Seconds since 2000-01-01, the GameCube RTC epoch. */
static u32 rtc_now(void) {
    return (u32)(time(NULL) - 946684800);
}

static u8 ipl_byte(u8 in) {
    if (!g_ipl_have_cmd) {
        g_ipl_cmd = (g_ipl_cmd << 8) | in;
        if (++g_ipl_pos == 4) {
            g_ipl_have_cmd = true;
            g_ipl_pos = 0;
        }
        return 0;
    }
    u32 dev = (g_ipl_cmd & 0x7FFFFFFFu) >> 6;
    bool write = g_ipl_cmd & 0x80000000u;
    u32 p = g_ipl_pos++;
    if (dev >= 0x800000u && dev < 0x800004u) {         /* RTC */
        u32 t = rtc_now();
        return write ? 0 : (u8)(t >> (8u * (3u - (p & 3u))));
    }
    if (dev >= 0x800004u && dev < 0x800044u) {         /* SRAM (addressed in bytes) */
        u32 off = (dev - 0x800004u) + p;
        if (off < sizeof(g_sram)) {
            if (write) g_sram[off] = in;
            else return g_sram[off];
        }
        return 0;
    }
    return 0;                                          /* boot ROM, UART, ...: not present */
}

/* --------------------------------------------------------- the channels --- */
/* Device on a channel's chip select: returns 0 card A/B, 1 IPL, -1 none. */
static int device_of(u32 ch, u32 csr) {
    u32 cs = (csr >> CSR_CS_SHIFT) & 7u;
    if (cs & 1u) return (ch < 2 && g_card[ch].present) ? 0 : -1;
    if ((cs & 2u) && ch == 0) return 1;
    return -1;
}

static u8 transfer_byte(u32 ch, u8 in) {
    int d = device_of(ch, g_ch[ch].csr);
    if (d == 0) return card_byte(&g_card[ch], in);
    if (d == 1) return ipl_byte(in);
    return 0;
}

static void chip_select(u32 ch, u32 old_csr, u32 new_csr) {
    int od = device_of(ch, old_csr), nd = device_of(ch, new_csr);
    u32 ocs = (old_csr >> CSR_CS_SHIFT) & 7u, ncs = (new_csr >> CSR_CS_SHIFT) & 7u;
    if (ocs == ncs)
        return;
    if (od == 0) card_deselect(&g_card[ch], &g_ch[ch]);
    if (od == 1) { g_ipl_have_cmd = false; g_ipl_pos = 0; g_ipl_cmd = 0; }
    if (nd == 0) card_select(&g_card[ch]);
    if (nd == 1) { g_ipl_have_cmd = false; g_ipl_pos = 0; g_ipl_cmd = 0; }
}

static void start_transfer(CPUState* cpu, u32 ch) {
    Channel* c = &g_ch[ch];
    bool dma = c->cr & 2u;
    u32 rw = (c->cr >> 2) & 3u;          /* 0 read, 1 write, 2 read/write */
    int d = device_of(ch, c->csr);
    if (dma) {
        u32 mar = c->mar & 0x03FFFFE0u, len = c->len;
        if (d == 0) {
            card_dma(&g_card[ch], cpu, mar, len, rw == 0);
        } else if (mar < cpu->ram_size && len <= cpu->ram_size - mar) {
            for (u32 i = 0; i < len; i++) {
                u8 out = transfer_byte(ch, rw == 0 ? 0 : cpu->ram[mar + i]);
                if (rw == 0) cpu->ram[mar + i] = out;
            }
        }
    } else {
        u32 n = ((c->cr >> 4) & 3u) + 1u, in = c->data, out = 0;
        for (u32 i = 0; i < n; i++) {
            u8 b = transfer_byte(ch, (u8)(in >> (24u - 8u * i)));
            out |= (u32)b << (24u - 8u * i);
        }
        if (rw != 1)
            c->data = out;
    }
    c->cr &= ~1u;                                    /* TSTART clears: done */
    c->csr |= CSR_TCINT;
}

bool exi_read(u32 off, u8 size, u32* out) {
    if (off < 0x6800u || off >= 0x6800u + 3u * 0x14u || size != 4)
        return false;
    u32 ch = (off - 0x6800u) / 0x14u, r = (off - 0x6800u) % 0x14u;
    Channel* c = &g_ch[ch];
    switch (r) {
    case 0x00: {
        u32 v = c->csr & ~CSR_EXT;
        if (ch < 2 && g_card[ch].present) v |= CSR_EXT;
        *out = v;
        return true;
    }
    case 0x04: *out = c->mar; return true;
    case 0x08: *out = c->len; return true;
    case 0x0C: *out = c->cr; return true;
    case 0x10: *out = c->data; return true;
    }
    return false;
}

bool exi_write(CPUState* cpu, u32 off, u32 value, u8 size) {
    if (off < 0x6800u || off >= 0x6800u + 3u * 0x14u || size != 4)
        return false;
    u32 ch = (off - 0x6800u) / 0x14u, r = (off - 0x6800u) % 0x14u;
    Channel* c = &g_ch[ch];
    switch (r) {
    case 0x00: {
        u32 old = c->csr;
        u32 keep = old & CSR_W1C & ~(value & CSR_W1C);         /* status bits: write 1 to clear */
        c->csr = (value & ~(CSR_W1C | CSR_EXT)) | keep;
        chip_select(ch, old, c->csr);
        return true;
    }
    case 0x04: c->mar = value; return true;
    case 0x08: c->len = value; return true;
    case 0x0C:
        c->cr = value;
        if (value & 1u)
            start_transfer(cpu, ch);
        return true;
    case 0x10: c->data = value; return true;
    }
    return false;
}

bool exi_interrupt_pending(void) {
    for (u32 ch = 0; ch < 3; ch++) {
        u32 s = g_ch[ch].csr;
        if (((s & CSR_EXIINTMASK) && (s & CSR_EXIINT)) || ((s & CSR_TCINTMASK) && (s & CSR_TCINT)) ||
            ((s & CSR_EXTINTMASK) && (s & CSR_EXTINT)))
            return true;
    }
    return false;
}

void exi_init(void) {
    memset(g_ch, 0, sizeof(g_ch));
    sram_init();
}

void exi_report(void) {
    for (int s = 0; s < 2; s++) {
        const Card* c = &g_card[s];
        if (c->present)
            printf("memory card %c: %s, %llu reads, %llu pages written, %llu sectors erased\n", 'A' + s, c->path,
                   c->reads, c->pages, c->erases);
    }
}
