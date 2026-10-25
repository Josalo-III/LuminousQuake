/* Original GLQuake geometry/texture conventions: Copyright (C) 1996-1997 Id Software, Inc.
 * Copy original Quake assets into renderer-owned arrays. GPL-2.0-or-later.
 * The backend never receives model_t, allocator handles or QuakeC entities. */
#include "quakedef.h"
#include "quake_scene.h"
#include "user_paths.h"
#include "pack_images.h"
#include "pack_archive.h"
#include <stdint.h>
#include <ctype.h>
void COM_InitFilesystem(void);
static void *legacy_heap;
#define ATLAS_SIZE 2048       /* the original square atlas, still used when a map fits */
#define ATLAS_WIDE 4096       /* larger maps (BSP2): bounce layers are 4x this wide, within Metal's 16384 */
#define ATLAS_MAX_HEIGHT 16384
static int BindTorchAccent(const lq_scene *scene, lq_light_source *light);
static const char *standard_styles[] = {
    "m", "mmnmmommommnonmmonqnmmo", "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
    "mmmmmaaaaammmmmaaaaaabcdefgabcdefg", "mamamamamama", "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
    "nmonqnmomnmomomno", "mmmaaaabcdefgmmmmaaaammmaamm", "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
    "aaaaaaaazzzzzzzz", "mmamammmmammamamaaamammma", "abcdefghijklmnopqrrqponmlkjihgfedcba"
};
static int switched_off[256];
void LQ_QuakeLightstyles(float time, float values[256])
{
    uint32_t tick = (uint32_t)(fmod(fmax(time, 0), 86400) * 10);
    for (int i = 0; i < 256; ++i) {
        values[i] = i == 255 || switched_off[i] ? 0 : 264.0f / 256;
        if (i < (int)(sizeof(standard_styles) / sizeof(standard_styles[0]))) {
            const char *style = standard_styles[i];
            values[i] = (style[tick % strlen(style)] - 'a') * 22.0f / 256;
        }
    }
}
static model_t *loaded_world; /* the last world model a scene was built from */
void LQ_QuakeSceneShutdown(void) { free(legacy_heap); legacy_heap = NULL; loaded_world = NULL; }
static int Failure(lq_scene *s, char *error, size_t size, const char *message)
{
    snprintf(error, size, "%s", message); LQ_SceneFree(s); return 0;
}
static void LightText(char *dest, size_t size, const char *value, lq_light_source *light)
{
    if (strlen(value) >= size) light->flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS;
    snprintf(dest,size,"%s",value);
}
static int LightColor(lq_light_source *light)
{
    light->color[0] = light->color[1] = light->color[2] = 1;
    if (!light->color_value[0]) return 1;
    float color[3], maximum; char extra;
    if (sscanf(light->color_value,"%f %f %f %c",color,color+1,color+2,&extra) != 3 ||
        !isfinite(color[0]) || !isfinite(color[1]) || !isfinite(color[2]) ||
        color[0] < 0 || color[1] < 0 || color[2] < 0) return 0;
    maximum = fmaxf(color[0],fmaxf(color[1],color[2]));
    if (maximum <= 0 || maximum > 255) return 0;
    float scale = maximum > 1 ? 1.0f/255.0f : 1;
    for (int i = 0; i < 3; ++i) light->color[i] = color[i]*scale;
    return 1;
}
/* Reproduce LIGHT's texture-aligned, 16-unit sample lattice by projecting
 * texture coordinates back onto the oriented face plane. */
static int LightSampleOrigin(const msurface_t *face, float s, float t, float out[3])
{
    float normal[3], texture_normal[3], tangent[2][3], origin[3], dot, dist;
    const float (*axes)[4] = face->texinfo->vecs;
    for (int i = 0; i < 3; ++i) normal[i] = face->plane->normal[i] *
        (face->flags & SURF_PLANEBACK ? -1.0f : 1.0f);
    texture_normal[0] = axes[1][1]*axes[0][2] - axes[1][2]*axes[0][1];
    texture_normal[1] = axes[1][2]*axes[0][0] - axes[1][0]*axes[0][2];
    texture_normal[2] = axes[1][0]*axes[0][1] - axes[1][1]*axes[0][0];
    float length = sqrtf(DotProduct(texture_normal,texture_normal));
    if (!isfinite(length) || length < 1e-6f) return 0;
    VectorScale(texture_normal,1.0f/length,texture_normal);
    dot = DotProduct(texture_normal,normal);
    if (fabsf(dot) < 1e-6f) return 0;
    if (dot < 0) {
        for (int i = 0; i < 3; ++i) texture_normal[i] = -texture_normal[i];
        dot = -dot;
    }
    float distance_scale = 1.0f/dot;
    for (int axis = 0; axis < 2; ++axis) {
        float source[3] = {axes[axis][0],axes[axis][1],axes[axis][2]};
        length = DotProduct(source,source);
        if (!isfinite(length) || length < 1e-6f) return 0;
        dist = DotProduct(source,normal)*distance_scale;
        VectorMA(source,-dist,texture_normal,tangent[axis]);
        VectorScale(tangent[axis],1.0f/length,tangent[axis]);
    }
    for (int i = 0; i < 3; ++i) origin[i] = -axes[0][3]*tangent[0][i] - axes[1][3]*tangent[1][i];
    float face_distance = face->plane->dist * (face->flags & SURF_PLANEBACK ? -1.0f : 1.0f);
    dist = (DotProduct(origin,normal)-face_distance-1.0f)*distance_scale;
    VectorMA(origin,-dist,texture_normal,origin);
    VectorMA(origin,s,tangent[0],out);
    VectorMA(out,t,tangent[1],out);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}
static int Number(const char *text, float *value)
{
    char extra;
    return sscanf(text,"%f %c",value,&extra) == 1 && isfinite(*value);
}
typedef struct { char name[64]; float origin[3]; } light_target;
/* LIGHT/ericw-tools spotlights: "mangle" (yaw, pitch up) or a matched target
 * aims the cone; "angle" is the full cone (default 40), "_softangle" an inner
 * full cone. Unmatched targets leave an ordinary point light, as LIGHT did. */
static void LightSpot(lq_light_source *light, const char *mangle, const char *soft,
    const light_target *targets, uint32_t target_count)
{
    float direction[3] = {0}, degrees = light->cone_angle > 0 ? light->cone_angle : 40, inner = 0;
    float yaw, pitch, roll; char extra;
    if (mangle[0]) {
        int fields = sscanf(mangle,"%f %f %f %c",&yaw,&pitch,&roll,&extra);
        if (fields < 2 || fields > 3 || !isfinite(yaw) || !isfinite(pitch)) {
            light->flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS; return;
        }
        yaw *= (float)M_PI/180; pitch *= (float)M_PI/180;
        direction[0] = cosf(yaw)*cosf(pitch); direction[1] = sinf(yaw)*cosf(pitch); direction[2] = sinf(pitch);
    } else if (light->target[0]) {
        uint32_t i = 0;
        while (i < target_count && strcmp(targets[i].name,light->target)) ++i;
        if (i == target_count) return;
        VectorSubtract(targets[i].origin,light->origin,direction);
    } else return;
    float length = sqrtf(DotProduct(direction,direction));
    if (!isfinite(length) || length < 1e-6f || degrees >= 360 ||
        (soft[0] && (!Number(soft,&inner) || inner < 0))) {
        light->flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS; return;
    }
    VectorScale(direction,1.0f/length,light->spot_direction);
    light->spotlight = 1;
    light->spot_outer = cosf(degrees*0.5f*(float)M_PI/180);
    light->spot_inner = inner > 0 && inner < degrees ? cosf(inner*0.5f*(float)M_PI/180) : light->spot_outer;
}
/* Per-source keys resolved once every entity has been read. */
typedef struct {
    char mangle[128], soft[128], surface[16];
    float surface_offset;
    uint8_t sun, surface_spotlight;
} light_extra;
/* Direction a "yaw pitch [roll]" mangle points along, as for spotlights. */
static int MangleDirection(const char *mangle, float out[3])
{
    float yaw, pitch, roll; char extra;
    int fields = sscanf(mangle,"%f %f %f %c",&yaw,&pitch,&roll,&extra);
    if (fields < 2 || fields > 3 || !isfinite(yaw) || !isfinite(pitch)) return 0;
    yaw *= (float)M_PI/180; pitch *= (float)M_PI/180;
    out[0] = cosf(yaw)*cosf(pitch); out[1] = sinf(yaw)*cosf(pitch); out[2] = sinf(pitch);
    return 1;
}
#define SURFACE_LIGHT_SPACING 128.0f /* ericw-tools -surflight_subdivide default */
#define SURFACE_LIGHT_PIECES 256
/* ericw-tools `_surface` templates: a copy of the template on every world
 * face with that texture, one per ~128-unit piece, `_surface_offset` (2)
 * units out along the face normal. The template itself does not emit; maps
 * keep templates together out of sight. A liquid face places copies on its
 * upper side only, and copies inside solid are skipped. */
