#include "mmio.h"
#include "gx/fifo.h"
#include "runtime/disc.h"
#include "runtime/pad.h"
#include "runtime/interrupts.h"
#include "audio/ax_hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MMIO_BASE 0xCC000000u
#define MMIO_SIZE 0x10000u

/* Plain register file: whatever was last written reads back. */
static u8 g_regs[MMIO_SIZE];
static u8 g_logged_read[MMIO_SIZE / 2];
static u8 g_logged_write[MMIO_SIZE / 2];
static bool g_log = true;
static u32 g_last_read_addr, g_last_read_value;

/* ARAM: the GameCube's 16 MB auxiliary RAM, reached only by DMA. */
#define ARAM_SIZE (16u * 1024u * 1024u)
static u8* g_aram;

typedef struct {
    u32 base, size;
    const char* name;
    unsigned long long reads, writes;
} Device;

static Device g_devices[] = {
    {0xCC000000u, 0x1000, "CP", 0, 0},   /* command processor */
    {0xCC001000u, 0x1000, "PE", 0, 0},   /* pixel engine */
    {0xCC002000u, 0x1000, "VI", 0, 0},   /* video interface */
    {0xCC003000u, 0x1000, "PI", 0, 0},   /* processor interface / interrupts */
    {0xCC004000u, 0x1000, "MI", 0, 0},   /* memory interface */
    {0xCC005000u, 0x1000, "DSP", 0, 0},  /* DSP + ARAM DMA + audio DMA */
    {0xCC006000u, 0x0400, "DI", 0, 0},   /* disc interface */
    {0xCC006400u, 0x0400, "SI", 0, 0},   /* serial (controllers) */
    {0xCC006800u, 0x0400, "EXI", 0, 0},  /* memory cards, SRAM, RTC */
    {0xCC006C00u, 0x0400, "AI", 0, 0},   /* audio streaming */
    {0xCC008000u, 0x8000, "WGPIPE", 0, 0}, /* graphics FIFO write-gather pipe */
};

