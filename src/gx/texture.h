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

typedef struct {
    u32 width, height;
    const u32* texels;     /* width*height, 0xAARRGGBB */
} GxTexture;

/* BP 0x64/0x65: copy a colour table from RAM into TMEM. */
void gx_tex_load_tlut(CPUState* cpu, u32 src_reg, u32 dest_reg);

/* The texture currently bound to texture map 0-7, decoded (NULL if it can't
   be read). */
const GxTexture* gx_tex_get(CPUState* cpu, u32 map);

/* Called once per displayed frame: each cached texture is re-checked against
   RAM the first time it is used in the new frame. */
void gx_tex_flush_cache(void);

/* Prints cache statistics. */
void gx_tex_report(void);

/* Decodes one texture; exposed for tests. fmt: GX texture format id.
   tlut / tlut_fmt are used for the paletted formats (4/8/14-bit index). */
bool gx_tex_decode(const u8* src, u32 fmt, u32 w, u32 h, const u8* tlut, u32 tlut_fmt, u32* out);

/* Bytes a texture of this format and size occupies in RAM. */
u32 gx_tex_size(u32 fmt, u32 w, u32 h);

#endif
