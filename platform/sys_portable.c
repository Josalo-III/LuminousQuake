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
#endif
#include <errno.h>
#include "quakedef.h"
#include "diagnostics.h"

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
    fprintf(stderr, "LuminousQuake: ");
    va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr);
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
void Sys_SendKeyEvents(void) {}
void Sys_LowFPPrecision(void) {}
void Sys_HighFPPrecision(void) {}
void Sys_SetFPCW(void) {}

int main(int argc, char **argv)
{
    quakeparms_t parms = {0};
    char *engine_argv[MAX_NUM_ARGVS + 1];
    double previous;
    int i, engine_argc = 0;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--version")) {
            printf("LuminousQuake 0.1.0 (%zu-bit native C, headless Phase 1 %s)\n",
                sizeof(void *) * CHAR_BIT,
#ifdef LQ_DEDICATED
                "server"
#else
                "client"
#endif
            );
            return 0;
        }
        if (!strcmp(argv[i], "--self-test")) return LQ_RunDiagnostics(argc, argv);
        if (!strcmp(argv[i], "--help")) {
            puts("Usage: luminousquake[-server] [--version | --self-test | --check-data]\n"
                 "       [-basedir PATH] [-game MOD] [-dedicated] [Quake arguments]\n"
                 "Phase 1 uses headless video/input/audio. Windowed operation is Phase 2.\n"
                 "Original Quake game data is required for engine startup.");
            return 0;
        }
    }
    if (argc >= MAX_NUM_ARGVS) Sys_Error("Too many command-line arguments");
    for (i = 0; i < argc; ++i) engine_argv[engine_argc++] = argv[i];
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
        if (strlen(com_argv[i + 1]) + sizeof("/id1/pak0.pak") >= MAX_OSPATH)
            Sys_Error("Base directory exceeds the legacy path limit");
    }
    i = COM_CheckParm("-game");
    if (i && (i + 1 >= com_argc || strlen(com_argv[i + 1]) >= MAX_QPATH))
        Sys_Error("-game needs a mod directory shorter than MAX_QPATH");
    if (COM_CheckParm("--check-data")) return LQ_CheckData(&parms);
    Sys_Printf("LuminousQuake Phase 1: headless native bootstrap.\n"
        "Use -basedir PATH for original Quake data, or --check-data to diagnose it.\n");
    isDedicated = COM_CheckParm("-dedicated") != 0;
    Host_Init(&parms);
    initialized = true;
    previous = Sys_FloatTime();
    for (;;) {
        double now = Sys_FloatTime();
        Host_Frame(now - previous);
        previous = now;
        Sys_Sleep();
    }
}
