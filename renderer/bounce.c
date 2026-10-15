/* Enhanced static-world bounce light from runtime light sources.
 * Direct light at LIGHT's sample lattice is computed from each source's own
 * falloff with BSP occlusion, reflected by the face's texture color, and
 * gathered once by cosine-weighted rays. The baked lightmap is never read.
 * GPL-2.0-or-later. */
#include "scene.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE_STYLES 4
#define CANDIDATE_STYLES 16
#define RAY_LENGTH 4096.0f

typedef struct {
    lq_scene *scene;
    float (*emit)[FACE_STYLES][3]; /* exitance per sample and face style slot */
    uint8_t (*emit_styles)[FACE_STYLES];
    float (*gather)[FACE_STYLES][3];
    uint8_t (*gather_styles)[FACE_STYLES];
    uint32_t rays;
} bounce_context;

static float Dot(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static int Blocking(const lq_scene *s, int32_t node)
{
    uint32_t leaf = (uint32_t)(-(node + 1));
    uint8_t contents = leaf < s->leaf_count ? s->leaf_contents[leaf] : LQ_LEAF_SOLID;
    return contents == LQ_LEAF_SOLID || contents == LQ_LEAF_SKY;
}
/* True when no solid or sky leaf lies on the segment. */
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
    return !Blocking(s, node);
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
 * but with exact polygon containment. Returns 1 on a hit, -1 when solid or sky
 * stops the ray first, 0 when it leaves the segment unobstructed. */
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
            if (draw->flags || !s->face_samples[candidate].width) continue;
            if (Dot(s->vertices[s->indices[draw->first_index]].normal, direction) >= 0) continue;
            if (!FaceContains(s, candidate, middle)) continue;
            *face = candidate; memcpy(hit, middle, sizeof(middle)); return 1;
        }
        return TraceFace(s, n->children[!side], middle, b, direction, face, hit, depth + 1);
    }
    return Blocking(s, node) ? -1 : 0;
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
        for (uint32_t l = 0; l < s->light_source_count; ++l) {
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
                if (light->spotlight) {
                    float along = -Dot(toward, light->spot_direction);
                    if (along <= light->spot_outer) continue;
                    if (light->spot_inner > light->spot_outer)
                        value *= fminf(fmaxf((along - light->spot_outer) / (light->spot_inner - light->spot_outer), 0), 1);
                }
                if (value < 0.01f || !SegmentClear(s, 0, light->origin, point, 0)) continue;
                if (slot == UINT32_MAX && (slot = Candidate(candidates, &candidate_count, (uint8_t)light->style)) == UINT32_MAX) break;
                for (int k = 0; k < 3; ++k) {
                    float exitance = value * light->color[k] * grid->albedo[k];
                    accumulate[i][slot][k] += exitance; candidates[slot].energy += exitance;
                }
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
/* Bilinear exitance at a hit point for one of the hit face's style slots. */
static void HitExitance(const bounce_context *c, uint32_t face, const float hit[3], int slot, float out[3])
{
    const lq_face_samples *grid = &c->scene->face_samples[face];
    float u = (Dot(hit, grid->s) + grid->s[3] - grid->mins[0]) / 16.0f;
    float v = (Dot(hit, grid->t) + grid->t[3] - grid->mins[1]) / 16.0f;
    u = fminf(fmaxf(u, 0), (float)(grid->width - 1)); v = fminf(fmaxf(v, 0), (float)(grid->height - 1));
    int x = (int)u, y = (int)v, x1 = x + 1 < grid->width ? x + 1 : x, y1 = y + 1 < grid->height ? y + 1 : y;
    float fx = u - x, fy = v - y;
    const float (*e)[FACE_STYLES][3] = c->emit + grid->first_sample;
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
        float helper[3] = {fabsf(normal[2]) < 0.9f ? 0.0f : 1.0f, 0, fabsf(normal[2]) < 0.9f ? 1.0f : 0.0f};
        tangent[0] = helper[1]*normal[2]-helper[2]*normal[1];
        tangent[1] = helper[2]*normal[0]-helper[0]*normal[2];
        tangent[2] = helper[0]*normal[1]-helper[1]*normal[0];
        float length = sqrtf(Dot(tangent, tangent));
        for (int k = 0; k < 3; ++k) tangent[k] /= length;
        bitangent[0] = normal[1]*tangent[2]-normal[2]*tangent[1];
        bitangent[1] = normal[2]*tangent[0]-normal[0]*tangent[2];
        bitangent[2] = normal[0]*tangent[1]-normal[1]*tangent[0];
        for (uint32_t i = 0; i < samples; ++i) {
            const float *point = s->sample_positions[grid->first_sample + i];
            if (!ValidSample(point)) continue;
            uint32_t seed = Hash((uint32_t)(grid->first_sample + i) * 0x9e3779b9u);
            float rotate_u = (seed & 0xffff) / 65536.0f, rotate_v = (seed >> 16) / 65536.0f;
            for (uint32_t r = 0; r < c->rays; ++r) {
                float u = (r + 0.5f) / c->rays + rotate_u, v = Radical(r) + rotate_v;
                u -= floorf(u); v -= floorf(v);
                float radius = sqrtf(u), angle = 6.2831853f * v;
                float a = radius * cosf(angle), b = radius * sinf(angle), h = sqrtf(fmaxf(1 - u, 0));
                float direction[3], target[3], hit[3];
                for (int k = 0; k < 3; ++k) {
                    direction[k] = tangent[k]*a + bitangent[k]*b + normal[k]*h;
                    target[k] = point[k] + direction[k] * RAY_LENGTH;
                }
                uint32_t face;
                if (TraceFace(s, 0, point, target, direction, &face, hit, 0) != 1 || face == f) continue;
                for (int slot = 0; slot < FACE_STYLES; ++slot) {
                    uint8_t style = c->emit_styles[face][slot];
                    if (style == 255) continue;
                    uint32_t at = Candidate(candidates, &candidate_count, style);
                    if (at == UINT32_MAX) continue;
                    float exitance[3];
                    HitExitance(c, face, hit, slot, exitance);
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
static void Serial(size_t count, size_t grain, lq_range_fn function, void *context)
{ (void)grain; function(context, 0, count); }
int LQ_SceneComputeBounce(lq_scene *s, uint32_t rays, lq_range_runner run, char *error, size_t size)
{
    if (!s || !s->face_samples || !s->bsp_nodes || !s->leaf_contents || !rays || rays > 4096 || !s->atlas_height) {
        snprintf(error, size, "bounce needs sample grids and a BSP"); return 0;
    }
    if (!run) run = Serial;
    bounce_context c = {s, NULL, NULL, NULL, NULL, rays};
    size_t samples = s->sample_count ? s->sample_count : 1;
    c.emit = calloc(samples, sizeof(*c.emit)); c.gather = calloc(samples, sizeof(*c.gather));
    c.emit_styles = malloc(s->draw_count * sizeof(*c.emit_styles));
    c.gather_styles = malloc(s->draw_count * sizeof(*c.gather_styles));
    uint8_t *pixels = calloc((size_t)s->lightmap.width * 4 * s->atlas_height, 4);
    if (!c.emit || !c.gather || !c.emit_styles || !c.gather_styles || !pixels) {
        free(c.emit); free(c.gather); free(c.emit_styles); free(c.gather_styles); free(pixels);
        snprintf(error, size, "cannot allocate bounce buffers"); return 0;
    }
    memset(c.emit_styles, 255, s->draw_count * sizeof(*c.emit_styles));
    memset(c.gather_styles, 255, s->draw_count * sizeof(*c.gather_styles));
    run(s->draw_count, 16, EmitFaces, &c);
    run(s->draw_count, 8, GatherFaces, &c);
    /* Stack four style layers vertically, each with the lightmap's layout and
     * its one-texel clamped border for filtering. Values are brightness units. */
    uint32_t width = s->lightmap.width, layer = s->atlas_height;
    for (uint32_t f = 0; f < s->draw_count; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        memcpy(s->draws[f].bounce_styles, c.gather_styles[f], FACE_STYLES);
        if (!grid->width) continue;
        for (int k = 0; k < FACE_STYLES; ++k) {
            if (c.gather_styles[f][k] == 255) continue;
            for (int y = -1; y <= grid->height; ++y) for (int x = -1; x <= grid->width; ++x) {
                int sx = x < 0 ? 0 : x >= grid->width ? grid->width - 1 : x;
                int sy = y < 0 ? 0 : y >= grid->height ? grid->height - 1 : y;
                const float *value = c.gather[grid->first_sample + sy * grid->width + sx][k];
                uint8_t *pixel = pixels + ((size_t)(k * layer + grid->atlas_y + y) * width + grid->atlas_x + x) * 4;
                for (int j = 0; j < 3; ++j) pixel[j] = (uint8_t)fminf(value[j] + 0.5f, 255);
                pixel[3] = 255;
            }
        }
    }
    free(s->bounce.pixels);
    s->bounce = (lq_image){width, 4 * layer, 4, pixels};
    free(c.emit); free(c.gather); free(c.emit_styles); free(c.gather_styles);
    return 1;
}
