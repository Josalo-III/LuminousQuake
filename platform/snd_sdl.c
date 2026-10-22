/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include <stdlib.h>
#include <stdio.h>

/* SDL audio adapter for Quake's C mixer. See SOURCE_PROVENANCE.md. */
#include "quakedef.h"
#include "sdl_backend.h"

static SDL_AudioStream *audio_stream;
static dma_t dma;
static byte *ring;
static size_t ring_bytes, read_byte;
static uint64_t frames_read, epoch;
static _Thread_local int paint_depth; /* the stream lock's depth on this thread */
static qboolean subsystem, audio_focus = true;
extern int sound_started;

/* The sound thread (see sound.h): every MIXER_PERIOD_NS it mixes, then
 * searches sound paths for up to MIXER_SEARCH seconds of what is left. The
 * locks live for the whole run; SDL mutexes are recursive. */
#define MIXER_PERIOD_NS (5 * SDL_NS_PER_MS)
#define MIXER_SEARCH 0.003
static SDL_Mutex *mixer_mutex, *paths_mutex;
static SDL_Thread *mixer_thread;
static SDL_AtomicInt mixer_stop;
static _Thread_local int mixer_held, paths_held;

void LQ_MixerLock(void) { if (mixer_mutex) { SDL_LockMutex(mixer_mutex); ++mixer_held; } }
void LQ_MixerUnlock(void) { if (mixer_mutex && mixer_held) { --mixer_held; SDL_UnlockMutex(mixer_mutex); } }
void LQ_PathsLock(void) { if (paths_mutex) { SDL_LockMutex(paths_mutex); ++paths_held; } }
void LQ_PathsUnlock(void) { if (paths_mutex && paths_held) { --paths_held; SDL_UnlockMutex(paths_mutex); } }
qboolean LQ_MixerRunning(void) { return mixer_thread != NULL; }

static int SDLCALL MixerMain(void *unused)
{
    (void)unused;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    while (!SDL_GetAtomicInt(&mixer_stop)) {
        Uint64 start = SDL_GetTicksNS(), spent;
        S_MixerStep(MIXER_SEARCH);
        spent = SDL_GetTicksNS() - start;
        if (spent < MIXER_PERIOD_NS) SDL_DelayNS(MIXER_PERIOD_NS - spent);
    }
    return 0;
}
static void StartMixer(void)
{
    if (mixer_thread) return;
    if (!mixer_mutex) mixer_mutex = SDL_CreateMutex();
    if (!paths_mutex) paths_mutex = SDL_CreateMutex();
    if (!mixer_mutex || !paths_mutex) {
        Con_Printf("Sound thread unavailable: %s; mixing each frame\n", SDL_GetError()); return;
    }
    SDL_SetAtomicInt(&mixer_stop, 0);
    mixer_thread = SDL_CreateThread(MixerMain, "LQ sound", NULL);
    if (!mixer_thread) Con_Printf("Sound thread unavailable: %s; mixing each frame\n", SDL_GetError());
}
static void StopMixer(void)
{
    if (!mixer_thread) return;
    SDL_SetAtomicInt(&mixer_stop, 1);
    /* Fatal exits may come while this thread holds a lock the mixer waits on. */
    while (paths_held) LQ_PathsUnlock();
    while (mixer_held) LQ_MixerUnlock();
    SDL_WaitThread(mixer_thread, NULL); mixer_thread = NULL;
}

/* The callback only consumes the already-mixed ring; it never touches gameplay
 * or loads assets. Writes to the ring, from the sound thread or the main
 * thread, hold the SDL stream lock, also held by SDL while invoking the
 * callback. SDL converts the fixed mixer format to the output device's format
 * and sample rate. */
