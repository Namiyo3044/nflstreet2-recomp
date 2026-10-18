#ifndef NFLS2_GX_TEXTURE_H
#define NFLS2_GX_TEXTURE_H

#include "cpu/cpu.h"

/* GameCube texture decoding.
 *
 * Textures live in main RAM in the GPU's tiled formats; paletted ones use a
 * colour table (TLUT) that the game first copies into texture memory (TMEM).
 * Decoded textures are cached as plain RGBA8 across frames, and decoded
 * again only when their bytes in RAM (or their colour table) change.
 */

#define GX_TEX_MAX_LEVELS 11

typedef struct {
    u32 width, height;     /* level 0 */
    const u32* texels;     /* all levels one after another, 0xAARRGGBB */
    u32 levels;            /* mip levels decoded (1 = no mipmaps) */
    u32 lw[GX_TEX_MAX_LEVELS], lh[GX_TEX_MAX_LEVELS];
    u32 off[GX_TEX_MAX_LEVELS];  /* texel offset of each level in texels */
} GxTexture;

/* BP 0x60-0x63: preload a texture from RAM into TMEM (GXPreLoadEntireTexture).
   Textures marked as preloaded are then read from TMEM, not RAM. */
void gx_tex_preload(CPUState* cpu);

/* BP 0x64/0x65: copy a colour table from RAM into TMEM. */
void gx_tex_load_tlut(CPUState* cpu, u32 src_reg, u32 dest_reg);

/* The texture currently bound to texture map 0-7, decoded with its mip
   levels when its filter uses them (NULL if it can't be read). */
const GxTexture* gx_tex_get(CPUState* cpu, u32 map);

/* Called once per displayed frame: each cached texture is re-checked against
   RAM the first time it is used in the new frame. */
void gx_tex_flush_cache(void);

/* Frees decoded buffers that were replaced or evicted. Call only when no
   frame that may use them is still being drawn. */
void gx_tex_collect_garbage(void);

/* Called with each decoded buffer just before it is freed (the GPU backend
   drops its copy). */
void gx_tex_set_free_hook(void (*hook)(const u32* texels));

/* Prints cache statistics. */
void gx_tex_report(void);

/* Decodes one texture; exposed for tests. fmt: GX texture format id.
   tlut / tlut_fmt are used for the paletted formats (4/8/14-bit index). */
bool gx_tex_decode(const u8* src, u32 fmt, u32 w, u32 h, const u8* tlut, u32 tlut_fmt, u32* out);

/* Bytes a texture of this format and size occupies in RAM. */
u32 gx_tex_size(u32 fmt, u32 w, u32 h);

/* The GPU wrote texture data to RAM (a frame-buffer copy): cached textures
   overlapping [addr, addr + size) are decoded again on their next use. */
void gx_tex_invalidate(u32 addr, u32 size);

#endif
