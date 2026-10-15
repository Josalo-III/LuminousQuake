/* GPL-2.0-or-later. Engine-owned frame adapter. */
#ifndef LQ_GPU_ENTITIES_H
#define LQ_GPU_ENTITIES_H
#include "scene.h"
void LQ_EntitiesReset(void);
void LQ_EntitiesBegin(const lq_scene *world);
void LQ_EntitiesBuild(const lq_scene *world);
void LQ_EntitiesParticle(const float origin[3], int color, int emissive, unsigned int seed);
const lq_frame *LQ_EntitiesFrame(void);
uint32_t LQ_AnimatedMaterial(const lq_scene *world, texture_t *texture);
#endif
