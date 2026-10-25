/* LuminousQuake renderer-facing scene data. GPL-2.0-or-later. */
#ifndef LQ_SCENE_H
#define LQ_SCENE_H
#include <stddef.h>
#include <stdint.h>

/* No engine, QuakeC, SDL or graphics API types cross this boundary.
 * All arrays/images are owned by the scene; GPU uploads copy the data they consume. */
typedef struct { float position[3], uv[2], light_uv[2], shade, normal[3]; } lq_vertex;
/* An image: RGBA8 (or 8-bit indices) at `pixels`, or, when `format` is
 * LQ_IMAGE_BC7, a BC7 mip chain of `levels` levels, `bytes` long, base level
 * first (texture packs at 1/2 and 1/4 resolution). */
enum { LQ_IMAGE_RAW = 0, LQ_IMAGE_BC7 = 1 };
typedef struct { uint32_t width, height, channels; uint8_t *pixels; uint32_t format, levels; size_t bytes; } lq_image;
enum { LQ_SURFACE_SKY = 1, LQ_SURFACE_WATER = 2, LQ_SURFACE_MODEL = 4,
       LQ_SURFACE_MASKED = 8, LQ_SURFACE_WEAPON = 16,
       LQ_SURFACE_ACTOR = 32, /* model lit by Luminous lights/probes; Classic uses shade */
       LQ_SURFACE_SUBMERGED = 64, /* the face lies inside a liquid (the GPU sets it for world faces) */
       LQ_SURFACE_DIFFUSE_EFFECT = 128, /* small, unoriented lit particles/sprites */
       LQ_SURFACE_NOSHADOW = 256, /* self-lit projectiles must not occlude their own source */
       LQ_SURFACE_SOLID_EFFECT = 512, /* small lit 3D particles: effect colour, oriented normals */
       LQ_SURFACE_ERODE = 1024, /* smoke: noise cuts away the fraction uv.y of the surface */
       LQ_SURFACE_SPRITE = 2048, /* Luminous filters it bilinearly; index 255 becomes partial coverage */
       LQ_SURFACE_TRANSLUCENT = 4096 /* entity alpha (protocols 666/999): blended by lq_draw.alpha, casts no shadow */ };
/* pack_color and pack_normal are optional community-pack images
 * (LQ_QuakeSceneLoadPack), RGBA8: colour with its glow in alpha, and a
 * DarkPlaces _norm map (tangent normal, green up the image, height in alpha). */
/* pack_gain scales the pack colour to the original texture's mean linear
 * brightness (clamped to 0.5-2; 0 when unset), for lq_pack_brightness.
 * pack_height is the normal map's mean height (alpha, 0-1), which parallax
 * holds at the face so the texture does not slide off its geometry. */