static uint32_t ExpandSurfaceLight(model_t *world, const lq_scene *scene, const lq_light_source *template,
    const light_extra *extra, lq_light_source *out, uint32_t capacity, uint32_t *faces)
{
    uint32_t count = 0;
    for (uint32_t d = scene->world_draw_begin; d < scene->world_draw_end && d < scene->draw_count; ++d) {
        const lq_draw *draw = &scene->draws[d];
        if (draw->flags & LQ_SURFACE_SKY || draw->material >= scene->material_count ||
            Q_strcasecmp((char *)scene->materials[draw->material].name, (char *)extra->surface)) continue;
        uint32_t corners = draw->index_count / 3 + 2, base = scene->indices[draw->first_index];
        if (corners > 64) continue;
        float polygon[64][3], points[SURFACE_LIGHT_PIECES][3];
        for (uint32_t i = 0; i < corners; ++i) memcpy(polygon[i], scene->vertices[base + i].position, sizeof(polygon[i]));
        const float *normal = scene->vertices[base].normal;
        uint32_t pieces = LQ_SurfaceLightPoints((const float (*)[3])polygon, corners, SURFACE_LIGHT_SPACING,
            points, SURFACE_LIGHT_PIECES);
        if (pieces > SURFACE_LIGHT_PIECES) pieces = SURFACE_LIGHT_PIECES;
        ++*faces;
        for (uint32_t i = 0; i < pieces; ++i) {
            vec3_t origin;
            for (int k = 0; k < 3; ++k) origin[k] = points[i][k] + normal[k] * extra->surface_offset;
            int contents = Mod_PointInLeaf(origin, world)->contents;
            if (contents == CONTENTS_SOLID || contents == CONTENTS_SKY) continue;
            if ((draw->flags & LQ_SURFACE_WATER) && contents != CONTENTS_EMPTY) continue;
            if (count == capacity) return count;
            lq_light_source *light = &out[count++];
            *light = *template;
            memcpy(light->origin, origin, sizeof(light->origin));
            memcpy(light->surface_plane, normal, 3 * sizeof(float));
            light->surface_plane[3] = DotProduct(normal, polygon[0]);
            if (extra->surface_spotlight) {
                float degrees = template->cone_angle > 0 && template->cone_angle < 360 ? template->cone_angle : 40;
                memcpy(light->spot_direction, normal, sizeof(light->spot_direction));
                light->spotlight = 1;
                light->spot_outer = light->spot_inner = cosf(degrees*0.5f*(float)M_PI/180);
            }
        }
    }
    return count;
}
static int Spawn(model_t *world, lq_scene *scene)
{
    char *text = world->entities;
    int found = 0; uint32_t capacity = 0, entity_index = 0, target_count = 0, target_capacity = 0;
    float range_scale = 0.5f, distance_scale = 1, angle_scale = 0.5f, world_gate = 0;
    light_target *targets = NULL;
    light_extra *extras = NULL;
    char world_sunlight[128] = "", world_sun_mangle[128] = "", world_sun_color[128] = "", world_penumbra[128] = "";
    memset(switched_off, 0, sizeof(switched_off));
    scene->bounce_scale = 1; scene->bounce_limit = 0;
    memset(scene->sun_direction, 0, sizeof(scene->sun_direction)); memset(scene->sun_light, 0, sizeof(scene->sun_light));
    scene->sun_angle_scale = 0.5f; scene->sun_penumbra = 0;
    while (text && (text = COM_Parse(text)) && !strcmp(com_token, "{")) {
        char classname[128] = "", origin[128] = "", angles[128] = "", yaw[128] = "";
        char delay[128] = "", wait[128] = "", mangle[128] = "", soft[128] = "", incidence[128] = "";
        char world_range[128] = "", world_distance[128] = "", deviance[128] = "", bounce[128] = "", bounces[128] = "";
        char sun[128] = "", surface[128] = "", surface_offset[128] = "", surface_spotlight[128] = "";
        char sunlight[128] = "", sun_mangle[128] = "", sun_color[128] = "", penumbra[128] = "", gate[128] = "";
        int style = 0, spawnflags = 0;
        lq_light_source light = {.entity_index=entity_index++};
        LQ_LightSourceDefaults(&light);
        while ((text = COM_Parse(text)) && strcmp(com_token, "}")) {
            char key[1024]; snprintf(key, sizeof(key), "%s", com_token);
            text = COM_Parse(text); if (!text) break;
            if (!strcmp(key, "classname")) snprintf(classname, sizeof(classname), "%s", com_token);
            else if (!strcmp(key, "origin")) snprintf(origin, sizeof(origin), "%s", com_token);
            else if (!strcmp(key, "angles")) snprintf(angles, sizeof(angles), "%s", com_token);
            else if (!strcmp(key, "angle")) snprintf(yaw, sizeof(yaw), "%s", com_token);
            else if (!strcmp(key, "delay")) snprintf(delay, sizeof(delay), "%s", com_token);
            else if (!strcmp(key, "wait")) snprintf(wait, sizeof(wait), "%s", com_token);
            else if (!strcmp(key, "mangle")) snprintf(mangle, sizeof(mangle), "%s", com_token);
            else if (!strcmp(key, "_softangle")) snprintf(soft, sizeof(soft), "%s", com_token);
            else if (!strcmp(key, "_anglescale") || !strcmp(key, "_anglesense"))
                snprintf(incidence, sizeof(incidence), "%s", com_token);
            else if (!strcmp(key, "_range")) snprintf(world_range, sizeof(world_range), "%s", com_token);
            else if (!strcmp(key, "_deviance")) snprintf(deviance, sizeof(deviance), "%s", com_token);
            else if (!strcmp(key, "_gate")) snprintf(gate, sizeof(gate), "%s", com_token);
            else if (!strcmp(key, "_bouncescale")) snprintf(bounce, sizeof(bounce), "%s", com_token);
            else if (!strcmp(key, "_bounce")) snprintf(bounces, sizeof(bounces), "%s", com_token);
            else if (!strcmp(key, "_dist")) snprintf(world_distance, sizeof(world_distance), "%s", com_token);
            else if (!strcmp(key, "_sun")) snprintf(sun, sizeof(sun), "%s", com_token);
            else if (!strcmp(key, "_surface")) snprintf(surface, sizeof(surface), "%s", com_token);
            else if (!strcmp(key, "_surface_offset")) snprintf(surface_offset, sizeof(surface_offset), "%s", com_token);
            else if (!strcmp(key, "_surface_spotlight")) snprintf(surface_spotlight, sizeof(surface_spotlight), "%s", com_token);
            else if (!strcmp(key, "_sunlight")) snprintf(sunlight, sizeof(sunlight), "%s", com_token);
            else if (!strcmp(key, "_sun_mangle") || !strcmp(key, "_sunlight_mangle"))
                snprintf(sun_mangle, sizeof(sun_mangle), "%s", com_token);
            else if (!strcmp(key, "_sunlight_color")) snprintf(sun_color, sizeof(sun_color), "%s", com_token);
            else if (!strcmp(key, "_sunlight_penumbra")) snprintf(penumbra, sizeof(penumbra), "%s", com_token);
            else if (!strcmp(key, "style")) {
                float value; char extra;
                style = sscanf(com_token,"%f %c",&value,&extra) == 1 && isfinite(value) && value >= 0 && value < 255 ? (int)value : -1;
            }
            else if (!strcmp(key, "spawnflags")) spawnflags = atoi(com_token);
            else if (!strcmp(key, "target")) LightText(light.target,sizeof(light.target),com_token,&light);
            else if (!strcmp(key, "targetname")) LightText(light.targetname,sizeof(light.targetname),com_token,&light);
            else if (!strcmp(key, "light") || !strcmp(key,"_light") || !strcmp(key,"light_lev"))
                LightText(light.light_value,sizeof(light.light_value),com_token,&light);
            else if (!strcmp(key,"_color")) LightText(light.color_value,sizeof(light.color_value),com_token,&light);
        }
        if (!strcmp(classname,"worldspawn")) {
            float value;
            if (world_range[0] && Number(world_range,&value) && value > 0) range_scale = value;
            if (world_distance[0] && Number(world_distance,&value) && value > 0) distance_scale = value;
            if (incidence[0] && Number(incidence,&value) && value >= 0 && value <= 1) angle_scale = value;
            if (gate[0] && Number(gate,&value) && value >= 0) world_gate = value;
            if (bounce[0] && Number(bounce,&value) && value >= 0 && value <= 16) scene->bounce_scale = value;
            if (bounces[0] && Number(bounces,&value) && value >= 1 && value == floorf(value))
                scene->bounce_limit = value < LQ_BOUNCE_MAX ? (uint32_t)value : LQ_BOUNCE_MAX;
            /* Neither key: the bake had no radiosity (the 1996 LIGHT never
             * bounced), so its darkness was authored without indirect light. */
            if (!bounces[0] && !bounce[0]) scene->bounce_scale = LQ_BOUNCE_SCALE_UNBAKED;
            snprintf(world_sunlight, sizeof(world_sunlight), "%s", sunlight);
            snprintf(world_sun_mangle, sizeof(world_sun_mangle), "%s", sun_mangle);
            snprintf(world_sun_color, sizeof(world_sun_color), "%s", sun_color);
            snprintf(world_penumbra, sizeof(world_penumbra), "%s", penumbra);
        }
        if (light.targetname[0] && strncmp(classname,"light",5)) {
            if (target_count == target_capacity) {
                target_capacity = target_capacity ? target_capacity*2 : 32;
                light_target *next = target_capacity <= 65536 ? realloc(targets,target_capacity*sizeof(*targets)) : NULL;
                if (!next) { free(targets); free(extras); return 0; }
                targets = next;
            }
            light_target *entry = &targets[target_count];
            if (sscanf(origin,"%f %f %f",&entry->origin[0],&entry->origin[1],&entry->origin[2]) == 3 &&
                isfinite(entry->origin[0]) && isfinite(entry->origin[1]) && isfinite(entry->origin[2])) {
                snprintf(entry->name,sizeof(entry->name),"%s",light.targetname); ++target_count;
            }
        }
        if (!strncmp(classname,"light",5)) {
            LightText(light.classname,sizeof(light.classname),classname,&light);
            light.style = style; light.spawnflags = spawnflags;
            if (style < 0 || style >= 255 ||
                sscanf(origin,"%f %f %f",&light.origin[0],&light.origin[1],&light.origin[2]) != 3 ||
                !isfinite(light.origin[0]) || !isfinite(light.origin[1]) || !isfinite(light.origin[2])) {
                Con_DPrintf("Light inventory: skipping invalid source entity %u.\n",light.entity_index);
            } else {
                char extra; float intensity = 0;
                if (light.light_value[0] && (sscanf(light.light_value,"%f %c",&intensity,&extra) != 1 || !isfinite(intensity))) {
                    light.flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS; intensity = 0;
                }
                /* Original LIGHT/entities.c uses 300 for absent/zero light. */
                light.intensity = light.flags ? 0 : intensity == 0 ? 300 : intensity;
                if (yaw[0] && (sscanf(yaw,"%f %c",&light.cone_angle,&extra) != 1 || !isfinite(light.cone_angle))) {
                    light.cone_angle = 0; light.flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS;
                }
                if (!LightColor(&light)) light.flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS;
                float value;
                light.angle_scale = angle_scale; light.output_scale = light.output_base = range_scale;
                light.attenuation = distance_scale;
                light.gate = world_gate;
                if (gate[0] && Number(gate,&value) && value >= 0) light.gate = value;
                if (delay[0]) {
                    if (Number(delay,&value) && value >= 0 && value < LQ_LIGHT_FORMULA_COUNT && value == floorf(value))
                        light.formula = (uint32_t)value;
                    else light.flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS;
                }
                if (wait[0]) {
                    if (Number(wait,&value) && value > 0) light.attenuation *= value;
                    else light.flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS;
                }
                if (incidence[0]) {
                    if (Number(incidence,&value) && value >= 0 && value <= 1) light.angle_scale = value;
                    else light.flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS;
                }
                if (deviance[0]) { /* ericw-tools' jittered-sphere radius */
                    if (Number(deviance,&value) && value >= 0 && value <= 1024) light.source_radius = value;
                    else light.flags |= LQ_LIGHT_UNRESOLVED_PARAMETERS;
                }
                if (scene->light_source_count == capacity) {
                    if (capacity == 4096) { free(targets); free(extras); return 0; }
                    capacity = capacity ? capacity*2 : 32;
                    lq_light_source *sources = realloc(scene->light_sources,capacity*sizeof(*sources));
                    light_extra *next = realloc(extras,capacity*sizeof(*extras));
                    if (next) extras = next;
                    if (!sources || !next) { if (sources) scene->light_sources = sources; free(targets); free(extras); return 0; }
                    scene->light_sources = sources;
                }
                light_extra *keys = &extras[scene->light_source_count];
                memset(keys, 0, sizeof(*keys));
                snprintf(keys->mangle,sizeof(keys->mangle),"%s",mangle);
                snprintf(keys->soft,sizeof(keys->soft),"%s",soft);
                keys->sun = Number(sun,&value) && value != 0;
                snprintf(keys->surface,sizeof(keys->surface),"%s",surface);
                keys->surface_offset = Number(surface_offset,&value) && fabsf(value) <= 256 ? value : 2;
                keys->surface_spotlight = Number(surface_spotlight,&value) && value != 0;
                scene->light_sources[scene->light_source_count++] = light;
            }
        }
        if (style >= 32 && style < 255 && !strncmp(classname, "light", 5))
            switched_off[style] = (spawnflags & 1) != 0;
        if (!found && !strcmp(classname, "info_player_start")) {
            if (sscanf(origin, "%f %f %f", &scene->spawn_origin[0], &scene->spawn_origin[1], &scene->spawn_origin[2]) == 3) {
                if (sscanf(angles, "%f %f %f", &scene->spawn_angles[0], &scene->spawn_angles[1], &scene->spawn_angles[2]) != 3)
                    scene->spawn_angles[1] = (float)atof(yaw);
                scene->spawn_origin[2] += 22; found = 1;
            }
        }
    }
    /* Global worldspawn keys precede the lights in a normal map, but resolve
     * them and the targets only after every entity has been read. */
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        lq_light_source *light = &scene->light_sources[i];
        if (!light->flags) LightSpot(light,extras[i].mangle,extras[i].soft,targets,target_count);
    }
    free(targets);
    /* ericw-tools sunlight: the strongest `_sun` light (aimed by target or
     * mangle, else straight down) or worldspawn `_sunlight`. One directional
     * light; a sun too faint to add a brightness unit is left out. */
    float strongest = 0, value;
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        const lq_light_source *light = &scene->light_sources[i];
        float strength = light->intensity * light->output_base;
        if (!extras[i].sun || light->flags || strength <= strongest) continue;
        static const float down[3] = {0, 0, -1};
        memcpy(scene->sun_direction, light->spotlight ? light->spot_direction : down, sizeof(scene->sun_direction));
        for (int k = 0; k < 3; ++k) scene->sun_light[k] = light->color[k] * strength;
        scene->sun_angle_scale = light->angle_scale; strongest = strength;
    }
    char extra_text;
    if (world_sunlight[0] && sscanf(world_sunlight,"%f",&value) == 1 && isfinite(value) && value * range_scale > strongest) {
        lq_light_source tint; LQ_LightSourceDefaults(&tint);
        snprintf(tint.color_value,sizeof(tint.color_value),"%s",world_sun_color);
        float direction[3] = {0, 0, -1};
        if ((!world_sun_mangle[0] || MangleDirection(world_sun_mangle,direction)) && LightColor(&tint)) {
            float length = sqrtf(DotProduct(direction,direction));
            for (int k = 0; k < 3; ++k) {
                scene->sun_direction[k] = direction[k] / length; scene->sun_light[k] = tint.color[k] * value * range_scale;
            }
            scene->sun_angle_scale = angle_scale; strongest = value * range_scale;
        }
    }
    if (strongest < 1) { memset(scene->sun_light, 0, sizeof(scene->sun_light)); memset(scene->sun_direction, 0, sizeof(scene->sun_direction)); }
    if (world_penumbra[0] && sscanf(world_penumbra,"%f %c",&value,&extra_text) == 1 && value >= 0 && value <= 90)
        scene->sun_penumbra = value;
    /* Suns and surface templates leave the point-light inventory; templates
     * are replaced by their copies. */
    uint32_t templates = 0, faces = 0, kept = 0, total = 0;
    for (uint32_t i = 0; i < scene->light_source_count; ++i)
        total += !extras[i].sun && !extras[i].surface[0];
    lq_light_source *sources = malloc(4096 * sizeof(*sources));
    if (!sources) { free(extras); return 0; }
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        const lq_light_source *light = &scene->light_sources[i];
        if (extras[i].sun) continue;
        if (!extras[i].surface[0]) { if (kept < 4096) sources[kept++] = *light; continue; }
        ++templates;
        if (!light->flags) kept += ExpandSurfaceLight(world, scene, light, &extras[i], sources + kept, 4096 - kept, &faces);
    }
    if (kept == 4096) Con_DPrintf("Light inventory: surface lights stop at the 4096-source limit.\n");
    free(scene->light_sources); free(extras);
    lq_light_source *fitted = realloc(sources, (kept ? kept : 1) * sizeof(*sources));
    scene->light_sources = fitted ? fitted : sources; scene->light_source_count = kept;
    uint32_t torch_accents = 0;
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        lq_light_source *light = &scene->light_sources[i];
        if (!LQ_LightIsAccent(light)) continue;
        light->accent_gain = LQ_ACCENT_SPOT_GAIN; light->output_scale = light->output_base * LQ_ACCENT_SPOT_GAIN;
        if (BindTorchAccent(scene, light)) ++torch_accents;
    }
    if (torch_accents) printf("Light inventory: %u accent spotlights follow their torch's flame\n", torch_accents);
    if (templates) printf("Light inventory: %u surface templates on %u faces became %u lights\n",
        templates, faces, kept - (total < kept ? total : kept));
    if (scene->sun_light[0] + scene->sun_light[1] + scene->sun_light[2] > 0)
        printf("Light inventory: sun %.0f %.0f %.0f along %.2f %.2f %.2f, penumbra %.0f degrees\n",
            scene->sun_light[0], scene->sun_light[1], scene->sun_light[2],
            scene->sun_direction[0], scene->sun_direction[1], scene->sun_direction[2], scene->sun_penumbra);
    return 1;
}
/* Renderer-owned world BSP: nodes with their on-plane faces and leaf contents,
 * for point lookup and Luminous ray casts. */
