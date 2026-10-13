/* SDL3 GPU implementation boundary. GPL-2.0-or-later. */
#ifndef LQ_GPU_SDL_H
#define LQ_GPU_SDL_H
#include "scene.h"
typedef struct SDL_Window SDL_Window;
typedef struct lq_gpu lq_gpu;
lq_gpu *LQ_GPUCreate(SDL_Window *window, int debug, char *error, size_t size);
int LQ_GPUUpload(lq_gpu *gpu, const lq_scene *scene, char *error, size_t size);
int LQ_GPURender(lq_gpu *gpu, const lq_view *view, uint32_t width, uint32_t height, char *error, size_t size);
int LQ_GPUReadPixels(lq_gpu *gpu, uint8_t **pixels, uint32_t *width, uint32_t *height, char *error, size_t size);
void LQ_GPUDestroy(lq_gpu *gpu);
#endif
