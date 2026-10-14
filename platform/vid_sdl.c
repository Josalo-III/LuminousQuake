/* LuminousQuake SDL presentation of Quake's software framebuffer.
 * GPL-2.0-or-later; see gnu.txt. */
#include "quakedef.h"
#include "d_local.h"
#include "sdl_backend.h"
#include "gpu_sdl.h"
#include "render_bridge.h"

viddef_t vid;
unsigned short d_8to16table[256];
unsigned d_8to24table[256];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static lq_gpu *gpu;
static qboolean backend_applied;
static qboolean startup_video_applied;
lq_gpu *LQ_PlayableGPU(void) { return gpu; }
static uint32_t *pixels;
static byte *surface_cache;
static byte direct_saved[48 * 48];
static SDL_Rect direct_rect;
static qboolean direct_active, cvars_registered;
static cvar_t vid_width = {"vid_width", "640", true};
static cvar_t vid_height = {"vid_height", "480", true};
static cvar_t vid_fullscreen = {"vid_fullscreen", "0", true};
static cvar_t vid_vsync = {"vid_vsync", "0", true};
static cvar_t gpu_filter = {"gpu_filter", "0", true};
static cvar_t gpu_msaa = {"gpu_msaa", "0", true};
void M_Print(int x, int y, char *text);
void M_DrawCharacter(int x, int y, int character);
void M_Menu_Options_f(void);
static void VideoMenuDraw(void);
static void VideoMenuKey(int key);
static void GPUOptions(void)
{
    if (!gpu) return;
    char error[256];
    int samples = (int)gpu_msaa.value;
    if (samples != 0 && samples != 2 && samples != 4) { samples = 0; Cvar_SetValue("gpu_msaa", 0); }
    if (!LQ_GPUOptions(gpu, gpu_filter.value != 0, samples, vid_vsync.value != 0, error, sizeof(error))) {
        Con_Printf("%s; using no MSAA with VSync.\n", error);
        Cvar_SetValue("gpu_msaa", 0); Cvar_SetValue("vid_vsync", 1);
        if (!LQ_GPUOptions(gpu, gpu_filter.value != 0, 0, 1, error, sizeof(error))) LQ_FallbackRenderer(error);
    }
}
void Host_WriteConfiguration(void);

SDL_Window *LQ_SDLWindow(void) { return window; }
SDL_Renderer *LQ_SDLRenderer(void) { return renderer; }

static int ArgumentDimension(const char *name, int fallback, int minimum, int maximum)
{
    int option = COM_CheckParm((char *)name), value;
    char *end;
    long parsed;
    if (!option) return fallback;
    if (option + 1 >= com_argc) Sys_Error("%s needs a dimension", name);
    parsed = strtol(com_argv[option + 1], &end, 10);
    if (!*com_argv[option + 1] || *end || parsed < minimum || parsed > maximum)
        Sys_Error("%s must be between %d and %d", name, minimum, maximum);
    value = (int)parsed;
    return value;
}

