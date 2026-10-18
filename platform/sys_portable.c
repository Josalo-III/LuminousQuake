/* LuminousQuake portable bootstrap. GPL-2.0-or-later; see gnu.txt. */
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <conio.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
#include <errno.h>
#include "quakedef.h"
#include "diagnostics.h"
#include "frame_timing.h"
#include "tick_clock.h"
#ifdef LQ_SDL
#include "sdl_backend.h"
#include "jobs.h"
#include "performance.h"
#ifdef __APPLE__
#include "mac_launch.h"
#endif
static void PlatformExit(void) { LQ_TickClockStop(); LQ_PerformanceStop(); S_Shutdown(); IN_Shutdown(); VID_Shutdown(); SDL_Quit(); }
static cvar_t lq_clock_mode = {"lq_clock_mode", "0", true};
static void ClockReport(void)
{
    lq_clock_stats s = LQ_TickClockStats();
    lq_clock_sample sample = {0};
    LQ_TickClockRead(&sample);
    Con_Printf("Tick clock: %s; %s margin=%uus late=%u max_late=%uus skipped=%u fallbacks=%u "
        "cores=%u load=%u%%%s switches=%u tick=%llu published=%llu\n",
        !s.running ? "synchronous fallback" : s.spinning ? "continuous" :
            s.margin_us ? "early wake" : "wait until",
        s.realtime ? "real-time" : "standard", s.margin_us, s.late, s.max_late_us,
        s.skipped, s.fallbacks,
        s.cores, s.busy_percent, s.load_valid ? "" : " (unknown)", s.switches,
        (unsigned long long)sample.tick,
        (unsigned long long)s.published);
}
static void PerformanceReport(void)
{
    lq_perf_snapshot s = LQ_PerformanceRead();
    Con_Printf("Performance: %s; cores=%u CPU=%u%%%s worker budget=%u/%u transitions=%u\n",
        LQ_PerformanceStateName(s.state), s.cores, s.busy_percent,
        s.load_valid ? "" : " (unknown)", LQ_JobsWorkerBudget(),
        LQ_JobsDesiredWorkerCount(), s.transitions);
}
#endif

void LQ_TickClockRegister(void)
{
#ifdef LQ_SDL
    Cvar_RegisterVariable(&lq_clock_mode);
    Cmd_AddCommand("lq_clock_report", ClockReport);
    Cmd_AddCommand("lq_performance_report", PerformanceReport);
#endif
}

#ifdef LQ_GPU
#include "gpu_viewer.h"
#endif

#define MAX_HANDLES 64
static FILE *sys_handles[MAX_HANDLES];
static qboolean initialized;
qboolean isDedicated;

static int FindHandle(void)
{
    int i;
    for (i = 1; i < MAX_HANDLES; ++i)
        if (!sys_handles[i]) return i;
    Sys_Error("Out of file handles");
    return -1;
}