typedef struct {
    lq_image indices; char name[17]; uint32_t resource_id; lq_image pack_color, pack_normal; float pack_gain, pack_height;
} lq_material;
typedef struct {
    uint32_t first_index, index_count, material, flags;
    uint8_t styles[4];
    uint32_t light_bits[4]; /* runtime-light slots reaching the face; Classic uses word 0 for dlights */
    uint8_t bounce_styles[4]; /* Luminous bounce layer styles; 255 is unused */
    /* Runtime-light slots whose shadow lookup on this face provably finds no
     * static blocker (LQ_SceneFindShadowFree); world faces only. */
    uint32_t shadow_free_bits[4];
    float alpha; /* LQ_SURFACE_TRANSLUCENT frame draws: the entity's opacity, 0-1 */
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
/* Inverse and inverse-square lights never reach zero. They stop where they
 * fall below this brightness (or a higher _gate), and the shader tapers them
 * to zero over the last LQ_LIGHT_TAPER of that range. Linear lights end
 * exactly, where they fall below half a unit. */
#define LQ_LIGHT_FADE_LEVEL 1.0f
#define LQ_LIGHT_TAPER 0.2f
/* Emitter radius for soft shadows when a map gives none (ericw "_deviance").
 * Quake's flames and fixtures are roughly this size; a point source would
 * cast perfectly hard, geometric shadow edges. */
#define LQ_LIGHT_DEFAULT_RADIUS 8.0f
/* World-space blur either side of a spotlight's cone edge, plus the emitter
 * radius: LIGHT's 16-unit lattice and bilinear filtering softened baked edges
 * by about this much, while per-pixel evaluation would cut them razor sharp. */
#define LQ_SPOT_EDGE_BLUR 16.0f
/* Accent spotlights (ericw unbounded falloffs: delay 1, 2, 3 or 5) mark key
 * items but run far past saturation (light 2500 unattenuated is ~5x full
 * white). Luminous scales their output so they draw the eye in a darker scene
 * without dominating it. Linear spotlights keep their compiler strength. */
#define LQ_ACCENT_SPOT_GAIN 0.15f
/* Rerelease accent spotlights within this distance of a light_torch_* or
 * light_flame_* follow that flame's strength (not its position), and only the
 * central LQ_TORCH_ACCENT_CORE of their lit area stays flat: the rest tapers to
 * the cone edge. */
#define LQ_TORCH_ACCENT_REACH 128.0f
#define LQ_TORCH_ACCENT_CORE 0.2f
/* A torch accent is light on the wall, not a highlight: it adds at most this
 * much brightness (128 is Quake's normal) to any surface. */
#define LQ_TORCH_ACCENT_LIMIT 32.0f
/* Luminous bounce is gathered repeatedly, as ericw-tools' radiosity is: each
 * bounce reflects the previous one's light off the texture color and scales
 * it by bounce_scale, so it fades geometrically. */
#define LQ_BOUNCE_MAX 16u
#define LQ_BOUNCE_CONVERGED 0.01f
/* Per-reflection scale for maps whose bake had no bounce (no worldspawn
 * _bounce or _bouncescale), such as every 1996 map: enough indirect light to
 * carry moving and switched lights without lifting authored darkness. */
#define LQ_BOUNCE_SCALE_UNBAKED 0.25f
/* Luminous sky light: brightness under an open sky per unit of the sky's mean
 * displayed color times 128 (where a white surface matches the sky). The sky
 * was first drawn from colormap row 0, about twice the palette colour; 2
 * keeps the outdoor light level tuned then now that it draws the palette's own. */
#define LQ_SKY_LIGHT_GAIN 2.0f
float LQ_SpotFactor(const float to_light[3], float distance, const float axis[3],
    float outer, float inner, float source_radius);
typedef struct {
    float origin[3], intensity, cone_angle, color[3];
    float attenuation; /* "wait" distance multiplier */
    float angle_scale; /* weight of the incidence term; 0.5 softens it */
    float output_scale; /* compiler range scale applied to the final value */
    float spot_direction[3], spot_outer, spot_inner; /* cosines of half-angles */
    float source_radius; /* emitter size: penumbra width grows with it */
    float accent_gain; /* applied output gain for accent spotlights; 1 otherwise */
    float output_base; /* compiler output scale before any accent gain */
    float gate; /* ericw-tools _gate: brightness below which the light stops; 0 unset */
    float flame_origin[3]; /* the torch or flame an accent spotlight lights; valid when flame_bound */
    uint32_t flame_bound;
    /* An ericw-tools surface-light copy's face plane (normal, distance); a zero
     * normal for every other light. */
    float surface_plane[4];
    /* Panel shadow group (LQ_SceneGroupSurfaceShadows): 1 + the group's first
     * source, 0 for none. Members share one shadow cube drawn from
     * shadow_origin, reaching shadow_reach, with emitter radius shadow_radius. */
    uint32_t shadow_group;
    float shadow_origin[3], shadow_reach, shadow_radius;
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
 * lightmap. Luminous uses it for its own bounce light, never the baked values. */
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
    /* Optional, laid out as light_visibility: the source's shadow lookup on
     * the face finds nothing static (LQ_SceneFindShadowFree). */
    uint32_t *shadow_free;
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
    /* Worldspawn _bouncescale, applied at every reflection; 1 when only _bounce
     * is given, LQ_BOUNCE_SCALE_UNBAKED when neither is. */
    float bounce_scale;
    /* Worldspawn _bounce: ericw-tools' bounce count, at most LQ_BOUNCE_MAX; 0
     * when absent, meaning LQ_BOUNCE_MAX. Bouncing stops sooner once a bounce
     * adds less than LQ_BOUNCE_CONVERGED of the first one's light. */
    uint32_t bounce_limit;
    uint32_t bounces; /* set by LQ_SceneComputeBounce: bounces gathered */
    /* Seconds LQ_SceneComputeBounce spent: sky, sun, direct light and emit,
     * all gathers, the bounce texture, static probes (load-time diagnosis). */
    float bounce_seconds[6];
    /* Staged bounce (LuminousQuake): when set, sky visibility and the gathers
     * run only for faces whose byte is nonzero (those near the start); direct
     * light, sun and probes cover every face. bounce_partial marks a scene
     * whose bounce covers only those faces, until the full bounce replaces
     * it; such a scene is not cached. */
    const uint8_t *bounce_faces;
    uint32_t bounce_partial;
    /* Optional Luminous bounce: four stacked RGBA layers of atlas_height rows,
     * brightness units, selected per draw by bounce_styles. Layer 0 alpha is
     * the sample's sky visibility (cosine-weighted fraction of rays reaching
     * sky, 0-255) whatever its styles; sky light's own bounce is in style 0.
     * With a sun, layer 1 alpha is the sample's sun visibility (0-255). */
    lq_image bounce;
    /* Luminous sky light: brightness a fully open, upward-facing sample
     * receives, from the mean displayed color of the map's sky materials.
     * Set by LQ_SceneComputeBounce; zero for maps without sky. */
    float sky_light[3];
    /* ericw-tools sunlight, from the strongest `_sun` light entity or the
     * worldspawn `_sunlight` keys: a directional light travelling along
     * sun_direction (unit). sun_light is its color times strength in
     * brightness units at normal incidence, zero for no sun; incidence is
     * softened by sun_angle_scale as for point lights. sun_penumbra is the
     * angular width in degrees over which its visibility is sampled. */
    float sun_direction[3], sun_light[3], sun_angle_scale, sun_penumbra;
    /* Optional Luminous irradiance probes: an ambient cube (irradiance toward
     * +X,-X,+Y,-Y,+Z,-Z, brightness units) at each grid point. probe_static is
     * RGBA, x-fastest, depth index (layer*6 + direction)*size[2] + z; each of
     * the four layers belongs to probe_styles[layer] (255 unused). Layer 0
     * alpha is sky visibility toward each direction (0-255), as in bounce;
     * layer 1 alpha is sun visibility at the probe, the same in all six. */
    float probe_origin[3], probe_spacing;
    uint32_t probe_size[3];
    uint8_t probe_styles[4];
    uint8_t *probe_static, *probe_valid;
    float (*draw_bounds)[6]; /* optional per-draw min xyz, max xyz (LQ_SceneDrawBounds) */
    /* Draws of the static world model; the rest are brush models (doors, lifts)
     * at their compiled positions. world_draw_end 0 means every draw is world. */
    uint32_t world_draw_begin, world_draw_end;
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
 * unchanged static shadows need not be redrawn. A zero range is an empty slot.
 * The slot count is bounded by the shader's light uniform block: 80 bytes per
 * light, 10 KB for 128, inside the 16 KB uniform range every Vulkan device
 * guarantees. Per-face masks are LQ_LIGHT_WORDS 32-bit words. */
#define LQ_MAX_RUNTIME_LIGHTS 128
#define LQ_LIGHT_WORDS (LQ_MAX_RUNTIME_LIGHTS / 32)
#define LQ_MAX_DYNAMIC_LIGHTS 8 /* gameplay lights use the last slots */
/* Shadow cubes come in three resolutions, bounded by memory (D32: 6 MB per
 * 512 cube, 174 MB in all). Gameplay lights and the first 8 map slots get
 * 512, the next 32 slots 256, the remaining 80 slots 128. Selection places
 * the most important lights in the sharper resolutions. Each cube is six
 * layers of a 2D array texture; SDL addresses depth-target layers with 8
 * bits, so an array holds at most 42 cubes and the 128 cubes span two. */
#define LQ_SHADOW_ARRAYS 4
#define LQ_SHADOW_ARRAY_SIZES {512, 256, 128, 128}
#define LQ_SHADOW_ARRAY_CUBES {16, 32, 40, 40}
/* Shadow array and cube within it of a runtime slot. */
void LQ_ShadowSlot(uint32_t slot, uint32_t *array, uint32_t *cube);
/* Luminous sums light in Quake's brightness units, as LIGHT did, where 128
 * shows a texture at its palette colour. The total becomes linear light as
 * (brightness / 128)^LQ_LIGHT_GAMMA, so a texel lit to brightness b looks as it
 * does in Classic, while shading, filtering and colour stay linear and 32-bit. */
#define LQ_LIGHT_GAMMA 2.2f
/* Luminous fullbrights (palette 224-255) emit this many times their palette
 * colour in linear light; tone mapping keeps SDR close to the palette and HDR
 * shows them above white. */
#define LQ_EMISSION_GAIN 3.0f
#define LQ_SHADOW_FAINT 4.0f /* brightness units; 128 is normal */
#define LQ_SHADOW_DOMINANCE 0.05f
#define LQ_SHADOW_DETAIL 64.0f /* a light this bright at a point filters its shadow fully */
#define LQ_DYNAMIC_LIGHT_SOURCE 0x80000000u /* source_index tag for gameplay lights */
typedef struct {
    float position_radius[4]; /* world xyz and falloff/shadow range */
    float color_strength[4]; /* linear RGB and live style times output scale */
    float falloff[4]; /* light value, attenuation, angle scale, formula */
    float spot[4]; /* direction xyz and outer cosine; outer < -1 is omnidirectional */
    float spot_inner; /* inner cosine; equal to outer for a hard edge */
    float source_radius; /* emitter radius for penumbrae; zero is a point */
    uint32_t source_index; /* scene light-source index; never sent to the GPU */
    uint8_t submerged; /* origin inside water, slime or lava */
    float limit; /* most brightness this light adds to a surface; 0 none (torch accents) */
    uint8_t offscreen; /* lights nothing in the selection view: its shadow updates wait; never sent to the GPU */
    /* The shadow cube's centre and range, and the emitter radius its filter
     * uses: the light's own unless it shares a panel group's cube. */
    float shadow_origin_range[4], shadow_radius;
    uint32_t shadow_slot; /* 1 + the slot whose cube this light reads; 0: its own */
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
    uint32_t luminous_lighting;
    uint32_t disable_shadows; /* Luminous diagnostic: falloff without occlusion */
    uint32_t disable_culling; /* diagnostic: submit back-facing and off-screen faces too */
    /* Brush models cast from their compiled positions (the viewer); in game
     * they move and cast as frame geometry instead. */
    uint32_t static_brush_shadows;
    uint32_t disable_bounce; /* Luminous diagnostic: direct light only */
    uint32_t disable_shadow_free; /* Luminous diagnostic: look up shadows even where none can fall */
    /* Luminous community texture packs: draw the pack's colour images, and
     * shape direct light by its normal maps, where a material has them. */
    uint32_t pack_textures, pack_bumps;
    uint32_t pack_match; /* scale pack colour to each original texture's brightness (pack_gain) */
    float pack_parallax; /* parallax depth from normal-map height, as a fraction of texture size; 0 off */
    uint32_t pack_relief_shadows; /* the strongest light shadows the height field too (needs parallax) */
    uint32_t pack_detail_cull; /* parallax, relief shadows and bumps fade out where their texels are under a pixel */
    float pack_gloss; /* specular from the pack's gloss maps (carried in its normal maps' blue), as a multiplier; 0 off */
    /* What the game does to its palette, done to pack colours too: Quake's
     * gamma (the brightness slider; zero means 1), then before it the screen
     * tints as one mix, colour * (1 - w) + rgb on 0-1 values. */
    float palette_gamma, palette_shift[4];
    float exposure; /* Luminous brightness scale; zero means 1 */
    float light_gamma; /* Luminous brightness-to-linear exponent; zero means LQ_LIGHT_GAMMA */
    /* Lights cast no shadow below shadow_faint brightness, or below
     * shadow_dominance of the brightest light at a point; zero means the
     * LQ_SHADOW_ defaults, negative turns the test off. */
    float shadow_faint, shadow_dominance;
    /* Brightness from which a light's shadow takes all 16 filter samples
     * (8 from a quarter of it, 4 below); zero means LQ_SHADOW_DETAIL,
     * negative means always 16. */
    float shadow_detail;
    float bounce_gain; /* Luminous artistic bounce multiplier; zero means 1 */
    float water_warp;
    float water_alpha; /* liquid opacity; zero or one draws liquids opaque */
    /* FitzQuake fog: density (0 none; GL_EXP2 at density / 64 per unit), then
     * colour 0-1; sky_fog is how much of it covers the sky (r_skyfog). */
    float fog[4], sky_fog;
} lq_view;
void LQ_LightSourceDefaults(lq_light_source *source);
int LQ_LightIsAccent(const lq_light_source *source);
/* Set every accent spotlight's output to output_base * gain (0 turns them
 * off). Direct light follows at once; bounce needs LQ_SceneComputeBounce. */
uint32_t LQ_SceneSetAccentGain(lq_scene *scene, float gain);
float LQ_LightSourceIntensity(const lq_light_source *source, const float styles[256]);
float LQ_LightFalloff(const lq_light_source *source, float distance);
float LQ_LightSourceRange(const lq_light_source *source);
uint32_t LQ_SceneLeaf(const lq_scene *scene, const float point[3]);
int LQ_SceneLiquid(const lq_scene *scene, const float point[3]);
/* Static-world bounce from the scene's runtime lights: direct light at each
 * sample (BSP-occluded), reflected by its texture color and gathered once by
 * cosine-weighted rays. Kept per light style so live styles still drive it.
 * `run` may execute ranges in parallel; outputs are disjoint per face. */
typedef void (*lq_range_fn)(void *context, size_t begin, size_t end);
typedef void (*lq_range_runner)(size_t count, size_t grain, lq_range_fn function, void *context);
int LQ_SceneComputeBounce(lq_scene *scene, uint32_t rays, lq_range_runner run, char *error, size_t size);
/* Optional ray tracing for bounce's passes that walk the BSP per sample. A
 * callback returning 0 leaves that pass to the CPU.
 * - sky: fraction of `rays` cosine-weighted rays reaching sky.
 * - sun: fraction of rays across the sun's penumbra reaching sky, for samples
 *   on faces turned toward it (0 elsewhere).
 * - direct: light arriving from the scene's unoccluded light sources (falloff,
 *   incidence and cone, times light color; not yet the texture's).
 * - gather: summed exitance of the faces `rays` cosine-weighted rays hit
 *   (bilinear from `emit`, before scaling).
 * Visibilities are 0 for buried samples. Entries list up to LQ_BOUNCE_ENTRIES
 * styles per sample in the order first met, then entries of style 255. */
#define LQ_BOUNCE_ENTRIES 16
typedef struct { uint32_t style; float rgb[3]; } lq_bounce_entry;
typedef struct {
    void *context;
    int (*sky)(void *context, const lq_scene *scene, uint32_t rays, float *visible);
    int (*sun)(void *context, const lq_scene *scene, float *visible);
    int (*direct)(void *context, const lq_scene *scene, lq_bounce_entry *entries);
    int (*gather)(void *context, const lq_scene *scene, uint32_t rays, const float (*emit)[4][3],
        const uint8_t (*emit_styles)[4], lq_bounce_entry *entries);
    uint32_t passes; /* set by LQ_SceneComputeBounceTraced: passes the tracer completed */
    const volatile int *cancel; /* nonzero stops the computation, which then fails (background use) */
} lq_bounce_tracer;
int LQ_SceneComputeBounceTraced(lq_scene *scene, uint32_t rays, lq_range_runner run,
    lq_bounce_tracer *tracer, char *error, size_t size);
/* Per-frame bounce from gameplay lights into the probe grid, near those lights
 * only: rays from each probe find surfaces they light (BSP-occluded) and gather
 * the reflected light. `output` holds size[0]*size[1]*size[2]*6 RGBA texels,
 * depth index direction*size[2] + z. Returns 1 when any gameplay light wrote. */
/* The gameplay lights (at most LQ_MAX_DYNAMIC_LIGHTS) whose probes need
 * tracing this frame, and the box of probes within their reach. Returns 0
 * when no gameplay light reaches the grid. */
uint32_t LQ_SceneDynamicProbeLights(const lq_scene *scene, const lq_runtime_light *lights, uint32_t count,
    const lq_runtime_light *selected[LQ_MAX_DYNAMIC_LIGHTS], uint32_t box_min[3], uint32_t box_size[3]);
int LQ_SceneDynamicProbes(const lq_scene *scene, const lq_runtime_light *lights, uint32_t count,
    uint32_t rays, lq_range_runner run, uint8_t *output);
uint32_t LQ_BuildRuntimeLights(const lq_scene *scene, const float styles[256],
    const float camera[3], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS]);
/* As above, but map lights occupy only the first `slots` entries. */
uint32_t LQ_BuildRuntimeLightsLimited(const lq_scene *scene, const float styles[256],
    const float camera[3], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS], uint32_t slots);
