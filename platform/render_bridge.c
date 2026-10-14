/* Interim playable Classic GPU: GPU BSP world plus CPU actors/effects/UI.
 * Software rendering still supplies depth/visibility for retained CPU layers.
 * GPL-2.0-or-later. */
#include "quakedef.h"
#include "d_local.h"
#include "sdl_backend.h"
#include "render_bridge.h"
#include "gpu_sdl.h"
#include "quake_scene.h"
#include "scene_fixture.h"

static byte *preserve, *scene_pixels, *rgba;
static short *world_depth;
static size_t count;
static qboolean captured_world, captured_scene, loaded;
static lq_gpu *owner;
static lq_view view;
static unsigned palette_cache[256];
static qboolean palette_valid;
int LQ_RenderBridgeActive(void) { return owner != NULL; }
void LQ_RenderAfterCommands(void) { LQ_ApplyRenderer(); }

void LQ_RenderBridgeShutdown(void)
{
    free(preserve); free(scene_pixels); free(rgba); free(world_depth);
    preserve = scene_pixels = rgba = NULL; world_depth = NULL; count = 0;
    owner = NULL; loaded = captured_world = captured_scene = palette_valid = false;
}
void LQ_RenderMapChanged(void) { loaded = false; captured_world = captured_scene = false; }
void LQ_RenderBegin(void)
{
    lq_scene scene = {0}; char error[256];
    captured_world = captured_scene = false;
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
        free(preserve); free(scene_pixels); free(rgba); free(world_depth);
        preserve = calloc(needed, 1); scene_pixels = malloc(needed);
        rgba = malloc(needed * 4); world_depth = malloc(needed * sizeof(short));
        if (!preserve || !scene_pixels || !rgba || !world_depth) {
            LQ_FallbackRenderer("GPU compositor allocation failed"); return;
        }
        count = needed;
    }
    memset(preserve, 0, count);
    if (!loaded && cls.state == ca_connected && cl.worldmodel && !con_forcedup) {
        if (!LQ_QuakeSceneFromWorld(&scene, error, sizeof(error)) ||
            !LQ_GPUUpload(gpu, &scene, error, sizeof(error))) {
            LQ_SceneFree(&scene); LQ_FallbackRenderer(error); return;
        }
        LQ_SceneFree(&scene); loaded = true; palette_valid = false;
        Con_Printf("Classic GPU: copied live BSP world; map resources replaced.\n");
    }
}
void LQ_RenderSurface(int x, int y, int length, int keep)
{
    if (!owner || !preserve || y < 0 || y >= (int)vid.height || x < 0 ||
        length < 0 || x > (int)vid.width || length > (int)vid.width - x) return;
    memset(preserve + (size_t)y * vid.width + x, keep ? 1 : 0, length);
}
void LQ_RenderWorldDone(void)
{
    if (!owner || !loaded || !world_depth || r_dowarp || Cvar_VariableValue("lcd_x") ||
        Cvar_VariableValue("r_drawflat") || Cvar_VariableValue("r_drawpolys") ||
        Cvar_VariableValue("r_drawculledpolys")) return;
    memcpy(world_depth, d_pzbuffer, count * sizeof(short));
    captured_world = true;
}
void LQ_RenderSceneDone(void)
{
    if (!captured_world) return;
    memcpy(scene_pixels, vid.buffer, count);
    for (size_t i = 0; i < count; ++i)
        if (world_depth[i] != d_pzbuffer[i]) preserve[i] = 1;
    memset(&view, 0, sizeof(view));
    memcpy(view.origin, r_refdef.vieworg, sizeof(view.origin));
    memcpy(view.angles, r_refdef.viewangles, sizeof(view.angles));
    view.horizontal_fov = atanf(r_refdef.horizontalFieldOfView * 0.5f) * 360.0f / (float)M_PI;
    view.time = (float)cl.time;
    for (int i = 0; i < 256; ++i) view.lightstyles[i] = d_lightstylevalue[i] / 256.0f;
    view.viewport[0] = r_refdef.vrect.x; view.viewport[1] = r_refdef.vrect.y;
    view.viewport[2] = r_refdef.vrect.width; view.viewport[3] = r_refdef.vrect.height;
    captured_scene = true;
}
qboolean LQ_RenderBridgePresent(void)
{
    char error[256]; lq_gpu *gpu = LQ_PlayableGPU();
    if (!gpu || !owner || !rgba || !count) return false;
    qboolean opaque = !captured_scene || key_dest == key_menu || key_dest == key_console;
    if (!captured_scene) {
        memset(&view, 0, sizeof(view)); view.horizontal_fov = 90;
        for (int i = 0; i < 256; ++i) view.lightstyles[i] = 1;
    }
    for (uint32_t y = 0; y < vid.height; ++y) for (uint32_t x = 0; x < vid.width; ++x) {
        size_t i = (size_t)y * vid.width + x;
        unsigned color = d_8to24table[vid.buffer[i]];
        rgba[i * 4] = color >> 16; rgba[i * 4 + 1] = color >> 8; rgba[i * 4 + 2] = color;
        qboolean outside = x < view.viewport[0] || y < view.viewport[1] ||
            x >= view.viewport[0] + view.viewport[2] || y >= view.viewport[1] + view.viewport[3];
        rgba[i * 4 + 3] = opaque || outside || preserve[i] || vid.buffer[i] != scene_pixels[i] ? 255 : 0;
    }
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
    lq_image image = {vid.width, vid.height, 4, rgba};
    if (!LQ_GPUOverlay(gpu, &image, error, sizeof(error)) ||
        !LQ_GPURender(gpu, &view, vid.width, vid.height, error, sizeof(error))) goto failed;
    return true;
failed:
    LQ_FallbackRenderer(error); return false;
}
