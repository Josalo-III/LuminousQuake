/* Luminous static-world bounce light from runtime light sources.
 * Direct light at LIGHT's sample lattice is computed from each source's own
 * falloff with BSP occlusion, reflected by the face's texture color, and
 * gathered once by cosine-weighted rays. The baked lightmap is never read.
 * The sky is an area light: each sample's cosine-weighted sky visibility is
 * kept for the shader, which scales it by the sky's brightness, and the
 * sky-lit surfaces' reflection joins the style 0 bounce. An ericw-tools sun
 * is a directional light: each sample keeps the fraction of rays across its
 * penumbra that reach sky, and sunlit surfaces reflect into style 0 too.
 * An lq_bounce_tracer may trace the sky, sun, direct-light and gather passes
 * instead (the GPU's renderer/shaders/bounce.comp follows this file); this
 * code then only keeps each face's styles. Every pass remains here as the
 * reference and the fallback. GPL-2.0-or-later. */
#include "scene.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE_STYLES 4
#define CANDIDATE_STYLES 16
#define RAY_LENGTH 4096.0f
#define SUN_RAYS 16u
#define SUN_RAY_LENGTH 16384.0f

typedef struct {
    lq_scene *scene;
    float (*emit)[FACE_STYLES][3]; /* exitance per sample and face style slot */
    uint8_t (*emit_styles)[FACE_STYLES];
    float (*gather)[FACE_STYLES][3];
    uint8_t (*gather_styles)[FACE_STYLES];
    float *sky; /* sky visibility per sample, 0-1 */
    uint32_t rays;
    float *sun; /* sun visibility per sample, 0-1; NULL without a sun */
    const lq_bounce_entry *entries; /* traced gather per sample, or NULL to trace here */
    const lq_bounce_entry *direct; /* traced light per sample, or NULL to trace here */
} bounce_context;

static float Dot(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void Serial(size_t count, size_t grain, lq_range_fn function, void *context);
static uint8_t Contents(const lq_scene *s, int32_t node)
{
    uint32_t leaf = (uint32_t)(-(node + 1));
    return leaf < s->leaf_count ? s->leaf_contents[leaf] : LQ_LEAF_SOLID;
}
static int Blocking(const lq_scene *s, int32_t node)
{
    uint8_t contents = Contents(s, node);
    return contents == LQ_LEAF_SOLID || contents == LQ_LEAF_SKY;
}
/* True when no solid leaf lies on the segment. Light sources pass through
 * sky, as LIGHT's TestLine stops only at solid; gathering rays end at sky. */
static int SegmentClear(const lq_scene *s, int32_t node, const float a[3], const float b[3], int depth)
{
    while (node >= 0) {
        if (depth > 4096 || (uint32_t)node >= s->bsp_node_count) return 0;
        const lq_bsp_node *n = &s->bsp_nodes[node];
        float da = Dot(n->normal,a) - n->dist, db = Dot(n->normal,b) - n->dist;
        if (da >= 0 && db >= 0) { node = n->children[0]; continue; }
        if (da < 0 && db < 0) { node = n->children[1]; continue; }
        float fraction = da / (da - db), middle[3];
        for (int i = 0; i < 3; ++i) middle[i] = a[i] + fraction * (b[i] - a[i]);
        int side = da < 0;
        return SegmentClear(s, n->children[side], a, middle, depth + 1) &&
            SegmentClear(s, n->children[!side], middle, b, depth + 1);
    }
    return Contents(s, node) != LQ_LEAF_SOLID;
}
/* Convex face containment on the face plane (vertices are a triangle fan). */
static int FaceContains(const lq_scene *s, uint32_t face, const float point[3])
{
    const lq_draw *draw = &s->draws[face];
    uint32_t count = draw->index_count / 3 + 2, base = s->indices[draw->first_index];
    const float *normal = s->vertices[base].normal;
    int inside = 0, outside = 0; /* either winding: inside is one consistent side */
    for (uint32_t i = 0; i < count; ++i) {
        const float *a = s->vertices[base + i].position, *b = s->vertices[base + (i + 1) % count].position;
        float edge[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]}, to[3] = {point[0]-a[0], point[1]-a[1], point[2]-a[2]};
        float cross[3] = {edge[1]*to[2]-edge[2]*to[1], edge[2]*to[0]-edge[0]*to[2], edge[0]*to[1]-edge[1]*to[0]};
        float side = Dot(cross, normal), tolerance = 0.01f * sqrtf(Dot(edge, edge));
        inside += side > tolerance; outside += side < -tolerance;
    }
    return !(inside && outside);
}
/* First lit face whose front a ray meets, in the manner of RecursiveLightPoint
 * but with exact polygon containment. Returns 1 on a hit, -1 when solid stops
 * the ray first, -2 when it reaches sky, 0 when it leaves the segment unobstructed.
 * Sky is the front of a sky face or a sky leaf: id's qbsp sealed sky brushes as
 * solid (original maps have no sky leaves), while ericw-tools keeps them. */
enum { TRACE_SKY = -2 };
static int TraceFace(const lq_scene *s, int32_t node, const float a[3], const float b[3],
    const float direction[3], uint32_t *face, float hit[3], int depth)
{
    while (node >= 0) {
        if (depth > 4096 || (uint32_t)node >= s->bsp_node_count) return -1;
        const lq_bsp_node *n = &s->bsp_nodes[node];
        float da = Dot(n->normal,a) - n->dist, db = Dot(n->normal,b) - n->dist;
        if (da >= 0 && db >= 0) { node = n->children[0]; continue; }
        if (da < 0 && db < 0) { node = n->children[1]; continue; }
        float fraction = da / (da - db), middle[3];
        for (int i = 0; i < 3; ++i) middle[i] = a[i] + fraction * (b[i] - a[i]);
        int side = da < 0;
        int result = TraceFace(s, n->children[side], a, middle, direction, face, hit, depth + 1);
        if (result) return result;
        for (uint32_t i = 0; i < n->face_count; ++i) {
            uint32_t candidate = n->first_face + i;
            const lq_draw *draw = &s->draws[candidate];
            int sky = (draw->flags & LQ_SURFACE_SKY) != 0;
            if ((draw->flags && !sky) || (!sky && !s->face_samples[candidate].width)) continue;
            if (Dot(s->vertices[s->indices[draw->first_index]].normal, direction) >= 0) continue;
            if (!FaceContains(s, candidate, middle)) continue;
            if (sky) return TRACE_SKY;
            *face = candidate; memcpy(hit, middle, sizeof(middle)); return 1;
        }
        return TraceFace(s, n->children[!side], middle, b, direction, face, hit, depth + 1);
    }
    if (!Blocking(s, node)) return 0;
    uint32_t leaf = (uint32_t)(-(node + 1));
    return leaf < s->leaf_count && s->leaf_contents[leaf] == LQ_LEAF_SKY ? TRACE_SKY : -1;
}
static int ValidSample(const float point[3]) { return isfinite(point[0]); }
/* Keep the strongest styles; weaker ones are folded away (rare: few faces see
 * more than four distinct styles). */
