/* LuminousQuake scene ownership, validation and Quake-coordinate camera.
 * GPL-2.0-or-later. */
#include "scene.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
void LQ_SceneFree(lq_scene *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < s->material_count; ++i) {
        free(s->materials[i].indices.pixels); free(s->materials[i].pack_color.pixels); free(s->materials[i].pack_normal.pixels);
    }
    free(s->light_sources); free(s->light_visibility); free(s->shadow_free);
    free(s->bsp_nodes); free(s->leaf_pvs); free(s->light_leaves); free(s->leaf_contents);
    free(s->face_samples); free(s->sample_positions); free(s->bounce.pixels);
    free(s->probe_static); free(s->probe_valid); free(s->draw_bounds);
    free(s->materials); free(s->vertices); free(s->indices); free(s->draws);
    free(s->colormap.pixels); free(s->lightmap.pixels); memset(s, 0, sizeof(*s));
}
/* Original LIGHT and ericw-tools defaults: linear falloff, unit attenuation,
 * half-weighted incidence and a 0.5 range scale on the stored result. */
void LQ_LightSourceDefaults(lq_light_source *source)
{
    if (!source) return;
    source->attenuation = 1; source->angle_scale = 0.5f; source->output_scale = 0.5f;
    source->formula = LQ_LIGHT_LINEAR; source->spotlight = 0;
    source->spot_outer = source->spot_inner = -2;
    source->source_radius = LQ_LIGHT_DEFAULT_RADIUS;
    source->accent_gain = 1; source->output_base = 0.5f;
    source->color[0] = source->color[1] = source->color[2] = 1;
}
static int DirectSource(const lq_light_source *source)
{
    return source && !source->flags && source->formula < LQ_LIGHT_FORMULA_COUNT &&
        source->formula != LQ_LIGHT_LOCAL_MINLIGHT && source->style < 255 &&
        isfinite(source->intensity) && source->intensity > 0 &&
        isfinite(source->attenuation) && source->attenuation > 0 &&
        isfinite(source->output_scale) && source->output_scale > 0;
}
/* Resolve live style state independently of the immutable map description.
 * No baked lightmap or initial spawnflag is multiplied into runtime styles. */
float LQ_LightSourceIntensity(const lq_light_source *source, const float styles[256])
{
    if (!source || !styles || source->style >= 255 || source->flags ||
        !isfinite(source->intensity) || !isfinite(styles[source->style])) return 0;
    float value = source->intensity * fmaxf(styles[source->style], 0);
    return isfinite(value) ? value : 0;
}
/* The compiler's per-source value at a distance, before the incidence, spot,
 * style and output-scale terms. The shader evaluates the same expressions. */
float LQ_LightFalloff(const lq_light_source *source, float distance)
{
    if (!DirectSource(source) || !isfinite(distance)) return 0;
    float light = source->intensity, scaled = source->attenuation * fmaxf(distance, 0);
    switch (source->formula) {
    case LQ_LIGHT_LINEAR: return fmaxf(light - scaled, 0);
    case LQ_LIGHT_INVERSE: return light * 128 / fmaxf(scaled, 1);
    case LQ_LIGHT_INVERSE2: return light * 16384 / fmaxf(scaled * scaled, 1);
    case LQ_LIGHT_INFINITE: return light;
    case LQ_LIGHT_INVERSE2A: return light * 16384 / ((scaled + 128) * (scaled + 128));
    default: return 0;
    }
}
/* Distance beyond which a source adds less than its fade level after output
 * scaling: half a brightness unit (an eighth of one colormap row) for linear
 * lights, LQ_LIGHT_FADE_LEVEL or the map's _gate for the unbounded falloffs.
 * This is the culling and shadow range. Sources without falloff (sunbeams and
 * other regional lights) are bounded by the largest Quake map extent. */
float LQ_LightSourceRange(const lq_light_source *source)
{
    if (!DirectSource(source)) return 0;
    float level = source->formula == LQ_LIGHT_LINEAR ? 0.5f : fmaxf(LQ_LIGHT_FADE_LEVEL, source->gate);
    float cutoff = level / source->output_scale, light = source->intensity, range;
    if (light <= cutoff) return 0;
    switch (source->formula) {
    case LQ_LIGHT_LINEAR: range = (light - cutoff) / source->attenuation; break;
    case LQ_LIGHT_INVERSE: range = 128 * light / (cutoff * source->attenuation); break;
    case LQ_LIGHT_INVERSE2: range = 128 * sqrtf(light / cutoff) / source->attenuation; break;
    case LQ_LIGHT_INVERSE2A: range = 128 * (sqrtf(light / cutoff) - 1) / source->attenuation; break;
    default: range = LQ_LIGHT_MAX_RANGE; break;
    }
    return isfinite(range) && range > 0 ? fminf(range, LQ_LIGHT_MAX_RANGE) : 0;
}
/* Spotlight weight at a point: the cone edge is blurred over a world-space
 * band at the receiver, and an authored inner cone fades in with smoothstep.
 * to_light is the unit vector from the point to the light. */
