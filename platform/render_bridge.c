/* Classic GPU frame adaptation: CPU simulation/animation, GPU 3D rasterization,
 * and explicit alpha coverage for the original palette-based 2D UI.
 * GPL-2.0-or-later. */
#include "quakedef.h"
#include "d_local.h"
#include "sdl_backend.h"
#include "render_bridge.h"
#include "gpu_sdl.h"
#include "quake_scene.h"
#include "scene_fixture.h"
#include "gpu_entities.h"
#include "effects.h"
#include "fluorescent.h"
#include "jobs.h"
extern cvar_t r_waterwarp;
extern mleaf_t *r_viewleaf;
extern entity_t *currententity;
void R_SetupFrame(void);
void R_MarkLeaves(void);
void R_DrawParticles(void);
void R_StoreEfrags(efrag_t **frags);
static lq_scene map_scene;
static lq_draw *world_draws, *interaction_draws;
/* Luminous (lq_renderer 2, experimental): map lights keep stable slots for
 * cached shadows; gameplay dlights take the last LQ_MAX_DYNAMIC_LIGHTS. */
static qboolean luminous;
static lq_runtime_light runtime_lights[LQ_MAX_RUNTIME_LIGHTS];
/* Spark events live outside Quake's dlight pool so effects cannot evict
 * gameplay lights or change Classic/Software presentation. */
static struct { float origin[3]; double born; int color, active; } spark_lights[16];
static uint32_t next_spark;
void LQ_RenderSpark(const float origin[3], int color)
{
    if (Cvar_VariableValue("lq_renderer") != 2) return;
    uint32_t slot = next_spark++ % 16;
    memcpy(spark_lights[slot].origin, origin, sizeof(vec3_t));
    spark_lights[slot].born = cl.time; spark_lights[slot].color = color & 255;
    spark_lights[slot].active = 1;
}
static uint8_t *dynamic_probes; /* per-frame gameplay-light bounce, GPU layout */
/* Failing tubes sputter with the crackle of the buzz Quake plays for them. */
#define TUBE_SOUND "ambience/buzz1.wav"
static lq_tube_track tube_track;
static double tube_offset; /* track position minus realtime */
static uint32_t tinted_camera;
static uint32_t TubeCamera(void) { return (uint32_t)Cvar_VariableValue("r_fluorescent"); }
/* Follow the buzz while it is mixed; while it is not, run on from where it was.
 * The device takes the mix in chunks, so small errors are eased out and only a
 * jump (the loop restarting, or resuming after silence) is followed at once. */
static const lq_tube_track *TubeTrack(void)
{
    double position; qboolean audible;
    if (!tube_track.count) return NULL;
    if (LQ_StaticSoundClock(TUBE_SOUND, &position, &audible) && audible) {
        double length = tube_track.length, error = fmod(position - realtime - tube_offset, length);
        if (error > length / 2) error -= length;
        if (error < -length / 2) error += length;
        tube_offset += fabs(error) > 0.05 ? error : error * 0.2;
    }
    tube_track.position = realtime + tube_offset;
    return &tube_track;
}
/* Translucent liquids (r_wateralpha) must show what lies across the surface.
 * Without water vis a compiled PVS stops at the leaves touching it, so the
 * frame adds the PVS of every visible leaf on the other side of the surface. */
static byte water_pvs[MAX_MAP_LEAFS/8];
static float WaterAlpha(void)
{
    float alpha = Cvar_VariableValue("r_wateralpha");
    return isfinite(alpha) && alpha > 0 && alpha < 1 ? fmaxf(alpha, 0.1f) : 1;
}
static qboolean Liquid(int contents) { return contents <= CONTENTS_WATER && contents >= CONTENTS_LAVA; }
static byte *ThroughWater(mleaf_t *view_leaf, model_t *world)
{
    size_t bytes = (world->numleafs + 7) / 8;
    memcpy(water_pvs, Mod_LeafPVS(view_leaf, world), bytes);
    qboolean below = Liquid(view_leaf->contents);
    if (!below && view_leaf->contents != CONTENTS_EMPTY) return water_pvs;
    for (int i = 0; i < world->numleafs; ++i) {
        mleaf_t *leaf = &world->leafs[i+1];
        if (!(water_pvs[i>>3] & (1<<(i&7))) || !(below ? leaf->contents == CONTENTS_EMPTY : Liquid(leaf->contents))) continue;
        const byte *across = Mod_LeafPVS(leaf, world);
        for (size_t k = 0; k < bytes; ++k) water_pvs[k] |= across[k];
    }
    return water_pvs;
}
static qboolean direct_frame;
static void CaptureView(void);

