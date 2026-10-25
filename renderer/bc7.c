/* BC7 mode 6 encoder. GPL-2.0-or-later. See bc7.h.
 * Block layout (bit 0 first): mode 6 as 0000001 (7 bits), then R0 R1 G0 G1
 * B0 B1 A0 A1 at 7 bits each, P0 and P1, then 16 indices at 4 bits, the
 * first (the anchor) at 3 bits with its top bit taken as 0. An endpoint
 * channel is (value << 1) | P; a texel is ((64 - w) * e0 + w * e1 + 32) >> 6
 * with w from the 4-bit weight table. */
#include "bc7.h"
#include <math.h>
#include <string.h>

static const int weights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

size_t LQ_BC7LevelBytes(uint32_t width, uint32_t height)
{
    return (size_t)((width + 3) / 4) * ((height + 3) / 4) * 16;
}

/* The texel each index gives between quantized endpoints e0 and e1. */
static void Palette(const int e0[4], const int e1[4], int out[16][4])
{
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 4; ++c) out[i][c] = ((64 - weights[i]) * e0[c] + weights[i] * e1[c] + 32) >> 6;
}

/* Best index for every texel against a palette; returns the summed squared error. */
static long Assign(const uint8_t texels[16][4], const int palette[16][4], uint8_t index[16])
{
    long total = 0;
    for (int t = 0; t < 16; ++t) {
        long best = 0x7fffffff; int which = 0;
        for (int i = 0; i < 16; ++i) {
            long e = 0;
            for (int c = 0; c < 4; ++c) { long d = (long)texels[t][c] - palette[i][c]; e += d * d; }
            if (e < best) { best = e; which = i; }
        }
        index[t] = (uint8_t)which; total += best;
    }
    return total;
}

/* Quantize two float endpoints with the given shared low bits. */
static void Quantize(const float a[4], const float b[4], int p0, int p1, int q0[4], int q1[4], int e0[4], int e1[4])
{
    for (int c = 0; c < 4; ++c) {
        int v0 = (int)floorf((a[c] - p0) * 0.5f + 0.5f), v1 = (int)floorf((b[c] - p1) * 0.5f + 0.5f);
        q0[c] = v0 < 0 ? 0 : v0 > 127 ? 127 : v0; q1[c] = v1 < 0 ? 0 : v1 > 127 ? 127 : v1;
        e0[c] = (q0[c] << 1) | p0; e1[c] = (q1[c] << 1) | p1;
    }
}

typedef struct { long error; int q0[4], q1[4], p0, p1; uint8_t index[16]; } fit;

/* The best of the four shared-bit choices for endpoints a and b. */
static fit Fit(const uint8_t texels[16][4], const float a[4], const float b[4])
{
    fit best; best.error = -1;
    for (int p = 0; p < 4; ++p) {
        fit f; int e0[4], e1[4], palette[16][4];
        f.p0 = p & 1; f.p1 = p >> 1;
        Quantize(a, b, f.p0, f.p1, f.q0, f.q1, e0, e1);
        Palette(e0, e1, palette);
        f.error = Assign(texels, palette, f.index);
        if (best.error < 0 || f.error < best.error) best = f;
    }
    return best;
}

static void PutBits(uint8_t out[16], int *at, uint32_t value, int count)
{
    for (int i = 0; i < count; ++i, ++*at)
        if (value & (1u << i)) out[*at >> 3] |= (uint8_t)(1u << (*at & 7));
}

