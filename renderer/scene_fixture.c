/* A near red triangle drawn before a far blue wall tests actual depth writes.
 * No game data or software-rasterized image is used. GPL-2.0-or-later. */
#include "scene_fixture.h"
#include <stdlib.h>
#include <string.h>
int LQ_SceneFixture(lq_scene *s)
{
    static const float positions[7][3] = {
        {64, 32, -32}, {64, -32, -32}, {64, 0, 32},
        {128, 128, -96}, {128, -128, -96}, {128, -128, 96}, {128, 128, 96}
    };
    static const uint32_t indices[] = {0, 1, 2, 3, 4, 5, 3, 5, 6};
    memset(s, 0, sizeof(*s));
    s->vertices = calloc(7, sizeof(*s->vertices)); s->indices = malloc(sizeof(indices));
    s->draws = calloc(2, sizeof(*s->draws)); s->materials = calloc(2, sizeof(*s->materials));
    s->colormap = (lq_image){256, 64, 4, calloc(256 * 64, 4)};
    s->lightmap = (lq_image){1, 1, 4, calloc(1, 4)};
    if (!s->vertices || !s->indices || !s->draws || !s->materials || !s->colormap.pixels || !s->lightmap.pixels) goto failed;
    s->vertex_count = 7; s->index_count = 9; s->draw_count = s->material_count = 2;
    for (int i = 0; i < 7; ++i) {
        memcpy(s->vertices[i].position, positions[i], sizeof(positions[i]));
        s->vertices[i].uv[0] = s->vertices[i].uv[1] = 0.5f;
        s->vertices[i].light_uv[0] = s->vertices[i].light_uv[1] = 0.5f;
    }
    memcpy(s->indices, indices, sizeof(indices));
    for (int i = 0; i < 2; ++i) {
        s->materials[i].indices = (lq_image){1, 1, 1, malloc(1)};
        if (!s->materials[i].indices.pixels) goto failed;
        *s->materials[i].indices.pixels = (uint8_t)(201 + i);
        s->draws[i] = (lq_draw){i ? 3 : 0, i ? 6 : 3, (uint32_t)i, 0, {0, 255, 255, 255}};
    }
    for (int i = 0; i < 64; ++i) {
        uint8_t *row = s->colormap.pixels + i * 256 * 4;
        row[201 * 4] = row[202 * 4 + 2] = (uint8_t)(i == 0 ? 255 : i == 31 ? 127 : 0);
        row[201 * 4 + 3] = row[202 * 4 + 3] = 255;
    }
    s->lightmap.pixels[0] = 255;
    return 1;
failed: LQ_SceneFree(s); return 0;
}
