/* SDL3 world-renderer pilot. Free-flight camera; does not execute QuakeC.
 * GPL-2.0-or-later. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "gpu_viewer.h"
#include "scene.h"
#include "scene_fixture.h"
#include "quake_scene.h"
#include "gpu_sdl.h"
#include "jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
static SDL_Window *window;
static lq_gpu *gpu;
static lq_scene scene;
static void Shutdown(void)
{
    LQ_JobsShutdown();
    LQ_GPUDestroy(gpu); gpu = NULL; LQ_SceneFree(&scene); LQ_QuakeSceneShutdown();
    if (window) SDL_DestroyWindow(window); window = NULL; SDL_Quit();
}
static const char *Argument(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], name)) return i + 1 < argc ? argv[i + 1] : "";
    return NULL;
}
static int Has(int argc, char **argv, const char *name)
{ for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], name)) return 1; return 0; }
static int SaveFrame(const char *path, char *error, size_t size)
{
    uint8_t *pixels; uint32_t width, height;
    if (!LQ_GPUReadPixels(gpu, &pixels, &width, &height, error, size)) return 0;
    SDL_Surface *surface = SDL_CreateSurfaceFrom((int)width, (int)height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    int result = surface && SDL_SaveBMP(surface, path);
    if (!result) snprintf(error, size, "frame save: %s", SDL_GetError());
    SDL_DestroySurface(surface); free(pixels); return result;
}
static int PixelTest(int expected, char *error, size_t size)
{
    uint8_t *pixels; uint32_t width, height;
    if (!LQ_GPUReadPixels(gpu, &pixels, &width, &height, error, size)) return 0;
    const uint8_t *center = pixels + (width / 2 + (height / 2) * width) * 4;
    const uint8_t *corner = pixels + (20 + 20 * width) * 4;
    const uint8_t *upper_left = pixels + ((width * 33 / 100) + (height * 30 / 100) * width) * 4;
    int good = center[0] == expected && center[1] == 0 && center[2] == 0 &&
        corner[0] == 0 && corner[1] == 0 && corner[2] == expected &&
        upper_left[0] == 0 && upper_left[1] == 0 && upper_left[2] == expected;
    if (!good) snprintf(error, size, "depth/palette/orientation readback failed: expected %d; center %u,%u,%u; corner %u,%u,%u; upper-left %u,%u,%u",
        expected, center[0], center[1], center[2], corner[0], corner[1], corner[2],
        upper_left[0], upper_left[1], upper_left[2]);
    free(pixels); return good;
}
static int CenterColor(int red, int blue, char *error, size_t size)
{
    uint8_t *pixels; uint32_t width, height;
    if (!LQ_GPUReadPixels(gpu, &pixels, &width, &height, error, size)) return 0;
    const uint8_t *p = pixels+(width/2+height/2*width)*4;
    int good = abs((int)p[0]-red) <= 1 && p[1] == 0 && abs((int)p[2]-blue) <= 1;
    if (!good) snprintf(error, size, "frame color: got %u,%u,%u expected %d,0,%d", p[0],p[1],p[2],red,blue);
    free(pixels); return good;
}
static int FrameTest(lq_view *view, char *error, size_t size)
{
    lq_scene fixture = {0}; int good = 0;
    if (!LQ_SceneFixture(&fixture)) return 0;
    lq_vertex vertices[3]; memcpy(vertices, fixture.vertices, sizeof(vertices));
    uint32_t indices[] = {0,1,2}; uint8_t blue = 202, masked = 255, checker[] = {201,202,202,201};
    lq_material material = {.indices={1,1,1,&blue},.resource_id=42};
    lq_draw draw = {0,3,0,LQ_SURFACE_MODEL,{255,255,255,255}};
    lq_frame frame = {vertices,indices,&draw,&material,3,3,1,1};
    for (int i = 0; i < 3; ++i) { vertices[i].position[0] = 32; vertices[i].shade = 31; }
    if (!LQ_GPUFrame(gpu,&frame,error,size)) goto done;
    /* Mutating the CPU frame after submission cannot change GPU geometry. */
    vertices[0].position[0] = 8192;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(0,127,error,size)) goto done;
    vertices[0].position[0] = 32;
    for (int i = 0; i < 3; ++i) vertices[i].position[0] = 256;
    if (!LQ_GPUFrame(gpu,&frame,error,size) || !LQ_GPURender(gpu,view,640,480,error,size) ||
        !CenterColor(127,0,error,size)) goto done; /* world occludes far actor */
    draw.flags |= LQ_SURFACE_WEAPON;
    if (!LQ_GPUFrame(gpu,&frame,error,size) || !LQ_GPURender(gpu,view,640,480,error,size) ||
        !CenterColor(0,127,error,size)) goto done; /* weapon depth range */
    for (int i = 0; i < 3; ++i) vertices[i].position[0] = 32;
    draw.flags = LQ_SURFACE_MODEL|LQ_SURFACE_MASKED;
    material.indices.pixels = &masked; material.resource_id = 43;
    if (!LQ_GPUFrame(gpu,&frame,error,size) || !LQ_GPURender(gpu,view,640,480,error,size) ||
        !CenterColor(127,0,error,size)) goto done; /* palette 255 transparent */
    material.indices = (lq_image){2,2,1,checker}; material.resource_id = 44;
    draw.flags = LQ_SURFACE_MODEL;
    int aa4 = LQ_GPUSamplesSupported(gpu,4) ? 4 : 0;
    if (!aa4) puts("SKIP 4x MSAA: backend unsupported; checking smooth filtering without MSAA");
    if (!LQ_GPUOptions(gpu,1,aa4,1,error,size) || !LQ_GPUFrame(gpu,&frame,error,size) ||
        !LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(64,64,error,size)) goto done;
    int aa2 = LQ_GPUSamplesSupported(gpu,2) ? 2 : 0;
    if (!aa2) puts("SKIP 2x MSAA: backend unsupported");
    if (!LQ_GPUOptions(gpu,0,aa2,1,error,size) || !LQ_GPURender(gpu,view,640,480,error,size) ||
        !CenterColor(127,0,error,size)) goto done;
    frame.draw_count = 0;
    if (!LQ_GPUFrame(gpu,&frame,error,size) || !LQ_GPUOptions(gpu,0,0,1,error,size)) goto done;
    fixture.draws[0].light_bits = 1;
    view->world_draws = fixture.draws; view->world_draw_count = 2;
    view->dynamic_lights[0][0] = 64; view->dynamic_lights[0][3] = 128;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(255,0,error,size)) goto done;
    good = 1;
