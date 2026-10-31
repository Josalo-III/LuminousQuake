/* LuminousQuake SDL presentation of Quake's software framebuffer.
 * GPL-2.0-or-later; see gnu.txt. */
#include "quakedef.h"
#include "d_local.h"
#include "sdl_backend.h"
#include "gpu_sdl.h"
#include "render_bridge.h"
#include "jobs.h"
#include "background.h"

viddef_t vid;
unsigned short d_8to16table[256];
unsigned d_8to24table[256];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static lq_gpu *gpu;
static qboolean backend_applied;
static int gpu_mode; /* 1 Classic, 2 Luminous: the lq_renderer the GPU map was built for */
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
static cvar_t vid_vsync = {"vid_vsync", "1", true};
static cvar_t gpu_filter = {"gpu_filter", "0", true};
static cvar_t gpu_mipmaps = {"gpu_mipmaps", "0", true};
static cvar_t gpu_anisotropy = {"gpu_anisotropy", "1", true};
static cvar_t gpu_msaa = {"gpu_msaa", "0", true};
/* GLQuake's name and meaning: liquid opacity, 1 opaque; GPU renderers only. */
static cvar_t r_wateralpha = {"r_wateralpha", "1", true};
/* Luminous fluorescent tubes: the camera the view stands for (LQ_CAMERA_*). */
static cvar_t r_fluorescent = {"r_fluorescent", "1", true};
/* Luminous particles: 0 flat flakes facing the view, 1 tumbling solids and
 * shaded droplets for blood, poison, lava and tar. */
static cvar_t lq_particles = {"lq_particles", "0", true};
/* Display transfer, every renderer: the exponent from display values to light
 * (2.2, the CRT gamma Quake's palette was drawn for). Higher darkens the
 * mid-tones, lower lifts them; Brightness (gamma) still works as before. */
static cvar_t lq_gamma = {"lq_gamma", "2.2", true};
/* GPU renderers: HDR output where the display offers it (1), or never (0). */
static cvar_t lq_hdr = {"lq_hdr", "1", true};
/* HDR highlights stop at this many times SDR white; 0 uses what macOS reports.
 * For displays told more headroom than they show (mirroring). */
static cvar_t lq_hdr_peak = {"lq_hdr_peak", "0", true};
/* HDR test pattern in place of the 3D view (GPU renderers): SDR white along
 * the top, then bands at 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4, 6 and 10 times it.
 * Where the bands stop changing is the display's real peak (lq_hdr_peak). */
static cvar_t lq_hdr_test = {"lq_hdr_test", "0"};
/* FitzQuake's names: GPU renderers blend model poses, and ease monsters'
 * 10 Hz steps, over each 0.1 s step. Software keeps the original stepping. */
static cvar_t r_lerpmodels = {"r_lerpmodels", "1", true};
static cvar_t r_lerpmove = {"r_lerpmove", "1", true};
/* Luminous: lights below lq_shadow_faint brightness, or below
 * lq_shadow_dominance of the brightest light at a point, cast no shadow
 * (0 turns either test off). */
static cvar_t lq_shadow_faint = {"lq_shadow_faint", "4", true};
static cvar_t lq_shadow_dominance = {"lq_shadow_dominance", "0.05", true};
/* A light at least this bright at a point filters its shadow with all 16
 * samples, from a quarter of it 8, below that 4 (0: always 16). */