void LQ_AudioConsume(byte *stream, int length)
{
    while (length > 0) {
        size_t count = ring_bytes - read_byte;
        if (count > (size_t)length) count = length;
        memcpy(stream, ring + read_byte, count);
        memset(ring + read_byte, 0, count); /* stale samples become silence */
        stream += count; length -= (int)count;
        read_byte = (read_byte + count) % ring_bytes;
        frames_read += count / (sizeof(short) * dma.channels);
    }
}
static void SDLCALL AudioCallback(void *context, SDL_AudioStream *stream,
                                 int additional, int total)
{
    byte samples[4096];
    (void)context;
    (void)total;
    if (!ring || !ring_bytes || additional <= 0) return;
    /* Whole stereo frames preserve the ring clock even for variable requests.
     * Use fixed scratch storage rather than allocating in the audio callback. */
    additional = (additional + 3) & ~3;
    while (additional > 0) {
        int count = additional > (int)sizeof(samples) ? (int)sizeof(samples) : additional;
        LQ_AudioConsume(samples, count);
        if (!SDL_PutAudioStreamData(stream, samples, count)) return;
        additional -= count;
    }
}
qboolean LQ_AudioReady(void) { return audio_stream != NULL; }
void SNDDMA_BeginPainting(void)
{
    if (audio_stream && !paint_depth++) SDL_LockAudioStream(audio_stream);
}
void SNDDMA_Submit(void)
{
    if (audio_stream && paint_depth > 0 && !--paint_depth) SDL_UnlockAudioStream(audio_stream);
}
uint64_t LQ_AudioFrames(void)
{
    uint64_t frames;
    SNDDMA_BeginPainting(); frames = frames_read; SNDDMA_Submit();
    return frames;
}
int SNDDMA_GetDMAPos(void)
{
    int position;
    SNDDMA_BeginPainting(); position = (int)(read_byte / sizeof(short)); SNDDMA_Submit();
    return position;
}
int SNDDMA_GetSoundtime(void)
{
    uint64_t elapsed = frames_read - epoch;
    /* Caller holds the painting lock. Preserve the old mixer's long-run reset. */
    if (elapsed > UINT64_C(0x40000000)) {
        epoch = frames_read; paintedtime = 0; S_StopAllSounds(true); elapsed = 0;
    }
    return (int)elapsed;
}
/* The soundtime now, between S_Updates: frames the device has taken. */
int LQ_AudioSoundtime(void)
{
    int frames;
    SNDDMA_BeginPainting(); frames = (int)(frames_read - epoch); SNDDMA_Submit();
    return frames;
}
void LQ_AudioFocus(qboolean focus)
{
    audio_focus = focus;
    if (!audio_stream) return;
    LQ_MixerLock();
    SDL_PauseAudioStreamDevice(audio_stream);
    if (!focus) {
        S_ClearBuffer();
        SDL_ClearAudioStream(audio_stream);
    } else SDL_ResumeAudioStreamDevice(audio_stream);
    LQ_MixerUnlock();
}
qboolean SNDDMA_Init(void)
{
    SDL_AudioSpec wanted;
    int option, rate = 44100;
    if (audio_stream) return true;
    if (COM_CheckParm("-nosound")) return false;
    option = COM_CheckParm("-sndspeed");
    if (option) {
        char *end; long parsed;
        if (option + 1 >= com_argc) Sys_Error("-sndspeed needs a sample rate");
        parsed = strtol(com_argv[option + 1], &end, 10);
        if (!*com_argv[option + 1] || *end || parsed < 8000 || parsed > 192000)
            Sys_Error("-sndspeed must be between 8000 and 192000");
        rate = (int)parsed;
    }
    SDL_SetMainReady();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Con_Printf("SDL audio unavailable: %s\n", SDL_GetError()); return false;
    }
    subsystem = true;
    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = rate; wanted.format = SDL_AUDIO_S16; wanted.channels = 2;
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                             &wanted, AudioCallback, NULL);
    if (!audio_stream) {
        Con_Printf("SDL audio device unavailable: %s\n", SDL_GetError());
        SNDDMA_Shutdown(); return false;
    }
    ring_bytes = 32768 * 2 * sizeof(short);
    ring = calloc(ring_bytes, 1);
    if (!ring) { SNDDMA_Shutdown(); return false; }
    memset(&dma, 0, sizeof(dma));
    dma.samplebits = 16; dma.speed = rate; dma.channels = 2;
    dma.samples = (int)(ring_bytes / sizeof(short)); dma.submission_chunk = 1;
    dma.buffer = ring; dma.gamealive = dma.soundalive = true;
    shm = &dma; frames_read = epoch = read_byte = 0; paint_depth = 0;
    paintedtime = 0;
    if (audio_focus) SDL_ResumeAudioStreamDevice(audio_stream);
    Con_Printf("SDL3 audio stream: %d Hz stereo signed 16-bit\n", dma.speed);
    StartMixer();
    return true;
}
void SNDDMA_Shutdown(void)
{
    StopMixer();
    if (audio_stream) {
        /* Fatal exits may interrupt mixing while this thread owns the lock. */
        if (paint_depth) { SDL_UnlockAudioStream(audio_stream); paint_depth = 0; }
        /* Destroying waits for the callback and closes its logical device. */
        SDL_DestroyAudioStream(audio_stream); audio_stream = NULL;
    }
    free(ring); ring = NULL; ring_bytes = read_byte = 0;
    paint_depth = 0; shm = NULL;
    if (subsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    subsystem = false;
}