/* As above. When more sources qualify than there are slots, those that cannot
 * light any draw with nonzero index_count in `visible` are left out first. */
uint32_t LQ_BuildRuntimeLightsVisible(const lq_scene *scene, const float styles[256],
    const float camera[3], const lq_draw *visible, lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS], uint32_t slots);
/* As above, and with a `clip` matrix (NULL for none) the sources that light a
 * visible draw on screen rank ahead of those lighting only draws off it, so a
 * light on what the player sees is never displaced by a nearer one behind. */
uint32_t LQ_BuildRuntimeLightsInView(const lq_scene *scene, const float styles[256], const float camera[3],
    const lq_draw *visible, const float clip[16], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS], uint32_t slots);
/* Whether a box (min xyz, max xyz) can meet a clip matrix's view volume. */
int LQ_BoundsInClip(const float clip[16], const float bounds[6]);
/* The view volume used to rank lights: the view widened by
 * LQ_SELECTION_FOV_MARGIN degrees, so lights just off the edge of the screen
 * already hold slots (and drawn shadow cubes) as the player turns. */
#define LQ_SELECTION_FOV_MARGIN 30.0f
/* A gameplay light (rocket, muzzle flash, explosion): Quake's dlight radius as a
 * linear light of that value, unscaled, so its strength matches Classic. */
