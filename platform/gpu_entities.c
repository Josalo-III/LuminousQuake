/* GLQuake MDL/sprite/particle conventions, Copyright (C) 1996-1997 Id Software.
 * CPU animation/transforms produce owned GPU submissions, no CPU rasterization.
 * GPL-2.0-or-later. */
#include "quakedef.h"
#include "gpu_entities.h"
#include "effects.h"
#include "render_bridge.h"
#include "pack_models.h"
extern float r_avertexnormals[162][3];
extern cvar_t r_drawentities, r_drawviewmodel;
extern entity_t *currententity;
mspriteframe_t *R_GetSpriteframe(msprite_t *sprite);
texture_t *R_TextureAnimation(texture_t *texture);
int R_LightPoint(vec3_t point);
static lq_vertex *vertices;
static uint32_t *indices;
static lq_draw *draws;
static uint32_t vertex_capacity, draw_capacity;
static lq_frame frame;
static lq_material materials[4095];
static struct { const void *model; uint32_t offset; int translated; byte colors[256]; } keys[4095];
static uint32_t owned_start, particle_material;
static int luminous_frame, solid_particles;
/* Read once a frame: r_lerpmodels, r_lerpmove, and in Luminous the pack's
 * replacement models (lq_pack_models) and skins (lq_pack_skins). */
static int lerp_models, lerp_move, pack_models, pack_skins;
static uint8_t particle_pixels[256];
/* Frame blending and step easing, after FitzQuake (r_lerpmodels, r_lerpmove).
 * QuakeC animates and moves monsters in 0.1 s thinks. A model blends from its
 * previous pose to the current one over the pose's interval, and an entity the
 * server marks U_NOLERP (MOVETYPE_STEP) eases from where it was shown to its
 * new origin and angles over 0.1 s, drawing it up to one step behind. Fire
 * keeps its authored poses; only its light steps between them (see FireLevel). */
#define LERP_STEP 0.1
#define LERP_TELEPORT 100.0f /* a larger step is a teleport and snaps */
#define LERP_STALE 0.5 /* an entity unseen this long starts afresh */
typedef struct {
    const model_t *model;
    const void *mesh; /* the alias header or pack model the poses belong to */
    int pose, previous_pose; /* offsets of resolved poses in the model's data, or pack model frames */
    double pose_start, move_start, seen;
    float pose_interval;
    vec3_t origin_from, origin_to, angles_from, angles_to;
    qboolean moving;
} lerp_state;
static lerp_state lerp_states[MAX_EDICTS + MAX_STATIC_ENTITIES + 1];
static void Reserve(uint32_t count)
{
    uint32_t need = frame.vertex_count+count;
    if (need > 2000000) Sys_Error("GPU actor geometry limit exceeded");
    if (need > vertex_capacity) {
        uint32_t capacity = vertex_capacity ? vertex_capacity : 4096;
        while (capacity < need) capacity *= 2;
        lq_vertex *v = realloc(vertices, capacity*sizeof(*v));
        uint32_t *i = realloc(indices, capacity*sizeof(*i));
        if (!v || !i) Sys_Error("Cannot allocate GPU actor geometry");
        vertices = v; indices = i; vertex_capacity = capacity;
    }
    if (frame.draw_count+1 > draw_capacity) {
        uint32_t capacity = draw_capacity ? draw_capacity*2 : 256;
        lq_draw *d = realloc(draws, capacity*sizeof(*d));
        if (!d) Sys_Error("Cannot allocate GPU actor draws");
        draws = d; draw_capacity = capacity;
    }
}
static lq_draw *Draw(uint32_t count, uint32_t material, uint32_t flags)
{
    Reserve(count);
    lq_draw *d = &draws[frame.draw_count++];
    *d = (lq_draw){frame.vertex_count, count, material, flags, {255,255,255,255}};
    for (uint32_t i = 0; i < count; ++i) indices[frame.vertex_count+i] = frame.vertex_count+i;
    return d;
}
/* Entity alpha (protocols 666/999): opacity 0-1, 1 for the default. */
static float Opacity(const entity_t *e) { return ENTALPHA_DECODE(e->alpha); }
/* A translucent entity's draws blend over the frame by its opacity, after
 * everything solid, and cast no shadow. */
