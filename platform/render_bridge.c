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
extern cvar_t r_waterwarp;
extern mleaf_t *r_viewleaf;
extern entity_t *currententity;
void R_SetupFrame(void);
void R_MarkLeaves(void);
void R_DrawParticles(void);
void R_StoreEfrags(efrag_t **frags);
static lq_scene map_scene;
static lq_draw *world_draws;
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
    direct_frame = false;
    free(preserve); free(rgba);
    preserve = rgba = NULL; count = 0;
    owner = NULL; loaded = captured_scene = palette_valid = false;
}
void LQ_RenderMapChanged(void) {
    loaded = false; captured_scene = direct_frame = false;
    LQ_EntitiesReset(); LQ_SceneFree(&map_scene); free(world_draws); world_draws = NULL;
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
    if (!loaded && cls.state == ca_connected && cl.worldmodel && !con_forcedup) {
        if (!LQ_QuakeSceneFromWorld(&map_scene, error, sizeof(error)) ||
            !LQ_GPUUpload(gpu, &map_scene, error, sizeof(error))) {
            LQ_FallbackRenderer(error); return;
        }
        world_draws = malloc(map_scene.draw_count*sizeof(*world_draws));
        if (!world_draws) { LQ_FallbackRenderer("Cannot allocate world visibility"); return; }
        loaded = true; palette_valid = false;
        Con_Printf("Classic GPU: copied live BSP world; map resources replaced.\n");
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
    byte *visible = Mod_LeafPVS(r_viewleaf, cl.worldmodel);
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
                world_draws[index].light_bits = surface->dlightframe == r_framecount ? surface->dlightbits : 0;
            }
        }
    }
    LQ_EntitiesBegin(&map_scene); LQ_EntitiesBuild(&map_scene); R_DrawParticles();
    V_SetContentsColor(r_viewleaf->contents);
    CaptureView();
    return 1;
}
int LQ_RenderGPUParticle(const float origin[3], int color)
{
    if (!direct_frame) return 0;
    LQ_EntitiesParticle(origin, color); return 1;
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
static void CaptureView(void)
{
    if (!direct_frame) return;
    memset(&view, 0, sizeof(view));
    memcpy(view.origin, r_refdef.vieworg, sizeof(view.origin));
    memcpy(view.angles, r_refdef.viewangles, sizeof(view.angles));
    view.horizontal_fov = atanf(r_refdef.horizontalFieldOfView * 0.5f) * 360.0f / (float)M_PI;
    view.time = (float)cl.time;
    for (int i = 0; i < 256; ++i) view.lightstyles[i] = d_lightstylevalue[i] / 256.0f;
    view.viewport[0] = r_refdef.vrect.x; view.viewport[1] = r_refdef.vrect.y;
    view.viewport[2] = r_refdef.vrect.width; view.viewport[3] = r_refdef.vrect.height;
    if (direct_frame) { view.world_draws = world_draws; view.world_draw_count = map_scene.draw_count; }
    for (int i = 0; i < MAX_DLIGHTS && i < 32; ++i) if (cl_dlights[i].die >= cl.time && cl_dlights[i].radius > 0) {
        memcpy(view.dynamic_lights[i], cl_dlights[i].origin, sizeof(vec3_t));
        view.dynamic_lights[i][3] = cl_dlights[i].radius;
    }
    view.water_warp = direct_frame && r_waterwarp.value && r_viewleaf->contents <= CONTENTS_WATER;
    captured_scene = true;
}
qboolean LQ_RenderBridgePresent(void)
{
    char error[256]; lq_gpu *gpu = LQ_PlayableGPU();
    if (!gpu || !owner || !rgba || !count) return false;
    qboolean opaque = !captured_scene || key_dest == key_console;
    if (!captured_scene) {
        memset(&view, 0, sizeof(view)); view.horizontal_fov = 90;
        for (int i = 0; i < 256; ++i) view.lightstyles[i] = 1;
    }
    /* UI is drawn in whole-pixel blocks. Upload its logical canvas rather than
     * converting/uploading millions of redundant transparent pixels at 4K. */
    uint32_t scale = direct_frame ? LQ_UIScale() : 1;
    uint32_t overlay_width = (vid.width+scale-1)/scale;
    uint32_t overlay_height = (vid.height+scale-1)/scale;
    for (uint32_t row = 0; row < overlay_height; ++row) for (uint32_t col = 0; col < overlay_width; ++col) {
        uint32_t x = col*scale, y = row*scale;
        size_t i = (size_t)y * vid.width + x;
        size_t pixel = (size_t)row*overlay_width+col;
        unsigned color = d_8to24table[vid.buffer[i]];
        rgba[pixel * 4] = color >> 16; rgba[pixel * 4 + 1] = color >> 8; rgba[pixel * 4 + 2] = color;
        qboolean outside = x < view.viewport[0] || y < view.viewport[1] ||
            x >= view.viewport[0] + view.viewport[2] || y >= view.viewport[1] + view.viewport[3];
        qboolean painted = opaque || outside || preserve[i];
        if (!painted && key_dest == key_menu) {
            rgba[pixel*4] = rgba[pixel*4+1] = rgba[pixel*4+2] = 0;
        }
        rgba[pixel * 4 + 3] = painted ? 255 : key_dest == key_menu ? 128 : 0;
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
    lq_image image = {overlay_width, overlay_height, 4, rgba};
    lq_frame empty = {0};
    if (!LQ_GPUFrame(gpu, direct_frame ? LQ_EntitiesFrame() : &empty, error, sizeof(error)) ||
        !LQ_GPUOverlay(gpu, &image, error, sizeof(error)) ||
        !LQ_GPURender(gpu, &view, vid.width, vid.height, error, sizeof(error))) goto failed;
    return true;
failed:
    LQ_FallbackRenderer(error); return false;
}
