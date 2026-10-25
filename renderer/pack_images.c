/* Community texture-pack image decoding. GPL-2.0-or-later. */
#include "pack_images.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS LQ_PACK_IMAGE_MAX
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "stb/stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int LQ_PackInflate(const uint8_t *data, size_t stored, uint8_t *out, size_t size)
{
    if (!data || !out || stored > 0x7fffffff || size > 0x7fffffff) return 0;
    return stbi_zlib_decode_noheader_buffer((char *)out, (int)size, (const char *)data, (int)stored) == (int)size;
}
int LQ_PackDecode(const uint8_t *data, size_t size, lq_image *out)
{
    memset(out, 0, sizeof(*out));
    if (!data || !size || size > 0x7fffffff) return 0;
    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels) return 0;
    if (w <= 0 || h <= 0 || w > LQ_PACK_IMAGE_MAX || h > LQ_PACK_IMAGE_MAX) { stbi_image_free(pixels); return 0; }
    /* Hand the scene memory it frees with free(): stb's allocator is malloc. */
    *out = (lq_image){(uint32_t)w, (uint32_t)h, 4, pixels};
    return 1;
}
int LQ_PackFoldGlow(lq_image *color, const lq_image *glow)
{
    if (!color || !glow || !color->pixels || !glow->pixels || color->channels != 4 || glow->channels != 4 ||
        color->width != glow->width || color->height != glow->height) return 0;
    size_t count = (size_t)color->width * color->height;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *g = glow->pixels + i * 4;
        uint8_t m = g[0] > g[1] ? g[0] : g[1];
        color->pixels[i * 4 + 3] = m > g[2] ? m : g[2];
    }
    return 1;
}
void LQ_PackSetAlpha(lq_image *image, uint8_t alpha)
{
    if (!image || !image->pixels || image->channels != 4) return;
    size_t count = (size_t)image->width * image->height;
    for (size_t i = 0; i < count; ++i) image->pixels[i * 4 + 3] = alpha;
}
static float decode_table[256];
static void DecodeTable(void)
{
    if (decode_table[255] == 0) for (int i = 0; i < 256; ++i) decode_table[i] = powf(i / 255.0f, 2.2f);
}
int LQ_PackGlossIntoNormal(lq_image *normal, const lq_image *gloss)
{
    int has_gloss = gloss && gloss->pixels && gloss->channels == 4 && gloss->width && gloss->height;
    if (!normal) return 0;
    if (!normal->pixels) {
        if (!has_gloss) return 1;
        size_t count = (size_t)gloss->width * gloss->height;
        uint8_t *flat = malloc(count * 4);
        if (!flat) return 0;
        for (size_t i = 0; i < count; ++i) { flat[i*4] = 128; flat[i*4+1] = 128; flat[i*4+2] = 0; flat[i*4+3] = 255; }
        *normal = (lq_image){gloss->width, gloss->height, 4, flat};
    }
    if (normal->channels != 4) return 0;
    for (uint32_t y = 0; y < normal->height; ++y)
        for (uint32_t x = 0; x < normal->width; ++x) {
            uint8_t strength = 0;
            if (has_gloss) {
                uint32_t gx = (uint32_t)((uint64_t)x * gloss->width / normal->width);
                uint32_t gy = (uint32_t)((uint64_t)y * gloss->height / normal->height);
                const uint8_t *g = gloss->pixels + ((size_t)gy * gloss->width + gx) * 4;
                strength = g[0] > g[1] ? g[0] : g[1];
                if (g[2] > strength) strength = g[2];
            }
            normal->pixels[((size_t)y * normal->width + x) * 4 + 2] = strength;
        }
    return 1;
}
int LQ_PackHalve(const lq_image *source, int normal_map, lq_image *out)
{
    memset(out, 0, sizeof(*out));
    if (!source || !source->pixels || source->channels != 4) return 0;
    uint32_t w = source->width > 1 ? source->width / 2 : 1, h = source->height > 1 ? source->height / 2 : 1;
    uint8_t *pixels = malloc((size_t)w * h * 4);
    if (!pixels) return 0;
    DecodeTable();
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float sum[4] = {0, 0, 0, 0}, gloss = 0; int count = 0;
            for (uint32_t dy = 0; dy < 2; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    uint32_t sx = x * 2 + dx, sy = y * 2 + dy;
                    if (sx >= source->width || sy >= source->height) continue;
                    const uint8_t *p = source->pixels + ((size_t)sy * source->width + sx) * 4;
                    if (normal_map) {
                        float nx = p[0] / 127.5f - 1, ny = p[1] / 127.5f - 1;
                        sum[0] += nx; sum[1] += ny; sum[2] += sqrtf(fmaxf(1 - nx * nx - ny * ny, 0));
                        gloss += p[2];
                    } else
                        for (int c = 0; c < 3; ++c) sum[c] += decode_table[p[c]];
                    sum[3] += p[3]; ++count;
                }
            uint8_t *q = pixels + ((size_t)y * w + x) * 4;
            if (normal_map) {
                float length = sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                float v[3] = {0, 0, 1};
                if (length > 1e-6f) for (int c = 0; c < 3; ++c) v[c] = sum[c] / length;
                for (int c = 0; c < 2; ++c) q[c] = (uint8_t)fminf(fmaxf((v[c] + 1) * 127.5f + 0.5f, 0), 255);
                q[2] = (uint8_t)(gloss / count + 0.5f);
            } else
                for (int c = 0; c < 3; ++c) q[c] = (uint8_t)fminf(powf(sum[c] / count, 1 / 2.2f) * 255 + 0.5f, 255);
            q[3] = (uint8_t)(sum[3] / count + 0.5f);
        }
    *out = (lq_image){w, h, 4, pixels};
    return 1;
}
float LQ_PackLinearMean(const lq_image *image)
{
    if (!image || !image->pixels || image->channels != 4) return 0;
    DecodeTable();
    size_t count = (size_t)image->width * image->height;
    double sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *p = image->pixels + i * 4;
        sum += 0.2126 * decode_table[p[0]] + 0.7152 * decode_table[p[1]] + 0.0722 * decode_table[p[2]];
    }
    return count ? (float)(sum / count) : 0;
}
