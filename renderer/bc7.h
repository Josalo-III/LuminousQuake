/* BC7 encoding of RGBA8 images, mode 6 only. GPL-2.0-or-later.
 * Mode 6 stores each 4x4 block as one line through RGBA (7-bit endpoints with
 * a shared low bit each) and 4-bit positions along it: 16 bytes a block, a
 * quarter of RGBA8. One line per block suits pack images, whose colour and
 * glow, or normal, gloss and height, change together within a block more
 * often than not; it is fast enough to encode at load. */
#ifndef LQ_BC7_H
#define LQ_BC7_H
#include <stddef.h>
#include <stdint.h>
/* Bytes for one level of w x h texels: 16 a 4x4 block, partial blocks whole. */
size_t LQ_BC7LevelBytes(uint32_t width, uint32_t height);
/* Encode one level: `rgba` is width x height RGBA8, rows top-down; `out`
 * receives LQ_BC7LevelBytes bytes, blocks in rows. Edge blocks repeat the
 * last row and column. */
void LQ_BC7EncodeLevel(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *out);
/* Decode one block to 16 RGBA8 texels (for tests). */
void LQ_BC7DecodeBlock(const uint8_t block[16], uint8_t rgba[64]);
#endif
