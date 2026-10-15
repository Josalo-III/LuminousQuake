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
    uint8_t bounce_styles[4]; /* Enhanced bounce layer styles; 255 is unused */
} lq_draw;
/* Renderer-owned descriptions from BSP entity text, retained after QC discards
 * static lights. Intensity is the compiler's light value; with the formula,
 * attenuation and output scale it defines the source's falloff and range.
 * Color is normalized linear input from supported three-component metadata. */
enum { LQ_LIGHT_UNRESOLVED_PARAMETERS = 1 };
/* LIGHT/ericw-tools "delay" formulas. Local minlight is not a direct light. */
enum { LQ_LIGHT_LINEAR, LQ_LIGHT_INVERSE, LQ_LIGHT_INVERSE2, LQ_LIGHT_INFINITE,
       LQ_LIGHT_LOCAL_MINLIGHT, LQ_LIGHT_INVERSE2A, LQ_LIGHT_FORMULA_COUNT };
#define LQ_LIGHT_MAX_RANGE 16384.0f
/* Emitter radius for soft shadows when a map gives none (ericw "_deviance").
 * Quake's flames and fixtures are roughly this size; a point source would
 * cast perfectly hard, geometric shadow edges. */
#define LQ_LIGHT_DEFAULT_RADIUS 8.0f
typedef struct {
    float origin[3], intensity, cone_angle, color[3];
    float attenuation; /* "wait" distance multiplier */
    float angle_scale; /* weight of the incidence term; 0.5 softens it */
    float output_scale; /* compiler range scale applied to the final value */
    float spot_direction[3], spot_outer, spot_inner; /* cosines of half-angles */
    float source_radius; /* emitter size: penumbra width grows with it */
    uint32_t formula, spotlight;
    uint32_t style, spawnflags, entity_index, flags;
    char classname[64], target[64], targetname[64];
    char light_value[128], color_value[128];
} lq_light_source;
/* Compact BSP for point lookup and ray casts. A negative child is leaf
 * -(child+1); faces lying on a node's plane are draws [first_face, +face_count). */
typedef struct { float normal[3], dist; int32_t children[2]; uint32_t first_face, face_count; } lq_bsp_node;
enum { LQ_LEAF_EMPTY, LQ_LEAF_SOLID, LQ_LEAF_SKY, LQ_LEAF_LIQUID };
/* LIGHT's 16-unit sample grid for a lit face, at the same atlas texels as the
 * lightmap. Enhanced uses it for its own bounce light, never the baked values. */
typedef struct {
    float s[4], t[4], mins[2]; /* texture axes/offsets and texturemins */
    float albedo[3]; /* mean texture color at normal brightness, 0-1 */
    uint16_t width, height, atlas_x, atlas_y;
    uint32_t first_sample; /* into sample_positions; width 0 means no grid */
} lq_face_samples;
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
    /* Optional potential light reach, packed as draw-major source bits: the
     * face lies in a leaf the source's PVS contains within its range. This is
     * conservative culling only; shadows decide actual occlusion. */
    uint32_t *light_visibility;
    uint32_t light_visibility_words;
    /* Optional leaf visibility for selecting lights that can affect the view.
     * leaf_pvs has leaf_count rows; light_leaves has one row per source. */
    lq_bsp_node *bsp_nodes;
    uint32_t bsp_node_count, leaf_count, leaf_words;
    uint32_t *leaf_pvs, *light_leaves;
    uint8_t *leaf_contents; /* LQ_LEAF_* per leaf when bsp_nodes exist */
    /* Optional per-draw sample grids (see lq_face_samples). */
    lq_face_samples *face_samples;
    float (*sample_positions)[3];
    uint32_t sample_count, atlas_height;
    float bounce_scale; /* worldspawn _bouncescale; 1 when absent */
    /* Optional Enhanced bounce: four stacked RGBA layers of atlas_height rows,
     * brightness units, selected per draw by bounce_styles. */
    lq_image bounce;
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
/* Each runtime slot owns one shadow cube; slots stay stable across frames so
 * unchanged static shadows need not be redrawn. A zero range is an empty slot. */
