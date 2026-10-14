/* Scene validation and Quake-to-GPU projection. GPL-2.0-or-later. */
#include "scene.h"
#include "scene_fixture.h"
#include "indexed_mips.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
int main(void)
{
    lq_mip_palette *palette = malloc(sizeof(*palette));
    uint8_t colors[256*4] = {0};
    CHECK(palette);
    colors[4] = 255; colors[8+2] = 255;
    colors[12] = colors[12+2] = 128;
    colors[16] = 170; colors[18] = 85;
    LQ_MipPaletteInit(palette, colors);
    uint8_t checker[] = {1,2,2,1}; lq_mip_chain chain;
    lq_image image = {2,2,1,checker};
    CHECK(LQ_IndexedMips(&image,palette,&chain));
    CHECK(chain.count == 2 && chain.bytes == 5 && chain.pixels[4] == 3);
    free(chain.pixels);
    uint8_t odd[] = {1,2,1}; image = (lq_image){3,1,1,odd};
    CHECK(LQ_IndexedMips(&image,palette,&chain));
    CHECK(chain.count == 2 && chain.widths[1] == 1 && chain.heights[1] == 1 && chain.pixels[3] == 4);
    free(chain.pixels);
    uint8_t bright[] = {224,225,225,224};
    image = (lq_image){2,2,1,bright};
    CHECK(LQ_IndexedMips(&image,palette,&chain) && chain.pixels[4] >= 224);
    free(chain.pixels);
    image.width = 8193; CHECK(!LQ_IndexedMips(&image,palette,&chain));
    image.width = 1; image.height = 1; image.pixels = bright;
    CHECK(LQ_IndexedMips(&image,palette,&chain) && chain.count == 1 && chain.pixels[0] == 224);
    free(chain.pixels); free(palette);
    puts("PASS color-aware indexed mips, fullbrights, odd and 1x1 sizes, bounds");
    lq_scene scene; lq_view view = {0}; char error[256]; float matrix[16];
    CHECK(LQ_SceneFixture(&scene)); CHECK(LQ_SceneCheck(&scene, error, sizeof(error)));
    scene.indices[0] = scene.vertex_count;
    CHECK(!LQ_SceneCheck(&scene, error, sizeof(error))); scene.indices[0] = 0;
    scene.draws[0].index_count = 0xffffffff;
    CHECK(!LQ_SceneCheck(&scene, error, sizeof(error))); scene.draws[0].index_count = 3;
    scene.vertices[0].uv[0] = NAN;
    CHECK(!LQ_SceneCheck(&scene, error, sizeof(error))); scene.vertices[0].uv[0] = 0.5f;
    view.horizontal_fov = 90; LQ_ViewMatrix(&view, 4.0f / 3, matrix);
    /* A point straight ahead has zero screen displacement and depth in [0,1]. */
    CHECK(fabsf(matrix[0] * 64 + matrix[12]) < 0.0001f);
    CHECK(fabsf(matrix[1] * 64 + matrix[13]) < 0.0001f);
    CHECK((matrix[2] * 64 + matrix[14]) / (matrix[3] * 64 + matrix[15]) > 0);
    CHECK((matrix[2] * 64 + matrix[14]) / (matrix[3] * 64 + matrix[15]) < 1);
    CHECK(matrix[4] < 0 && matrix[9] > 0); /* +Y is screen left; +Z screen up. */
    view.origin[0] = 100; view.angles[1] = 90; LQ_ViewMatrix(&view, 1, matrix);
    CHECK(fabsf(matrix[0] * 100 + matrix[4] * 64 + matrix[12]) < 0.0001f);
    LQ_SceneFree(&scene); CHECK(!scene.vertices && !scene.material_count); LQ_SceneFree(&scene);
    puts("PASS renderer scene ownership, invalid geometry rejection and Quake/GPU camera projection"); return 0;
}
