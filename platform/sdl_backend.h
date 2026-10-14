/* LuminousQuake SDL platform interface. GPL-2.0-or-later. */
#ifndef LQ_SDL_BACKEND_H
#define LQ_SDL_BACKEND_H
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
SDL_Window *LQ_SDLWindow(void);
SDL_Renderer *LQ_SDLRenderer(void);
typedef struct lq_gpu lq_gpu;
lq_gpu *LQ_PlayableGPU(void);
void LQ_ApplyRenderer(void);
void LQ_FallbackRenderer(const char *reason);
void LQ_RenderBridgeShutdown(void);
qboolean LQ_RenderBridgePresent(void);
void LQ_PumpEvents(void);
void LQ_HandleEvent(const SDL_Event *event);
void LQ_UpdateCapture(void);
void LQ_TestMouseDelta(int x, int y, usercmd_t *cmd);
void LQ_SetFocus(qboolean focused);
qboolean LQ_HasFocus(void);
void LQ_ResetQuit(void);
qboolean LQ_QuitRequested(void);
void LQ_SetDemo(qboolean active);
void LQ_AudioFocus(qboolean focused);
qboolean LQ_AudioReady(void);
uint64_t LQ_AudioFrames(void);
void LQ_AudioConsume(byte *stream, int length);
int LQ_PlatformDemo(int argc, char **argv, qboolean test);
qboolean LQ_SaveFrame(const char *path);
#endif
