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
#include "performance.h"
#include "fluorescent.h"
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
    LQ_PerformanceStop();
    LQ_GPUDestroy(gpu); gpu = NULL; LQ_SceneFree(&scene); LQ_QuakeSceneShutdown();
    if (window) SDL_DestroyWindow(window); window = NULL; SDL_Quit();
}
static const char *Argument(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], name)) return i + 1 < argc ? argv[i + 1] : "";
    return NULL;
}
/* Conservative clip-space test: a box is out only when all eight corners
 * lie beyond one side or behind the eye. */
static int Has(int argc, char **argv, const char *name)
{ for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], name)) return 1; return 0; }
/* --compare-probes: dynamic probes around gameplay lights near the camera on
 * the CPU and the GPU, twice, the lights moving between, so the GPU must also
 * clear the probes they left. */
static int CompareProbes(lq_scene *s, lq_gpu *device, const float camera[3])
{
    size_t bytes = LQ_GPUDynamicProbeBytes(device);
    uint8_t *cpu = malloc(bytes ? bytes : 1), *gpu_probes = malloc(bytes ? bytes : 1);
    int ok = bytes && cpu && gpu_probes;
    static const float flash[3] = {1.0f, 0.85f, 0.55f}, blast[3] = {1.0f, 0.7f, 0.35f};
    for (int round = 0; ok && round < 2; ++round) {
        lq_runtime_light lights[3];
        float a[3] = {camera[0], camera[1], camera[2]}, b[3] = {camera[0] + 96, camera[1], camera[2] + 16};
        float c[3] = {camera[0], camera[1] + 160, camera[2]};
        if (round) for (int k = 0; k < 3; ++k) { a[k] += k == 0 ? 192 : 0; b[k] += k == 1 ? -128 : 0; c[k] += k == 2 ? 32 : 0; }
        LQ_DynamicRuntimeLight(&lights[0], a, 200, flash, 4, 0);
        LQ_DynamicRuntimeLight(&lights[1], b, 350, blast, 12, 1);
        LQ_DynamicRuntimeLight(&lights[2], c, 200, flash, 4, 2);
        const lq_runtime_light *selected[LQ_MAX_DYNAMIC_LIGHTS]; uint32_t box_min[3], box_size[3];
        uint32_t count = LQ_SceneDynamicProbeLights(s, lights, 3, selected, box_min, box_size);
        uint64_t started = SDL_GetTicksNS();
        int active = LQ_SceneDynamicProbes(s, lights, 3, 16, LQ_JobsRunRange, cpu);
        double cpu_ms = (SDL_GetTicksNS() - started) / 1e6;
        if (!count || !active || !LQ_GPUTraceDynamicProbes(device, selected, count, box_min, box_size, 16) ||
            !LQ_GPUReadDynamicProbes(device, gpu_probes)) { ok = 0; break; }
        double total = 0; int peak = 0; uint64_t off = 0, lit_cpu = 0, lit_gpu = 0;
        for (size_t i = 0; i < bytes; ++i) {
            int d = abs((int)cpu[i] - (int)gpu_probes[i]);
            total += d; peak = d > peak ? d : peak; off += d > 2;
            if (i % 4 != 3) { lit_cpu += cpu[i] > 0; lit_gpu += gpu_probes[i] > 0; }
        }
        printf("Dynamic probes, round %d: %u lights, box %ux%ux%u; CPU %.2f ms; lit values CPU %llu, GPU %llu;"
            " mean difference %.4f, largest %d, %.3f%% differ by more than 2\n", round + 1, count,
            box_size[0], box_size[1], box_size[2], cpu_ms, (unsigned long long)lit_cpu, (unsigned long long)lit_gpu,
            total / bytes, peak, 100.0 * off / bytes);
        /* GPU time: tracing plus readback, less readback alone, over 30 runs. */
        uint64_t traced = 0, read = 0;
        for (int i = 0; ok && i < 30; ++i) {
            uint64_t t0 = SDL_GetTicksNS();
            ok = LQ_GPUTraceDynamicProbes(device, selected, count, box_min, box_size, 16) &&
                LQ_GPUReadDynamicProbes(device, gpu_probes);
            uint64_t t1 = SDL_GetTicksNS();
            ok = ok && LQ_GPUReadDynamicProbes(device, gpu_probes);
            traced += t1 - t0; read += SDL_GetTicksNS() - t1;
        }
        printf("  GPU tracing about %.2f ms per frame (%.2f ms with readback, %.2f ms readback alone)\n",
            (double)(traced - read) / 30e6, traced / 30e6, read / 30e6);
    }
    if (!ok) fprintf(stderr, "Dynamic probe comparison failed: no probes, no light reach, or no GPU probe path\n");
    free(cpu); free(gpu_probes);
    return ok;
}
/* --compare-bounce: bounce with CPU rays, then GPU rays, and how far the
 * results differ over every face sample of each atlas layer and every probe. */
