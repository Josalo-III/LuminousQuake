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
#include "background.h"
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
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
/* Recent maps stay loaded (AGENDA item 6): the built scenes of the last
 * SCENE_CACHE_ENTRIES maps are kept, so restarting a map after a death, or
 * returning to one, skips the BSP copy and Luminous's bounce (27 s on
 * ad_tfuma). A scene is keyed by the game folder, the map's name, a hash of
 * its geometry, entities and lightmaps, the renderer and the fluorescent
 * camera, so an edited map or a changed setting builds afresh. The cache
 * holds at most lq_scene_cache_mb; 0 means the agenda's default share, a
 * quarter of the RAM free when it is first needed (half of free RAM is
 * "available", split evenly with lighting), and -1 turns it off. */
#define SCENE_CACHE_ENTRIES 2
static cvar_t lq_scene_cache_mb = {"lq_scene_cache_mb", "0", true};
/* Diagnostics for measurements: 0 submits every PVS face (lq_cull), or
 * lights Luminous without shadow cubes or filtering (lq_shadows). */
static cvar_t lq_cull = {"lq_cull", "1"}, lq_shadows = {"lq_shadows", "1"};
/* Luminous panel lights: neighbouring ericw-tools surface-light pieces share
 * one shadow cube, drawn from their centre with the panel's size as the
 * emitter (1), or each piece casts its own (0). Lighting is the same either way. */
static cvar_t lq_panel_shadows = {"lq_panel_shadows", "1"};
/* Luminous skips shadow lookups proved to find no blocker (1), or looks them
 * all up (0, for comparisons). */
static cvar_t lq_shadow_free = {"lq_shadow_free", "1"};
/* Luminous tone mapping and emission, for tuning by eye (see LQ_GPUTone). */
static cvar_t lq_tone_shoulder = {"lq_tone_shoulder", "0.8"}, lq_tone_white = {"lq_tone_white", "6"};
static cvar_t lq_emission = {"lq_emission", "3"}, lq_sprite_emission = {"lq_sprite_emission", "3"};
static struct { uint64_t key; lq_scene scene; uint32_t tubes; size_t bytes; double used; } scene_cache[SCENE_CACHE_ENTRIES];
static uint64_t map_key; /* the loaded map_scene's key, 0 when none */
static qboolean list_lights;
static void ListLights_f(void) { list_lights = true; }
/* developer readout: one frame sampled every READOUT_SECONDS, nothing gathered in between */
#define READOUT_SECONDS 1.0
static double readout_since;
static uint32_t probe_lights; static double probe_volume;
static uint32_t map_tubes;
static uint64_t Hash(uint64_t h, const void *data, size_t size)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static uint64_t SceneKey(void)
{
    model_t *m = cl.worldmodel;
    uint64_t h = 14695981039346656037ull, settings[2] = {luminous, luminous ? LQ_FluorescentCast(TubeCamera()) : 0};
    if (!m) return 0;
    h = Hash(h, com_gamedir, strlen(com_gamedir) + 1); h = Hash(h, m->name, strlen(m->name) + 1);
    h = Hash(h, settings, sizeof(settings));
    if (m->vertexes) h = Hash(h, m->vertexes, (size_t)m->numvertexes * sizeof(*m->vertexes));
    if (m->entities) h = Hash(h, m->entities, strlen(m->entities));
    for (int i = 0; i < m->numsurfaces; ++i) { /* each face's lightmap, as baked */
        msurface_t *f = &m->surfaces[i];
        if (!f->samples) continue;
        int size = ((f->extents[0] >> 4) + 1) * ((f->extents[1] >> 4) + 1), styles = 0;
        while (styles < MAXLIGHTMAPS && f->styles[styles] != 255) ++styles;
        h = Hash(h, f->samples, (size_t)size * styles);
    }
    return h ? h : 1;
}
static size_t ImageBytes(const lq_image *image)
{ return image->pixels ? (size_t)image->width * image->height * (image->channels ? image->channels : 1) : 0; }
static size_t SceneBytes(const lq_scene *s)
{
    size_t bytes = (size_t)s->vertex_count * sizeof(*s->vertices) + (size_t)s->index_count * sizeof(*s->indices) +
        (size_t)s->draw_count * (sizeof(*s->draws) + (s->draw_bounds ? sizeof(*s->draw_bounds) : 0) +
            (s->face_samples ? sizeof(*s->face_samples) : 0) + (size_t)s->light_visibility_words * 4 * (s->shadow_free ? 2 : 1)) +
        (size_t)s->material_count * sizeof(*s->materials) + ImageBytes(&s->colormap) + ImageBytes(&s->lightmap) +
        ImageBytes(&s->bounce) + (size_t)s->light_source_count * (sizeof(*s->light_sources) + (size_t)s->leaf_words * 4) +
        (size_t)s->bsp_node_count * sizeof(*s->bsp_nodes) + (size_t)s->leaf_count * ((size_t)s->leaf_words * 4 + 1) +
        (size_t)s->sample_count * sizeof(*s->sample_positions);
    size_t probes = (size_t)s->probe_size[0] * s->probe_size[1] * s->probe_size[2];
    if (s->probe_static) bytes += probes * 4 * 6 * 4;
    if (s->probe_valid) bytes += probes;
    for (uint32_t i = 0; i < s->material_count; ++i) bytes += ImageBytes(&s->materials[i].indices);
    return bytes;
}
static size_t FreeRAM(void)
{
#ifdef __APPLE__
    vm_statistics64_data_t vm; mach_msg_type_number_t n = HOST_VM_INFO64_COUNT; vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS ||
        host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &n) != KERN_SUCCESS) return 0;
    /* macOS reclaims inactive and purgeable pages on demand. */
    return (size_t)(vm.free_count + vm.inactive_count + vm.purgeable_count) * page;
