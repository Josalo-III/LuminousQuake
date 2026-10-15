/* LuminousQuake renderer-facing scene data. GPL-2.0-or-later. */
#ifndef LQ_SCENE_H
#define LQ_SCENE_H
#include <stddef.h>
#include <stdint.h>

/* No engine, QuakeC, SDL or graphics API types cross this boundary.
 * All arrays/images are owned by the scene; GPU uploads copy the data they consume. */
typedef struct { float position[3], uv[2], light_uv[2], shade, normal[3]; } lq_vertex;
typedef struct { uint32_t width, height, channels; uint8_t *pixels; } lq_image;
enum { LQ_SURFACE_SKY = 1, LQ_SURFACE_WATER = 2, LQ_SURFACE_MODEL = 4,
       LQ_SURFACE_MASKED = 8, LQ_SURFACE_WEAPON = 16 };
typedef struct { lq_image indices; char name[17]; uint32_t resource_id; } lq_material;
typedef struct {
    uint32_t first_index, index_count, material, flags;
    uint8_t styles[4];
    uint32_t light_bits;
} lq_draw;
/* Renderer-owned descriptions from BSP entity text, retained after QC discards
 * static lights. Intensity is compiler input, not a calibrated runtime radius.
 * Target/cone and extended parameters must be resolved before illumination. */
enum { LQ_LIGHT_UNRESOLVED_PARAMETERS = 1 };
typedef struct {
    float origin[3], intensity, cone_angle;
    uint32_t style, spawnflags, entity_index, flags;
    char classname[64], target[64], targetname[64];
    char light_value[128], color_value[128];
} lq_light_source;
typedef struct {
    lq_vertex *vertices;
    uint32_t *indices;
    lq_draw *draws;
    lq_material *materials;
    uint32_t vertex_count, index_count, draw_count, material_count;
    lq_image colormap, lightmap;
    float spawn_origin[3], spawn_angles[3];
    lq_light_source *light_sources;
    uint32_t light_source_count;
} lq_scene;
/* Per-frame actor geometry is copied at submission. Texture IDs remain stable
 * for one uploaded map; upload/restart invalidates the backend texture cache. */
typedef struct {
    const lq_vertex *vertices;
    const uint32_t *indices;
    const lq_draw *draws;
    const lq_material *materials;
    uint32_t vertex_count, index_count, draw_count, material_count;
} lq_frame;
typedef struct {
    float origin[3], angles[3], horizontal_fov;
    float lightstyles[256], time;
    uint32_t viewport[4]; /* x/y/width/height; zero size uses the whole target */
    const lq_draw *world_draws;
    uint32_t world_draw_count;
    float dynamic_lights[32][4]; /* xyz/radius; Classic point-light contribution */
    float water_warp;
} lq_view;
float LQ_LightSourceIntensity(const lq_light_source *source, const float styles[256]);
void LQ_SceneFree(lq_scene *scene);
void LQ_ViewMatrix(const lq_view *view, float aspect, float matrix[16]);
int LQ_SceneCheck(const lq_scene *scene, char *error, size_t error_size);
#endif