static int CompareBounce(lq_scene *s, uint32_t rays, lq_gpu *device, char *error, size_t size)
{
    uint64_t started = SDL_GetTicksNS();
    if (!LQ_SceneComputeBounceTraced(s, rays, LQ_JobsRunRange, NULL, error, size)) return 0;
    double cpu_seconds = (SDL_GetTicksNS() - started) / 1e9;
    size_t atlas = (size_t)s->bounce.width * s->bounce.height * 4;
    size_t probes = (size_t)s->probe_size[0] * s->probe_size[1] * s->probe_size[2] * 6 * 4 * 4;
    uint8_t *cpu = malloc(atlas), *cpu_probes = malloc(probes ? probes : 1), (*cpu_styles)[4] = malloc(s->draw_count * 4);
    if (!cpu || !cpu_probes || !cpu_styles) { free(cpu); free(cpu_probes); free(cpu_styles); snprintf(error, size, "no memory"); return 0; }
    memcpy(cpu, s->bounce.pixels, atlas);
    if (probes && s->probe_static) memcpy(cpu_probes, s->probe_static, probes);
    for (uint32_t f = 0; f < s->draw_count; ++f) memcpy(cpu_styles[f], s->draws[f].bounce_styles, 4);
    lq_bounce_tracer tracer = LQ_GPUBounceTracer(device);
    started = SDL_GetTicksNS();
    int ok = LQ_SceneComputeBounceTraced(s, rays, LQ_JobsRunRange, &tracer, error, size);
    double gpu_seconds = (SDL_GetTicksNS() - started) / 1e9;
    if (ok && tracer.passes == 0) { snprintf(error, size, "the GPU traced no bounce passes"); ok = 0; }
    if (ok) {
        printf("Bounce: CPU rays %.2f s, GPU rays %.2f s (%u GPU passes), %u samples x %u rays\n",
            cpu_seconds, gpu_seconds, tracer.passes, s->sample_count, rays);
        uint32_t styles_differ = 0;
        for (uint32_t f = 0; f < s->draw_count; ++f) styles_differ += memcmp(cpu_styles[f], s->draws[f].bounce_styles, 4) != 0;
        printf("  faces whose bounce styles differ: %u of %u\n", styles_differ, s->draw_count);
        for (int layer = 0; layer < 4; ++layer) for (int channel = 0; channel < 2; ++channel) {
            double total = 0; int peak = 0; uint64_t count = 0, off = 0;
            for (uint32_t f = 0; f < s->draw_count; ++f) {
                const lq_face_samples *grid = &s->face_samples[f];
                for (uint32_t y = 0; y < grid->height; ++y) for (uint32_t x = 0; x < grid->width; ++x) {
                    size_t texel = ((size_t)(layer * s->atlas_height + grid->atlas_y + y) * s->bounce.width + grid->atlas_x + x) * 4;
                    for (int k = channel ? 3 : 0; k < (channel ? 4 : 3); ++k) {
                        int d = abs((int)cpu[texel + k] - (int)s->bounce.pixels[texel + k]);
                        total += d; peak = d > peak ? d : peak; off += d > 2; ++count;
                    }
                }
            }
            printf("  layer %d %s: mean difference %.3f, largest %d, %.2f%% differ by more than 2 (of %llu values)\n",
                layer, channel ? "alpha (sky/sun visibility)" : "rgb (bounce)", count ? total / count : 0, peak,
                count ? 100.0 * off / count : 0, (unsigned long long)count);
        }
        if (probes && s->probe_static) {
            double total = 0; int peak = 0; uint64_t off = 0;
            for (size_t i = 0; i < probes; ++i) {
                int d = abs((int)cpu_probes[i] - (int)s->probe_static[i]);
                total += d; peak = d > peak ? d : peak; off += d > 2;
            }
            printf("  probes: mean difference %.3f, largest %d, %.2f%% differ by more than 2\n",
                total / probes, peak, 100.0 * off / probes);
        }
    }
    free(cpu); free(cpu_probes); free(cpu_styles);
    return ok;
}
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
    fixture.draws[0].light_bits[0] = 1;
    view->world_draws = fixture.draws; view->world_draw_count = 2;
    view->dynamic_lights[0][0] = 64; view->dynamic_lights[0][3] = 128;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(255,0,error,size)) goto done;
    good = 1;
