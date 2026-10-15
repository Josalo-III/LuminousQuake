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
/* Shadow cube slots redrawn by the most recent render; unchanged slots reuse
 * their cached static-world shadows. */
uint32_t LQ_GPUShadowUpdates(const lq_gpu *gpu);
/* Dynamic probe layer for this map (see LQ_SceneDynamicProbes); NULL marks
 * it inactive so shaders skip it. */
size_t LQ_GPUDynamicProbeBytes(const lq_gpu *gpu);
int LQ_GPUDynamicProbes(lq_gpu *gpu, const uint8_t *pixels, char *error, size_t size);
int LQ_GPUReadPixels(lq_gpu *gpu, uint8_t **pixels, uint32_t *width, uint32_t *height, char *error, size_t size);
void LQ_GPUDestroy(lq_gpu *gpu);
#endif