static void Translucent(lq_draw *d, float opacity)
{
    if (opacity < 1) { d->flags |= LQ_SURFACE_TRANSLUCENT; d->alpha = opacity; }
}
static uint32_t Material(const void *model, uint32_t offset, const byte *pixels,
                         unsigned width, unsigned height, const byte *translation)
{
    for (uint32_t i = owned_start; i < frame.material_count; ++i)
        if (keys[i].model == model && keys[i].offset == offset && keys[i].translated == (translation != NULL) &&
            (!translation || !memcmp(keys[i].colors, translation, 256))) return i;
    if (frame.material_count >= 4095 || !width || !height || width > 8192 || height > 8192)
        Sys_Error("GPU actor material limit exceeded");
    uint32_t i = frame.material_count++;
    byte *copy = malloc((size_t)width*height);
    if (!copy) Sys_Error("Cannot allocate GPU skin copy");
    for (size_t j = 0; j < (size_t)width*height; ++j) copy[j] = translation ? translation[pixels[j]] : pixels[j];
    materials[i] = (lq_material){.indices={width,height,1,copy}, .resource_id=i+1};
    keys[i].model = model; keys[i].offset = offset; keys[i].translated = translation != NULL;
    if (translation) memcpy(keys[i].colors, translation, 256);
    return i;
}
void LQ_EntitiesReset(void)
{
    for (uint32_t i = owned_start; i < frame.material_count; ++i) free(materials[i].indices.pixels);
    free(vertices); free(indices); free(draws);
    vertices = NULL; indices = NULL; draws = NULL;
    vertex_capacity = draw_capacity = owned_start = 0;
    memset(&frame, 0, sizeof(frame)); memset(keys, 0, sizeof(keys)); memset(lerp_states, 0, sizeof(lerp_states));
}
void LQ_EntitiesBegin(const lq_scene *world)
{
    frame.vertex_count = frame.index_count = frame.draw_count = 0;
    luminous_frame = Cvar_VariableValue("lq_renderer") == 2;
    solid_particles = luminous_frame && Cvar_VariableValue("lq_particles") != 0;
    lerp_models = Cvar_VariableValue("r_lerpmodels") != 0; lerp_move = Cvar_VariableValue("r_lerpmove") != 0;
    pack_models = luminous_frame && Cvar_VariableValue("lq_pack_models") != 0;
    pack_skins = luminous_frame && Cvar_VariableValue("lq_pack_skins") != 0;
    if (!frame.material_count) {
        if (world->material_count >= 4094) Sys_Error("Too many world materials");
        for (uint32_t i = 0; i < world->material_count; ++i) {
            materials[i] = world->materials[i]; materials[i].resource_id = i+1;
            /* World pack images reach the GPU with the scene, not per frame. */
            memset(&materials[i].pack_color, 0, sizeof(materials[i].pack_color));
            memset(&materials[i].pack_normal, 0, sizeof(materials[i].pack_normal));
        }
        frame.material_count = owned_start = world->material_count;
        for (int i = 0; i < 256; ++i) particle_pixels[i] = i;
        particle_material = Material(particle_pixels, 0, particle_pixels, 256, 1, NULL);
    }
}
const lq_frame *LQ_EntitiesFrame(void)
{
    frame.vertices = vertices; frame.indices = indices; frame.draws = draws; frame.materials = materials;
    frame.index_count = frame.vertex_count; return &frame;
}
/* The member of a timed group shown at `time`, and how far through its
 * interval that is (0 to 1). */
static int GroupPhase(const float *intervals, int count, double time, float *fraction)
{
    if (count < 1 || !isfinite(intervals[count-1]) || intervals[count-1] <= 0)
        Sys_Error("Invalid GPU animation interval");
    float target = fmod(fmax(time, 0), intervals[count-1]);
    int i;
    for (i = 0; i < count-1 && target >= intervals[i]; ++i) {}
    float start = i ? intervals[i-1] : 0, length = intervals[i] - start;
    *fraction = length > 0 ? fminf(fmaxf((target - start) / length, 0), 1) : 0;
    return i;
}
static int GroupFrame(const float *intervals, int count, double time)
{ float fraction; return GroupPhase(intervals, count, time, &fraction); }
/* Fire (flames, braziers, torches) changes shape between poses rather than
 * moving, so its mesh is drawn at the authored poses only. With r_lerpmodels
 * its light's strength steps at 20 Hz: each pose's value for the first half of
 * the pose, then an in-between value for the second. The in-between is biased
 * toward the loop's ends: a third of the way to the next pose's value through
 * the first half of the loop, two thirds through the second. A six-pose loop
 * runs 1, 1.33, 2, 2.33, 3, 3.33, 4, 4.67, 5, 5.67, 6, 6.67. */
static float FireLevel(int member, int count, float fraction)
{
    if (Cvar_VariableValue("r_lerpmodels") == 0 || fraction < 0.5f || count < 2) return 0;
    return member < count * 0.5f ? 1.0f / 3 : 2.0f / 3;
}
static void Basis(entity_t *e, vec3_t forward, vec3_t right, vec3_t up)
{
    vec3_t angles; VectorCopy(e->angles, angles); angles[PITCH] = -angles[PITCH];
    AngleVectors(angles, forward, right, up);
}
/* Luminous: a torch or flame's light follows its animation. A pose's emission is
 * its surface area times the luminance of the skin it shows. The light's
 * strength follows each pose's emission relative to the cycle's mean, and its
 * position follows the shift of the emission centre. Quake starts every static
 * flame's cycle together, so Luminous gives each flame, model and light alike,
 * its own phase derived from its position. */
#define FLAME_POSES 32
typedef struct {
    const model_t *model;
    int pose, count;
    float gain[FLAME_POSES], offset[FLAME_POSES][3]; /* offset in model axes */
} flame_curve;
static flame_curve flame_curves[8];
static int flame_curve_count;
static const model_t *flame_world;
static int IsFlame(const model_t *model)
{ return model && (!strcmp(model->name, "progs/flame.mdl") || !strcmp(model->name, "progs/flame2.mdl")); }
static double FlameTime(const entity_t *e)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; ++i) { h ^= (uint32_t)(int32_t)floorf(e->origin[i] + 0.5f); h *= 16777619u; }
    return cl.time + (h >> 8) / (double)(1u << 24) * 8.0;
}
/* Pose-group index and the poses of e's current frame, and how far through
 * that pose's interval the flame is. */
