/* Community texture-pack images: JPG, PNG and TGA decoded to RGBA8.
 * GPL-2.0-or-later; decoding by the vendored stb_image (public domain/MIT). */
#ifndef LQ_PACK_IMAGES_H
#define LQ_PACK_IMAGES_H
#include "scene.h"
/* Largest pack image side accepted; Rygel's ultra pack tops out at 2048. */
#define LQ_PACK_IMAGE_MAX 4096
/* Decode an image file held in memory into `out` (RGBA8, rows top-down).
 * Returns 0, leaving `out` empty, for formats it cannot read or sizes over
 * LQ_PACK_IMAGE_MAX. */
int LQ_PackDecode(const uint8_t *data, size_t size, lq_image *out);
/* A glow (or luma) image of the colour image's size becomes its alpha: the
 * glow's brightest channel, how strongly that texel shines. Returns 0 when
 * the sizes differ. */
int LQ_PackFoldGlow(lq_image *color, const lq_image *glow);
/* Set every texel's alpha: a colour image without glow shines nowhere. */
void LQ_PackSetAlpha(lq_image *image, uint8_t alpha);
/* The next mip level of a pack image, half each side (rounded down, at least
 * 1), from 2x2 blocks. A colour image is averaged in linear light (its values
 * decoded and re-encoded with the 2.2 power world.frag uses), so distant
 * texels keep their brightness; alpha (glow) is averaged as stored. A normal
 * map's vectors are averaged and renormalized; its alpha (height) averaged.
 * Returns 0 if allocation fails. */
int LQ_PackHalve(const lq_image *source, int normal_map, lq_image *out);
/* Mean brightness of an RGBA8 image in linear light (2.2 power), Rec. 709. */
float LQ_PackLinearMean(const lq_image *image);
/* Inflate a raw deflate stream (a .pk3 entry) into exactly `size` bytes.
 * Returns 0 when it does not fill `out` exactly. */
int LQ_PackInflate(const uint8_t *data, size_t stored, uint8_t *out, size_t size);
#endif
