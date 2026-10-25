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
/* A pack normal map as Luminous stores it: red and green the tangent-space
 * normal's x and y (z is rebuilt from them, as it points out of the face),
 * blue the gloss map's strength (its brightest channel; 0 where there is no
 * gloss), alpha the height. `gloss` may be empty; one of another size is
 * resampled to the normal map's. Without a normal map but with gloss,
 * `normal` becomes a flat one of the gloss's size at full height. Returns 0
 * if allocation fails. */
int LQ_PackGlossIntoNormal(lq_image *normal, const lq_image *gloss);
/* A normal map from a height-only _bump image, as DarkPlaces makes one
 * (Image_HeightmapToNormalmap: neighbouring heights' differences, wrapping at
 * the edges, against 1530 / bumpscale; it uses r_shadow_bumpscale_bumpmap 4),
 * in this engine's layout: x and y in red and green, no gloss in blue, the
 * height in alpha. Returns 0, leaving `out` empty, if allocation fails. */
int LQ_PackNormalFromBump(const lq_image *bump, float bumpscale, lq_image *out);
/* The next mip level of a pack image, half each side (rounded down, at least
 * 1), from 2x2 blocks. A colour image is averaged in linear light (its values
 * decoded and re-encoded with the 2.2 power world.frag uses), so distant
 * texels keep their brightness; alpha (glow) is averaged as stored. A normal
 * map's vectors (x and y in red and green, z rebuilt) are averaged and
 * renormalized; its blue (gloss) and alpha (height) averaged as stored.
 * Returns 0 if allocation fails. */
int LQ_PackHalve(const lq_image *source, int normal_map, lq_image *out);
/* Mean brightness of an RGBA8 image in linear light (2.2 power), Rec. 709. */
float LQ_PackLinearMean(const lq_image *image);
/* Inflate a raw deflate stream (a .pk3 entry) into exactly `size` bytes.
 * Returns 0 when it does not fill `out` exactly. */
int LQ_PackInflate(const uint8_t *data, size_t stored, uint8_t *out, size_t size);
#endif
