/* SDL3 world-renderer pilot. Free-flight camera; does not execute QuakeC.
 * GPL-2.0-or-later. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "gpu_viewer.h"
#include "scene.h"
#include "scene_fixture.h"
#include "quake_scene.h"
#include "gpu_sdl.h"
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
    int test = Has(argc, argv, "--gpu-test"), result = 1, active = 1, captured = 1, quit = 0;
    float seconds = 0;
    lq_view view = {0}; view.horizontal_fov = 90;
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
    if (Has(argc, argv, "--light-info")) {
        for (uint32_t i = 0; i < scene.light_source_count; ++i) {
            const lq_light_source *light = &scene.light_sources[i];
            printf("Light entity %u: %s origin %.3f %.3f %.3f intensity %.3f style %u spawnflags %u cone %.3f target [%s] targetname [%s] flags %u\n",
                light->entity_index,light->classname,light->origin[0],light->origin[1],light->origin[2],light->intensity,
                light->style,light->spawnflags,light->cone_angle,light->target,light->targetname,light->flags);
        }
        result = 0; goto done;
    }
    if (Has(argc, argv, "--scene-info")) { result = 0; goto done; }
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
    LQ_SceneFree(&scene); /* Upload/metadata ownership must not depend on loader storage. */
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
        int width, height; SDL_GetWindowSizeInPixels(window, &width, &height);
        if (width > 0 && height > 0 && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
            if (!LQ_GPURender(gpu, &view, (uint32_t)width, (uint32_t)height, error, sizeof(error))) goto done;
        } else SDL_Delay(10);
    }
    if (capture && !SaveFrame(capture, error, sizeof(error))) goto done;
    result = 0;
done:
    if (result) fprintf(stderr, "LuminousQuake GPU: %s\n", error);
    Shutdown(); return result;
}