typedef struct { uint8_t style; float energy; } style_energy;
static uint32_t ChooseStyles(const style_energy *candidates, uint32_t count, uint8_t out[FACE_STYLES])
{
    uint32_t order[CANDIDATE_STYLES], used = 0;
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    for (uint32_t i = 0; i < count; ++i) for (uint32_t j = i + 1; j < count; ++j)
        if (candidates[order[j]].energy > candidates[order[i]].energy) { uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
    memset(out, 255, FACE_STYLES);
    for (uint32_t i = 0; i < count && used < FACE_STYLES; ++i)
        if (candidates[order[i]].energy > 0) out[used++] = candidates[order[i]].style;
    return used;
}
static uint32_t Candidate(style_energy *candidates, uint32_t *count, uint8_t style)
{
    for (uint32_t i = 0; i < *count; ++i) if (candidates[i].style == style) return i;
    if (*count == CANDIDATE_STYLES) return UINT32_MAX;
    candidates[*count] = (style_energy){style, 0};
    return (*count)++;
}
/* Pass 1: exitance (direct brightness times texture color) per sample/style. */
static void EmitFaces(void *opaque, size_t begin, size_t end)
{
    bounce_context *c = opaque;
    const lq_scene *s = c->scene;
    float (*accumulate)[CANDIDATE_STYLES][3] = NULL;
    size_t capacity = 0;
    for (size_t f = begin; f < end; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        uint32_t samples = (uint32_t)grid->width * grid->height;
        if (!samples) continue;
        if (samples > capacity) {
            free(accumulate); capacity = samples;
            accumulate = malloc(capacity * sizeof(*accumulate));
            if (!accumulate) return;
        }
        memset(accumulate, 0, samples * sizeof(*accumulate));
        style_energy candidates[CANDIDATE_STYLES]; uint32_t candidate_count = 0;
        const lq_draw *draw = &s->draws[f];
        const float *normal = s->vertices[s->indices[draw->first_index]].normal;
        for (uint32_t i = 0; c->direct && i < samples; ++i) {
            if (!ValidSample(s->sample_positions[grid->first_sample + i])) continue;
            const lq_bounce_entry *e = c->direct + (size_t)(grid->first_sample + i) * LQ_BOUNCE_ENTRIES;
            for (int j = 0; j < LQ_BOUNCE_ENTRIES && e[j].style < 255; ++j) {
                uint32_t slot = Candidate(candidates, &candidate_count, (uint8_t)e[j].style);
                if (slot == UINT32_MAX) continue;
                for (int k = 0; k < 3; ++k) {
                    float exitance = e[j].rgb[k] * grid->albedo[k];
                    accumulate[i][slot][k] += exitance; candidates[slot].energy += exitance;
                }
            }
        }
        for (uint32_t l = 0; !c->direct && l < s->light_source_count; ++l) {
            const lq_light_source *light = &s->light_sources[l];
            if (s->light_visibility && !(s->light_visibility[f * s->light_visibility_words + l / 32] & (1u << (l % 32)))) continue;
            float range = LQ_LightSourceRange(light);
            if (range <= 0) continue;
            uint32_t slot = UINT32_MAX;
            for (uint32_t i = 0; i < samples; ++i) {
                const float *point = s->sample_positions[grid->first_sample + i];
                if (!ValidSample(point)) continue;
                float toward[3] = {light->origin[0]-point[0], light->origin[1]-point[1], light->origin[2]-point[2]};
                float distance = sqrtf(Dot(toward, toward));
                if (distance >= range || distance < 0.001f) continue;
                for (int k = 0; k < 3; ++k) toward[k] /= distance;
                float facing = Dot(normal, toward);
                if (facing <= 0) continue;
                float value = LQ_LightFalloff(light, distance) *
                    ((1 - light->angle_scale) + light->angle_scale * facing) * light->output_scale;
                if (light->spotlight) value *= LQ_SpotFactor(toward, distance, light->spot_direction,
                    light->spot_outer, light->spot_inner, light->source_radius);
                if (value < 0.01f || !SegmentClear(s, 0, light->origin, point, 0)) continue;
                if (slot == UINT32_MAX && (slot = Candidate(candidates, &candidate_count, (uint8_t)light->style)) == UINT32_MAX) break;
                for (int k = 0; k < 3; ++k) {
                    float exitance = value * light->color[k] * grid->albedo[k];
                    accumulate[i][slot][k] += exitance; candidates[slot].energy += exitance;
                }
            }
        }
        /* Sky light and sunlight are constant, so their reflection belongs to style 0. */
        uint32_t slot = UINT32_MAX;
        float toward_sun = -Dot(normal, s->sun_direction);
        float sun_incidence = (1 - s->sun_angle_scale) + s->sun_angle_scale * toward_sun;
        for (uint32_t i = 0; c->sun && toward_sun > 0 && i < samples; ++i) {
            float visible = c->sun[grid->first_sample + i];
            if (visible <= 0 || !ValidSample(s->sample_positions[grid->first_sample + i])) continue;
            if (slot == UINT32_MAX && (slot = Candidate(candidates, &candidate_count, 0)) == UINT32_MAX) break;
            for (int k = 0; k < 3; ++k) {
                float exitance = visible * sun_incidence * s->sun_light[k] * grid->albedo[k];
                accumulate[i][slot][k] += exitance; candidates[slot].energy += exitance;
            }
        }
        for (uint32_t i = 0; c->sky && i < samples; ++i) {
            float visible = c->sky[grid->first_sample + i];
            if (visible <= 0 || !ValidSample(s->sample_positions[grid->first_sample + i])) continue;
            if (slot == UINT32_MAX && (slot = Candidate(candidates, &candidate_count, 0)) == UINT32_MAX) break;
            for (int k = 0; k < 3; ++k) {
                float exitance = visible * s->sky_light[k] * grid->albedo[k];
                accumulate[i][slot][k] += exitance; candidates[slot].energy += exitance;
            }
        }
        ChooseStyles(candidates, candidate_count, c->emit_styles[f]);
        for (int k = 0; k < FACE_STYLES; ++k) {
            uint8_t style = c->emit_styles[f][k];
            uint32_t from = 0;
            while (from < candidate_count && candidates[from].style != style) ++from;
            for (uint32_t i = 0; i < samples; ++i)
                for (int j = 0; j < 3; ++j)
                    c->emit[grid->first_sample + i][k][j] = style == 255 || from == candidate_count ? 0 : accumulate[i][from][j];
        }
    }
    free(accumulate);
}
static float Radical(uint32_t bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return bits * 2.3283064365386963e-10f;
}
static uint32_t Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x;
}
/* Orthonormal frame about a face normal for hemisphere sampling. */
static void Basis(const float normal[3], float tangent[3], float bitangent[3])
{
    float helper[3] = {fabsf(normal[2]) < 0.9f ? 0.0f : 1.0f, 0, fabsf(normal[2]) < 0.9f ? 1.0f : 0.0f};
    tangent[0] = helper[1]*normal[2]-helper[2]*normal[1];
    tangent[1] = helper[2]*normal[0]-helper[0]*normal[2];
    tangent[2] = helper[0]*normal[1]-helper[1]*normal[0];
    float length = sqrtf(Dot(tangent, tangent));
    for (int k = 0; k < 3; ++k) tangent[k] /= length;
    bitangent[0] = normal[1]*tangent[2]-normal[2]*tangent[1];
    bitangent[1] = normal[2]*tangent[0]-normal[0]*tangent[2];
    bitangent[2] = normal[0]*tangent[1]-normal[1]*tangent[0];
}
/* Ray r of `rays` cosine-weighted directions, stratified and rotated per sample. */
static void CosineDirection(const float normal[3], const float tangent[3], const float bitangent[3],
    uint32_t r, uint32_t rays, uint32_t seed, float direction[3])
{
    float u = (r + 0.5f) / rays + (seed & 0xffff) / 65536.0f, v = Radical(r) + (seed >> 16) / 65536.0f;
    u -= floorf(u); v -= floorf(v);
    float radius = sqrtf(u), angle = 6.2831853f * v;
    float a = radius * cosf(angle), b = radius * sinf(angle), h = sqrtf(fmaxf(1 - u, 0));
    for (int k = 0; k < 3; ++k) direction[k] = tangent[k]*a + bitangent[k]*b + normal[k]*h;
}
/* Buried samples borrow the nearest valid neighbour, as bounce does. */
static void BorrowSky(const lq_scene *s, const lq_face_samples *grid, float *visible)
{
    uint32_t samples = (uint32_t)grid->width * grid->height;
    for (uint32_t i = 0; i < samples; ++i) {
        if (ValidSample(s->sample_positions[grid->first_sample + i])) continue;
        int x = (int)(i % grid->width), y = (int)(i / grid->width), best = -1, best_distance = 1 << 30;
        for (uint32_t j = 0; j < samples; ++j) {
            if (!ValidSample(s->sample_positions[grid->first_sample + j])) continue;
            int dx = (int)(j % grid->width) - x, dy = (int)(j / grid->width) - y;
            if (dx*dx + dy*dy < best_distance) { best_distance = dx*dx + dy*dy; best = (int)j; }
        }
        if (best >= 0) visible[i] = visible[best];
    }
}
/* Pass 0: sky visibility. With cosine-weighted rays the fraction reaching sky
 * is the irradiance from a uniform sky of unit radiance, divided by pi. */