done:
    view->world_draws = NULL; view->world_draw_count = 0;
    memset(view->dynamic_lights,0,sizeof(view->dynamic_lights)); LQ_SceneFree(&fixture);
    return good;
}
static int LuminousLightTest(lq_view *view, char *error, size_t size)
{
    lq_scene fixture = {0}; int good = 0;
    float saved_style = view->lightstyles[0];
    if (!LQ_SceneFixture(&fixture)) goto done;
    /* A palette entry that is fullbright in every colormap row must still go
     * dark when it belongs to a non-emitting effect. */
    for (int row = 0; row < 64; ++row) {
        fixture.colormap.pixels[(row*256 + 224)*4 + 2] = 255;
        fixture.colormap.pixels[(row*256 + 224)*4 + 3] = 255;
    }
    /* Row 32 holds the palette color at normal brightness (128). Luminous
     * lights surfaces, actors and effects from it in linear light, so 128
     * brightness shows it unchanged: half red, half blue. */
    fixture.colormap.pixels[(32*256 + 201)*4] = 127;
    fixture.colormap.pixels[(32*256 + 202)*4 + 2] = 127;
    fixture.colormap.pixels[(32*256 + 201)*4 + 3] = fixture.colormap.pixels[(32*256 + 202)*4 + 3] = 255;
    if (!LQ_GPUUpload(gpu,&fixture,error,size)) goto done;
    fixture.draws[0].light_bits[0] = fixture.draws[1].light_bits[0] = 1;
    view->world_draws = fixture.draws; view->world_draw_count = fixture.draw_count;
    view->lightstyles[0] = 1;
    view->luminous_lighting = 1; view->runtime_light_count = 1;
    /* At 64 units from the fixture, the compiler-inspired term below yields
     * 128 brightness: the row-32 colour, the expected half red. A half-red
     * light gives 64 brightness in red: a quarter of linear light, which the
     * display encoding shows as half that red. */
    view->runtime_lights[0].position_radius[3] = 320;
    view->runtime_lights[0].falloff[0] = 320; view->runtime_lights[0].falloff[1] = 1;
    view->runtime_lights[0].falloff[2] = 0.5f; view->runtime_lights[0].spot[3] = -2;
    view->runtime_lights[0].color_strength[0] = view->runtime_lights[0].color_strength[1] =
        view->runtime_lights[0].color_strength[2] = 1;
    view->runtime_lights[0].color_strength[3] = 0.5f;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(127,0,error,size)) goto done;
    /* Shading is continuous, not 64 colormap rows: 100 brightness shows
     * 127 * 100/128, where a row lookup would land on a row left black here. */
    view->runtime_lights[0].color_strength[3] = 100.0f / 256;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(99,0,error,size)) goto done;
    view->runtime_lights[0].color_strength[3] = 0;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(0,0,error,size)) goto done;
    view->runtime_lights[0].color_strength[0] = 0.5f;
    view->runtime_lights[0].color_strength[1] = view->runtime_lights[0].color_strength[2] = 1;
    view->runtime_lights[0].color_strength[3] = 0.5f;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(64,0,error,size)) goto done;
    {
        /* An actor over the same spot: Luminous lights it from the runtime light
         * (the row-32 colour, half blue); Classic keeps its vertex shade. */
        lq_vertex actor[3]; memcpy(actor, fixture.vertices, sizeof(actor));
        uint32_t order[] = {0,1,2}; uint8_t blue = 202;
        for (int i = 0; i < 3; ++i) actor[i].shade = 0;
        lq_material skin = {.indices={1,1,1,&blue},.resource_id=46};
        lq_draw draw = {0,3,0,LQ_SURFACE_MODEL|LQ_SURFACE_ACTOR,{255,255,255,255}};
        lq_frame frame = {actor,order,&draw,&skin,3,3,1,1};
        view->runtime_lights[0].color_strength[0] = 1;
        int lit = LQ_GPUFrame(gpu,&frame,error,size) && LQ_GPURender(gpu,view,640,480,error,size) &&
            CenterColor(0,127,error,size);
        view->luminous_lighting = 0;
        lit = lit && LQ_GPURender(gpu,view,640,480,error,size) && CenterColor(0,255,error,size);
        /* Non-emitting particles receive light from either side and go dark
         * with no light. At 128 brightness they show their palette color, like
         * a wall in the same light, not double it. A self-lit particle stays
         * bright in the same scene. */
        view->luminous_lighting = 1;
        draw.flags |= LQ_SURFACE_DIFFUSE_EFFECT;
        view->runtime_lights[0].position_radius[0] = 128;
        lit = lit && LQ_GPUFrame(gpu,&frame,error,size) && LQ_GPURender(gpu,view,640,480,error,size) &&
            CenterColor(0,127,error,size);
        blue = 224; skin.resource_id = 48;
        view->runtime_lights[0].color_strength[3] = 0.125f;
        lit = lit && LQ_GPUFrame(gpu,&frame,error,size) && LQ_GPURender(gpu,view,640,480,error,size) &&
            CenterColor(0,64,error,size);
        view->runtime_lights[0].color_strength[3] = 0;
        lit = lit && LQ_GPURender(gpu,view,640,480,error,size) && CenterColor(0,0,error,size);
        draw.flags = LQ_SURFACE_MODEL;
        lit = lit && LQ_GPUFrame(gpu,&frame,error,size) && LQ_GPURender(gpu,view,640,480,error,size) &&
            CenterColor(0,255,error,size);
        view->luminous_lighting = 0;
        lit = lit && LQ_GPURender(gpu,view,640,480,error,size) && CenterColor(0,255,error,size);
        view->luminous_lighting = 1; frame.draw_count = 0;
        if (!lit || !LQ_GPUFrame(gpu,&frame,error,size)) goto done;
    }
    view->luminous_lighting = 0; view->runtime_light_count = 0;
    if (!LQ_GPURender(gpu,view,640,480,error,size) || !CenterColor(255,0,error,size)) goto done;
    puts("PASS Luminous 32-bit linear point/actor lighting, diffuse effects, fullbright emission, darkness and Classic isolation");
    good = 1;