static int BuildBSP(model_t *world, lq_scene *scene)
{
    uint32_t leaves = (uint32_t)world->numleafs + 1;
    if (!world->nodes || world->numnodes <= 0 || world->numleafs <= 0) return 1;
    scene->bsp_nodes = calloc((size_t)world->numnodes,sizeof(*scene->bsp_nodes));
    scene->leaf_contents = calloc(leaves,1);
    if (!scene->bsp_nodes || !scene->leaf_contents) return 0;
    scene->leaf_count = leaves;
    for (uint32_t l = 0; l < leaves; ++l) {
        int contents = l ? world->leafs[l].contents : CONTENTS_SOLID;
        scene->leaf_contents[l] = contents == CONTENTS_EMPTY ? LQ_LEAF_EMPTY : contents == CONTENTS_SKY ? LQ_LEAF_SKY :
            contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA ? LQ_LEAF_LIQUID : LQ_LEAF_SOLID;
    }
    /* The node array also holds brush-model trees; keep only the world tree,
     * renumbered in discovery order from its root. */
    const mnode_t **order = malloc((size_t)world->numnodes * sizeof(*order));
    int32_t *remap = malloc((size_t)world->numnodes * sizeof(*remap));
    if (!order || !remap) { free(order); free(remap); return 0; }
    for (int i = 0; i < world->numnodes; ++i) remap[i] = -1;
    uint32_t count = 0;
    order[count] = world->nodes; remap[0] = (int32_t)count++;
    for (uint32_t i = 0; i < count; ++i) {
        const mnode_t *node = order[i];
        lq_bsp_node *out = &scene->bsp_nodes[i];
        VectorCopy(node->plane->normal,out->normal); out->dist = node->plane->dist;
        out->first_face = node->firstsurface; out->face_count = node->numsurfaces;
        if ((uint64_t)out->first_face + out->face_count > scene->draw_count) out->face_count = 0;
        for (int j = 0; j < 2; ++j) {
            const mnode_t *child = node->children[j];
            if (child->contents < 0) {
                ptrdiff_t leaf = (const mleaf_t *)child - world->leafs;
                out->children[j] = leaf >= 0 && (uint32_t)leaf < leaves ? -(int32_t)leaf - 1 : -1;
                continue;
            }
            ptrdiff_t index = child - world->nodes;
            if (index < 0 || index >= world->numnodes) { free(order); free(remap); return 0; }
            if (remap[index] < 0) { remap[index] = (int32_t)count; order[count++] = child; }
            out->children[j] = remap[index];
        }
    }
    free(order); free(remap);
    scene->bsp_node_count = count;
    return 1;
}
/* Conservative light reach from the compiled PVS: a source can only light
 * faces in leaves its own leaf can see, within its falloff range. Shadows, not
 * this table, decide occlusion; it only bounds per-frame light selection and
 * per-face work. Brush-model faces are kept for every source. */
