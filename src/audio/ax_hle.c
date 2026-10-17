#include "audio/ax_hle.h"

#include <stdio.h>

/* AXPB field offsets (bytes), from the SDK's ax.h. */
#define PB_NEXT_HI      0x00
#define PB_NEXT_LO      0x02
#define PB_SRC_SELECT   0x08
#define PB_STATE        0x0E
#define PB_ADDR_LOOP    0x6E   /* loopFlag */
#define PB_ADDR_FORMAT  0x70
#define PB_ADDR_LOOPA   0x72   /* loopAddressHi, Lo */
#define PB_ADDR_END     0x76   /* endAddressHi, Lo */
#define PB_ADDR_CUR     0x7A   /* currentAddressHi, Lo */
#define PB_ADPCM_PS     0xA0   /* adpcm.pred_scale */
#define PB_ADPCM_YN1    0xA2
#define PB_ADPCM_YN2    0xA4
#define PB_SRC_RATIO    0xA6   /* src.ratioHi, Lo (16.16 fixed point) */
#define PB_SRC_FRAC     0xAA   /* src.currentAddressFrac */
#define PB_LOOP_PS      0xB4   /* adpcmLoop.loop_pred_scale */
#define PB_LOOP_YN1     0xB6
#define PB_LOOP_YN2     0xB8

#define AX_STATE_STOP 0
#define AX_STATE_RUN  1
#define AX_SRC_NONE   2
#define AX_FMT_ADPCM  0x00

#define SAMPLES_PER_FRAME 160   /* 5 ms at 32 kHz */

static unsigned long long g_lists, g_voice_frames, g_voices_ended;
static u32 g_max_voices;

static u8* ram_ptr(CPUState* cpu, u32 addr, u32 size) {
    u32 p = addr & 0x01FFFFFFu;
    return (p + size <= cpu->ram_size) ? cpu->ram + p : NULL;
}

static u32 rd16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 rd32h(const u8* p) { return (rd16(p) << 16) | rd16(p + 2); }   /* hi/lo u16 pair */
static void wr16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32h(u8* p, u32 v) { wr16(p, v >> 16); wr16(p + 2, v & 0xFFFFu); }

/* ADPCM addresses count nibbles; every 8-byte frame starts with a 2-nibble
   header (predictor/scale), so 14 of each 16 nibbles are samples. */
static u32 step_adpcm(u32 a) {
    a++;
    if ((a & 15u) == 0)
        a += 2;
    return a;
}

static void process_voice(u8* pb) {
    if (rd16(pb + PB_STATE) != AX_STATE_RUN)
        return;
    g_voice_frames++;

    bool adpcm = rd16(pb + PB_ADDR_FORMAT) == AX_FMT_ADPCM;
    bool looping = rd16(pb + PB_ADDR_LOOP) != 0;
    u32 cur = rd32h(pb + PB_ADDR_CUR);
    u32 end = rd32h(pb + PB_ADDR_END);
    u32 loop = rd32h(pb + PB_ADDR_LOOPA);

    /* How many source samples this frame consumes: 160 output samples at the
       voice's resampling ratio, carrying the fractional part across frames. */
    u32 ratio = rd16(pb + PB_SRC_SELECT) == AX_SRC_NONE ? 0x10000u : rd32h(pb + PB_SRC_RATIO);
    u64 total = (u64)rd16(pb + PB_SRC_FRAC) + (u64)ratio * SAMPLES_PER_FRAME;
    u32 advance = (u32)(total >> 16);
    wr16(pb + PB_SRC_FRAC, (u32)(total & 0xFFFFu));

    for (u32 i = 0; i < advance; i++) {
        if (cur == end) {
            if (!looping) {
                wr16(pb + PB_STATE, AX_STATE_STOP);
                g_voices_ended++;
                break;
            }
            cur = loop;
            if (adpcm) {   /* restart the decoder state from the loop point */
                wr16(pb + PB_ADPCM_PS, rd16(pb + PB_LOOP_PS));
                wr16(pb + PB_ADPCM_YN1, rd16(pb + PB_LOOP_YN1));
                wr16(pb + PB_ADPCM_YN2, rd16(pb + PB_LOOP_YN2));
            }
            continue;
        }
        cur = adpcm ? step_adpcm(cur) : cur + 1;
    }
    wr32h(pb + PB_ADDR_CUR, cur);
}

static void process_pb_list(CPUState* cpu, u32 addr) {
    u32 count = 0;
    while (addr && count < 128) {          /* AX has at most 64 voices */
        u8* pb = ram_ptr(cpu, addr, 0xC0);
        if (!pb)
            break;
        process_voice(pb);
        addr = rd32h(pb + PB_NEXT_HI);
        count++;
    }
    if (count > g_max_voices)
        g_max_voices = count;
}

void ax_hle_command_list(CPUState* cpu, u32 address) {
    g_lists++;
    for (u32 n = 0; n < 192; n++) {        /* a list is at most 384 u16 words */
        u8* p = ram_ptr(cpu, address, 2);
        if (!p)
            return;
        u32 op = rd16(p);
        u8* args = p + 2;
        switch (op) {
        case 0x00:                          /* studio (main mix) buffer */
        case 0x07:                          /* surround buffer */
        case 0x11:                          /* Dolby Pro Logic II buffer */
            address += 2 + 4;
            break;
        case 0x02: {                        /* voice list */
            u8* a = ram_ptr(cpu, address + 2, 4);
            if (a)
                process_pb_list(cpu, rd32h(a));
            address += 2 + 4;
            break;
        }
        case 0x03:                          /* process voices */
            address += 2;
            break;
        case 0x04:                          /* aux A send/return */
        case 0x05:                          /* aux B */
        case 0x10:                          /* aux B (DPL2) */
        case 0x12:                          /* compressor */
        case 0x0E:                          /* output to the audio DMA buffer */
            address += 2 + 8;
            break;
        case 0x13:                          /* aux A (DPL2) */
            address += 2 + 24;
            break;
        case 0x0F:                          /* end of list */
            return;
        default:
            (void)args;
            return;                         /* unknown: stop rather than misparse */
        }
    }
}

void ax_hle_report(void) {
    printf("ax: %llu command lists, %llu voice-frames processed, %llu sounds ended, up to %u voices listed\n",
           g_lists, g_voice_frames, g_voices_ended, g_max_voices);
}
