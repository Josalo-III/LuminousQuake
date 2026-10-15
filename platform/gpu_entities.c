/* GLQuake MDL/sprite/particle conventions, Copyright (C) 1996-1997 Id Software.
 * CPU animation/transforms produce owned GPU submissions, no CPU rasterization.
 * GPL-2.0-or-later. */
#include "quakedef.h"
#include "gpu_entities.h"
#include "render_bridge.h"
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
static uint8_t particle_pixels[256];
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
    memset(&frame, 0, sizeof(frame)); memset(keys, 0, sizeof(keys));
}
void LQ_EntitiesBegin(const lq_scene *world)
{
    frame.vertex_count = frame.index_count = frame.draw_count = 0;
    if (!frame.material_count) {
        if (world->material_count >= 4094) Sys_Error("Too many world materials");
        for (uint32_t i = 0; i < world->material_count; ++i) {
            materials[i] = world->materials[i]; materials[i].resource_id = i+1;
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
static int GroupFrame(const float *intervals, int count, double time)
{
    if (count < 1 || !isfinite(intervals[count-1]) || intervals[count-1] <= 0)
        Sys_Error("Invalid GPU animation interval");
    float target = fmod(fmax(time, 0), intervals[count-1]);
    int i;
    for (i = 0; i < count-1 && target >= intervals[i]; ++i) {}
    return i;
}
static void Basis(entity_t *e, vec3_t forward, vec3_t right, vec3_t up)
{
    vec3_t angles; VectorCopy(e->angles, angles); angles[PITCH] = -angles[PITCH];
    AngleVectors(angles, forward, right, up);
}
static void Transform(const float local[3], entity_t *e, const float forward[3],
                      const float right[3], const float up[3], float output[3])
{
    for (int i = 0; i < 3; ++i) output[i] = e->origin[i]+forward[i]*local[0]-right[i]*local[1]+up[i]*local[2];
}
static void Alias(entity_t *e, int weapon)
{
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
    uint32_t material = Material(e->model, selected->skin, (byte *)header+selected->skin,
                                model->skinwidth, model->skinheight, translation);
    int pose = e->frame >= 0 && e->frame < model->numframes ? e->frame : 0;
    int offset = header->frames[pose].frame;
    if (header->frames[pose].type == ALIAS_GROUP) {
        maliasgroup_t *g = (maliasgroup_t *)((byte *)header+offset);
        offset = g->frames[GroupFrame((float *)((byte *)header+g->intervals), g->numframes, cl.time+e->syncbase)].frame;
    }
    trivertx_t *points = (trivertx_t *)((byte *)header+offset);
    stvert_t *uv = (stvert_t *)((byte *)header+header->stverts);
    mtriangle_t *triangles = (mtriangle_t *)((byte *)header+header->triangles);
    vec3_t forward, right, up; Basis(e, forward, right, up);
    float ambient = R_LightPoint(e->origin), shade = ambient;
    if (weapon && ambient < 24) ambient = shade = 24;
    for (int i = 0; i < MAX_DLIGHTS; ++i) {
        dlight_t *light = &cl_dlights[i];
        if (light->die < cl.time || !light->radius) continue;
        vec3_t distance; VectorSubtract(e->origin, light->origin, distance);
        ambient += fmaxf(light->radius-Length(distance), 0);
    }
    ambient = fminf(ambient, 128); shade = fminf(shade, 192-ambient);
    for (int i = 1; i <= cl.maxclients; ++i)
        if (e == &cl_entities[i] && ambient < 8) { ambient = shade = 8; break; }
    int flame = !strcmp(e->model->name, "progs/flame.mdl") || !strcmp(e->model->name, "progs/flame2.mdl");
    if (flame) ambient = 256, shade = 0;
    /* Enhanced lights actors from runtime lights and probes; flames stay
     * fullbright emitters. Classic ignores the flag and uses `shade`. */
    Draw(model->numtris*3, material, LQ_SURFACE_MODEL | (weapon ? LQ_SURFACE_WEAPON : 0) | (flame ? 0 : LQ_SURFACE_ACTOR));
    for (int i = 0; i < model->numtris; ++i) for (int j = 0; j < 3; ++j) {
        int p = triangles[i].vertindex[j];
        if (p < 0 || p >= model->numverts || points[p].lightnormalindex >= 162) Sys_Error("Invalid GPU alias vertex");
        lq_vertex *v = &vertices[frame.vertex_count++]; memset(v, 0, sizeof(*v));
        vec3_t local, normal;
        for (int k = 0; k < 3; ++k) local[k] = points[p].v[k]*model->scale[k]+model->scale_origin[k];
        Transform(local, e, forward, right, up, v->position);
        const float *n = r_avertexnormals[points[p].lightnormalindex];
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
    Draw(6, material, LQ_SURFACE_MODEL|LQ_SURFACE_MASKED);
    for (int i = 0; i < 6; ++i) {
        int corner = corners[i]; lq_vertex *v = &vertices[frame.vertex_count++]; memset(v,0,sizeof(*v));
        for (int j = 0; j < 3; ++j) v->position[j] = e->origin[j]+right[j]*coords[corner][0]+up[j]*coords[corner][1];
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
         * surfaces. Their geometry and textures belong to their own model. */
        for (int i = 0; i < model->nummodelsurfaces; ++i) {
            msurface_t *s = &model->surfaces[model->firstmodelsurface+i];
            texture_t *texture = R_TextureAnimation(s->texinfo->texture);
            uint32_t material = Material(texture,0,(byte *)texture+texture->offsets[0],texture->width,texture->height,NULL);
            Draw((s->numedges-2)*3,material,LQ_SURFACE_MODEL|LQ_SURFACE_ACTOR);
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
        memcpy(d->styles, source->styles, 4);
        d->light_bits = UINT32_MAX;
        for (uint32_t j = 0; j < source->index_count; ++j) {
            const lq_vertex *input = &world->vertices[world->indices[source->first_index+j]];
            lq_vertex *v = &vertices[frame.vertex_count++]; *v = *input;
            Transform(input->position, e, forward, right, up, v->position);
            for (int k = 0; k < 3; ++k) v->normal[k] = input->normal[0]*forward[k]-input->normal[1]*right[k]+input->normal[2]*up[k];
        }
    }
}
void LQ_EntitiesBuild(const lq_scene *world)
{
    if (r_drawentities.value) for (int i = 0; i < cl_numvisedicts; ++i) {
        entity_t *e = cl_visedicts[i];
        if (!e->model || e == &cl_entities[cl.viewentity]) continue;
        if (e->model->type == mod_alias) Alias(e, 0);
        else if (e->model->type == mod_sprite) Sprite(e);
        else if (e->model->type == mod_brush) Brush(e, world);
    }
    if (r_drawviewmodel.value && cl.viewent.model && !(cl.items & IT_INVISIBILITY) &&
        cl.stats[STAT_HEALTH] > 0 && !cl.intermission) Alias(&cl.viewent, 1);
    currententity = &cl_entities[0];
}
void LQ_EntitiesParticle(const float origin[3], int color)
{
    vec3_t delta; VectorSubtract(origin, r_origin, delta);
    float distance = DotProduct(delta, vpn), size = distance < 20 ? 1.5f : 1.5f*(1+distance*0.004f);
    Draw(3, particle_material, LQ_SURFACE_MODEL);
    for (int i = 0; i < 3; ++i) {
        lq_vertex *v = &vertices[frame.vertex_count++]; memset(v,0,sizeof(*v));
        for (int j = 0; j < 3; ++j) v->position[j] = origin[j]+(i==1 ? vup[j]*size : i==2 ? vright[j]*size : 0);
        v->uv[0] = ((color & 255)+0.5f)/256; v->uv[1] = 0.5f;
    }
}