void LQ_DynamicRuntimeLight(lq_runtime_light *light, const float origin[3], float radius,
    const float color[3], float source_radius, uint32_t key);
uint32_t LQ_BuildLightInteractions(const lq_scene *scene,
    const lq_runtime_light *lights, uint32_t light_count, lq_draw *output);
/* As above, for draws with nonzero index_count in `visible` only (others get
 * no bits); `visible` may be NULL for all draws. */
int LQ_SpotReachesBox(const float origin[3], const float axis[3], float outer, float blur, const float bounds[6]);
uint32_t LQ_BuildVisibleLightInteractions(const lq_scene *scene, const lq_runtime_light *lights,
    uint32_t light_count, const lq_draw *visible, lq_draw *output);
/* Light-face pairs whose shadow lookup provably finds nothing: no static
 * world geometry or brush model lies between the light and the face,
 * including the margin the soft-shadow filter samples around each point.
 * Static map lights on world faces only; moving actors are not considered.
 * Reads the scene only. Returns 0 if cancelled or the scene lacks a BSP. */
typedef struct { uint64_t pairs, clear; double area, clear_area; } lq_shadow_free_count;
int LQ_SceneCountShadowFree(const lq_scene *scene, const volatile int *cancel, lq_shadow_free_count *out);
/* As above, and with `bits` (draw_count * light_visibility_words words, laid
 * out as light_visibility) set the clear pairs' bits. Runtime interactions
 * then mark those slots in each draw's shadow_free_bits while the light is at
 * its source's origin. Moving casters are the renderer's to rule out. */