static Device* device_for(u32 addr) {
    for (size_t i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
        if (addr >= g_devices[i].base && addr < g_devices[i].base + g_devices[i].size)
            return &g_devices[i];
    return NULL;
}

static u32 reg_get(u32 off, u8 size) {
    u32 v = 0;
    for (u8 i = 0; i < size; i++)
        v = (v << 8) | g_regs[(off + i) & (MMIO_SIZE - 1)];
    return v;
}

static void reg_set(u32 off, u32 value, u8 size) {
    for (u8 i = 0; i < size; i++)
        g_regs[(off + i) & (MMIO_SIZE - 1)] = (u8)(value >> (8 * (size - 1 - i)));
}

static u16 reg16(u32 off) { return (u16)reg_get(off, 2); }
static void set16(u32 off, u16 v) { reg_set(off, v, 2); }

/* ---------------------------------------------------------------- DSP --- */
/* Only what the OS needs to bring the DSP up: reset/halt handshake, the boot
   mail it sends back, and instant ARAM DMA. Audio itself comes much later. */
#define DSP 0x5000u
#define DSP_CSR_RES     0x0001u
#define DSP_CSR_HALT    0x0004u
#define DSP_CSR_AIDINT  0x0008u
#define DSP_CSR_ARINT   0x0020u
#define DSP_CSR_DSPINT  0x0080u
#define DSP_CSR_W1C     (DSP_CSR_AIDINT | DSP_CSR_ARINT | DSP_CSR_DSPINT)

static u32 g_dsp_mail;
static bool g_dsp_mail_valid;
static u16 g_dsp_csr = DSP_CSR_HALT;

/* Audio DMA: while running, each buffer of length*32 bytes (16-bit stereo at
   32 kHz, 4 bytes per sample) takes that long to play, and its end raises the
   AI DMA interrupt. AX mixes one 5 ms frame per interrupt. */
static bool g_ai_dma_on;
static u64 g_ai_dma_next;

static u64 ai_dma_period(void) {
    u32 blocks = reg16(DSP + 0x36) & 0x7FFFu;
    if (!blocks)
        blocks = 0x14;
    u64 samples = (u64)blocks * 32u / 4u;
    return samples * 40500000ull / 32000ull;
}

static void aram_dma(CPUState* cpu) {
    u32 mm = reg_get(DSP + 0x20, 4) & 0x01FFFFFFu;
    u32 ar = reg_get(DSP + 0x24, 4) & 0x03FFFFFFu;
    u32 cnt = reg_get(DSP + 0x28, 4);
    bool to_ram = (cnt & 0x80000000u) != 0;
    u32 len = cnt & 0x03FFFFE0u;
    if (ar + len > ARAM_SIZE) {
        /* Beyond the internal 16 MB is where expansion ARAM would sit. There
           is none, so the transfer does nothing; ARInit's size probe relies on
           this to conclude that no expansion is fitted. */
    } else if (mm + len > cpu->ram_size) {
        fprintf(stderr, "mmio: ARAM DMA out of range (mm 0x%08X ar 0x%08X len 0x%X)\n", mm, ar, len);
    } else if (to_ram) {
        memcpy(cpu->ram + mm, g_aram + ar, len);
    } else {
        memcpy(g_aram + ar, cpu->ram + mm, len);
    }
    g_dsp_csr |= DSP_CSR_ARINT;
}

static bool dsp_read(u32 off, u8 size, u32* out) {
    u32 r = off - DSP;
    if (r == 0x04 && size == 4) {
        *out = (g_dsp_mail_valid ? 0x80000000u : 0u) | (g_dsp_mail & 0x7FFFFFFFu);
        g_dsp_mail_valid = false;
        return true;
    }
    switch (r) {
    case 0x00: /* CPU->DSP mail high: the DSP takes mail instantly */
        *out = reg16(off) & 0x7FFFu;
        return true;
    case 0x04: /* DSP->CPU mail high */
        *out = (g_dsp_mail_valid ? 0x8000u : 0u) | ((g_dsp_mail >> 16) & 0x7FFFu);
        return true;
    case 0x06: /* DSP->CPU mail low: reading it consumes the mail */
        *out = g_dsp_mail & 0xFFFFu;
        g_dsp_mail_valid = false;
        return true;
    case 0x0A: /* CSR: reset completes instantly, no DMA ever in progress */
        *out = g_dsp_csr & ~(DSP_CSR_RES | 0x0200u | 0x0400u);
        return true;
    case 0x16: /* ARAM refresh/ready: bit 0 set means ARAM is usable */
        *out = reg16(off) | 1u;
        return true;
    default:
        return false;
    }
}

/* The DSP's side of the mail protocol. There is no DSP program running, so
   this answers the way the DSP ROM and a freshly started task would:
   - after reset the first boot runs the OS's tiny init program, which reports
     0x80544348; every later boot is the ROM, which reports 0x8071FEED;
   - __DSP_boot_task then sends the task's load parameters, ending with
     0x80F3D001 and the entry point; the task starts and reports 0xDCD10000
     ("task initialised") with a DSP interrupt, which runs the task's init
     callback in the game (for AX, the audio library). */
static bool g_dsp_booted_once;
static unsigned long long g_ax_frames, g_ai_dma_blocks;
static u32 g_dsp_cpu_mail_hi;
static u32 g_dsp_prev_mail;
static unsigned g_dsp_mail_log;

static void dsp_send_to_cpu(u32 mail, bool interrupt) {
    g_dsp_mail = mail;
    g_dsp_mail_valid = true;
    if (interrupt)
        g_dsp_csr |= DSP_CSR_DSPINT;
    if (g_dsp_mail_log < 64 && (mail != 0xDCD10001u || g_ax_frames < 2)) {
        g_dsp_mail_log++;
        printf("  dsp mail -> cpu 0x%08X%s\n", mail, interrupt ? " (interrupt)" : "");
    }
}

static u32 g_dsp_ucode_addr, g_dsp_ucode_len;

/* Identifies the DSP program being started, so audio can later be handled
   by a native implementation of that exact program (AX version). */
static void dsp_log_ucode(CPUState* cpu) {
    u32 phys = g_dsp_ucode_addr & 0x01FFFFFFu;
    if (!g_dsp_ucode_len || phys >= cpu->ram_size || g_dsp_ucode_len > cpu->ram_size - phys)
        return;
    u64 h = 0xCBF29CE484222325ull;               /* FNV-1a */
    for (u32 i = 0; i < g_dsp_ucode_len; i++)
        h = (h ^ cpu->ram[phys + i]) * 0x100000001B3ull;
    printf("  dsp: starting program from 0x%08X, %u bytes, fnv64 %016llX\n",
           g_dsp_ucode_addr, g_dsp_ucode_len, (unsigned long long)h);
}

static void dsp_receive_mail(CPUState* cpu, u32 mail) {
    bool frame_mail = (mail & 0xFFFF0000u) == 0xBABE0000u || (g_dsp_prev_mail & 0xFFFF0000u) == 0xBABE0000u;
    if (g_dsp_mail_log < 64 && (!frame_mail || g_ax_frames < 2)) {
        g_dsp_mail_log++;
        printf("  dsp mail <- cpu 0x%08X\n", mail);
    }
    if (g_dsp_prev_mail == 0x80F3A001u)
        g_dsp_ucode_addr = mail;
    else if (g_dsp_prev_mail == 0x80F3A002u)
        g_dsp_ucode_len = mail;
    if (g_dsp_prev_mail == 0x80F3D001u) {
        dsp_log_ucode(cpu);
        /* That was the task's entry point: the task is now running. */
        dsp_send_to_cpu(0xDCD10000u, true);
    }
    if ((g_dsp_prev_mail & 0xFFFF0000u) == 0xBABE0000u) {
        /* AX: this mail is the address of the frame's command list (the one
           before it, 0xBABE0180, announced it). The mixing itself isn't done
           yet, but voices are advanced (ax_hle.c); then reply "resume" so AX
           knows the frame is finished, as its DSP program would. */
        g_ax_frames++;
        ax_hle_command_list(cpu, mail);
        dsp_send_to_cpu(0xDCD10001u, true);
        mail = 0;   /* an address, not a command: don't let it match anything */
    }
    g_dsp_prev_mail = mail;
}

static bool dsp_write(CPUState* cpu, u32 off, u32 value, u8 size) {
    u32 r = off - DSP;
    if (r == 0x0A && size == 2) {
        u16 v = (u16)value;
        bool was_halted = (g_dsp_csr & DSP_CSR_HALT) != 0;
        u16 keep = (u16)(v & ~DSP_CSR_W1C);
        u16 ints = (u16)(g_dsp_csr & DSP_CSR_W1C & ~(v & DSP_CSR_W1C));
        g_dsp_csr = (u16)(keep | ints);
        if (was_halted && !(g_dsp_csr & DSP_CSR_HALT)) {
            dsp_send_to_cpu(g_dsp_booted_once ? 0x8071FEEDu : 0x80544348u, false);
            g_dsp_booted_once = true;
        }
        return true;
    }
    /* CPU -> DSP mailbox: the mail is complete once the low half is written. */
    if (r == 0x00 && size == 4) {
        reg_set(off, value, 4);
        dsp_receive_mail(cpu, value);
        return true;
    }
    if (r == 0x00 && size == 2) {
        reg_set(off, value, 2);
        g_dsp_cpu_mail_hi = value & 0xFFFFu;
        return true;
    }
    if (r == 0x02 && size == 2) {
        reg_set(off, value, 2);
        dsp_receive_mail(cpu, (g_dsp_cpu_mail_hi << 16) | (value & 0xFFFFu));
        return true;
    }
    reg_set(off, value, size);
    if ((r == 0x28 && size == 4) || (r == 0x2A && size == 2))
        aram_dma(cpu);
    if (r == 0x36 && size == 2) {
        /* Audio DMA control: bit 15 starts streaming the output buffer to the
           audio interface; the low bits are its length in 32-byte blocks. */
        bool on = (value & 0x8000u) != 0;
        if (on && !g_ai_dma_on)
            g_ai_dma_next = cpu->timebase + ai_dma_period();
        g_ai_dma_on = on;
    }
    return true;
}

/* ----------------------------------------------------------------- PE --- */
/* Pixel engine interrupts: "draw done" (finish) and tokens, raised by the
   graphics command stream (gx/fifo.c). */
#define PE_INTRCTRL 0x100Au
#define PE_TOKEN    0x100Eu
#define PE_TOKEN_EN  0x1u
#define PE_FINISH_EN 0x2u
#define PE_TOKEN_INT 0x4u
#define PE_FINISH_INT 0x8u

static u16 g_pe_ctrl;

/* Frame pacing. The game's only per-frame wait is GXDrawDone, which sleeps
   until the GPU reports it has finished. Our GPU finishes instantly, so
   without pacing the game draws hundreds of frames per video field. Real
   hardware shows at most one new frame per field, so once a frame has been
   copied to the display in this field, the next "finished" report is held
   until the next field starts. */
static bool g_pacing = true;
static bool g_copied_this_field, g_finish_held;
static unsigned long long g_finish_held_count;

void mmio_set_frame_pacing(bool on) {
    g_pacing = on;
}

void mmio_pe_display_copy(void) {
    g_copied_this_field = true;
}

void mmio_pe_field(void) {
    g_copied_this_field = false;
    if (g_finish_held) {
        g_finish_held = false;
        g_pe_ctrl |= PE_FINISH_INT;
    }
}

void mmio_pe_finish(void) {
    if (g_pacing && g_copied_this_field) {
        g_finish_held = true;
        g_finish_held_count++;
        return;
    }
    g_pe_ctrl |= PE_FINISH_INT;
}

void mmio_pe_token(u16 token, bool interrupt) {
    set16(PE_TOKEN, token);
    if (interrupt)
        g_pe_ctrl |= PE_TOKEN_INT;
}

/* ----------------------------------------------------------------- DI --- */
/* Disc interface: commands complete instantly, reading from the virtual disc
   built from the extracted disc folder (runtime/disc.c). */
#define DI 0x6000u
#define DI_SR     (DI + 0x00)   /* status: masks and interrupt flags */
#define DI_CVR    (DI + 0x04)   /* cover */
#define DI_CMD0   (DI + 0x08)
#define DI_CMD1   (DI + 0x0C)
#define DI_CMD2   (DI + 0x10)
#define DI_MAR    (DI + 0x14)   /* DMA address in main RAM */
#define DI_LENGTH (DI + 0x18)
#define DI_CR     (DI + 0x1C)   /* control: start, DMA, read/write */
#define DI_IMMBUF (DI + 0x20)
#define DI_SR_DEINTMASK 0x02u
#define DI_SR_DEINT     0x04u
#define DI_SR_TCINTMASK 0x08u
#define DI_SR_TCINT     0x10u
#define DI_SR_BRKINTMASK 0x20u
#define DI_SR_BRKINT    0x40u

static u32 g_di_sr, g_di_cvr;
static unsigned g_di_logged;

static bool di_interrupt_pending(void) {
    return ((g_di_sr & DI_SR_TCINT) && (g_di_sr & DI_SR_TCINTMASK)) ||
           ((g_di_sr & DI_SR_DEINT) && (g_di_sr & DI_SR_DEINTMASK)) ||
           ((g_di_sr & DI_SR_BRKINT) && (g_di_sr & DI_SR_BRKINTMASK)) ||
           ((g_di_cvr & 0x4u) && (g_di_cvr & 0x2u));
}

static void di_execute(CPUState* cpu) {
    u32 cmd0 = reg_get(DI_CMD0, 4);
    u32 cmd = cmd0 >> 24;
    u32 cr = reg_get(DI_CR, 4);
    bool dma = (cr & 2u) != 0;
    u32 mar = reg_get(DI_MAR, 4) & 0x01FFFFE0u;
    u32 len = reg_get(DI_LENGTH, 4);

    switch (cmd) {
    case 0xA8: {   /* read: offset in 4-byte units, length in bytes */
        u64 offset = (u64)reg_get(DI_CMD1, 4) << 2;
        u32 n = reg_get(DI_CMD2, 4);
        if (g_di_logged < 64) {
            g_di_logged++;
            printf("  disc read 0x%09llX +0x%X -> 0x%08X\n", (unsigned long long)offset, n,
                   0x80000000u | mar);
        }
        if (dma) {
            if (mar + n <= cpu->ram_size)
                disc_read(offset, cpu->ram + mar, n);
            set16(DI_LENGTH, 0);
            reg_set(DI_LENGTH, 0, 4);
        } else {
            u8 word[4];
            disc_read(offset, word, 4);
            reg_set(DI_IMMBUF, ((u32)word[0] << 24) | ((u32)word[1] << 16) | ((u32)word[2] << 8) | word[3], 4);
        }
        break;
    }
    case 0x12: {   /* inquiry: drive information */
        if (dma && mar + 0x20 <= cpu->ram_size) {
            memset(cpu->ram + mar, 0, 0x20);
            cpu->ram[mar + 3] = 0x02;              /* revision */
            cpu->ram[mar + 4] = 0x20; cpu->ram[mar + 5] = 0x02;
            cpu->ram[mar + 6] = 0x04; cpu->ram[mar + 7] = 0x02;
            reg_set(DI_LENGTH, 0, 4);
        }
        break;
    }
    case 0xE0:     /* request error: no error */
        reg_set(DI_IMMBUF, 0, 4);
        break;
    case 0xAB:     /* seek */
    case 0xE1:     /* audio stream play */
    case 0xE2:     /* audio stream status */
    case 0xE3:     /* stop motor */
    case 0xE4:     /* audio buffer config */
        reg_set(DI_IMMBUF, 0, 4);
        break;
    default:
        if (g_di_logged < 64) {
            g_di_logged++;
            printf("  disc: unhandled command 0x%08X\n", cmd0);
        }
        reg_set(DI_IMMBUF, 0, 4);
        break;
    }
    (void)len;
    reg_set(DI_CR, cr & ~1u, 4);      /* transfer finished */
    g_di_sr |= DI_SR_TCINT;
}

static bool di_read(u32 off, u8 size, u32* out) {
    if (off == DI_SR && size == 4) { *out = g_di_sr; return true; }
    if (off == DI_CVR && size == 4) { *out = g_di_cvr; return true; }  /* cover closed */
    return false;
}

static bool di_write(CPUState* cpu, u32 off, u32 value, u8 size) {
    if (off == DI_SR && size == 4) {
        u32 masks = value & (DI_SR_DEINTMASK | DI_SR_TCINTMASK | DI_SR_BRKINTMASK);
        u32 flags = g_di_sr & (DI_SR_DEINT | DI_SR_TCINT | DI_SR_BRKINT);
        flags &= ~(value & (DI_SR_DEINT | DI_SR_TCINT | DI_SR_BRKINT));   /* write 1 to clear */
        g_di_sr = masks | flags;
        return true;
    }
    if (off == DI_CVR && size == 4) {
        u32 flags = (g_di_cvr & 0x4u) & ~(value & 0x4u);
        g_di_cvr = (value & 0x2u) | flags;
        return true;
    }
    if (off == DI_CR && size == 4) {
        reg_set(off, value, 4);
        if (value & 1u)
            di_execute(cpu);
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------- SI --- */
/* Serial interface: controller ports. A standard controller answers on port 1
   (channel 0); ports 2-4 are empty and report "no response". Transfers
   complete instantly; polled controller data is refreshed every video field. */
#define SI 0x6400u
#define SI_POLL   (SI + 0x30)
#define SI_COMCSR (SI + 0x34)
#define SI_SR     (SI + 0x38)
#define SI_IOBUF  (SI + 0x80)
#define SI_TCINT      0x80000000u
#define SI_TCINTMSK   0x40000000u
#define SI_COMERR     0x20000000u
#define SI_RDSTINT    0x10000000u
#define SI_RDSTINTMSK 0x08000000u

static u32 g_si_comcsr, g_si_sr;
static u32 g_si_inbuf[4][2];
static unsigned g_si_logged;

static bool si_interrupt_pending(void) {
    return ((g_si_comcsr & SI_TCINT) && (g_si_comcsr & SI_TCINTMSK)) ||
           ((g_si_comcsr & SI_RDSTINT) && (g_si_comcsr & SI_RDSTINTMSK));
}

static void si_transfer(void) {
    u32 ch = (g_si_comcsr >> 1) & 3u;
    u32 out_len = (g_si_comcsr >> 16) & 0x7Fu;
    u32 in_len = (g_si_comcsr >> 8) & 0x7Fu;
    if (!out_len) out_len = 128;
    if (!in_len) in_len = 128;
    u8 cmd = g_regs[SI_IOBUF];
    u8 resp[128];
    u32 n = 0;
    bool present = (ch == 0);

    memset(resp, 0, sizeof(resp));
    if (present) {
        switch (cmd) {
        case 0x00:   /* identify / reset: standard GameCube controller */
        case 0xFF:
            resp[0] = 0x09; resp[1] = 0x00; resp[2] = 0x00;
            n = 3;
            break;
        case 0x41:   /* read origin */
        case 0x42:   /* recalibrate */
            resp[0] = 0x00; resp[1] = 0x80;
            resp[2] = resp[3] = resp[4] = resp[5] = 0x80;   /* sticks centred */
            resp[6] = resp[7] = 0x1F;                        /* trigger rest values */
            n = 10;
            break;
        case 0x40:   /* direct poll */
            pad_poll_response(interrupts_game_seconds(), resp);
            n = 8;
            break;
        default:
            present = false;
            break;
        }
    }
    if (g_si_logged < 16) {
        g_si_logged++;
        printf("  si: port %u command 0x%02X (%u out, %u in) -> %s\n", ch + 1, cmd, out_len, in_len,
               present ? "controller" : "no device");
    }

    g_si_comcsr &= ~(SI_COMERR | 1u);              /* TSTART done */
    if (present) {
        for (u32 i = 0; i < in_len && i < sizeof(resp); i++)
            g_regs[SI_IOBUF + i] = i < n ? resp[i] : 0;
    } else {
        g_si_comcsr |= SI_COMERR;
        g_si_sr |= 0x08u << ((3 - ch) * 8);         /* no response */
    }
    g_si_comcsr |= SI_TCINT;
}

/* Once per video field: refresh the polled controller data, as the hardware
   does on its own when polling is enabled. */
void mmio_si_field(void) {
    u32 poll = reg_get(SI_POLL, 4);
    for (u32 ch = 0; ch < 4; ch++) {
        if (!(poll & (0x80u >> ch)))
            continue;
        if (ch != 0) {
            g_si_sr |= 0x08u << ((3 - ch) * 8);
            continue;
        }
        u8 r[8];
        pad_poll_response(interrupts_game_seconds(), r);
        g_si_inbuf[ch][0] = ((u32)r[0] << 24) | ((u32)r[1] << 16) | ((u32)r[2] << 8) | r[3];
        g_si_inbuf[ch][1] = ((u32)r[4] << 24) | ((u32)r[5] << 16) | ((u32)r[6] << 8) | r[7];
        g_si_sr |= 0x20u << ((3 - ch) * 8);         /* RDST: new data ready */
        g_si_comcsr |= SI_RDSTINT;
    }
}

static bool si_read(u32 off, u8 size, u32* out) {
    if (size != 4)
        return false;
    if (off == SI_COMCSR) { *out = g_si_comcsr; return true; }
    if (off == SI_SR) { *out = g_si_sr; return true; }
    if (off < SI + 0x30) {
        u32 ch = (off - SI) / 0x0Cu, r = (off - SI) % 0x0Cu;
        if (r == 4) {                                /* input buffer high: reading consumes RDST */
            *out = g_si_inbuf[ch][0];
            g_si_sr &= ~(0x20u << ((3 - ch) * 8));
            if (!(g_si_sr & 0x20202020u))
                g_si_comcsr &= ~SI_RDSTINT;
            return true;
        }
        if (r == 8) { *out = g_si_inbuf[ch][1]; return true; }
    }
    return false;
}

static bool si_write(u32 off, u32 value, u8 size) {
    if (size != 4)
        return false;
    if (off == SI_COMCSR) {
        u32 w1c = value & (SI_TCINT | SI_RDSTINT);
        g_si_comcsr = (g_si_comcsr & (SI_TCINT | SI_RDSTINT | SI_COMERR) & ~w1c) |
                      (value & ~(SI_TCINT | SI_RDSTINT | SI_COMERR));
        if (value & 1u)
            si_transfer();
        return true;
    }
    if (off == SI_SR) {
        g_si_sr &= ~(value & 0x0F0F0F0Fu);           /* write 1 to clear error bits */
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------- AI --- */
/* Audio streaming interface. Only the sample counter matters for now: while
   streaming is on it advances at 32 or 48 kHz of game time, which AIInit
   times to work out the hardware's sample-rate behaviour. */
#define AI 0x6C00u
#define AI_PSTAT   0x01u   /* playing */
#define AI_AFR     0x02u   /* 1 = 48 kHz, 0 = 32 kHz */
#define AI_AIINT   0x08u   /* interrupt status, write 1 to clear */
#define AI_SCRESET 0x20u   /* write 1 to reset the sample counter */
#define TIMEBASE_HZ 40500000ull

static u64 g_ai_base_tb;
static u32 g_ai_base_count;

static u32 ai_sample_count(CPUState* cpu) {
    u32 cr = reg_get(AI, 4);
    if (!(cr & AI_PSTAT))
        return g_ai_base_count;
    u64 rate = (cr & AI_AFR) ? 48000ull : 32000ull;
    return g_ai_base_count + (u32)((cpu->timebase - g_ai_base_tb) * rate / TIMEBASE_HZ);
}

static void ai_write_control(CPUState* cpu, u32 value) {
    u32 count = ai_sample_count(cpu);   /* freeze the count at the old rate */
    u32 old = reg_get(AI, 4);
    if (value & AI_SCRESET)
        count = 0;
    u32 status = (old & AI_AIINT) & ~(value & AI_AIINT);
    reg_set(AI, (value & ~(AI_SCRESET | AI_AIINT)) | status, 4);
    g_ai_base_count = count;
    g_ai_base_tb = cpu->timebase;
}

/* ---------------------------------------------------------------- EXI --- */
/* Transfers complete the moment they start; no devices answer yet, so SRAM
   and the RTC read as zero and memory cards are absent. */
static bool exi_write(u32 off, u32 value, u8 size) {
    u32 r = (off - 0x6800u) % 0x14u;
    u32 ch = (off - 0x6800u) / 0x14u;
    if (ch < 3 && r == 0x0C && (value & 1u)) {
        reg_set(off, value & ~1u, size);              /* TSTART clears */
        u32 csr = 0x6800u + ch * 0x14u;
        reg_set(csr, reg_get(csr, 4) | 0x8u, 4);      /* TCINT: transfer complete */
        return true;
    }
    return false;
}

/* --------------------------------------------------------------- glue --- */
static void log_access(bool write, u32 addr, u32 value, u8 size) {
    u32 slot = ((addr - MMIO_BASE) & (MMIO_SIZE - 1)) >> 1;
    u8* seen = write ? g_logged_write : g_logged_read;
    if (!g_log || seen[slot])
        return;
    seen[slot] = 1;
    Device* d = device_for(addr);
    printf("  mmio %s %-6s 0x%08X = 0x%0*X\n", write ? "W" : "R", d ? d->name : "?", addr,
           size * 2, value);
}

/* Virtual memory window at 0x7E000000-0x7FFFFFFF. The game places its
   database heap here (0x104000 bytes at 0x7E000000, set up in fn_800246C0)
   and maps it with the CPU's address translation; we simply back the whole
   window with its own host memory, as Dolphin does ("fake VMEM"). */
#define VMEM_BASE 0x7E000000u
#define VMEM_SIZE 0x02000000u
static u8* g_vmem;
static unsigned long long g_vmem_reads, g_vmem_writes, g_unmapped;

static bool in_vmem(u32 ea, u8 size) {
    return ea >= VMEM_BASE && ea - VMEM_BASE <= VMEM_SIZE - size;
}

static void warn_unmapped(const char* what, u32 ea, u8 size, u32 pc) {
    if (g_unmapped++ < 16)
        fprintf(stderr, "warn: %s%u %s unmapped 0x%08X (pc 0x%08X)\n", what, size * 8,
                what[0] == 'r' ? "from" : "to", ea, pc);
}

/* ------------------------------------------------- write-gather pipe --- */
/* Writes to 0xCC008000 are gathered into 32-byte bursts and stored in RAM at
   the CPU FIFO write pointer (PI 0x14), which advances and wraps between the
   FIFO base (PI 0x0C) and end (PI 0x10). The GPU only sees the commands while
   it is linked to that FIFO (CP control bit 4). Games unlink it to record
   display lists into RAM (GXBeginDisplayList, GXRedirectWriteGatherPipe). */
#define PI_FIFO_BASE 0x300Cu
#define PI_FIFO_END  0x3010u
#define PI_FIFO_WP   0x3014u
#define CP_CTRL      0x0002u
#define CP_CTRL_LINK 0x0010u
static u8 g_burst[32];
static u32 g_burst_len;
static bool g_cp_ctrl_written;
static unsigned long long g_pipe_to_gpu, g_pipe_recorded, g_link_changes;

static bool gp_linked(void) {
    return !g_cp_ctrl_written || (reg16(CP_CTRL) & CP_CTRL_LINK);
}

static void pipe_flush_burst(CPUState* cpu) {
    u32 wp = reg_get(PI_FIFO_WP, 4);
    u32 base = reg_get(PI_FIFO_BASE, 4) & 0x03FFFFE0u;
    /* 27 bits: GXRedirectWriteGatherPipe sets base 0, end 0x04000000 ("no end"),
       which a 26-bit mask would turn into 0. */
    u32 end = reg_get(PI_FIFO_END, 4) & 0x07FFFFFFu;
    g_burst_len = 0;
    if (end <= base)
        return;                       /* no CPU FIFO set up yet */
    u32 addr = wp & 0x03FFFFE0u, wrap = wp & 0x04000000u;
    if (addr + 32u <= cpu->ram_size)
        memcpy(cpu->ram + addr, g_burst, 32);
    addr += 32u;
    if (addr >= end) {
        addr = base;
        wrap ^= 0x04000000u;
    }
    reg_set(PI_FIFO_WP, addr | wrap, 4);
}

/* Writing WPAR (the write-gather pipe address register) resets the pipe and
   discards a partly filled burst. The SDK relies on this: after redirecting
   the pipe it pads with 31 zero bytes to push the last data out, then writes
   WPAR so the leftover zeros never reach memory. */
void mmio_wpar_written(void) {
    g_burst_len = 0;
}

static void pipe_write(CPUState* cpu, u32 value, u8 size) {
    for (int i = size - 1; i >= 0; i--) {
        g_burst[g_burst_len++] = (u8)(value >> (i * 8));
        if (g_burst_len == 32)
            pipe_flush_burst(cpu);
    }
    if (gp_linked()) {
        gx_fifo_write(cpu, value, size);
        g_pipe_to_gpu += size;
    } else {
        g_pipe_recorded += size;
    }
}

static u64 external_read(CPUState* cpu, u32 ea, u8 size) {
    if (in_vmem(ea, size)) {
        const u8* p = g_vmem + (ea - VMEM_BASE);
        u64 v = 0;
        for (u8 i = 0; i < size; i++)
            v = (v << 8) | p[i];
        g_vmem_reads++;
        return v;
    }
    if ((ea & 0xFFFF0000u) != MMIO_BASE) {
        warn_unmapped("read", ea, size, cpu->pc);
        return 0;
    }
    u32 off = ea - MMIO_BASE;
    Device* d = device_for(ea);
    if (d)
        d->reads++;

    u32 value;
    if (off == 0x3000u && size == 4)
        value = mmio_pi_cause();
    else if (off == AI + 0x08u && size == 4)
        value = ai_sample_count(cpu);
    else if (off >= DI && off < DI + 0x40 && di_read(off, size, &value))
        ;
    else if (off >= SI && off < SI + 0x40 && si_read(off, size, &value))
        ;
    else if (off == 0x0000u && size == 2)
        value = 0x000Cu;                   /* CP status: FIFO read and command processing idle */
    else if (off == PE_INTRCTRL && size == 2)
        value = g_pe_ctrl;
    else if (!(off >= DSP && off < DSP + 0x200 && dsp_read(off, size, &value)))
        value = reg_get(off, size);
    g_last_read_addr = ea;
    g_last_read_value = value;
    if (!(d && strcmp(d->name, "WGPIPE") == 0))
        log_access(false, ea, value, size);
    return value;
}

static void external_write(CPUState* cpu, u32 ea, u64 value64, u8 size) {
    u32 value = (u32)value64;
    if (in_vmem(ea, size)) {
        u8* p = g_vmem + (ea - VMEM_BASE);
        for (int i = size - 1; i >= 0; i--) {
            p[i] = (u8)value64;
            value64 >>= 8;
        }
        g_vmem_writes++;
        return;
    }
    if ((ea & 0xFFFF0000u) != MMIO_BASE) {
        warn_unmapped("write", ea, size, cpu->pc);
        return;
    }
    u32 off = ea - MMIO_BASE;
    Device* d = device_for(ea);
    if (d)
        d->writes++;

    if (off >= 0x8000u) {
        pipe_write(cpu, value, size);      /* graphics command stream */
        return;
    }
    log_access(true, ea, value, size);

    if (off == CP_CTRL && size == 2) {
        bool was = gp_linked();
        g_cp_ctrl_written = true;
        reg_set(off, value, size);
        if (gp_linked() != was)
            g_link_changes++;
        return;
    }

    if (off >= DSP && off < DSP + 0x200) {
        dsp_write(cpu, off, value, size);
        return;
    }
    if (off >= 0x6800u && off < 0x6800u + 3 * 0x14u && exi_write(off, value, size))
        return;
    if (off == AI && size == 4) {
        ai_write_control(cpu, value);
        return;
    }
    if (off >= DI && off < DI + 0x40 && di_write(cpu, off, value, size))
        return;
    if (off >= SI && off < SI + 0x40 && si_write(off, value, size))
        return;
    if (off == PE_INTRCTRL && size == 2) {
        /* Enable bits are stored; writing 1 to a status bit acknowledges it. */
        u16 v = (u16)value;
        g_pe_ctrl = (u16)((v & 0x3u) | (g_pe_ctrl & 0xCu & ~(v & 0xCu)));
        return;
    }
    reg_set(off, value, size);
}

void mmio_ai_dma_tick(CPUState* cpu) {
    while (g_ai_dma_on && cpu->timebase >= g_ai_dma_next) {
        g_dsp_csr |= DSP_CSR_AIDINT;
        g_ai_dma_next += ai_dma_period();
        g_ai_dma_blocks++;
    }
}

/* ---------------------------------------------------------- interrupts --- */
/* PI interrupt cause (0xCC003000) is computed from the devices' own status
   bits rather than stored, as on hardware. */
#define PI_INT_DSP   0x00000040u
#define PI_INT_VI    0x00000100u
#define PI_INT_RSW   0x00010000u /* set = reset button NOT pressed */

static bool dsp_interrupt_pending(void) {
    u16 c = g_dsp_csr;
    return ((c & 0x0008u) && (c & 0x0010u)) ||  /* AI DMA */
           ((c & 0x0020u) && (c & 0x0040u)) ||  /* ARAM DMA */
           ((c & 0x0080u) && (c & 0x0100u));    /* DSP */
}

u32 mmio_pi_cause(void) {
    u32 cause = PI_INT_RSW;
    for (u32 i = 0; i < 4; i++) {
        u16 hi = reg16(0x2030u + i * 4u);
        if ((hi & 0x8000u) && (hi & 0x1000u))
            cause |= PI_INT_VI;
    }
    if (dsp_interrupt_pending())
        cause |= PI_INT_DSP;
    if (di_interrupt_pending())
        cause |= 0x00000004u;   /* DI */
    if (si_interrupt_pending())
        cause |= 0x00000008u;   /* SI */
    if ((g_pe_ctrl & PE_TOKEN_INT) && (g_pe_ctrl & PE_TOKEN_EN))
        cause |= 0x00000200u;   /* PE token */
    if ((g_pe_ctrl & PE_FINISH_INT) && (g_pe_ctrl & PE_FINISH_EN))
        cause |= 0x00000400u;   /* PE finish */
    return cause;
}

u32 mmio_reg(u32 off, u8 size) {
    return reg_get(off, size);
}

u32 mmio_pi_mask(void) {
    return reg_get(0x3004u, 4);
}

void mmio_vi_raise(u32 which) {
    u32 off = 0x2030u + (which & 3u) * 4u;
    u16 hi = reg16(off);
    if (hi & 0x1000u)              /* only if that display interrupt is enabled */
        set16(off, (u16)(hi | 0x8000u));
}

void mmio_install(CPUState* cpu) {
    memset(g_regs, 0, sizeof(g_regs));
    if (!g_aram)
        g_aram = (u8*)calloc(1, ARAM_SIZE);
    if (!g_vmem)
        g_vmem = (u8*)calloc(1, VMEM_SIZE);
    set16(DSP + 0x0A, DSP_CSR_HALT);
    cpu->external_read = external_read;
    cpu->external_write = external_write;
}

void mmio_report(void) {
    printf("audio: %llu AI DMA buffers played, %llu AX frames\n", g_ai_dma_blocks, g_ax_frames);
    printf("frame pacing: %s, %llu draw-done reports held to the next field\n",
           g_pacing ? "on" : "off", g_finish_held_count);
    printf("gather pipe: %llu bytes to the GPU, %llu bytes recorded to RAM, %llu link changes\n",
           g_pipe_to_gpu, g_pipe_recorded, g_link_changes);
    printf("memory: virtual window 0x7E000000 r%llu/w%llu, unmapped accesses %llu\n",
           g_vmem_reads, g_vmem_writes, g_unmapped);
    printf("hardware register accesses:");
    for (size_t i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
        if (g_devices[i].reads || g_devices[i].writes)
            printf(" %s(r%llu/w%llu)", g_devices[i].name, g_devices[i].reads, g_devices[i].writes);
    printf("\n");
    if (g_last_read_addr) {
        Device* d = device_for(g_last_read_addr);
        printf("last hardware register read: %s 0x%08X = 0x%X\n", d ? d->name : "?",
               g_last_read_addr, g_last_read_value);
    }
}