static int FlamePoses(entity_t *e, aliashdr_t *header, trivertx_t *poses[FLAME_POSES], int *count, float *fraction)
{
    mdl_t *model = (mdl_t *)((byte *)header+header->model);
    int pose = e->frame >= 0 && e->frame < model->numframes ? e->frame : 0;
    *fraction = 0;
    if (header->frames[pose].type != ALIAS_GROUP) {
        *count = 1; poses[0] = (trivertx_t *)((byte *)header+header->frames[pose].frame);
        return 0;
    }
    maliasgroup_t *g = (maliasgroup_t *)((byte *)header+header->frames[pose].frame);
    *count = g->numframes < FLAME_POSES ? g->numframes : FLAME_POSES;
    for (int i = 0; i < *count; ++i) poses[i] = (trivertx_t *)((byte *)header+g->frames[i].frame);
    return GroupPhase((float *)((byte *)header+g->intervals), g->numframes, FlameTime(e), fraction) % *count;
}
static const flame_curve *FlameCurve(entity_t *e, aliashdr_t *header, trivertx_t *poses[FLAME_POSES], int count)
{
    mdl_t *model = (mdl_t *)((byte *)header+header->model);
    int pose = e->frame >= 0 && e->frame < model->numframes ? e->frame : 0;
    if (flame_world != cl.worldmodel) { flame_curve_count = 0; flame_world = cl.worldmodel; }
    for (int i = 0; i < flame_curve_count; ++i)
        if (flame_curves[i].model == e->model && flame_curves[i].pose == pose) return &flame_curves[i];
    if (flame_curve_count == 8 || !model->numskins) return NULL;
    maliasskindesc_t *skin = (maliasskindesc_t *)((byte *)header+header->skindesc);
    if (skin->type == ALIAS_SKIN_GROUP)
        skin = ((maliasskingroup_t *)((byte *)header+skin->skin))->skindescs;
    const byte *pixels = (byte *)header+skin->skin;
    stvert_t *uv = (stvert_t *)((byte *)header+header->stverts);
    mtriangle_t *triangles = (mtriangle_t *)((byte *)header+header->triangles);
    float emission[FLAME_POSES], centre[FLAME_POSES][3], mean = 0, mean_centre[3] = {0};
    for (int f = 0; f < count; ++f) {
        double total = 0, moment[3] = {0};
        for (int i = 0; i < model->numtris; ++i) {
            float p[3][3], s[3], t[3];
            for (int j = 0; j < 3; ++j) {
                int v = triangles[i].vertindex[j];
                for (int k = 0; k < 3; ++k) p[j][k] = poses[f][v].v[k]*model->scale[k]+model->scale_origin[k];
                s[j] = uv[v].s / 65536.0f + (!triangles[i].facesfront && uv[v].onseam ? model->skinwidth*0.5f : 0);
                t[j] = uv[v].t / 65536.0f;
            }
            vec3_t a, b, n; VectorSubtract(p[1], p[0], a); VectorSubtract(p[2], p[0], b);
            CrossProduct(a, b, n);
            double area = 0.5*Length(n), luminance = 0, local[3] = {0};
            /* Fifteen barycentric samples of the skin across the triangle. */
            for (int u = 0; u <= 4; ++u) for (int w = 0; w <= 4-u; ++w) {
                float wa = u/4.0f, wb = w/4.0f, wc = 1-wa-wb;
                int x = (int)(wa*s[0]+wb*s[1]+wc*s[2]), y = (int)(wa*t[0]+wb*t[1]+wc*t[2]);
                x = x < 0 ? 0 : x >= model->skinwidth ? model->skinwidth-1 : x;
                y = y < 0 ? 0 : y >= model->skinheight ? model->skinheight-1 : y;
                const byte *c = host_basepal+3*pixels[y*model->skinwidth+x];
                double l = (0.2126*c[0]+0.7152*c[1]+0.0722*c[2])/255.0;
                luminance += l;
                for (int k = 0; k < 3; ++k) local[k] += l*(wa*p[0][k]+wb*p[1][k]+wc*p[2][k]);
            }
            total += area*luminance/15;
            for (int k = 0; k < 3; ++k) moment[k] += area*local[k]/15;
        }
        emission[f] = (float)total; mean += emission[f]/count;
        for (int k = 0; k < 3; ++k) {
            centre[f][k] = total > 0 ? (float)(moment[k]/total) : 0;
            mean_centre[k] += centre[f][k]/count;
        }
    }
    if (mean <= 0) return NULL;
    flame_curve *curve = &flame_curves[flame_curve_count++];
    curve->model = e->model; curve->pose = pose; curve->count = count;
    for (int f = 0; f < count; ++f) {
        curve->gain[f] = emission[f]/mean;
        for (int k = 0; k < 3; ++k) curve->offset[f][k] = centre[f][k]-mean_centre[k];
        Con_DPrintf("Flame light %s frame %d pose %d: strength %.2f, centre shift %.2f %.2f %.2f\n",
            e->model->name, pose, f, curve->gain[f], curve->offset[f][0], curve->offset[f][1], curve->offset[f][2]);
    }
    return curve;
}
int LQ_FlameLight(const float origin[3], float *gain, float offset[3])
{
    for (int i = 0; i < cl.num_statics; ++i) {
        entity_t *e = &cl_static_entities[i];
        if (!IsFlame(e->model) || fabsf(e->origin[0]-origin[0]) > 1 ||
            fabsf(e->origin[1]-origin[1]) > 1 || fabsf(e->origin[2]-origin[2]) > 1) continue;
        aliashdr_t *header = Mod_Extradata(e->model);
        trivertx_t *poses[FLAME_POSES]; int count; float fraction;
        int f = FlamePoses(e, header, poses, &count, &fraction);
        const flame_curve *curve = FlameCurve(e, header, poses, count);
        if (!curve) return 0;
        vec3_t forward, right, up; Basis(e, forward, right, up);
        const float *o = curve->offset[f];
        for (int k = 0; k < 3; ++k) offset[k] = forward[k]*o[0]-right[k]*o[1]+up[k]*o[2];
        /* Strength steps with the flame's shape; the position stays at the
         * authored pose, so the shadow is redrawn only at 10 Hz. */
        float level = FireLevel(f, count, fraction);
        *gain = curve->gain[f] + (curve->gain[(f + 1) % count] - curve->gain[f]) * level;
        return 1;
    }
    return 0;
}
static lerp_state *LerpState(const entity_t *e)
{
    if (e >= cl_entities && e < cl_entities + MAX_EDICTS) return &lerp_states[e - cl_entities];
    if (e >= cl_static_entities && e < cl_static_entities + MAX_STATIC_ENTITIES)
        return &lerp_states[MAX_EDICTS + (e - cl_static_entities)];
    if (e == &cl.viewent) return &lerp_states[MAX_EDICTS + MAX_STATIC_ENTITIES];
    return NULL; /* temporary entities (beams) are rebuilt every frame */
}
/* FitzQuake's r_nolerp_list: fire and two view models whose loops are
 * drawn stepped. */