done:
    view->world_draws = NULL; view->world_draw_count = 0;
    memset(view->dynamic_lights,0,sizeof(view->dynamic_lights)); LQ_SceneFree(&fixture);
    return good;
}
static int EnhancedLightTest(lq_view *view, char *error, size_t size)
{
    lq_scene fixture = {0}; int good = 0;
    float saved_style = view->lightstyles[0];
    if (!LQ_SceneFixture(&fixture) || !LQ_GPUUpload(gpu,&fixture,error,size)) goto done;
    fixture.draws[0].light_bits = fixture.draws[1].light_bits = 1;
    view->world_draws = fixture.draws; view->world_draw_count = fixture.draw_count;
    view->lightstyles[0] = 1;
    view->enhanced_lighting = 1; view->runtime_light_count = 1;
    /* At 64 units from the fixture, the compiler-inspired term below yields
     * 128 brightness: palette row 31, the expected half-lit red. */
    view->runtime_lights[0].position_radius[3] = 320;
    view->runtime_lights[0].falloff[0] = 320; view->runtime_lights[0].falloff[1] = 1;
    view->runtime_lights[0].falloff[2] = 0.5f; view->runtime_lights[0].spot[3] = -2;
    view->runtime_lights[0].color_strength[0] = view->runtime_lights[0].color_strength[1] =
        view->runtime_lights[0].color_strength[2] = 1;
    view->runtime_lights[0].color_strength[3] = 0.5f;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(127,0,error,size)) goto done;
    view->runtime_lights[0].color_strength[3] = 0;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(0,0,error,size)) goto done;
    view->runtime_lights[0].color_strength[0] = 0.5f;
    view->runtime_lights[0].color_strength[1] = view->runtime_lights[0].color_strength[2] = 1;
    view->runtime_lights[0].color_strength[3] = 0.5f;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(64,0,error,size)) goto done;
    {
        /* An actor over the same spot: Enhanced lights it from the runtime light
         * (palette row 31, half-lit blue); Classic keeps its vertex shade. */
        lq_vertex actor[3]; memcpy(actor, fixture.vertices, sizeof(actor));
        uint32_t order[] = {0,1,2}; uint8_t blue = 202;
        for (int i = 0; i < 3; ++i) actor[i].shade = 0;
        lq_material skin = {.indices={1,1,1,&blue},.resource_id=46};
        lq_draw draw = {0,3,0,LQ_SURFACE_MODEL|LQ_SURFACE_ACTOR,{255,255,255,255}};
        lq_frame frame = {actor,order,&draw,&skin,3,3,1,1};
        view->runtime_lights[0].color_strength[0] = 1;
        int lit = LQ_GPUFrame(gpu,&frame,error,size) && LQ_GPURender(gpu,view,640,480,error,size) &&
            CenterColor(0,127,error,size);
        view->enhanced_lighting = 0;
        lit = lit && LQ_GPURender(gpu,view,640,480,error,size) && CenterColor(0,255,error,size);
        view->enhanced_lighting = 1; frame.draw_count = 0;
        if (!lit || !LQ_GPUFrame(gpu,&frame,error,size)) goto done;
    }
    view->enhanced_lighting = 0; view->runtime_light_count = 0;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(255,0,error,size)) goto done;
    puts("PASS Enhanced normal/distance/style point lighting, Enhanced actors and Classic isolation");
    good = 1;
done:
    view->lightstyles[0] = saved_style;
    view->world_draws = NULL; view->world_draw_count = 0;
    view->enhanced_lighting = 0; view->runtime_light_count = 0;
    memset(view->runtime_lights,0,sizeof(view->runtime_lights)); LQ_SceneFree(&fixture);
    return good;
}
/* Controlled occlusion scene: a point light, a closed box and a receiving wall
 * along each cube axis. The box is hidden from the camera but still casts, so
 * the readback sees only the receiver's direct light and shadow. */