static void SkyFaces(void *opaque, size_t begin, size_t end)
{
    bounce_context *c = opaque;
    const lq_scene *s = c->scene;
    for (size_t f = begin; f < end; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        uint32_t samples = (uint32_t)grid->width * grid->height;
        if (!samples) continue;
        const float *normal = s->vertices[s->indices[s->draws[f].first_index]].normal;
        float tangent[3], bitangent[3];
        Basis(normal, tangent, bitangent);
        float *visible = c->sky + grid->first_sample;
        for (uint32_t i = 0; i < samples; ++i) {
            const float *point = s->sample_positions[grid->first_sample + i];
            visible[i] = 0;
            if (!ValidSample(point)) continue;
            uint32_t seed = Hash((uint32_t)(grid->first_sample + i) * 0x9e3779b9u ^ 0x632be5abu), hits = 0;
            for (uint32_t r = 0; r < c->rays; ++r) {
                float direction[3], target[3], hit[3]; uint32_t face;
                CosineDirection(normal, tangent, bitangent, r, c->rays, seed, direction);
                for (int k = 0; k < 3; ++k) target[k] = point[k] + direction[k] * RAY_LENGTH;
                hits += TraceFace(s, 0, point, target, direction, &face, hit, 0) == TRACE_SKY;
            }
            visible[i] = (float)hits / c->rays;
        }
        BorrowSky(s, grid, visible);
    }
}
/* Pass 0 after GPU tracing: only the borrowing. */
static void SkyBorrowFaces(void *opaque, size_t begin, size_t end)
{
    bounce_context *c = opaque;
    for (size_t f = begin; f < end; ++f) {
        const lq_face_samples *grid = &c->scene->face_samples[f];
        if (grid->width) BorrowSky(c->scene, grid, c->sky + grid->first_sample);
    }
}
/* Bilinear per-sample value at a hit point for one of a face's style slots. */
static void HitValue(const lq_scene *s, const float (*values)[FACE_STYLES][3], uint32_t face,
    const float hit[3], int slot, float out[3])
{
    const lq_face_samples *grid = &s->face_samples[face];
    float u = (Dot(hit, grid->s) + grid->s[3] - grid->mins[0]) / 16.0f;
    float v = (Dot(hit, grid->t) + grid->t[3] - grid->mins[1]) / 16.0f;
    u = fminf(fmaxf(u, 0), (float)(grid->width - 1)); v = fminf(fmaxf(v, 0), (float)(grid->height - 1));
    int x = (int)u, y = (int)v, x1 = x + 1 < grid->width ? x + 1 : x, y1 = y + 1 < grid->height ? y + 1 : y;
    float fx = u - x, fy = v - y;
    const float (*e)[FACE_STYLES][3] = values + grid->first_sample;
    for (int k = 0; k < 3; ++k) {
        float top = e[y*grid->width + x][slot][k] * (1-fx) + e[y*grid->width + x1][slot][k] * fx;
        float bottom = e[y1*grid->width + x][slot][k] * (1-fx) + e[y1*grid->width + x1][slot][k] * fx;
        out[k] = top * (1-fy) + bottom * fy;
    }
}
/* Pass 2: one cosine-weighted gather per sample. With cosine-weighted rays and
 * Lambertian emitters, irradiance is the mean exitance of the surfaces hit. */
