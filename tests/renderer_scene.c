/* Scene validation and Quake-to-GPU projection. GPL-2.0-or-later. */
#include "scene.h"
#include "scene_fixture.h"
#include "indexed_mips.h"
#include "effects.h"
#include "fluorescent.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
static int EffectTest(void)
{
    float corners[LQ_PARTICLE_MAX_SIDES][2], again[LQ_PARTICLE_MAX_SIDES][2], smallest = INFINITY, largest = 0;
    float areas[LQ_PARTICLE_MAX_SIDES+1] = {0};
    uint32_t counts[LQ_PARTICLE_MAX_SIDES+1] = {0};
    for (uint32_t seed = 0; seed < 4096; ++seed) {
        uint32_t count = LQ_ParticleCorners(seed, corners);
        CHECK(count >= 3 && count <= LQ_PARTICLE_MAX_SIDES);
        CHECK(LQ_ParticleCorners(seed, again) == count);
        CHECK(!memcmp(corners, again, count*sizeof(*corners)));
        float area = 0, center[2] = {0};
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t j = (i+1)%count, k = (i+2)%count;
            CHECK(isfinite(corners[i][0]) && isfinite(corners[i][1]));
            CHECK((corners[j][0]-corners[i][0])*(corners[k][1]-corners[j][1]) -
                  (corners[j][1]-corners[i][1])*(corners[k][0]-corners[j][0]) > 0);
            area += corners[i][0]*corners[j][1] - corners[i][1]*corners[j][0];
            center[0] += corners[i][0]; center[1] += corners[i][1];
        }
        CHECK(fabsf(center[0]) < 0.00001f && fabsf(center[1]) < 0.00001f);
        CHECK(area * 0.5f <= 0.5f + 0.00001f); /* never larger than Classic's triangle */
        smallest = fminf(smallest, area); largest = fmaxf(largest, area);
        areas[count] += area * 0.5f; ++counts[count];
    }
    CHECK(largest > smallest*3); /* Visible size variation, stable frame to frame. */
    CHECK(largest * 0.5f > 0.4f); /* the largest come close to Classic's size */
    for (uint32_t sides = 3; sides <= LQ_PARTICLE_MAX_SIDES; ++sides) {
        CHECK(counts[sides] > 768 && counts[sides] < 1280); /* Every outline occurs frequently. */
        CHECK(areas[sides]/counts[sides] > 0.15f && areas[sides]/counts[sides] < 0.35f);
    } /* Mean area is about half of Classic's 0.5 for every side count. */
    lq_runtime_light samples[LQ_BEAM_LIGHTS];
    float start[3] = {0,0,0}, end[3] = {600,0,0};
    uint32_t count = LQ_BeamRuntimeLights(start, end, 100, samples);
    CHECK(count == LQ_BEAM_LIGHTS);
    /* Every part of a beam illuminates a receiver 64 units alongside it. */
    for (int x = 0; x <= 600; x += 10) {
        float value = 0;
        for (uint32_t i = 0; i < count; ++i) {
            float dx = samples[i].position_radius[0] - x;
            float distance = sqrtf(dx*dx + 64*64);
            value += fmaxf(samples[i].falloff[0] - samples[i].falloff[1]*distance, 0) * samples[i].color_strength[3];
            CHECK(samples[i].source_index == (LQ_DYNAMIC_LIGHT_SOURCE | (100+i)));
            CHECK(samples[i].color_strength[2] > samples[i].color_strength[0]);
        }
        CHECK(value > 40 && value < 200);
    }
    end[0] = 0;
    CHECK(LQ_BeamRuntimeLights(start, end, 100, samples) == 1);
    CHECK(samples[0].position_radius[0] == 0);
    end[0] = NAN; CHECK(!LQ_BeamRuntimeLights(start, end, 100, samples));
    puts("PASS stable convex 3-6 sided particle flakes, varied scale up to Classic's size and continuous beam-light coverage");
    return 0;
}
/* A closed 256-unit white room lit from its center: one wall per BSP node,
 * a single empty leaf inside. Every wall receives direct light, so bounce at
 * each sample is the mean exitance of the walls its hemisphere sees. */