enum { LERP_BLEND, LERP_STEPPED, LERP_FIRE };
static int LerpKind(const model_t *model)
{
    static const char *const fire[] = {"progs/flame.mdl", "progs/flame2.mdl", "progs/braztall.mdl",
        "progs/brazshrt.mdl", "progs/longtrch.mdl", "progs/flame_pyre.mdl", "progs/h2stuff/newfire.mdl"};
    for (size_t i = 0; i < sizeof(fire) / sizeof(fire[0]); ++i) if (!strcmp(model->name, fire[i])) return LERP_FIRE;
    if (!strcmp(model->name, "progs/v_saw.mdl") || !strcmp(model->name, "progs/v_xfist.mdl")) return LERP_STEPPED;
    return LERP_BLEND;
}
static float AngleLerp(float from, float to, float f)
{
    float delta = fmodf(to - from, 360.0f);
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    return from + delta * f;
}
/* Where a step-moving entity is shown now. Its target is the server's latest
 * placement: after a U_NOLERP update the original client snaps there and, on
 * frames before the next message, interpolates from the message before, which
 * steps it back. A new target starts a step from the previous one (not from
 * where it is shown), as FitzQuake does, so a leap updated every frame runs
 * at most a frame behind instead of trailing. */
static void EasedPlacement(lerp_state *state, entity_t *e, vec3_t origin, vec3_t angles)
{
    float *target = e->msg_origins[0], *facing = e->msg_angles[0];
    if (!VectorCompare(target, state->origin_to) || !VectorCompare(facing, state->angles_to)) {
        vec3_t step; VectorSubtract(target, state->origin_to, step);
        if (Length(step) > LERP_TELEPORT) { VectorCopy(target, state->origin_from); VectorCopy(facing, state->angles_from); }
        else { VectorCopy(state->origin_to, state->origin_from); VectorCopy(state->angles_to, state->angles_from); }
        VectorCopy(target, state->origin_to); VectorCopy(facing, state->angles_to);
        state->move_start = cl.time; state->moving = true;
    }
    float f = state->moving ? (float)fmin(fmax((cl.time - state->move_start) / LERP_STEP, 0), 1) : 1;
    for (int k = 0; k < 3; ++k) {
        origin[k] = state->origin_from[k] + (state->origin_to[k] - state->origin_from[k]) * f;
        angles[k] = AngleLerp(state->angles_from[k], state->angles_to[k], f);
    }
}
static void Transform(const float local[3], entity_t *e, const float forward[3],
                      const float right[3], const float up[3], float output[3])
{
    for (int i = 0; i < 3; ++i) output[i] = e->origin[i]+forward[i]*local[0]-right[i]*local[1]+up[i]*local[2];
}
/* Blend toward `pose` (a key into `mesh`: an alias header's pose offset or a
 * pack model's frame) over `interval`, and ease a step-mover toward its new
 * place. Returns the blend weight; *previous is the pose blended from and
 * *shown the entity where it is drawn. */
static float Animate(entity_t *e, const void *mesh, int pose, float interval, int *previous, entity_t *shown)
{
    lerp_state *state = LerpState(e);
    if (state && (state->model != e->model || state->mesh != mesh ||
            cl.time - state->seen > LERP_STALE || cl.time < state->seen)) {
        memset(state, 0, sizeof(*state));
        state->model = e->model; state->mesh = mesh; state->pose = state->previous_pose = pose; state->pose_start = cl.time;
        VectorCopy(e->origin, state->origin_to); VectorCopy(e->angles, state->angles_to);
    }
    if (state) state->seen = cl.time;
    float blend = 1;
    *previous = pose;
    if (state && lerp_models && LerpKind(e->model) == LERP_BLEND) {
        if (pose != state->pose) {
            state->previous_pose = state->pose; state->pose = pose;
            state->pose_start = cl.time; state->pose_interval = fmaxf(interval, 0.01f);
        }
        *previous = state->previous_pose;
        if (*previous != pose) blend = (float)fmin(fmax((cl.time - state->pose_start) / state->pose_interval, 0), 1);
    }
    *shown = *e;
    if (state && e->movestep && lerp_move) EasedPlacement(state, e, shown->origin, shown->angles);
    else if (state) {
        VectorCopy(e->origin, state->origin_to); VectorCopy(e->angles, state->angles_to);
        state->moving = false;
    }
    return blend;
}
/* A single pose lasts until the server's next think when it said when that
 * is (U_LERPFINISH), as FitzQuake reads it; otherwise 0.1 s. */
static float PoseInterval(const entity_t *e)
{ return e->lerpfinish > 0 ? (float)fmax(e->lerpfinish - cl.time, 0.01) : LERP_STEP; }
static int IsEmissive(const model_t *model)
{
    return IsFlame(model) || !strcmp(model->name, "progs/bolt.mdl") ||
        !strcmp(model->name, "progs/bolt2.mdl") || !strcmp(model->name, "progs/bolt3.mdl") ||
        !strcmp(model->name, "progs/laser.mdl") || !strcmp(model->name, "progs/lavaball.mdl");
}
/* Luminous lights actors from runtime lights and probes. Flames and energy
 * effects stay self-lit; a rocket cannot cast around its own light origin.
 * Classic ignores these flags and keeps its original vertex shade. */
static uint32_t ModelFlags(const entity_t *e, int weapon)
{
    int emissive = IsEmissive(e->model);
    return LQ_SURFACE_MODEL | (weapon ? LQ_SURFACE_WEAPON : 0) | (emissive ? 0 : LQ_SURFACE_ACTOR) |
        (emissive || (e->model->flags & EF_ROCKET) ? LQ_SURFACE_NOSHADOW : 0);
}
/* Protocol 999 entity scale, about the entity's origin. */
static float EntityScale(const entity_t *e)
{ return e->scale && e->scale != ENTSCALE_DEFAULT ? e->scale / (float)ENTSCALE_DEFAULT : 1; }
/* A community pack's material: its images are the pack store's; only the
 * palette image (`pixels`, the original skin or a placeholder) is copied. */