float LQ_SpotFactor(const float to_light[3], float distance, const float axis[3],
    float outer, float inner, float source_radius)
{
    float along = -(to_light[0]*axis[0] + to_light[1]*axis[1] + to_light[2]*axis[2]);
    float factor = 1;
    if (outer > 0.01f) {
        float axial = distance * along, radial = distance * sqrtf(fmaxf(1 - along*along, 0));
        float edge = radial - axial * sqrtf(fmaxf(1 - outer*outer, 0)) / outer, blur = LQ_SPOT_EDGE_BLUR + source_radius;
        float t = fminf(fmaxf((edge + blur) / (2 * blur), 0), 1);
        factor = 1 - t * t * (3 - 2 * t);
        if (along <= 0) factor = 0;
    } else if (along <= outer) return 0;
    if (inner > outer) {
        float t = fminf(fmaxf((along - outer) / (inner - outer), 0), 1);
        factor *= t * t * (3 - 2 * t);
    }
    return factor;
}
int LQ_LightIsAccent(const lq_light_source *source)
{
    return source && source->spotlight && source->formula != LQ_LIGHT_LINEAR &&
        source->formula != LQ_LIGHT_LOCAL_MINLIGHT;
}
uint32_t LQ_SceneSetAccentGain(lq_scene *scene, float gain)
{
    uint32_t count = 0;
    if (!scene || !isfinite(gain) || gain < 0) return 0;
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        lq_light_source *light = &scene->light_sources[i];
        if (!LQ_LightIsAccent(light)) continue;
        light->accent_gain = gain; light->output_scale = light->output_base * gain; ++count;
    }
    return count;
}
uint32_t LQ_SceneLeaf(const lq_scene *scene, const float point[3])
{
    if (!scene || !point || !scene->bsp_nodes || !scene->bsp_node_count) return UINT32_MAX;
    int32_t node = 0;
    for (uint32_t steps = 0; node >= 0 && steps <= scene->bsp_node_count; ++steps) {
        if ((uint32_t)node >= scene->bsp_node_count) return UINT32_MAX;
        const lq_bsp_node *n = &scene->bsp_nodes[node];
        float side = n->normal[0]*point[0]+n->normal[1]*point[1]+n->normal[2]*point[2]-n->dist;
        node = n->children[side < 0];
    }
    if (node >= 0) return UINT32_MAX;
    uint32_t leaf = (uint32_t)(-(node + 1));
    return leaf < scene->leaf_count ? leaf : UINT32_MAX;
}
int LQ_SceneLiquid(const lq_scene *scene, const float point[3])
{
    uint32_t leaf = LQ_SceneLeaf(scene, point);
    return leaf != UINT32_MAX && scene->leaf_contents && scene->leaf_contents[leaf] == LQ_LEAF_LIQUID;
}
static void FillRuntimeLight(lq_runtime_light *light, const lq_light_source *source,
    uint32_t index, const float styles[256])
{
    memset(light, 0, sizeof(*light));
    memcpy(light->position_radius, source->origin, 3*sizeof(float));
    light->position_radius[3] = LQ_LightSourceRange(source);
    memcpy(light->color_strength, source->color, 3*sizeof(float));
    float style = styles[source->style];
    light->color_strength[3] = isfinite(style) ? fmaxf(style, 0) * source->output_scale : 0;
    light->falloff[0] = source->intensity; light->falloff[1] = source->attenuation;
    light->falloff[2] = source->angle_scale; light->falloff[3] = (float)source->formula;
    light->limit = source->flame_bound ? LQ_TORCH_ACCENT_LIMIT : 0;
    if (source->spotlight) {
        memcpy(light->spot, source->spot_direction, 3*sizeof(float));
        light->spot[3] = source->spot_outer; light->spot_inner = source->spot_inner;
    } else light->spot[3] = light->spot_inner = -2;
    light->source_radius = source->source_radius;
    light->source_index = index;
    memcpy(light->shadow_origin_range, light->position_radius, sizeof(light->shadow_origin_range));
    light->shadow_radius = light->source_radius;
}
/* Select up to LQ_MAX_RUNTIME_LIGHTS sources whose potential reach meets the
 * camera's PVS, nearest (range-weighted) first. `output` is also the previous
 * frame's slots: a selected source keeps its slot so its shadow cube can be
 * reused, and a held source stays selected while its style is momentarily dark.
 * Returns one past the highest occupied slot; zero-range slots are empty. */
uint32_t LQ_BuildRuntimeLights(const lq_scene *scene, const float styles[256],
    const float camera[3], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS])
{ return LQ_BuildRuntimeLightsLimited(scene, styles, camera, output, LQ_MAX_RUNTIME_LIGHTS); }
void LQ_DynamicRuntimeLight(lq_runtime_light *light, const float origin[3], float radius,
    const float color[3], float source_radius, uint32_t key)
{
    memset(light, 0, sizeof(*light));
    memcpy(light->position_radius, origin, 3*sizeof(float));
    light->position_radius[3] = isfinite(radius) && radius > 0 ? radius : 0;
    memcpy(light->color_strength, color, 3*sizeof(float));
    light->color_strength[3] = 1;
    light->falloff[0] = light->position_radius[3]; light->falloff[1] = 1; light->falloff[2] = 0.5f;
    light->falloff[3] = LQ_LIGHT_LINEAR;
    light->spot[3] = light->spot_inner = -2;
    light->source_radius = source_radius;
    light->source_index = LQ_DYNAMIC_LIGHT_SOURCE | key;
    memcpy(light->shadow_origin_range, light->position_radius, sizeof(light->shadow_origin_range));
    light->shadow_radius = source_radius;
}
void LQ_ShareGroupShadows(const lq_scene *scene, lq_runtime_light *lights, uint32_t count, int enabled)
{
    if (!lights) return;
    if (count > LQ_MAX_RUNTIME_LIGHTS) count = LQ_MAX_RUNTIME_LIGHTS;
    /* Each light first takes its own cube, from where it now stands (flames
     * move after selection). */
    for (uint32_t i = 0; i < count; ++i) {
        lq_runtime_light *light = &lights[i];
        memcpy(light->shadow_origin_range, light->position_radius, sizeof(light->shadow_origin_range));
        light->shadow_radius = light->source_radius; light->shadow_slot = 0;
    }
    if (!enabled || !scene) return;
    for (uint32_t i = 0; i < count; ++i) {
        lq_runtime_light *owner = &lights[i];
        if (owner->position_radius[3] <= 0 || owner->shadow_slot || owner->source_index >= scene->light_source_count) continue;
        const lq_light_source *source = &scene->light_sources[owner->source_index];
        if (!source->shadow_group) continue;
        memcpy(owner->shadow_origin_range, source->shadow_origin, sizeof(source->shadow_origin));
        owner->shadow_origin_range[3] = source->shadow_reach;
        owner->shadow_radius = source->shadow_radius;
        /* The cube waits only while no member lights anything in view. */
        for (uint32_t j = i + 1; j < count; ++j) {
            lq_runtime_light *member = &lights[j];
            if (member->position_radius[3] <= 0 || member->source_index >= scene->light_source_count ||
                scene->light_sources[member->source_index].shadow_group != source->shadow_group) continue;
            memcpy(member->shadow_origin_range, owner->shadow_origin_range, sizeof(member->shadow_origin_range));
            member->shadow_radius = owner->shadow_radius;
            member->shadow_slot = i + 1;
            owner->offscreen &= member->offscreen;
        }
    }
}
void LQ_ShadowSlot(uint32_t slot, uint32_t *array, uint32_t *cube)
{
    static const uint32_t cubes[] = LQ_SHADOW_ARRAY_CUBES;
    const uint32_t dynamic = LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS;
    _Static_assert(16 + 32 + 40 + 40 == LQ_MAX_RUNTIME_LIGHTS, "every slot owns one cube");
    if (slot >= dynamic) { *array = 0; *cube = slot - dynamic; return; }
    slot += LQ_MAX_DYNAMIC_LIGHTS; /* map slots follow the gameplay cubes */
    for (*array = 0; *array + 1 < LQ_SHADOW_ARRAYS && slot >= cubes[*array]; ++*array) slot -= cubes[*array];
    *cube = slot;
}
/* Resolution class of a slot (0 sharpest): arrays of equal size share one. */
#define LQ_SHADOW_TIERS 3
static uint32_t SlotTier(uint32_t slot)
{
    static const uint32_t sizes[] = LQ_SHADOW_ARRAY_SIZES;
    uint32_t array, cube; LQ_ShadowSlot(slot, &array, &cube);
    return sizes[array] >= 512 ? 0 : sizes[array] >= 256 ? 1 : 2;
}
static void DrawBounds(const lq_scene *scene, const lq_draw *draw, float out[6]);
/* Whether a spotlight's beam, with the blurred edge the shader gives it, can
 * reach a box. The shader lights points whose distance outside the cone,
 * measured across the axis, is under the blur, and nothing behind the light.
 * Distance to the cone surface changes no faster than position, so the box's
 * bounding sphere gives a conservative test. outer <= 0.01 (a near-hemisphere)
 * is always accepted. */