int Sys_FileOpenRead(char *path, int *handle)
{
    FILE *file = fopen(path, "rb");
    long size;
    if (!file) { *handle = -1; return -1; }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || size > INT_MAX ||
        fseek(file, 0, SEEK_SET)) {
        fclose(file);
        Sys_Error("Cannot read file size within Quake's format limits: %s", path);
    }
    *handle = FindHandle();
    sys_handles[*handle] = file;
    return (int)size;
}
int Sys_FileOpenWrite(char *path)
{
    int handle = FindHandle();
    sys_handles[handle] = fopen(path, "wb");
    if (!sys_handles[handle]) Sys_Error("Cannot write %s: %s", path, strerror(errno));
    return handle;
}
void Sys_FileClose(int handle) { fclose(sys_handles[handle]); sys_handles[handle] = NULL; }
void Sys_FileSeek(int handle, int position)
{
    if (fseek(sys_handles[handle], position, SEEK_SET)) Sys_Error("File seek failed");
}
int Sys_FileRead(int handle, void *dest, int count)
{ return (int)fread(dest, 1, count, sys_handles[handle]); }
int Sys_FileWrite(int handle, void *data, int count)
{ return (int)fwrite(data, 1, count, sys_handles[handle]); }
int Sys_FileTime(char *path)
{
    /* The existing filesystem only compares this result against -1. */
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    fclose(file);
    return 1;
}
void Sys_mkdir(char *path)
{
#ifdef _WIN32
    if (_mkdir(path) && errno != EEXIST)
#else
    if (mkdir(path, 0777) && errno != EEXIST)
#endif
        Sys_Error("Cannot create %s: %s", path, strerror(errno));
}
void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{ (void)startaddr; (void)length; }
void Sys_Printf(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt); vprintf(fmt, args); va_end(args);
    fflush(stdout);
}
void Sys_DebugLog(char *path, char *fmt, ...)
{
    FILE *file = fopen(path, "a");
    va_list args;
    if (!file) return;
    va_start(args, fmt); vfprintf(file, fmt, args); va_end(args);
    fclose(file);
}
void Sys_Error(char *fmt, ...)
{
    va_list args;
    char message[2048];
    fprintf(stderr, "LuminousQuake: ");
    va_start(args, fmt); vsnprintf(message, sizeof(message), fmt, args); va_end(args);
    fputs(message, stderr);
    fputc('\n', stderr);
#if defined(LQ_SDL) && defined(__APPLE__)
    LQ_MacError(message);
#endif
    /* Initialization failures must not call shutdown through incomplete state. */
    exit(EXIT_FAILURE);
}
void Sys_Quit(void)
{
    if (initialized) Host_Shutdown();
    exit(EXIT_SUCCESS);
}
double Sys_FloatTime(void)
{
#ifdef _WIN32
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) Sys_Error("Cannot read monotonic clock");
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
#endif
}
void Sys_Sleep(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
#endif
}
char *Sys_ConsoleInput(void)
{
    static char line[256];
    static size_t used;
    int ch;
#ifndef _WIN32
    fd_set readable;
    struct timeval timeout = {0, 0};
    FD_ZERO(&readable); FD_SET(STDIN_FILENO, &readable);
    if (select(STDIN_FILENO + 1, &readable, NULL, NULL, &timeout) <= 0) return NULL;
    if (read(STDIN_FILENO, line + used, 1) != 1) return NULL;
    ch = (unsigned char)line[used];
#else
    if (!_kbhit()) return NULL;
    ch = _getch();
#endif
    if (ch == '\n' || ch == '\r') { line[used] = 0; used = 0; return line; }
    if (ch == 8 || ch == 127) { if (used) --used; }
    else if (used < sizeof(line) - 1) line[used++] = (char)ch;
    return NULL;
}
void Sys_SendKeyEvents(void)
{
#ifdef LQ_SDL
    LQ_PumpEvents();
    if (LQ_QuitRequested()) Sys_Quit();
#endif
}
void Sys_LowFPPrecision(void) {}
void Sys_HighFPPrecision(void) {}
void Sys_SetFPCW(void) {}

#ifndef _WIN32
/* A fatal signal appends a backtrace to ~/Library/Logs/LuminousQuake/crash.log
 * (~/.local/state/luminousquake elsewhere), since macOS may not keep a report.
 * The handler uses only async-signal-safe calls; the path and image slide are
 * prepared at startup. Symbolize with: atos -o LuminousQuake -l <load> <frames>. */
static char crash_path[1024];
static void CrashWrite(int fd, const char *text) { write(fd, text, strlen(text)); }
static void CrashHex(int fd, unsigned long long value)
{
    char digits[19] = "0x"; int n = 16;
    for (int i = 0; i < 16; ++i) digits[2 + i] = "0123456789abcdef"[(value >> ((15 - i) * 4)) & 15];
    digits[2 + n] = 0; CrashWrite(fd, digits);
}
static void CrashHandler(int sig)
{
    int fd = crash_path[0] ? open(crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    if (fd < 0) fd = 2;
    void *frames[64]; int count = backtrace(frames, 64);
    CrashWrite(fd, "\n=== LuminousQuake " LQ_VERSION " crashed, signal ");
    char number[4] = {(char)('0' + sig / 10 % 10), (char)('0' + sig % 10), '\n', 0};
    CrashWrite(fd, number);
#ifdef __APPLE__
    CrashWrite(fd, "load address ");
    CrashHex(fd, 0x100000000ULL + (unsigned long long)_dyld_get_image_vmaddr_slide(0));
    CrashWrite(fd, "\n");
#endif
    backtrace_symbols_fd(frames, count, fd);
    if (fd != 2) { backtrace_symbols_fd(frames, count, 2); close(fd); }
    signal(sig, SIG_DFL); raise(sig);
}
static void InstallCrashLog(void)
{
    const char *home = getenv("HOME");
    if (home && strlen(home) < sizeof(crash_path) - 64) {
#ifdef __APPLE__
        snprintf(crash_path, sizeof(crash_path), "%s/Library/Logs/LuminousQuake", home);
#else
        snprintf(crash_path, sizeof(crash_path), "%s/.local/state/luminousquake", home);
#endif
        mkdir(crash_path, 0755);
        strcat(crash_path, "/crash.log");
    }
    void *warm[1]; backtrace(warm, 1); /* load the unwinder before it is needed */
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) signal(signals[i], CrashHandler);
}
#endif