#elif defined(_WIN32)
    MEMORYSTATUSEX status = {sizeof(status)};
    return GlobalMemoryStatusEx(&status) ? (size_t)status.ullAvailPhys : 0;
#else
    long pages = sysconf(_SC_AVPHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? (size_t)pages * (size_t)page : 0;
#endif
}
static size_t SceneCacheBudget(void)
{
    static size_t automatic;
    if (lq_scene_cache_mb.value < 0) return 0;
    if (lq_scene_cache_mb.value > 0) return (size_t)lq_scene_cache_mb.value << 20;
    if (!automatic) { automatic = FreeRAM() / 4; if (!automatic) automatic = (size_t)512 << 20; }
    return automatic;
}
static void SceneCacheFreeAll(void)
{
    for (int i = 0; i < SCENE_CACHE_ENTRIES; ++i) { LQ_SceneFree(&scene_cache[i].scene); scene_cache[i].key = 0; }
}
/* Keep map_scene (moved, not copied) if it fits; otherwise free it. */
static void SceneCacheStore(void)
{
    size_t bytes = SceneBytes(&map_scene), budget = SceneCacheBudget(), held = 0;
    if (!map_key || !map_scene.draw_count || bytes > budget) { LQ_SceneFree(&map_scene); map_key = 0; return; }
    int slot = 0;
    for (int i = 0; i < SCENE_CACHE_ENTRIES; ++i) if (scene_cache[i].key == map_key) { LQ_SceneFree(&scene_cache[i].scene); scene_cache[i].key = 0; }
    for (int i = 0; i < SCENE_CACHE_ENTRIES; ++i) {
        if (!scene_cache[i].key) { slot = i; break; }
        if (scene_cache[i].used < scene_cache[slot].used) slot = i;
    }
    LQ_SceneFree(&scene_cache[slot].scene); scene_cache[slot].key = 0;
    for (;;) { /* oldest out until the newcomer fits */
        int oldest = -1; held = 0;
        for (int i = 0; i < SCENE_CACHE_ENTRIES; ++i) if (scene_cache[i].key) {
            held += scene_cache[i].bytes;
            if (oldest < 0 || scene_cache[i].used < scene_cache[oldest].used) oldest = i;
        }
        if (held + bytes <= budget || oldest < 0) break;
        LQ_SceneFree(&scene_cache[oldest].scene); scene_cache[oldest].key = 0;
    }
    scene_cache[slot].scene = map_scene; scene_cache[slot].key = map_key; scene_cache[slot].tubes = map_tubes;
    scene_cache[slot].bytes = bytes; scene_cache[slot].used = realtime;
    memset(&map_scene, 0, sizeof(map_scene)); map_key = 0;
}
void LQ_RenderBridgeRegister(void)
{
    Cvar_RegisterVariable(&lq_scene_cache_mb); Cvar_RegisterVariable(&lq_cull); Cvar_RegisterVariable(&lq_shadows);
    Cvar_RegisterVariable(&lq_panel_shadows); Cvar_RegisterVariable(&lq_shadow_free);
    Cvar_RegisterVariable(&lq_tone_shoulder); Cvar_RegisterVariable(&lq_tone_white);
    Cvar_RegisterVariable(&lq_emission); Cvar_RegisterVariable(&lq_sprite_emission);
    Cmd_AddCommand("lq_lights", ListLights_f);
}
static qboolean SceneCacheTake(uint64_t key)
{
    for (int i = 0; i < SCENE_CACHE_ENTRIES; ++i) if (key && scene_cache[i].key == key) {
        map_scene = scene_cache[i].scene; map_tubes = scene_cache[i].tubes;
        memset(&scene_cache[i].scene, 0, sizeof(scene_cache[i].scene)); scene_cache[i].key = 0;
        return true;
    }
    return false;
}
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
void LQ_RenderAfterCommands(void) { LQ_BackgroundPoll(); LQ_ApplyRenderer(); }

