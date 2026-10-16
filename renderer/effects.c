/* GPL-2.0-or-later. */
#include "effects.h"
#include <math.h>
#include <string.h>
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
static void Cross(const float a[3], const float b[3], const float c[3], float out[3])
{
    float u[3], v[3];
    for (int k = 0; k < 3; ++k) { u[k] = b[k]-a[k]; v[k] = c[k]-a[k]; }
    out[0] = u[1]*v[2]-u[2]*v[1]; out[1] = u[2]*v[0]-u[0]*v[2]; out[2] = u[0]*v[1]-u[1]*v[0];
}
uint32_t LQ_ParticleSolid(uint32_t seed, double time, float faces[LQ_PARTICLE_MAX_FACES][3][3])
{
    static const uint8_t tetrahedron[4][3] = {{0,1,2},{0,3,1},{0,2,3},{1,3,2}};
    static const uint8_t bipyramid[6][3] = {{3,0,1},{3,1,2},{3,2,0},{4,1,0},{4,2,1},{4,0,2}};
    static const uint8_t octahedron[8][3] = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    float points[6][3] = {{0}};
    uint32_t shape = Hash(seed ^ 0x2c1b3c6du) % 3, count;
    const uint8_t (*triangles)[3];
    /* Radial variation of a convex solid's vertices about an interior centre
     * keeps it convex for these three: any four points, a bipyramid whose axis
     * pierces its equator, and an octahedron with its vertices on the axes. */
    if (shape == 0) {
        static const float corner[4][3] = {{1,1,1},{1,-1,-1},{-1,1,-1},{-1,-1,1}};
        for (int i = 0; i < 4; ++i) {
            float radius = (0.7f + Random(&seed) * 0.3f) * 0.57735027f;
            for (int k = 0; k < 3; ++k) points[i][k] = corner[i][k] * radius;
        }
        triangles = tetrahedron; count = 4;
    } else if (shape == 1) {
        for (int i = 0; i < 3; ++i) {
            float a = i * 2.0943951f + (Random(&seed) - 0.5f) * 0.4f, radius = 0.75f + Random(&seed) * 0.25f;
            points[i][0] = cosf(a) * radius; points[i][1] = sinf(a) * radius;
        }
        points[3][2] = 0.75f + Random(&seed) * 0.35f; points[4][2] = -(0.75f + Random(&seed) * 0.35f);
        triangles = bipyramid; count = 6;
    } else {
        for (int i = 0; i < 6; ++i) points[i][i/2] = (i & 1 ? -1 : 1) * (0.7f + Random(&seed) * 0.3f);
        triangles = octahedron; count = 8;
    }
    /* A positive stretch per axis keeps convexity and winding. */
    for (int k = 0; k < 3; ++k) {
        float stretch = 0.7f + Random(&seed) * 0.6f;
        for (int i = 0; i < 6; ++i) points[i][k] *= stretch;
    }
    float area = 0, center[3] = {0}, n[3];
    uint32_t vertices = shape == 0 ? 4 : shape == 1 ? 5 : 6;
    for (uint32_t i = 0; i < vertices; ++i) for (int k = 0; k < 3; ++k) center[k] += points[i][k] / vertices;
    for (uint32_t f = 0; f < count; ++f) {
        Cross(points[triangles[f][0]], points[triangles[f][1]], points[triangles[f][2]], n);
        area += 0.5f * sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    }
    /* Cauchy: a convex body's mean silhouette is a quarter of its surface. The
     * flat particles average about half of Classic's 0.5 triangle. */
    float target = 0.15f + Random(&seed) * 0.2f, scale = sqrtf(target / (area * 0.25f));
    /* Tumble: each axis spins at its own random rate and direction from a
     * random phase; double time keeps long sessions precise. */
    float c[3], s[3];
    for (int k = 0; k < 3; ++k) {
        double rate = (1.0 + Random(&seed) * 7.0) * (Random(&seed) < 0.5f ? -1 : 1);
        float angle = (float)fmod(Random(&seed) * 6.2831853 + rate * time, 6.283185307179586);
        c[k] = cosf(angle); s[k] = sinf(angle);
    }
    for (uint32_t i = 0; i < vertices; ++i) {
        float p[3], q;
        for (int k = 0; k < 3; ++k) p[k] = (points[i][k] - center[k]) * scale;
        q = p[1]*c[0] - p[2]*s[0]; p[2] = p[1]*s[0] + p[2]*c[0]; p[1] = q; /* about x */
        q = p[2]*c[1] - p[0]*s[1]; p[0] = p[2]*s[1] + p[0]*c[1]; p[2] = q; /* about y */
        q = p[0]*c[2] - p[1]*s[2]; p[1] = p[0]*s[2] + p[1]*c[2]; p[0] = q; /* about z */
        memcpy(points[i], p, sizeof(p));
    }
    for (uint32_t f = 0; f < count; ++f) for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) faces[f][j][k] = points[triangles[f][j]][k];
    return count;
}
static void Normalize(float v[3])
{
    float length = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    for (int k = 0; k < 3; ++k) v[k] /= length;
}
uint32_t LQ_ParticleSphere(int fine, const float (**faces)[3][3])
{
    static float coarse[20][3][3], detailed[80][3][3];
    static int built;
    if (!built) {
        static const uint8_t ico[20][3] = {{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},
            {10,7,6},{7,1,8},{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}};
        const float t = 1.6180340f;
        float points[12][3] = {{-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0},{0,-1,t},{0,1,t},{0,-1,-t},{0,1,-t},
            {t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1}};
        for (int i = 0; i < 12; ++i) Normalize(points[i]);
        for (int f = 0; f < 20; ++f) {
            float mid[3][3];
            for (int c = 0; c < 3; ++c) memcpy(coarse[f][c], points[ico[f][c]], sizeof(coarse[f][c]));
            for (int e = 0; e < 3; ++e) {
                for (int k = 0; k < 3; ++k) mid[e][k] = coarse[f][e][k] + coarse[f][(e+1)%3][k];
                Normalize(mid[e]);
            }
            /* Corner triangles keep the parent's winding; the centre joins the midpoints. */
            for (int c = 0; c < 3; ++c) {
                memcpy(detailed[f*4+c][0], coarse[f][c], sizeof(mid[0]));
                memcpy(detailed[f*4+c][1], mid[c], sizeof(mid[0]));
                memcpy(detailed[f*4+c][2], mid[(c+2)%3], sizeof(mid[0]));
            }
            memcpy(detailed[f*4+3], mid, sizeof(mid));
        }
        built = 1;
    }
    *faces = fine ? (const float (*)[3][3])detailed : (const float (*)[3][3])coarse;
    return fine ? 80 : 20;
}
float LQ_ParticleDropRadius(uint32_t seed)
{
    seed = Hash(seed ^ 0x6a09e667u);
    return sqrtf((0.15f + Random(&seed) * 0.2f) / 3.14159265f);
}
float LQ_ParticleProjectedArea(const float faces[][3][3], uint32_t count, const float direction[3])
{
    /* Each silhouette point is covered by one front and one back face. */
    float area = 0, n[3];
    for (uint32_t f = 0; f < count; ++f) {
        Cross(faces[f][0], faces[f][1], faces[f][2], n);
        area += fabsf(n[0]*direction[0] + n[1]*direction[1] + n[2]*direction[2]);
    }
    return area * 0.25f;
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