int LQ_SpotReachesBox(const float origin[3], const float axis[3], float outer, float blur, const float bounds[6])
{
    if (outer <= 0.01f || outer > 1) return 1;
    float v[3], half2 = 0, length2 = 0, along = 0;
    for (int k = 0; k < 3; ++k) {
        float centre = (bounds[k] + bounds[k+3]) * 0.5f, extent = (bounds[k+3] - bounds[k]) * 0.5f;
        v[k] = centre - origin[k]; half2 += extent * extent; length2 += v[k] * v[k]; along += v[k] * axis[k];
    }
    float r = sqrtf(half2);
    if (along < -r) return 0;
    float radial = sqrtf(fmaxf(length2 - along * along, 0)), sine = sqrtf(fmaxf(1 - outer * outer, 0));
    return radial * outer - along * sine - r < blur * outer;
}
/* Whether a source can light a draw, by the interaction test below. */
static int LightsDraw(const lq_scene *scene, uint32_t d, const lq_light_source *source, float range)
{
    const float *origin = source->origin;
    const lq_draw *draw = &scene->draws[d];
    float computed[6];
    const float *bounds = scene->draw_bounds ? scene->draw_bounds[d] : computed;
    if (!scene->draw_bounds) DrawBounds(scene, draw, computed);
    const lq_vertex *first = &scene->vertices[scene->indices[draw->first_index]];
    float distance2 = 0, facing = 0;
    for (int axis = 0; axis < 3; ++axis) {
        float p = origin[axis];
        float outside = p < bounds[axis] ? bounds[axis] - p : p > bounds[axis+3] ? p - bounds[axis+3] : 0;
        distance2 += outside * outside; facing += (p - first->position[axis]) * first->normal[axis];
    }
    if (!(facing > 0 && distance2 < range * range)) return 0;
    return !source->spotlight ||
        LQ_SpotReachesBox(origin, source->spot_direction, source->spot_outer, LQ_SPOT_EDGE_BLUR + source->source_radius, bounds);
}
/* Whether static world solid (or sky) may meet a convex hull of points. A
 * node the hull straddles is searched on both sides with the whole hull, so
 * the answer errs towards yes. */
static int HullMeetsSolid(const lq_scene *s, int32_t node, const float (*points)[3], int count, int depth)
{
    while (node >= 0) {
        if ((uint32_t)node >= s->bsp_node_count || depth > 1024) return 1;
        const lq_bsp_node *n = &s->bsp_nodes[node];
        int front = 0, back = 0;
        for (int i = 0; i < count; ++i) {
            float side = n->normal[0]*points[i][0] + n->normal[1]*points[i][1] + n->normal[2]*points[i][2] - n->dist;
            if (side >= -0.01f) front = 1;
            if (side < 0.01f) back = 1;
        }
        if (front && back && HullMeetsSolid(s, n->children[0], points, count, depth + 1)) return 1;
        node = n->children[front && !back ? 0 : 1];
        ++depth;
    }
    uint32_t leaf = (uint32_t)(-(node + 1));
    return leaf >= s->leaf_count || s->leaf_contents[leaf] == LQ_LEAF_SOLID || s->leaf_contents[leaf] == LQ_LEAF_SKY;
}
/* A shadow lookup (shadowFactor in world.frag) compares cube depths along
 * rays from the light to points within the filter's search radius of the
 * receiving point, against the receiving plane raised by a texel. Over a face
 * those rays fill the pyramid from the light to the face's rectangle in its
 * plane, widened by the search radius (more at oblique angles) and a texel,
 * and raised half a unit off the wall. Nothing solid in that pyramid means
 * every lookup on the face finds no blocker. The search radius is bounded
 * with the face's farthest point and the coarsest (128) cube. */
int LQ_SceneCountShadowFree(const lq_scene *s, const volatile int *cancel, lq_shadow_free_count *out)
{ return LQ_SceneFindShadowFree(s, cancel, out, NULL); }
int LQ_SceneFindShadowFree(const lq_scene *s, const volatile int *cancel, lq_shadow_free_count *out, uint32_t *bits_out)
{
    memset(out, 0, sizeof(*out));
    if (bits_out && s) memset(bits_out, 0, (size_t)s->draw_count * s->light_visibility_words * sizeof(*bits_out));
    if (!s || !s->bsp_nodes || !s->leaf_contents || !s->light_visibility || !s->light_source_count) return 0;
    uint32_t begin = s->world_draw_end ? s->world_draw_begin : 0, end = s->world_draw_end ? s->world_draw_end : s->draw_count;
    float *range = malloc(s->light_source_count * sizeof(*range));
    if (!range) return 0;
    for (uint32_t i = 0; i < s->light_source_count; ++i) range[i] = LQ_LightSourceRange(&s->light_sources[i]);
    /* Brush models cast from their compiled positions, and doors and lifts
     * move: any whose bounds meet a pyramid spoil it. */
    float (*brushes)[6] = malloc((s->draw_count ? s->draw_count : 1) * sizeof(*brushes));
    uint32_t brush_count = 0;
    if (!brushes) { free(range); return 0; }
    for (uint32_t m = 0; m < s->draw_count; ++m) {
        const lq_draw *brush = &s->draws[m];
        if ((m >= begin && m < end) || brush->flags & (LQ_SURFACE_SKY|LQ_SURFACE_WATER) || brush->index_count < 3) continue;
        if (s->draw_bounds) memcpy(brushes[brush_count], s->draw_bounds[m], sizeof(brushes[0]));
        else DrawBounds(s, brush, brushes[brush_count]);
        ++brush_count;
    }
    for (uint32_t d = begin; d < end; ++d) {
        if (*cancel) { free(range); free(brushes); return 0; }
        const lq_draw *draw = &s->draws[d];
        if (draw->flags & (LQ_SURFACE_SKY|LQ_SURFACE_WATER) || draw->index_count < 3) continue;
        float computed[6];
        const float *bounds = s->draw_bounds ? s->draw_bounds[d] : computed;
        if (!s->draw_bounds) DrawBounds(s, draw, computed);
        const lq_vertex *first = &s->vertices[s->indices[draw->first_index]];
        const float *n = first->normal;
        float dist = n[0]*first->position[0] + n[1]*first->position[1] + n[2]*first->position[2];
        float u[3], w[3], axis[3] = {0, 0, 0};
        axis[fabsf(n[2]) < 0.9f ? 2 : 0] = 1;
        u[0] = n[1]*axis[2] - n[2]*axis[1]; u[1] = n[2]*axis[0] - n[0]*axis[2]; u[2] = n[0]*axis[1] - n[1]*axis[0];
        float length = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
        if (length < 1e-6f) continue;
        for (int k = 0; k < 3; ++k) u[k] /= length;
        w[0] = n[1]*u[2] - n[2]*u[1]; w[1] = n[2]*u[0] - n[0]*u[2]; w[2] = n[0]*u[1] - n[1]*u[0];
        float umin = INFINITY, umax = -INFINITY, wmin = INFINITY, wmax = -INFINITY, area = 0;
        for (uint32_t j = 0; j < draw->index_count; ++j) {
            const float *p = s->vertices[s->indices[draw->first_index + j]].position;
            float a = p[0]*u[0] + p[1]*u[1] + p[2]*u[2], b = p[0]*w[0] + p[1]*w[1] + p[2]*w[2];
            umin = fminf(umin, a); umax = fmaxf(umax, a); wmin = fminf(wmin, b); wmax = fmaxf(wmax, b);
        }
        for (uint32_t j = 0; j + 2 < draw->index_count; j += 3) {
            const float *a = s->vertices[s->indices[draw->first_index + j]].position;
            const float *b = s->vertices[s->indices[draw->first_index + j + 1]].position;
            const float *c = s->vertices[s->indices[draw->first_index + j + 2]].position;
            float e[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]}, f[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
            float x = e[1]*f[2]-e[2]*f[1], y = e[2]*f[0]-e[0]*f[2], z = e[0]*f[1]-e[1]*f[0];
            area += 0.5f * sqrtf(x*x + y*y + z*z);
        }
        const uint32_t *bits = s->light_visibility + (size_t)d * s->light_visibility_words;
        for (uint32_t word = 0; word < s->light_visibility_words; ++word)
            for (uint32_t b = bits[word]; b; b &= b - 1) {
                uint32_t i = word * 32 + (uint32_t)__builtin_ctz(b);
                if (i >= s->light_source_count || range[i] <= 0) continue;
                const lq_light_source *source = &s->light_sources[i];
                if (!LightsDraw(s, d, source, range[i])) continue;
                const float *o = source->origin;
                float h = n[0]*o[0] + n[1]*o[1] + n[2]*o[2] - dist, far2 = 0;
                if (h <= 0) continue;
                ++out->pairs; out->area += area;
                for (int k = 0; k < 3; ++k) {
                    float f = fmaxf(fabsf(o[k] - bounds[k]), fabsf(o[k] - bounds[k+3]));
                    far2 += f * f;
                }
                float farthest = sqrtf(far2), incidence = h / farthest;
                if (incidence < 0.25f) continue; /* oblique: the widened pyramid would be unbounded */
                float texel = 2 * farthest / 128;
                float search = fmaxf(fminf(3 * source->source_radius, farthest * 0.5f), texel);
                float margin = search / incidence + texel + 1, raised = dist + 0.5f;
                float hull[5][3];
                memcpy(hull[0], o, sizeof(hull[0]));
                for (int corner = 0; corner < 4; ++corner) {
                    float a = corner & 1 ? umax + margin : umin - margin, c = corner & 2 ? wmax + margin : wmin - margin;
                    for (int k = 0; k < 3; ++k) hull[corner + 1][k] = u[k] * a + w[k] * c + n[k] * raised;
                }
                if (HullMeetsSolid(s, 0, (const float (*)[3])hull, 5, 0)) continue;
                float box[6] = {INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};
                for (int p = 0; p < 5; ++p) for (int k = 0; k < 3; ++k) {
                    box[k] = fminf(box[k], hull[p][k]); box[k+3] = fmaxf(box[k+3], hull[p][k]);
                }
                int blocked = 0;
                for (uint32_t m = 0; m < brush_count && !blocked; ++m) {
                    const float *mb = brushes[m];
                    blocked = mb[0] <= box[3] && mb[3] >= box[0] && mb[1] <= box[4] && mb[4] >= box[1] &&
                        mb[2] <= box[5] && mb[5] >= box[2];
                }
                if (blocked) continue;
                ++out->clear; out->clear_area += area;
                if (bits_out) bits_out[(size_t)d * s->light_visibility_words + i / 32] |= 1u << (i % 32);
            }
    }
    free(range); free(brushes);
    return 1;
}
/* For each source with range[i] > 0, whether it can light a visible draw:
 * reach[i] is 2 when one is on screen (`shown`, if given), 1 when it lights
 * only draws elsewhere in the PVS, 0 when it lights none. With precomputed
 * reach bits, each visible draw visits only the sources that can reach it. */
