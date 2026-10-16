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
/* Enhanced bounce rays on this GPU, for LQ_SceneComputeBounceTraced. A pass
 * the GPU cannot run returns 0 and falls back to the CPU. */
lq_bounce_tracer LQ_GPUBounceTracer(lq_gpu *gpu);
/* Enhanced: trace this frame's dynamic probes on the GPU for the gameplay
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
/* Cube faces redrawn by the most recent render (moving casters redraw only the
 * faces they touch). */
uint32_t LQ_GPUShadowFaceUpdates(const lq_gpu *gpu);
/* Dynamic probe layer for this map (see LQ_SceneDynamicProbes); NULL marks
 * it inactive so shaders skip it. */
size_t LQ_GPUDynamicProbeBytes(const lq_gpu *gpu);
int LQ_GPUDynamicProbes(lq_gpu *gpu, const uint8_t *pixels, char *error, size_t size);
int LQ_GPUReadPixels(lq_gpu *gpu, uint8_t **pixels, uint32_t *width, uint32_t *height, char *error, size_t size);
void LQ_GPUDestroy(lq_gpu *gpu);
#endif