static cvar_t lq_shadow_detail = {"lq_shadow_detail", "64", true};
void M_Print(int x, int y, char *text);
void M_DrawCharacter(int x, int y, int character);
void M_Menu_Options_f(void);
static void VideoMenuDraw(void);
static void VideoMenuKey(int key);
static void GPUOptions(void)
{
    if (!gpu) return;
    char error[256];
    int mips = gpu_mipmaps.value == 2 ? 2 : gpu_mipmaps.value == 1 ? 1 : 0;
    int aniso = gpu_anisotropy.value == 16 ? 16 : gpu_anisotropy.value == 8 ? 8 :
        gpu_anisotropy.value == 4 ? 4 : gpu_anisotropy.value == 2 ? 2 : 1;
    Cvar_SetValue("gpu_mipmaps", mips); Cvar_SetValue("gpu_anisotropy", aniso);
    if (!LQ_GPUTextureOptions(gpu, mips, aniso, error, sizeof(error))) {
        LQ_FallbackRenderer(error); return;
    }
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
    float choice = Cvar_VariableValue("lq_renderer");
    if (choice == 1 || choice == 2) {
        /* Classic and Luminous share the GPU; the map is rebuilt for the mode. */
        if (gpu && choice != gpu_mode) { LQ_RenderMapChanged(); gpu_mode = (int)choice; }
        if (gpu) { GPUOptions(); return; }
        SDL_DestroyTexture(texture); texture = NULL;
        SDL_DestroyRenderer(renderer); renderer = NULL;
        gpu = LQ_GPUCreate(window, 0, error, sizeof(error));
        if (!gpu) { LQ_FallbackRenderer(error); return; }
        GPUOptions(); gpu_mode = (int)choice;
        Con_DPrintf(choice == 2 ? "Luminous (experimental): runtime lights, soft shadows, bounce, probes and weapon lights.\n" :
            "Classic GPU enabled: GPU world, actors, brushes and effects.\n");
    } else if (gpu) LQ_FallbackRenderer("Software selected");
}

/* lq_capture <file.bmp>: saves the last frame shown, as --screenshot does at
 * exit, so one test run can take several views. */
