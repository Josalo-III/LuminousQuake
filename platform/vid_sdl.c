/* LuminousQuake SDL presentation of Quake's software framebuffer.
 * GPL-2.0-or-later; see gnu.txt. */
#include "quakedef.h"
#include "d_local.h"
#include "sdl_backend.h"

viddef_t vid;
unsigned short d_8to16table[256];
unsigned d_8to24table[256];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t *pixels;
static byte *surface_cache;
static byte direct_saved[48 * 48];
static SDL_Rect direct_rect;
static qboolean direct_active, cvars_registered;
static cvar_t vid_width = {"vid_width", "640", true};
static cvar_t vid_height = {"vid_height", "480", true};
static cvar_t vid_fullscreen = {"vid_fullscreen", "0", true};
static cvar_t vid_vsync = {"vid_vsync", "0", true};

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
    count = (size_t)width * height;
    cache_size = D_SurfaceCacheForRes(width, height);
    if (cache_size < 1024 || cache_size > 128 * 1024 * 1024)
        Sys_Error("Invalid software surface-cache size");
    buffer = calloc(count, 1);
    zbuffer = calloc(count, sizeof(*zbuffer));
    converted = calloc(count, sizeof(*converted));
    cache = malloc(cache_size);
    next = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!buffer || !zbuffer || !converted || !cache || !next) {
        free(buffer); free(zbuffer); free(converted); free(cache);
        SDL_DestroyTexture(next);
        Sys_Error("Cannot create software framebuffer: %s", SDL_GetError());
    }
    if (surface_cache) D_FlushCaches();
    free(vid.buffer); free(d_pzbuffer); free(pixels); free(surface_cache);
    SDL_DestroyTexture(texture);
    vid.buffer = vid.conbuffer = buffer;
    d_pzbuffer = zbuffer; pixels = converted; surface_cache = cache; texture = next;
    vid.width = vid.conwidth = vid.rowbytes = vid.conrowbytes = width;
    vid.height = vid.conheight = height;
    vid.maxwarpwidth = WARP_WIDTH; vid.maxwarpheight = WARP_HEIGHT;
    vid.aspect = (float)height / width * (320.0f / 240.0f);
    vid.numpages = 1; vid.recalc_refdef = true;
    vid.colormap = host_colormap;
    vid.fullbright = host_colormap ? 256 - LittleLong(*((int *)host_colormap + 2048)) : 256;
    D_InitCaches(surface_cache, cache_size);
    SDL_RenderSetLogicalSize(renderer, width, height);
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
    if (SDL_SetWindowFullscreen(window, vid_fullscreen.value ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0)) {
        Con_Printf("Fullscreen change failed: %s\n", SDL_GetError());
        Cvar_SetValue("vid_fullscreen", !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP));
    }
}
static void ToggleFullscreen(void)
{
    Cvar_SetValue("vid_fullscreen", !vid_fullscreen.value);
    Fullscreen();
}
static void Restart(void)
{
    if (!window) return;
    if (!isfinite(vid_width.value) || !isfinite(vid_height.value) ||
        vid_width.value < 320 || vid_width.value > MAXWIDTH ||
        vid_height.value < 200 || vid_height.value > MAXHEIGHT)
        Sys_Error("Invalid vid_width/vid_height for software renderer");
    Framebuffer((int)vid_width.value, (int)vid_height.value);
    Fullscreen();
    if (SDL_RenderSetVSync(renderer, vid_vsync.value != 0))
        Con_Printf("VSync setting unavailable: %s\n", SDL_GetError());
    LQ_UpdateCapture();
}

void VID_Init(unsigned char *palette)
{
    SDL_RendererInfo info;
    int width, height, win_width, win_height;
    Uint32 flags;
    if (window) return;
    if (!cvars_registered) {
        Cvar_RegisterVariable(&vid_width); Cvar_RegisterVariable(&vid_height);
        Cvar_RegisterVariable(&vid_fullscreen); Cvar_RegisterVariable(&vid_vsync);
        Cmd_AddCommand("vid_restart", Restart);
        Cmd_AddCommand("vid_togglefullscreen", ToggleFullscreen);
        cvars_registered = true;
    }
    width = ArgumentDimension("-width", (int)vid_width.value, 320, MAXWIDTH);
    height = ArgumentDimension("-height", (int)vid_height.value, 200, MAXHEIGHT);
    win_width = ArgumentDimension("-window-width", 960, 320, 16384);
    win_height = ArgumentDimension("-window-height", 720, 200, 16384);
    if (COM_CheckParm("-fullscreen")) Cvar_SetValue("vid_fullscreen", 1);
    if (COM_CheckParm("-window")) Cvar_SetValue("vid_fullscreen", 0);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SCALING, "0");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) Sys_Error("SDL video initialization failed: %s", SDL_GetError());
    flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (vid_fullscreen.value) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window = SDL_CreateWindow("LuminousQuake", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_width, win_height, flags);
    if (!window) Sys_Error("SDL window creation failed: %s", SDL_GetError());
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED |
        (vid_vsync.value ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) Sys_Error("SDL renderer creation failed: %s", SDL_GetError());
    SDL_GetRendererInfo(renderer, &info);
    Con_Printf("SDL presentation: %s\n", info.name);
    Cvar_SetValue("vid_width", width); Cvar_SetValue("vid_height", height);
    Framebuffer(width, height);
    VID_SetPalette(palette);
    LQ_SetFocus((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    SDL_StartTextInput();
}
void VID_Update(vrect_t *rects)
{
    size_t i, count = (size_t)vid.width * vid.height;
    (void)rects;
    if (!renderer) return;
    for (i = 0; i < count; ++i) pixels[i] = d_8to24table[vid.buffer[i]];
    if (SDL_UpdateTexture(texture, NULL, pixels, (int)vid.width * sizeof(*pixels)))
        Sys_Error("SDL texture update failed: %s", SDL_GetError());
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}
qboolean LQ_SaveFrame(const char *path)
{
    SDL_Surface *surface;
    int width, height, result;
    if (!renderer || SDL_GetRendererOutputSize(renderer, &width, &height)) return false;
    surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return false;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, NULL, NULL);
    result = SDL_RenderReadPixels(renderer, NULL, surface->format->format, surface->pixels, surface->pitch);
    if (!result) result = SDL_SaveBMP(surface, path);
    SDL_FreeSurface(surface); SDL_RenderPresent(renderer);
    return result == 0;
}
void VID_Shutdown(void)
{
    if (!window && !renderer) return;
    IN_ClearStates();
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_StopTextInput();
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
