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
    for (uint32_t i = 0; i < s->material_count; ++i) free(s->materials[i].indices.pixels);
    free(s->light_sources); free(s->light_visibility);
    free(s->bsp_nodes); free(s->leaf_pvs); free(s->light_leaves);
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
/* Distance beyond which a source adds less than half a brightness unit (an
 * eighth of one colormap row) after output scaling: the culling and shadow
 * range. Sources without falloff are bounded by the largest Quake map extent. */
float LQ_LightSourceRange(const lq_light_source *source)
{
    if (!DirectSource(source)) return 0;
    float cutoff = 0.5f / source->output_scale, light = source->intensity, range;
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
    if (source->spotlight) {
        memcpy(light->spot, source->spot_direction, 3*sizeof(float));
        light->spot[3] = source->spot_outer; light->spot_inner = source->spot_inner;
    } else light->spot[3] = light->spot_inner = -2;
    light->source_radius = source->source_radius;
    light->source_index = index;
}
/* Select up to LQ_MAX_RUNTIME_LIGHTS sources whose potential reach meets the
 * camera's PVS, nearest (range-weighted) first. `output` is also the previous
 * frame's slots: a selected source keeps its slot so its shadow cube can be
 * reused, and a held source stays selected while its style is momentarily dark.
 * Returns one past the highest occupied slot; zero-range slots are empty. */
uint32_t LQ_BuildRuntimeLights(const lq_scene *scene, const float styles[256],
    const float camera[3], lq_runtime_light output[LQ_MAX_RUNTIME_LIGHTS])
{
    typedef struct { float score; uint32_t entity, source; } candidate;
    candidate selected[LQ_MAX_RUNTIME_LIGHTS];
    lq_runtime_light previous[LQ_MAX_RUNTIME_LIGHTS];
    uint32_t count = 0, used = 0;
    if (!scene || !styles || !camera || !output) return 0;
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
        candidate value = {sqrtf(dx*dx+dy*dy+dz*dz)-fminf(range,1024), source->entity_index, i};
        uint32_t at = count;
        while (at && (value.score < selected[at-1].score ||
            (value.score == selected[at-1].score && value.entity < selected[at-1].entity))) --at;
        if (at >= LQ_MAX_RUNTIME_LIGHTS) continue;
        if (count < LQ_MAX_RUNTIME_LIGHTS) ++count;
        memmove(selected+at+1,selected+at,(count-at-1)*sizeof(*selected));
        selected[at] = value;
    }
    uint8_t placed[LQ_MAX_RUNTIME_LIGHTS] = {0};
    memset(output, 0, sizeof(*output) * LQ_MAX_RUNTIME_LIGHTS);
    for (uint32_t slot = 0; slot < LQ_MAX_RUNTIME_LIGHTS; ++slot) {
        output[slot].source_index = UINT32_MAX;
        if (previous[slot].position_radius[3] <= 0) continue;
        for (uint32_t j = 0; j < count; ++j) if (!placed[j] && selected[j].source == previous[slot].source_index) {
            FillRuntimeLight(&output[slot], &scene->light_sources[selected[j].source], selected[j].source, styles);
            placed[j] = 1; break;
        }
    }
    for (uint32_t j = 0, slot = 0; j < count; ++j) {
        if (placed[j]) continue;
        while (output[slot].position_radius[3] > 0) ++slot;
        FillRuntimeLight(&output[slot], &scene->light_sources[selected[j].source], selected[j].source, styles);
    }
    for (uint32_t slot = 0; slot < LQ_MAX_RUNTIME_LIGHTS; ++slot)
        if (output[slot].position_radius[3] > 0) used = slot + 1;
    return used;
}
/* Broad-phase light/surface relationships are renderer data, independent of
 * the shadow technique. A set bit means a light can reach the face by range,
 * facing and PVS; the shadow test still decides whether a caster blocks it.
 * The original baked samples are deliberately absent from this decision. */
uint32_t LQ_BuildLightInteractions(const lq_scene *scene,
    const lq_runtime_light *lights, uint32_t light_count, lq_draw *output)
{
    uint32_t relationships = 0;
    if (!scene || !output || (!lights && light_count)) return 0;
    if (light_count > LQ_MAX_RUNTIME_LIGHTS) light_count = LQ_MAX_RUNTIME_LIGHTS;
    memcpy(output,scene->draws,scene->draw_count*sizeof(*output));
    for (uint32_t d = 0; d < scene->draw_count; ++d) {
        lq_draw *draw = &output[d];
        draw->light_bits = 0;
        if (draw->flags & (LQ_SURFACE_SKY | LQ_SURFACE_WATER) || draw->index_count < 3) continue;
        float bounds_min[3] = {INFINITY,INFINITY,INFINITY};
        float bounds_max[3] = {-INFINITY,-INFINITY,-INFINITY};
        const lq_vertex *first = &scene->vertices[scene->indices[draw->first_index]];
        for (uint32_t j = 0; j < draw->index_count; ++j) {
            const lq_vertex *v = &scene->vertices[scene->indices[draw->first_index+j]];
            for (int axis = 0; axis < 3; ++axis) {
                bounds_min[axis] = fminf(bounds_min[axis],v->position[axis]);
                bounds_max[axis] = fmaxf(bounds_max[axis],v->position[axis]);
            }
        }
        for (uint32_t i = 0; i < light_count; ++i) {
            const float *position = lights[i].position_radius;
            float toward[3], distance2 = 0;
            for (int axis = 0; axis < 3; ++axis) {
                toward[axis] = position[axis]-first->position[axis];
                float outside = position[axis] < bounds_min[axis] ? bounds_min[axis]-position[axis] :
                    position[axis] > bounds_max[axis] ? position[axis]-bounds_max[axis] : 0;
                distance2 += outside*outside;
            }
            float facing = toward[0]*first->normal[0]+toward[1]*first->normal[1]+toward[2]*first->normal[2];
            float radius = position[3];
            int visible = !scene->light_visibility ||
                (lights[i].source_index < scene->light_source_count &&
                 (scene->light_visibility[d * scene->light_visibility_words +
                     lights[i].source_index / 32] & (1u << (lights[i].source_index % 32))));
            if (radius > 0 && facing > 0 && distance2 < radius*radius && visible) {
                draw->light_bits |= 1u << i;
                ++relationships;
            }
        }
    }
    return relationships;
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
    for (uint32_t i = 0; i < s->material_count; ++i)
        REQUIRE(ImageCheck(&s->materials[i].indices, 1), "invalid indexed texture");
    for (int j = 0; j < 3; ++j)
        REQUIRE(isfinite(s->spawn_origin[j]) && isfinite(s->spawn_angles[j]), "nonfinite spawn camera");
    REQUIRE(s->light_source_count <= 4096 && (!s->light_source_count || s->light_sources), "invalid light source inventory");
    REQUIRE(!s->light_visibility || (s->light_source_count && s->light_visibility_words ==
        (s->light_source_count + 31) / 32), "invalid static light visibility");
    REQUIRE(!s->bsp_nodes || (s->bsp_node_count && s->leaf_count && s->leaf_pvs &&
        s->leaf_words == (s->leaf_count + 31) / 32 && (!s->light_source_count || s->light_leaves)),
        "invalid BSP light visibility");
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