#define LQ_MAX_RUNTIME_LIGHTS 32
#define LQ_MAX_DYNAMIC_LIGHTS 8 /* gameplay lights use the last slots */
#define LQ_DYNAMIC_LIGHT_SOURCE 0x80000000u /* source_index tag for gameplay lights */
typedef struct {
    float position_radius[4]; /* world xyz and falloff/shadow range */
    float color_strength[4]; /* linear RGB and live style times output scale */
    float falloff[4]; /* light value, attenuation, angle scale, formula */
    float spot[4]; /* direction xyz and outer cosine; outer < -1 is omnidirectional */
    float spot_inner; /* inner cosine; equal to outer for a hard edge */
    float source_radius; /* emitter radius for penumbrae; zero is a point */
    uint32_t source_index; /* scene light-source index; never sent to the GPU */
} lq_runtime_light;
typedef struct {
    float origin[3], angles[3], horizontal_fov;
    float lightstyles[256], time;
    uint32_t viewport[4]; /* x/y/width/height; zero size uses the whole target */
    const lq_draw *world_draws;
    uint32_t world_draw_count;
    float dynamic_lights[32][4]; /* xyz/radius; Classic point-light contribution */
    lq_runtime_light runtime_lights[LQ_MAX_RUNTIME_LIGHTS];
    uint32_t runtime_light_count;
    uint32_t enhanced_lighting;
    uint32_t disable_shadows; /* Enhanced diagnostic: falloff without occlusion */
    uint32_t disable_bounce; /* Enhanced diagnostic: direct light only */
    float exposure; /* Enhanced brightness scale; zero means 1 */
    float bounce_gain; /* Enhanced artistic bounce multiplier; zero means 1 */
    float water_warp;
} lq_view;
void LQ_LightSourceDefaults(lq_light_source *source);
float LQ_LightSourceIntensity(const lq_light_source *source, const float styles[256]);
float LQ_LightFalloff(const lq_light_source *source, float distance);
float LQ_LightSourceRange(const lq_light_source *source);
uint32_t LQ_SceneLeaf(const lq_scene *scene, const float point[3]);
/* Static-world bounce from the scene's runtime lights: direct light at each
 * sample (BSP-occluded), reflected by its texture color and gathered once by
 * cosine-weighted rays. Kept per light style so live styles still drive it.
 * `run` may execute ranges in parallel; outputs are disjoint per face. */
typedef void (*lq_range_fn)(void *context, size_t begin, size_t end);
typedef void (*lq_range_runner)(size_t count, size_t grain, lq_range_fn function, void *context);
int LQ_SceneComputeBounce(lq_scene *scene, uint32_t rays, lq_range_runner run, char *error, size_t size);
uint32_t LQ_BuildRuntimeLights(const lq_scene *scene, const float styles[256],
    const float camera[3], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS]);
/* As above, but map lights occupy only the first `slots` entries. */
uint32_t LQ_BuildRuntimeLightsLimited(const lq_scene *scene, const float styles[256],
    const float camera[3], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS], uint32_t slots);
/* A gameplay light (rocket, muzzle flash, explosion): Quake's dlight radius as a
 * linear light of that value, unscaled, so its strength matches Classic. */
void LQ_DynamicRuntimeLight(lq_runtime_light *light, const float origin[3], float radius,
    const float color[3], float source_radius, uint32_t key);
uint32_t LQ_BuildLightInteractions(const lq_scene *scene,
    const lq_runtime_light *lights, uint32_t light_count, lq_draw *output);
void LQ_SceneFree(lq_scene *scene);
void LQ_ViewMatrix(const lq_view *view, float aspect, float matrix[16]);
int LQ_SceneCheck(const lq_scene *scene, char *error, size_t error_size);
#endif
