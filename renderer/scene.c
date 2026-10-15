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
    free(s->light_sources);
    free(s->materials); free(s->vertices); free(s->indices); free(s->draws);
    free(s->colormap.pixels); free(s->lightmap.pixels); memset(s, 0, sizeof(*s));
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
    for (uint32_t i = 0; i < s->light_source_count; ++i) {
        const lq_light_source *light = &s->light_sources[i];
        REQUIRE(!(light->flags & ~LQ_LIGHT_UNRESOLVED_PARAMETERS) && light->style < 255 && isfinite(light->intensity) && isfinite(light->cone_angle), "invalid light source parameters");
        for (int j = 0; j < 3; ++j) REQUIRE(isfinite(light->origin[j]), "nonfinite light source origin");
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