int main(int argc, char **argv)
{
    quakeparms_t parms = {0};
    char *engine_argv[MAX_NUM_ARGVS + 1];
    char installation[MAX_OSPATH] = "";
    double previous, run_start, run_seconds = 0, profile_warmup = -1;
    const char *capture_path = NULL;
    qboolean profile_report = false, profile_reset = false;
    int i, engine_argc = 0;
#ifndef _WIN32
    if (!getenv("LQ_NO_CRASH_LOG")) InstallCrashLog();
#endif
#ifdef LQ_SDL
    atexit(PlatformExit);
#endif
    for (i = 1; i < argc; ++i) {
#ifdef LQ_SDL
        if (!strcmp(argv[i], "--platform-demo")) return LQ_PlatformDemo(argc, argv, false);
        if (!strcmp(argv[i], "--platform-test")) return LQ_PlatformDemo(argc, argv, true);
#endif
        if (!strcmp(argv[i], "--version")) {
            printf("LuminousQuake " LQ_VERSION " (%zu-bit native C, %s)\n",
                sizeof(void *) * CHAR_BIT,
#ifdef LQ_DEDICATED
                "dedicated server"
#elif defined(LQ_GPU)
                "SDL3 GPU world viewer"
#elif defined(LQ_SDL)
                "SDL3 client"
#else
                "headless client"
#endif
            );
            return 0;
        }
        if (!strcmp(argv[i], "--self-test")) return LQ_RunDiagnostics(argc, argv);
        if (!strcmp(argv[i], "--help")) {
#ifdef LQ_GPU
            puts("Usage: luminousquake-gpu -basedir PATH [-map e1m1] [--run-seconds N] [--screenshot PATH]\n"
                 "       luminousquake-gpu --gpu-test\n"
                 "Experimental GPU world viewer: WASD fly, E/Q vertical, mouse look, F1 capture, F11 fullscreen, Esc exit.\n"
                 "This target does not run gameplay, models or QuakeC.");
#else
            puts("Usage: luminousquake[-server] [--version | --self-test | --check-data]\n"
                 "       [-basedir PATH] [-game MOD] [-dedicated] [Quake arguments]\n"
                 "The SDL client supports --platform-demo (no assets needed) and --platform-test.\n"
                 "Use --run-seconds N for a bounded smoke run; --screenshot PATH saves a BMP.\n"
                 "--profile-warmup-seconds N resets enabled CPU timing after startup.\n"
                 "Original Quake game data is required for engine startup.");
#endif
            return 0;
        }
    }
#ifdef LQ_GPU
    for (i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--check-data") || !strcmp(argv[i], "-dedicated")) break;
    if (i == argc) return LQ_GPUViewer(argc, argv);
#endif
    /* Resolve assets before entering the engine. Finder supplies an arbitrary
     * working directory; SDL's base path supplies executable-relative discovery. */
#ifdef LQ_SDL
    int diagnostic = 0, explicit_base = 0;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--check-data") || !strcmp(argv[i], "-dedicated")) diagnostic = 1;
        if (!strcmp(argv[i], "-basedir")) explicit_base = 1;
    }
    if (!diagnostic && !explicit_base) {
#ifdef __APPLE__
        int selected = LQ_MacInstallation(argc, argv, installation, sizeof(installation));
        if (!selected) return 0;
        if (selected < 0) Sys_Error("Cannot initialize installation selection");
#endif
        if (!installation[0]) {
            const char *base = SDL_GetBasePath();
            if (!base || strlen(base) >= sizeof(installation) - 32) Sys_Error("Cannot resolve executable path; use -basedir PATH");
            snprintf(installation, sizeof(installation), "%s", base);
        }
    }
#endif
    if (argc >= MAX_NUM_ARGVS) Sys_Error("Too many command-line arguments");
    for (i = 0; i < argc; ++i) engine_argv[engine_argc++] = argv[i];
    if (installation[0]) {
        if (engine_argc + 2 >= MAX_NUM_ARGVS) Sys_Error("Too many command-line arguments");
        engine_argv[engine_argc++] = "-basedir"; engine_argv[engine_argc++] = installation;
    }
#ifdef LQ_DEDICATED
    engine_argv[engine_argc++] = "-dedicated";
#endif
    engine_argv[engine_argc] = NULL;
    COM_InitArgv(engine_argc, engine_argv);
    if (COM_CheckParm("-record") || COM_CheckParm("-playback"))
        Sys_Error("Legacy low-level VCR recording/playback is not supported by this portable build; ordinary demos are separate");
    parms.argc = com_argc; parms.argv = com_argv;
    parms.basedir = ".";
    parms.memsize = 256 * 1024 * 1024;
    parms.membase = malloc(parms.memsize);
    if (!parms.membase) Sys_Error("Cannot allocate engine heap");
    i = COM_CheckParm("-basedir");
    if (i) {
        if (i + 1 >= com_argc) Sys_Error("-basedir needs a directory");
        if (strlen(com_argv[i + 1]) + MAX_QPATH + 32 >= MAX_OSPATH)
            Sys_Error("Base directory exceeds the legacy path limit");
    }
    i = COM_CheckParm("-game");
    if (i && (i + 1 >= com_argc || strlen(com_argv[i + 1]) >= MAX_QPATH ||
              strstr(com_argv[i + 1], "..") || strchr(com_argv[i + 1], '/') ||
              strchr(com_argv[i + 1], '\\') || strchr(com_argv[i + 1], ':')))
        Sys_Error("-game needs a mod directory shorter than MAX_QPATH");
    if (COM_CheckParm("--check-data")) return LQ_CheckData(&parms);
    Sys_Printf("LuminousQuake: native Quake bootstrap.\n"
        "Use -basedir PATH for original Quake data, or --check-data to diagnose it.\n");
    isDedicated = COM_CheckParm("-dedicated") != 0;
    Host_Init(&parms);
    initialized = true;
#if defined(LQ_SDL) && defined(__APPLE__)
    if (LQ_MacFinderLaunch()) Cbuf_AddText("menu_main\n");
#endif
    i = COM_CheckParm("--run-seconds");
    if (i) {
        char *end;
        if (i + 1 >= com_argc) Sys_Error("--run-seconds requires a duration");
        run_seconds = strtod(com_argv[i + 1], &end);
        if (*end || !isfinite(run_seconds)) Sys_Error("Invalid --run-seconds duration");
    }
    if (i && (run_seconds <= 0 || run_seconds > 3600)) Sys_Error("--run-seconds requires 0 < seconds <= 3600");
    i = COM_CheckParm("--screenshot");
    if (i) {
        if (i + 1 >= com_argc) Sys_Error("--screenshot requires a path");
        capture_path = com_argv[i + 1];
    }
    profile_report = COM_CheckParm("--profile-report") != 0;
    i = COM_CheckParm("--profile-warmup-seconds");
    if (i) {
        char *end;
        if (i + 1 >= com_argc) Sys_Error("--profile-warmup-seconds requires a duration");
        profile_warmup = strtod(com_argv[i + 1], &end);
        if (*end || !isfinite(profile_warmup) || profile_warmup < 0 ||
            (run_seconds > 0 && profile_warmup >= run_seconds))
            Sys_Error("--profile-warmup-seconds must be non-negative and shorter than --run-seconds");
    }
#ifdef LQ_SDL
    /* Controls were registered during Host_InitLocal, before startup config
     * execution. Start the publisher after platform initialization is ready. */
    if (!isDedicated) {
        if (!LQ_PerformanceStart(0, NULL, NULL))
            Con_Printf("Resource monitor unavailable; automatic policy remains conservative.\n");
        if (!LQ_TickClockStart())
            Con_Printf("Tick clock unavailable; retaining synchronous timing.\n");
    }
#endif
    run_start = previous = Sys_FloatTime();
    for (;;) {
        double now = Sys_FloatTime();
#ifdef LQ_SDL
        int mode = isfinite(lq_clock_mode.value) && lq_clock_mode.value >= 0 && lq_clock_mode.value <= 2 ? (int)lq_clock_mode.value : LQ_CLOCK_AUTO;
        LQ_TickClockConfigure(mode, LQ_JobsDesiredWorkerCount(),
            cls.state == ca_connected && !cl.paused && key_dest == key_game);
#endif
        Host_Frame(now - previous);
        previous = now;
        if (!profile_reset && profile_warmup >= 0 && now - run_start >= profile_warmup) {
            LQ_ProfileReset(); profile_reset = true;
        }
        if (run_seconds > 0 && now - run_start >= run_seconds) {
#ifdef LQ_SDL
            if (capture_path && !LQ_SaveFrame(capture_path)) Sys_Error("Cannot save screenshot");
#else
            (void)capture_path;
#endif
            if (profile_report) LQ_ProfileReport();
            Sys_Quit();
        }
        Sys_Sleep();
    }
}