static int BuildLightReach(model_t *world, lq_scene *scene)
{
    uint32_t leaves = scene->leaf_count, words = (leaves + 31) / 32;
    uint64_t pairs = 0;
    if (!scene->bsp_nodes || !scene->light_source_count) return 1;
    if ((uint64_t)leaves * words * sizeof(uint32_t) > 32u * 1024 * 1024) {
        printf("Light reach: %u leaves exceed the PVS table budget; all lights stay eligible.\n",leaves);
        return 1;
    }
    scene->leaf_pvs = calloc((size_t)leaves*words,sizeof(uint32_t));
    scene->light_leaves = calloc((size_t)scene->light_source_count*words,sizeof(uint32_t));
    uint32_t source_words = (scene->light_source_count + 31) / 32;
    scene->light_visibility = calloc((size_t)scene->draw_count*source_words,sizeof(uint32_t));
    if (!scene->leaf_pvs || !scene->light_leaves || !scene->light_visibility) return 0;
    scene->leaf_words = words; scene->light_visibility_words = source_words;
    for (uint32_t l = 0; l < leaves; ++l) {
        const byte *vis = Mod_LeafPVS(world->leafs + l, world);
        uint32_t *row = scene->leaf_pvs + (size_t)l * words;
        row[l / 32] |= 1u << (l % 32);
        for (int j = 0; j < world->numleafs; ++j)
            if (vis[j >> 3] & (1 << (j & 7))) row[(j + 1) / 32] |= 1u << ((j + 1) % 32);
    }
    int first = world->firstmodelsurface, last = world->firstmodelsurface + world->nummodelsurfaces;
    for (uint32_t source = 0; source < scene->light_source_count; ++source) {
        lq_light_source *light = &scene->light_sources[source];
        float range = LQ_LightSourceRange(light);
        if (range <= 0) continue;
        uint32_t own = (uint32_t)(Mod_PointInLeaf(light->origin,world) - world->leafs);
        const uint32_t *pvs = scene->leaf_pvs + (size_t)own * words;
        uint32_t *reach = scene->light_leaves + (size_t)source * words;
        for (uint32_t l = 0; l < leaves; ++l) {
            if (!(pvs[l / 32] & (1u << (l % 32)))) continue;
            const mleaf_t *leaf = world->leafs + l;
            float distance2 = 0;
            for (int axis = 0; axis < 3; ++axis) {
                float low = leaf->minmaxs[axis], high = leaf->minmaxs[axis + 3], p = light->origin[axis];
                float outside = p < low ? low - p : p > high ? p - high : 0;
                distance2 += outside * outside;
            }
            if (distance2 > range * range) continue;
            reach[l / 32] |= 1u << (l % 32);
            for (int k = 0; k < leaf->nummarksurfaces; ++k) {
                int face = (int)(leaf->firstmarksurface[k] - world->surfaces);
                if (face >= 0 && (uint32_t)face < scene->draw_count)
                    scene->light_visibility[(size_t)face*source_words + source/32] |= 1u << (source % 32);
            }
        }
        for (uint32_t face = 0; face < scene->draw_count; ++face)
            if ((int)face < first || (int)face >= last)
                scene->light_visibility[(size_t)face*source_words + source/32] |= 1u << (source % 32);
    }
    for (size_t i = 0; i < (size_t)scene->draw_count*source_words; ++i)
        for (uint32_t bits = scene->light_visibility[i]; bits; bits &= bits - 1) ++pairs;
    printf("Light reach: %u leaves, %llu potential face/source pairs.\n",leaves,(unsigned long long)pairs);
    return 1;
}
static int SolidPoint(model_t *world, float point[3])
{
    int contents = Mod_PointInLeaf(point,world)->contents;
    return contents == CONTENTS_SOLID || contents == CONTENTS_SKY;
}
/* LIGHT's per-face sample lattice for Luminous bounce, one unit in front of
 * the face. Points that start inside solid move toward the face center, as
 * LIGHT does before tracing; any that cannot are NaN (no sample). */