static void EncodeBlock(const uint8_t texels[16][4], uint8_t out[16])
{
    /* Principal axis through RGBA: power iteration on the covariance,
     * starting from the bounding box's diagonal. */
    float mean[4] = {0}, lo[4] = {255, 255, 255, 255}, hi[4] = {0};
    for (int t = 0; t < 16; ++t)
        for (int c = 0; c < 4; ++c) {
            mean[c] += texels[t][c] / 16.0f;
            if (texels[t][c] < lo[c]) lo[c] = texels[t][c];
            if (texels[t][c] > hi[c]) hi[c] = texels[t][c];
        }
    float cov[4][4] = {{0}};
    for (int t = 0; t < 16; ++t)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) cov[i][j] += (texels[t][i] - mean[i]) * (texels[t][j] - mean[j]);
    float axis[4];
    for (int c = 0; c < 4; ++c) axis[c] = hi[c] - lo[c];
    for (int k = 0; k < 6; ++k) {
        float next[4] = {0}, length = 0;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) next[i] += cov[i][j] * axis[j];
        for (int c = 0; c < 4; ++c) length += next[c] * next[c];
        if (length < 1e-12f) break;
        length = 1.0f / sqrtf(length);
        for (int c = 0; c < 4; ++c) axis[c] = next[c] * length;
    }
    float length = 0;
    for (int c = 0; c < 4; ++c) length += axis[c] * axis[c];
    float a[4], b[4];
    if (length < 1e-12f) {
        memcpy(a, mean, sizeof(a)); memcpy(b, mean, sizeof(b));
    } else {
        length = 1.0f / sqrtf(length);
        for (int c = 0; c < 4; ++c) axis[c] *= length;
        float tmin = 1e9f, tmax = -1e9f;
        for (int t = 0; t < 16; ++t) {
            float d = 0;
            for (int c = 0; c < 4; ++c) d += (texels[t][c] - mean[c]) * axis[c];
            if (d < tmin) tmin = d;
            if (d > tmax) tmax = d;
        }
        for (int c = 0; c < 4; ++c) {
            a[c] = fminf(fmaxf(mean[c] + axis[c] * tmin, 0), 255);
            b[c] = fminf(fmaxf(mean[c] + axis[c] * tmax, 0), 255);
        }
    }
    fit best = Fit(texels, a, b);
    /* One least-squares pass: endpoints that best fit the chosen positions. */
    if (best.error > 0) {
        float s00 = 0, s01 = 0, s11 = 0, r0[4] = {0}, r1[4] = {0};
        for (int t = 0; t < 16; ++t) {
            float w = weights[best.index[t]] / 64.0f, u = 1 - w;
            s00 += u * u; s01 += u * w; s11 += w * w;
            for (int c = 0; c < 4; ++c) { r0[c] += u * texels[t][c]; r1[c] += w * texels[t][c]; }
        }
        float det = s00 * s11 - s01 * s01;
        if (fabsf(det) > 1e-6f) {
            float na[4], nb[4];
            for (int c = 0; c < 4; ++c) {
                na[c] = fminf(fmaxf((r0[c] * s11 - r1[c] * s01) / det, 0), 255);
                nb[c] = fminf(fmaxf((r1[c] * s00 - r0[c] * s01) / det, 0), 255);
            }
            fit refined = Fit(texels, na, nb);
            if (refined.error < best.error) best = refined;
        }
    }
    /* The anchor's index must have its top bit clear: swap the ends if not. */
    if (best.index[0] & 8) {
        for (int c = 0; c < 4; ++c) { int q = best.q0[c]; best.q0[c] = best.q1[c]; best.q1[c] = q; }
        int p = best.p0; best.p0 = best.p1; best.p1 = p;
        for (int t = 0; t < 16; ++t) best.index[t] = (uint8_t)(15 - best.index[t]);
    }
    memset(out, 0, 16);
    int at = 0;
    PutBits(out, &at, 1u << 6, 7);
    for (int c = 0; c < 4; ++c) { PutBits(out, &at, (uint32_t)best.q0[c], 7); PutBits(out, &at, (uint32_t)best.q1[c], 7); }
    PutBits(out, &at, (uint32_t)best.p0, 1); PutBits(out, &at, (uint32_t)best.p1, 1);
    PutBits(out, &at, best.index[0], 3);
    for (int t = 1; t < 16; ++t) PutBits(out, &at, best.index[t], 4);
}

void LQ_BC7EncodeLevel(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *out)
{
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t texels[16][4];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    uint32_t sx = bx * 4 + x, sy = by * 4 + y;
                    if (sx >= width) sx = width - 1;
                    if (sy >= height) sy = height - 1;
                    memcpy(texels[y * 4 + x], rgba + ((size_t)sy * width + sx) * 4, 4);
                }
            EncodeBlock(texels, out + ((size_t)by * bw + bx) * 16);
        }
}

static uint32_t GetBits(const uint8_t block[16], int *at, int count)
{
    uint32_t value = 0;
    for (int i = 0; i < count; ++i, ++*at)
        if (block[*at >> 3] & (1u << (*at & 7))) value |= 1u << i;
    return value;
}

void LQ_BC7DecodeBlock(const uint8_t block[16], uint8_t rgba[64])
{
    int at = 7, e0[4], e1[4], palette[16][4];
    uint32_t q[8];
    for (int i = 0; i < 8; ++i) q[i] = GetBits(block, &at, 7);
    int p0 = (int)GetBits(block, &at, 1), p1 = (int)GetBits(block, &at, 1);
    for (int c = 0; c < 4; ++c) { e0[c] = (int)(q[c * 2] << 1) | p0; e1[c] = (int)(q[c * 2 + 1] << 1) | p1; }
    Palette(e0, e1, palette);
    for (int t = 0; t < 16; ++t) {
        uint32_t index = GetBits(block, &at, t ? 4 : 3);
        for (int c = 0; c < 4; ++c) rgba[t * 4 + c] = (uint8_t)palette[index][c];
    }
}