static uint32_t PackMaterial(const lq_material *source, const byte *pixels, unsigned width, unsigned height)
{
    for (uint32_t i = owned_start; i < frame.material_count; ++i)
        if (keys[i].model == source && keys[i].offset == UINT32_MAX) return i;
    uint32_t i = Material(source, UINT32_MAX, pixels, width, height, NULL);
    materials[i].pack_color = source->pack_color; materials[i].pack_normal = source->pack_normal;
    materials[i].pack_gain = source->pack_gain;
    return i;
}
/* Luminous: a pack's replacement mesh (MD3 or MDL), drawn in place of the
 * original model, which the game keeps for everything else. */
static void PackMesh(entity_t *e, const lq_pack_model *m, uint32_t flags)
{
    int pose = e->frame >= 0 && (uint32_t)e->frame < m->frame_count ? e->frame : 0, previous;
    entity_t shown;
    float blend = Animate(e, m, pose, PoseInterval(e), &previous, &shown);
    if ((uint32_t)previous >= m->frame_count) previous = pose;
    const float (*to)[3] = &m->positions[(size_t)pose * m->vertex_count];
    const float (*from)[3] = &m->positions[(size_t)previous * m->vertex_count];
    const float (*n1)[3] = &m->normals[(size_t)pose * m->vertex_count];
    const float (*n0)[3] = &m->normals[(size_t)previous * m->vertex_count];
    vec3_t forward, right, up; Basis(&shown, forward, right, up);
    float size = EntityScale(e);
    uint32_t skin = e->skinnum >= 0 && (uint32_t)e->skinnum < m->skin_count ? (uint32_t)e->skinnum : 0;
    for (uint32_t s = 0; s < m->surface_count; ++s) {
        const lq_pack_surface *surface = &m->surfaces[s];
        const lq_material *source = &surface->skins[skin];
        if (!source->pack_color.pixels) source = &surface->skins[0];
        if (!source->pack_color.pixels || !surface->corner_count) continue;
        Translucent(Draw(surface->corner_count, PackMaterial(source, source->indices.pixels, 1, 1), flags), Opacity(e));
        for (uint32_t c = 0; c < surface->corner_count; ++c) {
            uint32_t p = m->corners[surface->first_corner + c];
            lq_vertex *v = &vertices[frame.vertex_count++]; memset(v, 0, sizeof(*v));
            vec3_t local, n;
            for (int k = 0; k < 3; ++k) {
                local[k] = (from[p][k] + (to[p][k] - from[p][k]) * blend) * size;
                n[k] = n0[p][k] + (n1[p][k] - n0[p][k]) * blend;
            }
            if (VectorNormalize(n) < 0.001f) VectorCopy(n1[p], n);
            Transform(local, &shown, forward, right, up, v->position);
            for (int k = 0; k < 3; ++k) v->normal[k] = n[0]*forward[k]-n[1]*right[k]+n[2]*up[k];
            v->uv[0] = m->uv[p][0]; v->uv[1] = m->uv[p][1];
        }
    }
}
static void Alias(entity_t *e, int weapon)
{
    const lq_pack_model *pack = pack_models || pack_skins ? LQ_PackModel(e->model) : NULL;
    if (pack && pack->replaces && pack_models) { PackMesh(e, pack, ModelFlags(e, weapon)); return; }
    aliashdr_t *header = Mod_Extradata(e->model);
    mdl_t *model = (mdl_t *)((byte *)header+header->model);
    int skin = e->skinnum >= 0 && e->skinnum < model->numskins ? e->skinnum : 0;
    maliasskindesc_t *skins = (maliasskindesc_t *)((byte *)header+header->skindesc);
    maliasskindesc_t *selected = &skins[skin];
    if (selected->type == ALIAS_SKIN_GROUP) {
        maliasskingroup_t *g = (maliasskingroup_t *)((byte *)header+selected->skin);
        selected = &g->skindescs[GroupFrame((float *)((byte *)header+g->intervals), g->numskins, cl.time+e->syncbase)];
    }
    const byte *translation = e->colormap && e->colormap != host_colormap ? e->colormap : NULL;
    /* Luminous: the pack's skin for a model it does not replace (player
     * colours keep the original skin). */
    const lq_material *pack_skin = pack && !pack->replaces && pack_skins && !translation &&
        (uint32_t)skin < pack->skin_count && pack->surfaces[0].skins[skin].pack_color.pixels ?
        &pack->surfaces[0].skins[skin] : NULL;
    uint32_t material = pack_skin ?
        PackMaterial(pack_skin, (byte *)header+selected->skin, model->skinwidth, model->skinheight) :
        Material(e->model, selected->skin, (byte *)header+selected->skin, model->skinwidth, model->skinheight, translation);
    int pose = e->frame >= 0 && e->frame < model->numframes ? e->frame : 0;
    int offset = header->frames[pose].frame;
    float interval = PoseInterval(e);
    if (header->frames[pose].type == ALIAS_GROUP) {
        maliasgroup_t *g = (maliasgroup_t *)((byte *)header+offset);
        const float *intervals = (float *)((byte *)header+g->intervals);
        double time = luminous_frame && IsFlame(e->model) ? FlameTime(e) : cl.time+e->syncbase;
        int member = GroupFrame(intervals, g->numframes, time);
        offset = g->frames[member].frame;
        interval = member ? intervals[member] - intervals[member-1] : intervals[0];
    }
    int previous;
    entity_t shown;
    float blend = Animate(e, header, offset, interval, &previous, &shown);
    trivertx_t *points = (trivertx_t *)((byte *)header+offset);
    trivertx_t *from = (trivertx_t *)((byte *)header+previous);
    stvert_t *uv = (stvert_t *)((byte *)header+header->stverts);
    mtriangle_t *triangles = (mtriangle_t *)((byte *)header+header->triangles);
    vec3_t forward, right, up; Basis(&shown, forward, right, up);
    float ambient = R_LightPoint(shown.origin), shade = ambient;
    if (weapon && ambient < 24) ambient = shade = 24;
    for (int i = 0; i < MAX_DLIGHTS; ++i) {
        dlight_t *light = &cl_dlights[i];
        if (light->die < cl.time || !light->radius) continue;
        vec3_t distance; VectorSubtract(shown.origin, light->origin, distance);
        ambient += fmaxf(light->radius-Length(distance), 0);
    }
    ambient = fminf(ambient, 128); shade = fminf(shade, 192-ambient);
    for (int i = 1; i <= cl.maxclients; ++i)
        if (e == &cl_entities[i] && ambient < 8) { ambient = shade = 8; break; }
    if (IsFlame(e->model) || (luminous_frame && IsEmissive(e->model))) ambient = 256, shade = 0;
    Translucent(Draw(model->numtris*3, material, ModelFlags(e, weapon)), Opacity(e));
    float size = EntityScale(e);
    for (int i = 0; i < model->numtris; ++i) for (int j = 0; j < 3; ++j) {
        int p = triangles[i].vertindex[j];
        if (p < 0 || p >= model->numverts || points[p].lightnormalindex >= 162 || from[p].lightnormalindex >= 162)
            Sys_Error("Invalid GPU alias vertex");
        lq_vertex *v = &vertices[frame.vertex_count++]; memset(v, 0, sizeof(*v));
        vec3_t local, normal, n;
        const float *n0 = r_avertexnormals[from[p].lightnormalindex], *n1 = r_avertexnormals[points[p].lightnormalindex];
        for (int k = 0; k < 3; ++k) {
            local[k] = ((from[p].v[k] + (points[p].v[k] - from[p].v[k]) * blend) * model->scale[k] + model->scale_origin[k]) * size;
            n[k] = n0[k] + (n1[k] - n0[k]) * blend;
        }
        if (VectorNormalize(n) < 0.001f) VectorCopy(n1, n);
        Transform(local, &shown, forward, right, up, v->position);
        for (int k = 0; k < 3; ++k) normal[k] = n[0]*forward[k]-n[1]*right[k]+n[2]*up[k];
        VectorCopy(normal,v->normal);
        float brightness = ambient+shade*fmaxf(normal[0], 0);
        v->shade = fminf(fmaxf((255-brightness)*0.25f, 0), 63);
        float s = uv[p].s / 65536.0f;
        if (!triangles[i].facesfront && uv[p].onseam) s += model->skinwidth*0.5f;
        v->uv[0] = (s+0.5f)/model->skinwidth; v->uv[1] = (uv[p].t/65536.0f+0.5f)/model->skinheight;
    }
}
static void Sprite(entity_t *e)
{
    currententity = e;
    msprite_t *sprite = Mod_Extradata(e->model);
    mspriteframe_t *s = R_GetSpriteframe(sprite);
    uint32_t material = Material(s, 0, s->pixels, s->width, s->height, NULL);
    vec3_t right, up, forward;
    VectorCopy(vright, right); VectorCopy(vup, up);
    if (sprite->type == SPR_ORIENTED) AngleVectors(e->angles, forward, right, up);
    else if (sprite->type == SPR_FACING_UPRIGHT || sprite->type == SPR_VP_PARALLEL_UPRIGHT) {
        up[0] = up[1] = 0; up[2] = 1;
        if (sprite->type == SPR_FACING_UPRIGHT) {
            right[0] = e->origin[1]-r_origin[1]; right[1] = -(e->origin[0]-r_origin[0]);
        } else { right[0] = vpn[1]; right[1] = -vpn[0]; }
        right[2] = 0; if (VectorNormalize(right) < 0.001f) return;
    } else if (sprite->type == SPR_VP_PARALLEL_ORIENTED) {
        float a = e->angles[ROLL]*(float)M_PI/180, c = cosf(a), t = sinf(a);
        for (int i = 0; i < 3; ++i) { right[i] = vright[i]*c+vup[i]*t; up[i] = vup[i]*c-vright[i]*t; }
    }
    const int corners[] = {0,1,2,0,2,3};
    const float coords[4][2] = {{s->left,s->up},{s->right,s->up},{s->right,s->down},{s->left,s->down}};
    int emissive = !strcmp(e->model->name, "progs/s_explod.spr") || !strcmp(e->model->name, "progs/s_light.spr");
    Translucent(Draw(6, material, LQ_SURFACE_MODEL|LQ_SURFACE_MASKED|LQ_SURFACE_SPRITE|
        (emissive ? 0 : LQ_SURFACE_ACTOR|LQ_SURFACE_DIFFUSE_EFFECT)), Opacity(e));
    for (int i = 0; i < 6; ++i) {
        int corner = corners[i]; lq_vertex *v = &vertices[frame.vertex_count++]; memset(v,0,sizeof(*v));
        for (int j = 0; j < 3; ++j) v->position[j] = e->origin[j]+right[j]*coords[corner][0]+up[j]*coords[corner][1];
        for (int j = 0; j < 3; ++j) v->normal[j] = -vpn[j];
        v->uv[0] = corner == 1 || corner == 2; v->uv[1] = corner >= 2;
    }
}
uint32_t LQ_AnimatedMaterial(const lq_scene *world, texture_t *texture)
{
    texture = R_TextureAnimation(texture);
    for (uint32_t i = 0; i < world->material_count; ++i)
        if (!strncmp(world->materials[i].name, texture->name, 16)) return i;
    return 0;
}
static void Brush(entity_t *e, const lq_scene *world)
{
    currententity = e;
    vec3_t forward, right, up; Basis(e, forward, right, up);
    model_t *model = e->model;
    if (model->surfaces != cl.worldmodel->surfaces) {
        /* Ammo/health boxes are standalone BSP models, not inline world
         * surfaces. Their geometry and textures belong to their own model,
         * unless a pack replaces them (Rygel's are MD3s). */
        const lq_pack_model *pack = pack_models ? LQ_PackModel(model) : NULL;
        if (pack && pack->replaces) { PackMesh(e, pack, LQ_SURFACE_MODEL|LQ_SURFACE_ACTOR); return; }
        for (int i = 0; i < model->nummodelsurfaces; ++i) {
            msurface_t *s = &model->surfaces[model->firstmodelsurface+i];
            texture_t *texture = R_TextureAnimation(s->texinfo->texture);
            uint32_t material = Material(texture,0,(byte *)texture+texture->offsets[0],texture->width,texture->height,NULL);
            Translucent(Draw((s->numedges-2)*3,material,LQ_SURFACE_MODEL|LQ_SURFACE_ACTOR), Opacity(e));
            float shade = model->lightdata ? fminf(fmaxf((255-R_LightPoint(e->origin))*0.25f,0),63) : 0;
            for (int triangle = 1; triangle < s->numedges-1; ++triangle) for (int corner = 0; corner < 3; ++corner) {
                int p = corner == 0 ? 0 : triangle+corner-1;
                int edge = model->surfedges[s->firstedge+p];
                const float *position = model->vertexes[model->edges[abs(edge)].v[edge < 0]].position;
                lq_vertex *v = &vertices[frame.vertex_count++]; memset(v,0,sizeof(*v));
                Transform(position,e,forward,right,up,v->position); v->shade = shade;
                float sign = s->flags & SURF_PLANEBACK ? -1 : 1;
                for (int k = 0; k < 3; ++k) v->normal[k] = sign*(s->plane->normal[0]*forward[k]-s->plane->normal[1]*right[k]+s->plane->normal[2]*up[k]);
                for (int axis = 0; axis < 2; ++axis)
                    v->uv[axis] = (DotProduct(position,s->texinfo->vecs[axis])+s->texinfo->vecs[axis][3])/
                        (axis ? texture->height : texture->width);
            }
        }
        return;
    }
    for (int i = 0; i < model->nummodelsurfaces; ++i) {
        uint32_t surface = model->firstmodelsurface+i;
        if (surface >= world->draw_count) Sys_Error("Invalid GPU brush surface");
        const lq_draw *source = &world->draws[surface];
        lq_draw *d = Draw(source->index_count, LQ_AnimatedMaterial(world, model->surfaces[surface].texinfo->texture), source->flags);
        Translucent(d, Opacity(e));
        memcpy(d->styles, source->styles, 4);
        memset(d->light_bits, 0xff, sizeof(d->light_bits));
        float centre[3] = {0, 0, 0};
        for (uint32_t j = 0; j < source->index_count; ++j) {
            const lq_vertex *input = &world->vertices[world->indices[source->first_index+j]];
            lq_vertex *v = &vertices[frame.vertex_count++]; *v = *input;
            Transform(input->position, e, forward, right, up, v->position);
            for (int k = 0; k < 3; ++k) v->normal[k] = input->normal[0]*forward[k]-input->normal[1]*right[k]+input->normal[2]*up[k];
            for (int k = 0; k < 3; ++k) centre[k] += v->position[k] / source->index_count;
        }
        /* Underwater, as world faces are judged (see LQ_GPUUpload): the face's
         * centre 2 units out along its normal, where the entity is now. With
         * translucent liquids, light from the other side of the surface is
         * dimmed by its opacity; without this a hatch on a pool's floor took
         * full light and stood out from the floor around it. */
        if (source->index_count && !(source->flags & (LQ_SURFACE_SKY|LQ_SURFACE_WATER))) {
            const lq_vertex *last = &vertices[frame.vertex_count-1];
            vec3_t point;
            for (int k = 0; k < 3; ++k) point[k] = centre[k] + last->normal[k] * 2;
            int contents = Mod_PointInLeaf(point, cl.worldmodel)->contents;
            if (contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA)
                d->flags |= LQ_SURFACE_SUBMERGED;
        }
    }
}
static void Entity(entity_t *e, const lq_scene *world)
{
    if (e->model->type == mod_alias) Alias(e, 0);
    else if (e->model->type == mod_sprite) Sprite(e);
    else if (e->model->type == mod_brush) Brush(e, world);
}
static float Distance(const entity_t *e)
{
    vec3_t centre, delta;
    for (int k = 0; k < 3; ++k) centre[k] = e->origin[k] + (e->model->mins[k] + e->model->maxs[k]) * 0.5f;
    VectorSubtract(centre, r_origin, delta);
    return DotProduct(delta, delta);
}
static int FartherFirst(const void *a, const void *b)
{
    float da = Distance(*(entity_t *const *)a), db = Distance(*(entity_t *const *)b);
    return da < db ? 1 : da > db ? -1 : 0;
}
void LQ_EntitiesBuild(const lq_scene *world)
{
    /* Translucent entities come after the solid ones, farthest first, so each
     * blends over what lies behind it. A fully transparent one draws nothing. */
    static entity_t *translucent[MAX_VISEDICTS];
    int blended = 0;
    if (r_drawentities.value) for (int i = 0; i < cl_numvisedicts; ++i) {
        entity_t *e = cl_visedicts[i];
        if (!e->model || e == &cl_entities[cl.viewentity] || e->alpha == ENTALPHA_ZERO) continue;
        if (Opacity(e) < 1) translucent[blended++] = e;
        else Entity(e, world);
    }
    qsort(translucent, blended, sizeof(*translucent), FartherFirst);
    for (int i = 0; i < blended; ++i) Entity(translucent[i], world);
    if (r_drawviewmodel.value && cl.viewent.model && !(cl.items & IT_INVISIBILITY) &&
        cl.stats[STAT_HEALTH] > 0 && !cl.intermission && cl.viewent.alpha != ENTALPHA_ZERO) Alias(&cl.viewent, 1);
    currententity = &cl_entities[0];
}
/* A smoke puff's size within its range: 0-1, strongly skewed small (the
 * fourth power of a uniform draw), so small puffs far outnumber large ones:
 * half fall in the bottom 6% of the range, one in ten in the top half. */