/* The height the import's shelf packing needs for a given atlas width, or -1
 * when a face's lightmap is wider than the atlas. Mirrors the packing below. */
static int AtlasHeight(model_t *world, int width)
{
    int x = 1, y = 1, row = 0;
    for (int i = 0; i < world->numsurfaces; ++i) {
        msurface_t *f = world->surfaces + i;
        if (f->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
        int w = (f->extents[0] >> 4) + 1, h = (f->extents[1] >> 4) + 1;
        if (w < 1 || h < 1 || w + 2 > width) return -1;
        if (x + w + 1 > width) { x = 1; y += row; row = 0; }
        x += w + 2;
        if (h + 2 > row) row = h + 2;
    }
    return row ? y + row - 1 : y;    /* the tallest last-row face needs y + h + 1 */
}

/* A rerelease accent spotlight beside a torch: bind it to the nearest
 * light_torch_* or light_flame_* so it follows that flame's strength, and give
 * all but the central LQ_TORCH_ACCENT_CORE of its lit area to the penumbra.
 * The flat core's radius is sqrt(core) of the cone's at any distance, so its
 * half-angle has tan(inner) = sqrt(core) * tan(outer). An authored tighter
 * _softangle is kept. */
static int BindTorchAccent(const lq_scene *scene, lq_light_source *light)
{
    float best = LQ_TORCH_ACCENT_REACH * LQ_TORCH_ACCENT_REACH;
    const lq_light_source *flame = NULL;
    for (uint32_t j = 0; j < scene->light_source_count; ++j) {
        const lq_light_source *other = &scene->light_sources[j];
        if (strncmp(other->classname, "light_torch", 11) && strncmp(other->classname, "light_flame", 11)) continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) d += (other->origin[k] - light->origin[k]) * (other->origin[k] - light->origin[k]);
        if (d < best) { best = d; flame = other; }
    }
    if (!flame) return 0;
    memcpy(light->flame_origin, flame->origin, sizeof(light->flame_origin));
    light->flame_bound = 1;
    if (light->spot_outer > 0.01f && light->spot_outer < 1) {
        float outer = acosf(light->spot_outer);
        float inner = cosf(atanf(sqrtf(LQ_TORCH_ACCENT_CORE) * tanf(outer)));
        if (inner > light->spot_inner) light->spot_inner = inner;
    }
    return 1;
}

static int BuildSampleGrids(model_t *world, lq_scene *s, const uint16_t (*atlas)[2])
{
    uint64_t total = 0;
    float (*albedo)[3] = calloc(s->material_count,sizeof(*albedo));
    s->face_samples = calloc(s->draw_count,sizeof(*s->face_samples));
    if (!albedo || !s->face_samples) { free(albedo); return 0; }
    for (uint32_t m = 0; m < s->material_count; ++m) {
        const lq_image *image = &s->materials[m].indices;
        double sum[3] = {0};
        size_t pixels = (size_t)image->width * image->height;
        for (size_t i = 0; i < pixels; ++i)
            for (int c = 0; c < 3; ++c) sum[c] += s->colormap.pixels[(31*256 + image->pixels[i])*4 + c];
        for (int c = 0; c < 3; ++c) albedo[m][c] = pixels ? (float)(sum[c] / pixels / 255.0) : 0;
    }
    for (uint32_t i = 0; i < s->draw_count && i < (uint32_t)world->numsurfaces; ++i)
        if (!s->draws[i].flags) {
            const msurface_t *f = world->surfaces + i;
            total += (uint64_t)((f->extents[0] >> 4) + 1) * ((f->extents[1] >> 4) + 1);
        }
    if (total > 16u * 1024 * 1024) { free(albedo); return 0; }
    s->sample_positions = malloc((size_t)(total ? total : 1) * sizeof(*s->sample_positions));
    if (!s->sample_positions) { free(albedo); return 0; }
    for (uint32_t i = 0; i < s->draw_count && i < (uint32_t)world->numsurfaces; ++i) {
        const lq_draw *draw = &s->draws[i];
        if (draw->flags) continue;
        msurface_t *f = world->surfaces + i;
        lq_face_samples *grid = &s->face_samples[i];
        for (int k = 0; k < 4; ++k) { grid->s[k] = f->texinfo->vecs[0][k]; grid->t[k] = f->texinfo->vecs[1][k]; }
        grid->mins[0] = f->texturemins[0]; grid->mins[1] = f->texturemins[1];
        grid->width = (uint16_t)((f->extents[0] >> 4) + 1); grid->height = (uint16_t)((f->extents[1] >> 4) + 1);
        grid->atlas_x = atlas[i][0]; grid->atlas_y = atlas[i][1];
        memcpy(grid->albedo, albedo[draw->material], sizeof(grid->albedo));
        grid->first_sample = s->sample_count;
        float center[3] = {0};
        const float *normal = s->vertices[s->indices[draw->first_index]].normal;
        for (uint32_t j = 0; j < draw->index_count; ++j)
            for (int axis = 0; axis < 3; ++axis) center[axis] += s->vertices[s->indices[draw->first_index + j]].position[axis];
        for (int axis = 0; axis < 3; ++axis) center[axis] = center[axis] / draw->index_count + normal[axis];
        for (int y = 0; y < grid->height; ++y) for (int x = 0; x < grid->width; ++x) {
            float *point = s->sample_positions[s->sample_count++];
            if (!LightSampleOrigin(f,(float)(f->texturemins[0] + x*16),(float)(f->texturemins[1] + y*16),point)) {
                point[0] = point[1] = point[2] = NAN; continue;
            }
            if (!SolidPoint(world,point)) continue;
            float start[3]; VectorCopy(point,start);
            int moved = 0;
            for (int step = 1; step <= 4 && !moved; ++step) {
                for (int axis = 0; axis < 3; ++axis) point[axis] = start[axis] + (center[axis] - start[axis]) * step * 0.25f;
                moved = !SolidPoint(world,point);
            }
            if (!moved) point[0] = point[1] = point[2] = NAN;
        }
    }
    free(albedo);
    return 1;
}
int LQ_QuakeSceneLoad(int argc, char **argv, const char *map, lq_scene *s, char *error, size_t size)
{
    model_t *world;
    byte *palette, *colormap;
    texture_t **texture_sources;
    char path[MAX_QPATH];
    uint64_t vertices = 0, indices = 0;
    int atlas_x = 1, atlas_y = 1, row_height = 0, option;
    int atlas_width = ATLAS_SIZE, atlas_height = ATLAS_SIZE;
    int lighting_bytes;
    int light_analysis = 0;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i],"--light-analysis")) light_analysis = 1;
    if (!map || !*map || strlen(map) > MAX_QPATH - 10 || strstr(map, "..") || strchr(map, '/') || strchr(map, '\\'))
        return Failure(s, error, size, "Use a map name such as e1m1, without a path or extension");
    if (argc && legacy_heap) return Failure(s, error, size, "A legacy scene is already loaded");
    if (argc >= MAX_NUM_ARGVS) return Failure(s, error, size, "Too many command-line arguments");
    if (argc) {
    LQ_ReadOnlyFiles();
    COM_InitArgv(argc, argv);
    option = COM_CheckParm("-basedir");
    if (option && (option + 1 >= com_argc || strlen(com_argv[option + 1]) + 32 >= MAX_OSPATH))
        return Failure(s, error, size, "Missing/overlong -basedir; use a shorter path to local assets");
    legacy_heap = malloc(256 * 1024 * 1024);
    if (!legacy_heap) return Failure(s, error, size, "Cannot allocate legacy asset heap");
    host_parms.basedir = "."; host_parms.membase = legacy_heap; host_parms.memsize = 256 * 1024 * 1024;
    Memory_Init(legacy_heap, 256 * 1024 * 1024); Cbuf_Init(); Cmd_Init();
    /* An asset viewer, like --check-data, initializes discovery without running
     * gameplay startup or the legacy registered-game/mod check. */
    COM_InitByteOrder(); COM_InitFilesystem(); Mod_Init(); R_InitTextures();
    palette = COM_LoadHunkFile("gfx/palette.lmp");
    if (!palette || com_filesize != 768) return Failure(s, error, size, "Missing/invalid gfx/palette.lmp");
    colormap = COM_LoadHunkFile("gfx/colormap.lmp");
    if (!colormap || com_filesize < 256 * 64) return Failure(s, error, size, "Missing/invalid gfx/colormap.lmp");
    } else {
        palette = host_basepal; colormap = host_colormap;
        if (!palette || !colormap || !cl.worldmodel)
            return Failure(s, error, size, "No live world assets");
    }
    s->colormap = (lq_image){256, 64, 4, malloc(256 * 64 * 4)};
    if (!s->colormap.pixels) return Failure(s, error, size, "Cannot allocate lighting images");
    for (int i = 0; i < 256 * 64; ++i) {
        memcpy(s->colormap.pixels + i * 4, palette + colormap[i] * 3, 3);
        s->colormap.pixels[i * 4 + 3] = 255;
    }
    snprintf(path, sizeof(path), "maps/%s.bsp", map);
    { dheader_t header; FILE *file = NULL;
      int bytes = COM_FOpenFile(path, &file);
      if (!file) return Failure(s, error, size, "Missing map");
      size_t count = fread(&header, 1, sizeof(header), file); fclose(file);
      if (bytes < (int)sizeof(header) || count != sizeof(header)) return Failure(s, error, size, "Truncated map");
      lighting_bytes = LittleLong(header.lumps[LUMP_LIGHTING].filelen);
      if (lighting_bytes < 0 || lighting_bytes > bytes) return Failure(s, error, size, "Invalid map lighting range"); }
    world = argc ? Mod_ForName(path, false) : cl.worldmodel;
    if (!world || world->type != mod_brush) return Failure(s, error, size, "Cannot load brush map");
    loaded_world = world;
    /* Large maps outgrow the 2048x2048 lightmap atlas; they get a 4096-wide one
     * just tall enough for their faces. */
    atlas_width = ATLAS_SIZE; atlas_height = AtlasHeight(world, atlas_width);
    if (atlas_height < 0 || atlas_height > ATLAS_SIZE) {
        atlas_width = ATLAS_WIDE; atlas_height = AtlasHeight(world, atlas_width);
        if (atlas_height < 0) return Failure(s, error, size, "Invalid surface lightmap size");
        if (atlas_height > ATLAS_MAX_HEIGHT) return Failure(s, error, size, "Lightmap atlas full; multiple pages are not implemented yet");
        atlas_height = (atlas_height + 1 + 3) & ~3;   /* room for the packed rows' full height */
    } else atlas_height = ATLAS_SIZE;
    s->lightmap = (lq_image){(uint32_t)atlas_width, (uint32_t)atlas_height, 4, calloc((size_t)atlas_width * atlas_height, 4)};
    if (!s->lightmap.pixels) return Failure(s, error, size, "Cannot allocate lighting images");
    if (light_analysis)
        puts("LQ_LIGHT_SAMPLE,map,face,sample_s,sample_t,x,y,z,nx,ny,nz,style,value");
    uint64_t analysis_samples = 0;
    for (int i = 0; i < world->numsurfaces; ++i) {
        msurface_t *f = world->surfaces + i;
        if (f->numedges < 3 || f->numedges > 65535) return Failure(s, error, size, "Invalid BSP polygon size");
        vertices += f->numedges; indices += (f->numedges - 2) * 3;
    }
    if (vertices > UINT32_MAX / sizeof(lq_vertex) || indices > UINT32_MAX / sizeof(uint32_t))
        return Failure(s, error, size, "Map exceeds GPU buffer limits");
    s->vertices = calloc((size_t)vertices, sizeof(*s->vertices));
    s->indices = calloc((size_t)indices, sizeof(*s->indices));
    s->draws = calloc(world->numsurfaces, sizeof(*s->draws));
    s->materials = calloc(world->numtextures + 1, sizeof(*s->materials));
    texture_sources = calloc(world->numtextures + 1, sizeof(*texture_sources));
    uint16_t (*face_atlas)[2] = calloc(world->numsurfaces, sizeof(*face_atlas));
    if (!s->vertices || !s->indices || !s->draws || !s->materials || !texture_sources || !face_atlas) {
        free(texture_sources); free(face_atlas); return Failure(s, error, size, "Cannot allocate scene geometry");
    }
