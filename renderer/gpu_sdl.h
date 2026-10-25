/* SDL3 GPU implementation boundary. GPL-2.0-or-later. */
#ifndef LQ_GPU_SDL_H
#define LQ_GPU_SDL_H
#include "scene.h"
typedef struct SDL_Window SDL_Window;
typedef struct lq_gpu lq_gpu;
lq_gpu *LQ_GPUCreate(SDL_Window *window, int debug, char *error, size_t size);
int LQ_GPUUpload(lq_gpu *gpu, const lq_scene *scene, char *error, size_t size);
int LQ_GPUOverlay(lq_gpu *gpu, const lq_image *image, char *error, size_t size);
int LQ_GPUColormap(lq_gpu *gpu, const lq_image *image, char *error, size_t size);
int LQ_GPUFrame(lq_gpu *gpu, const lq_frame *frame, char *error, size_t size);
int LQ_GPUOptions(lq_gpu *gpu, int smooth, int samples, int vsync, char *error, size_t size);
int LQ_GPUTextureOptions(lq_gpu *gpu, int mipmaps, int anisotropy, char *error, size_t size);
int LQ_GPUSamplesSupported(lq_gpu *gpu, int samples);
int LQ_GPURender(lq_gpu *gpu, const lq_view *view, uint32_t width, uint32_t height, char *error, size_t size);
/* Luminous bounce rays on this GPU, for LQ_SceneComputeBounceTraced. A pass
 * the GPU cannot run returns 0 and falls back to the CPU. */
lq_bounce_tracer LQ_GPUBounceTracer(lq_gpu *gpu);
/* Luminous: trace this frame's dynamic probes on the GPU for the gameplay
 * lights and box from LQ_SceneDynamicProbeLights; the next render writes
 * them before drawing. Returns 0 when this GPU cannot (then use
 * LQ_SceneDynamicProbes and LQ_GPUDynamicProbes). */
int LQ_GPUTraceDynamicProbes(lq_gpu *gpu, const lq_runtime_light *const *lights, uint32_t count,
    const uint32_t box_min[3], const uint32_t box_size[3], uint32_t rays);
/* Diagnostics: write any pending probes now and read the dynamic probe texture
 * (LQ_GPUDynamicProbeBytes bytes). */
int LQ_GPUReadDynamicProbes(lq_gpu *gpu, uint8_t *pixels);
/* Shadow cube slots redrawn by the most recent render; unchanged slots reuse
 * their cached static-world shadows. */
uint32_t LQ_GPUShadowUpdates(const lq_gpu *gpu);
uint32_t LQ_GPUSubmittedFaces(const lq_gpu *gpu);
/* Of those, faces whose brightest light was known before drawing (Luminous). */
uint32_t LQ_GPUKnownLeadFaces(const lq_gpu *gpu);
/* Cube faces redrawn by the most recent render (moving casters redraw only the
 * faces they touch). */
uint32_t LQ_GPUShadowFaceUpdates(const lq_gpu *gpu);
/* Dynamic probe layer for this map (see LQ_SceneDynamicProbes); NULL marks
 * it inactive so shaders skip it. */
size_t LQ_GPUDynamicProbeBytes(const lq_gpu *gpu);
int LQ_GPUDynamicProbes(lq_gpu *gpu, const uint8_t *pixels, char *error, size_t size);
int LQ_GPUReadPixels(lq_gpu *gpu, uint8_t **pixels, uint32_t *width, uint32_t *height, char *error, size_t size);
/* Display transfer: gamma (lq_gamma, 2.2 normally) for every GPU renderer,
 * whether HDR output may be used where the display offers it (lq_hdr), and a
 * cap on HDR highlights in multiples of SDR white (lq_hdr_peak; 0 none). */
void LQ_GPUDisplay(lq_gpu *gpu, float gamma, int hdr, float peak);
/* lq_hdr_test: replace the 3D view with bands at known light levels (0.5-10x SDR white). */
void LQ_GPUTestPattern(lq_gpu *gpu, int on);
void LQ_GPUToneRamp(lq_gpu *gpu, int on);
int LQ_GPUSupportsBC7(lq_gpu *gpu); /* texture packs below full size are stored as BC7 */
/* Bounce tracing paced for play (staged bounce, on the background thread):
 * `chunk` samples a submission, one submission for each frame drawn, so a
 * frame waits behind at most one slice; `cancel` nonzero stops it between
 * submissions. The struct must outlive the tracer's use. */
typedef struct { lq_gpu *gpu; uint32_t chunk; int paced; const volatile int *cancel; } lq_bounce_pace;
lq_bounce_tracer LQ_GPUBounceTracerPaced(lq_bounce_pace *pace);
/* Replace the bounce texture, static probes, sky and sun values after a
 * staged bounce finishes for the same map. Returns 0 on failure. */
int LQ_GPURefreshBounce(lq_gpu *gpu, const lq_scene *scene);
/* Shadow cube size: 1 doubles LQ_SHADOW_ARRAY_SIZES, -1 halves them, 0 as is (the lighting memory share). */
void LQ_GPUShadowScale(lq_gpu *gpu, int shift); /* known light levels through the tone curve, with markers */
/* Luminous tone mapping and emission: the shoulder below which linear light is
 * left alone (0.8), the white point that reaches the display's ceiling (6 on
 * SDR; on HDR the larger of it and twice the headroom), and the gains at which
 * fullbright texels emit, for world and models (3) and for sprites such as
 * explosions (3). Out-of-range values take the defaults. */
void LQ_GPUTone(lq_gpu *gpu, float shoulder, float white, float emission, float sprite_emission);
/* Whether frames are going out as HDR, and the headroom above SDR white. */
int LQ_GPUDisplayHDR(const lq_gpu *gpu, float *headroom);
/* A skybox: six RGBA8 faces (gfx/env/<name>rt, lf, bk, ft, up, dn: +x, -x,
 * +y, -y, +z, -z in Quake's axes), drawn on sky surfaces in place of the
 * map's sky texture. A face with no pixels is black; NULL removes the box. */
int LQ_GPUSkybox(lq_gpu *gpu, const lq_image faces[6], char *error, size_t size);
void LQ_GPUDestroy(lq_gpu *gpu);
#endif