done:
    view->lightstyles[0] = saved_style;
    view->world_draws = NULL; view->world_draw_count = 0;
    view->luminous_lighting = 0; view->runtime_light_count = 0;
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
        fixture.draws[0] = (lq_draw){0, 6, 0, 0, {255,255,255,255}, {0}};
        fixture.draws[1] = (lq_draw){6, 36, 0, 0, {255,255,255,255}, {0}};
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
        view->luminous_lighting = 1;
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
        /* Every shadow array: the same light in a 256 and each 128 slot casts
         * the same shadow, with the same face orientation. */
        memcpy(light->origin, origin, sizeof(origin)); light->intensity = 600;
        for (uint32_t slot = 20; slot <= 100; slot += 40) { /* 256 and both 128 arrays */
            memset(view->runtime_lights, 0, sizeof(view->runtime_lights));
            if (LQ_BuildRuntimeLights(&fixture, view->lightstyles, view->origin, view->runtime_lights) != 1) {
                snprintf(error, size, "shadow tier fixture selection"); goto done;
            }
            view->runtime_lights[slot] = view->runtime_lights[0]; memset(&view->runtime_lights[0], 0, sizeof(view->runtime_lights[0]));
            view->runtime_light_count = slot + 1;
            memcpy(interactions, fixture.draws, 2 * sizeof(*interactions));
            interactions[0].light_bits[slot / 32] = 1u << (slot % 32); interactions[1].index_count = 0;
            if (!LQ_GPURender(gpu, view, 640, 480, error, size)) goto done;
            float shadow[3], lit[3];
            for (int j = 0; j < 3; ++j) {
                shadow[j] = origin[j] + 256*a[j] + 80*u[j] + 48*v[j];
                lit[j] = origin[j] + 256*a[j] - 80*u[j] - 48*v[j];
            }
            int dark, bright;
            if (!ScreenRed(view, shadow, &dark, error, size) || !ScreenRed(view, lit, &bright, error, size)) goto done;
            if (dark >= 16 || bright <= 100) {
                snprintf(error, size, "shadow axis %d slot %u: shadow red %d, lit red %d", axis, slot, dark, bright);
                goto done;
            }
        }
        memset(view->runtime_lights, 0, sizeof(view->runtime_lights));
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
            /* Average three locations on each side: the world-anchored 16-tap
             * pattern is intentionally noisy at any single penumbra pixel. */
            int umbra = 0, rim = 0;
            for (int sample = 0; sample < 3; ++sample) {
                float inside[3], outside[3]; int dark, bright;
                for (int j = 0; j < 3; ++j) {
                    inside[j] = origin[j] + 256*a[j] + (80 + 10*sample)*u[j] + 48*v[j];
                    outside[j] = origin[j] + 256*a[j] + (10*sample)*u[j] + 48*v[j];
                }
                if (!ScreenRed(view, inside, &dark, error, size) || !ScreenRed(view, outside, &bright, error, size)) goto done;
                umbra += dark; rim += bright;
            }
            umbra /= 3; rim /= 3;
            int ok = radius ? umbra > 20 && rim < 240 && rim > umbra + 20 : umbra < 16 && rim > 140;
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
        for (int step = 0; step < 6; ++step) {
            /* 0: new cube; 1: touched face only; 2: box moved -60u; 3: box gone; 4: nothing to redraw. */
            if (step == 2) for (int i = 0; i < 24; ++i) for (int j = 0; j < 3; ++j) actor[i].position[j] -= 60*u[j];
            if (step == 3) moving.draw_count = 0;
            if (step == 5) { moving.draw_count = 1; box_draw.flags |= LQ_SURFACE_NOSHADOW; }
            if ((step <= 3 || step == 5) && !LQ_GPUFrame(gpu, &moving, error, size)) goto done;
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
            uint32_t expected = step == 0 ? 6 : step >= 4 ? 0 : 1;
            if (!ok || faces != expected) {
                snprintf(error, size, "moving caster axis %d step %d: old %d new %d lit %d, %u faces redrawn",
                    axis, step, old_red, new_red, lit_red, faces);
                goto done;
            }
        }
        memcpy(view->origin, origin, sizeof(origin));
        fixture.draw_count = 2;
    }
    puts("PASS Luminous cube shadows on six axes in all three resolution tiers, moving light, cached static shadows, range cutoff, emitter penumbrae and moving casters");
    good = 1;