static void Framebuffer(int width, int height)
{
    int cache_size;
    size_t count;
    byte *buffer, *cache;
    short *zbuffer;
    uint32_t *converted;
    SDL_Texture *next;
    if (width < 320 || width > MAXWIDTH || height < 200 || height > MAXHEIGHT || (width & 7))
        Sys_Error("Software resolution must be 320..%d by 200..%d, with width divisible by 8", MAXWIDTH, MAXHEIGHT);
    if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
        int output_width, output_height;
        SDL_GetWindowSizeInPixels(window, &output_width, &output_height);
        if (output_width > 0 && output_height > 0) {
            height = (int)lround((double)width * output_height / output_width);
            if (height > MAXHEIGHT) {
                height = MAXHEIGHT;
                width = ((int)((double)height * output_width / output_height)) & ~7;
            }
            if (height < 200 || width < 320) Sys_Error("Unsupported display aspect ratio");
        }
    } else if (window) {
        float aspect = (float)width / height;
        int window_width, window_height;
        SDL_GetWindowSize(window, &window_width, &window_height);
        SDL_SetWindowSize(window, window_width, (int)lround(window_width / aspect));
    }
    count = (size_t)width * height;
    cache_size = D_SurfaceCacheForRes(width, height);
    if (cache_size < 1024 || cache_size > 128 * 1024 * 1024)
        Sys_Error("Invalid software surface-cache size");
    buffer = calloc(count, 1);
    zbuffer = calloc(count, sizeof(*zbuffer));
    converted = calloc(count, sizeof(*converted));
    cache = malloc(cache_size);
    next = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, width, height) : NULL;
    if (!buffer || !zbuffer || !converted || !cache || (renderer && !next)) {
        free(buffer); free(zbuffer); free(converted); free(cache);
        SDL_DestroyTexture(next);
        Sys_Error("Cannot create software framebuffer: %s", SDL_GetError());
    }
    if (next) {
        SDL_SetTextureScaleMode(next, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(next, SDL_BLENDMODE_NONE);
    }
    if (surface_cache) D_FlushCaches();
    free(vid.buffer); free(d_pzbuffer); free(pixels); free(surface_cache);
    SDL_DestroyTexture(texture);
    vid.buffer = vid.conbuffer = buffer;
    d_pzbuffer = zbuffer; pixels = converted; surface_cache = cache; texture = next;
    if (vid.height) {
        scr_con_current *= (float)height/vid.height;
        scr_conlines *= (float)height/vid.height;
    }
    vid.width = vid.conwidth = vid.rowbytes = vid.conrowbytes = width;
    vid.height = vid.conheight = height;
    vid.maxwarpwidth = WARP_WIDTH; vid.maxwarpheight = WARP_HEIGHT;
    vid.aspect = 1.0f; /* SDL presents square pixels at every aspect ratio. */
    vid.numpages = 1; vid.recalc_refdef = true;
    vid.colormap = host_colormap;
    vid.fullbright = host_colormap ? 256 - LittleLong(*((int *)host_colormap + 2048)) : 256;
    D_InitCaches(surface_cache, cache_size);
    if (renderer) SDL_SetRenderLogicalPresentation(renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    direct_active = false;
    Con_Printf("Software framebuffer: %dx%d\n", width, height);
}

void VID_SetPalette(unsigned char *palette)
{
    int i;
    for (i = 0; i < 256; ++i) {
        unsigned red = palette[i * 3], green = palette[i * 3 + 1], blue = palette[i * 3 + 2];
        d_8to24table[i] = UINT32_C(0xff000000) | (red << 16) | (green << 8) | blue;
        d_8to16table[i] = (unsigned short)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
    }
}
void VID_ShiftPalette(unsigned char *palette) { VID_SetPalette(palette); }

static void Fullscreen(void)
{
    if (!SDL_SetWindowFullscreen(window, vid_fullscreen.value != 0)) {
        Con_Printf("Fullscreen change failed: %s\n", SDL_GetError());
        Cvar_SetValue("vid_fullscreen", !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
    } else if (!SDL_SyncWindow(window))
        Con_Printf("Fullscreen transition failed: %s\n", SDL_GetError());
}
static void ToggleFullscreen(void)
{
    Cvar_SetValue("vid_fullscreen", !vid_fullscreen.value);
    Cbuf_AddText("vid_restart\n");
}
static void Restart(void)
{
    if (!window) return;
    if (!isfinite(vid_width.value) || !isfinite(vid_height.value) ||
        vid_width.value < 320 || vid_width.value > MAXWIDTH ||
        vid_height.value < 200 || vid_height.value > MAXHEIGHT)
        Sys_Error("Invalid vid_width/vid_height for software renderer");
    backend_applied = false;
    LQ_ApplyRenderer();
    Fullscreen();
    Framebuffer((int)vid_width.value, (int)vid_height.value);
    GPUOptions();
    if (renderer && !SDL_SetRenderVSync(renderer, vid_vsync.value != 0))
        Con_Printf("VSync setting unavailable: %s\n", SDL_GetError());
    LQ_UpdateCapture();
    Host_WriteConfiguration();
}
void LQ_FallbackRenderer(const char *reason)
{
    Con_Printf("Software renderer fallback: %s\n", reason);
    LQ_RenderBridgeShutdown();
    LQ_GPUDestroy(gpu); gpu = NULL;
    if (!renderer) renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) Sys_Error("Cannot restore software presentation: %s", SDL_GetError());
    Framebuffer((int)vid.width, (int)vid.height);
    SDL_SetRenderVSync(renderer, vid_vsync.value != 0);
}
void LQ_ApplyRenderer(void)
{
    char error[256];
    if (backend_applied || !window) return;
    backend_applied = true;
    if (!startup_video_applied) {
        if (!isfinite(vid_width.value) || !isfinite(vid_height.value) ||
            vid_width.value < 320 || vid_width.value > MAXWIDTH ||
            vid_height.value < 200 || vid_height.value > MAXHEIGHT)
            Sys_Error("Invalid saved video resolution");
        int width = ArgumentDimension("-width", (int)vid_width.value, 320, MAXWIDTH);
        int height = ArgumentDimension("-height", (int)vid_height.value, 200, MAXHEIGHT);
        Cvar_SetValue("vid_width", width); Cvar_SetValue("vid_height", height);
        if (COM_CheckParm("-fullscreen")) Cvar_SetValue("vid_fullscreen", 1);
        else if (COM_CheckParm("-window")) Cvar_SetValue("vid_fullscreen", 0);
        Fullscreen();
        Framebuffer(width, height);
        if (renderer) SDL_SetRenderVSync(renderer, vid_vsync.value != 0);
        startup_video_applied = true;
    }
    if (Cvar_VariableValue("lq_renderer") == 1) {
        if (gpu) { GPUOptions(); return; }
        SDL_DestroyTexture(texture); texture = NULL;
        SDL_DestroyRenderer(renderer); renderer = NULL;
        gpu = LQ_GPUCreate(window, 0, error, sizeof(error));
        if (!gpu) { LQ_FallbackRenderer(error); return; }
        GPUOptions();
        Con_Printf("Classic GPU enabled: GPU world, actors, brushes and effects.\n");
    } else {
        if (gpu) LQ_FallbackRenderer("Software selected");
        if (Cvar_VariableValue("lq_renderer") == 2)
            Con_Printf("Enhanced renderer is pending integration; using Software.\n");
    }
}

void VID_Init(unsigned char *palette)
{
    int width, height, win_width, win_height;
    SDL_WindowFlags flags;
    if (window) return;
    if (!cvars_registered) {
        Cvar_RegisterVariable(&vid_width); Cvar_RegisterVariable(&vid_height);
        Cvar_RegisterVariable(&vid_fullscreen); Cvar_RegisterVariable(&vid_vsync);
        Cvar_RegisterVariable(&gpu_filter); Cvar_RegisterVariable(&gpu_msaa);
        Cmd_AddCommand("vid_restart", Restart);
        Cmd_AddCommand("vid_togglefullscreen", ToggleFullscreen);
        vid_menudrawfn = VideoMenuDraw;
        vid_menukeyfn = VideoMenuKey;
        cvars_registered = true;
    }
    width = ArgumentDimension("-width", (int)vid_width.value, 320, MAXWIDTH);
    height = ArgumentDimension("-height", (int)vid_height.value, 200, MAXHEIGHT);
    win_width = ArgumentDimension("-window-width", 960, 320, 16384);
    win_height = ArgumentDimension("-window-height", 720, 200, 16384);
    if (COM_CheckParm("-fullscreen")) Cvar_SetValue("vid_fullscreen", 1);
    if (COM_CheckParm("-window")) Cvar_SetValue("vid_fullscreen", 0);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_MAC_PRESS_AND_HOLD, "0");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) Sys_Error("SDL video initialization failed: %s", SDL_GetError());
    flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (vid_fullscreen.value) flags |= SDL_WINDOW_FULLSCREEN;
    window = SDL_CreateWindow("LuminousQuake", win_width, win_height, flags);
    if (!window) Sys_Error("SDL window creation failed: %s", SDL_GetError());
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) renderer = SDL_CreateRenderer(window, "software");
    if (!renderer) Sys_Error("SDL renderer creation failed: %s", SDL_GetError());
    SDL_SetRenderVSync(renderer, vid_vsync.value != 0);
    Con_Printf("SDL3 presentation: %s\n", SDL_GetRendererName(renderer));
    Cvar_SetValue("vid_width", width); Cvar_SetValue("vid_height", height);
    Framebuffer(width, height);
    VID_SetPalette(palette);