static void GatherFaces(void *opaque, size_t begin, size_t end)
{
    bounce_context *c = opaque;
    const lq_scene *s = c->scene;
    float (*accumulate)[CANDIDATE_STYLES][3] = NULL;
    size_t capacity = 0;
    for (size_t f = begin; f < end; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        uint32_t samples = (uint32_t)grid->width * grid->height;
        if (!samples) continue;
        if (samples > capacity) {
            free(accumulate); capacity = samples;
            accumulate = malloc(capacity * sizeof(*accumulate));
            if (!accumulate) return;
        }
        memset(accumulate, 0, samples * sizeof(*accumulate));
        style_energy candidates[CANDIDATE_STYLES]; uint32_t candidate_count = 0;
        const lq_draw *draw = &s->draws[f];
        const float *normal = s->vertices[s->indices[draw->first_index]].normal;
        float tangent[3], bitangent[3];
        Basis(normal, tangent, bitangent);
        for (uint32_t i = 0; i < samples; ++i) {
            const float *point = s->sample_positions[grid->first_sample + i];
            if (!ValidSample(point)) continue;
            if (c->entries) {
                const lq_bounce_entry *e = c->entries + (size_t)(grid->first_sample + i) * LQ_BOUNCE_ENTRIES;
                for (int j = 0; j < LQ_BOUNCE_ENTRIES && e[j].style < 255; ++j) {
                    uint32_t at = Candidate(candidates, &candidate_count, (uint8_t)e[j].style);
                    if (at == UINT32_MAX) continue;
                    for (int k = 0; k < 3; ++k) { accumulate[i][at][k] += e[j].rgb[k]; candidates[at].energy += e[j].rgb[k]; }
                }
                continue;
            }
            uint32_t seed = Hash((uint32_t)(grid->first_sample + i) * 0x9e3779b9u);
            for (uint32_t r = 0; r < c->rays; ++r) {
                float direction[3], target[3], hit[3];
                CosineDirection(normal, tangent, bitangent, r, c->rays, seed, direction);
                for (int k = 0; k < 3; ++k) target[k] = point[k] + direction[k] * RAY_LENGTH;
                uint32_t face;
                if (TraceFace(s, 0, point, target, direction, &face, hit, 0) != 1 || face == f) continue;
                for (int slot = 0; slot < FACE_STYLES; ++slot) {
                    uint8_t style = c->emit_styles[face][slot];
                    if (style == 255) continue;
                    uint32_t at = Candidate(candidates, &candidate_count, style);
                    if (at == UINT32_MAX) continue;
                    float exitance[3];
                    HitValue(s, (const float (*)[FACE_STYLES][3])c->emit, face, hit, slot, exitance);
                    for (int k = 0; k < 3; ++k) { accumulate[i][at][k] += exitance[k]; candidates[at].energy += exitance[k]; }
                }
            }
        }
        ChooseStyles(candidates, candidate_count, c->gather_styles[f]);
        float scale = s->bounce_scale / (float)c->rays;
        for (int k = 0; k < FACE_STYLES; ++k) {
            uint8_t style = c->gather_styles[f][k];
            uint32_t from = 0;
            while (from < candidate_count && candidates[from].style != style) ++from;
            for (uint32_t i = 0; i < samples; ++i)
                for (int j = 0; j < 3; ++j)
                    c->gather[grid->first_sample + i][k][j] = style == 255 || from == candidate_count ? 0 :
                        accumulate[i][from][j] * scale;
        }
        /* Samples buried in solid borrow the nearest valid neighbour so the
         * filtered atlas has no dark seams. */
        for (uint32_t i = 0; i < samples; ++i) {
            if (ValidSample(s->sample_positions[grid->first_sample + i])) continue;
            int x = (int)(i % grid->width), y = (int)(i / grid->width), best = -1, best_distance = 1 << 30;
            for (uint32_t j = 0; j < samples; ++j) {
                if (!ValidSample(s->sample_positions[grid->first_sample + j])) continue;
                int dx = (int)(j % grid->width) - x, dy = (int)(j / grid->width) - y;
                if (dx*dx + dy*dy < best_distance) { best_distance = dx*dx + dy*dy; best = (int)j; }
            }
            if (best >= 0) memcpy(c->gather[grid->first_sample + i], c->gather[grid->first_sample + best], sizeof(c->gather[0]));
        }
    }
    free(accumulate);
}
/* ---- Irradiance probes ---------------------------------------------------- */
#define PROBE_SPACING 64.0f
#define PROBE_LIMIT 262144u
#define PROBE_RAYS 96u
typedef struct {
    const lq_scene *scene;
    const bounce_context *bounce; /* static: face exitance and bounce */
    uint8_t layer_of_style[256];
    const lq_runtime_light *lights[LQ_MAX_DYNAMIC_LIGHTS]; /* dynamic */
    uint32_t light_count, rays, box_min[3], box_size[3];
    uint8_t *output;
    uint32_t layers;
} probe_context;
static void ProbePosition(const lq_scene *s, uint32_t x, uint32_t y, uint32_t z, float out[3])
{
    out[0] = s->probe_origin[0] + x * s->probe_spacing;
    out[1] = s->probe_origin[1] + y * s->probe_spacing;
    out[2] = s->probe_origin[2] + z * s->probe_spacing;
}
/* Even sphere coverage (Fibonacci lattice), rotated per probe about Z. */
static void SphereDirection(uint32_t i, uint32_t count, float rotation, float out[3])
{
    float z = 1 - (2.0f * i + 1) / count, radius = sqrtf(fmaxf(1 - z*z, 0));
    float angle = i * 2.39996323f + rotation;
    out[0] = radius * cosf(angle); out[1] = radius * sinf(angle); out[2] = z;
}
/* Light arriving from direction d adds irradiance to the cube faces it faces. */
static void CubeAdd(float cube[6][3], const float d[3], const float value[3])
{
    for (int axis = 0; axis < 3; ++axis) {
        float weight = fabsf(d[axis]);
        float *face = cube[axis*2 + (d[axis] < 0)];
        for (int k = 0; k < 3; ++k) face[k] += value[k] * weight;
    }
}
/* Fraction of rays toward the sun, spread evenly over its penumbra disc,
 * that reach sky: ericw-tools lights a sample from a sun only when its ray
 * meets sky. */