done:
    view->lightstyles[0] = saved_style;
    view->world_draws = NULL; view->world_draw_count = 0;
    view->luminous_lighting = 0; view->runtime_light_count = 0;
    memset(view->origin, 0, sizeof(view->origin)); memset(view->angles, 0, sizeof(view->angles));
    memset(view->runtime_lights, 0, sizeof(view->runtime_lights));
    free(interactions); LQ_SceneFree(&fixture);
    return good;
}
/* A thick wall meets a grazing receiver at a sealed corner, as at E1M1's
 * first lift. The source's entire eight-unit emitter is behind the wall.
 * Mirror vertically to cover the same join viewed from below. A third pass
 * makes the wall sky, which LIGHT never treats as an occluder. */
static int ContactShadowTest(lq_view *view, char *error, size_t size)
{
    int good = 0;
    lq_scene fixture = {0};
    lq_view saved = *view;
    lq_draw interactions[2];
    for (int variant = 0; variant < 3; ++variant) {
        int mirror = variant == 1 ? 1 : -1, sky = variant == 2;
        LQ_SceneFree(&fixture);
        if (!LQ_SceneFixture(&fixture)) goto done;
        free(fixture.vertices); free(fixture.indices);
        fixture.vertices = calloc(28, sizeof(*fixture.vertices));
        fixture.indices = calloc(42, sizeof(*fixture.indices));
        fixture.light_sources = calloc(1, sizeof(*fixture.light_sources));
        if (!fixture.vertices || !fixture.indices || !fixture.light_sources) goto done;
        for (int row = 0; row < 64; ++row) fixture.colormap.pixels[(row*256 + 201)*4] = (uint8_t)(255 - 4*row);
        shadow_mesh mesh = {fixture.vertices, fixture.indices, 0, 0};
        float receiver[3] = {240,-8,-64*mirror}, u[3] = {1,0,0}, v[3] = {0,0,1}, n[3] = {0,1,0};
        ShadowQuad(&mesh, receiver, u, v, 56, 64, n);
        float box[3] = {112,-64,-64*mirror}, half[3] = {72,72,128};
        for (int face = 0; face < 6; ++face) {
            float normal[3] = {0}, fu[3] = {0}, fv[3] = {0}, center[3];
            int axis = face/2, ua = (axis+1)%3, va = (axis+2)%3;
            normal[axis] = face & 1 ? -1 : 1; fu[ua] = fv[va] = 1;
            for (int j = 0; j < 3; ++j) center[j] = box[j] + half[j]*normal[j];
            ShadowQuad(&mesh, center, fu, fv, half[ua], half[va], normal);
        }
        fixture.vertex_count = mesh.vertex_count; fixture.index_count = mesh.index_count;
        fixture.draws[0] = (lq_draw){0,6,0,0,{255,255,255,255},{0}};
        fixture.draws[1] = (lq_draw){6,36,0,sky ? LQ_SURFACE_SKY : 0,{255,255,255,255},{0}};
        fixture.light_source_count = 1;
        LQ_LightSourceDefaults(fixture.light_sources);
        fixture.light_sources[0].intensity = 600; fixture.light_sources[0].source_radius = 8;
        if (!LQ_GPUUpload(gpu, &fixture, error, size)) goto done;
        memset(view, 0, sizeof(*view));
        view->horizontal_fov = 90; view->luminous_lighting = 1; view->lightstyles[0] = 1;
        view->origin[0] = 240; view->origin[1] = 80; view->origin[2] = -64*mirror; view->angles[1] = 270;
        view->runtime_light_count = LQ_BuildRuntimeLights(&fixture, view->lightstyles, view->origin, view->runtime_lights);
        LQ_BuildLightInteractions(&fixture, view->runtime_lights, view->runtime_light_count, interactions);
        interactions[1].index_count = 0; /* hidden, but still casts */
        view->world_draws = interactions; view->world_draw_count = 2;
        for (int shadows = 0; shadows <= 1; ++shadows) {
            view->disable_shadows = !shadows;
            if (!LQ_GPURender(gpu, view, 640, 480, error, size)) goto done;
            for (int x = 186; x <= 218; x += 8) {
                float point[3] = {(float)x,-8,-64*mirror}; int red;
                if (!ScreenRed(view, point, &red, error, size)) goto done;
                if (shadows && !sky ? red >= 16 : red <= 80) {
                    snprintf(error, size, "sealed corner mirror %d sky %d x %d shadows %d: red %d", mirror, sky, x, shadows, red);
                    goto done;
                }
            }
        }
    }
    puts("PASS Luminous sealed grazing corners above and below, with unshadowed controls; sky does not occlude");
    good = 1;
done:
    if (!good && !*error) snprintf(error, size, "contact shadow fixture allocation");
    *view = saved; LQ_SceneFree(&fixture);
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
    int test = Has(argc, argv, "--gpu-test"), luminous = Has(argc, argv, "--luminous") ||
        Has(argc, argv, "--enhanced"); /* the renderer's former name */
    uint64_t shadow_updates = 0, frames = 0;
    int result = 1, active = 1, captured = 1, quit = 0;
    lq_draw *interaction_draws = NULL, *pvs_draws = NULL;
    uint8_t *pvs = NULL;
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
    const char *light_gamma = Argument(argc, argv, "--light-gamma");
    if (light_gamma) {
        char *end; view.light_gamma = strtof(light_gamma, &end);
        if (!*light_gamma || *end || !isfinite(view.light_gamma) || view.light_gamma < 1 || view.light_gamma > 3) {
            fprintf(stderr, "--light-gamma needs 1 <= exponent <= 3\n"); return 1;
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
    const char *bounce_scale = Argument(argc, argv, "--bounce-scale");
    if (bounce_scale) { /* replaces the map's per-reflection bounce scale */
        char *end; float scale = strtof(bounce_scale, &end);
        if (!*bounce_scale || *end || !isfinite(scale) || scale < 0 || scale > 16) {
            fprintf(stderr, "--bounce-scale needs 0..16\n"); return 1;
        }
        scene.bounce_scale = scale;
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
    int compare = Has(argc, argv, "--compare-bounce");
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) { snprintf(error, sizeof(error), "SDL3: %s", SDL_GetError()); goto done; }
    if (!LQ_PerformanceStart(0, NULL, NULL))
        fprintf(stderr, "Resource monitor unavailable; retaining configured worker budget.\n");
    window = SDL_CreateWindow("LuminousQuake GPU world viewer / WASD fly / E Q vertical / F1 capture / Esc exit",
        960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (test || compare || Has(argc, argv, "--compare-probes") || Has(argc, argv, "--light-budget") ? SDL_WINDOW_HIDDEN : 0));
    if (!window) { snprintf(error, sizeof(error), "window: %s", SDL_GetError()); goto done; }
    gpu = LQ_GPUCreate(window, 1, error, sizeof(error));
    if (!gpu) goto done;
    if (Has(argc, argv, "--smooth") && !LQ_GPUOptions(gpu, 1, 0, 1, error, sizeof(error))) goto done;
    /* As in game: failing fluorescent tubes take private styles, and tubes film's colour, before bounce. */
    if (luminous) { LQ_SceneAssignFluorescentStyles(&scene); LQ_SceneTintFluorescents(&scene, LQ_CAMERA_FILM); }
    if (luminous && compare) {
        LQ_JobsRegister();
        result = CompareBounce(&scene, (uint32_t)rays, gpu, error, sizeof(error)) ? 0 : 1;
        goto done;
    }
    if (luminous && !view.disable_bounce) {
        LQ_JobsRegister();
        uint64_t started = SDL_GetTicksNS();
        lq_bounce_tracer tracer = LQ_GPUBounceTracer(gpu);
        if (!LQ_SceneComputeBounceTraced(&scene, (uint32_t)rays, LQ_JobsRunRange,
                Has(argc, argv, "--cpu-bounce") ? NULL : &tracer, error, sizeof(error))) goto done;
        /* Over every face sample: first-layer bounce, and sky/sun visibility. */
        double total = 0, peak = 0, sky = 0; uint64_t texels = 0, open = 0, sunlit = 0;
        int has_sky = scene.sky_light[0] + scene.sky_light[1] + scene.sky_light[2] > 0;
        int has_sun = scene.sun_light[0] + scene.sun_light[1] + scene.sun_light[2] > 0;
        for (uint32_t f = 0; f < scene.draw_count; ++f) {
            const lq_face_samples *grid = &scene.face_samples[f];
            for (uint32_t y = 0; y < grid->height; ++y) for (uint32_t x = 0; x < grid->width; ++x) {
                size_t texel = (size_t)(grid->atlas_y + y) * scene.bounce.width + grid->atlas_x + x;
                const uint8_t *p = scene.bounce.pixels + texel * 4;
                const uint8_t *sun = p + (size_t)scene.atlas_height * scene.bounce.width * 4;
                double value = (p[0] + p[1] + p[2]) / 3.0; total += value; peak = value > peak ? value : peak; ++texels;
                if (has_sky) { sky += p[3] / 255.0; open += p[3] > 0; }
                if (has_sun) sunlit += sun[3] > 0;
            }
        }
        printf("Luminous bounce: %u samples, %ld rays each, %u bounces at scale %.2f, %s, %.2f s; first layer mean %.1f, peak %.0f brightness; "
            "sky seen by %.1f%% (mean visibility %.3f), sun by %.1f%%\n",
            scene.sample_count, rays, scene.bounces, scene.bounce_scale, tracer.passes ? "rays on the GPU" : "rays on the CPU",
            (double)(SDL_GetTicksNS() - started) / 1e9,
            texels ? total / texels : 0, peak, texels ? 100.0 * open / texels : 0, texels ? sky / texels : 0,
            texels ? 100.0 * sunlit / texels : 0);
        if (scene.probe_static) {
            size_t probes = (size_t)scene.probe_size[0] * scene.probe_size[1] * scene.probe_size[2], valid = 0;
            for (size_t i = 0; i < probes; ++i) valid += scene.probe_valid[i];
            printf("Luminous probes: %ux%ux%u at %.0f units, %zu in open space; styles %u %u %u %u\n",
                scene.probe_size[0], scene.probe_size[1], scene.probe_size[2], scene.probe_spacing, valid,
                scene.probe_styles[0], scene.probe_styles[1], scene.probe_styles[2], scene.probe_styles[3]);
        }
    }
    int fixed_camera = Has(argc, argv, "--fixed-camera");
    active = 0;
    if (!test) printf("GPU camera: %.1f %.1f %.1f; angles %.1f %.1f %.1f\n",
        view.origin[0], view.origin[1], view.origin[2], view.angles[0], view.angles[1], view.angles[2]);
    if (Has(argc, argv, "--light-budget")) {
        /* Which reaching map lights the runtime slot limit leaves out here,
         * for the game's map slots and the viewer's full set. */
        uint32_t sources = scene.light_source_count ? scene.light_source_count : 1;
        uint8_t *visible = malloc(scene.draw_count ? scene.draw_count : 1);
        lq_light_drop *game = calloc(sources, sizeof(*game)), *all = calloc(sources, sizeof(*all));
        float styles[256]; LQ_QuakeLightstyles(0, styles);
        if (!visible || !game || !all || !LQ_QuakeSceneVisibleDraws(&scene, view.origin, visible)) {
            free(visible); free(game); free(all); snprintf(error, sizeof(error), "cannot mark PVS faces"); goto done;
        }
        enum { SHOWN = 16 };
        uint32_t game_drops, all_drops, game_slots = LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS;
        /* First the whole PVS the game submits, then the faces inside a 4:3 view.
         * Both select as the game does, ranking lights on faces in view first. */
        float matrix[16], selection[16];
        LQ_ViewMatrix(&view, 4.0f / 3, matrix); LQ_LightSelectionClip(&view, 4.0f / 3, selection);
        if (!LQ_SceneDrawBounds(&scene)) {
            free(visible); free(game); free(all); snprintf(error, sizeof(error), "cannot bound draws"); goto done;
        }
        for (int pass = 0; pass < 2; ++pass) {
            uint32_t faces = 0;
            for (uint32_t i = 0; i < scene.draw_count; ++i) {
                faces += visible[i] && (!pass || LQ_BoundsInClip(matrix, scene.draw_bounds[i]));
            }
            const float *counted = pass ? matrix : NULL;
            uint32_t reaching = LQ_LightBudgetInView(&scene, styles, view.origin, visible, selection, counted,
                game_slots, game, sources, &game_drops);
            LQ_LightBudgetInView(&scene, styles, view.origin, visible, selection, counted,
                LQ_MAX_RUNTIME_LIGHTS, all, sources, &all_drops);
            printf("Light budget, %s: %u map lights can reach %u faces; %u game slots drop %u, %u viewer slots drop %u\n",
                pass ? "in view" : "PVS", reaching, faces, game_slots, game_drops, LQ_MAX_RUNTIME_LIGHTS, all_drops);
        }
        if (game_drops) printf("Dropped in game for faces in view, strongest first (falloff at the nearest face; no incidence, cone or shadow):\n");
        for (uint32_t i = 0; i < game_drops && i < SHOWN; ++i) {
            const lq_light_source *light = &scene.light_sources[game[i].source];
            int viewer = 0;
            for (uint32_t j = 0; j < all_drops && !viewer; ++j) viewer = all[j].source == game[i].source;
            printf("  entity %u %s light %.0f style %u at %.0f %.0f %.0f%s: up to %.0f%s\n", light->entity_index,
                light->classname, light->intensity, light->style, light->origin[0], light->origin[1], light->origin[2],
                light->spotlight ? " (spot)" : "", game[i].peak, viewer ? "; viewer also drops" : "");
        }
        free(visible); free(game); free(all); result = 0; goto done;
    }
    if (!LQ_GPUUpload(gpu, &scene, error, sizeof(error))) goto done;
    if (luminous && Has(argc, argv, "--compare-probes")) { result = CompareProbes(&scene, gpu, view.origin) ? 0 : 1; goto done; }
    if (luminous) {
        interaction_draws = malloc(scene.draw_count*sizeof(*interaction_draws));
        if (interaction_draws && !LQ_SceneDrawBounds(&scene)) { free(interaction_draws); interaction_draws = NULL; }
        if (!interaction_draws) { snprintf(error,sizeof(error),"cannot allocate light interactions"); goto done; }
        pvs = malloc(scene.draw_count); pvs_draws = malloc(scene.draw_count*sizeof(*pvs_draws));
        if (!pvs || !pvs_draws) { snprintf(error,sizeof(error),"cannot allocate light selection"); goto done; }
        view.world_draws = interaction_draws; view.world_draw_count = scene.draw_count;
    }
    if (test || !luminous) LQ_SceneFree(&scene); /* Ordinary upload must not depend on loader storage. */
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
        if (!LuminousLightTest(&view, error, sizeof(error))) goto done;
        if (!ShadowTest(&view, error, sizeof(error))) goto done;
        if (!ContactShadowTest(&view, error, sizeof(error))) goto done;
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
        if (luminous) {
            /* Select as the game does: its map slots, ranking lights that reach faces
             * in view, then the rest of the camera's PVS. */
            if (LQ_QuakeSceneVisibleDraws(&scene, view.origin, pvs))
                for (uint32_t i = 0; i < scene.draw_count; ++i) {
                    pvs_draws[i] = scene.draws[i];
                    if (!pvs[i]) pvs_draws[i].index_count = 0;
                }
            else memcpy(pvs_draws, scene.draws, scene.draw_count*sizeof(*pvs_draws));
            int width, height; SDL_GetWindowSizeInPixels(window, &width, &height);
            float selection[16];
            LQ_LightSelectionClip(&view, width > 0 && height > 0 ? (float)width / (float)height : 4.0f / 3, selection);
            view.runtime_light_count = LQ_BuildRuntimeLightsInView(&scene, view.lightstyles, view.origin, pvs_draws,
                selection, view.runtime_lights, LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS);
            LQ_BuildLightInteractions(&scene,view.runtime_lights,view.runtime_light_count,interaction_draws);
            view.luminous_lighting = 1;
        }
        int width, height; SDL_GetWindowSizeInPixels(window, &width, &height);
        if (width > 0 && height > 0 && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
            if (!LQ_GPURender(gpu, &view, (uint32_t)width, (uint32_t)height, error, sizeof(error))) goto done;
            shadow_updates += LQ_GPUShadowUpdates(gpu); ++frames;
        } else SDL_Delay(10);
    }
    if (capture && !SaveFrame(capture, error, sizeof(error))) goto done;
    if (luminous) {
        uint32_t active_lights = 0;
        for (uint32_t i = 0; i < view.runtime_light_count; ++i) active_lights += view.runtime_lights[i].position_radius[3] > 0;
        printf("Luminous lights: %u active slots; %llu shadow cubes drawn over %llu frames%s\n", active_lights,
            (unsigned long long)shadow_updates, (unsigned long long)frames, view.disable_shadows ? " (shadows disabled)" : "");
    }
    result = 0;
done:
    if (result) fprintf(stderr, "LuminousQuake GPU: %s\n", error);
    free(interaction_draws); free(pvs_draws); free(pvs); Shutdown(); return result;
}