int LQ_SceneFindShadowFree(const lq_scene *scene, const volatile int *cancel, lq_shadow_free_count *out,
    uint32_t *bits);
/* ericw-tools surface-light placement: split a convex polygon on axis planes
 * at multiples of `subdivide`, as GLQuake subdivides warped faces, and write
 * each piece's vertex centroid. Returns the number of pieces (only the first
 * `capacity` are written). */
uint32_t LQ_SurfaceLightPoints(const float (*polygon)[3], uint32_t count, float subdivide,
    float (*out)[3], uint32_t capacity);
/* Panel shadows: neighbouring copies of one surface-light template on one
 * plane share a shadow cube instead of each drawing and filtering their own.
 * Copies are gathered nearest-first while every member lies within
 * `max_extent` of the group's centre, the centre sees every member (BSP), and
 * the group has at most `max_members`. Spotlight copies stay alone. The shared
 * emitter radius is the extent plus `piece_radius`, half a piece's size.
 * Needs the scene's BSP. Returns the number of groups of two or more. */
#define LQ_PANEL_SHADOW_EXTENT 192.0f
#define LQ_PANEL_SHADOW_MEMBERS 4u
uint32_t LQ_SceneGroupSurfaceShadows(lq_scene *scene, float max_extent, uint32_t max_members, float piece_radius);
/* Point grouped lights at one cube per group: the lowest slot of a group
 * holding any member draws the cube from the group's centre, the others read
 * it. With `enabled` 0 every light keeps its own cube. */