#define FAIL(message) do { free(texture_sources); free(face_atlas); return Failure(s, error, size, message); } while (0)
    for (int i = 0; i < world->numsurfaces; ++i) {
        msurface_t *f = world->surfaces + i;
        texture_t *texture = f->texinfo->texture;
        uint32_t material = 0, base = s->vertex_count;
        lq_draw *draw = &s->draws[s->draw_count++];
        int light_width = (f->extents[0] >> 4) + 1, light_height = (f->extents[1] >> 4) + 1;
        int light_x = 0, light_y = 0;
        while (material < s->material_count && texture_sources[material] != texture) ++material;
        if (material == s->material_count) {
            if (!texture || s->material_count >= (uint32_t)world->numtextures + 1 || !texture->width || !texture->height ||
                texture->width > 8192 || texture->height > 8192) FAIL("Invalid BSP texture");
            lq_material *m = &s->materials[s->material_count++];
            texture_sources[material] = texture;
            m->indices = (lq_image){texture->width, texture->height, 1, malloc((size_t)texture->width * texture->height)};
            if (!m->indices.pixels) FAIL("Cannot allocate texture copy");
            memcpy(m->indices.pixels, (byte *)texture + texture->offsets[0], (size_t)texture->width * texture->height);
            memcpy(m->name, texture->name, 16); m->name[16] = 0;
        }
        draw->first_index = s->index_count; draw->index_count = (f->numedges - 2) * 3; draw->material = material;
        draw->flags = (f->flags & SURF_DRAWSKY ? LQ_SURFACE_SKY : 0) |
            (f->flags & SURF_DRAWTURB ? LQ_SURFACE_WATER : 0);
        memset(draw->styles, 255, 4); draw->styles[0] = 0;
        memset(draw->bounce_styles, 255, 4);
        if (!draw->flags) {
            if (light_width < 1 || light_height < 1 || light_width + 2 > atlas_width || light_height + 2 > atlas_height)
                FAIL("Invalid surface lightmap size");
            if (atlas_x + light_width + 1 > atlas_width) { atlas_x = 1; atlas_y += row_height; row_height = 0; }
            if (atlas_y + light_height + 1 > atlas_height) FAIL("Lightmap atlas full; multiple pages are not implemented yet");
            light_x = atlas_x; light_y = atlas_y; atlas_x += light_width + 2;
            face_atlas[i][0] = (uint16_t)light_x; face_atlas[i][1] = (uint16_t)light_y;
            if (light_height + 2 > row_height) row_height = light_height + 2;
            int maps = 0;
            if (f->samples) {
                while (maps < 4 && f->styles[maps] != 255) ++maps;
                if (!world->lightdata || (uintptr_t)f->samples < (uintptr_t)world->lightdata ||
                    (uintptr_t)f->samples - (uintptr_t)world->lightdata > (uintptr_t)lighting_bytes ||
                    (size_t)maps * light_width * light_height > (size_t)lighting_bytes -
                        ((uintptr_t)f->samples - (uintptr_t)world->lightdata)) FAIL("Surface samples outside BSP lighting");
                memcpy(draw->styles, f->styles, 4);
                if (light_analysis) for (int y = 0; y < light_height; ++y) for (int x = 0; x < light_width; ++x) {
                    float point[3];
                    if (!LightSampleOrigin(f,(float)(f->texturemins[0]+x*16),
                            (float)(f->texturemins[1]+y*16),point))
                        FAIL("Cannot reconstruct lightmap sample position");
                    for (int layer = 0; layer < maps; ++layer) {
                        int value = f->samples[layer*light_width*light_height+y*light_width+x];
                        float sign = f->flags & SURF_PLANEBACK ? -1.0f : 1.0f;
                        printf("LQ_LIGHT_SAMPLE,%s,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%d\n",
                            map,i,x,y,point[0],point[1],point[2],f->plane->normal[0]*sign,
                            f->plane->normal[1]*sign,f->plane->normal[2]*sign,f->styles[layer],value);
                        ++analysis_samples;
                    }
                }
            }
            for (int y = -1; y <= light_height; ++y) for (int x = -1; x <= light_width; ++x) {
                int sx = x < 0 ? 0 : x >= light_width ? light_width - 1 : x;
                int sy = y < 0 ? 0 : y >= light_height ? light_height - 1 : y;
                uint8_t *pixel = s->lightmap.pixels + ((size_t)(light_y + y) * atlas_width + light_x + x) * 4;
                for (int j = 0; j < maps; ++j) pixel[j] = f->samples[j * light_width * light_height + sy * light_width + sx];
                if (!world->lightdata) pixel[0] = 255;
            }
        }
        if (f->firstedge < 0 || f->firstedge > world->numsurfedges || f->numedges > world->numsurfedges - f->firstedge)
            FAIL("Surface edge range outside BSP");
        for (int j = 0; j < f->numedges; ++j) {
            int edge = world->surfedges[f->firstedge + j];
            if (edge == INT_MIN || abs(edge) >= world->numedges) FAIL("Edge outside BSP");
            int vertex = world->edges[abs(edge)].v[edge < 0];
            if (vertex >= world->numvertexes) FAIL("Vertex outside BSP");
            lq_vertex *v = &s->vertices[s->vertex_count++];
            memcpy(v->position, world->vertexes[vertex].position, sizeof(v->position));
            for (int k = 0; k < 3; ++k)
                v->normal[k] = f->plane->normal[k] * (f->flags & SURF_PLANEBACK ? -1 : 1);
            for (int k = 0; k < 2; ++k) {
                float coord = DotProduct(v->position, f->texinfo->vecs[k]) + f->texinfo->vecs[k][3];
                v->uv[k] = coord / (k ? texture->height : texture->width);
                v->light_uv[k] = ((coord - f->texturemins[k]) / 16 + 0.5f + (k ? light_y : light_x)) / (k ? atlas_height : atlas_width);
            }
        }
        for (int j = 1; j < f->numedges - 1; ++j) {
            s->indices[s->index_count++] = base;
            s->indices[s->index_count++] = base + j;
            s->indices[s->index_count++] = base + j + 1;
        }
    }
    /* Upload every texture frame once, including animation frames which are
     * not directly referenced by a BSP face in its initial state. */
    for (int i = 0; i < world->numtextures; ++i) {
        texture_t *texture = world->textures[i];
        if (!texture) continue;
        uint32_t material = 0;
        while (material < s->material_count && texture_sources[material] != texture) ++material;
        if (material < s->material_count) continue;
        if (!texture->width || !texture->height || texture->width > 8192 || texture->height > 8192 ||
            s->material_count >= (uint32_t)world->numtextures+1) FAIL("Invalid animation texture");
        lq_material *m = &s->materials[s->material_count++];
        texture_sources[material] = texture;
        m->indices = (lq_image){texture->width, texture->height, 1, malloc((size_t)texture->width*texture->height)};
        if (!m->indices.pixels) FAIL("Cannot allocate animated texture");
        memcpy(m->indices.pixels, (byte *)texture+texture->offsets[0], (size_t)texture->width*texture->height);
        memcpy(m->name, texture->name, 16); m->name[16] = 0;
    }
    free(texture_sources); texture_sources = NULL;
    s->atlas_height = (uint32_t)(atlas_y + row_height);
    s->world_draw_begin = (uint32_t)world->firstmodelsurface;
    s->world_draw_end = (uint32_t)(world->firstmodelsurface + world->nummodelsurfaces);
    if (!Spawn(world, s)) FAIL("Cannot retain map light inventory (allocation/4096-source limit)");
    if (!BuildBSP(world,s) || !BuildLightReach(world,s)) FAIL("Cannot build light reach");
    {
        uint32_t copies = 0, grouped = 0;
        uint32_t groups = LQ_SceneGroupSurfaceShadows(s, LQ_PANEL_SHADOW_EXTENT, LQ_PANEL_SHADOW_MEMBERS,
            SURFACE_LIGHT_SPACING / 2);
        for (uint32_t i = 0; i < s->light_source_count; ++i) {
            const lq_light_source *light = &s->light_sources[i];
            copies += light->surface_plane[0] != 0 || light->surface_plane[1] != 0 || light->surface_plane[2] != 0;
            grouped += light->shadow_group != 0;
        }
        if (groups) printf("Light inventory: %u of %u surface-light copies share %u panel shadow cubes\n",
            grouped, copies, groups);
    }
    if (!BuildSampleGrids(world,s,face_atlas)) FAIL("Cannot build light sample grids");
    free(face_atlas);
    if (!LQ_SceneCheck(s, error, size)) { LQ_SceneFree(s); return 0; }
    printf("GPU scene: %u triangles, %u draws, %u indexed materials, %dx%d lightmap atlas\n",
        s->index_count / 3, s->draw_count, s->material_count, atlas_width, atlas_height);
    uint32_t targeted = 0, styled = 0, unresolved = 0;
    for (uint32_t i = 0; i < s->light_source_count; ++i) {
        targeted += s->light_sources[i].target[0] != 0;
        styled += s->light_sources[i].style != 0;
        unresolved += s->light_sources[i].flags != 0;
    }
    printf("Light inventory: %u sources, %u targeted, %u styled, %u unresolved; falloff and shadow prototype\n",
        s->light_source_count,targeted,styled,unresolved);
    if (light_analysis) printf("LQ_LIGHT_ANALYSIS,map=%s,faces=%d,style_samples=%llu\n",
        map,world->numsurfaces,(unsigned long long)analysis_samples);
    return 1;
