/* Palette-aware mip generation. GPL-2.0-or-later. */
#ifndef LQ_INDEXED_MIPS_H
#define LQ_INDEXED_MIPS_H
#include "scene.h"
typedef struct {
    uint8_t colors[256][4], matches[65536];
    uint32_t keys[65536];
} lq_mip_palette;
typedef struct {
    uint8_t *pixels;
    uint32_t count, bytes, offsets[14], widths[14], heights[14];
} lq_mip_chain;
void LQ_MipPaletteInit(lq_mip_palette *palette, const uint8_t *rgba);
int LQ_IndexedMips(const lq_image *image, lq_mip_palette *palette, lq_mip_chain *chain);
#endif
