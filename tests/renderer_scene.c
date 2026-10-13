/* Scene validation and Quake-to-GPU projection. GPL-2.0-or-later. */
#include "scene.h"
#include "scene_fixture.h"
#include <stdio.h>
#include <math.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
int main(void)
{
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