static void Capture(void)
{
    if (Cmd_Argc() != 2) { Con_Printf("usage: lq_capture <file.bmp>\n"); return; }
    Con_Printf(LQ_SaveFrame(Cmd_Argv(1)) ? "Saved %s\n" : "Could not save %s\n", Cmd_Argv(1));
}
void VID_Init(unsigned char *palette)
{
    int width, height, win_width, win_height;
    SDL_WindowFlags flags;
    if (window) return;
    if (!cvars_registered) {
        LQ_JobsRegister();
        Cvar_RegisterVariable(&vid_width); Cvar_RegisterVariable(&vid_height);
        Cvar_RegisterVariable(&vid_fullscreen); Cvar_RegisterVariable(&vid_vsync);
        Cvar_RegisterVariable(&gpu_filter); Cvar_RegisterVariable(&gpu_msaa);
        Cvar_RegisterVariable(&gpu_mipmaps); Cvar_RegisterVariable(&gpu_anisotropy);
        Cvar_RegisterVariable(&r_wateralpha); Cvar_RegisterVariable(&r_fluorescent);
        Cvar_RegisterVariable(&lq_particles);
        Cvar_RegisterVariable(&lq_gamma); Cvar_RegisterVariable(&lq_hdr); Cvar_RegisterVariable(&lq_hdr_peak);
        LQ_RenderBridgeRegister();
        Cvar_RegisterVariable(&lq_hdr_test);
        Cvar_RegisterVariable(&r_lerpmodels); Cvar_RegisterVariable(&r_lerpmove);
        Cvar_RegisterVariable(&lq_shadow_faint); Cvar_RegisterVariable(&lq_shadow_dominance);
        Cvar_RegisterVariable(&lq_shadow_detail);
        Cmd_AddCommand("vid_restart", Restart);
        Cmd_AddCommand("vid_togglefullscreen", ToggleFullscreen);
        Cmd_AddCommand("lq_capture", Capture);
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
#ifdef __APPLE__
    /* Fullscreen without a macOS fullscreen Space. After the slide into a
     * Space, macOS sent no mouse motion at all until a click in the window
     * (logged October 19), so mouse look stayed dead after startup.
     * SDL_VIDEO_MAC_FULLSCREEN_SPACES=1 in the environment still wins. */
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
#endif
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
    {   /* Software: the palette passes through lq_gamma (identity at 2.2). */
        static uint32_t display[256];
        float exponent = isfinite(lq_gamma.value) && lq_gamma.value >= 1 && lq_gamma.value <= 3.5f ? lq_gamma.value / 2.2f : 1;
        for (i = 0; i < 256; ++i) {
            uint32_t c = d_8to24table[i];
            if (exponent == 1) { display[i] = c; continue; }
            display[i] = c & UINT32_C(0xff000000);
            for (int shift = 0; shift < 24; shift += 8) {
                float v = powf(((c >> shift) & 255) / 255.0f, exponent);
                display[i] |= (uint32_t)(v * 255 + 0.5f) << shift;
            }
        }
        for (i = 0; i < count; ++i) pixels[i] = display[vid.buffer[i]];
    }
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
    LQ_JobsShutdown();
    LQ_BackgroundShutdown();
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
static int video_filter, video_msaa, video_mipmaps, video_anisotropy, video_water, video_tubes, video_particles;
static int video_lerp, video_fog;
static int video_hdr, video_peak; /* lq_hdr; index into peak_levels */
static float video_peak_custom; /* a lq_hdr_peak set from the console that no preset matches; kept until changed here */
static const float peak_levels[] = {0, 1.5f, 2, 3, 4, 6, 8, 10}; /* 8: the replaced MacBook XDR panel (October 31) */
#define PEAK_LEVELS ((int)(sizeof(peak_levels) / sizeof(peak_levels[0])))
static const char *tube_cameras[] = {"eye", "film 24p", "NTSC 1/60", "NTSC 1/250", "PAL 25p"};
static const float water_levels[] = {1, 0.8f, 0.6f, 0.4f};
static const int anisotropy_levels[] = {1,2,4,8,16};
static float video_fov;
static qboolean video_menu_active;
static qboolean video_changed; /* a setting changed here: the config is saved on leaving */
static int video_pending_fullscreen;
void M_ConfirmVidRestart(void (*apply)(void));
static char video_packs[32][64];   /* texture packs found (LQ_TexturePacks) */
static const char *video_places[32];
static int video_pack_count;
/* The pack choices in order: auto (-2), none (-1), then each pack found. */
static int VideoPackIndex(void)
{
    const char *current = Cvar_VariableString("lq_texture_pack");
    if (!current[0] || !Q_strcasecmp((char *)current, "auto")) return -2;  /* empty is auto (an early default) */
    for (int i = 0; i < video_pack_count; ++i)
        if (!Q_strcasecmp(video_packs[i], (char *)current)) return i;
    return -1;  /* none, or a folder no longer there */
}
static void VideoMenuInit(void)
{
    if (video_menu_active) return;
    video_pack_count = LQ_TexturePacks(video_packs, video_places, 32);
    video_choice = 5;
    for (int i = 0; i < (int)(sizeof(resolutions) / sizeof(resolutions[0])); ++i)
        if (vid_width.value == resolutions[i][0] && vid_height.value == resolutions[i][1]) video_choice = i;
    video_fullscreen = vid_fullscreen.value != 0; video_vsync = vid_vsync.value != 0;
    video_filter = gpu_filter.value != 0;
    video_mipmaps = gpu_mipmaps.value == 2 ? 2 : gpu_mipmaps.value == 1 ? 1 : 0;
    video_anisotropy = 0;
    for (int i = 0; i < 5; ++i) if (gpu_anisotropy.value == anisotropy_levels[i]) video_anisotropy = i;
    video_water = 0;
    for (int i = 0; i < 4; ++i) if (fabsf(r_wateralpha.value - water_levels[i]) < 0.01f) video_water = i;
    video_tubes = r_fluorescent.value >= 0 && r_fluorescent.value < 5 ? (int)r_fluorescent.value : 1;
    video_particles = lq_particles.value != 0;
    video_lerp = r_lerpmodels.value != 0 || r_lerpmove.value != 0;
    video_fog = Cvar_VariableValue("lq_fog") != 0;
    video_hdr = lq_hdr.value != 0;
    video_peak = 0; video_peak_custom = 0;
    for (int i = 1; i < PEAK_LEVELS; ++i) if (fabsf(lq_hdr_peak.value - peak_levels[i]) < 0.01f) video_peak = i;
    if (!video_peak && isfinite(lq_hdr_peak.value) && lq_hdr_peak.value >= 1) video_peak_custom = lq_hdr_peak.value;
    video_msaa = gpu_msaa.value == 4 ? 2 : gpu_msaa.value == 2 ? 1 : 0;
    video_fov = Cvar_VariableValue("fov");
    if (!isfinite(video_fov) || video_fov < 10 || video_fov > 170) video_fov = 90;
    video_menu_active = true;
}
/* Everything here applies at once except the resolution and fullscreen,
 * which need the video system restarted: those ask first (M_ConfirmVidRestart)
 * and restart on Y. The resolution is browsed with left/right and asked for
 * on Enter, so stepping through the list doesn't restart at every step.
 * Settings the GPU holds (VSync, filtering, MSAA, mipmaps, anisotropy, HDR)
 * change over at a vid_restart queued for the next frame, as Apply did: the
 * key handler never touches the GPU or the swapchain itself. */
static int VideoCurrentResolution(void)
{
    for (int i = 0; i < (int)(sizeof(resolutions) / sizeof(resolutions[0])); ++i)
        if (vid_width.value == resolutions[i][0] && vid_height.value == resolutions[i][1]) return i;
    return -1;
}
static void VideoApplyResolution(void)
{
    Cvar_SetValue("vid_width", resolutions[video_choice][0]);
    Cvar_SetValue("vid_height", resolutions[video_choice][1]);
}
static void VideoApplyFullscreen(void)
{
    Cvar_SetValue("vid_fullscreen", video_pending_fullscreen);
    video_fullscreen = video_pending_fullscreen;
}
static qboolean video_resync; /* reread the menu's values once a queued restart has run */
/* Sets every setting from the menu's values. gpu: one the GPU holds, so a
 * vid_restart is queued to change it over, and the values are read back
 * afterwards, since the GPU may refuse some (MSAA it lacks, say). */
static void VideoApply(qboolean gpu)
{
    Cvar_SetValue("fov", video_fov);
    Cvar_SetValue("vid_vsync", video_vsync);
    Cvar_SetValue("gpu_filter", video_filter); Cvar_SetValue("gpu_msaa", video_msaa == 2 ? 4 : video_msaa == 1 ? 2 : 0);
    Cvar_SetValue("gpu_mipmaps", video_mipmaps); Cvar_SetValue("r_wateralpha", water_levels[video_water]);
    Cvar_SetValue("gpu_anisotropy", anisotropy_levels[video_anisotropy]);
    Cvar_SetValue("r_fluorescent", video_tubes);
    Cvar_SetValue("lq_particles", video_particles);
    Cvar_SetValue("r_lerpmodels", video_lerp); Cvar_SetValue("r_lerpmove", video_lerp);
    Cvar_SetValue("lq_fog", video_fog);
    Cvar_SetValue("lq_hdr", video_hdr); Cvar_SetValue("lq_hdr_peak", video_peak_custom ? video_peak_custom : peak_levels[video_peak]);
    if (gpu) { Cbuf_AddText("vid_restart\n"); video_resync = true; }
    video_changed = true;
}
static void VideoMenuDraw(void)
{
    if (video_resync) {  /* a resolution browsed but not yet asked for stays shown */
        int choice = video_choice;
        video_menu_active = false; VideoMenuInit(); video_choice = choice; video_resync = false;
    }
    VideoMenuInit();
    M_Print(88, 4, "VIDEO OPTIONS");
    M_Print(32, 20, "Resolution");
    M_Print(168, 20, va("%d x %d", resolutions[video_choice][0], resolutions[video_choice][1]));
    M_Print(32, 28, "Horizontal FOV"); M_Print(168, 28, va("%.0f deg", video_fov));
    M_Print(32, 36, "Fullscreen"); M_Print(168, 36, video_fullscreen ? "on" : "off");
    M_Print(32, 44, "VSync"); M_Print(168, 44, video_vsync ? "on" : "off");
    M_Print(32, 52, "GPU filtering"); M_Print(168, 52, video_filter ? "smooth" : "crisp");
    M_Print(32, 60, "GPU anti-alias"); M_Print(168, 60, video_msaa == 2 ? "4x MSAA" : video_msaa == 1 ? "2x MSAA" : "off");
    M_Print(32, 68, "GPU mipmaps"); M_Print(168, 68, video_mipmaps == 2 ? "trilinear" : video_mipmaps == 1 ? "nearest level" : "off");
    M_Print(32, 76, "GPU anisotropy"); M_Print(168, 76, video_anisotropy ? va("%dx", anisotropy_levels[video_anisotropy]) : "off");
    M_Print(32, 84, "GPU water"); M_Print(168, 84, video_water ? va("%d%% opaque", (int)(water_levels[video_water] * 100 + 0.5f)) : "opaque");
    M_Print(32, 92, "Tube camera"); M_Print(168, 92, tube_cameras[video_tubes]);
    M_Print(32, 100, "3D particles"); M_Print(168, 100, video_particles ? "on" : "off");
    M_Print(32, 108, "Smooth animation"); M_Print(168, 108, video_lerp ? "on" : "off");
    M_Print(32, 116, "Fog"); M_Print(168, 116, video_fog ? "map's" : "off");
    M_Print(32, 124, "HDR"); M_Print(168, 124, video_hdr ? "auto" : "off");
    M_Print(32, 132, "HDR peak"); M_Print(168, 132, video_peak ? va("%gx white", peak_levels[video_peak]) :
        video_peak_custom ? va("%gx white", video_peak_custom) : "as reported");
    /* Texture packs: both GPU renderers draw the pack's colour, glow, models
     * and skins (Luminous Options has what only Luminous does with it). */
    {
        int at = VideoPackIndex();
        M_Print(32, 140, "Texture pack");
        M_Print(168, 140, at == -2 ? (video_pack_count ? va("auto: %.11s", video_packs[0]) : "auto: none") :
            at < 0 ? "none" : va("%.17s", video_packs[at]));
        static const char *sizes[] = {"off", "full", "1/2", "1/4", "dynamic"};
        int size = (int)Cvar_VariableValue("lq_pack_textures");
        M_Print(32, 148, "Hi-res textures"); M_Print(168, 148, (char *)sizes[size >= 0 && size <= 4 ? size : 4]);
        M_Print(32, 156, "Pack brightness"); M_Print(168, 156, Cvar_VariableValue("lq_pack_brightness") ? "match" : "as made");
        M_Print(32, 164, "Pack models"); M_Print(168, 164, Cvar_VariableValue("lq_pack_models") ? "on" : "off");
        M_Print(32, 172, "Pack model skins"); M_Print(168, 172, Cvar_VariableValue("lq_pack_skins") ? "on" : "off");
    }
    M_DrawCharacter(16, 20 + video_cursor * 8, 12 + ((int)(realtime * 4) & 1));
    M_Print(32, 182, va("Current: %u x %u", vid.width* (LQ_UIActive() ? LQ_UIScale() : 1), vid.height* (LQ_UIActive() ? LQ_UIScale() : 1)));
    if (video_cursor == 0 && video_choice != VideoCurrentResolution())
        M_Print(32, 192, "Enter uses it; restarts video.");
    else if (video_cursor == 0 || video_cursor == 2)
        M_Print(32, 192, "Changing it restarts video.");
    else if (video_cursor == 7 && !video_mipmaps)
        M_Print(32, 192, "Anisotropy needs mipmaps enabled.");
    else if (video_cursor == 8 && video_water)
        M_Print(32, 192, "Unvised maps draw every room.");
    else if (video_cursor == 9)
        M_Print(32, 192, "Luminous: how fluorescents film.");
    else if (video_cursor == 10)
        M_Print(32, 192, "Luminous: solid chunks, drops.");
    else if (video_cursor == 11)
        M_Print(32, 192, "GPU: blend poses, ease steps.");
    else if (video_cursor == 12)
        M_Print(32, 192, "GPU: fog from maps that set it.");
    else if (video_cursor == 13)
        M_Print(32, 192, "GPU: HDR when the display is.");
    else if (video_cursor == 14)
        M_Print(32, 192, "Cap highlights if they clip.");
    else if (video_cursor == 15) {
        int at = VideoPackIndex();
        M_Print(32, 192, !video_pack_count ? "No pack: put a .pk3 beside the app." :
            at == -1 ? "Original textures only." : "A new pack reloads the map.");
    } else if (video_cursor == 16)
        M_Print(32, 192, "Dynamic fits memory; 1/2, 1/4 compress.");
    else if (video_cursor == 17)
        M_Print(32, 192, "Match: each at its original's level.");
    else if (video_cursor == 18)
        M_Print(32, 192, "GPU: the pack's own models.");
    else if (video_cursor == 19)
        M_Print(32, 192, "GPU: pack skins on original models.");
    else M_Print(32, 192, "Changes apply at once.");
}
static void VideoMenuKey(int key)
{
    VideoMenuInit();
    if (key == K_ESCAPE) {
        video_menu_active = false;
        if (video_changed) { Host_WriteConfiguration(); video_changed = false; }
        M_Menu_Options_f(); return;
    }
    if (key == K_UPARROW || key == K_DOWNARROW) {
        int current = VideoCurrentResolution();
        if (current >= 0) video_choice = current;  /* a resolution browsed and left goes back */
        video_cursor = (video_cursor + (key == K_UPARROW ? 19 : 1)) % 20;
        return;
    }
    if (key != K_LEFTARROW && key != K_RIGHTARROW && key != K_ENTER) return;
    if (video_cursor == 0) {
        int count = sizeof(resolutions) / sizeof(resolutions[0]);
        if (key != K_ENTER) video_choice = (video_choice + (key == K_LEFTARROW ? -1 : 1) + count) % count;
        else if (video_choice != VideoCurrentResolution()) M_ConfirmVidRestart(VideoApplyResolution);
        return;
    }
    if (video_cursor == 2) {
        video_pending_fullscreen = !video_fullscreen;
        M_ConfirmVidRestart(VideoApplyFullscreen);
        return;
    }
    if (video_cursor >= 15) {  /* texture packs: their settings change directly, no restart */
        if (video_cursor == 15) {
            int at = VideoPackIndex() + (key == K_LEFTARROW ? -1 : 1);
            if (at < -2) at = video_pack_count - 1;
            if (at >= video_pack_count) at = -2;
            Cvar_Set("lq_texture_pack", at == -2 ? "auto" : at == -1 ? "none" : video_packs[at]);
        } else if (video_cursor == 16) {  /* off, full, 1/2, 1/4, dynamic: a new size reloads the map */
            int size = (int)Cvar_VariableValue("lq_pack_textures");
            if (size < 0 || size > 4) size = 4;
            Cvar_SetValue("lq_pack_textures", (size + (key == K_LEFTARROW ? 4 : 1)) % 5);
        } else {
            static const char *switches[] = {"lq_pack_brightness", "lq_pack_models", "lq_pack_skins"};
            const char *name = switches[video_cursor - 17];
            Cvar_SetValue((char *)name, Cvar_VariableValue((char *)name) != 0 ? 0 : 1);
        }
        video_changed = true;
        return;
    }
    if (video_cursor == 1) {
        video_fov += key == K_LEFTARROW ? -5 : 5;
        if (video_fov < 10) video_fov = 10;
        if (video_fov > 170) video_fov = 170;
    } else if (video_cursor == 3) video_vsync = !video_vsync;
    else if (video_cursor == 4) video_filter = !video_filter;
    else if (video_cursor == 5) video_msaa = (video_msaa + (key == K_LEFTARROW ? 2 : 1)) % 3;
    else if (video_cursor == 6) video_mipmaps = (video_mipmaps + (key == K_LEFTARROW ? 2 : 1)) % 3;
    else if (video_cursor == 7) video_anisotropy = (video_anisotropy + (key == K_LEFTARROW ? 4 : 1)) % 5;
    else if (video_cursor == 8) video_water = (video_water + (key == K_LEFTARROW ? 3 : 1)) % 4;
    else if (video_cursor == 9) video_tubes = (video_tubes + (key == K_LEFTARROW ? 4 : 1)) % 5;
    else if (video_cursor == 10) video_particles = !video_particles;
    else if (video_cursor == 11) video_lerp = !video_lerp;
    else if (video_cursor == 12) video_fog = !video_fog;
    else if (video_cursor == 13) video_hdr = !video_hdr;
    else if (video_cursor == 14) { video_peak = (video_peak + (key == K_LEFTARROW ? PEAK_LEVELS - 1 : 1)) % PEAK_LEVELS; video_peak_custom = 0; }
    VideoApply((video_cursor >= 3 && video_cursor <= 7) || video_cursor >= 13);
}