void LQ_ShareGroupShadows(const lq_scene *scene, lq_runtime_light *lights, uint32_t count, int enabled);
/* Light budget diagnostic. A map light reaches the view when it could light a
 * visible draw (`visible` nonzero per draw, NULL for all): in its PVS reach and
 * range, with the draw in front of it. `peak` bounds what it adds there in
 * brightness units: falloff at the draw's nearest point, before incidence,
 * spot cone and shadows. Reaching lights a fresh `slots`-light selection at
 * `camera` leaves out are written to `dropped`, strongest first, up to
 * `capacity`; *dropped_count gets all of them. Returns the number reaching. */
typedef struct { uint32_t source; float peak; } lq_light_drop;
uint32_t LQ_LightBudget(const lq_scene *scene, const float styles[256], const float camera[3],
    const uint8_t *visible, uint32_t slots, lq_light_drop *dropped, uint32_t capacity,
    uint32_t *dropped_count);
/* As above, selecting as the game does with `select_clip` (see
 * LQ_BuildRuntimeLightsInView), and counting only draws that also meet
 * `count_clip` when it is given. */
uint32_t LQ_LightBudgetInView(const lq_scene *scene, const float styles[256], const float camera[3],
    const uint8_t *visible, const float select_clip[16], const float count_clip[16], uint32_t slots,
    lq_light_drop *dropped, uint32_t capacity, uint32_t *dropped_count);
/* Cache per-draw bounds for per-frame interaction building. */
int LQ_SceneDrawBounds(lq_scene *scene);
void LQ_SceneFree(lq_scene *scene);
void LQ_ViewMatrix(const lq_view *view, float aspect, float matrix[16]);
void LQ_LightSelectionClip(const lq_view *view, float aspect, float matrix[16]);
int LQ_SceneCheck(const lq_scene *scene, char *error, size_t error_size);
#endif
