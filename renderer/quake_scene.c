/* Original GLQuake geometry/texture conventions: Copyright (C) 1996-1997 Id Software, Inc.
 * Copy original Quake assets into renderer-owned arrays. GPL-2.0-or-later.
 * The backend never receives model_t, allocator handles or QuakeC entities. */
#include "quakedef.h"
#include "quake_scene.h"
#include "user_paths.h"
#include <stdint.h>
void COM_InitFilesystem(void);
static void *legacy_heap;
#define ATLAS_SIZE 2048
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
void LQ_QuakeSceneShutdown(void) { free(legacy_heap); legacy_heap = NULL; }
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
static int Spawn(model_t *world, lq_scene *scene)
{
    char *text = world->entities;
    int found = 0; uint32_t capacity = 0, entity_index = 0, target_count = 0, target_capacity = 0;
    float range_scale = 0.5f, distance_scale = 1, angle_scale = 0.5f;
    light_target *targets = NULL;
    char (*spots)[2][128] = NULL;
    memset(switched_off, 0, sizeof(switched_off));
    scene->bounce_scale = 1;
    while (text && (text = COM_Parse(text)) && !strcmp(com_token, "{")) {
        char classname[128] = "", origin[128] = "", angles[128] = "", yaw[128] = "";
        char delay[128] = "", wait[128] = "", mangle[128] = "", soft[128] = "", incidence[128] = "";
        char world_range[128] = "", world_distance[128] = "", deviance[128] = "", bounce[128] = "";
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
            else if (!strcmp(key, "_bouncescale")) snprintf(bounce, sizeof(bounce), "%s", com_token);
            else if (!strcmp(key, "_dist")) snprintf(world_distance, sizeof(world_distance), "%s", com_token);
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
            if (bounce[0] && Number(bounce,&value) && value >= 0 && value <= 16) scene->bounce_scale = value;
        }
        if (light.targetname[0] && strncmp(classname,"light",5)) {
            if (target_count == target_capacity) {
                target_capacity = target_capacity ? target_capacity*2 : 32;
                light_target *next = target_capacity <= 65536 ? realloc(targets,target_capacity*sizeof(*targets)) : NULL;
                if (!next) { free(targets); free(spots); return 0; }
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
                Con_Printf("Light inventory: skipping invalid source entity %u.\n",light.entity_index);
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
                light.angle_scale = angle_scale; light.output_scale = range_scale;
                light.attenuation = distance_scale;
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
                    if (capacity == 4096) { free(targets); free(spots); return 0; }
                    capacity = capacity ? capacity*2 : 32;
                    lq_light_source *sources = realloc(scene->light_sources,capacity*sizeof(*sources));
                    char (*next)[2][128] = realloc(spots,capacity*sizeof(*spots));
                    if (next) spots = next;
                    if (!sources || !next) { if (sources) scene->light_sources = sources; free(targets); free(spots); return 0; }
                    scene->light_sources = sources;
                }
                snprintf(spots[scene->light_source_count][0],128,"%s",mangle);
                snprintf(spots[scene->light_source_count][1],128,"%s",soft);
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
        if (!light->flags) LightSpot(light,spots[i][0],spots[i][1],targets,target_count);
    }
    free(targets); free(spots);
    return 1;
}
/* Renderer-owned world BSP: nodes with their on-plane faces and leaf contents,
 * for point lookup and Enhanced ray casts. */
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
/* LIGHT's per-face sample lattice for Enhanced bounce, one unit in front of
 * the face. Points that start inside solid move toward the face center, as
 * LIGHT does before tracing; any that cannot are NaN (no sample). */
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
    s->lightmap = (lq_image){ATLAS_SIZE, ATLAS_SIZE, 4, calloc(ATLAS_SIZE * ATLAS_SIZE, 4)};
    if (!s->colormap.pixels || !s->lightmap.pixels) return Failure(s, error, size, "Cannot allocate lighting images");
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
            if (light_width < 1 || light_height < 1 || light_width + 2 > ATLAS_SIZE || light_height + 2 > ATLAS_SIZE)
                FAIL("Invalid surface lightmap size");
            if (atlas_x + light_width + 1 > ATLAS_SIZE) { atlas_x = 1; atlas_y += row_height; row_height = 0; }
            if (atlas_y + light_height + 1 > ATLAS_SIZE) FAIL("Lightmap atlas full; multiple pages are not implemented yet");
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
                uint8_t *pixel = s->lightmap.pixels + ((light_y + y) * ATLAS_SIZE + light_x + x) * 4;
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
                v->light_uv[k] = ((coord - f->texturemins[k]) / 16 + 0.5f + (k ? light_y : light_x)) / ATLAS_SIZE;
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
    if (!Spawn(world, s)) FAIL("Cannot retain map light inventory (allocation/4096-source limit)");
    if (!BuildBSP(world,s) || !BuildLightReach(world,s)) FAIL("Cannot build light reach");
    if (!BuildSampleGrids(world,s,face_atlas)) FAIL("Cannot build light sample grids");
    free(face_atlas);
    if (!LQ_SceneCheck(s, error, size)) { LQ_SceneFree(s); return 0; }
    printf("GPU scene: %u triangles, %u draws, %u indexed materials, %dx%d lightmap atlas\n",
        s->index_count / 3, s->draw_count, s->material_count, ATLAS_SIZE, ATLAS_SIZE);
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
