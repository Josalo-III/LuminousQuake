/* Luminous effect geometry and light placement. GPL-2.0-or-later. */
#ifndef LQ_EFFECTS_H
#define LQ_EFFECTS_H
#include "scene.h"
#define LQ_BEAM_LIGHTS 6
#define LQ_PARTICLE_MAX_SIDES 6
/* Returns 3-6 stable, centred convex corners; no gameplay random consumption. */
uint32_t LQ_ParticleCorners(uint32_t seed, float corners[LQ_PARTICLE_MAX_SIDES][2]);
/* Samples cover the live beam, including short/zero-length impacts. */
uint32_t LQ_BeamRuntimeLights(const float start[3], const float end[3], uint32_t key,
    lq_runtime_light output[LQ_BEAM_LIGHTS]);
#endif
