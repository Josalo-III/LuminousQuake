/* Asset-free platform demonstration and backend integration checks.
 * New LuminousQuake code, GPL-2.0-or-later. */
#include "quakedef.h"
#include "d_local.h"
#include "sdl_backend.h"
#include "render_bridge.h"
#include "jobs.h"
#include "performance.h"
extern cvar_t scr_fov;
#include <stdalign.h>
#include <stdatomic.h>

surfcache_t *D_SCAlloc(int width, int size);
extern byte *draw_chars;
extern char key_lines[32][256];
extern int edit_line, key_linepos;
extern kbutton_t in_forward, in_mlook;
extern int sound_started, soundtime;
static byte font[128 * 128];
static sfx_t demo_sound;
static float demo_depth[8];

typedef struct { unsigned *values; atomic_uint *visits; } job_test_context;
static void TestJobRange(void *opaque_context, size_t begin, size_t end)
{
    job_test_context *context = opaque_context;
    for (size_t i = begin; i < end; ++i) {
        context->values[i] = (unsigned)(i ^ 0x5a5a);
        if (context->visits) atomic_fetch_add(&context->visits[i], 1);
    }
}

#define VERIFY(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL SDL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

static atomic_int test_cpu_load;
static bool TestCpuLoad(void *unused, double *busy)
{
    (void)unused;
    *busy = atomic_load(&test_cpu_load) / 100.0;
    return true;
}

static int TestResourceJobs(void)
{
    unsigned values[1003];
    atomic_uint visits[1003];
    for (size_t i = 0; i < 1003; ++i) atomic_init(&visits[i], 0);
    job_test_context context = {values, visits};
    atomic_store(&test_cpu_load, 20);
    VERIFY(LQ_PerformanceStart(0, TestCpuLoad, NULL));
    Cvar_SetValue("lq_workers", -1);
    unsigned full = LQ_JobsWorkerCount();
    for (unsigned phase = 0; phase < 3; ++phase) {
        bool constrained = phase == 1;
        atomic_store(&test_cpu_load, constrained ? 95 : 20);
        lq_perf_state wanted = constrained ? LQ_PERF_CONSTRAINED : LQ_PERF_HEADROOM;
        Uint64 end = SDL_GetTicks() + 3500;
        while (LQ_PerformanceRead().state != wanted && SDL_GetTicks() < end) SDL_Delay(2);
        VERIFY(LQ_PerformanceRead().state == wanted);
        unsigned desired = LQ_JobsDesiredWorkerCount();
        VERIFY(LQ_JobsWorkerBudget() == (constrained && desired > 1 ? (desired + 1) / 2 : desired));
        /* Completion must still cover every row exactly once, with no pool
         * recreation when the shared resource state changes the active cohort. */
        for (unsigned repeat = 0; repeat < 20; ++repeat) {
            memset(values, 0, sizeof(values));
            for (size_t i = 0; i < 1003; ++i) atomic_store(&visits[i], 0);
            LQ_JobsRunRange(1003, repeat % 2 ? 1 : 37, TestJobRange, &context);
            VERIFY(LQ_JobsWorkerCount() == full);
            for (size_t i = 0; i < 1003; ++i) {
                VERIFY(values[i] == (unsigned)(i ^ 0x5a5a));
                VERIFY(atomic_load(&visits[i]) == 1);
            }
        }
        if (constrained) {
            Cvar_SetValue("lq_workers", 4);
            VERIFY(LQ_JobsWorkerBudget() == 4);
            Cvar_SetValue("lq_workers", -1);
        }
    }
    LQ_PerformanceStop();
    puts("PASS shared CPU pressure, automatic worker budgets, overrides, and stable job completion");
    return 0;
}

