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

static SDL_AudioDeviceID device;
static dma_t dma;
static byte *ring;
static size_t ring_bytes, read_byte;
static uint64_t frames_read, epoch;
static int paint_depth;
static qboolean subsystem, audio_focus = true;
extern int sound_started;

/* The callback only consumes the already-mixed ring; it never touches gameplay
 * or loads assets. Main-thread writes hold the SDL device lock. */
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
static void AudioCallback(void *context, Uint8 *stream, int length)
{
    (void)context;
    if (!ring || !ring_bytes) { memset(stream, 0, length); return; }
    LQ_AudioConsume(stream, length);
}
qboolean LQ_AudioReady(void) { return device != 0; }
void SNDDMA_BeginPainting(void)
{
    if (device && !paint_depth++) SDL_LockAudioDevice(device);
}
void SNDDMA_Submit(void)
{
    if (device && paint_depth > 0 && !--paint_depth) SDL_UnlockAudioDevice(device);
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
void LQ_AudioFocus(qboolean focus)
{
    audio_focus = focus;
    if (!device) return;
    SDL_PauseAudioDevice(device, 1);
    if (!focus) S_ClearBuffer();
    SDL_PauseAudioDevice(device, focus ? 0 : 1);
}
qboolean SNDDMA_Init(void)
{
    SDL_AudioSpec wanted, obtained;
    int option, rate = 44100;
    if (device) return true;
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
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Con_Printf("SDL audio unavailable: %s\n", SDL_GetError()); return false;
    }
    subsystem = true;
    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = rate; wanted.format = AUDIO_S16SYS; wanted.channels = 2;
    wanted.samples = 1024; wanted.callback = AudioCallback;
    device = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!device) {
        Con_Printf("SDL audio device unavailable: %s\n", SDL_GetError());
        SNDDMA_Shutdown(); return false;
    }
    ring_bytes = 32768 * 2 * sizeof(short);
    ring = calloc(ring_bytes, 1);
    if (!ring) { SNDDMA_Shutdown(); return false; }
    memset(&dma, 0, sizeof(dma));
    dma.samplebits = 16; dma.speed = obtained.freq; dma.channels = 2;
    dma.samples = (int)(ring_bytes / sizeof(short)); dma.submission_chunk = 1;
    dma.buffer = ring; dma.gamealive = dma.soundalive = true;
    shm = &dma; frames_read = epoch = read_byte = 0; paint_depth = 0;
    paintedtime = 0;
    SDL_PauseAudioDevice(device, audio_focus ? 0 : 1);
    Con_Printf("SDL audio: %d Hz stereo signed 16-bit\n", dma.speed);
    return true;
}
void SNDDMA_Shutdown(void)
{
    if (device) {
        /* Fatal exits may interrupt mixing while this thread owns the lock. */
        if (paint_depth) { SDL_UnlockAudioDevice(device); paint_depth = 0; }
        /* Closing waits for the callback before releasing the ring. */
        SDL_CloseAudioDevice(device); device = 0;
    }
    free(ring); ring = NULL; ring_bytes = read_byte = 0;
    paint_depth = 0; shm = NULL;
    if (subsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    subsystem = false;
}
