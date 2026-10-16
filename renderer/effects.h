/* Luminous effect geometry and light placement. GPL-2.0-or-later. */
#ifndef LQ_EFFECTS_H
#define LQ_EFFECTS_H
#include "scene.h"
#define LQ_BEAM_LIGHTS 6
#define LQ_PARTICLE_MAX_SIDES 6
/* Returns 3-6 stable, centred convex corners, at most Classic's triangle in
 * area (0.5 for unit size); no gameplay random consumption. */
uint32_t LQ_ParticleCorners(uint32_t seed, float corners[LQ_PARTICLE_MAX_SIDES][2]);
#define LQ_PARTICLE_MAX_FACES 8
/* Returns the 4-8 outward-wound triangles of a stable convex solid centred on
 * the origin (a tetrahedron, triangular bipyramid or octahedron with varied
 * proportions), turned to its tumble at time. Its mean projected area matches
 * the flat particles' for unit size; no gameplay random consumption. */
uint32_t LQ_ParticleSolid(uint32_t seed, double time, float faces[LQ_PARTICLE_MAX_FACES][3][3]);
/* Unit icosphere (20 faces, or 80 when fine); positions are also normals. */
uint32_t LQ_ParticleSphere(int fine, const float (**faces)[3][3]);
/* A droplet's radius for unit size: its disc has the solids' mean area. */
float LQ_ParticleDropRadius(uint32_t seed);
/* Area of the solid's silhouette seen along unit direction. */
float LQ_ParticleProjectedArea(const float faces[][3][3], uint32_t count, const float direction[3]);
/* Samples cover the live beam, including short/zero-length impacts. */
uint32_t LQ_BeamRuntimeLights(const float start[3], const float end[3], uint32_t key,
    lq_runtime_light output[LQ_BEAM_LIGHTS]);
#endif