static void VisibleReach(const lq_scene *scene, const lq_draw *visible, const uint8_t *shown,
    const float *range, uint8_t *reach)
{
    memset(reach, 0, scene->light_source_count);
    for (uint32_t d = 0; d < scene->draw_count; ++d) {
        const lq_draw *draw = &scene->draws[d];
        if (!visible[d].index_count || draw->flags & LQ_SURFACE_SKY || draw->index_count < 3) continue;
        uint8_t level = !shown || shown[d] ? 2 : 1;
        if (!scene->light_visibility) {
            for (uint32_t i = 0; i < scene->light_source_count; ++i)
                if (range[i] > 0 && reach[i] < level && LightsDraw(scene, d, &scene->light_sources[i], range[i]))
                    reach[i] = level;
            continue;
        }
        const uint32_t *bits = scene->light_visibility + (size_t)d * scene->light_visibility_words;
        for (uint32_t w = 0; w < scene->light_visibility_words; ++w)
            for (uint32_t b = bits[w]; b; b &= b - 1) {
                uint32_t i = w * 32 + (uint32_t)__builtin_ctz(b);
                if (i < scene->light_source_count && range[i] > 0 && reach[i] < level &&
                    LightsDraw(scene, d, &scene->light_sources[i], range[i])) reach[i] = level;
            }
    }
}
int LQ_BoundsInClip(const float m[16], const float bounds[6])
{
    int outside[5] = {0};
    for (int corner = 0; corner < 8; ++corner) {
        float p[3] = {bounds[corner & 1 ? 3 : 0], bounds[corner & 2 ? 4 : 1], bounds[corner & 4 ? 5 : 2]};
        float x = m[0]*p[0] + m[4]*p[1] + m[8]*p[2] + m[12], y = m[1]*p[0] + m[5]*p[1] + m[9]*p[2] + m[13];
        float w = m[3]*p[0] + m[7]*p[1] + m[11]*p[2] + m[15];
        outside[0] += x < -w; outside[1] += x > w; outside[2] += y < -w; outside[3] += y > w; outside[4] += w <= 0;
    }
    for (int i = 0; i < 5; ++i) if (outside[i] == 8) return 0;
    return 1;
}
void LQ_LightSelectionClip(const lq_view *v, float aspect, float out[16])
{
    lq_view wider = *v;
    wider.horizontal_fov = fminf(v->horizontal_fov + LQ_SELECTION_FOV_MARGIN, 170);
    LQ_ViewMatrix(&wider, aspect > 0 && isfinite(aspect) ? aspect : 4.0f / 3, out);
}
/* Draws in the PVS (`visible`) whose bounds meet `clip`; NULL without a clip. */
static uint8_t *ShownDraws(const lq_scene *scene, const lq_draw *visible, const float clip[16])
{
    if (!clip || !visible) return NULL;
    uint8_t *shown = malloc(scene->draw_count ? scene->draw_count : 1);
    if (!shown) return NULL;
    for (uint32_t d = 0; d < scene->draw_count; ++d) {
        float computed[6];
        const float *bounds = scene->draw_bounds ? scene->draw_bounds[d] : computed;
        if (!scene->draw_bounds && visible[d].index_count) DrawBounds(scene, &scene->draws[d], computed);
        shown[d] = visible[d].index_count && LQ_BoundsInClip(clip, bounds);
    }
    return shown;
}
/* offscreen orders candidates (over budget only); hidden marks the light. */
typedef struct { float score; uint32_t offscreen, hidden, entity, source; } light_candidate;
static int CandidateOrder(const void *a, const void *b)
{
    const light_candidate *x = a, *y = b;
    if (x->offscreen != y->offscreen) return x->offscreen < y->offscreen ? -1 : 1;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    return x->entity < y->entity ? -1 : x->entity > y->entity;
}
uint32_t LQ_BuildRuntimeLightsLimited(const lq_scene *scene, const float styles[256],
    const float camera[3], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS], uint32_t slots)
{ return LQ_BuildRuntimeLightsVisible(scene, styles, camera, NULL, output, slots); }
uint32_t LQ_BuildRuntimeLightsVisible(const lq_scene *scene, const float styles[256],
    const float camera[3], const lq_draw *visible, lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS], uint32_t slots)
{ return LQ_BuildRuntimeLightsInView(scene, styles, camera, visible, NULL, output, slots); }
uint32_t LQ_BuildRuntimeLightsInView(const lq_scene *scene, const float styles[256], const float camera[3],
    const lq_draw *visible, const float clip[16], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS], uint32_t slots)
{
    lq_runtime_light previous[LQ_MAX_RUNTIME_LIGHTS];
    uint32_t count = 0, used = 0;
    if (!scene || !styles || !camera || !output) return 0;
    if (slots > LQ_MAX_RUNTIME_LIGHTS) slots = LQ_MAX_RUNTIME_LIGHTS;
    light_candidate *list = malloc((scene->light_source_count ? scene->light_source_count : 1) * sizeof(*list));
    if (!list) return 0;
    memcpy(previous, output, sizeof(previous));
    const uint32_t *view = NULL;
    if (scene->leaf_pvs && scene->light_leaves && scene->leaf_words) {
        uint32_t leaf = LQ_SceneLeaf(scene, camera);
        if (leaf < scene->leaf_count) view = scene->leaf_pvs + (size_t)leaf * scene->leaf_words;
    }
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        const lq_light_source *source = &scene->light_sources[i];
        float range = LQ_LightSourceRange(source);
        if (range <= 0) continue;
        int held = 0;
        for (uint32_t slot = 0; slot < LQ_MAX_RUNTIME_LIGHTS && !held; ++slot)
            held = previous[slot].position_radius[3] > 0 && previous[slot].source_index == i;
        if (!held && LQ_LightSourceIntensity(source, styles) <= 0) continue;
        if (view) {
            const uint32_t *reach = scene->light_leaves + (size_t)i * scene->leaf_words;
            uint32_t overlap = 0;
            for (uint32_t w = 0; w < scene->leaf_words && !overlap; ++w) overlap = reach[w] & view[w];
            if (!overlap) continue;
        }
        float dx = source->origin[0]-camera[0], dy = source->origin[1]-camera[1], dz = source->origin[2]-camera[2];
        list[count++] = (light_candidate){sqrtf(dx*dx+dy*dy+dz*dz)-fminf(range,1024), 0, 0, source->entity_index, i};
    }
    /* Drop sources that light no PVS draw. Given a selection view (slightly
     * wider than the screen), mark those that light nothing inside it
     * offscreen and, over budget, rank them after those that do. Offscreen
     * lights keep their slots, and so their cached shadow cubes, while the
     * view turns; the shadow pass defers their updates. Dropping them instead
     * (October 19) rebuilt whole cubes whenever the player looked around.
     * Under budget, rank stays by nearness alone: ranking by view as well
     * moved lights between shadow tiers, and so slots, at every turn. */
    float *range = visible && (clip || count > slots) ? calloc(scene->light_source_count, sizeof(*range)) : NULL;
    uint8_t *reach = range ? malloc(scene->light_source_count) : NULL;
    if (reach) {
        uint8_t *shown = ShownDraws(scene, visible, clip);
        for (uint32_t j = 0; j < count; ++j)
            range[list[j].source] = LQ_LightSourceRange(&scene->light_sources[list[j].source]);
        VisibleReach(scene, visible, shown, range, reach);
        uint32_t kept = 0;
        for (uint32_t j = 0; j < count; ++j) {
            if (!reach[list[j].source]) continue;
            list[j].hidden = shown && reach[list[j].source] < 2;
            list[j].offscreen = count > slots && list[j].hidden;
            list[kept++] = list[j];
        }
        count = kept;
        free(shown);
    }
    free(range); free(reach);
    qsort(list, count, sizeof(*list), CandidateOrder);
    if (count > slots) count = slots;
    /* Rank r belongs in the tier of the r-th sharpest slot. A source keeps its
     * slot (and cached cube) while its rank stays within half again of that
     * slot's tier band; otherwise it moves to a free slot of its own tier. */
    uint32_t band_start[LQ_SHADOW_TIERS] = {0}, band_end[LQ_SHADOW_TIERS] = {0}, tier_slots[LQ_SHADOW_TIERS] = {0};
    for (uint32_t slot = 0; slot < slots; ++slot) ++tier_slots[SlotTier(slot)];
    for (uint32_t t = 0, first = 0; t < LQ_SHADOW_TIERS; ++t) {
        band_start[t] = first; first += tier_slots[t]; band_end[t] = first;
    }
    uint8_t placed[LQ_MAX_RUNTIME_LIGHTS] = {0};
    memset(output, 0, sizeof(*output) * slots);
    for (uint32_t slot = 0; slot < slots; ++slot) {
        output[slot].source_index = UINT32_MAX;
        if (previous[slot].position_radius[3] <= 0) continue;
        uint32_t tier = SlotTier(slot);
        for (uint32_t j = 0; j < count; ++j) if (!placed[j] && list[j].source == previous[slot].source_index) {
            if (j < band_start[tier] / 1.5f || j >= band_end[tier] * 1.5f) break; /* re-tier */
            FillRuntimeLight(&output[slot], &scene->light_sources[list[j].source], list[j].source, styles);
            output[slot].offscreen = (uint8_t)list[j].hidden;
            placed[j] = 1; break;
        }
    }
    for (uint32_t j = 0; j < count; ++j) {
        if (placed[j]) continue;
        uint32_t want = 0;
        while (want + 1 < LQ_SHADOW_TIERS && j >= band_end[want]) ++want;
        uint32_t chosen = UINT32_MAX;
        /* Desired tier, then coarser ones, then sharper ones. */
        for (uint32_t step = 0; step < 2 * LQ_SHADOW_TIERS && chosen == UINT32_MAX; ++step) {
            int tier = step < LQ_SHADOW_TIERS ? (int)(want + step) : (int)want - (int)(step - LQ_SHADOW_TIERS + 1);
            if (tier < 0 || tier >= LQ_SHADOW_TIERS) continue;
            for (uint32_t slot = 0; slot < slots && chosen == UINT32_MAX; ++slot)
                if (output[slot].position_radius[3] <= 0 && SlotTier(slot) == (uint32_t)tier) chosen = slot;
        }
        if (chosen == UINT32_MAX) break;
        FillRuntimeLight(&output[chosen], &scene->light_sources[list[j].source], list[j].source, styles);
        output[chosen].offscreen = (uint8_t)list[j].hidden;
    }
    free(list);
    for (uint32_t slot = 0; slot < slots; ++slot)
        if (output[slot].position_radius[3] > 0) {
            output[slot].submerged = (uint8_t)LQ_SceneLiquid(scene, output[slot].position_radius);
            used = slot + 1;
        }
    return used;
}
/* Broad-phase light/surface relationships are renderer data, independent of
 * the shadow technique. A set bit means a light can reach the face by range,
 * facing and PVS; the shadow test still decides whether a caster blocks it.
 * The original baked samples are deliberately absent from this decision. */