#undef FAIL
}
/* World faces in the camera leaf and its PVS, as the game submits them
 * before frustum culling. */
int LQ_QuakeSceneVisibleDraws(const lq_scene *s, const float camera[3], uint8_t *visible)
{
    if (!loaded_world || !s || !camera || !visible) return 0;
    memset(visible, 0, s->draw_count);
    vec3_t point = {camera[0], camera[1], camera[2]};
    mleaf_t *own = Mod_PointInLeaf(point, loaded_world);
    byte *pvs = Mod_LeafPVS(own, loaded_world);
    for (int i = 0; i <= loaded_world->numleafs; ++i) {
        mleaf_t *leaf = &loaded_world->leafs[i];
        if (leaf != own && (!i || !(pvs[(i-1)>>3] & (1<<((i-1)&7))))) continue;
        for (int j = 0; j < leaf->nummarksurfaces; ++j) {
            size_t index = (size_t)(leaf->firstmarksurface[j] - loaded_world->surfaces);
            if (index < s->draw_count) visible[index] = 1;
        }
    }
    return 1;
}
int LQ_QuakeSceneFromWorld(lq_scene *s, char *error, size_t size)
{
    char name[MAX_QPATH];
    if (!cl.worldmodel || strncmp(cl.worldmodel->name, "maps/", 5))
        return Failure(s, error, size, "No live BSP world");
    snprintf(name, sizeof(name), "%s", cl.worldmodel->name + 5);
    size_t length = strlen(name);
    if (length < 5 || strcmp(name + length - 4, ".bsp"))
        return Failure(s, error, size, "Invalid live BSP name");
    name[length - 4] = 0;
    return LQ_QuakeSceneLoad(0, NULL, name, s, error, size);
}
/* Community texture packs. */
enum { PACK_COLOR, PACK_NORMAL, PACK_GLOW, PACK_GLOSS, PACK_KINDS };
/* Each image's file as read; entries of a .pk3 are still raw (archived). */
typedef struct { lq_archive_blob files[PACK_KINDS]; uint8_t archived[PACK_KINDS]; } pack_files;
typedef struct { lq_scene *scene; pack_files *files; uint32_t found[PACK_KINDS]; } pack_job;
static uint8_t *LoadPathFile(const char *path, size_t *size)
{
    int h, length = Sys_FileOpenRead((char *)path, &h);
    if (length < 0) return NULL;
    uint8_t *bytes = malloc(length > 0 ? (size_t)length : 1);
    if (bytes && Sys_FileRead(h, bytes, length) == length) { Sys_FileClose(h); *size = (size_t)length; return bytes; }
    free(bytes); Sys_FileClose(h);
    return NULL;
}
/* The game's search path first (loose files in a -game folder or id1), then
 * the chosen pack, a folder or a .pk3 (`archive`), which adds nothing else. */