static float SunVisibility(const lq_scene *s, const float point[3], uint32_t seed)
{
    float toward[3] = {-s->sun_direction[0], -s->sun_direction[1], -s->sun_direction[2]}, tangent[3], bitangent[3];
    Basis(toward, tangent, bitangent);
    float spread = tanf(s->sun_penumbra * 0.5f * 0.017453293f), turn = (seed & 0xffff) / 65536.0f;
    uint32_t rays = spread > 0 ? SUN_RAYS : 1, hits = 0;
    for (uint32_t r = 0; r < rays; ++r) {
        float radius = sqrtf((r + 0.5f) / rays) * spread, angle = 6.2831853f * (Radical(r) + turn);
        float direction[3], target[3], hit[3], length = 0; uint32_t face;
        for (int k = 0; k < 3; ++k) {
            direction[k] = toward[k] + (tangent[k] * cosf(angle) + bitangent[k] * sinf(angle)) * (rays > 1 ? radius : 0);
            length += direction[k] * direction[k];
        }
        for (int k = 0; k < 3; ++k) { direction[k] /= sqrtf(length); target[k] = point[k] + direction[k] * SUN_RAY_LENGTH; }
        hits += TraceFace(s, 0, point, target, direction, &face, hit, 0) == TRACE_SKY;
    }
    return (float)hits / rays;
}
/* Pass 0b: sun visibility for samples on faces turned toward the sun. */
static void SunFaces(void *opaque, size_t begin, size_t end)
{
    bounce_context *c = opaque;
    const lq_scene *s = c->scene;
    for (size_t f = begin; f < end; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        uint32_t samples = (uint32_t)grid->width * grid->height;
        if (!samples) continue;
        const float *normal = s->vertices[s->indices[s->draws[f].first_index]].normal;
        int facing = Dot(normal, s->sun_direction) < 0;
        for (uint32_t i = 0; i < samples; ++i) {
            const float *point = s->sample_positions[grid->first_sample + i];
            c->sun[grid->first_sample + i] = facing && ValidSample(point) ?
                SunVisibility(s, point, Hash((uint32_t)(grid->first_sample + i) * 0x85ebca6bu ^ 0x27d4eb2fu)) : 0;
        }
    }
}
/* Alpha holds `alpha` (per direction, 0-1) when given, else 255. */
static void StoreCube(const probe_context *c, uint32_t x, uint32_t y, uint32_t z, uint32_t layer,
    float cube[6][3], float scale, const float *alpha)
{
    const lq_scene *s = c->scene;
    for (uint32_t d = 0; d < 6; ++d) {
        size_t depth = (size_t)(layer*6 + d) * s->probe_size[2] + z;
        uint8_t *texel = c->output + ((depth * s->probe_size[1] + y) * s->probe_size[0] + x) * 4;
        for (int k = 0; k < 3; ++k) texel[k] = (uint8_t)fminf(fmaxf(cube[d][k] * scale + 0.5f, 0), 255);
        texel[3] = alpha ? (uint8_t)fminf(fmaxf(alpha[d] * 255 + 0.5f, 0), 255) : 255;
    }
}
static void StaticProbes(void *opaque, size_t begin, size_t end)
{
    const probe_context *c = opaque;
    const lq_scene *s = c->scene;
    const bounce_context *b = c->bounce;
    for (size_t p = begin; p < end; ++p) {
        if (!s->probe_valid[p]) continue;
        uint32_t x = (uint32_t)(p % s->probe_size[0]), y = (uint32_t)(p / s->probe_size[0] % s->probe_size[1]);
        uint32_t z = (uint32_t)(p / ((size_t)s->probe_size[0] * s->probe_size[1]));
        float origin[3], cube[FACE_STYLES][6][3], sky[6][3];
        memset(cube, 0, sizeof(cube)); memset(sky, 0, sizeof(sky));
        ProbePosition(s, x, y, z, origin);
        float rotation = (Hash((uint32_t)p) & 0xffff) / 65536.0f * 6.2831853f;
        for (uint32_t r = 0; r < c->rays; ++r) {
            float d[3], target[3], hit[3]; uint32_t face;
            SphereDirection(r, c->rays, rotation, d);
            for (int k = 0; k < 3; ++k) target[k] = origin[k] + d[k] * RAY_LENGTH;
            int result = TraceFace(s, 0, origin, target, d, &face, hit, 0);
            if (result == TRACE_SKY) { static const float one[3] = {1, 1, 1}; CubeAdd(sky, d, one); }
            if (result != 1) continue;
            const float *albedo = s->face_samples[face].albedo;
            for (int slot = 0; slot < FACE_STYLES; ++slot) {
                float value[3];
                uint8_t style = b->emit_styles[face][slot], layer = style == 255 ? 255 : c->layer_of_style[style];
                if (layer != 255) {
                    HitValue(s, (const float (*)[FACE_STYLES][3])b->emit, face, hit, slot, value);
                    CubeAdd(cube[layer], d, value);
                }
                style = b->gather_styles[face][slot]; layer = style == 255 ? 255 : c->layer_of_style[style];
                if (layer != 255) {
                    HitValue(s, (const float (*)[FACE_STYLES][3])b->gather, face, hit, slot, value);
                    for (int k = 0; k < 3; ++k) value[k] *= albedo[k];
                    CubeAdd(cube[layer], d, value);
                }
            }
        }
        /* Irradiance E(n) = (4/N) sum M max(0, d.n) for Lambertian exitance M. */
        float scale = 4.0f / c->rays * s->bounce_scale, visible[6], sun[6];
        for (int d = 0; d < 6; ++d) visible[d] = sky[d][0] * 4.0f / c->rays;
        /* Sun visibility at the probe; the shader applies each normal's incidence. */
        int has_sun = b->sun != NULL;
        sun[0] = has_sun ? SunVisibility(s, origin, Hash((uint32_t)p ^ 0x165667b1u)) : 0;
        for (int d = 1; d < 6; ++d) sun[d] = sun[0];
        for (uint32_t layer = 0; layer < c->layers; ++layer)
            StoreCube(c, x, y, z, layer, cube[layer], scale, layer == 0 ? visible : layer == 1 && has_sun ? sun : NULL);
    }
}
/* Probes inside solid take the mean of filled neighbours, so trilinear
 * filtering near walls does not pull in black. */
