/* LuminousQuake renderer-facing scene data. GPL-2.0-or-later. */
#ifndef LQ_SCENE_H
#define LQ_SCENE_H
#include <stddef.h>
#include <stdint.h>

/* No engine, QuakeC, SDL or graphics API types cross this boundary.
 * All arrays/images are owned by the scene; GPU uploads copy their contents. */
typedef struct { float position[3], uv[2], light_uv[2]; } lq_vertex;
typedef struct { uint32_t width, height, channels; uint8_t *pixels; } lq_image;
enum { LQ_SURFACE_SKY = 1, LQ_SURFACE_WATER = 2 };
typedef struct { lq_image indices; char name[17]; } lq_material;
typedef struct {
    uint32_t first_index, index_count, material, flags;
    uint8_t styles[4];
} lq_draw;
typedef struct {
    lq_vertex *vertices;
    uint32_t *indices;
    lq_draw *draws;
    lq_material *materials;
    uint32_t vertex_count, index_count, draw_count, material_count;
    lq_image colormap, lightmap;
    float spawn_origin[3], spawn_angles[3];
} lq_scene;
typedef struct {
    float origin[3], angles[3], horizontal_fov;
    float lightstyles[256], time;
    uint32_t viewport[4]; /* x/y/width/height; zero size uses the whole target */
} lq_view;
void LQ_SceneFree(lq_scene *scene);
void LQ_ViewMatrix(const lq_view *view, float aspect, float matrix[16]);
int LQ_SceneCheck(const lq_scene *scene, char *error, size_t error_size);
#endif