#ifdef __APPLE__
    /* A visible Cocoa window is not necessarily an active application when
     * launched as a Terminal executable. SDL's Cocoa RaiseWindow activates
     * the application and makes this window key. Do this only at startup so
     * switching to another application continues to release input normally. */
    SDL_RaiseWindow(window);
    SDL_PumpEvents();
#endif
    LQ_SetFocus((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
}
void VID_Update(vrect_t *rects)
{
    size_t i, count = (size_t)vid.width * vid.height;
    (void)rects;
    if (LQ_UIActive()) return; /* Present only after the full-resolution canvas is restored. */
    if (gpu && LQ_RenderBridgePresent()) return;
    if (!renderer) return;
    for (i = 0; i < count; ++i) pixels[i] = d_8to24table[vid.buffer[i]];
    if (!SDL_UpdateTexture(texture, NULL, pixels, (int)vid.width * sizeof(*pixels)))
        Sys_Error("SDL texture update failed: %s", SDL_GetError());
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}
qboolean LQ_SaveFrame(const char *path)
{
    SDL_Surface *surface;
    qboolean result;
    if (gpu) {
        byte *data; uint32_t width, height; char error[256];
        if (!LQ_GPUReadPixels(gpu, &data, &width, &height, error, sizeof(error))) return false;
        surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, data, width * 4);
        result = surface && SDL_SaveBMP(surface, path);
        SDL_DestroySurface(surface); free(data); return result;
    }
    if (!renderer) return false;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer); SDL_RenderTexture(renderer, texture, NULL, NULL);
    surface = SDL_RenderReadPixels(renderer, NULL);
    if (!surface) return false;
    result = SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface); SDL_RenderPresent(renderer);
    return result;
}
void VID_Shutdown(void)
{
    if (!window && !renderer) return;
    LQ_RenderBridgeShutdown(); LQ_GPUDestroy(gpu); gpu = NULL;
    LQ_UIShutdown();
    backend_applied = false;
    startup_video_applied = false;
    IN_ClearStates();
    SDL_SetWindowRelativeMouseMode(window, false);
    SDL_StopTextInput(window);
    if (surface_cache) D_FlushCaches();
    free(vid.buffer); free(d_pzbuffer); free(pixels); free(surface_cache);
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    texture = NULL; renderer = NULL; window = NULL;
    pixels = NULL; surface_cache = NULL; d_pzbuffer = NULL;
    memset(&vid, 0, sizeof(vid));
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
void D_BeginDirectRect(int x, int y, byte *bitmap, int width, int height)
{
    int row;
    if (!vid.buffer || width > 48 || height > 48 || width < 1 || height < 1 ||
        x < 0 || y < 0 || x + width > (int)vid.width || y + height > (int)vid.height) return;
    direct_rect = (SDL_Rect){x, y, width, height}; direct_active = true;
    for (row = 0; row < height; ++row) {
        byte *dest = vid.buffer + (y + row) * vid.rowbytes + x;
        memcpy(direct_saved + row * width, dest, width);
        memcpy(dest, bitmap + row * width, width);
    }
    VID_Update(NULL);
}
void D_EndDirectRect(int x, int y, int width, int height)
{
    int row;
    (void)x; (void)y; (void)width; (void)height;
    if (!direct_active || !vid.buffer) return;
    for (row = 0; row < direct_rect.h; ++row)
        memcpy(vid.buffer + (direct_rect.y + row) * vid.rowbytes + direct_rect.x,
            direct_saved + row * direct_rect.w, direct_rect.w);
    direct_active = false;
    VID_Update(NULL);
}
void VID_HandlePause(qboolean pause) { (void)pause; LQ_UpdateCapture(); }

static const int resolutions[][2] = {
    {320, 240}, {400, 300}, {512, 384},
    {640, 360}, {640, 400}, {640, 480}, {800, 600},
    {960, 540}, {960, 600}, {1024, 576}, {1024, 768},
    {1280, 720}, {1280, 800}, {1280, 960}, {1280, 1024},
    {1440, 900}, {1600, 900}, {1600, 1200}, {1680, 1050},
    {1920, 1080}, {1920, 1200}, {1920, 1440},
    {2048, 1152}, {2560, 1440}, {2560, 1600}, {2880, 1800},
    {3200, 1800}, {3840, 2160}, {3840, 2400}
};
static int video_cursor, video_choice, video_fullscreen, video_vsync;
static int video_filter, video_msaa;
static float video_fov;
static qboolean video_menu_active;
static void VideoMenuInit(void)
{
    if (video_menu_active) return;
    video_choice = 5;
    for (int i = 0; i < (int)(sizeof(resolutions) / sizeof(resolutions[0])); ++i)
        if (vid_width.value == resolutions[i][0] && vid_height.value == resolutions[i][1]) video_choice = i;
    video_fullscreen = vid_fullscreen.value != 0; video_vsync = vid_vsync.value != 0;
    video_filter = gpu_filter.value != 0;
    video_msaa = gpu_msaa.value == 4 ? 2 : gpu_msaa.value == 2 ? 1 : 0;
    video_fov = Cvar_VariableValue("fov");
    if (!isfinite(video_fov) || video_fov < 10 || video_fov > 170) video_fov = 90;
    video_menu_active = true;
}
static void VideoMenuDraw(void)
{
    VideoMenuInit();
    M_Print(88, 16, "VIDEO OPTIONS");
    M_Print(32, 40, "Resolution");
    M_Print(168, 40, va("%d x %d", resolutions[video_choice][0], resolutions[video_choice][1]));
    M_Print(32, 56, "Horizontal FOV"); M_Print(168, 56, va("%.0f deg", video_fov));
    M_Print(32, 72, "Fullscreen"); M_Print(168, 72, video_fullscreen ? "on" : "off");
    M_Print(32, 88, "VSync"); M_Print(168, 88, video_vsync ? "on" : "off");
    M_Print(32, 104, "GPU filtering"); M_Print(168, 104, video_filter ? "smooth" : "crisp");
    M_Print(32, 120, "GPU anti-alias"); M_Print(168, 120, video_msaa == 2 ? "4x MSAA" : video_msaa == 1 ? "2x MSAA" : "off");
    M_Print(32, 136, "Apply changes");
    M_DrawCharacter(16, 40 + video_cursor * 16, 12 + ((int)(realtime * 4) & 1));
    M_Print(32, 160, va("Current: %u x %u", vid.width* (LQ_UIActive() ? LQ_UIScale() : 1), vid.height* (LQ_UIActive() ? LQ_UIScale() : 1)));
    M_Print(32, 176, "Left/right changes; Apply saves.");
    M_Print(32, 192, "Escape returns to Options.");
}
static void VideoMenuKey(int key)
{
    VideoMenuInit();
    if (key == K_ESCAPE) { video_menu_active = false; M_Menu_Options_f(); return; }
    if (key == K_UPARROW) { video_cursor = (video_cursor + 6) % 7; return; }
    if (key == K_DOWNARROW) { video_cursor = (video_cursor + 1) % 7; return; }
    if (key != K_LEFTARROW && key != K_RIGHTARROW && key != K_ENTER) return;
    if (video_cursor == 0) {
        int count = sizeof(resolutions) / sizeof(resolutions[0]);
        video_choice = (video_choice + (key == K_LEFTARROW ? -1 : 1) + count) % count;
    } else if (video_cursor == 1) {
        video_fov += key == K_LEFTARROW ? -5 : 5;
        if (video_fov < 10) video_fov = 10;
        if (video_fov > 170) video_fov = 170;
    } else if (video_cursor == 2) video_fullscreen = !video_fullscreen;
    else if (video_cursor == 3) video_vsync = !video_vsync;
    else if (video_cursor == 4) video_filter = !video_filter;
    else if (video_cursor == 5) video_msaa = (video_msaa + (key == K_LEFTARROW ? 2 : 1)) % 3;
    else if (key == K_ENTER) {
        Cvar_SetValue("vid_width", resolutions[video_choice][0]);
        Cvar_SetValue("vid_height", resolutions[video_choice][1]);
        Cvar_SetValue("fov", video_fov);
        Cvar_SetValue("vid_fullscreen", video_fullscreen); Cvar_SetValue("vid_vsync", video_vsync);
        Cvar_SetValue("gpu_filter", video_filter); Cvar_SetValue("gpu_msaa", video_msaa == 2 ? 4 : video_msaa == 1 ? 2 : 0);
        Cbuf_AddText("vid_restart\n");
        video_menu_active = false;
    }
}