/* Background work on the loaded map's scene is tagged with scene_tasks and
 * cancelled before that scene is moved to the cache or freed. */
static uint32_t scene_tasks;
static void CancelSceneTasks(void) { if (scene_tasks) LQ_BackgroundCancel(scene_tasks); }
typedef struct {
    lq_scene scene; /* a shallow copy: its arrays stay alive until cancelled */
    lq_shadow_free_count count;
    uint32_t *bits; /* the clear pairs, handed to map_scene when done */
    double seconds;
    int done;
} shadow_free_task;
static void ShadowFreeRun(void *context, const volatile int *cancel)
{
    shadow_free_task *task = context;
    Uint64 started = SDL_GetTicksNS();
    size_t words = (size_t)task->scene.draw_count * task->scene.light_visibility_words;
    task->bits = words ? malloc(words * sizeof(*task->bits)) : NULL;
    task->done = task->bits && LQ_SceneFindShadowFree(&task->scene, cancel, &task->count, task->bits);
    task->seconds = (double)(SDL_GetTicksNS() - started) / 1e9;
}
/* Runs on the main thread, before the scene the task read can be cached or
 * freed (scene tasks are cancelled first), so map_scene is still that scene. */
static void ShadowFreeFinish(void *context, int cancelled)
{
    shadow_free_task *task = context;
    if (!cancelled && task->done && task->count.pairs)
        Con_Printf("Luminous: %llu of %llu light-face pairs (%.0f%%; %.0f%% of their face area) need no shadow lookup, "
            "proved in %.1f s in the background; their lookups are skipped.\n", (unsigned long long)task->count.clear,
            (unsigned long long)task->count.pairs, 100.0 * task->count.clear / task->count.pairs,
            task->count.area > 0 ? 100.0 * task->count.clear_area / task->count.area : 0, task->seconds);
    if (!cancelled && task->done && !map_scene.shadow_free) { map_scene.shadow_free = task->bits; task->bits = NULL; }
    free(task->bits); free(task);
}