/* ericw-tools surface-light pieces: GLQuake-style 128-unit axis splits. */
static int SurfaceLightTest(void)
{
    float square[4][3] = {{0,0,0},{256,0,0},{256,256,0},{0,256,0}}, points[8][3];
    CHECK(LQ_SurfaceLightPoints((const float (*)[3])square, 4, 128, points, 8) == 4);
    int seen = 0;
    for (int i = 0; i < 4; ++i) {
        CHECK(points[i][2] == 0);
        CHECK((points[i][0] == 64 || points[i][0] == 192) && (points[i][1] == 64 || points[i][1] == 192));
        seen |= 1 << ((points[i][0] > 128) + 2*(points[i][1] > 128));
    }
    CHECK(seen == 15);
    /* Cuts fall on the grid multiple nearest the centre: a strip inside one
     * cell stays whole, one across a grid line is cut there, and slivers
     * under 8 units are not cut off. */
    float strip[4][3] = {{4,0,0},{124,0,0},{124,16,0},{4,16,0}};
    CHECK(LQ_SurfaceLightPoints((const float (*)[3])strip, 4, 128, points, 8) == 1);
    CHECK(points[0][0] == 64 && points[0][1] == 8);
    for (int i = 0; i < 4; ++i) strip[i][0] -= 64;
    CHECK(LQ_SurfaceLightPoints((const float (*)[3])strip, 4, 128, points, 8) == 2);
    float sliver[4][3] = {{0,0,0},{132,0,0},{132,16,0},{0,16,0}};
    CHECK(LQ_SurfaceLightPoints((const float (*)[3])sliver, 4, 128, points, 8) == 1);
    /* Counting continues past the capacity; degenerate input yields nothing. */
    CHECK(LQ_SurfaceLightPoints((const float (*)[3])square, 4, 128, points, 2) == 4);
    CHECK(LQ_SurfaceLightPoints((const float (*)[3])square, 2, 128, points, 8) == 0);
    puts("PASS surface-light pieces: axis splits at 128-unit multiples, centroids, small faces and capacity");
    return 0;
}
/* Lights 0..33 along a wall, 10 units out; the camera at the first. */
static int LightBudgetTest(void)
{
    enum { COUNT = LQ_MAX_RUNTIME_LIGHTS + 2, GAME = LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS };
    lq_scene wall = {0};
    lq_vertex vertices[4] = {0};
    uint32_t indices[6] = {0,1,2,0,2,3};
    lq_draw draw = {0,6,0,0,{255,255,255,255}};
    static const float corners[4][2] = {{-50,-50},{COUNT*10,-50},{COUNT*10,50},{-50,50}};
    for (int i = 0; i < 4; ++i) {
        vertices[i].position[0] = corners[i][0]; vertices[i].position[2] = corners[i][1];
        vertices[i].normal[1] = 1;
    }
    wall.vertices = vertices; wall.indices = indices; wall.draws = &draw;
    wall.vertex_count = 4; wall.index_count = 6; wall.draw_count = 1;
    lq_light_source lights[COUNT] = {0};
    for (uint32_t i = 0; i < COUNT; ++i) {
        LQ_LightSourceDefaults(&lights[i]);
        lights[i].origin[0] = (float)i*10; lights[i].origin[1] = 10;
        lights[i].intensity = 100; lights[i].entity_index = i;
    }
    wall.light_sources = lights; wall.light_source_count = COUNT;
    float styles[256], camera[3] = {0,10,0};
    for (int i = 0; i < 256; ++i) styles[i] = 1;
    lq_light_drop dropped[COUNT];
    uint32_t drops = 0;
    /* Every light reaches the wall at (100 - 10) * 0.5; the nearest GAME stay. */
    CHECK(LQ_LightBudget(&wall, styles, camera, NULL, GAME, dropped, COUNT, &drops) == COUNT);
    CHECK(drops == COUNT - GAME && dropped[0].source == GAME && fabsf(dropped[0].peak - 45) < 0.01f);
    for (uint32_t i = 0; i < drops; ++i) CHECK(dropped[i].source == GAME + i);
    CHECK(LQ_LightBudget(&wall, styles, camera, NULL, LQ_MAX_RUNTIME_LIGHTS, dropped, COUNT, &drops) == COUNT && drops == 2);
    /* A truncated list still counts every drop; stronger drops sort first. */
    lights[COUNT-1].intensity = 150; /* longer reach, still ranked last */
    CHECK(LQ_LightBudget(&wall, styles, camera, NULL, GAME, dropped, 3, &drops) == COUNT && drops == COUNT - GAME);
    CHECK(dropped[0].source == COUNT-1 && fabsf(dropped[0].peak - 70) < 0.01f && dropped[1].source == GAME);
    /* Behind the wall, dark styles and draws out of view do not reach. */
    lights[COUNT-1].origin[1] = -10; lights[COUNT-2].style = 1; styles[1] = 0;
    CHECK(LQ_LightBudget(&wall, styles, camera, NULL, GAME, dropped, COUNT, &drops) == COUNT - 2 && drops == COUNT - GAME - 2);
    uint8_t hidden = 0;
    CHECK(LQ_LightBudget(&wall, styles, camera, &hidden, GAME, dropped, COUNT, &drops) == 0 && drops == 0);
    /* Over budget, selection first drops sources that light nothing in view. */
    lq_runtime_light runtime[LQ_MAX_RUNTIME_LIGHTS] = {0};
    lq_draw shown = draw, unseen = draw; unseen.index_count = 0;
    CHECK(LQ_BuildRuntimeLightsVisible(&wall, styles, camera, &shown, runtime, GAME) == GAME);
    memset(runtime, 0, sizeof(runtime));
    CHECK(LQ_BuildRuntimeLightsVisible(&wall, styles, camera, &unseen, runtime, GAME) == 0);
    puts("PASS light budget: reaching lights, dropped by slot limit, strongest first, facing, styles, visibility and the over-budget filter");
    return 0;
}
static int BounceRoomTest(void)
{
    enum { HALF = 128, GRID = HALF*2/16 + 1 };
    lq_scene room = {0}; char error[256];
    room.vertices = calloc(24, sizeof(*room.vertices)); room.indices = calloc(36, sizeof(*room.indices));
    room.draws = calloc(6, sizeof(*room.draws)); room.materials = calloc(1, sizeof(*room.materials));
    room.face_samples = calloc(6, sizeof(*room.face_samples));
    room.sample_positions = calloc(6*GRID*GRID, sizeof(*room.sample_positions));
    room.bsp_nodes = calloc(6, sizeof(*room.bsp_nodes)); room.leaf_contents = calloc(2, 1);
    room.light_sources = calloc(1, sizeof(*room.light_sources));
    room.colormap = (lq_image){256, 64, 4, calloc(256*64, 4)};
    room.lightmap = (lq_image){128, 32, 4, calloc(128*32, 4)};
    room.materials[0].indices = (lq_image){1, 1, 1, calloc(1, 1)};
    CHECK(room.vertices && room.indices && room.draws && room.materials && room.face_samples && room.sample_positions &&
        room.bsp_nodes && room.leaf_contents && room.light_sources && room.colormap.pixels && room.lightmap.pixels &&
        room.materials[0].indices.pixels);
    room.vertex_count = 24; room.index_count = 36; room.draw_count = 6; room.material_count = 1;
    room.bsp_node_count = 6; room.leaf_count = 2; room.leaf_contents[0] = LQ_LEAF_SOLID;
    room.atlas_height = 2 + GRID; room.bounce_scale = 1;
    for (int wall = 0; wall < 6; ++wall) {
        int axis = wall / 2, u = (axis + 1) % 3, v = (axis + 2) % 3;
        float inward = wall & 1 ? -1.0f : 1.0f; /* even walls sit at -HALF */
        lq_bsp_node *node = &room.bsp_nodes[wall];
        node->normal[axis] = inward; node->dist = -HALF;
        node->children[0] = wall < 5 ? wall + 1 : -2; node->children[1] = -1;
        node->first_face = (uint32_t)wall; node->face_count = 1;
        static const float corners[4][2] = {{-1,-1},{1,-1},{1,1},{-1,1}};
        for (int c = 0; c < 4; ++c) {
            lq_vertex *vertex = &room.vertices[wall*4 + c];
            vertex->position[axis] = -inward * HALF;
            vertex->position[u] = corners[c][0] * HALF; vertex->position[v] = corners[c][1] * HALF;
            vertex->normal[axis] = inward;
        }
        static const uint32_t fan[6] = {0,1,2,0,2,3};
        for (int i = 0; i < 6; ++i) room.indices[wall*6 + i] = (uint32_t)(wall*4) + fan[i];
        room.draws[wall] = (lq_draw){(uint32_t)wall*6, 6, 0, 0, {0,255,255,255}, {0}, {255,255,255,255}};
        lq_face_samples *grid = &room.face_samples[wall];
        grid->s[u] = 1; grid->t[v] = 1; grid->mins[0] = grid->mins[1] = -HALF;
        grid->width = grid->height = GRID; grid->atlas_x = (uint16_t)(1 + wall*(GRID + 2)); grid->atlas_y = 1;
        grid->albedo[0] = grid->albedo[1] = grid->albedo[2] = 1;
        grid->first_sample = room.sample_count;
        for (int y = 0; y < GRID; ++y) for (int x = 0; x < GRID; ++x) {
            float *point = room.sample_positions[room.sample_count++];
            point[axis] = -inward * (HALF - 1); point[u] = (float)(x*16 - HALF); point[v] = (float)(y*16 - HALF);
        }
    }
    lq_light_source *light = room.light_sources; room.light_source_count = 1;
    LQ_LightSourceDefaults(light); light->intensity = 600; light->style = 5;
    CHECK(LQ_SceneCheck(&room, error, sizeof(error)));
    CHECK(LQ_SceneComputeBounce(&room, 256, NULL, error, sizeof(error)));
    CHECK(LQ_SceneCheck(&room, error, sizeof(error)));
    /* Direct exitance runs from 236 at a wall's center to ~149 at its corners;
     * a wall center sees mostly the opposite wall and the side walls' middles. */
    const uint8_t *center = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + GRID/2) * 4;
    for (int wall = 0; wall < 6; ++wall) {
        CHECK(room.draws[wall].bounce_styles[0] == 5 && room.draws[wall].bounce_styles[1] == 255);
        const uint8_t *texel = center + (size_t)wall * (GRID + 2) * 4;
        CHECK(texel[0] > 140 && texel[0] < 215 && texel[0] == texel[1] && abs((int)texel[0] - center[0]) <= 3);
    }
    /* Probes: at 0.4 reflectance a wall's direct exitance runs ~60-94 and one
     * bounce adds ~30, so the centre probe, which sees every wall, measures
     * ~105 in all six directions. */
    for (int wall = 0; wall < 6; ++wall)
        room.face_samples[wall].albedo[0] = room.face_samples[wall].albedo[1] = room.face_samples[wall].albedo[2] = 0.4f;
    CHECK(LQ_SceneComputeBounce(&room, 128, NULL, error, sizeof(error)));
    CHECK(room.probe_static && room.probe_styles[0] == 5 && room.probe_styles[1] == 255);
    uint32_t px = room.probe_size[0] / 2, py = room.probe_size[1] / 2, pz = room.probe_size[2] / 2;
    float probe_center[3] = {room.probe_origin[0] + px*room.probe_spacing, room.probe_origin[1] + py*room.probe_spacing,
        room.probe_origin[2] + pz*room.probe_spacing};
    CHECK(fabsf(probe_center[0]) < 1 && fabsf(probe_center[1]) < 1 && fabsf(probe_center[2]) < 1);
    int first_value = -1;
    for (uint32_t d = 0; d < 6; ++d) {
        const uint8_t *texel = room.probe_static + ((((size_t)d * room.probe_size[2] + pz) * room.probe_size[1] + py) *
            room.probe_size[0] + px) * 4;
        if (first_value < 0) first_value = texel[0];
        CHECK(texel[0] > 85 && texel[0] < 125 && abs((int)texel[0] - first_value) <= 6);
    }
    /* Gameplay light: a muzzle-flash-sized light in the room bounces into
     * probes near it and nowhere when absent. */
    size_t dynamic_bytes = (size_t)room.probe_size[0] * room.probe_size[1] * room.probe_size[2] * 6 * 4;
    uint8_t *dynamic = malloc(dynamic_bytes);
    CHECK(dynamic);
    lq_runtime_light gameplay[LQ_MAX_RUNTIME_LIGHTS] = {0};
    static const float warm[3] = {1, 1, 1};
    float flash[3] = {0, 0, 0};
    LQ_DynamicRuntimeLight(&gameplay[LQ_MAX_RUNTIME_LIGHTS-1], flash, 220, warm, 4, 1);
    CHECK(!LQ_SceneDynamicProbes(&room, gameplay, LQ_MAX_RUNTIME_LIGHTS-1, 16, NULL, dynamic));
    CHECK(LQ_SceneDynamicProbes(&room, gameplay, LQ_MAX_RUNTIME_LIGHTS, 16, NULL, dynamic));
    const uint8_t *bounced = dynamic + (((size_t)pz * room.probe_size[1] + py) * room.probe_size[0] + px) * 4;
    /* Walls at 128-222 units get (220 - d) * incidence, at most 92; at 0.4
     * reflectance the centre receives roughly a fifth of that. */
    CHECK(bounced[0] > 8 && bounced[0] < 40 && bounced[0] == bounced[1]);
    free(dynamic);
    /* Black walls reflect nothing. */
    for (int wall = 0; wall < 6; ++wall) memset(room.face_samples[wall].albedo, 0, sizeof(room.face_samples[wall].albedo));
    CHECK(LQ_SceneComputeBounce(&room, 64, NULL, error, sizeof(error)));
    center = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + GRID/2) * 4;
    CHECK(room.draws[0].bounce_styles[0] == 255 && center[0] == 0 && room.sky_light[0] == 0);
    /* Sky ceiling: the +Z wall becomes sky over a sky leaf, drawn with a mid
     * grey (128/255 at row 0), so an open sky gives brightness ~64. The floor
     * centre sees the 256-unit ceiling 255 units up: form factor ~0.24. The
     * centre probe's +Z face sees it from 128 units: ~0.55; -Z sees none. */
    uint8_t *contents = realloc(room.leaf_contents, 3);
    lq_image *sky = &room.materials[0].indices;
    CHECK(contents && (sky->pixels = realloc(sky->pixels, 2)));
    room.leaf_contents = contents; room.leaf_contents[2] = LQ_LEAF_SKY; room.leaf_count = 3;
    room.bsp_nodes[5].children[1] = -3;
    room.draws[5].flags = LQ_SURFACE_SKY; room.face_samples[5].width = 0;
    sky->width = 2; sky->pixels[0] = sky->pixels[1] = 0;
    room.colormap.pixels[0] = room.colormap.pixels[1] = room.colormap.pixels[2] = 128;
    CHECK(LQ_SceneComputeBounce(&room, 256, NULL, error, sizeof(error)));
    CHECK(fabsf(room.sky_light[0] - 128.0f * 128 / 255 * LQ_SKY_LIGHT_GAIN) < 0.01f);
    const uint8_t *floor = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + 4*(GRID + 2) + GRID/2) * 4;
    CHECK(floor[3] > 52 && floor[3] < 72);
    const uint8_t *up = room.probe_static + ((((size_t)4 * room.probe_size[2] + pz) * room.probe_size[1] + py) *
        room.probe_size[0] + px) * 4;
    const uint8_t *down = room.probe_static + ((((size_t)5 * room.probe_size[2] + pz) * room.probe_size[1] + py) *
        room.probe_size[0] + px) * 4;
    CHECK(up[3] > 125 && up[3] < 158 && down[3] == 0);
    /* id's qbsp sealed sky brushes as solid: the sky face alone marks sky. */
    int sky_leaf_floor = floor[3];
    room.bsp_nodes[5].children[1] = -1;
    CHECK(LQ_SceneComputeBounce(&room, 256, NULL, error, sizeof(error)));
    floor = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + 4*(GRID + 2) + GRID/2) * 4;
    CHECK(abs((int)floor[3] - sky_leaf_floor) <= 2);
    room.bsp_nodes[5].children[1] = -3;
    /* LIGHT's rays stop only at solid: a source in the sky leaf above the
     * ceiling still lights the room, so its style reaches the bounce. */
    for (int wall = 0; wall < 6; ++wall)
        room.face_samples[wall].albedo[0] = room.face_samples[wall].albedo[1] = room.face_samples[wall].albedo[2] = 1;
    light->origin[2] = HALF + 64;
    CHECK(LQ_SceneLeaf(&room, light->origin) == 2);
    CHECK(LQ_SceneComputeBounce(&room, 64, NULL, error, sizeof(error)));
    int sky_lit = 0;
    for (int wall = 0; wall < 5; ++wall) for (int k = 0; k < 4; ++k) sky_lit |= room.draws[wall].bounce_styles[k] == 5;
    CHECK(sky_lit);
    /* Sun straight down through the sky ceiling: the floor and the centre
     * probe see it (layer 1 alpha); walls face across it and keep none. Its
     * reflection off the floor lights a wall's bounce. Lit from below, the
     * solid floor hides it from everything. */
    light->origin[2] = 0; light->style = 0; light->intensity = 1;
    CHECK(LQ_SceneComputeBounce(&room, 64, NULL, error, sizeof(error)));
    const uint8_t *wall_center = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + GRID/2) * 4;
    int unlit_wall = wall_center[0];
    room.sun_direction[2] = -1; room.sun_light[0] = room.sun_light[1] = room.sun_light[2] = 40;
    room.sun_angle_scale = 0.5f; room.sun_penumbra = 4;
    CHECK(LQ_SceneComputeBounce(&room, 64, NULL, error, sizeof(error)));
    size_t layer1 = (size_t)room.atlas_height * room.bounce.width * 4;
    const uint8_t *sun_floor = room.bounce.pixels + layer1 +
        ((size_t)(1 + GRID/2) * room.bounce.width + 1 + 4*(GRID + 2) + GRID/2) * 4;
    wall_center = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + GRID/2) * 4;
    CHECK(sun_floor[3] == 255 && wall_center[layer1 + 3] == 0);
    CHECK(wall_center[0] > unlit_wall + 4);
    size_t probe_sun = (((size_t)(6 + 4) * room.probe_size[2] + pz) * room.probe_size[1] + py) * room.probe_size[0] + px;
    CHECK(room.probe_static[probe_sun * 4 + 3] == 255);
    room.sun_direction[2] = 1;
    CHECK(LQ_SceneComputeBounce(&room, 64, NULL, error, sizeof(error)));
    sun_floor = room.bounce.pixels + layer1 + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + 4*(GRID + 2) + GRID/2) * 4;
    CHECK(sun_floor[3] == 0 && room.probe_static[probe_sun * 4 + 3] == 0);
    LQ_SceneFree(&room);
    puts("PASS static bounce and probe gather: closed room hits, cosine weighting, style layers, albedo, gameplay-light probes and sky visibility through sky faces or leaves, light through sky leaves, sun visibility and bounce");
    return 0;
}
static int FluorescentTest(void)
{
    lq_light_source sources[5];
    memset(sources, 0, sizeof(sources));
    const char *names[5] = {"light_fluoro", "light_fluorospark", "light_fluorospark", "light", "light"};
    for (int i = 0; i < 5; ++i) {
        LQ_LightSourceDefaults(&sources[i]);
        snprintf(sources[i].classname, sizeof(sources[i].classname), "%s", names[i]);
        sources[i].origin[0] = 100.0f * i; sources[i].intensity = 300;
    }
    sources[1].style = 10; sources[2].style = 33; /* a switched failing tube keeps its switch */
    sources[3].origin[0] = 116; /* the beam spotlight in the failing tube's fixture */
    sources[4].origin[0] = 500; /* an ordinary light */
    lq_scene scene = {0}; scene.light_sources = sources; scene.light_source_count = 5;
    CHECK(LQ_LightFluorescent(&sources[0]) == 1 && LQ_LightFluorescent(&sources[1]) == 2);
    CHECK(LQ_SceneAssignFluorescentStyles(&scene) == 1);
    CHECK(sources[1].style == LQ_FLUORESCENT_STYLE_BASE && sources[2].style == 33);
    CHECK(sources[3].style == LQ_FLUORESCENT_STYLE_BASE && sources[4].style == 0);
    float tube[3], beam[3];
    CHECK(LQ_FluorescentGain(&scene, 1, 2.5, LQ_CAMERA_FILM, tube) && LQ_FluorescentGain(&scene, 3, 2.5, LQ_CAMERA_FILM, beam));
    CHECK(!memcmp(tube, beam, sizeof(tube)));
    float rgb[3], low[3] = {9,9,9}, high[3] = {0}, step = 0;
    CHECK(LQ_FluorescentGain(&scene, 0, 3.7, LQ_CAMERA_EYE, rgb) && rgb[0] == 1 && rgb[1] == 1 && rgb[2] == 1);
    /* NTSC's 1/60 shutter spans whole 60 and 120 Hz cycles: in sync, steady. */
    for (double t = 0; t < 20; t += 0.0137) {
        CHECK(LQ_FluorescentGain(&scene, 0, t, LQ_CAMERA_NTSC, rgb));
        for (int k = 0; k < 3; ++k) CHECK(fabsf(rgb[k] - 1) < 0.002f);
    }
    /* Film breathes slowly: a few percent, blue deepest, smooth frame to frame. */
    float last = 0;
    for (double t = 0; t < 30; t += 0.01) {
        CHECK(LQ_FluorescentGain(&scene, 0, t, LQ_CAMERA_FILM, rgb));
        for (int k = 0; k < 3; ++k) { low[k] = fminf(low[k], rgb[k]); high[k] = fmaxf(high[k], rgb[k]); }
        if (t > 0) step = fmaxf(step, fabsf(rgb[1] - last));
        last = rgb[1];
    }
    CHECK(high[1] - low[1] > 0.05f && high[1] - low[1] < 0.2f);
    CHECK(high[2] - low[2] > high[1] - low[1] && high[1] - low[1] > high[0] - low[0]);
    CHECK(step < 0.01f);
    /* A fast shutter keeps far more of the 120 Hz term. */
    float fast_low = 9, fast_high = 0;
    for (double t = 0; t < 30; t += 0.01) {
        LQ_FluorescentGain(&scene, 0, t, LQ_CAMERA_NTSC_FAST, rgb);
        fast_low = fminf(fast_low, rgb[1]); fast_high = fmaxf(fast_high, rgb[1]);
    }
    CHECK(fast_high - fast_low > 3 * (high[1] - low[1]));
    /* PAL's 5 Hz flutter changes far faster than film's breathing. */
    float pal_step = 0;
    for (double t = 0; t < 5; t += 0.01) {
        LQ_FluorescentGain(&scene, 0, t, LQ_CAMERA_PAL, rgb); float before = rgb[1];
        LQ_FluorescentGain(&scene, 0, t + 0.05, LQ_CAMERA_PAL, rgb); pal_step = fmaxf(pal_step, fabsf(rgb[1] - before));
    }
    CHECK(pal_step > 0.03f);
    /* A failing tube is mostly lit, drops out a few percent of the time, glows
     * while restriking and overshoots on strike; the same time gives the same light. */
    uint32_t dark = 0, glow = 0, over = 0, samples = 0;
    for (double t = 0; t < 600; t += 0.005, ++samples) {
        float styles[256] = {0}, again[256] = {0};
        LQ_FluorescentStyles(&scene, t, styles); LQ_FluorescentStyles(&scene, t, again);
        float v = styles[LQ_FLUORESCENT_STYLE_BASE];
        CHECK(v == again[LQ_FLUORESCENT_STYLE_BASE] && v >= 0 && v < 1.6f && styles[33] == 0);
        dark += v == 0; glow += v > 0 && v < 0.1f; over += v > 1.2f;
        if (v > 0 && v < 0.1f) { CHECK(LQ_FluorescentGain(&scene, 1, t, LQ_CAMERA_EYE, rgb) && rgb[0] > rgb[2] * 4); }
    }
    CHECK(dark > samples / 100 && dark < samples / 10);
    CHECK(glow > samples / 200 && over > samples / 500);
    CHECK(!LQ_FluorescentGain(&scene, 4, 0, LQ_CAMERA_FILM, rgb) && !LQ_FluorescentGain(&scene, 5, 0, LQ_CAMERA_FILM, rgb));
    puts("PASS fluorescent tubes: private failing-tube styles, camera-synced mains flicker and phosphor colour, failure timeline");
    return 0;
}
int main(void)
{
    lq_mip_palette *palette = malloc(sizeof(*palette));
    uint8_t colors[256*4] = {0};
    CHECK(palette);
    colors[4] = 255; colors[8+2] = 255;
    colors[12] = colors[12+2] = 128;
    colors[16] = 170; colors[18] = 85;
    LQ_MipPaletteInit(palette, colors);
    uint8_t checker[] = {1,2,2,1}; lq_mip_chain chain;
    lq_image image = {2,2,1,checker};
    CHECK(LQ_IndexedMips(&image,palette,&chain));
    CHECK(chain.count == 2 && chain.bytes == 5 && chain.pixels[4] == 3);
    free(chain.pixels);
    uint8_t odd[] = {1,2,1}; image = (lq_image){3,1,1,odd};
    CHECK(LQ_IndexedMips(&image,palette,&chain));
    CHECK(chain.count == 2 && chain.widths[1] == 1 && chain.heights[1] == 1 && chain.pixels[3] == 4);
    free(chain.pixels);
    uint8_t bright[] = {224,225,225,224};
    image = (lq_image){2,2,1,bright};
    CHECK(LQ_IndexedMips(&image,palette,&chain) && chain.pixels[4] >= 224);
    free(chain.pixels);
    image.width = 8193; CHECK(!LQ_IndexedMips(&image,palette,&chain));
    image.width = 1; image.height = 1; image.pixels = bright;
    CHECK(LQ_IndexedMips(&image,palette,&chain) && chain.count == 1 && chain.pixels[0] == 224);
    free(chain.pixels); free(palette);
    puts("PASS color-aware indexed mips, fullbrights, odd and 1x1 sizes, bounds");
    lq_scene scene; lq_view view = {0}; char error[256]; float matrix[16];
    CHECK(LQ_SceneFixture(&scene)); CHECK(LQ_SceneCheck(&scene, error, sizeof(error)));
    scene.light_sources = calloc(1,sizeof(*scene.light_sources)); CHECK(scene.light_sources);
    scene.light_source_count = 1;
    lq_light_source *source = scene.light_sources;
    LQ_LightSourceDefaults(source);
    source->intensity = 300; source->style = 32; source->spawnflags = 1;
    float styles[256] = {0};
    CHECK(LQ_LightSourceIntensity(source,styles) == 0);
    styles[32] = 0.5f; CHECK(LQ_LightSourceIntensity(source,styles) == 150);
    styles[32] = 1; CHECK(LQ_LightSourceIntensity(source,styles) == 300); /* live style overrides initial off flag */
    source->origin[0] = 100;
    lq_runtime_light runtime[LQ_MAX_RUNTIME_LIGHTS] = {0}; float camera[3] = {0};
    CHECK(LQ_BuildRuntimeLights(&scene,styles,camera,runtime) == 1);
    /* Linear: value 300 falls below half a brightness unit (value 1 after the
     * 0.5 output scale) at 299 units. */
    CHECK(runtime[0].position_radius[0] == 100 && runtime[0].position_radius[3] == 299);
    CHECK(runtime[0].color_strength[0] == 1 && runtime[0].color_strength[3] == 0.5f);
    CHECK(runtime[0].falloff[0] == 300 && runtime[0].falloff[1] == 1 && runtime[0].spot[3] < -1);
    source->formula = LQ_LIGHT_LOCAL_MINLIGHT; memset(runtime,0,sizeof(runtime));
    CHECK(LQ_BuildRuntimeLights(&scene,styles,camera,runtime) == 0);
    source->formula = LQ_LIGHT_LINEAR;
    CHECK(LQ_BuildRuntimeLights(&scene,styles,camera,runtime) == 1);
    styles[32] = 0; /* a held light keeps its slot (and cached shadow) while dark */
    CHECK(LQ_BuildRuntimeLights(&scene,styles,camera,runtime) == 1 && runtime[0].color_strength[3] == 0);
    memset(runtime,0,sizeof(runtime));
    CHECK(LQ_BuildRuntimeLights(&scene,styles,camera,runtime) == 0);
    styles[32] = 1; source->origin[0] = 0;
    source->flags = LQ_LIGHT_UNRESOLVED_PARAMETERS;
    CHECK(LQ_LightSourceIntensity(source,styles) == 0); source->flags = 0;
    source->style = 255; CHECK(!LQ_SceneCheck(&scene,error,sizeof(error)));
    source->style = 32; source->origin[0] = NAN;
    CHECK(!LQ_SceneCheck(&scene,error,sizeof(error))); source->origin[0] = 0;
    scene.vertices[0].normal[0] = NAN;
    CHECK(!LQ_SceneCheck(&scene,error,sizeof(error))); scene.vertices[0].normal[0] = -1;
    CHECK(LQ_SceneCheck(&scene,error,sizeof(error)));
    lq_draw interactions[2];
    memset(runtime,0,sizeof(runtime)); runtime[0].position_radius[3] = 100;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 1);
    CHECK(interactions[0].light_bits[0] == 1 && interactions[1].light_bits[0] == 0);
    runtime[0].position_radius[3] = 200;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 2);
    CHECK(interactions[0].light_bits[0] == 1 && interactions[1].light_bits[0] == 1);
    runtime[0].position_radius[0] = 200;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 0);
    CHECK(interactions[0].light_bits[0] == 0 && interactions[1].light_bits[0] == 0);
    runtime[0].position_radius[0] = 0; scene.draws[0].flags = LQ_SURFACE_SKY;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 1);
    CHECK(interactions[0].light_bits[0] == 0 && interactions[1].light_bits[0] == 1);
    scene.draws[0].flags = 0;
    scene.light_visibility_words = 1;
    scene.light_visibility = calloc(2,sizeof(*scene.light_visibility));
    CHECK(scene.light_visibility);
    scene.light_visibility[0] = 1; /* draw 0 sees source 0; draw 1 is blocked. */
    runtime[0].source_index = 0;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 1);
    CHECK(interactions[0].light_bits[0] == 1 && interactions[1].light_bits[0] == 0);
    free(scene.light_visibility); scene.light_visibility = NULL; scene.light_visibility_words = 0;
    source->attenuation = 0; CHECK(!LQ_SceneCheck(&scene,error,sizeof(error))); source->attenuation = 1;
    source->spotlight = 1; CHECK(!LQ_SceneCheck(&scene,error,sizeof(error)));
    source->spot_direction[2] = -1; source->spot_outer = source->spot_inner = 0.9397f;
    CHECK(LQ_SceneCheck(&scene,error,sizeof(error))); source->spotlight = 0;
    puts("PASS owned and selected point lights, animated/switchable styles and normal validation");
    /* ericw-tools "delay" formulas and their cutoff ranges. */
    lq_light_source falloff = {0}; LQ_LightSourceDefaults(&falloff); falloff.intensity = 200;
    CHECK(LQ_LightFalloff(&falloff,50) == 150 && LQ_LightSourceRange(&falloff) == 199);
    falloff.attenuation = 2; CHECK(LQ_LightFalloff(&falloff,50) == 100 && LQ_LightSourceRange(&falloff) == 99.5f);
    falloff.attenuation = 1; falloff.formula = LQ_LIGHT_INVERSE;
    CHECK(LQ_LightFalloff(&falloff,256) == 100 && LQ_LightSourceRange(&falloff) == LQ_LIGHT_MAX_RANGE);
    falloff.formula = LQ_LIGHT_INVERSE2;
    CHECK(LQ_LightFalloff(&falloff,256) == 50 && fabsf(LQ_LightSourceRange(&falloff) - 1810.19f) < 0.01f);
    CHECK(fabsf(LQ_LightFalloff(&falloff,LQ_LightSourceRange(&falloff)) - 1) < 0.001f);
    falloff.formula = LQ_LIGHT_INVERSE2A;
    CHECK(LQ_LightFalloff(&falloff,128) == 50);
    CHECK(fabsf(LQ_LightFalloff(&falloff,LQ_LightSourceRange(&falloff)) - 1) < 0.001f);
    falloff.formula = LQ_LIGHT_INFINITE;
    CHECK(LQ_LightFalloff(&falloff,10000) == 200 && LQ_LightSourceRange(&falloff) == LQ_LIGHT_MAX_RANGE);
    falloff.formula = LQ_LIGHT_LOCAL_MINLIGHT;
    CHECK(LQ_LightFalloff(&falloff,0) == 0 && LQ_LightSourceRange(&falloff) == 0);
    falloff.formula = LQ_LIGHT_LINEAR; falloff.intensity = 1;
    CHECK(LQ_LightSourceRange(&falloff) == 0);
    puts("PASS linear, inverse, inverse-square, offset inverse-square and unattenuated falloff ranges");
    scene.indices[0] = scene.vertex_count;
    CHECK(!LQ_SceneCheck(&scene, error, sizeof(error))); scene.indices[0] = 0;
    scene.draws[0].index_count = 0xffffffff;
    CHECK(!LQ_SceneCheck(&scene, error, sizeof(error))); scene.draws[0].index_count = 3;
    scene.vertices[0].uv[0] = NAN;
    CHECK(!LQ_SceneCheck(&scene, error, sizeof(error))); scene.vertices[0].uv[0] = 0.5f;
    view.horizontal_fov = 90; LQ_ViewMatrix(&view, 4.0f / 3, matrix);
    /* A point straight ahead has zero screen displacement and depth in [0,1]. */
    CHECK(fabsf(matrix[0] * 64 + matrix[12]) < 0.0001f);
    CHECK(fabsf(matrix[1] * 64 + matrix[13]) < 0.0001f);
    CHECK((matrix[2] * 64 + matrix[14]) / (matrix[3] * 64 + matrix[15]) > 0);
    CHECK((matrix[2] * 64 + matrix[14]) / (matrix[3] * 64 + matrix[15]) < 1);
    CHECK(matrix[4] < 0 && matrix[9] > 0); /* +Y is screen left; +Z screen up. */
    view.origin[0] = 100; view.angles[1] = 90; LQ_ViewMatrix(&view, 1, matrix);
    CHECK(fabsf(matrix[0] * 100 + matrix[4] * 64 + matrix[12]) < 0.0001f);
    LQ_SceneFree(&scene); CHECK(!scene.vertices && !scene.material_count); LQ_SceneFree(&scene);
    lq_scene selection = {0};
    selection.light_source_count = LQ_MAX_RUNTIME_LIGHTS+2;
    selection.light_sources = calloc(selection.light_source_count,sizeof(*selection.light_sources));
    CHECK(selection.light_sources);
    styles[0] = 1;
    for (uint32_t i = 0; i < selection.light_source_count; ++i) {
        LQ_LightSourceDefaults(&selection.light_sources[i]);
        selection.light_sources[i].origin[0] = (float)i*10;
        selection.light_sources[i].intensity = 100; selection.light_sources[i].entity_index = i;
    }
    /* Nearest first, placed by rank into the sharpest free shadow slots: ranks
     * 0-15 share the 512 cubes (map slots 0-7, then the gameplay slots 120-127
     * the viewer gives to map lights), 16-47 the 256 cubes (slots 8-39), the
     * rest the 128 cubes (slots 40-119). */
    memset(runtime,0,sizeof(runtime));
    CHECK(LQ_BuildRuntimeLights(&selection,styles,camera,runtime) == LQ_MAX_RUNTIME_LIGHTS);
    CHECK(runtime[0].source_index == 0 && runtime[7].source_index == 7 && runtime[120].source_index == 8);
    CHECK(runtime[127].source_index == 15 && runtime[8].source_index == 16 && runtime[39].source_index == 47);
    CHECK(runtime[40].source_index == 48 && runtime[119].source_index == 127);
    uint32_t array, cube;
    LQ_ShadowSlot(0, &array, &cube); CHECK(array == 0 && cube == LQ_MAX_DYNAMIC_LIGHTS);
    LQ_ShadowSlot(120, &array, &cube); CHECK(array == 0 && cube == 0);
    LQ_ShadowSlot(8, &array, &cube); CHECK(array == 1 && cube == 0);
    LQ_ShadowSlot(79, &array, &cube); CHECK(array == 2 && cube == 39);
    LQ_ShadowSlot(80, &array, &cube); CHECK(array == 3 && cube == 0);
    /* A small move keeps every slot, and so every cached cube. */
    lq_runtime_light before[LQ_MAX_RUNTIME_LIGHTS];
    memcpy(before, runtime, sizeof(before));
    camera[0] = 4;
    CHECK(LQ_BuildRuntimeLights(&selection,styles,camera,runtime) == LQ_MAX_RUNTIME_LIGHTS);
    for (uint32_t i = 0; i < LQ_MAX_RUNTIME_LIGHTS; ++i) CHECK(runtime[i].source_index == before[i].source_index);
    /* Across the row the order reverses: sources 0/1 leave, 128/129 take the
     * sharpest cubes, and a source keeps its slot only while its new rank
     * stays within half again of its resolution band. */
    camera[0] = (LQ_MAX_RUNTIME_LIGHTS + 1) * 10;
    CHECK(LQ_BuildRuntimeLights(&selection,styles,camera,runtime) == LQ_MAX_RUNTIME_LIGHTS);
    for (uint32_t i = 0; i < LQ_MAX_RUNTIME_LIGHTS; ++i) CHECK(runtime[i].source_index >= 2);
    CHECK(runtime[0].source_index == 129 && runtime[1].source_index == 128);
    CHECK(runtime[72].source_index == 80); /* rank 49, still in the 128 band */
    for (uint32_t i = 0; i < LQ_MAX_RUNTIME_LIGHTS; ++i)
        if (runtime[i].source_index == 5) { LQ_ShadowSlot(i, &array, &cube); CHECK(array >= 1); } /* rank 124 leaves 512 */
    camera[0] = 0;
    /* PVS reach: one plane at x=0, leaf 1 in front and leaf 2 behind, each
     * seeing only itself. A source in leaf 2 is ineligible from leaf 1. */
    lq_bsp_node node = {{1,0,0},0,{-2,-3}};
    uint32_t pvs[3] = {7,2,4}, reach[1] = {4};
    selection.light_source_count = 1; selection.light_sources[0].origin[0] = -64;
    selection.bsp_nodes = &node; selection.bsp_node_count = 1; selection.leaf_count = 3;
    selection.leaf_words = 1; selection.leaf_pvs = pvs; selection.light_leaves = reach;
    float front[3] = {64,0,0}, back[3] = {-32,0,0};
    CHECK(LQ_SceneLeaf(&selection,front) == 1 && LQ_SceneLeaf(&selection,back) == 2);
    memset(runtime,0,sizeof(runtime));
    CHECK(LQ_BuildRuntimeLights(&selection,styles,front,runtime) == 0);
    CHECK(LQ_BuildRuntimeLights(&selection,styles,back,runtime) == 1 && runtime[0].source_index == 0);
    selection.bsp_nodes = NULL; selection.leaf_pvs = selection.light_leaves = NULL;
    LQ_SceneFree(&selection);
    puts("PASS deterministic bounded runtime-light selection, stable shadow slots and PVS reach");
    CHECK(LightBudgetTest() == 0);
    CHECK(SurfaceLightTest() == 0);
    CHECK(BounceRoomTest() == 0);
    CHECK(EffectTest() == 0);
    CHECK(FluorescentTest() == 0);
    puts("PASS renderer scene ownership, invalid geometry rejection and Quake/GPU camera projection"); return 0;
}
