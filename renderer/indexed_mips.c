/* Average colors, then quantize to Quake's palette. Never average indices.
 * Keep wholly fullbright blocks in the fullbright range. GPL-2.0-or-later. */
#include "indexed_mips.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
void LQ_MipPaletteInit(lq_mip_palette *p, const uint8_t *rgba)
{
    memcpy(p->colors, rgba, sizeof(p->colors));
    memset(p->keys, 255, sizeof(p->keys));
}
static uint8_t Match(lq_mip_palette *p, const unsigned rgb[3], int fullbright)
{
    uint32_t key = rgb[0] | rgb[1]<<8 | rgb[2]<<16 | (uint32_t)fullbright<<24;
    uint32_t slot = (key*2654435761u)>>16;
    if (p->keys[slot] == key) return p->matches[slot];
    unsigned best = UINT_MAX; int match = 0;
    for (int i = fullbright ? 224 : 0; i < (fullbright ? 256 : 224); ++i) {
        unsigned distance = 0;
        for (int c = 0; c < 3; ++c) {
            int d = (int)rgb[c] - p->colors[i][c]; distance += d*d;
        }
        if (distance < best) { best = distance; match = i; }
    }
    p->keys[slot] = key; p->matches[slot] = match; return match;
}
int LQ_IndexedMips(const lq_image *im, lq_mip_palette *p, lq_mip_chain *m)
{
    memset(m, 0, sizeof(*m));
    if (!im || !p || !im->pixels || im->channels != 1 || !im->width || !im->height ||
        im->width > 8192 || im->height > 8192) return 0;
    uint32_t w = im->width, h = im->height;
    for (;;) {
        unsigned n = m->count++;
        m->offsets[n] = m->bytes; m->widths[n] = w; m->heights[n] = h;
        m->bytes += w*h;
        if (w == 1 && h == 1) break;
        w = w > 1 ? w/2 : 1; h = h > 1 ? h/2 : 1;
    }
    m->pixels = malloc(m->bytes);
    if (!m->pixels) return 0;
    memcpy(m->pixels, im->pixels, im->width*im->height);
    for (unsigned n = 1; n < m->count; ++n) {
        w = m->widths[n]; h = m->heights[n];
        unsigned pw = m->widths[n-1], ph = m->heights[n-1];
        const uint8_t *src = m->pixels+m->offsets[n-1];
        uint8_t *dst = m->pixels+m->offsets[n];
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            unsigned sum[3] = {0}, count = 0; int same = 1, first = -1, bright = 1;
            /* Area bounds include the final row/column of odd-sized images. */
            for (unsigned j = y*ph/h; j < (y+1)*ph/h; ++j)
                for (unsigned i = x*pw/w; i < (x+1)*pw/w; ++i) {
                    unsigned index = src[j*pw+i];
                    if (first < 0) first = index; else if (first != (int)index) same = 0;
                    bright &= index >= 224;
                    for (int c = 0; c < 3; ++c) sum[c] += p->colors[index][c];
                    ++count;
                }
            for (int c = 0; c < 3; ++c) sum[c] = (sum[c]+count/2)/count;
            dst[y*w+x] = same ? first : Match(p, sum, bright);
        }
    }
    return 1;
}
