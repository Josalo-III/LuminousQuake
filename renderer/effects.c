/* GPL-2.0-or-later. */
#include "effects.h"
#include <math.h>
static uint32_t Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}
static float Random(uint32_t *state)
{ *state = Hash(*state + 0x9e3779b9u); return (*state & 0xffff) / 65535.0f; }
uint32_t LQ_ParticleCorners(uint32_t seed, float corners[LQ_PARTICLE_MAX_SIDES][2])
{
    uint32_t count = 3 + (Hash(seed ^ 0x51ed270bu) & 3);
    float sector = 6.2831853f / count;
    float angle = Random(&seed) * 6.2831853f;
    /* Normalize polygon area across side counts. Classic's half-square
     * triangle is the largest size: particles span 50-100% of its width, so a
     * quarter to all of its area, never more. */
    float scale = (0.25f + Random(&seed) * 0.25f) * sqrtf(4.0f / (count * sinf(sector)));
    float aspect = 0.6f + Random(&seed) * 0.8f, center[2] = {0};
    /* Separated angular sectors preserve convexity while varying outline. */
    for (uint32_t i = 0; i < count; ++i) {
        float a = angle + i * sector + (Random(&seed) - 0.5f) * sector * 0.22f;
        float radius = scale * (0.8f + Random(&seed) * 0.2f);
        corners[i][0] = cosf(a) * radius * aspect;
        corners[i][1] = sinf(a) * radius / aspect;
        center[0] += corners[i][0] / count; center[1] += corners[i][1] / count;
    }
    for (uint32_t i = 0; i < count; ++i) for (int k = 0; k < 2; ++k) corners[i][k] -= center[k];
    return count;
}
uint32_t LQ_BeamRuntimeLights(const float start[3], const float end[3], uint32_t key,
    lq_runtime_light output[LQ_BEAM_LIGHTS])
{
    float delta[3], distance = 0;
    for (int k = 0; k < 3; ++k) {
        if (!isfinite(start[k]) || !isfinite(end[k])) return 0;
        delta[k] = end[k] - start[k]; distance += delta[k] * delta[k];
    }
    distance = sqrtf(distance);
    if (!isfinite(distance)) return 0;
    uint32_t count = (uint32_t)fminf(fmaxf(ceilf(distance / 96.0f), 1), LQ_BEAM_LIGHTS);
    float spacing = distance / count, radius = fmaxf(160, spacing * 1.25f);
    static const float blue[3] = {0.45f, 0.65f, 1.0f};
    for (uint32_t i = 0; i < count; ++i) {
        float origin[3], t = (i + 0.5f) / count;
        for (int k = 0; k < 3; ++k) origin[k] = start[k] + delta[k] * t;
        LQ_DynamicRuntimeLight(&output[i], origin, radius, blue, 6, key + i);
        /* Keep output bounded as samples overlap or the beam grows longer. */
        output[i].falloff[0] = 160; output[i].falloff[1] = 160 / radius;
        output[i].color_strength[3] = 0.65f;
    }
    return count;
}