static float SmokeSize(unsigned int seed)
{
    uint32_t h = seed * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    float u = (h & 0xffffff) / (float)0x1000000;
    return u * u * u * u;
}
void LQ_EntitiesParticle(const float origin[3], int color, int emissive, int kind, float scale, float erode,
    unsigned int seed)
{
    vec3_t delta; VectorSubtract(origin, r_origin, delta);
    float distance = DotProduct(delta, vpn), size = distance < 20 ? 1.5f : 1.5f*(1+distance*0.004f);
    if (solid_particles && kind) {
        /* Droplets and smoke puffs: smooth-shaded spheres, finer when near.
         * A puff is a world-sized ball (scale x 1.5 units), not screen-sized. */
        const float (*sphere)[3][3];
        /* A puff is world-sized, not screen-sized, and wears away as it ages. */
        int smoke = kind == LQ_PARTICLE_SMOKE || kind == LQ_PARTICLE_TRAIL_SMOKE;
        /* Torch smoke: 0.25-1 unit when emitted. Trail smoke: at most half
         * Classic's triangle (size), so never bigger than a Software particle. */
        float radius = kind == LQ_PARTICLE_SMOKE ? scale*(0.45f + 1.35f*SmokeSize(seed))
                     : kind == LQ_PARTICLE_TRAIL_SMOKE ? fminf(scale*(0.2f + 0.5f*SmokeSize(seed)), 0.5f) * size
                     : size*LQ_ParticleDropRadius(seed);
        if (radius <= 0.05f || erode >= 0.98f) return;
        uint32_t count = LQ_ParticleSphere(distance < 96 || radius > 3, &sphere);
        Draw(count*3, particle_material, LQ_SURFACE_MODEL|
            (emissive ? 0 : LQ_SURFACE_ACTOR|LQ_SURFACE_SOLID_EFFECT)|(smoke ? LQ_SURFACE_ERODE : 0));
        for (uint32_t f = 0; f < count; ++f) for (int c = 0; c < 3; ++c) {
            lq_vertex *v = &vertices[frame.vertex_count++]; memset(v, 0, sizeof(*v));
            for (int j = 0; j < 3; ++j) { v->position[j] = origin[j] + radius*sphere[f][c][j]; v->normal[j] = sphere[f][c][j]; }
            v->uv[0] = ((color & 255)+0.5f)/256; v->uv[1] = smoke ? erode : 0.5f;
        }
        return;
    }
    if (solid_particles) {
        /* Lit by their face normals like actors. A tumbling solid's silhouette
         * stays within Classic's triangle, the flakes' cap. */
        float faces[LQ_PARTICLE_MAX_FACES][3][3], n[3];
        uint32_t count = LQ_ParticleSolid(seed, cl.time, faces);
        float area = LQ_ParticleProjectedArea((const float (*)[3][3])faces, count, vpn);
        if (area > 0.5f) size *= sqrtf(0.5f/area);
        Draw(count*3, particle_material, LQ_SURFACE_MODEL|
            (emissive ? 0 : LQ_SURFACE_ACTOR|LQ_SURFACE_SOLID_EFFECT));
        for (uint32_t f = 0; f < count; ++f) {
            vec3_t u, w; VectorSubtract(faces[f][1], faces[f][0], u); VectorSubtract(faces[f][2], faces[f][0], w);
            CrossProduct(u, w, n); VectorNormalize(n);
            for (int c = 0; c < 3; ++c) {
                lq_vertex *v = &vertices[frame.vertex_count++]; memset(v, 0, sizeof(*v));
                for (int j = 0; j < 3; ++j) { v->position[j] = origin[j] + size*faces[f][c][j]; v->normal[j] = n[j]; }
                v->uv[0] = ((color & 255)+0.5f)/256; v->uv[1] = 0.5f;
            }
        }
        return;
    }
    if (luminous_frame) {
        float corners[LQ_PARTICLE_MAX_SIDES][2];
        uint32_t sides = LQ_ParticleCorners(seed, corners), count = (sides-2)*3;
        Draw(count, particle_material, LQ_SURFACE_MODEL|
            (emissive ? 0 : LQ_SURFACE_ACTOR|LQ_SURFACE_DIFFUSE_EFFECT));
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t corner = i%3 == 0 ? 0 : i/3 + i%3;
            lq_vertex *v = &vertices[frame.vertex_count++]; memset(v, 0, sizeof(*v));
            for (int j = 0; j < 3; ++j) {
                v->position[j] = origin[j] + size*(vright[j]*corners[corner][0] + vup[j]*corners[corner][1]);
                v->normal[j] = -vpn[j];
            }
            v->uv[0] = ((color & 255)+0.5f)/256; v->uv[1] = 0.5f;
        }
        return;
    }
    Draw(3, particle_material, LQ_SURFACE_MODEL);
    for (int i = 0; i < 3; ++i) {
        lq_vertex *v = &vertices[frame.vertex_count++]; memset(v,0,sizeof(*v));
        for (int j = 0; j < 3; ++j) v->position[j] = origin[j]+(i==1 ? vup[j]*size : i==2 ? vright[j]*size : 0);
        v->uv[0] = ((color & 255)+0.5f)/256; v->uv[1] = 0.5f;
    }
}
