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
/* Luminous: for a light whose static flame model sits at origin, its current
 * pose's strength relative to the cycle's mean and the world shift of its
 * emission centre. Returns 0 when no flame is there. */
int LQ_FlameLight(const float origin[3], float *gain, float offset[3]);
#endif