static void Palette(byte *palette)
{
    int i;
    for (i = 0; i < 256; ++i) {
        palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] = (byte)i;
        if (i >= 64 && i < 128) {
            palette[i * 3] = 8 + (i - 64) * 2;
            palette[i * 3 + 1] = 20 + (i - 64) * 2;
            palette[i * 3 + 2] = 40 + (i - 64) * 3;
        }
        if (i >= 128 && i < 192) {
            palette[i * 3] = 80 + (i - 128) * 2;
            palette[i * 3 + 1] = 35 + (i - 128) * 2;
            palette[i * 3 + 2] = 12 + (i - 128);
        }
    }
}
static void Font(void)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-./";
    static const byte glyph[][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}, {0,4,4,0,4,4,0}, {0,0,0,31,0,0,0},
        {0,0,0,0,0,4,4}, {1,2,2,4,8,8,16}
    };
    size_t i;
    int x, y, code;
    for (i = 0; i < sizeof(alphabet) - 1; ++i)
        for (code = alphabet[i]; code <= alphabet[i] + 128; code += 128)
            for (y = 0; y < 7; ++y) for (x = 0; x < 5; ++x)
                if (glyph[i][y] & (1 << (4 - x)))
                    font[((code >> 4) * 8 + y) * 128 + (code & 15) * 8 + x + 1] = 235;
    draw_chars = font;
}
static void Text(int x, int y, const char *text)
{
    char clipped[128];
    size_t length = strlen(text), limit = (vid.width - x) / 8;
    if (length > limit) length = limit;
    if (length >= sizeof(clipped)) length = sizeof(clipped) - 1;
    memcpy(clipped, text, length); clipped[length] = 0;
    Draw_String(x, y, clipped);
}
static float Edge(float ax, float ay, float bx, float by, float x, float y)
{ return (x - ax) * (by - ay) - (y - ay) * (bx - ax); }
static void Triangle(const float *a, const float *b, const float *c, byte color)
{
    int x, y, minx = (int)fminf(a[0], fminf(b[0], c[0]));
    int maxx = (int)fmaxf(a[0], fmaxf(b[0], c[0]));
    int miny = (int)fminf(a[1], fminf(b[1], c[1]));
    int maxy = (int)fmaxf(a[1], fmaxf(b[1], c[1]));
    float orientation = Edge(a[0], a[1], b[0], b[1], c[0], c[1]);
    if (minx < 0) minx = 0; if (miny < 0) miny = 0;
    if (maxx >= (int)vid.width) maxx = vid.width - 1;
    if (maxy >= (int)vid.height) maxy = vid.height - 1;
    for (y = miny; y <= maxy; ++y) for (x = minx; x <= maxx; ++x)
        if (Edge(a[0], a[1], b[0], b[1], x + 0.5f, y + 0.5f) * orientation >= 0 &&
            Edge(b[0], b[1], c[0], c[1], x + 0.5f, y + 0.5f) * orientation >= 0 &&
            Edge(c[0], c[1], a[0], a[1], x + 0.5f, y + 0.5f) * orientation >= 0)
            vid.buffer[y * vid.rowbytes + x] = color;
}
static void Scene(double elapsed)
{
    static const int faces[12][3] = {{0,1,3},{0,3,2},{4,6,7},{4,7,5},
        {0,4,5},{0,5,1},{2,3,7},{2,7,6},{0,2,6},{0,6,4},{1,5,7},{1,7,3}};
    float projected[8][2], face_depth[12]; int order[12];
    float yaw = (float)elapsed * 0.2f + cl.viewangles[YAW] * (float)M_PI / 180;
    float pitch = 0.35f + cl.viewangles[PITCH] * (float)M_PI / 180;
    int x, y, i, j;
    char status[96];
    for (y = 0; y < (int)vid.height; ++y) {
        for (x = 0; x < (int)vid.width; ++x) {
            byte color = (byte)(66 + y * 20 / vid.height);
            if (y > (int)vid.height * 3 / 5) {
                float depth = vid.height * 0.6f / (y - vid.height * 0.6f + 1);
                int tile_x = (int)((x - vid.width * 0.5f) * depth / 55);
                int tile_y = (int)(depth * 8);
                color = ((tile_x ^ tile_y) & 1) ? 32 : 43;
            }
            vid.buffer[y * vid.rowbytes + x] = color;
        }
    }
    for (i = 0; i < 8; ++i) {
        float xx = i & 1 ? 1 : -1, yy = i & 2 ? 1 : -1, zz = i & 4 ? 1 : -1;
        float a = xx * cosf(yaw) + zz * sinf(yaw);
        float b = zz * cosf(yaw) - xx * sinf(yaw);
        float vertical = yy * cosf(pitch) - b * sinf(pitch);
        float depth = b * cosf(pitch) + yy * sinf(pitch) + 5;
        demo_depth[i] = depth;
        projected[i][0] = vid.width * 0.5f + a / depth * vid.height * 1.3f;
        projected[i][1] = vid.height * 0.5f - vertical / depth * vid.height * 1.3f;
    }
    for (i = 0; i < 12; ++i) {
        order[i] = i;
        face_depth[i] = demo_depth[faces[i][0]] + demo_depth[faces[i][1]] + demo_depth[faces[i][2]];
    }
    for (i = 0; i < 12; ++i) for (j = i + 1; j < 12; ++j)
        if (face_depth[order[i]] < face_depth[order[j]]) { int temp = order[i]; order[i] = order[j]; order[j] = temp; }
    for (i = 0; i < 12; ++i) {
        int face = order[i];
        Triangle(projected[faces[face][0]], projected[faces[face][1]], projected[faces[face][2]],
            (byte)(130 + face / 2 * 9));
    }
    Draw_Fill(0, 0, vid.width, 54, 18);
    Text(16, 12, "LUMINOUSQUAKE");
    Text(16, 30, "PHASE 2 / SDL PLATFORM DEMO / GENERATED ASSETS");
    Draw_Fill(0, vid.height - 58, vid.width, 58, 18);
    snprintf(status, sizeof(status), "%ux%u FRAMEBUFFER / %s AUDIO / %s",
        vid.width, vid.height, LQ_AudioReady() ? "SDL" : "NO", LQ_HasFocus() ? "FOCUSED" : "UNFOCUSED");
    Text(16, vid.height - 46, status);
    Text(16, vid.height - 28, "MOUSE LOOK / F11 FULLSCREEN / ESC EXIT");
    if (key_dest == key_console) Text(16, vid.height - 12, key_lines[edit_line]);
    else Text(16, vid.height - 12, "PRESS T FOR TEST TONE / GRAVE FOR CONSOLE");
    VID_Update(NULL);
}
static void Tone(void)
{
    sfxcache_t *cache;
    int i, samples;
    if (!sound_started || !shm) return;
    samples = shm->speed / 2;
    if (!demo_sound.cache.data) {
        strcpy(demo_sound.name, "__platform_tone__");
        cache = Cache_Alloc(&demo_sound.cache, sizeof(*cache) + samples * sizeof(short), "demo tone");
        cache->length = samples; cache->loopstart = -1; cache->speed = shm->speed;
        cache->width = 2; cache->stereo = 0;
        for (i = 0; i < samples; ++i) ((short *)cache->data)[i] =
            (short)(sinf((float)i * 440 * 2 * (float)M_PI / shm->speed) * 6000);
    }
    S_StartSound(cl.viewentity, 0, &demo_sound, vec3_origin, 0.5f, 0);
}
static void KeyEvent(SDL_Event *event, Uint32 type, SDL_Scancode scan, SDL_Keycode symbol)
{
    memset(event, 0, sizeof(*event)); event->type = type;
    event->key.windowID = SDL_GetWindowID(LQ_SDLWindow());
    event->key.scancode = scan; event->key.key = symbol;
    LQ_HandleEvent(event);
}
static int TestPlatform(qboolean no_audio)
{
    SDL_Event event;
    int saved_width = vid.width, saved_height = vid.height;
    size_t bytes; byte *output;
    extern qboolean keydown[256];
    if (COM_CheckParm("--test-startup-focus")) {
        Uint64 deadline = SDL_GetTicks() + 1000;
        do {
            LQ_PumpEvents();
            if (SDL_GetKeyboardFocus() == LQ_SDLWindow() && LQ_HasFocus()) break;
            SDL_Delay(10);
        } while (SDL_GetTicks() < deadline);
        VERIFY(SDL_GetKeyboardFocus() == LQ_SDLWindow());
        VERIFY(LQ_HasFocus());
        puts("PASS native startup keyboard focus (no synthetic focus event)");
    }
    LQ_PumpEvents(); LQ_SetFocus(true);
    {
        unsigned values[1003] = {0};
        job_test_context context = {values};
        Cvar_SetValue("lq_workers", 4);
        LQ_JobsRunRange(1003, 37, TestJobRange, &context);
        VERIFY(LQ_JobsWorkerCount() > 0 && LQ_JobsWorkerCount() <= 4);
        for (size_t i = 0; i < 1003; ++i) VERIFY(values[i] == (unsigned)(i ^ 0x5a5a));
        memset(values, 0, sizeof(values));
        LQ_JobsRunRange(10, 37, TestJobRange, &context);
        for (size_t i = 0; i < 10; ++i) VERIFY(values[i] == (unsigned)(i ^ 0x5a5a));
        for (size_t i = 10; i < 1003; ++i) VERIFY(values[i] == 0);
        memset(values, 0, sizeof(values));
        Cvar_SetValue("lq_workers", 0);
        LQ_JobsRunRange(1003, 37, TestJobRange, &context);
        VERIFY(LQ_JobsWorkerCount() == 0);
        for (size_t i = 0; i < 1003; ++i) VERIFY(values[i] == (unsigned)(i ^ 0x5a5a));
        Cvar_SetValue("lq_workers", -1);
        puts("PASS parallel and forced-serial range jobs");
    }
    VERIFY(TestResourceJobs() == 0);
    { float old_yaw = cl.viewangles[YAW];
      SDL_Event motion;
      memset(&motion, 0, sizeof(motion)); motion.type = SDL_EVENT_MOUSE_MOTION;
      motion.motion.windowID = SDL_GetWindowID(LQ_SDLWindow());
      motion.motion.xrel = 100; motion.motion.yrel = -10000;
      usercmd_t movement = {0};
      if (SDL_GetWindowRelativeMouseMode(LQ_SDLWindow())) {
          VERIFY(SDL_PushEvent(&motion) == 1); LQ_PumpEvents(); IN_Move(&movement);
      } else {
          puts("SKIP OS relative capture: driver unavailable; checking Quake mouse-delta math");
          LQ_TestMouseDelta(100, -10000, &movement);
      }
      VERIFY(fabsf(cl.viewangles[YAW] - old_yaw + 100 * sensitivity.value * m_yaw.value) < 0.001f);
      VERIFY(cl.viewangles[PITCH] == -70); }
    {
        usercmd_t movement = {0};
        VERIFY(freelook.archive && freelook.value == 1);
        Cvar_SetValue("freelook", 0);
        cl.viewangles[PITCH] = 0;
        LQ_TestMouseDelta(0, 20, &movement);
        VERIFY(cl.viewangles[PITCH] == 0 && movement.forwardmove != 0);
        Cmd_ExecuteString("+mlook", src_command);
        LQ_TestMouseDelta(0, 20, &movement);
        VERIFY(cl.viewangles[PITCH] != 0);
        Cmd_ExecuteString("-mlook", src_command);
        cl.viewangles[PITCH] = 0;
        Cvar_SetValue("freelook", 1);
        LQ_TestMouseDelta(0, 20, &movement);
        VERIFY(cl.viewangles[PITCH] != 0 && !(in_mlook.state & 1));
        puts("PASS saved mouselook preference and held +mlook override");
    }
    Key_SetBinding('w', "+forward");
    KeyEvent(&event, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W, SDLK_W); Cbuf_Execute();
    VERIFY(in_forward.state & 1);
    memset(&event, 0, sizeof(event)); event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    event.window.windowID = SDL_GetWindowID(LQ_SDLWindow());
    LQ_HandleEvent(&event); Cbuf_Execute();
    VERIFY(!(in_forward.state & 1)); VERIFY(!keydown['w']); VERIFY(!SDL_GetWindowRelativeMouseMode(LQ_SDLWindow()));
    event.type = SDL_EVENT_WINDOW_FOCUS_GAINED; LQ_HandleEvent(&event);
    key_dest = key_console; LQ_UpdateCapture();
    VERIFY(SDL_TextInputActive(LQ_SDLWindow()));
    KeyEvent(&event, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDLK_A);
    memset(&event, 0, sizeof(event)); event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = "A!"; LQ_HandleEvent(&event);
    VERIFY(!strcmp(key_lines[edit_line] + 1, "A!"));
    KeyEvent(&event, SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDLK_A);
    KeyEvent(&event, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT);
    KeyEvent(&event, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RSHIFT, SDLK_RSHIFT);
    KeyEvent(&event, SDL_EVENT_KEY_UP, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT);
    VERIFY(keydown[K_SHIFT]);
    KeyEvent(&event, SDL_EVENT_KEY_UP, SDL_SCANCODE_RSHIFT, SDLK_RSHIFT); VERIFY(!keydown[K_SHIFT]);
    key_dest = key_game;
    LQ_UpdateCapture(); VERIFY(!SDL_TextInputActive(LQ_SDLWindow()));
    SDL_SetWindowSize(LQ_SDLWindow(), 800, 600); LQ_PumpEvents();
    VERIFY(vid.width == (unsigned)saved_width && vid.height == (unsigned)saved_height);
    if (COM_CheckParm("--test-fullscreen")) {
        Cbuf_AddText("vid_togglefullscreen\n"); Cbuf_Execute(); LQ_PumpEvents();
        VERIFY(SDL_GetWindowFlags(LQ_SDLWindow()) & SDL_WINDOW_FULLSCREEN);
        Cbuf_AddText("vid_togglefullscreen\n"); Cbuf_Execute(); LQ_PumpEvents();
        VERIFY(!(SDL_GetWindowFlags(LQ_SDLWindow()) & SDL_WINDOW_FULLSCREEN));
        puts("PASS native fullscreen enter/exit");
    }
    memset(vid.buffer, 0, vid.width * vid.height);
    Draw_Fill(0, 0, vid.width, vid.height, 150); VID_Update(NULL);
    { SDL_Surface *capture; char path[128];
      snprintf(path, sizeof(path), "luminousquake-sdl-%llu.bmp", (unsigned long long)SDL_GetTicks());
      VERIFY(LQ_SaveFrame(path)); capture = SDL_LoadBMP(path); VERIFY(capture != NULL);
      SDL_Surface *rgba = SDL_ConvertSurface(capture, SDL_PIXELFORMAT_ARGB8888);
      VERIFY(rgba != NULL);
      VERIFY(((uint32_t *)rgba->pixels)[rgba->w / 2 + rgba->h / 2 * (rgba->pitch / 4)] == d_8to24table[150]);
      SDL_DestroySurface(rgba); SDL_DestroySurface(capture); remove(path); }
    VERIFY(d_8to24table[150] == (UINT32_C(0xff000000) | (124u << 16) | (79u << 8) | 34u));
    { byte palette[768]; Palette(palette); palette[450] = 5; palette[451] = 7; palette[452] = 9;
      VID_ShiftPalette(palette); VID_Update(NULL);
      VERIFY(d_8to24table[150] == UINT32_C(0xff050709)); VID_SetPalette(palette); }
    Cvar_SetValue("vid_width", 800); Cvar_SetValue("vid_height", 600);
    Cbuf_AddText("vid_restart\n"); Cbuf_Execute();
    VERIFY(vid.width == 800 && vid.height == 600);
    VERIFY((uintptr_t)D_SCAlloc(8, 17) % _Alignof(surfcache_t) == 0);
    VERIFY((uintptr_t)D_SCAlloc(8, 19) % _Alignof(surfcache_t) == 0);
    if (no_audio) {
        VERIFY(!LQ_AudioReady());
        S_Update(vec3_origin, vec3_origin, vec3_origin, vec3_origin);
    } else {
        uint64_t before;
        short *samples;
        VERIFY(LQ_AudioReady());
        LQ_AudioFocus(false);
        SNDDMA_BeginPainting();
        bytes = (size_t)shm->samples * sizeof(short);
        output = malloc(bytes + 16); VERIFY(output != NULL);
        samples = (short *)shm->buffer;
        for (size_t i = 0; i < bytes / sizeof(short); ++i) samples[i] = 1000;
        before = LQ_AudioFrames();
        LQ_AudioConsume(output, (int)bytes + 16);
        VERIFY(((short *)output)[3] == 1000);
        for (size_t i = bytes; i < bytes + 16; ++i) VERIFY(output[i] == 0);
        VERIFY(LQ_AudioFrames() - before == bytes / 4 + 4);
        VERIFY(SNDDMA_GetDMAPos() >= 0 && SNDDMA_GetDMAPos() < shm->samples);
        SNDDMA_Submit(); free(output);
        /* Exercise the real C mixer into the shared audio ring. */
        { sfx_t sound = {0}; sfxcache_t *cache;
          strcpy(sound.name, "__mixer_fixture__");
          cache = Cache_Alloc(&sound.cache, sizeof(*cache) + 64 * sizeof(short), "mixer test");
          cache->length = 64; cache->loopstart = -1; cache->speed = shm->speed;
          cache->width = 2; cache->stereo = 0;
          for (int i = 0; i < 64; ++i) ((short *)cache->data)[i] = i & 1 ? -1000 : 1000;
          S_ClearBuffer(); memset(channels, 0, sizeof(channels)); total_channels = 1;
          channels[0].sfx = &sound; channels[0].leftvol = 255; channels[0].rightvol = 128;
          channels[0].end = 64; paintedtime = 0;
          SNDDMA_BeginPainting(); S_PaintChannels(32);
          VERIFY(((short *)shm->buffer)[0] == (((1000 * 255) >> 8) * (int)(volume.value * 256) >> 8));
          VERIFY(((short *)shm->buffer)[1] == (((1000 * 128) >> 8) * (int)(volume.value * 256) >> 8));
          SNDDMA_Submit(); channels[0].sfx = NULL; Cache_Free(&sound.cache); }
        S_Shutdown(); VERIFY(!LQ_AudioReady()); S_Startup(); VERIFY(LQ_AudioReady());
        LQ_AudioFocus(true); before = LQ_AudioFrames(); SDL_Delay(70);
        VERIFY(LQ_AudioFrames() > before);
    }
    memset(&event, 0, sizeof(event)); event.type = SDL_EVENT_QUIT;
    /* The menu stages resolution changes; Apply must use the real restart path. */
    VERIFY(vid_menudrawfn && vid_menukeyfn);
    vid_menudrawfn();
    int old_width = vid.width, old_height = vid.height;
    vid_menukeyfn(K_RIGHTARROW);
    VERIFY(vid.width == (unsigned)old_width && vid.height == (unsigned)old_height);
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_RIGHTARROW);
    VERIFY(Cvar_VariableValue("fov") == 90);
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_DOWNARROW);
    vid_menukeyfn(K_RIGHTARROW); /* smooth filtering, staged */
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_RIGHTARROW); /* 2x MSAA */
    VERIFY(Cvar_VariableValue("gpu_filter") == 0 && Cvar_VariableValue("gpu_msaa") == 0);
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_RIGHTARROW); vid_menukeyfn(K_RIGHTARROW); /* trilinear */
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_LEFTARROW); /* 16x anisotropy */
    VERIFY(Cvar_VariableValue("gpu_mipmaps") == 0 && Cvar_VariableValue("gpu_anisotropy") == 1);
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_LEFTARROW); /* 40% opaque liquids */
    VERIFY(Cvar_VariableValue("r_wateralpha") == 1);
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_RIGHTARROW); /* NTSC tube camera */
    VERIFY(Cvar_VariableValue("r_fluorescent") == 1);
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_RIGHTARROW); /* 3D particles */
    VERIFY(Cvar_VariableValue("lq_particles") == 0);
    vid_menukeyfn(K_DOWNARROW); vid_menukeyfn(K_RIGHTARROW); /* smooth animation off */
    VERIFY(Cvar_VariableValue("r_lerpmodels") == 1 && Cvar_VariableValue("r_lerpmove") == 1);
    vid_menukeyfn(K_DOWNARROW);
    vid_menukeyfn(K_ENTER); Cbuf_Execute();
    VERIFY(fabsf(Cvar_VariableValue("r_wateralpha") - 0.4f) < 0.001f && Cvar_FindVar("r_wateralpha")->archive);
    VERIFY(Cvar_VariableValue("r_fluorescent") == 2 && Cvar_FindVar("r_fluorescent")->archive);
    VERIFY(Cvar_VariableValue("lq_particles") == 1 && Cvar_FindVar("lq_particles")->archive);
    VERIFY(Cvar_VariableValue("r_lerpmodels") == 0 && Cvar_FindVar("r_lerpmodels")->archive);
    VERIFY(Cvar_VariableValue("r_lerpmove") == 0 && Cvar_FindVar("r_lerpmove")->archive);
    Cvar_SetValue("r_wateralpha", 1); Cvar_SetValue("r_fluorescent", 1); Cvar_SetValue("lq_particles", 0);
    Cvar_SetValue("r_lerpmodels", 1); Cvar_SetValue("r_lerpmove", 1);
    VERIFY(vid.width == 960 && vid.height == 540);
    VERIFY(Cvar_FindVar("vid_width")->archive && Cvar_FindVar("vid_height")->archive);
    VERIFY(Cvar_VariableValue("fov") == 95 && Cvar_FindVar("fov")->archive);
    VERIFY(Cvar_VariableValue("gpu_filter") == 1 && Cvar_VariableValue("gpu_msaa") == 2);
    VERIFY(Cvar_VariableValue("gpu_mipmaps") == 2 && Cvar_FindVar("gpu_mipmaps")->archive);
    VERIFY(Cvar_VariableValue("gpu_anisotropy") == 16 && Cvar_FindVar("gpu_anisotropy")->archive);
    /* Reach 1080p through the same selector used by the player. */
    for (int i = 0; i < 12; ++i) vid_menukeyfn(K_UPARROW);
    for (int i = 0; i < 12; ++i) vid_menukeyfn(K_RIGHTARROW);
    for (int i = 0; i < 12; ++i) vid_menukeyfn(K_DOWNARROW);
    vid_menukeyfn(K_ENTER); Cbuf_Execute();
    VERIFY(vid.width == 1920 && vid.height == 1080);
    Cvar_SetValue("vid_width", 640); Cvar_SetValue("vid_height", 480);
    Cbuf_AddText("vid_restart\n"); Cbuf_Execute();
    VERIFY(LQ_UIScale() == 1);
    Cvar_SetValue("vid_width", 800); Cvar_SetValue("vid_height", 600);
    Cbuf_AddText("vid_restart\n"); Cbuf_Execute();
    memset(vid.buffer, 1, vid.width * vid.height);
    LQ_UIBegin(); VERIFY(LQ_UIScale() == 2 && vid.width == 400);
    Draw_Fill(3, 5, 1, 1, 150); LQ_UIEnd();
    VERIFY(vid.buffer[10 * vid.rowbytes + 6] == 150 && vid.buffer[11 * vid.rowbytes + 7] == 150);
    VERIFY(vid.buffer[10 * vid.rowbytes + 8] == 1);
    Cvar_SetValue("vid_width", 1600); Cvar_SetValue("vid_height", 1200);
    Cbuf_AddText("vid_restart\n"); Cbuf_Execute();
    LQ_UIBegin(); VERIFY(LQ_UIScale() == 4 && vid.width == 400); LQ_UIEnd();
    Cvar_SetValue("vid_width", 1920); Cvar_SetValue("vid_height", 1080);
    Cbuf_AddText("vid_restart\n"); Cbuf_Execute();
    VERIFY(vid.aspect == 1 && vid.width == 1920 && vid.height == 1080);
    LQ_UIBegin(); VERIFY(LQ_UIScale() == 2 && vid.width == 960 && vid.height == 540); LQ_UIEnd();
    puts("PASS video menu stages, applies and archives resolution");
    LQ_HandleEvent(&event); VERIFY(LQ_QuitRequested()); LQ_ResetQuit();
    puts("PASS SDL focus/key releases, text/modifiers, resize, palette, video restart, cache alignment, audio fallback/mixer/ring, quit");
    return 0;
}
int LQ_PlatformDemo(int argc, char **argv, qboolean test)
{
    quakeparms_t parms = {0}; byte palette[768];
    double start, previous, seconds = 0;
    int result = 0, argument;
    COM_InitArgv(argc, argv);
    parms.memsize = 256 * 1024 * 1024; parms.membase = malloc(parms.memsize); parms.basedir = ".";
    if (!parms.membase) Sys_Error("Cannot allocate demonstration heap");
    host_parms = parms;
    Memory_Init(parms.membase, parms.memsize); Cbuf_Init(); Cmd_Init(); COM_Init(".");
    Key_Init(); CL_Init(); IN_Init(); Font(); Palette(palette);
    Cvar_RegisterVariable(&scr_fov);
    cls.state = ca_connected; key_dest = key_game; LQ_SetDemo(true);
    VID_Init(palette);
    SDL_SetWindowTitle(LQ_SDLWindow(), "LuminousQuake / Phase 2 / F11 fullscreen / T tone / Esc exit");
    Con_Init();
    S_Init(); S_AmbientOff();
    Key_SetBinding('`', "toggleconsole"); Key_SetBinding(K_F11, "vid_togglefullscreen");
    Key_SetBinding('t', "platform_tone"); Cmd_AddCommand("platform_tone", Tone);
    Cbuf_Execute();
    if (test) result = TestPlatform(COM_CheckParm("--expect-no-audio") != 0);
    else {
        argument = COM_CheckParm("--demo-seconds");
        if (argument) {
            char *end;
            if (argument + 1 >= com_argc) Sys_Error("--demo-seconds needs a duration");
            seconds = strtod(com_argv[argument + 1], &end);
            if (*end || seconds <= 0 || seconds > 3600) Sys_Error("Invalid demonstration duration");
        }
        start = previous = Sys_FloatTime();
        while (!LQ_QuitRequested() && (!seconds || Sys_FloatTime() - start < seconds)) {
            double now = Sys_FloatTime(); usercmd_t movement = {0};
            host_frametime = now - previous; previous = now;
            LQ_PumpEvents(); Cbuf_Execute(); IN_Move(&movement);
            S_Update(vec3_origin, vec3_origin, vec3_origin, vec3_origin);
            Scene(now - start);
            SDL_Delay(10);
        }
        argument = COM_CheckParm("--screenshot");
        if (argument && argument + 1 < com_argc && !LQ_SaveFrame(com_argv[argument + 1]))
            Sys_Error("Cannot save demonstration frame: %s", SDL_GetError());
    }
    LQ_PerformanceStop();
    S_Shutdown(); IN_Shutdown(); VID_Shutdown();
    LQ_SetDemo(false); SDL_Quit(); free(parms.membase);
    return result;
}
