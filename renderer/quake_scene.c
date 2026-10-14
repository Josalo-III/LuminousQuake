/* Original GLQuake geometry/texture conventions: Copyright (C) 1996-1997 Id Software, Inc.
 * Copy original Quake assets into renderer-owned arrays. GPL-2.0-or-later.
 * The backend never receives model_t, allocator handles or QuakeC entities. */
#include "quakedef.h"
#include "quake_scene.h"
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
static void Spawn(model_t *world, lq_scene *scene)
{
    char *text = world->entities;
    int found = 0;
    memset(switched_off, 0, sizeof(switched_off));
    while (text && (text = COM_Parse(text)) && !strcmp(com_token, "{")) {
        char classname[128] = "", origin[128] = "", angles[128] = "", yaw[128] = "";
        int style = 0, spawnflags = 0;
        while ((text = COM_Parse(text)) && strcmp(com_token, "}")) {
            char key[1024]; snprintf(key, sizeof(key), "%s", com_token);
            text = COM_Parse(text); if (!text) break;
            if (!strcmp(key, "classname")) snprintf(classname, sizeof(classname), "%s", com_token);
            else if (!strcmp(key, "origin")) snprintf(origin, sizeof(origin), "%s", com_token);
            else if (!strcmp(key, "angles")) snprintf(angles, sizeof(angles), "%s", com_token);
            else if (!strcmp(key, "angle")) snprintf(yaw, sizeof(yaw), "%s", com_token);
            else if (!strcmp(key, "style")) style = atoi(com_token);
            else if (!strcmp(key, "spawnflags")) spawnflags = atoi(com_token);
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
    if (!map || !*map || strlen(map) > MAX_QPATH - 10 || strstr(map, "..") || strchr(map, '/') || strchr(map, '\\'))
        return Failure(s, error, size, "Use a map name such as e1m1, without a path or extension");
    if (argc && legacy_heap) return Failure(s, error, size, "A legacy scene is already loaded");
    if (argc >= MAX_NUM_ARGVS) return Failure(s, error, size, "Too many command-line arguments");
    if (argc) {
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
    for (int i = 0; i < world->nummodelsurfaces; ++i) {
        msurface_t *f = world->surfaces + world->firstmodelsurface + i;
        if (f->numedges < 3 || f->numedges > 65535) return Failure(s, error, size, "Invalid BSP polygon size");
        vertices += f->numedges; indices += (f->numedges - 2) * 3;
    }
    if (vertices > UINT32_MAX / sizeof(lq_vertex) || indices > UINT32_MAX / sizeof(uint32_t))
        return Failure(s, error, size, "Map exceeds GPU buffer limits");
    s->vertices = calloc((size_t)vertices, sizeof(*s->vertices));
    s->indices = calloc((size_t)indices, sizeof(*s->indices));
    s->draws = calloc(world->nummodelsurfaces, sizeof(*s->draws));
    s->materials = calloc(world->numtextures + 1, sizeof(*s->materials));
    texture_sources = calloc(world->numtextures + 1, sizeof(*texture_sources));
    if (!s->vertices || !s->indices || !s->draws || !s->materials || !texture_sources) {
        free(texture_sources); return Failure(s, error, size, "Cannot allocate scene geometry");
    }
#define FAIL(message) do { free(texture_sources); return Failure(s, error, size, message); } while (0)
    for (int i = 0; i < world->nummodelsurfaces; ++i) {
        msurface_t *f = world->surfaces + world->firstmodelsurface + i;
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
        if (!draw->flags) {
            if (light_width < 1 || light_height < 1 || light_width + 2 > ATLAS_SIZE || light_height + 2 > ATLAS_SIZE)
                FAIL("Invalid surface lightmap size");
            if (atlas_x + light_width + 1 > ATLAS_SIZE) { atlas_x = 1; atlas_y += row_height; row_height = 0; }
            if (atlas_y + light_height + 1 > ATLAS_SIZE) FAIL("Lightmap atlas full; multiple pages are not implemented yet");
            light_x = atlas_x; light_y = atlas_y; atlas_x += light_width + 2;
            if (light_height + 2 > row_height) row_height = light_height + 2;
            int maps = 0;
            if (f->samples) {
                while (maps < 4 && f->styles[maps] != 255) ++maps;
                if (!world->lightdata || (uintptr_t)f->samples < (uintptr_t)world->lightdata ||
                    (uintptr_t)f->samples - (uintptr_t)world->lightdata > (uintptr_t)lighting_bytes ||
                    (size_t)maps * light_width * light_height > (size_t)lighting_bytes -
                        ((uintptr_t)f->samples - (uintptr_t)world->lightdata)) FAIL("Surface samples outside BSP lighting");
                memcpy(draw->styles, f->styles, 4);
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
    free(texture_sources); Spawn(world, s);
    if (!LQ_SceneCheck(s, error, size)) { LQ_SceneFree(s); return 0; }
    printf("GPU scene: %u triangles, %u draws, %u indexed materials, %dx%d lightmap atlas\n",
        s->index_count / 3, s->draw_count, s->material_count, ATLAS_SIZE, ATLAS_SIZE);
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