void LQ_RenderBridgeShutdown(void)
{
    CancelSceneTasks();
    LQ_EntitiesReset(); LQ_SceneFree(&map_scene); map_key = 0; SceneCacheFreeAll(); free(world_draws); world_draws = NULL;
    free(interaction_draws); interaction_draws = NULL; memset(runtime_lights, 0, sizeof(runtime_lights));
    free(dynamic_probes); dynamic_probes = NULL;
    memset(spark_lights, 0, sizeof(spark_lights)); next_spark = 0;
    direct_frame = false;
    free(preserve); free(rgba);
    preserve = rgba = NULL; count = 0;
    owner = NULL; loaded = captured_scene = palette_valid = false;
}
void LQ_RenderMapChanged(void) {
    CancelSceneTasks();
    loaded = false; captured_scene = direct_frame = false;
    memset(spark_lights, 0, sizeof(spark_lights)); next_spark = 0;
    LQ_EntitiesReset(); SceneCacheStore(); free(world_draws); world_draws = NULL;
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
        uint64_t key = SceneKey();
        qboolean cached = SceneCacheTake(key);
        if (cached) Con_Printf("%s: reused the scene built when this map was last loaded.\n",
            luminous ? "Luminous" : "Classic GPU");
        else if (!LQ_QuakeSceneFromWorld(&map_scene, error, sizeof(error))) { LQ_FallbackRenderer(error); return; }
        map_key = key;
        if (luminous) {
            /* Before bounce, so each failing tube's bounce follows it. */
            if (!cached) {
                map_tubes = LQ_SceneAssignFluorescentStyles(&map_scene);
                LQ_SceneTintFluorescents(&map_scene, TubeCamera());
            }
            uint32_t tubes = map_tubes;
            tinted_camera = TubeCamera();
            const void *samples; int width, length, rate;
            tube_track.count = 0; tube_offset = 0;
            if (tubes && LQ_SoundSamples(TUBE_SOUND, &samples, &width, &length, &rate))
                LQ_TubeTrackFromSound(&tube_track, samples, width, (uint32_t)length, (uint32_t)rate);
            if (tubes) Con_DPrintf("Luminous fluorescents: %u failing tubes on private styles, %s.\n", tubes,
                tube_track.count ? "sputtering with " TUBE_SOUND : "on their own timelines");
        }
        if (luminous && !cached) {
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
        if (luminous && !map_scene.draw_bounds && !LQ_SceneDrawBounds(&map_scene)) { LQ_FallbackRenderer("Cannot allocate draw bounds"); return; }
        world_draws = malloc(map_scene.draw_count*sizeof(*world_draws));
        interaction_draws = luminous ? malloc(map_scene.draw_count*sizeof(*interaction_draws)) : NULL;
        if (!world_draws || (luminous && !interaction_draws)) { LQ_FallbackRenderer("Cannot allocate world visibility"); return; }
        memset(runtime_lights, 0, sizeof(runtime_lights));
        free(dynamic_probes); dynamic_probes = NULL;
        if (luminous && LQ_GPUDynamicProbeBytes(gpu)) dynamic_probes = malloc(LQ_GPUDynamicProbeBytes(gpu));
        loaded = true; palette_valid = false;
        if (luminous && !map_scene.shadow_free) {
            shadow_free_task *task = calloc(1, sizeof(*task));
            if (!++scene_tasks) scene_tasks = 1;
            if (task) { task->scene = map_scene; LQ_BackgroundSubmit(scene_tasks, ShadowFreeRun, ShadowFreeFinish, task); }
        }
        if (!cached) Con_Printf("%s: copied live BSP world; map resources replaced.\n", luminous ? "Luminous (experimental)" : "Classic GPU");
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
#define LQ_EXPLOSION_FLASH 1.5f /* explosion light: output and reach over Quake's dlight */
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
    /* Neighbouring pieces of one lit panel share a shadow cube (lq_panel_shadows). */
    LQ_ShareGroupShadows(&map_scene, runtime_lights, LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS,
        lq_panel_shadows.value != 0);
    lq_runtime_light selected[LQ_MAX_DYNAMIC_LIGHTS], candidate;
    float scores[LQ_MAX_DYNAMIC_LIGHTS]; uint32_t dynamic = 0;
    for (int i = 0; i < MAX_DLIGHTS; ++i) {
        const dlight_t *light = &cl_dlights[i];
        if (light->die < cl.time || light->radius <= 0) continue;
        unsigned kind = (unsigned)light->kind;
        if (kind >= sizeof(colors)/sizeof(colors[0])) kind = LQ_DLIGHT_GENERIC;
        LQ_DynamicRuntimeLight(&candidate, light->origin, light->radius, colors[kind],
            kind == LQ_DLIGHT_EXPLOSION ? 12 : 4, (uint32_t)i);
        /* An explosion flashes as a striking tube does, but half as hard:
         * 1.5 times as bright and as far (2 was too much in play). */
        if (kind == LQ_DLIGHT_EXPLOSION) LQ_FlashRuntimeLight(&candidate, LQ_EXPLOSION_FLASH);
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
    float detail = Cvar_VariableValue("lq_shadow_detail");
    view.shadow_detail = detail > 0 ? detail : -1;
    view.disable_shadow_free = lq_shadow_free.value == 0;
    LQ_BuildVisibleLightInteractions(&map_scene, runtime_lights, count, world_draws, interaction_draws);
    for (uint32_t i = 0; i < map_scene.draw_count; ++i)
        if (world_draws[i].index_count)
            memcpy(world_draws[i].light_bits, interaction_draws[i].light_bits, sizeof(world_draws[i].light_bits));
    if (list_lights) { /* lq_lights: what each kept light touches */
        float exact[16]; LQ_ViewMatrix(&view, view.viewport[3] ? (float)view.viewport[2] / view.viewport[3] : 4.0f / 3, exact);
        uint32_t touched[LQ_MAX_RUNTIME_LIGHTS] = {0}, shown[LQ_MAX_RUNTIME_LIGHTS] = {0}, surfaces = 0, total = 0, most = 0;
        for (uint32_t i = 0; i < map_scene.draw_count; ++i) {
            if (!world_draws[i].index_count) continue;
            int on = map_scene.draw_bounds && LQ_BoundsInClip(exact, map_scene.draw_bounds[i]);
            uint32_t n = 0;
            for (uint32_t l = 0; l < LQ_MAX_RUNTIME_LIGHTS; ++l)
                if (world_draws[i].light_bits[l >> 5] & (1u << (l & 31))) { ++touched[l]; shown[l] += on; ++n; }
            if (on) { ++surfaces; total += n; if (n > most) most = n; }
        }
        Con_Printf("lq_lights: %u kept; on-screen surfaces %u, %.1f lights each on average, %u at most\n",
            count, surfaces, surfaces ? (double)total / surfaces : 0, most);
        for (uint32_t l = 0; l < count; ++l) {
            const lq_runtime_light *r = &runtime_lights[l];
            if (r->position_radius[3] <= 0) continue;
            float d[3] = {r->position_radius[0] - view.origin[0], r->position_radius[1] - view.origin[1], r->position_radius[2] - view.origin[2]};
            Con_Printf("  slot %3u entity %4u at %6.0f %6.0f %6.0f reach %5.0f, %5.0f away, strength %.2f: %u PVS surfaces, %u on screen\n",
                l, r->source_index < map_scene.light_source_count ? map_scene.light_sources[r->source_index].entity_index : 0,
                r->position_radius[0], r->position_radius[1], r->position_radius[2], r->position_radius[3],
                sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]), r->color_strength[3], touched[l], shown[l]);
        }
        list_lights = false;
    }
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
    view.disable_culling = !lq_cull.value; view.disable_shadows = !lq_shadows.value;
    view.water_warp = direct_frame && r_waterwarp.value && r_viewleaf->contents <= CONTENTS_WATER;
    if (luminous && direct_frame && interaction_draws) LuminousLights();
    captured_scene = true;
}
qboolean LQ_RenderBridgePresent(void)
{
    char error[256]; lq_gpu *gpu = LQ_PlayableGPU();
    if (!gpu || !owner || !rgba || !count) return false;
    qboolean sample = developer.value && luminous && direct_frame && realtime - readout_since >= READOUT_SECONDS;
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
        if (sample) { probe_lights = count; probe_volume = count ? (double)box_size[0] * box_size[1] * box_size[2] : 0; }
        if (!count || !LQ_GPUTraceDynamicProbes(gpu, lights, count, box_min, box_size, 16)) {
            int active = count && LQ_SceneDynamicProbes(&map_scene, view.runtime_lights, view.runtime_light_count, 16,
                LQ_JobsRunRange, dynamic_probes);
            if (!LQ_GPUDynamicProbes(gpu, active ? dynamic_probes : NULL, error, sizeof(error))) goto failed;
        }
    }
    LQ_GPUDisplay(gpu, Cvar_VariableValue("lq_gamma"), Cvar_VariableValue("lq_hdr") != 0,
        Cvar_VariableValue("lq_hdr_peak"));
    LQ_GPUTestPattern(gpu, Cvar_VariableValue("lq_hdr_test") != 0);
    LQ_GPUTone(gpu, lq_tone_shoulder.value, lq_tone_white.value, lq_emission.value, lq_sprite_emission.value);
    {   /* Say in the console when output switches between SDR and HDR. */
        static int reported = -1; static float reported_headroom;
        float headroom; int hdr = LQ_GPUDisplayHDR(gpu, &headroom);
        if (hdr != reported || (hdr && fabsf(headroom - reported_headroom) > 0.25f)) {
            /* Record first: while loading, Con_Printf redraws, re-entering here. */
            reported = hdr; reported_headroom = headroom;
            if (hdr) Con_Printf("Display: HDR, highlights up to %.1fx SDR white\n", headroom);
            else Con_Printf("Display: SDR%s\n", Cvar_VariableValue("lq_hdr") != 0 ? "" : " (lq_hdr 0)");
        }
    }
    if (!LQ_GPUFrame(gpu, direct_frame ? LQ_EntitiesFrame() : &empty, error, sizeof(error)) ||
        !LQ_GPUOverlay(gpu, &image, error, sizeof(error)) ||
        !LQ_GPURender(gpu, &view, vid.width, vid.height, error, sizeof(error))) goto failed;
    if (sample) {
        /* The GPU work of this one frame that doesn't scale with resolution,
         * and the frame rate since the last sample. */
        static int frames_at;
        const lq_frame *f = LQ_EntitiesFrame();
        uint32_t world = 0;
        for (uint32_t i = 0; i < map_scene.draw_count; ++i) world += world_draws[i].index_count != 0;
        if (readout_since > 0)
            Con_DPrintf("Luminous frame work: %.0f fps; this frame %u lights, %u shadow faces redrawn, %u entity draws, %u of %u world draws (%u drawn, brightest light known on %u), %u lights in view; probe trace %u lights, %.0f probes\n",
                (host_framecount - frames_at) / (realtime - readout_since), LQ_GPUShadowUpdates(gpu), LQ_GPUShadowFaceUpdates(gpu),
                f ? f->draw_count : 0, world, map_scene.draw_count, LQ_GPUSubmittedFaces(gpu), LQ_GPUKnownLeadFaces(gpu),
                view.runtime_light_count, probe_lights, probe_volume);
        readout_since = realtime; frames_at = host_framecount;
    }
    return true;
failed:
    LQ_FallbackRenderer(error); return false;
}