static void DilateProbes(const lq_scene *s, uint8_t *texels, uint32_t blocks, uint8_t *filled)
{
    uint32_t nx = s->probe_size[0], ny = s->probe_size[1], nz = s->probe_size[2];
    size_t count = (size_t)nx * ny * nz;
    for (int pass = 0; pass < 4; ++pass) {
        uint8_t *next = malloc(count);
        if (!next) return;
        memcpy(next, filled, count);
        for (size_t p = 0; p < count; ++p) {
            if (filled[p]) continue;
            int x = (int)(p % nx), y = (int)(p / nx % ny), z = (int)(p / ((size_t)nx * ny));
            static const int offsets[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
            size_t neighbours[6]; int found = 0;
            for (int i = 0; i < 6; ++i) {
                int a = x + offsets[i][0], b = y + offsets[i][1], c = z + offsets[i][2];
                if (a < 0 || b < 0 || c < 0 || a >= (int)nx || b >= (int)ny || c >= (int)nz) continue;
                size_t q = (size_t)a + (size_t)nx * ((size_t)b + (size_t)ny * c);
                if (filled[q]) neighbours[found++] = q;
            }
            if (!found) continue;
            for (uint32_t block = 0; block < blocks; ++block) {
                uint8_t *target = texels + (((size_t)block * nz + z) * ny * nx + (size_t)y * nx + x) * 4;
                for (int k = 0; k < 4; ++k) {
                    unsigned sum = 0;
                    for (int i = 0; i < found; ++i) {
                        size_t q = neighbours[i];
                        uint32_t qz = (uint32_t)(q / ((size_t)nx * ny)), qy = (uint32_t)(q / nx % ny), qx = (uint32_t)(q % nx);
                        sum += texels[(((size_t)block * nz + qz) * ny * nx + (size_t)qy * nx + qx) * 4 + k];
                    }
                    target[k] = (uint8_t)(sum / found);
                }
            }
            next[p] = 1;
        }
        memcpy(filled, next, count); free(next);
    }
}
/* Map-wide probe layers: style 0 first, then the three strongest other
 * styles. Unchosen animated styles (below 32) fold into style 0; unchosen
 * switchable styles are left out rather than shown while switched off. */
static uint32_t ProbeStyles(const bounce_context *c, uint8_t styles[FACE_STYLES], uint8_t layer_of_style[256])
{
    const lq_scene *s = c->scene;
    double energy[256] = {0};
    for (uint32_t f = 0; f < s->draw_count; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        for (uint32_t i = 0; grid->width && i < (uint32_t)grid->width * grid->height; ++i)
            for (int slot = 0; slot < FACE_STYLES; ++slot) {
                const float *e = c->emit[grid->first_sample + i][slot], *g = c->gather[grid->first_sample + i][slot];
                if (c->emit_styles[f][slot] != 255) energy[c->emit_styles[f][slot]] += e[0] + e[1] + e[2];
                if (c->gather_styles[f][slot] != 255) energy[c->gather_styles[f][slot]] += g[0] + g[1] + g[2];
            }
    }
    uint32_t count = 0;
    memset(styles, 255, FACE_STYLES); memset(layer_of_style, 255, 256);
    if (energy[0] > 0) { styles[count] = 0; layer_of_style[0] = (uint8_t)count++; }
    while (count < FACE_STYLES) {
        int best = -1;
        for (int style = 1; style < 255; ++style)
            if (energy[style] > 0 && layer_of_style[style] == 255 && (best < 0 || energy[style] > energy[best])) best = style;
        if (best < 0) break;
        styles[count] = (uint8_t)best; layer_of_style[best] = (uint8_t)count++;
    }
    for (int style = 1; style < 32; ++style)
        if (layer_of_style[style] == 255 && layer_of_style[0] != 255) layer_of_style[style] = layer_of_style[0];
    return count;
}
static int BuildStaticProbes(lq_scene *s, const bounce_context *b, lq_range_runner run)
{
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t i = 0; i < s->vertex_count; ++i)
        for (int k = 0; k < 3; ++k) {
            low[k] = fminf(low[k], s->vertices[i].position[k]); high[k] = fmaxf(high[k], s->vertices[i].position[k]);
        }
    if (!(high[0] > low[0] && high[1] > low[1] && high[2] > low[2])) return 0;
    float spacing = PROBE_SPACING;
    uint32_t size[3];
    for (;;) {
        for (int k = 0; k < 3; ++k) size[k] = (uint32_t)ceilf((high[k] - low[k]) / spacing) + 1;
        if ((uint64_t)size[0] * size[1] * size[2] <= PROBE_LIMIT && size[2] * 6 * FACE_STYLES <= 2048 &&
            size[0] <= 2048 && size[1] <= 2048) break;
        spacing *= 1.25f;
    }
    size_t count = (size_t)size[0] * size[1] * size[2];
    free(s->probe_static); free(s->probe_valid);
    s->probe_static = calloc(count * 6 * FACE_STYLES, 4); s->probe_valid = calloc(count, 1);
    if (!s->probe_static || !s->probe_valid) return 0;
    memcpy(s->probe_origin, low, sizeof(low)); s->probe_spacing = spacing;
    memcpy(s->probe_size, size, sizeof(size));
    for (size_t p = 0; p < count; ++p) {
        float origin[3];
        ProbePosition(s, (uint32_t)(p % size[0]), (uint32_t)(p / size[0] % size[1]), (uint32_t)(p / ((size_t)size[0] * size[1])), origin);
        uint32_t leaf = LQ_SceneLeaf(s, origin);
        s->probe_valid[p] = leaf < s->leaf_count && s->leaf_contents[leaf] != LQ_LEAF_SOLID && s->leaf_contents[leaf] != LQ_LEAF_SKY;
    }
    probe_context c = {s, b, {0}, {0}, 0, PROBE_RAYS, {0}, {0}, s->probe_static, FACE_STYLES};
    ProbeStyles(b, s->probe_styles, c.layer_of_style);
    run(count, 64, StaticProbes, &c);
    uint8_t *filled = malloc(count);
    if (filled) { memcpy(filled, s->probe_valid, count); DilateProbes(s, s->probe_static, 6 * FACE_STYLES, filled); free(filled); }
    return 1;
}
/* Value of a runtime light at a distance, as the shader evaluates it. */
static float RuntimeValue(const lq_runtime_light *light, float distance)
{
    float value = light->falloff[0], scaled = light->falloff[1] * distance;
    switch ((int)(light->falloff[3] + 0.5f)) {
    case LQ_LIGHT_LINEAR: return fmaxf(value - scaled, 0);
    case LQ_LIGHT_INVERSE: return value * 128 / fmaxf(scaled, 1);
    case LQ_LIGHT_INVERSE2: return value * 16384 / fmaxf(scaled * scaled, 1);
    case LQ_LIGHT_INFINITE: return value;
    case LQ_LIGHT_INVERSE2A: return value * 16384 / ((scaled + 128) * (scaled + 128));
    default: return 0;
    }
}
static void DynamicProbes(void *opaque, size_t begin, size_t end)
{
    const probe_context *c = opaque;
    const lq_scene *s = c->scene;
    for (size_t i = begin; i < end; ++i) {
        uint32_t x = c->box_min[0] + (uint32_t)(i % c->box_size[0]);
        uint32_t y = c->box_min[1] + (uint32_t)(i / c->box_size[0] % c->box_size[1]);
        uint32_t z = c->box_min[2] + (uint32_t)(i / ((size_t)c->box_size[0] * c->box_size[1]));
        size_t p = x + (size_t)s->probe_size[0] * (y + (size_t)s->probe_size[1] * z);
        if (!s->probe_valid[p]) continue;
        float origin[3], cube[6][3];
        memset(cube, 0, sizeof(cube));
        ProbePosition(s, x, y, z, origin);
        int near = 0;
        for (uint32_t l = 0; l < c->light_count; ++l) {
            const float *at = c->lights[l]->position_radius;
            float dx = at[0]-origin[0], dy = at[1]-origin[1], dz = at[2]-origin[2];
            near |= dx*dx + dy*dy + dz*dz < (at[3] + s->probe_spacing) * (at[3] + s->probe_spacing);
        }
        if (!near) continue;
        float rotation = (Hash((uint32_t)p ^ 0x5bd1e995u) & 0xffff) / 65536.0f * 6.2831853f;
        for (uint32_t r = 0; r < c->rays; ++r) {
            float d[3], target[3], hit[3]; uint32_t face;
            SphereDirection(r, c->rays, rotation, d);
            for (int k = 0; k < 3; ++k) target[k] = origin[k] + d[k] * RAY_LENGTH;
            if (TraceFace(s, 0, origin, target, d, &face, hit, 0) != 1) continue;
            const float *normal = s->vertices[s->indices[s->draws[face].first_index]].normal;
            const float *albedo = s->face_samples[face].albedo;
            float lifted[3] = {hit[0] + normal[0], hit[1] + normal[1], hit[2] + normal[2]};
            for (uint32_t l = 0; l < c->light_count; ++l) {
                const lq_runtime_light *light = c->lights[l];
                float toward[3] = {light->position_radius[0]-hit[0], light->position_radius[1]-hit[1], light->position_radius[2]-hit[2]};
                float distance = sqrtf(Dot(toward, toward));
                if (distance >= light->position_radius[3] || distance < 0.001f) continue;
                float facing = Dot(normal, toward) / distance;
                if (facing <= 0) continue;
                float value = RuntimeValue(light, distance) * ((1 - light->falloff[2]) + light->falloff[2] * facing) *
                    light->color_strength[3];
                if (value < 0.01f || !SegmentClear(s, 0, lifted, light->position_radius, 0)) continue;
                float exitance[3];
                for (int k = 0; k < 3; ++k) exitance[k] = value * light->color_strength[k] * albedo[k];
                CubeAdd(cube, d, exitance);
            }
        }
        StoreCube(c, x, y, z, 0, cube, 4.0f / c->rays * s->bounce_scale, NULL);
    }
}
uint32_t LQ_SceneDynamicProbeLights(const lq_scene *s, const lq_runtime_light *lights, uint32_t count,
    const lq_runtime_light *selected[LQ_MAX_DYNAMIC_LIGHTS], uint32_t box_min[3], uint32_t box_size[3])
{
    if (!s || !s->probe_valid) return 0;
    uint32_t chosen = 0;
    int64_t low[3] = {INT64_MAX, INT64_MAX, INT64_MAX}, high[3] = {INT64_MIN, INT64_MIN, INT64_MIN};
    for (uint32_t i = 0; i < count && i < LQ_MAX_RUNTIME_LIGHTS; ++i) {
        const lq_runtime_light *light = &lights[i];
        if (!(light->source_index & LQ_DYNAMIC_LIGHT_SOURCE) || light->position_radius[3] <= 0 ||
            light->color_strength[3] <= 0 || chosen == LQ_MAX_DYNAMIC_LIGHTS) continue;
        selected[chosen++] = light;
        for (int k = 0; k < 3; ++k) {
            float reach = light->position_radius[3] + s->probe_spacing;
            int64_t a = (int64_t)floorf((light->position_radius[k] - reach - s->probe_origin[k]) / s->probe_spacing);
            int64_t b = (int64_t)ceilf((light->position_radius[k] + reach - s->probe_origin[k]) / s->probe_spacing);
            if (a < low[k]) low[k] = a;
            if (b > high[k]) high[k] = b;
        }
    }
    if (!chosen) return 0;
    for (int k = 0; k < 3; ++k) {
        if (low[k] < 0) low[k] = 0;
        if (high[k] >= (int64_t)s->probe_size[k]) high[k] = (int64_t)s->probe_size[k] - 1;
        if (high[k] < low[k]) return 0;
        box_min[k] = (uint32_t)low[k]; box_size[k] = (uint32_t)(high[k] - low[k] + 1);
    }
    return chosen;
}
int LQ_SceneDynamicProbes(const lq_scene *s, const lq_runtime_light *lights, uint32_t count,
    uint32_t rays, lq_range_runner run, uint8_t *output)
{
    if (!s || !s->probe_valid || !output || !rays) return 0;
    size_t texels = (size_t)s->probe_size[0] * s->probe_size[1] * s->probe_size[2] * 6;
    memset(output, 0, texels * 4);
    probe_context c = {s, NULL, {0}, {0}, 0, rays, {0}, {0}, output, 1};
    c.light_count = LQ_SceneDynamicProbeLights(s, lights, count, c.lights, c.box_min, c.box_size);
    if (!c.light_count) return 0;
    if (!run) run = Serial;
    run((size_t)c.box_size[0] * c.box_size[1] * c.box_size[2], 16, DynamicProbes, &c);
    return 1;
}
static void Serial(size_t count, size_t grain, lq_range_fn function, void *context)
{ (void)grain; function(context, 0, count); }
/* Sky radiance in brightness units: the mean color the sky draws with (front
 * layer over back, colormap row 0 as the shader samples it), weighted by sky
 * face area. Brightness 128 shows a texture at its palette color, so a white
 * surface under an open sky of this radiance matches the sky's own color. */