static byte *preserve, *rgba;
static size_t count;
static qboolean captured_scene, loaded;
static lq_gpu *owner;
static lq_view view;
static unsigned palette_cache[256];
static qboolean palette_valid;
int LQ_RenderBridgeActive(void) { return owner != NULL; }
void LQ_RenderAfterCommands(void) { LQ_ApplyRenderer(); }

void LQ_RenderBridgeShutdown(void)
{
    LQ_EntitiesReset(); LQ_SceneFree(&map_scene); free(world_draws); world_draws = NULL;
    free(interaction_draws); interaction_draws = NULL; memset(runtime_lights, 0, sizeof(runtime_lights));
    free(dynamic_probes); dynamic_probes = NULL;
    memset(spark_lights, 0, sizeof(spark_lights)); next_spark = 0;
    direct_frame = false;
    free(preserve); free(rgba);
    preserve = rgba = NULL; count = 0;
    owner = NULL; loaded = captured_scene = palette_valid = false;
}
void LQ_RenderMapChanged(void) {
    loaded = false; captured_scene = direct_frame = false;
    memset(spark_lights, 0, sizeof(spark_lights)); next_spark = 0;
    LQ_EntitiesReset(); LQ_SceneFree(&map_scene); free(world_draws); world_draws = NULL;
    free(interaction_draws); interaction_draws = NULL; memset(runtime_lights, 0, sizeof(runtime_lights));
    free(dynamic_probes); dynamic_probes = NULL;
}
void LQ_RenderBegin(void)
{
    lq_scene scene = {0}; char error[256];
    captured_scene = direct_frame = false;
    if (!host_initialized) return;
    lq_gpu *gpu = LQ_PlayableGPU();
    if (!gpu) return;
    if (owner != gpu) {
        owner = gpu; loaded = false; palette_valid = false;
        /* A placeholder permits opaque software menus before any map exists. */
        if (!LQ_SceneFixture(&scene) || !LQ_GPUUpload(gpu, &scene, error, sizeof(error))) {
            LQ_SceneFree(&scene); LQ_FallbackRenderer("GPU placeholder upload failed"); return;
        }
        LQ_SceneFree(&scene);
    }
    size_t needed = (size_t)vid.width * vid.height;
    if (count != needed) {
        free(preserve); free(rgba);
        preserve = calloc(needed, 1); rgba = malloc(needed * 4);
        if (!preserve || !rgba) {
            LQ_FallbackRenderer("GPU compositor allocation failed"); return;
        }
        count = needed;
    }
    memset(preserve, 0, count);
    /* A tube's colour is in its bounce, so a camera recording another colour reloads the map's lighting. */
    if (loaded && luminous && LQ_FluorescentCast(TubeCamera()) != LQ_FluorescentCast(tinted_camera)) LQ_RenderMapChanged();
    if (!loaded && cls.state == ca_connected && cl.worldmodel && !con_forcedup) {
        luminous = Cvar_VariableValue("lq_renderer") == 2;
        if (!LQ_QuakeSceneFromWorld(&map_scene, error, sizeof(error))) { LQ_FallbackRenderer(error); return; }
        if (luminous) {
            /* Before bounce, so each failing tube's bounce follows it. */
            uint32_t tubes = LQ_SceneAssignFluorescentStyles(&map_scene);
            tinted_camera = TubeCamera(); LQ_SceneTintFluorescents(&map_scene, tinted_camera);
            const void *samples; int width, length, rate;
            tube_track.count = 0; tube_offset = 0;
            if (tubes && LQ_SoundSamples(TUBE_SOUND, &samples, &width, &length, &rate))
                LQ_TubeTrackFromSound(&tube_track, samples, width, (uint32_t)length, (uint32_t)rate);
            if (tubes) Con_DPrintf("Luminous fluorescents: %u failing tubes on private styles, %s.\n", tubes,
                tube_track.count ? "sputtering with " TUBE_SOUND : "on their own timelines");
            double started = Sys_FloatTime();
            lq_bounce_tracer tracer = LQ_GPUBounceTracer(gpu);
            if (LQ_SceneComputeBounceTraced(&map_scene, 64, LQ_JobsRunRange, &tracer, error, sizeof(error)))
                Con_Printf("Luminous bounce: %u samples, %u bounces, sky light %.0f %.0f %.0f, in %.2f s (%s rays).\n",
                    map_scene.sample_count, map_scene.bounces,
                    map_scene.sky_light[0], map_scene.sky_light[1], map_scene.sky_light[2], Sys_FloatTime() - started,
                    tracer.passes ? "GPU" : "CPU");
            else Con_Printf("Luminous bounce unavailable: %s\n", error);
        }
        if (!LQ_GPUUpload(gpu, &map_scene, error, sizeof(error))) { LQ_FallbackRenderer(error); return; }
        if (luminous && !LQ_SceneDrawBounds(&map_scene)) { LQ_FallbackRenderer("Cannot allocate draw bounds"); return; }
        world_draws = malloc(map_scene.draw_count*sizeof(*world_draws));
        interaction_draws = luminous ? malloc(map_scene.draw_count*sizeof(*interaction_draws)) : NULL;
        if (!world_draws || (luminous && !interaction_draws)) { LQ_FallbackRenderer("Cannot allocate world visibility"); return; }
        memset(runtime_lights, 0, sizeof(runtime_lights));
        free(dynamic_probes); dynamic_probes = NULL;
        if (luminous && LQ_GPUDynamicProbeBytes(gpu)) dynamic_probes = malloc(LQ_GPUDynamicProbeBytes(gpu));
        loaded = true; palette_valid = false;
        Con_Printf("%s: copied live BSP world; map resources replaced.\n", luminous ? "Luminous (experimental)" : "Classic GPU");
    }
}
int LQ_RenderGPUView(void)
{
    if (!owner || !loaded || Cvar_VariableValue("lcd_x") || Cvar_VariableValue("r_drawflat") ||
        Cvar_VariableValue("r_drawpolys") || Cvar_VariableValue("r_drawculledpolys")) return 0;
    /* Water distortion belongs in the GPU projection; do not shrink the view
     * to the software warp buffer. Simulation and palette contents are retained. */
    float warp = r_waterwarp.value; r_waterwarp.value = 0;
    R_SetupFrame(); r_waterwarp.value = warp; R_MarkLeaves();
    direct_frame = true;
    for (int y = r_refdef.vrect.y; y < r_refdef.vrect.y+r_refdef.vrect.height; ++y)
        memset(vid.buffer+(size_t)y*vid.rowbytes+r_refdef.vrect.x, 0, r_refdef.vrect.width);
    memcpy(world_draws, map_scene.draws, map_scene.draw_count*sizeof(*world_draws));
    for (uint32_t i = 0; i < map_scene.draw_count; ++i) world_draws[i].index_count = 0;
    currententity = &cl_entities[0];
    byte *visible = WaterAlpha() < 1 ? ThroughWater(r_viewleaf, cl.worldmodel) : Mod_LeafPVS(r_viewleaf, cl.worldmodel);
    for (int i = 0; i < cl.worldmodel->numleafs; ++i) if (visible[i>>3] & (1<<(i&7))) {
        mleaf_t *leaf = &cl.worldmodel->leafs[i+1];
        R_StoreEfrags(&leaf->efrags);
        for (int j = 0; j < leaf->nummarksurfaces; ++j) {
            msurface_t *surface = leaf->firstmarksurface[j];
            size_t index = surface-cl.worldmodel->surfaces;
            if (index >= (size_t)cl.worldmodel->firstmodelsurface &&
                index < (size_t)(cl.worldmodel->firstmodelsurface+cl.worldmodel->nummodelsurfaces)) {
                world_draws[index] = map_scene.draws[index];
                world_draws[index].material = LQ_AnimatedMaterial(&map_scene, surface->texinfo->texture);
                world_draws[index].light_bits[0] = surface->dlightframe == r_framecount ? surface->dlightbits : 0;
            }
        }
    }
    LQ_EntitiesBegin(&map_scene); LQ_EntitiesBuild(&map_scene); R_DrawParticles();
    V_SetContentsColor(r_viewleaf->contents);
    CaptureView();
    return 1;
}
int LQ_RenderGPUParticle(const float origin[3], int color, int emissive, int kind, float scale, float erode,
    unsigned int seed)
{
    if (!direct_frame) return 0;
    LQ_EntitiesParticle(origin, color, emissive, kind, scale, erode, seed); return 1;
}
float LQ_RenderFlameGain(const float origin[3])
{
    float gain, offset[3];
    return LQ_FlameLight(origin, &gain, offset) ? gain : 0;
}
void LQ_RenderUIRegion(int x, int y, int width, int height, const byte *pixels, int stride, int transparent)
{
    if (!direct_frame || !preserve || width < 0 || height < 0 ||
        (unsigned)width > vid.width || (unsigned)height > vid.height) return;
    int scale = LQ_UIActive() ? LQ_UIScale() : 1;
    unsigned output_width = vid.width*scale, output_height = vid.height*scale;
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col) {
        int px = (x+col)*scale, py = (y+row)*scale;
        if (px < 0 || py < 0 || (unsigned)(px+scale) > output_width || (unsigned)(py+scale) > output_height ||
            (pixels && pixels[row*stride+col] == transparent)) continue;
        for (int i = 0; i < scale; ++i) memset(preserve+(size_t)(py+i)*output_width+px, 1, scale);
    }
}
int LQ_RenderUIPainted(int x, int y)
{
    if (!direct_frame || !preserve) return 0;
    return x >= 0 && y >= 0 && x < (int)vid.width && y < (int)vid.height &&
        preserve[(size_t)y*vid.width+x];
}
const byte *LQ_RenderUICoverage(void) { return direct_frame ? preserve : NULL; }