static int PackFile(const char *pack, lq_archive *archive, const char *map, const char *name, const char *suffix,
                    lq_archive_blob *file, uint8_t *archived)
{
    static const char *formats[] = {"tga", "png", "jpg"};
    char path[MAX_OSPATH], full[MAX_OSPATH];
    memset(file, 0, sizeof(*file)); *archived = 0;
    for (int source = 0; source < 2; ++source) {
        if (source == 1 && (!pack || !pack[0])) break;
        for (int folder = 0; folder < 2; ++folder)
            for (int f = 0; f < 3; ++f) {
                if (folder == 0) snprintf(path, sizeof(path), "textures/%s/%s%s.%s", map, name, suffix, formats[f]);
                else snprintf(path, sizeof(path), "textures/%s%s.%s", name, suffix, formats[f]);
                if (source == 1 && archive) {
                    if (LQ_ArchiveReadRaw(archive, path, file)) { *archived = 1; return 1; }
                    continue;
                }
                int length = 0;
                if (source == 0) file->data = COM_LoadMallocFileQuiet(path, &length), file->size = (size_t)length;
                else if (snprintf(full, sizeof(full), "%s/%s", pack, path) < (int)sizeof(full))
                    file->data = LoadPathFile(full, &file->size);
                if (file->data) { file->stored = file->size; return 1; }
            }
    }
    return 0;
}
lq_archive *LQ_QuakePackArchive(const char *pack)
{
    size_t length = pack ? strlen(pack) : 0;
    return length > 4 && !Q_strcasecmp((char *)pack + length - 4, ".pk3") ? LQ_ArchiveOpen(pack) : NULL;
}
int LQ_QuakeLoadImage(const char *pack, lq_archive *archive, const char *base, lq_image *out)
{
    static const char *formats[] = {"tga", "png", "jpg"};
    char path[MAX_OSPATH], full[MAX_OSPATH];
    memset(out, 0, sizeof(*out));
    for (int source = 0; source < 2; ++source) {
        if (source == 1 && (!pack || !pack[0])) break;
        for (int f = 0; f < 3; ++f) {
            lq_archive_blob file = {0};
            snprintf(path, sizeof(path), "%s.%s", base, formats[f]);
            if (source == 1 && archive) {
                if (!LQ_ArchiveReadRaw(archive, path, &file) || !LQ_ArchiveExpand(&file)) continue;
            } else if (source == 0) {
                int length = 0;
                file.data = COM_LoadMallocFileQuiet(path, &length); file.size = (size_t)length;
            } else if (snprintf(full, sizeof(full), "%s/%s", pack, path) < (int)sizeof(full))
                file.data = LoadPathFile(full, &file.size);
            if (!file.data) continue;
            int decoded = LQ_PackDecode(file.data, file.size, out);
            free(file.data);
            if (decoded) return 1;
        }
    }
    return 0;
}
static void PackDecode(void *opaque, size_t begin, size_t end)
{
    pack_job *job = opaque;
    for (size_t i = begin; i < end; ++i) {
        lq_material *m = &job->scene->materials[i];
        pack_files *f = &job->files[i];
        lq_image glow = {0}, gloss = {0};
        for (int k = 0; k < PACK_KINDS; ++k) if (f->archived[k]) LQ_ArchiveExpand(&f->files[k]);
        if (LQ_PackDecode(f->files[PACK_COLOR].data, f->files[PACK_COLOR].size, &m->pack_color)) {
            LQ_PackSetAlpha(&m->pack_color, 0);
            /* A glow of another size is left out (it runs on a worker: no printing). */
            if (LQ_PackDecode(f->files[PACK_GLOW].data, f->files[PACK_GLOW].size, &glow))
                LQ_PackFoldGlow(&m->pack_color, &glow);
            free(glow.pixels);
        }
        LQ_PackDecode(f->files[PACK_NORMAL].data, f->files[PACK_NORMAL].size, &m->pack_normal);
        /* The gloss rides in the normal map's blue (LQ_PackGlossIntoNormal). */
        LQ_PackDecode(f->files[PACK_GLOSS].data, f->files[PACK_GLOSS].size, &gloss);
        LQ_PackGlossIntoNormal(&m->pack_normal, &gloss);
        free(gloss.pixels);
        for (int k = 0; k < PACK_KINDS; ++k) { free(f->files[k].data); f->files[k].data = NULL; }
    }
}
uint32_t LQ_QuakeSceneLoadPack(lq_scene *s, const char *pack, lq_range_runner run, uint32_t counts[4])
{
    for (int k = 0; k < PACK_KINDS; ++k) counts[k] = 0;
    if (!s || !s->material_count || !loaded_world || !run) return 0;
    char map[MAX_QPATH];
    COM_FileBase(loaded_world->name, map);
    pack_files *files = calloc(s->material_count, sizeof(*files));
    if (!files) return 0;
    lq_archive *archive = LQ_QuakePackArchive(pack);
    uint32_t any = 0;
    for (uint32_t i = 0; i < s->material_count; ++i) {
        const lq_material *m = &s->materials[i];
        /* Sky and liquids keep their own drawing for now; solid textures only. */
        if (!m->name[0] || m->name[0] == '*' || !Q_strncasecmp((char *)m->name, "sky", 3) ||
            m->pack_color.pixels || m->pack_normal.pixels) continue;
        char name[32], lower[32];
        snprintf(name, sizeof(name), "%s", m->name);
        for (int k = 0; name[k]; ++k) lower[k] = (char)tolower((unsigned char)name[k]), lower[k + 1] = 0;
        static const char *suffixes[PACK_KINDS][2] = {{"", NULL}, {"_norm", NULL}, {"_glow", "_luma"}, {"_gloss", NULL}};
        pack_files *f = &files[i];
        for (int k = 0; k < PACK_KINDS; ++k)
            for (int j = 0; j < 2 && !f->files[k].data && suffixes[k][j]; ++j)
                if (!PackFile(pack, archive, map, name, suffixes[k][j], &f->files[k], &f->archived[k]) && strcmp(name, lower))
                    PackFile(pack, archive, map, lower, suffixes[k][j], &f->files[k], &f->archived[k]);
        /* A glow belongs to a replacement colour image (as in DarkPlaces). */
        if (!f->files[PACK_COLOR].data) { free(f->files[PACK_GLOW].data); f->files[PACK_GLOW].data = NULL; }
        /* So does a gloss: specular on the original 8-bit texture would be invented. */
        if (!f->files[PACK_COLOR].data) { free(f->files[PACK_GLOSS].data); f->files[PACK_GLOSS].data = NULL; }
    }
    LQ_ArchiveClose(archive);
    pack_job job = {s, files, {0}};
    run(s->material_count, 1, PackDecode, &job);
    /* Each pack colour's brightness against its original's: the palette's own
     * colours (colormap row 32, Quake's normal brightness) of the texture's
     * non-fullbright texels, both in linear light. */
    const uint8_t *row = s->colormap.pixels && s->colormap.width == 256 && s->colormap.height > 32 ?
        s->colormap.pixels + (size_t)32 * 256 * 4 : NULL;
    for (uint32_t i = 0; row && i < s->material_count; ++i) {
        lq_material *m = &s->materials[i];
        if (!m->pack_color.pixels) continue;
        double sum = 0; size_t count = 0, texels = (size_t)m->indices.width * m->indices.height;
        for (size_t t = 0; t < texels; ++t) {
            uint8_t index = m->indices.pixels[t];
            if (index >= 224) continue;
            const uint8_t *c = row + (size_t)index * 4;
            sum += 0.2126 * pow(c[0] / 255.0, 2.2) + 0.7152 * pow(c[1] / 255.0, 2.2) + 0.0722 * pow(c[2] / 255.0, 2.2);
            ++count;
        }
        float pack = LQ_PackLinearMean(&m->pack_color);
        m->pack_gain = count && pack > 1e-5f ? fminf(fmaxf((float)(sum / count) / pack, 0.5f), 2.0f) : 1;
    }
    /* Parallax holds each texture's mean height at the face: DarkPlaces holds
     * height 1 there, and a height map that sits mostly low (Rygel's
     * wizmet1_2 averages 0.03) then carves the whole texture to full depth,
     * sliding it across its face as the view moves. */
    for (uint32_t i = 0; i < s->material_count; ++i) {
        lq_material *m = &s->materials[i];
        if (!m->pack_normal.pixels) continue;
        size_t texels = (size_t)m->pack_normal.width * m->pack_normal.height;
        double sum = 0;
        for (size_t t = 0; t < texels; ++t) sum += m->pack_normal.pixels[t * 4 + 3];
        m->pack_height = texels ? (float)(sum / (255.0 * texels)) : 1;
    }
    for (uint32_t i = 0; i < s->material_count; ++i) {
        const lq_material *m = &s->materials[i];
        counts[PACK_COLOR] += m->pack_color.pixels != NULL;
        counts[PACK_NORMAL] += m->pack_normal.pixels != NULL;
        if (m->pack_normal.pixels) {
            size_t texels = (size_t)m->pack_normal.width * m->pack_normal.height;
            for (size_t t = 0; t < texels; ++t) if (m->pack_normal.pixels[t * 4 + 2]) { ++counts[PACK_GLOSS]; break; }
        }
        if (m->pack_color.pixels) {
            size_t texels = (size_t)m->pack_color.width * m->pack_color.height;
            for (size_t t = 0; t < texels; ++t) if (m->pack_color.pixels[t * 4 + 3]) { ++counts[PACK_GLOW]; break; }
        }
        any += m->pack_color.pixels || m->pack_normal.pixels;
    }
    free(files);
    return any;
}