static void DrawBounds(const lq_scene *scene, const lq_draw *draw, float out[6])
{
    out[0] = out[1] = out[2] = INFINITY; out[3] = out[4] = out[5] = -INFINITY;
    for (uint32_t j = 0; j < draw->index_count; ++j) {
        const lq_vertex *v = &scene->vertices[scene->indices[draw->first_index+j]];
        for (int axis = 0; axis < 3; ++axis) {
            out[axis] = fminf(out[axis],v->position[axis]);
            out[axis+3] = fmaxf(out[axis+3],v->position[axis]);
        }
    }
}
int LQ_SceneDrawBounds(lq_scene *scene)
{
    if (!scene || !scene->draw_count) return 0;
    float (*bounds)[6] = malloc(scene->draw_count * sizeof(*bounds));
    if (!bounds) return 0;
    for (uint32_t d = 0; d < scene->draw_count; ++d) DrawBounds(scene, &scene->draws[d], bounds[d]);
    free(scene->draw_bounds); scene->draw_bounds = bounds;
    return 1;
}
uint32_t LQ_BuildLightInteractions(const lq_scene *scene,
    const lq_runtime_light *lights, uint32_t light_count, lq_draw *output)
{ return LQ_BuildVisibleLightInteractions(scene, lights, light_count, NULL, output); }
uint32_t LQ_BuildVisibleLightInteractions(const lq_scene *scene, const lq_runtime_light *lights,
    uint32_t light_count, const lq_draw *visible, lq_draw *output)
{
    uint32_t relationships = 0;
    if (!scene || !output || (!lights && light_count)) return 0;
    if (light_count > LQ_MAX_RUNTIME_LIGHTS) light_count = LQ_MAX_RUNTIME_LIGHTS;
    memcpy(output,scene->draws,scene->draw_count*sizeof(*output));
    for (uint32_t d = 0; d < scene->draw_count; ++d) {
        lq_draw *draw = &output[d];
        memset(draw->light_bits, 0, sizeof(draw->light_bits));
        memset(draw->shadow_free_bits, 0, sizeof(draw->shadow_free_bits));
        if (draw->flags & LQ_SURFACE_SKY || draw->index_count < 3) continue;
        if (visible && !visible[d].index_count) continue;
        float computed[6];
        const float *bounds = scene->draw_bounds ? scene->draw_bounds[d] : computed;
        if (!scene->draw_bounds) DrawBounds(scene, draw, computed);
        const float *bounds_min = bounds, *bounds_max = bounds + 3;
        const lq_vertex *first = &scene->vertices[scene->indices[draw->first_index]];
        const uint32_t *reach = scene->light_visibility ? scene->light_visibility + d * scene->light_visibility_words : NULL;
        const uint32_t *clear = scene->shadow_free && scene->light_visibility ?
            scene->shadow_free + (size_t)d * scene->light_visibility_words : NULL;
        for (uint32_t i = 0; i < light_count; ++i) {
            const float *position = lights[i].position_radius;
            uint32_t source = lights[i].source_index;
            /* The precomputed reach bit rejects most pairs; gameplay lights have
             * no static reach entry, so range and facing bound them. */
            if (position[3] <= 0 ||
                (reach && source < scene->light_source_count && !(reach[source / 32] & (1u << (source % 32))))) continue;
            float toward[3], distance2 = 0;
            for (int axis = 0; axis < 3; ++axis) {
                toward[axis] = position[axis]-first->position[axis];
                float outside = position[axis] < bounds_min[axis] ? bounds_min[axis]-position[axis] :
                    position[axis] > bounds_max[axis] ? position[axis]-bounds_max[axis] : 0;
                distance2 += outside*outside;
            }
            float facing = toward[0]*first->normal[0]+toward[1]*first->normal[1]+toward[2]*first->normal[2];
            float radius = position[3];
            if (facing > 0 && distance2 < radius*radius && (lights[i].spot[3] < -1 ||
                LQ_SpotReachesBox(position, lights[i].spot, lights[i].spot[3], LQ_SPOT_EDGE_BLUR + lights[i].source_radius, bounds))) {
                draw->light_bits[i / 32] |= 1u << (i % 32);
                ++relationships;
                /* The proof holds for the source where it stands; a flame's
                 * light that has moved with its pose looks its shadow up. */
                const float *origin = source < scene->light_source_count ? scene->light_sources[source].origin : NULL;
                if (clear && origin && (clear[source / 32] & (1u << (source % 32))) && position[0] == origin[0] &&
                    position[1] == origin[1] && position[2] == origin[2])
                    draw->shadow_free_bits[i / 32] |= 1u << (i % 32);
            }
        }
    }
    return relationships;
}
#define SURFACE_POLYGON 64
static void SurfacePieces(const float (*polygon)[3], uint32_t count, float subdivide,
    float (*out)[3], uint32_t capacity, uint32_t *written, int depth)
{
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t i = 0; i < count; ++i) for (int k = 0; k < 3; ++k) {
        low[k] = fminf(low[k], polygon[i][k]); high[k] = fmaxf(high[k], polygon[i][k]);
    }
    for (int axis = 0; axis < 3 && depth < 32; ++axis) {
        float middle = subdivide * floorf((low[axis] + high[axis]) * 0.5f / subdivide + 0.5f);
        if (high[axis] - middle < 8 || middle - low[axis] < 8) continue;
        float front[SURFACE_POLYGON][3], back[SURFACE_POLYGON][3];
        uint32_t fronts = 0, backs = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const float *a = polygon[i], *b = polygon[(i + 1) % count];
            float da = a[axis] - middle, db = b[axis] - middle;
            if (da >= 0 && fronts < SURFACE_POLYGON) memcpy(front[fronts++], a, sizeof(front[0]));
            if (da <= 0 && backs < SURFACE_POLYGON) memcpy(back[backs++], a, sizeof(back[0]));
            if ((da > 0 && db < 0) || (da < 0 && db > 0)) {
                float t = da / (da - db), cut[3];
                for (int k = 0; k < 3; ++k) cut[k] = a[k] + t * (b[k] - a[k]);
                cut[axis] = middle;
                if (fronts < SURFACE_POLYGON) memcpy(front[fronts++], cut, sizeof(cut));
                if (backs < SURFACE_POLYGON) memcpy(back[backs++], cut, sizeof(cut));
            }
        }
        if (fronts >= 3) SurfacePieces((const float (*)[3])front, fronts, subdivide, out, capacity, written, depth + 1);
        if (backs >= 3) SurfacePieces((const float (*)[3])back, backs, subdivide, out, capacity, written, depth + 1);
        return;
    }
    if (*written < capacity) {
        float *centroid = out[*written];
        for (int k = 0; k < 3; ++k) {
            centroid[k] = 0;
            for (uint32_t i = 0; i < count; ++i) centroid[k] += polygon[i][k] / count;
        }
    }
    ++*written;
}
uint32_t LQ_SurfaceLightPoints(const float (*polygon)[3], uint32_t count, float subdivide,
    float (*out)[3], uint32_t capacity)
{
    uint32_t written = 0;
    if (!polygon || count < 3 || count > SURFACE_POLYGON || !(subdivide >= 16) || (capacity && !out)) return 0;
    SurfacePieces(polygon, count, subdivide, out, capacity, &written, 0);
    return written;
}
uint32_t LQ_LightBudget(const lq_scene *scene, const float styles[256], const float camera[3],
    const uint8_t *visible, uint32_t slots, lq_light_drop *dropped, uint32_t capacity,
    uint32_t *dropped_count)
{ return LQ_LightBudgetInView(scene, styles, camera, visible, NULL, NULL, slots, dropped, capacity, dropped_count); }
uint32_t LQ_LightBudgetInView(const lq_scene *scene, const float styles[256], const float camera[3],
    const uint8_t *visible, const float select_clip[16], const float count_clip[16], uint32_t slots,
    lq_light_drop *dropped, uint32_t capacity, uint32_t *dropped_count)
{
    uint32_t reaching = 0, drops = 0;
    if (dropped_count) *dropped_count = 0;
    if (!scene || !styles || !camera || (capacity && !dropped)) return 0;
    lq_runtime_light selection[LQ_MAX_RUNTIME_LIGHTS];
    memset(selection, 0, sizeof(selection)); /* no slots held from a previous frame */
    /* Select as the game does, preferring lights that reach the visible draws. */
    lq_draw *shown = visible ? malloc((scene->draw_count ? scene->draw_count : 1) * sizeof(*shown)) : NULL;
    if (visible && !shown) return 0;
    for (uint32_t d = 0; shown && d < scene->draw_count; ++d) {
        shown[d] = scene->draws[d];
        if (!visible[d]) shown[d].index_count = 0;
    }
    uint32_t used = LQ_BuildRuntimeLightsInView(scene, styles, camera, shown, select_clip, selection, slots);
    free(shown);
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        const lq_light_source *source = &scene->light_sources[i];
        float range = LQ_LightSourceRange(source);
        if (range <= 0) continue;
        float level = LQ_LightSourceIntensity(source, styles) / source->intensity; /* live style */
        if (level <= 0) continue;
        float peak = 0;
        for (uint32_t d = 0; d < scene->draw_count; ++d) {
            const lq_draw *draw = &scene->draws[d];
            if ((visible && !visible[d]) || draw->flags & LQ_SURFACE_SKY || draw->index_count < 3) continue;
            if (scene->light_visibility &&
                !(scene->light_visibility[d * scene->light_visibility_words + i / 32] & (1u << (i % 32)))) continue;
            float computed[6];
            const float *bounds = scene->draw_bounds ? scene->draw_bounds[d] : computed;
            if (!scene->draw_bounds) DrawBounds(scene, draw, computed);
            if (count_clip && !LQ_BoundsInClip(count_clip, bounds)) continue;
            const lq_vertex *first = &scene->vertices[scene->indices[draw->first_index]];
            float distance2 = 0, facing = 0;
            for (int axis = 0; axis < 3; ++axis) {
                float p = source->origin[axis];
                float outside = p < bounds[axis] ? bounds[axis] - p : p > bounds[axis+3] ? p - bounds[axis+3] : 0;
                distance2 += outside * outside;
                facing += (p - first->position[axis]) * first->normal[axis];
            }
            if (facing <= 0 || distance2 >= range * range) continue;
            peak = fmaxf(peak, LQ_LightFalloff(source, sqrtf(distance2)) * source->output_scale * level);
        }
        if (peak <= 0) continue;
        ++reaching;
        int kept = 0;
        for (uint32_t slot = 0; slot < used && !kept; ++slot)
            kept = selection[slot].position_radius[3] > 0 && selection[slot].source_index == i;
        if (kept) continue;
        ++drops;
        uint32_t at = drops - 1 < capacity ? drops - 1 : capacity;
        while (at && peak > dropped[at-1].peak) --at;
        if (at >= capacity) continue;
        uint32_t last = drops < capacity ? drops - 1 : capacity - 1;
        memmove(dropped + at + 1, dropped + at, (last - at) * sizeof(*dropped));
        dropped[at] = (lq_light_drop){i, peak};
    }
    if (dropped_count) *dropped_count = drops;
    return reaching;
}
static int ImageCheck(const lq_image *image, uint32_t channels)
{
    return image->pixels && image->width && image->height && image->width <= 8192 &&
        image->height <= 8192 && image->channels == channels;
}
int LQ_SceneCheck(const lq_scene *s, char *error, size_t size)
{
#define REQUIRE(c, message) do { if (!(c)) { snprintf(error, size, "%s", message); return 0; } } while (0)
    REQUIRE(s && s->vertices && s->indices && s->draws && s->materials, "missing scene arrays");
    REQUIRE(s->vertex_count && s->index_count && s->draw_count && s->material_count, "empty scene");
    REQUIRE(s->vertex_count <= UINT32_MAX / sizeof(lq_vertex) &&
        s->index_count <= UINT32_MAX / sizeof(uint32_t), "scene exceeds GPU buffer size");
    REQUIRE(ImageCheck(&s->colormap, 4) && s->colormap.width == 256 && s->colormap.height == 64,
        "invalid palette colormap");
    REQUIRE(ImageCheck(&s->lightmap, 4), "invalid lightmap atlas");
    for (uint32_t i = 0; i < s->material_count; ++i) {
        const lq_material *m = &s->materials[i];
        REQUIRE(ImageCheck(&m->indices, 1), "invalid indexed texture");
        REQUIRE((!m->pack_color.pixels || ImageCheck(&m->pack_color, 4)) &&
            (!m->pack_normal.pixels || ImageCheck(&m->pack_normal, 4)), "invalid texture-pack image");
    }
    for (int j = 0; j < 3; ++j)
        REQUIRE(isfinite(s->spawn_origin[j]) && isfinite(s->spawn_angles[j]), "nonfinite spawn camera");
    REQUIRE(s->light_source_count <= 4096 && (!s->light_source_count || s->light_sources), "invalid light source inventory");
    REQUIRE(!s->light_visibility || (s->light_source_count && s->light_visibility_words ==
        (s->light_source_count + 31) / 32), "invalid static light visibility");
    REQUIRE(!s->bsp_nodes || (s->bsp_node_count && s->leaf_count && s->leaf_contents), "invalid BSP nodes");
    REQUIRE(!s->leaf_pvs || (s->bsp_nodes && s->leaf_words == (s->leaf_count + 31) / 32 &&
        (!s->light_source_count || s->light_leaves)), "invalid BSP light visibility");
    for (uint32_t i = 0; s->bsp_nodes && i < s->bsp_node_count; ++i)
        REQUIRE((uint64_t)s->bsp_nodes[i].first_face + s->bsp_nodes[i].face_count <= s->draw_count, "invalid BSP node faces");
    for (uint32_t i = 0; s->leaf_contents && i < s->leaf_count; ++i)
        REQUIRE(s->leaf_contents[i] <= LQ_LEAF_LIQUID, "invalid BSP leaf contents");
    for (uint32_t i = 0; s->face_samples && i < s->draw_count; ++i) {
        const lq_face_samples *grid = &s->face_samples[i];
        REQUIRE(!grid->width || (grid->height && s->sample_positions &&
            (uint64_t)grid->first_sample + (uint64_t)grid->width * grid->height <= s->sample_count &&
            grid->atlas_x + grid->width + 1u <= s->lightmap.width && grid->atlas_y + grid->height + 1u <= s->atlas_height &&
            s->atlas_height <= s->lightmap.height), "invalid light sample grid");
    }
    REQUIRE(isfinite(s->bounce_scale) && s->bounce_scale >= 0, "invalid bounce scale");
    REQUIRE(s->world_draw_begin <= s->world_draw_end && s->world_draw_end <= s->draw_count, "invalid world draw range");
    REQUIRE(!s->bounce.pixels || (s->face_samples && s->bounce.channels == 4 && s->bounce.width == s->lightmap.width &&
        s->bounce.height == 4 * s->atlas_height), "invalid bounce atlas");
    REQUIRE(!s->probe_static || (s->probe_valid && s->probe_size[0] && s->probe_size[1] && s->probe_size[2] &&
        s->probe_size[0] <= 2048 && s->probe_size[1] <= 2048 && s->probe_size[2] * 24 <= 2048 &&
        isfinite(s->probe_spacing) && s->probe_spacing > 0 && isfinite(s->probe_origin[0]) &&
        isfinite(s->probe_origin[1]) && isfinite(s->probe_origin[2])), "invalid irradiance probe grid");
    for (uint32_t i = 0; s->bsp_nodes && i < s->bsp_node_count; ++i)
        for (int j = 0; j < 2; ++j) {
            int32_t child = s->bsp_nodes[i].children[j];
            REQUIRE(child >= 0 ? (uint32_t)child < s->bsp_node_count : (uint32_t)(-(child + 1)) < s->leaf_count,
                "invalid BSP node child");
        }
    for (uint32_t i = 0; i < s->light_source_count; ++i) {
        const lq_light_source *light = &s->light_sources[i];
        REQUIRE(!(light->flags & ~LQ_LIGHT_UNRESOLVED_PARAMETERS) && light->style < 255 && isfinite(light->intensity) && isfinite(light->cone_angle), "invalid light source parameters");
        REQUIRE(light->formula < LQ_LIGHT_FORMULA_COUNT && isfinite(light->attenuation) && light->attenuation > 0 &&
            isfinite(light->angle_scale) && light->angle_scale >= 0 && light->angle_scale <= 1 &&
            isfinite(light->output_scale) && light->output_scale >= 0 &&
            isfinite(light->source_radius) && light->source_radius >= 0 && light->source_radius <= 1024,
            "invalid light falloff parameters");
        if (light->shadow_group) {
            REQUIRE(light->shadow_group <= s->light_source_count && !light->spotlight &&
                isfinite(light->shadow_origin[0]) && isfinite(light->shadow_origin[1]) && isfinite(light->shadow_origin[2]) &&
                isfinite(light->shadow_reach) && light->shadow_reach > 0 && light->shadow_reach <= LQ_LIGHT_MAX_RANGE * 2 &&
                isfinite(light->shadow_radius) && light->shadow_radius >= 0 && light->shadow_radius <= 1024,
                "invalid panel shadow group");
        }
        if (light->spotlight) {
            float length = sqrtf(light->spot_direction[0]*light->spot_direction[0] +
                light->spot_direction[1]*light->spot_direction[1] + light->spot_direction[2]*light->spot_direction[2]);
            REQUIRE(isfinite(length) && fabsf(length - 1) < 0.001f && light->spot_outer >= -1 &&
                light->spot_outer < 1 && light->spot_inner >= light->spot_outer && light->spot_inner <= 1,
                "invalid spotlight parameters");
        }
        for (int j = 0; j < 3; ++j) {
            REQUIRE(isfinite(light->origin[j]), "nonfinite light source origin");
            REQUIRE(isfinite(light->color[j]) && light->color[j] >= 0 && light->color[j] <= 1, "invalid light source color");
        }
        REQUIRE(memchr(light->classname,0,sizeof(light->classname)) && memchr(light->target,0,sizeof(light->target)) &&
            memchr(light->targetname,0,sizeof(light->targetname)) && memchr(light->light_value,0,sizeof(light->light_value)) &&
            memchr(light->color_value,0,sizeof(light->color_value)), "unterminated light source metadata");
    }
    for (uint32_t i = 0; i < s->vertex_count; ++i) {
        const lq_vertex *v = &s->vertices[i];
        for (int j = 0; j < 3; ++j) REQUIRE(isfinite(v->position[j]) && isfinite(v->normal[j]), "nonfinite position/normal");
        for (int j = 0; j < 2; ++j)
            REQUIRE(isfinite(v->uv[j]) && isfinite(v->light_uv[j]), "nonfinite texture coordinate");
        REQUIRE(isfinite(v->shade), "nonfinite vertex shading");
    }
    for (uint32_t i = 0; i < s->index_count; ++i)
        REQUIRE(s->indices[i] < s->vertex_count, "index outside vertex array");
    for (uint32_t i = 0; i < s->draw_count; ++i) {
        const lq_draw *d = &s->draws[i];
        REQUIRE(d->material < s->material_count, "draw references invalid material");
        REQUIRE(d->first_index <= s->index_count && d->index_count <= s->index_count - d->first_index &&
            d->index_count && !(d->index_count % 3), "invalid triangle range");
        REQUIRE(!(d->flags & ~(LQ_SURFACE_SKY | LQ_SURFACE_WATER)), "invalid surface flags");
    }
    return 1;
#undef REQUIRE
}
void LQ_ViewMatrix(const lq_view *v, float aspect, float out[16])
{
    const float radians = 0.017453292519943295f, near_z = 1, far_z = 16384;
    float pitch = v->angles[0] * radians, yaw = v->angles[1] * radians, roll = v->angles[2] * radians;
    float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
    float cr = cosf(roll), sr = sinf(roll);
    float forward[3] = {cp * cy, cp * sy, -sp};
    float right[3] = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
    float up[3] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
    float horizontal = 1 / tanf(v->horizontal_fov * radians * 0.5f);
    /* Column-major matrix: Quake +X forward/+Z up -> SDL GPU depth [0,1]. */
    memset(out, 0, 16 * sizeof(float));
    for (int j = 0; j < 3; ++j) {
        out[j * 4] = right[j] * horizontal;
        out[j * 4 + 1] = up[j] * horizontal * aspect;
        out[j * 4 + 2] = forward[j] * far_z / (far_z - near_z);
        out[j * 4 + 3] = forward[j];
        out[12] -= v->origin[j] * right[j] * horizontal;
        out[13] -= v->origin[j] * up[j] * horizontal * aspect;
        out[14] -= v->origin[j] * forward[j] * far_z / (far_z - near_z);
        out[15] -= v->origin[j] * forward[j];
    }
    out[14] -= near_z * far_z / (far_z - near_z);
}