static void SkyLight(lq_scene *s)
{
    double sum[3] = {0}, weight = 0;
    memset(s->sky_light, 0, sizeof(s->sky_light));
    if (!s->colormap.pixels || s->colormap.width < 256) return;
    for (uint32_t f = 0; f < s->draw_count; ++f) {
        const lq_draw *draw = &s->draws[f];
        if (!(draw->flags & LQ_SURFACE_SKY) || draw->material >= s->material_count) continue;
        const lq_image *image = &s->materials[draw->material].indices;
        uint32_t half = image->width / 2;
        if (!half || !image->height || !image->pixels) continue;
        const float *a = s->vertices[s->indices[draw->first_index]].position;
        float area = 0;
        for (uint32_t i = 2; i < draw->index_count / 3 + 2; ++i) {
            const float *b = s->vertices[s->indices[draw->first_index] + i - 1].position;
            const float *e = s->vertices[s->indices[draw->first_index] + i].position;
            float u[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]}, v[3] = {e[0]-a[0], e[1]-a[1], e[2]-a[2]};
            float n[3] = {u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
            area += 0.5f * sqrtf(Dot(n, n));
        }
        double color[3] = {0};
        for (uint32_t y = 0; y < image->height; ++y) for (uint32_t x = 0; x < half; ++x) {
            uint8_t index = image->pixels[(size_t)y * image->width + x];
            if (!index) index = image->pixels[(size_t)y * image->width + half + x];
            for (int k = 0; k < 3; ++k) color[k] += s->colormap.pixels[index * 4 + k];
        }
        for (int k = 0; k < 3; ++k) sum[k] += color[k] / ((double)half * image->height * 255.0) * area;
        weight += area;
    }
    if (weight > 0) for (int k = 0; k < 3; ++k) s->sky_light[k] = (float)(sum[k] / weight * 128.0 * LQ_SKY_LIGHT_GAIN);
}
int LQ_SceneComputeBounce(lq_scene *s, uint32_t rays, lq_range_runner run, char *error, size_t size)
{ return LQ_SceneComputeBounceTraced(s, rays, run, NULL, error, size); }
int LQ_SceneComputeBounceTraced(lq_scene *s, uint32_t rays, lq_range_runner run,
    lq_bounce_tracer *tracer, char *error, size_t size)
{
    if (!s || !s->face_samples || !s->bsp_nodes || !s->leaf_contents || !rays || rays > 4096 || !s->atlas_height) {
        snprintf(error, size, "bounce needs sample grids and a BSP"); return 0;
    }
    if (!run) run = Serial;
    if (tracer) tracer->passes = 0;
    bounce_context c = {s, NULL, NULL, NULL, NULL, NULL, rays};
    size_t samples = s->sample_count ? s->sample_count : 1;
    c.emit = calloc(samples, sizeof(*c.emit)); c.gather = calloc(samples, sizeof(*c.gather));
    c.sky = calloc(samples, sizeof(*c.sky));
    c.emit_styles = malloc(s->draw_count * sizeof(*c.emit_styles));
    c.gather_styles = malloc(s->draw_count * sizeof(*c.gather_styles));
    uint8_t *pixels = calloc((size_t)s->lightmap.width * 4 * s->atlas_height, 4);
    if (!c.emit || !c.gather || !c.sky || !c.emit_styles || !c.gather_styles || !pixels) {
        free(c.emit); free(c.gather); free(c.sky); free(c.emit_styles); free(c.gather_styles); free(pixels);
        snprintf(error, size, "cannot allocate bounce buffers"); return 0;
    }
    memset(c.emit_styles, 255, s->draw_count * sizeof(*c.emit_styles));
    memset(c.gather_styles, 255, s->draw_count * sizeof(*c.gather_styles));
    SkyLight(s);
    if (s->sky_light[0] + s->sky_light[1] + s->sky_light[2] > 0) {
        if (tracer && tracer->sky && tracer->sky(tracer->context, s, rays, c.sky)) {
            ++tracer->passes; run(s->draw_count, 8, SkyBorrowFaces, &c);
        } else run(s->draw_count, 8, SkyFaces, &c);
    } else { free(c.sky); c.sky = NULL; }
    if (s->sun_light[0] + s->sun_light[1] + s->sun_light[2] > 0) {
        c.sun = calloc(samples, sizeof(*c.sun));
        if (!c.sun) {
            free(c.emit); free(c.gather); free(c.sky); free(c.emit_styles); free(c.gather_styles); free(pixels);
            snprintf(error, size, "cannot allocate sun visibility"); return 0;
        }
        if (tracer && tracer->sun && tracer->sun(tracer->context, s, c.sun)) ++tracer->passes;
        else run(s->draw_count, 8, SunFaces, &c);
    }
    lq_bounce_entry *direct = tracer && tracer->direct ? malloc(samples * LQ_BOUNCE_ENTRIES * sizeof(*direct)) : NULL;
    if (direct && tracer->direct(tracer->context, s, direct)) { c.direct = direct; ++tracer->passes; }
    run(s->draw_count, 16, EmitFaces, &c);
    free(direct); c.direct = NULL;
    lq_bounce_entry *entries = tracer && tracer->gather ? malloc(samples * LQ_BOUNCE_ENTRIES * sizeof(*entries)) : NULL;
    if (entries && tracer->gather(tracer->context, s, rays, (const float (*)[4][3])c.emit,
            (const uint8_t (*)[4])c.emit_styles, entries)) { c.entries = entries; ++tracer->passes; }
    run(s->draw_count, 8, GatherFaces, &c);
    free(entries); c.entries = NULL;
    /* Stack four style layers vertically, each with the lightmap's layout and
     * its one-texel clamped border for filtering. Values are brightness units. */
    uint32_t width = s->lightmap.width, layer = s->atlas_height;
    for (uint32_t f = 0; f < s->draw_count; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        memcpy(s->draws[f].bounce_styles, c.gather_styles[f], FACE_STYLES);
        if (!grid->width) continue;
        for (int k = 0; k < FACE_STYLES; ++k) {
            if (c.gather_styles[f][k] == 255 && !(k == 0 && c.sky) && !(k == 1 && c.sun)) continue;
            for (int y = -1; y <= grid->height; ++y) for (int x = -1; x <= grid->width; ++x) {
                int sx = x < 0 ? 0 : x >= grid->width ? grid->width - 1 : x;
                int sy = y < 0 ? 0 : y >= grid->height ? grid->height - 1 : y;
                uint32_t sample = grid->first_sample + sy * grid->width + sx;
                const float *value = c.gather[sample][k];
                uint8_t *pixel = pixels + ((size_t)(k * layer + grid->atlas_y + y) * width + grid->atlas_x + x) * 4;
                if (c.gather_styles[f][k] != 255)
                    for (int j = 0; j < 3; ++j) pixel[j] = (uint8_t)fminf(value[j] + 0.5f, 255);
                pixel[3] = k == 0 && c.sky ? (uint8_t)fminf(c.sky[sample] * 255 + 0.5f, 255) :
                    k == 1 && c.sun ? (uint8_t)fminf(c.sun[sample] * 255 + 0.5f, 255) : 255;
            }
        }
    }
    free(s->bounce.pixels);
    s->bounce = (lq_image){width, 4 * layer, 4, pixels};
    if (!BuildStaticProbes(s, &c, run)) {
        free(s->probe_static); free(s->probe_valid); s->probe_static = s->probe_valid = NULL;
        memset(s->probe_size, 0, sizeof(s->probe_size));
    }
    free(c.emit); free(c.gather); free(c.sky); free(c.sun); free(c.emit_styles); free(c.gather_styles);
    return 1;
}