typedef struct {
    const byte *source, *coverage;
    byte *output;
    uint32_t source_width, output_width, scale;
    int viewport[4];
    unsigned palette[256];
    qboolean opaque, menu;
} lq_overlay_rows;

static void ConvertOverlayRows(void *opaque_context, size_t begin, size_t end)
{
    lq_overlay_rows *context = opaque_context;
    for (size_t row = begin; row < end; ++row) for (uint32_t col = 0; col < context->output_width; ++col) {
        uint32_t x = col * context->scale, y = (uint32_t)row * context->scale;
        size_t source_pixel = (size_t)y * context->source_width + x;
        size_t output_pixel = row * context->output_width + col;
        unsigned color = context->palette[context->source[source_pixel]];
        byte *rgba_pixel = context->output + output_pixel * 4;
        rgba_pixel[0] = color >> 16; rgba_pixel[1] = color >> 8; rgba_pixel[2] = color;
        qboolean outside = x < (uint32_t)context->viewport[0] || y < (uint32_t)context->viewport[1] ||
            x >= (uint32_t)(context->viewport[0] + context->viewport[2]) ||
            y >= (uint32_t)(context->viewport[1] + context->viewport[3]);
        qboolean painted = context->opaque || outside || context->coverage[source_pixel];
        if (!painted && context->menu) rgba_pixel[0] = rgba_pixel[1] = rgba_pixel[2] = 0;
        rgba_pixel[3] = painted ? 255 : context->menu ? 128 : 0;
    }
}
/* Keep nearby effects within the eight dynamic shadow slots, weighted by range. */
static void SelectEffect(const lq_runtime_light *candidate, lq_runtime_light selected[LQ_MAX_DYNAMIC_LIGHTS],
    float scores[LQ_MAX_DYNAMIC_LIGHTS], uint32_t *count)
{
    vec3_t delta; VectorSubtract(candidate->position_radius, view.origin, delta);
    float score = Length(delta) - candidate->position_radius[3];
    uint32_t at = *count;
    while (at && score < scores[at-1]) --at;
    if (at >= LQ_MAX_DYNAMIC_LIGHTS) return;
    if (*count < LQ_MAX_DYNAMIC_LIGHTS) ++*count;
    memmove(selected+at+1, selected+at, (*count-at-1)*sizeof(*selected));
    memmove(scores+at+1, scores+at, (*count-at-1)*sizeof(*scores));
    selected[at] = *candidate; scores[at] = score;
}
static void LuminousLights(void)
{
    static const float colors[][3] = {
        {1.0f, 0.85f, 0.65f}, /* unclassified legacy light */
        {1.0f, 0.85f, 0.55f}, /* muzzle flash */
        {1.0f, 0.6f, 0.25f},  /* rocket / lava ball */
        {1.0f, 0.7f, 0.35f},  /* explosion */
        {1.0f, 1.0f, 1.0f}    /* powerup glow */
    };
    /* Lights on faces in (or just beside) the view outrank nearer ones off it. */
    float selection[16];
    LQ_LightSelectionClip(&view, view.viewport[3] > 0 ? (float)view.viewport[2] / (float)view.viewport[3] : 0, selection);
    LQ_BuildRuntimeLightsInView(&map_scene, view.lightstyles, view.origin, world_draws, selection, runtime_lights,
        LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS);
    /* Torch and flame lights follow their flame's animation. The position moves
     * only when the pose changes, so the shadow cube is redrawn at that rate. */
    for (uint32_t i = 0; i < LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS; ++i) {
        lq_runtime_light *light = &runtime_lights[i];
        float gain, offset[3];
        if (light->position_radius[3] <= 0 || light->source_index >= map_scene.light_source_count) continue;
        const lq_light_source *source = &map_scene.light_sources[light->source_index];
        /* A torch's accent spotlight takes its flame's strength but stays put. */
        if (source->flame_bound) {
            if (LQ_FlameLight(source->flame_origin, &gain, offset)) light->color_strength[3] *= gain;
            continue;
        }
        if (!LQ_FlameLight(source->origin, &gain, offset)) continue;
        light->color_strength[3] *= gain;
        for (int k = 0; k < 3; ++k) light->position_radius[k] += offset[k];
    }
    /* Fluorescent tubes flicker with the mains as the chosen camera records it,
     * and a failing tube's strike flash throws its light twice as far. */
    uint32_t camera = TubeCamera();
    const lq_tube_track *track = TubeTrack();
    for (uint32_t i = 0; i < LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS; ++i) {
        lq_runtime_light *light = &runtime_lights[i];
        float rgb[3];
        if (light->position_radius[3] <= 0 ||
            !LQ_FluorescentGain(&map_scene, track, light->source_index, cl.time, camera, rgb)) continue;
        for (int k = 0; k < 3; ++k) light->color_strength[k] *= rgb[k];
        LQ_FluorescentReach(&map_scene, track, light);
    }
    lq_runtime_light selected[LQ_MAX_DYNAMIC_LIGHTS], candidate;
    float scores[LQ_MAX_DYNAMIC_LIGHTS]; uint32_t dynamic = 0;
    for (int i = 0; i < MAX_DLIGHTS; ++i) {
        const dlight_t *light = &cl_dlights[i];
        if (light->die < cl.time || light->radius <= 0) continue;
        unsigned kind = (unsigned)light->kind;
        if (kind >= sizeof(colors)/sizeof(colors[0])) kind = LQ_DLIGHT_GENERIC;
        LQ_DynamicRuntimeLight(&candidate, light->origin, light->radius, colors[kind],
            kind == LQ_DLIGHT_EXPLOSION ? 12 : 4, (uint32_t)i);
        SelectEffect(&candidate, selected, scores, &dynamic);
    }
    for (uint32_t i = 0; i < MAX_BEAMS; ++i) {
        const beam_t *beam = &cl_beams[i];
        if (!beam->model || beam->endtime < cl.time ||
            (strcmp(beam->model->name, "progs/bolt.mdl") && strcmp(beam->model->name, "progs/bolt2.mdl") &&
             strcmp(beam->model->name, "progs/bolt3.mdl"))) continue;
        lq_runtime_light samples[LQ_BEAM_LIGHTS];
        uint32_t n = LQ_BeamRuntimeLights(beam->start, beam->end, 0x1000 + i*LQ_BEAM_LIGHTS, samples);
        for (uint32_t j = 0; j < n; ++j) SelectEffect(&samples[j], selected, scores, &dynamic);
    }
    for (uint32_t i = 0; i < 16; ++i) {
        if (!spark_lights[i].active) continue;
        double age = cl.time - spark_lights[i].born;
        if (age < 0 || age >= 0.2) { spark_lights[i].active = 0; continue; }
        unsigned palette = d_8to24table[spark_lights[i].color];
        float color[3] = {(palette >> 16 & 255)/255.0f, (palette >> 8 & 255)/255.0f, (palette & 255)/255.0f};
        LQ_DynamicRuntimeLight(&candidate, spark_lights[i].origin, 96, color, 8, 0x2000 + i);
        candidate.color_strength[3] = (float)(1 - age/0.2);
        SelectEffect(&candidate, selected, scores, &dynamic);
    }
    uint32_t base = LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS;
    for (uint32_t i = 0; i < LQ_MAX_DYNAMIC_LIGHTS; ++i) {
        lq_runtime_light *slot = &runtime_lights[base + i];
        if (i < dynamic) *slot = selected[i];
        else memset(slot, 0, sizeof(*slot));
        slot->submerged = i < dynamic && Liquid(Mod_PointInLeaf(slot->position_radius, cl.worldmodel)->contents);
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < LQ_MAX_RUNTIME_LIGHTS; ++i) if (runtime_lights[i].position_radius[3] > 0) count = i + 1;
    memcpy(view.runtime_lights, runtime_lights, sizeof(runtime_lights));
    view.runtime_light_count = count;
    view.luminous_lighting = 1;
    float faint = Cvar_VariableValue("lq_shadow_faint"), dominance = Cvar_VariableValue("lq_shadow_dominance");
    view.shadow_faint = faint > 0 ? faint : -1;
    view.shadow_dominance = dominance > 0 ? dominance : -1;
    LQ_BuildVisibleLightInteractions(&map_scene, runtime_lights, count, world_draws, interaction_draws);
    for (uint32_t i = 0; i < map_scene.draw_count; ++i)
        if (world_draws[i].index_count)
            memcpy(world_draws[i].light_bits, interaction_draws[i].light_bits, sizeof(world_draws[i].light_bits));
}
static void CaptureView(void)
{
    if (!direct_frame) return;
    memset(&view, 0, sizeof(view));
    memcpy(view.origin, r_refdef.vieworg, sizeof(view.origin));
    memcpy(view.angles, r_refdef.viewangles, sizeof(view.angles));
    view.horizontal_fov = atanf(r_refdef.horizontalFieldOfView * 0.5f) * 360.0f / (float)M_PI;
    view.time = (float)cl.time;
    for (int i = 0; i < 256; ++i) view.lightstyles[i] = d_lightstylevalue[i] / 256.0f;
    if (luminous && direct_frame) LQ_FluorescentStyles(&map_scene, TubeTrack(), cl.time, view.lightstyles);
    view.viewport[0] = r_refdef.vrect.x; view.viewport[1] = r_refdef.vrect.y;
    view.viewport[2] = r_refdef.vrect.width; view.viewport[3] = r_refdef.vrect.height;
    if (direct_frame) { view.world_draws = world_draws; view.world_draw_count = map_scene.draw_count; }
    for (int i = 0; i < MAX_DLIGHTS && i < 32; ++i) if (cl_dlights[i].die >= cl.time && cl_dlights[i].radius > 0) {
        memcpy(view.dynamic_lights[i], cl_dlights[i].origin, sizeof(vec3_t));
        view.dynamic_lights[i][3] = cl_dlights[i].radius;
    }
    view.water_alpha = WaterAlpha();
    view.water_warp = direct_frame && r_waterwarp.value && r_viewleaf->contents <= CONTENTS_WATER;
    if (luminous && direct_frame && interaction_draws) LuminousLights();
    captured_scene = true;
}
qboolean LQ_RenderBridgePresent(void)
{
    char error[256]; lq_gpu *gpu = LQ_PlayableGPU();
    if (!gpu || !owner || !rgba || !count) return false;
    /* Console drawing already marks its own coverage. Making the entire
     * overlay opaque hides the live world beneath a half-height console. */
    qboolean opaque = !captured_scene;
    if (!captured_scene) {
        memset(&view, 0, sizeof(view)); view.horizontal_fov = 90;
        for (int i = 0; i < 256; ++i) view.lightstyles[i] = 1;
    }
    /* UI is drawn in whole-pixel blocks. Upload its logical canvas rather than
     * converting/uploading millions of redundant transparent pixels at 4K. */
    uint32_t scale = direct_frame ? LQ_UIScale() : 1;
    uint32_t overlay_width = (vid.width+scale-1)/scale;
    uint32_t overlay_height = (vid.height+scale-1)/scale;
    lq_overlay_rows conversion = {vid.buffer, preserve, rgba, vid.width, overlay_width, scale,
        {view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3]}, {0}, opaque,
        key_dest == key_menu};
    memcpy(conversion.palette, d_8to24table, sizeof(conversion.palette));
    LQ_JobsRunRange(overlay_height, 16, ConvertOverlayRows, &conversion);
    if (loaded && (!palette_valid || memcmp(palette_cache, d_8to24table, sizeof(palette_cache)))) {
        byte colors[256 * 64 * 4];
        for (int i = 0; i < 256 * 64; ++i) {
            unsigned color = d_8to24table[host_colormap[i]];
            colors[i * 4] = color >> 16; colors[i * 4 + 1] = color >> 8;
            colors[i * 4 + 2] = color; colors[i * 4 + 3] = 255;
        }
        lq_image image = {256, 64, 4, colors};
        if (!LQ_GPUColormap(gpu, &image, error, sizeof(error))) goto failed;
        memcpy(palette_cache, d_8to24table, sizeof(palette_cache)); palette_valid = true;
    }
    lq_image image = {overlay_width, overlay_height, 4, rgba};
    lq_frame empty = {0};
    /* Gameplay lights bounce through the probe grid, around them each frame:
     * traced on the GPU when it can write the probe texture, else here. */
    if (luminous && direct_frame && dynamic_probes) {
        const lq_runtime_light *lights[LQ_MAX_DYNAMIC_LIGHTS]; uint32_t box_min[3], box_size[3];
        uint32_t count = LQ_SceneDynamicProbeLights(&map_scene, view.runtime_lights, view.runtime_light_count,
            lights, box_min, box_size);
        if (!count || !LQ_GPUTraceDynamicProbes(gpu, lights, count, box_min, box_size, 16)) {
            int active = count && LQ_SceneDynamicProbes(&map_scene, view.runtime_lights, view.runtime_light_count, 16,
                LQ_JobsRunRange, dynamic_probes);
            if (!LQ_GPUDynamicProbes(gpu, active ? dynamic_probes : NULL, error, sizeof(error))) goto failed;
        }
    }
    if (!LQ_GPUFrame(gpu, direct_frame ? LQ_EntitiesFrame() : &empty, error, sizeof(error)) ||
        !LQ_GPUOverlay(gpu, &image, error, sizeof(error)) ||
        !LQ_GPURender(gpu, &view, vid.width, vid.height, error, sizeof(error))) goto failed;
    return true;
failed:
    LQ_FallbackRenderer(error); return false;
}