typedef struct { lq_vertex *vertices; uint32_t *indices; uint32_t vertex_count, index_count; } shadow_mesh;
static void ShadowQuad(shadow_mesh *mesh, const float center[3], const float u[3], const float v[3],
    float half_u, float half_v, const float normal[3])
{
    static const float corners[4][2] = {{-1,-1},{1,-1},{1,1},{-1,1}};
    static const uint32_t order[6] = {0,1,2,0,2,3};
    for (int c = 0; c < 4; ++c) {
        lq_vertex *vertex = &mesh->vertices[mesh->vertex_count + c];
        memset(vertex, 0, sizeof(*vertex));
        for (int j = 0; j < 3; ++j) {
            vertex->position[j] = center[j] + u[j]*half_u*corners[c][0] + v[j]*half_v*corners[c][1];
            vertex->normal[j] = normal[j];
        }
        vertex->uv[0] = vertex->uv[1] = vertex->light_uv[0] = vertex->light_uv[1] = 0.5f;
    }
    for (int i = 0; i < 6; ++i) mesh->indices[mesh->index_count++] = mesh->vertex_count + order[i];
    mesh->vertex_count += 4;
}
static int ScreenRed(const lq_view *view, const float point[3], int *red, char *error, size_t size)
{
    float m[16], clip[4];
    LQ_ViewMatrix(view, 640.0f/480, m);
    for (int r = 0; r < 4; ++r) clip[r] = m[r]*point[0] + m[4+r]*point[1] + m[8+r]*point[2] + m[12+r];
    int x = (int)((clip[0]/clip[3]*0.5f + 0.5f) * 640), y = (int)((0.5f - clip[1]/clip[3]*0.5f) * 480);
    uint8_t *pixels; uint32_t width, height;
    if (x < 0 || x >= 640 || y < 0 || y >= 480) { snprintf(error, size, "shadow probe off screen"); return 0; }
    if (!LQ_GPUReadPixels(gpu, &pixels, &width, &height, error, size)) return 0;
    *red = pixels[(y*width + x)*4]; free(pixels); return 1;
}
static int ShadowTest(lq_view *view, char *error, size_t size)
{
    static const float camera_angles[6][2] = {{0,0},{0,180},{0,90},{0,270},{-90,0},{90,0}};
    int good = 0;
    lq_draw *interactions = NULL;
    lq_scene fixture = {0};
    float saved_style = view->lightstyles[0];
    view->lightstyles[0] = 1;
    for (int axis = 0; axis < 6; ++axis) {
        LQ_SceneFree(&fixture); free(interactions); interactions = NULL;
        float a[3] = {0}, u[3] = {0}, v[3] = {0}, origin[3] = {32, -48, 64};
        a[axis/2] = axis & 1 ? -1.0f : 1.0f;
        u[(axis/2 + 1) % 3] = 1; v[(axis/2 + 2) % 3] = 1;
        if (!LQ_SceneFixture(&fixture)) { snprintf(error, size, "shadow fixture allocation"); goto done; }
        free(fixture.vertices); free(fixture.indices);
        fixture.vertices = calloc(28, sizeof(*fixture.vertices));
        fixture.indices = calloc(42, sizeof(*fixture.indices));
        interactions = calloc(2, sizeof(*interactions));
        if (!fixture.vertices || !fixture.indices || !interactions) { snprintf(error, size, "shadow fixture allocation"); goto done; }
        /* Receiver brightness maps straight to red: row r gives 255 - 4r. */
        for (int row = 0; row < 64; ++row) fixture.colormap.pixels[(row*256 + 201)*4] = (uint8_t)(255 - 4*row);
        shadow_mesh mesh = {fixture.vertices, fixture.indices, 0, 0};
        float center[3], normal[3];
        for (int j = 0; j < 3; ++j) { center[j] = origin[j] + 256*a[j]; normal[j] = -a[j]; }
        ShadowQuad(&mesh, center, u, v, 320, 320, normal);
        float box[3];
        for (int j = 0; j < 3; ++j) box[j] = origin[j] + 128*a[j] + 40*u[j] + 24*v[j];
        for (int face = 0; face < 6; ++face) {
            float n[3] = {0}, fu[3] = {0}, fv[3] = {0}, c[3];
            n[face/2] = face & 1 ? -1.0f : 1.0f;
            fu[(face/2 + 1) % 3] = 1; fv[(face/2 + 2) % 3] = 1;
            for (int j = 0; j < 3; ++j) c[j] = box[j] + 12*n[j];
            ShadowQuad(&mesh, c, fu, fv, 12, 12, n);
        }
        fixture.vertex_count = mesh.vertex_count; fixture.index_count = mesh.index_count;
        fixture.draws[0] = (lq_draw){0, 6, 0, 0, {255,255,255,255}, 0};
        fixture.draws[1] = (lq_draw){6, 36, 0, 0, {255,255,255,255}, 0};
        fixture.light_sources = calloc(1, sizeof(*fixture.light_sources));
        if (!fixture.light_sources) { snprintf(error, size, "shadow fixture allocation"); goto done; }
        fixture.light_source_count = 1;
        lq_light_source *light = fixture.light_sources;
        LQ_LightSourceDefaults(light); light->intensity = 600;
        memcpy(light->origin, origin, sizeof(origin));
        if (!LQ_GPUUpload(gpu, &fixture, error, size)) goto done;
        memset(view->runtime_lights, 0, sizeof(view->runtime_lights));
        memcpy(view->origin, origin, sizeof(origin));
        view->angles[0] = camera_angles[axis][0]; view->angles[1] = camera_angles[axis][1];
        view->enhanced_lighting = 1;
        for (int step = 0; step < 4; ++step) {
            /* 0: shadow; 1: cached redraw; 2: light moved, shadow follows; 3: out of range. */
            if (step == 2) for (int j = 0; j < 3; ++j) light->origin[j] = origin[j] + 60*u[j];
            if (step == 3) light->intensity = 200;
            view->runtime_light_count = LQ_BuildRuntimeLights(&fixture, view->lightstyles, view->origin, view->runtime_lights);
            LQ_BuildLightInteractions(&fixture, view->runtime_lights, view->runtime_light_count, interactions);
            interactions[1].index_count = 0; /* hidden caster */
            view->world_draws = interactions; view->world_draw_count = 2;
            if (!LQ_GPURender(gpu, view, 640, 480, error, size)) goto done;
            uint32_t updates = LQ_GPUShadowUpdates(gpu);
            float shift = step >= 2 ? 60.0f : 0.0f, shadow[3], lit[3];
            for (int j = 0; j < 3; ++j) {
                shadow[j] = origin[j] + 256*a[j] + (80 - shift)*u[j] + 48*v[j];
                lit[j] = origin[j] + 256*a[j] + (shift ? 80 : -80)*u[j] + (shift ? 48 : -48)*v[j];
            }
            int dark, bright;
            if (!ScreenRed(view, shadow, &dark, error, size) || !ScreenRed(view, lit, &bright, error, size)) goto done;
            int expected_updates = step == 1 ? 0 : 1;
            int ok = step == 3 ? bright < 8 : dark < 16 && bright > 100;
            if (!ok || (int)updates != expected_updates) {
                snprintf(error, size, "shadow axis %d step %d: shadow red %d, lit red %d, cube updates %u", axis, step, dark, bright, updates);
                goto done;
            }
        }
        /* Penumbra: the box's far edge projects to u = 28 * 256 / 140 = 51.2 on
         * the receiver. A point source keeps it hard; a 32-unit emitter grades
         * both sides of it. */
        memcpy(light->origin, origin, sizeof(origin)); light->intensity = 600;
        for (int radius = 0; radius <= 32; radius += 32) {
            light->source_radius = (float)radius;
            view->runtime_light_count = LQ_BuildRuntimeLights(&fixture, view->lightstyles, view->origin, view->runtime_lights);
            LQ_BuildLightInteractions(&fixture, view->runtime_lights, view->runtime_light_count, interactions);
            interactions[1].index_count = 0;
            if (!LQ_GPURender(gpu, view, 640, 480, error, size)) goto done;
            float inside[3], outside[3]; int umbra, rim;
            for (int j = 0; j < 3; ++j) {
                inside[j] = origin[j] + 256*a[j] + 59*u[j] + 48*v[j];
                outside[j] = origin[j] + 256*a[j] + 43*u[j] + 48*v[j];
            }
            if (!ScreenRed(view, inside, &umbra, error, size) || !ScreenRed(view, outside, &rim, error, size)) goto done;
            int ok = radius ? umbra > 20 && rim < 130 && rim > umbra + 10 : umbra < 16 && rim > 140;
            if (!ok) {
                snprintf(error, size, "penumbra axis %d radius %d: inside red %d, outside red %d", axis, radius, umbra, rim);
                goto done;
            }
        }
        /* Moving caster: the same box as an actor frame draw, with the static
         * box removed. Only the touched cube face is redrawn while it stays;
         * moving it moves the shadow, and removing it erases the old shadow. */
        fixture.draw_count = 1; light->source_radius = 0;
        if (!LQ_GPUUpload(gpu, &fixture, error, size)) goto done;
        memset(view->runtime_lights, 0, sizeof(view->runtime_lights));
        for (int j = 0; j < 3; ++j) view->origin[j] = origin[j] + 70*u[j]; /* sees past the box */
        lq_vertex actor[24]; uint32_t order[36]; uint8_t skin_pixel = 201;
        memcpy(actor, fixture.vertices + 4, sizeof(actor));
        for (int i = 0; i < 36; ++i) order[i] = fixture.indices[6 + i] - 4;
        lq_material skin = {.indices={1,1,1,&skin_pixel},.resource_id=47};
        lq_draw box_draw = {0,36,0,LQ_SURFACE_MODEL|LQ_SURFACE_ACTOR,{255,255,255,255}};
        lq_frame moving = {actor,order,&box_draw,&skin,24,36,1,1};
        for (int step = 0; step < 5; ++step) {
            /* 0: new cube; 1: touched face only; 2: box moved -60u; 3: box gone; 4: nothing to redraw. */
            if (step == 2) for (int i = 0; i < 24; ++i) for (int j = 0; j < 3; ++j) actor[i].position[j] -= 60*u[j];
            if (step == 3) moving.draw_count = 0;
            if (step <= 3 && !LQ_GPUFrame(gpu, &moving, error, size)) goto done;
            view->runtime_light_count = LQ_BuildRuntimeLights(&fixture, view->lightstyles, view->origin, view->runtime_lights);
            LQ_BuildLightInteractions(&fixture, view->runtime_lights, view->runtime_light_count, interactions);
            view->world_draws = interactions; view->world_draw_count = 1;
            if (!LQ_GPURender(gpu, view, 640, 480, error, size)) goto done;
            uint32_t faces = LQ_GPUShadowFaceUpdates(gpu);
            float old_shadow[3], new_shadow[3], lit[3]; int old_red, new_red, lit_red;
            for (int j = 0; j < 3; ++j) {
                old_shadow[j] = origin[j] + 256*a[j] + 80*u[j] + 48*v[j];
                new_shadow[j] = origin[j] + 256*a[j] - 40*u[j] + 48*v[j];
                lit[j] = origin[j] + 256*a[j] - 80*u[j] - 48*v[j];
            }
            if (!ScreenRed(view, old_shadow, &old_red, error, size) || !ScreenRed(view, new_shadow, &new_red, error, size) ||
                !ScreenRed(view, lit, &lit_red, error, size)) goto done;
            int ok = lit_red > 100 && (step < 2 ? old_red < 16 && new_red > 100 :
                step == 2 ? old_red > 100 && new_red < 16 : old_red > 100 && new_red > 100);
            uint32_t expected = step == 0 ? 6 : step == 4 ? 0 : 1;
            if (!ok || faces != expected) {
                snprintf(error, size, "moving caster axis %d step %d: old %d new %d lit %d, %u faces redrawn",
                    axis, step, old_red, new_red, lit_red, faces);
                goto done;
            }
        }
        memcpy(view->origin, origin, sizeof(origin));
        fixture.draw_count = 2;
    }
    puts("PASS Enhanced cube shadows on six axes, moving light, cached static shadows, range cutoff, emitter penumbrae and moving casters");
    good = 1;
done:
    view->lightstyles[0] = saved_style;
    view->world_draws = NULL; view->world_draw_count = 0;
    view->enhanced_lighting = 0; view->runtime_light_count = 0;
    memset(view->origin, 0, sizeof(view->origin)); memset(view->angles, 0, sizeof(view->angles));
    memset(view->runtime_lights, 0, sizeof(view->runtime_lights));
    free(interactions); LQ_SceneFree(&fixture);
    return good;
}
static int MipmapTest(lq_view *view, char *error, size_t size)
{
    lq_scene fixture = {0}; int good = 0;
    if (!LQ_SceneFixture(&fixture)) return 0;
    for (int row = 0; row < 64; ++row) {
        uint8_t *color = fixture.colormap.pixels+(row*256+200)*4;
        color[0] = color[2] = row == 0 ? 128 : row == 31 ? 64 : 0; color[3] = 255;
    }
    if (!LQ_GPUUpload(gpu,&fixture,error,size)) goto done;
    lq_vertex vertices[3]; memcpy(vertices,fixture.vertices,sizeof(vertices));
    uint8_t pixels[64*64];
    for (int y = 0; y < 64; ++y) memset(pixels+y*64,y < 32 ? 201 : 202,64);
    uint32_t indices[] = {0,1,2};
    lq_material material = {.indices={64,64,1,pixels},.resource_id=45};
    lq_draw draw = {0,3,0,LQ_SURFACE_MODEL,{255,255,255,255}};
    lq_frame frame = {vertices,indices,&draw,&material,3,3,1,1};
    for (int i = 0; i < 3; ++i) {
        vertices[i].position[0] = 32; vertices[i].shade = 31;
        vertices[i].uv[0] = i == 0 ? -512 : i == 1 ? 512 : 0;
        vertices[i].uv[1] = 0.25f;
    }
    if (!LQ_GPUOptions(gpu,1,0,1,error,size) || !LQ_GPUFrame(gpu,&frame,error,size)) goto done;
    for (int mode = 1; mode <= 2; ++mode) {
        if (!LQ_GPUTextureOptions(gpu,mode,1,error,size) ||
            !LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(64,64,error,size)) goto done;
        for (int aniso = 2; aniso <= 16; aniso *= 2) {
            if (!LQ_GPUTextureOptions(gpu,mode,aniso,error,size) ||
                !LQ_GPURender(gpu,view,640,480,error,size)) goto done;
        }
        if (!CenterColor(127,0,error,size)) goto done;
    }
    /* Trilinear includes bilinear sampling even if crisp was selected. */
    if (!LQ_GPUOptions(gpu,0,0,1,error,size) || !LQ_GPUTextureOptions(gpu,2,16,error,size) ||
        !LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(127,0,error,size)) goto done;
    draw.flags |= LQ_SURFACE_MASKED;
    if (!LQ_GPUFrame(gpu,&frame,error,size) || !LQ_GPUTextureOptions(gpu,2,1,error,size) ||
        !LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(127,0,error,size)) goto done;
    good = 1;
    puts("PASS mip minification, trilinear, 2x-16x anisotropy and crisp masked textures");
done:
    if (!good) fprintf(stderr,"MipmapTest: %s\n", error);
    LQ_GPUTextureOptions(gpu,0,1,error,size); LQ_SceneFree(&fixture);
    return good;
}
static void Move(lq_view *view, float seconds)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    float yaw = view->angles[1] * 0.01745329252f, pitch = view->angles[0] * 0.01745329252f;
    float speed = keys[SDL_SCANCODE_LSHIFT] ? 960 : 320;
    float forward = (float)keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S];
    float right = (float)keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A];
    float up = (float)(keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_SPACE]) -
        (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_LCTRL]);
    float length = sqrtf(forward * forward + right * right + up * up);
    if (length > 1) { forward /= length; right /= length; up /= length; }
    view->origin[0] += seconds * speed * (forward * cosf(pitch) * cosf(yaw) + right * sinf(yaw));
    view->origin[1] += seconds * speed * (forward * cosf(pitch) * sinf(yaw) - right * cosf(yaw));
    view->origin[2] += seconds * speed * (-forward * sinf(pitch) + up);
}
static int OverlayTest(lq_view *view, char *error, size_t size)
{
    uint8_t image[8 * 8 * 4] = {0}, *pixels;
    uint32_t width, height;
    image[1] = 255; image[3] = 255; /* opaque top-left green marker */
    image[(7 + 7 * 8) * 4] = 255; image[(7 + 7 * 8) * 4 + 3] = 128;
    lq_image overlay = {8, 8, 4, image};
    if (!LQ_GPUOverlay(gpu, &overlay, error, size) ||
        !LQ_GPURender(gpu, view, 640, 480, error, size) ||
        !LQ_GPUReadPixels(gpu, &pixels, &width, &height, error, size)) return 0;
    const uint8_t *top = pixels + (20 + 20 * width) * 4;
    const uint8_t *bottom = pixels + (20 + (height - 20) * width) * 4;
    const uint8_t *center = pixels + (width / 2 + height / 2 * width) * 4;
    const uint8_t *blend = pixels + (width - 20 + (height - 20) * width) * 4;
    int good = top[0] == 0 && top[1] == 255 && top[2] == 0 &&
        bottom[0] == 0 && bottom[1] == 0 && bottom[2] == 127 &&
        center[0] == 127 && center[1] == 0 && center[2] == 0 &&
        blend[0] >= 127 && blend[0] <= 129 && blend[2] >= 62 && blend[2] <= 64;
    if (!good) snprintf(error, size, "overlay orientation/transparency/blending mismatch: top %u,%u,%u bottom %u,%u,%u blend %u,%u,%u",
        top[0], top[1], top[2], bottom[0], bottom[1], bottom[2], blend[0], blend[1], blend[2]);
    free(pixels);
    return good && LQ_GPUOverlay(gpu, NULL, error, size);
}
static int ViewportTest(lq_view *view, char *error, size_t size)
{
    uint8_t *pixels; uint32_t width, height;
    view->viewport[0] = 80; view->viewport[1] = 60;
    view->viewport[2] = 320; view->viewport[3] = 240;
    if (!LQ_GPURender(gpu, view, 640, 480, error, size) ||
        !LQ_GPUReadPixels(gpu, &pixels, &width, &height, error, size)) return 0;
    const uint8_t *center = pixels + (240 + 180 * width) * 4;
    const uint8_t *outside = pixels + (20 + 20 * width) * 4;
    int good = center[0] == 127 && center[1] == 0 && center[2] == 0 &&
        outside[0] == 5 && outside[1] == 5 && outside[2] == 6;
    if (!good) snprintf(error, size, "viewport origin/extent readback mismatch");
    free(pixels); memset(view->viewport, 0, sizeof(view->viewport));
    return good;
}
int LQ_GPUViewer(int argc, char **argv)
{
    char error[512] = "";
    const char *map = Argument(argc, argv, "-map"), *duration = Argument(argc, argv, "--run-seconds");
    const char *capture = Argument(argc, argv, "--screenshot");
    int test = Has(argc, argv, "--gpu-test"), enhanced = Has(argc, argv, "--enhanced");
    uint64_t shadow_updates = 0, frames = 0;
    int result = 1, active = 1, captured = 1, quit = 0;
    lq_draw *interaction_draws = NULL;
    float seconds = 0;
    lq_view view = {0}; view.horizontal_fov = 90;
    view.disable_shadows = Has(argc, argv, "--no-shadows");
    view.static_brush_shadows = 1; /* no gameplay: brush models stay where compiled */
    view.disable_bounce = Has(argc, argv, "--no-bounce");
    const char *exposure = Argument(argc, argv, "--exposure"), *bounce_rays = Argument(argc, argv, "--bounce-rays");
    const char *bounce_gain = Argument(argc, argv, "--bounce-gain");
    if (exposure) {
        char *end; view.exposure = strtof(exposure, &end);
        if (!*exposure || *end || !isfinite(view.exposure) || view.exposure <= 0 || view.exposure > 16) {
            fprintf(stderr, "--exposure needs 0 < scale <= 16\n"); return 1;
        }
    }
    if (bounce_gain) {
        char *end; view.bounce_gain = strtof(bounce_gain, &end);
        if (!*bounce_gain || *end || !isfinite(view.bounce_gain) || view.bounce_gain <= 0 || view.bounce_gain > 64) {
            fprintf(stderr, "--bounce-gain needs 0 < gain <= 64\n"); return 1;
        }
    }
    const char *water = Argument(argc, argv, "--water-alpha");
    if (water) {
        char *end; view.water_alpha = strtof(water, &end);
        if (!*water || *end || !isfinite(view.water_alpha) || view.water_alpha <= 0 || view.water_alpha > 1) {
            fprintf(stderr, "--water-alpha needs 0 < opacity <= 1\n"); return 1;
        }
    }
    long rays = 64;
    if (bounce_rays) {
        char *end; rays = strtol(bounce_rays, &end, 10);
        if (!*bounce_rays || *end || rays < 1 || rays > 4096) { fprintf(stderr, "--bounce-rays needs 1..4096\n"); return 1; }
    }
    const char *camera = Argument(argc, argv, "--camera");
    if (!map) map = Argument(argc, argv, "+map");
    if (!map) map = "e1m1";
    if (duration) {
        char *end; errno = 0; seconds = strtof(duration, &end);
        if (!*duration || *end || errno || !isfinite(seconds) || seconds <= 0 || seconds > 3600) {
            fprintf(stderr, "--run-seconds needs 0 < seconds <= 3600\n"); return 1;
        }
    }
    if (capture && !*capture) { fprintf(stderr, "--screenshot requires a path\n"); return 1; }
    atexit(Shutdown);
    if (test) {
        if (!LQ_SceneFixture(&scene)) { snprintf(error, sizeof(error), "cannot allocate GPU fixture"); goto done; }
    } else {
        if (!LQ_QuakeSceneLoad(argc, argv, map, &scene, error, sizeof(error))) goto done;
        memcpy(view.origin, scene.spawn_origin, sizeof(view.origin));
        memcpy(view.angles, scene.spawn_angles, sizeof(view.angles));
    }
    const char *spot_gain = Argument(argc, argv, "--accent-gain");
    if (spot_gain) { /* replaces the default accent gain before bounce is gathered */
        char *end; float gain = strtof(spot_gain, &end);
        if (!*spot_gain || *end || !isfinite(gain) || gain <= 0 || gain > 4) {
            fprintf(stderr, "--accent-gain needs 0 < gain <= 4\n"); return 1;
        }
        LQ_SceneSetAccentGain(&scene, gain);
    }
    const char *light_radius = Argument(argc, argv, "--light-radius");
    if (light_radius) {
        char *end; float radius = strtof(light_radius, &end);
        if (!*light_radius || *end || !isfinite(radius) || radius < 0 || radius > 1024) {
            fprintf(stderr, "--light-radius needs 0..1024 units\n"); return 1;
        }
        for (uint32_t i = 0; i < scene.light_source_count; ++i) scene.light_sources[i].source_radius = radius;
    }
    if (camera) {
        char extra;
        if (sscanf(camera,"%f %f %f %f %f %c",&view.origin[0],&view.origin[1],&view.origin[2],
                &view.angles[0],&view.angles[1],&extra) != 5) {
            fprintf(stderr, "--camera needs \"x y z pitch yaw\"\n"); return 1;
        }
    }
    if (Has(argc, argv, "--light-info")) {
        for (uint32_t i = 0; i < scene.light_source_count; ++i) {
            const lq_light_source *light = &scene.light_sources[i];
            printf("Light entity %u: %s origin %.3f %.3f %.3f intensity %.3f style %u spawnflags %u cone %.3f target [%s] targetname [%s] rawlight [%s] color [%s] flags %u formula %u attenuation %.3f range %.1f",
                light->entity_index,light->classname,light->origin[0],light->origin[1],light->origin[2],light->intensity,
                light->style,light->spawnflags,light->cone_angle,light->target,light->targetname,
                light->light_value,light->color_value,light->flags,light->formula,light->attenuation,
                LQ_LightSourceRange(light));
            if (light->spotlight) printf(" spot %.3f %.3f %.3f outer %.4f inner %.4f",light->spot_direction[0],
                light->spot_direction[1],light->spot_direction[2],light->spot_outer,light->spot_inner);
            putchar('\n');
        }
        result = 0; goto done;
    }
    if (Has(argc, argv, "--scene-info") || Has(argc, argv, "--light-analysis")) { result = 0; goto done; }
    if (enhanced && !view.disable_bounce) {
        LQ_JobsRegister();
        uint64_t started = SDL_GetTicksNS();
        if (!LQ_SceneComputeBounce(&scene, (uint32_t)rays, LQ_JobsRunRange, error, sizeof(error))) goto done;
        double total = 0, peak = 0; uint64_t texels = 0;
        for (uint32_t y = 0; y < scene.atlas_height; ++y) for (uint32_t x = 0; x < scene.bounce.width; ++x) {
            const uint8_t *p = scene.bounce.pixels + ((size_t)y * scene.bounce.width + x) * 4;
            if (!p[3]) continue;
            double value = (p[0] + p[1] + p[2]) / 3.0; total += value; peak = value > peak ? value : peak; ++texels;
        }
        printf("Enhanced bounce: %u samples, %ld rays each, %u workers, %.2f s; style-0 mean %.1f, peak %.0f brightness\n",
            scene.sample_count, rays, LQ_JobsWorkerCount(), (double)(SDL_GetTicksNS() - started) / 1e9,
            texels ? total / texels : 0, peak);
        if (scene.probe_static) {
            size_t probes = (size_t)scene.probe_size[0] * scene.probe_size[1] * scene.probe_size[2], valid = 0;
            for (size_t i = 0; i < probes; ++i) valid += scene.probe_valid[i];
            printf("Enhanced probes: %ux%ux%u at %.0f units, %zu in open space; styles %u %u %u %u\n",
                scene.probe_size[0], scene.probe_size[1], scene.probe_size[2], scene.probe_spacing, valid,
                scene.probe_styles[0], scene.probe_styles[1], scene.probe_styles[2], scene.probe_styles[3]);
        }
    }
    int fixed_camera = Has(argc, argv, "--fixed-camera");
    active = 0;
    if (!test) printf("GPU camera: %.1f %.1f %.1f; angles %.1f %.1f %.1f\n",
        view.origin[0], view.origin[1], view.origin[2], view.angles[0], view.angles[1], view.angles[2]);
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) { snprintf(error, sizeof(error), "SDL3: %s", SDL_GetError()); goto done; }
    window = SDL_CreateWindow("LuminousQuake GPU world viewer / WASD fly / E Q vertical / F1 capture / Esc exit",
        960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (test ? SDL_WINDOW_HIDDEN : 0));
    if (!window) { snprintf(error, sizeof(error), "window: %s", SDL_GetError()); goto done; }
    gpu = LQ_GPUCreate(window, 1, error, sizeof(error));
    if (!gpu || !LQ_GPUUpload(gpu, &scene, error, sizeof(error))) goto done;
    if (enhanced) {
        interaction_draws = malloc(scene.draw_count*sizeof(*interaction_draws));
        if (!interaction_draws) { snprintf(error,sizeof(error),"cannot allocate light interactions"); goto done; }
        view.world_draws = interaction_draws; view.world_draw_count = scene.draw_count;
    }
    if (test || !enhanced) LQ_SceneFree(&scene); /* Ordinary upload must not depend on loader storage. */
    if (test) {
        for (int i = 0; i < 256; ++i) view.lightstyles[i] = 1;
        for (int i = 0; i < 2; ++i) {
            if (i) {
                if (!LQ_SceneFixture(&scene) || !LQ_GPUUpload(gpu, &scene, error, sizeof(error))) goto done;
                LQ_SceneFree(&scene);
            }
            view.lightstyles[0] = i ? 0.5f : 1;
            if (!LQ_GPURender(gpu, &view, i ? 640 : 320, i ? 480 : 240, error, sizeof(error)) ||
                !PixelTest(i ? 127 : 255, error, sizeof(error))) goto done;
        }
        if (!OverlayTest(&view, error, sizeof(error))) goto done;
        if (!ViewportTest(&view, error, sizeof(error))) goto done;
        if (!FrameTest(&view, error, sizeof(error))) goto done;
        if (!EnhancedLightTest(&view, error, sizeof(error))) goto done;
        if (!ShadowTest(&view, error, sizeof(error))) goto done;
        if (!MipmapTest(&view, error, sizeof(error))) goto done;
        if (!LQ_GPURender(gpu, &view, 640, 480, error, sizeof(error))) goto done;
        if (capture && !SaveFrame(capture, error, sizeof(error))) goto done;
        puts("PASS SDL3 GPU geometry, depth/orientation, palette/lightstyles, overlay alpha, viewport, scene replacement, resize, readback and detached ownership");
        result = 0; goto done;
    }
    active = !!(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
    SDL_SetWindowRelativeMouseMode(window, active && !fixed_camera);
    uint64_t start = SDL_GetTicksNS(), previous = start;
    while (!quit) {
        uint64_t now = SDL_GetTicksNS();
        float elapsed = (float)((double)(now - previous) / 1e9); previous = now;
        view.time = (float)((double)(now - start) / 1e9);
        if (seconds && view.time >= seconds) break;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) { active = 0; SDL_SetWindowRelativeMouseMode(window, false); }
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) { active = 1; SDL_SetWindowRelativeMouseMode(window, captured && !fixed_camera); }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.key == SDLK_ESCAPE) quit = 1;
                else if (event.key.key == SDLK_F1) { captured = !captured; SDL_SetWindowRelativeMouseMode(window, active && captured && !fixed_camera); }
                else if (event.key.key == SDLK_F11 || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
#ifdef __APPLE__
                else if (event.key.key == SDLK_Q && (event.key.mod & SDL_KMOD_GUI)) quit = 1;
#endif
            } else if (event.type == SDL_EVENT_MOUSE_MOTION && active && captured && !fixed_camera) {
                view.angles[1] -= event.motion.xrel * 0.066f;
                view.angles[0] += event.motion.yrel * 0.066f;
                if (view.angles[0] > 80) view.angles[0] = 80;
                if (view.angles[0] < -80) view.angles[0] = -80;
            }
        }
        if (active && !fixed_camera) Move(&view, elapsed > 0.1f ? 0.1f : elapsed);
        LQ_QuakeLightstyles(view.time, view.lightstyles);
        if (enhanced) {
            view.runtime_light_count = LQ_BuildRuntimeLights(&scene,view.lightstyles,view.origin,view.runtime_lights);
            LQ_BuildLightInteractions(&scene,view.runtime_lights,view.runtime_light_count,interaction_draws);
            view.enhanced_lighting = 1;
        }
        int width, height; SDL_GetWindowSizeInPixels(window, &width, &height);
        if (width > 0 && height > 0 && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
            if (!LQ_GPURender(gpu, &view, (uint32_t)width, (uint32_t)height, error, sizeof(error))) goto done;
            shadow_updates += LQ_GPUShadowUpdates(gpu); ++frames;
        } else SDL_Delay(10);
    }
    if (capture && !SaveFrame(capture, error, sizeof(error))) goto done;
    if (enhanced) {
        uint32_t active_lights = 0;
        for (uint32_t i = 0; i < view.runtime_light_count; ++i) active_lights += view.runtime_lights[i].position_radius[3] > 0;
        printf("Enhanced lights: %u active slots; %llu shadow cubes drawn over %llu frames%s\n", active_lights,
            (unsigned long long)shadow_updates, (unsigned long long)frames, view.disable_shadows ? " (shadows disabled)" : "");
    }
    result = 0;
done:
    if (result) fprintf(stderr, "LuminousQuake GPU: %s\n", error);
    free(interaction_draws); Shutdown(); return result;
}
