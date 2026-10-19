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
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
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
    /* 3D particles: closed, convex, outward-wound and stable for a seed; time
     * only turns them, and their mean silhouette matches the flakes'. */
    uint32_t shapes[LQ_PARTICLE_MAX_FACES+1] = {0};
    float mean = 0, widest = 0;
    for (uint32_t seed = 0; seed < 4096; ++seed) {
        float faces[LQ_PARTICLE_MAX_FACES][3][3], again[LQ_PARTICLE_MAX_FACES][3][3], later[LQ_PARTICLE_MAX_FACES][3][3];
        uint32_t faces_count = LQ_ParticleSolid(seed, 12.5, faces);
        CHECK(faces_count == 4 || faces_count == 6 || faces_count == 8);
        CHECK(LQ_ParticleSolid(seed, 12.5, again) == faces_count && !memcmp(faces, again, sizeof(faces[0])*faces_count));
        CHECK(LQ_ParticleSolid(seed, 12.75, later) == faces_count && memcmp(faces, later, sizeof(faces[0])*faces_count));
        ++shapes[faces_count];
        float closure[3] = {0}, surface = 0, surface_later = 0;
        for (uint32_t f = 0; f < faces_count; ++f) {
            float u[3], v[3], n[3], m[3];
            for (int k = 0; k < 3; ++k) { u[k] = faces[f][1][k]-faces[f][0][k]; v[k] = faces[f][2][k]-faces[f][0][k]; }
            n[0] = u[1]*v[2]-u[2]*v[1]; n[1] = u[2]*v[0]-u[0]*v[2]; n[2] = u[0]*v[1]-u[1]*v[0];
            for (int k = 0; k < 3; ++k) { u[k] = later[f][1][k]-later[f][0][k]; v[k] = later[f][2][k]-later[f][0][k]; }
            m[0] = u[1]*v[2]-u[2]*v[1]; m[1] = u[2]*v[0]-u[0]*v[2]; m[2] = u[0]*v[1]-u[1]*v[0];
            surface += 0.5f*sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            surface_later += 0.5f*sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
            for (int k = 0; k < 3; ++k) closure[k] += n[k];
            /* Faces point away from the centre; every vertex lies behind every face's plane. */
            CHECK(n[0]*faces[f][0][0]+n[1]*faces[f][0][1]+n[2]*faces[f][0][2] > 0);
            for (uint32_t g = 0; g < faces_count; ++g) for (int c = 0; c < 3; ++c) {
                float d = 0;
                for (int k = 0; k < 3; ++k) d += n[k]*(faces[g][c][k]-faces[f][0][k]);
                CHECK(d < 0.00001f);
            }
        }
        CHECK(fabsf(closure[0]) < 0.0001f && fabsf(closure[1]) < 0.0001f && fabsf(closure[2]) < 0.0001f);
        CHECK(fabsf(surface - surface_later) < 0.0001f);
        CHECK(surface*0.25f >= 0.15f - 0.0001f && surface*0.25f <= 0.35f + 0.0001f);
        float axes[3][3] = {{1,0,0},{0,1,0},{0,0,1}}, average = 0;
        for (int a = 0; a < 3; ++a) {
            float seen = LQ_ParticleProjectedArea((const float (*)[3][3])faces, faces_count, axes[a]);
            average += seen/3; widest = fmaxf(widest, seen);
        }
        mean += average/4096;
    }
    for (uint32_t n = 4; n <= 8; n += 2) CHECK(shapes[n] > 1100 && shapes[n] < 1650);
    CHECK(mean > 0.2f && mean < 0.3f); /* like the flakes, about half of Classic's 0.5 */
    CHECK(widest > 0.4f); /* some views exceed the cap, which the renderer applies */
    /* Droplets: closed, outward-wound unit spheres; discs no larger than Classic's triangle. */
    for (int fine = 0; fine < 2; ++fine) {
        const float (*sphere)[3][3];
        uint32_t sphere_faces = LQ_ParticleSphere(fine, &sphere);
        CHECK(sphere_faces == (fine ? 80u : 20u));
        float closure[3] = {0};
        for (uint32_t f = 0; f < sphere_faces; ++f) {
            float u[3], v[3], n[3];
            for (int k = 0; k < 3; ++k) { u[k] = sphere[f][1][k]-sphere[f][0][k]; v[k] = sphere[f][2][k]-sphere[f][0][k]; }
            n[0] = u[1]*v[2]-u[2]*v[1]; n[1] = u[2]*v[0]-u[0]*v[2]; n[2] = u[0]*v[1]-u[1]*v[0];
            CHECK(n[0]*sphere[f][0][0]+n[1]*sphere[f][0][1]+n[2]*sphere[f][0][2] > 0);
            for (int k = 0; k < 3; ++k) closure[k] += n[k];
            for (int c = 0; c < 3; ++c)
                CHECK(fabsf(sphere[f][c][0]*sphere[f][c][0]+sphere[f][c][1]*sphere[f][c][1]+sphere[f][c][2]*sphere[f][c][2]-1) < 0.0001f);
        }
        CHECK(fabsf(closure[0]) < 0.0001f && fabsf(closure[1]) < 0.0001f && fabsf(closure[2]) < 0.0001f);
    }
    for (uint32_t seed = 0; seed < 4096; ++seed) {
        float r = LQ_ParticleDropRadius(seed);
        CHECK(r == LQ_ParticleDropRadius(seed) && 3.14159265f*r*r >= 0.15f - 0.0001f && 3.14159265f*r*r <= 0.35f + 0.0001f);
    }
    puts("PASS stable convex 3-6 sided particle flakes, varied scale up to Classic's size, closed convex tumbling 3D particles, spherical droplets and continuous beam-light coverage");
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
/* E1M1's exterior: a facade far ahead and a wall behind the camera with more
 * lights close by than there are slots. Nearness alone kept the lights behind
 * and dropped the facade's, so its lights went out as the player backed away. */
static int InViewSelectionTest(void)
{
    enum { NEAR_LIGHTS = LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS, FAR_LIGHTS = 4,
           COUNT = NEAR_LIGHTS + FAR_LIGHTS };
    lq_scene scene = {0};
    lq_vertex vertices[8] = {0};
    uint32_t indices[12] = {0,1,2,0,2,3, 4,5,6,4,6,7};
    lq_draw draws[2] = {{0,6,0,0,{255,255,255,255}}, {6,6,0,0,{255,255,255,255}}};
    static const float corners[4][2] = {{-100,-100},{100,-100},{100,100},{-100,100}};
    for (int i = 0; i < 4; ++i) {
        vertices[i].position[0] = 1000; vertices[i].normal[0] = -1;   /* facade, in view */
        vertices[i+4].position[0] = -50; vertices[i+4].normal[0] = 1; /* wall behind the camera */
        for (int k = 0; k < 8; k += 4) {
            vertices[i+k].position[1] = corners[i][0]; vertices[i+k].position[2] = corners[i][1];
        }
    }
    scene.vertices = vertices; scene.indices = indices; scene.draws = draws;
    scene.vertex_count = 8; scene.index_count = 12; scene.draw_count = 2;
    lq_light_source lights[COUNT] = {0};
    for (uint32_t i = 0; i < COUNT; ++i) {
        LQ_LightSourceDefaults(&lights[i]);
        int far = i >= NEAR_LIGHTS;
        lights[i].origin[0] = far ? 990 : -40; lights[i].origin[1] = (float)(i % 40) * 4 - 80;
        lights[i].intensity = 100; lights[i].entity_index = i;
    }
    scene.light_sources = lights; scene.light_source_count = COUNT;
    float styles[256];
    for (int i = 0; i < 256; ++i) styles[i] = 1;
    lq_view view = {0}; view.horizontal_fov = 90;
    float clip[16]; LQ_LightSelectionClip(&view, 4.0f / 3, clip);
    lq_runtime_light runtime[LQ_MAX_RUNTIME_LIGHTS] = {0};
    int far_kept = 0;
    /* Without the view, the nearest fill every slot. */
    LQ_BuildRuntimeLightsVisible(&scene, styles, view.origin, draws, runtime, NEAR_LIGHTS);
    for (uint32_t s = 0; s < NEAR_LIGHTS; ++s) far_kept += runtime[s].position_radius[3] > 0 && runtime[s].source_index >= NEAR_LIGHTS;
    CHECK(far_kept == 0);
    /* With it, every light on the facade keeps a slot, and the wall's lights,
     * which light nothing on screen, fill the rest marked offscreen. */
    memset(runtime, 0, sizeof(runtime)); far_kept = 0;
    CHECK(LQ_BuildRuntimeLightsInView(&scene, styles, view.origin, draws, clip, runtime, NEAR_LIGHTS) == NEAR_LIGHTS);
    int offscreen_ok = 1;
    for (uint32_t s = 0; s < NEAR_LIGHTS; ++s) {
        if (runtime[s].position_radius[3] <= 0) continue;
        int far = runtime[s].source_index >= NEAR_LIGHTS;
        far_kept += far; offscreen_ok &= runtime[s].offscreen == !far;
    }
    CHECK(far_kept == FAR_LIGHTS); CHECK(offscreen_ok);
    /* Facing the other way, the wall's lights lead again. */
    view.angles[1] = 180; LQ_LightSelectionClip(&view, 4.0f / 3, clip);
    memset(runtime, 0, sizeof(runtime)); far_kept = 0;
    LQ_BuildRuntimeLightsInView(&scene, styles, view.origin, draws, clip, runtime, NEAR_LIGHTS);
    for (uint32_t s = 0; s < NEAR_LIGHTS; ++s) far_kept += runtime[s].position_radius[3] > 0 && runtime[s].source_index >= NEAR_LIGHTS;
    CHECK(far_kept == 0);
    /* The budget report, selecting as the game does, drops nothing in view. */
    lq_light_drop dropped[COUNT]; uint32_t drops = 0;
    uint8_t pvs[2] = {1, 1};
    view.angles[1] = 0; LQ_LightSelectionClip(&view, 4.0f / 3, clip);
    float exact[16]; LQ_ViewMatrix(&view, 4.0f / 3, exact);
    CHECK(LQ_LightBudgetInView(&scene, styles, view.origin, pvs, clip, exact, NEAR_LIGHTS, dropped, COUNT, &drops) == FAR_LIGHTS);
    CHECK(drops == 0);
    puts("PASS in-view light selection: lights on faces in view outrank nearer lights behind the camera");
    return 0;
}
/* A room lit on all four walls, under the slot budget. Turning the camera
 * changes which lights are offscreen but not which slot holds which light, so
 * no shadow cube is rebuilt just for looking around. */
static int TurningKeepsSlotsTest(void)
{
    enum { WALLS = 4, LIGHTS = 48 };
    static const float dir[WALLS][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    lq_scene scene = {0};
    lq_vertex vertices[WALLS * 4] = {0}; uint32_t indices[WALLS * 6]; lq_draw draws[WALLS] = {0};
    for (int w = 0; w < WALLS; ++w) {
        float tx = -dir[w][1], ty = dir[w][0], c[4][2] = {{-400,-200},{400,-200},{400,200},{-400,200}};
        for (int i = 0; i < 4; ++i) {
            lq_vertex *v = &vertices[w*4+i];
            v->position[0] = dir[w][0]*400 + tx*c[i][0]; v->position[1] = dir[w][1]*400 + ty*c[i][0];
            v->position[2] = c[i][1]; v->normal[0] = -dir[w][0]; v->normal[1] = -dir[w][1];
        }
        uint32_t b = (uint32_t)w*4, q[6] = {b,b+1,b+2,b,b+2,b+3}; memcpy(&indices[w*6], q, sizeof(q));
        draws[w] = (lq_draw){(uint32_t)w*6, 6, 0, 0, {255,255,255,255}};
    }
    scene.vertices = vertices; scene.indices = indices; scene.draws = draws;
    scene.vertex_count = WALLS*4; scene.index_count = WALLS*6; scene.draw_count = WALLS;
    lq_light_source lights[LIGHTS] = {0};
    for (int i = 0; i < LIGHTS; ++i) { /* twelve per wall, 40 units off it */
        LQ_LightSourceDefaults(&lights[i]);
        int w = i % WALLS; float along = (float)(i / WALLS) * 64 - 352;
        lights[i].origin[0] = dir[w][0]*360 - dir[w][1]*along; lights[i].origin[1] = dir[w][1]*360 + dir[w][0]*along;
        lights[i].intensity = 200; lights[i].entity_index = (uint32_t)i;
    }
    scene.light_sources = lights; scene.light_source_count = LIGHTS;
    float styles[256];
    for (int i = 0; i < 256; ++i) styles[i] = 1;
    lq_runtime_light runtime[LQ_MAX_RUNTIME_LIGHTS] = {0};
    uint32_t first[LQ_MAX_RUNTIME_LIGHTS];
    int moved = 0, offscreen_seen = 1;
    for (int yaw = 0; yaw <= 720; yaw += 10) {
        lq_view view = {0}; view.horizontal_fov = 90; view.angles[1] = (float)yaw;
        float clip[16]; LQ_LightSelectionClip(&view, 16.0f / 9, clip);
        LQ_BuildRuntimeLightsInView(&scene, styles, view.origin, draws, clip, runtime,
            LQ_MAX_RUNTIME_LIGHTS - LQ_MAX_DYNAMIC_LIGHTS);
        int offscreen = 0;
        for (uint32_t s = 0; s < LQ_MAX_RUNTIME_LIGHTS; ++s) {
            uint32_t source = runtime[s].position_radius[3] > 0 ? runtime[s].source_index : UINT32_MAX;
            if (!yaw) first[s] = source; else moved += first[s] != source;
            offscreen += source != UINT32_MAX && runtime[s].offscreen;
        }
        offscreen_seen &= offscreen > 0;
    }
    CHECK(moved == 0); CHECK(offscreen_seen);
    puts("PASS turning keeps light slots: offscreen lights keep their slots and shadow cubes");
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
    room.atlas_height = 2 + GRID; room.bounce_scale = 1; room.bounce_limit = 1;
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
    /* Repeated bounces: a closed room returns all its light, so each bounce is
     * 0.4 of the last and their sum 1/(1 - 0.4) of the first, reached once a
     * bounce adds under 1% (0.4^5 < 0.01). */
    const uint8_t *bounced_wall = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + GRID/2 + 3 * (GRID + 2)) * 4;
    int single = bounced_wall[0];
    CHECK(room.bounces == 1);
    room.bounce_limit = 0;
    CHECK(LQ_SceneComputeBounce(&room, 128, NULL, error, sizeof(error)));
    bounced_wall = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + GRID/2 + 3 * (GRID + 2)) * 4;
    CHECK(room.bounces >= 5 && room.bounces <= 7);
    CHECK(fabsf(bounced_wall[0] - single / 0.6f) <= 0.06f * single / 0.6f && bounced_wall[0] == bounced_wall[1]);
    room.bounce_limit = 2;
    CHECK(LQ_SceneComputeBounce(&room, 128, NULL, error, sizeof(error)));
    bounced_wall = room.bounce.pixels + ((size_t)(1 + GRID/2) * room.bounce.width + 1 + GRID/2 + 3 * (GRID + 2)) * 4;
    CHECK(room.bounces == 2 && abs((int)bounced_wall[0] - (int)(single * 1.4f + 0.5f)) <= 2);
    room.bounce_limit = 1;
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
     * grey (128/255 in the palette row), so an open sky gives brightness ~128. The floor
     * centre sees the 256-unit ceiling 255 units up: form factor ~0.24. The
     * centre probe's +Z face sees it from 128 units: ~0.55; -Z sees none. */
    uint8_t *contents = realloc(room.leaf_contents, 3);
    lq_image *sky = &room.materials[0].indices;
    CHECK(contents && (sky->pixels = realloc(sky->pixels, 2)));
    room.leaf_contents = contents; room.leaf_contents[2] = LQ_LEAF_SKY; room.leaf_count = 3;
    room.bsp_nodes[5].children[1] = -3;
    room.draws[5].flags = LQ_SURFACE_SKY; room.face_samples[5].width = 0;
    sky->width = 2; sky->pixels[0] = sky->pixels[1] = 0;
    uint8_t *grey = room.colormap.pixels + 32 * 256 * 4; /* palette row */
    grey[0] = grey[1] = grey[2] = 128;
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
    CHECK(LQ_FluorescentGain(&scene, NULL, 1, 2.5, LQ_CAMERA_FILM, tube) && LQ_FluorescentGain(&scene, NULL, 3, 2.5, LQ_CAMERA_FILM, beam));
    CHECK(!memcmp(tube, beam, sizeof(tube)));
    float rgb[3], low[3] = {9,9,9}, high[3] = {0}, step = 0;
    CHECK(LQ_FluorescentGain(&scene, NULL, 0, 3.7, LQ_CAMERA_EYE, rgb) && rgb[0] == 1 && rgb[1] == 1 && rgb[2] == 1);
    /* NTSC's 1/60 shutter spans whole 60 and 120 Hz cycles: in sync, steady. */
    for (double t = 0; t < 20; t += 0.0137) {
        CHECK(LQ_FluorescentGain(&scene, NULL, 0, t, LQ_CAMERA_NTSC, rgb));
        for (int k = 0; k < 3; ++k) CHECK(fabsf(rgb[k] - 1) < 0.002f);
    }
    /* Film breathes slowly: a few percent, blue deepest, smooth frame to frame. */
    float last = 0;
    for (double t = 0; t < 30; t += 0.01) {
        CHECK(LQ_FluorescentGain(&scene, NULL, 0, t, LQ_CAMERA_FILM, rgb));
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
        LQ_FluorescentGain(&scene, NULL, 0, t, LQ_CAMERA_NTSC_FAST, rgb);
        fast_low = fminf(fast_low, rgb[1]); fast_high = fmaxf(fast_high, rgb[1]);
    }
    CHECK(fast_high - fast_low > 3 * (high[1] - low[1]));
    /* PAL's 5 Hz flutter changes far faster than film's breathing. */
    float pal_step = 0;
    for (double t = 0; t < 5; t += 0.01) {
        LQ_FluorescentGain(&scene, NULL, 0, t, LQ_CAMERA_PAL, rgb); float before = rgb[1];
        LQ_FluorescentGain(&scene, NULL, 0, t + 0.05, LQ_CAMERA_PAL, rgb); pal_step = fmaxf(pal_step, fabsf(rgb[1] - before));
    }
    CHECK(pal_step > 0.03f);
    /* A failing tube is mostly lit, drops out a few percent of the time, glows
     * while restriking and overshoots on strike; the same time gives the same light. */
    uint32_t dark = 0, glow = 0, over = 0, samples = 0;
    for (double t = 0; t < 600; t += 0.005, ++samples) {
        float styles[256] = {0}, again[256] = {0};
        LQ_FluorescentStyles(&scene, NULL, t, styles); LQ_FluorescentStyles(&scene, NULL, t, again);
        float v = styles[LQ_FLUORESCENT_STYLE_BASE];
        CHECK(v == again[LQ_FLUORESCENT_STYLE_BASE] && v >= 0 && v < 1.6f && styles[33] == 0);
        dark += v == 0; glow += v > 0 && v < 0.1f; over += v > 1.2f;
        if (v > 0 && v < 0.1f) { CHECK(LQ_FluorescentGain(&scene, NULL, 1, t, LQ_CAMERA_EYE, rgb) && rgb[0] > rgb[2] * 4); }
    }
    CHECK(dark > samples / 100 && dark < samples / 10);
    CHECK(glow > samples / 200 && over > samples / 500);
    CHECK(!LQ_FluorescentGain(&scene, NULL, 4, 0, LQ_CAMERA_FILM, rgb) && !LQ_FluorescentGain(&scene, NULL, 5, 0, LQ_CAMERA_FILM, rgb));
    /* The mercury lines have no persistence, so a fast shutter sees green spike
     * with blue: more of green's ripple than its phosphor alone would keep. */
    float spike_low[3] = {9,9,9}, spike_high[3] = {0};
    for (double t = 0; t < 30; t += 0.01) {
        LQ_FluorescentGain(&scene, NULL, 0, t, LQ_CAMERA_NTSC_FAST, rgb);
        for (int k = 0; k < 3; ++k) { spike_low[k] = fminf(spike_low[k], rgb[k]); spike_high[k] = fmaxf(spike_high[k], rgb[k]); }
    }
    CHECK(spike_high[1] - spike_low[1] > 0.62f * (spike_high[2] - spike_low[2]));
    CHECK(spike_high[0] - spike_low[0] < spike_high[1] - spike_low[1]);
    /* Mercury's green as each camera records it, in the tube's fixture too and
     * in its bounce; luminance and other lights unchanged. */
    float green_over_red[3];
    for (uint32_t camera = 0; camera < 3; ++camera) {
        lq_light_source tinted[5]; memcpy(tinted, sources, sizeof(tinted));
        lq_scene tinting = scene; tinting.light_sources = tinted;
        uint32_t chosen = camera == 0 ? LQ_CAMERA_EYE : camera == 1 ? LQ_CAMERA_PAL : LQ_CAMERA_FILM;
        LQ_SceneTintFluorescents(&tinting, chosen);
        const float *c = tinted[0].color;
        CHECK(c[1] == 1 && c[0] < c[2] && c[2] < 1);
        float before = sources[0].output_scale, after = tinted[0].output_scale;
        CHECK(fabsf((0.2126f*c[0] + 0.7152f*c[1] + 0.0722f*c[2]) * after - before) < 1e-4f);
        CHECK(!memcmp(tinted[1].color, c, sizeof(tinted[1].color)) && !memcmp(tinted[3].color, c, sizeof(tinted[3].color)));
        CHECK(tinted[4].color[0] == 1 && tinted[4].color[1] == 1 && tinted[4].output_scale == sources[4].output_scale);
        green_over_red[camera] = c[1] / c[0];
    }
    CHECK(green_over_red[0] < green_over_red[1] && green_over_red[1] < green_over_red[2] && green_over_red[2] > 1.3f);
    CHECK(LQ_FluorescentCast(LQ_CAMERA_NTSC) == LQ_FluorescentCast(LQ_CAMERA_PAL) &&
          LQ_FluorescentCast(LQ_CAMERA_FILM) != LQ_FluorescentCast(LQ_CAMERA_PAL));
    /* With its sound, the failing tube sputters with the crackle: steady over
     * the hum, out through each short burst, overshooting bluish as it holds,
     * flashing bluish through the one big burst, and the same each time round. */
    static signed char buzz[2 * 11025];
    for (uint32_t i = 0; i < sizeof(buzz); ++i) {
        double t = i / 11025.0, hum = 6 * sin(2 * M_PI * 120 * t);
        int crackle = (t >= 0.3 && t < 0.35) || (t >= 0.6 && t < 0.65) || (t >= 1.0 && t < 1.2);
        buzz[i] = (signed char)(crackle ? (i * 2654435761u >> 24) - 128 : hum);
    }
    static lq_tube_track track;
    CHECK(LQ_TubeTrackFromSound(&track, buzz, 1, sizeof(buzz), 11025) == 400);
    CHECK(!LQ_TubeTrackFromSound(&track, buzz, 3, sizeof(buzz), 11025) && !track.count);
    LQ_TubeTrackFromSound(&track, buzz, 1, sizeof(buzz), 11025);
    float steady[256] = {0}, out[256] = {0}, after[256] = {0}, flash[256] = {0}, again[256] = {0}, beam_rgb[3];
    float normal = 264.0f / 256;
    track.position = 1.6; LQ_FluorescentStyles(&scene, &track, 0, steady);
    track.position = 0.33; LQ_FluorescentStyles(&scene, &track, 0, out);
    track.position = 0.373; LQ_FluorescentStyles(&scene, &track, 0, after);
    CHECK(LQ_FluorescentGain(&scene, &track, 1, 0, LQ_CAMERA_EYE, rgb) && rgb[2] > rgb[0]);
    CHECK(LQ_FluorescentGain(&scene, &track, 3, 0, LQ_CAMERA_EYE, beam_rgb) && !memcmp(rgb, beam_rgb, sizeof(rgb)));
    track.position = 2.373; LQ_FluorescentStyles(&scene, &track, 0, again);
    CHECK(fabsf(steady[LQ_FLUORESCENT_STYLE_BASE] - normal) < 1e-5f && out[LQ_FLUORESCENT_STYLE_BASE] < 0.1f);
    CHECK(after[LQ_FLUORESCENT_STYLE_BASE] > 1.1f * normal && after[LQ_FLUORESCENT_STYLE_BASE] < 1.6f);
    CHECK(fabsf(again[LQ_FLUORESCENT_STYLE_BASE] - after[LQ_FLUORESCENT_STYLE_BASE]) < 1e-4f);
    track.position = 0.63; LQ_FluorescentStyles(&scene, &track, 0, out);
    CHECK(out[LQ_FLUORESCENT_STYLE_BASE] < 0.1f);
    track.position = 1.1; LQ_FluorescentStyles(&scene, &track, 0, flash);
    CHECK(flash[LQ_FLUORESCENT_STYLE_BASE] > 1.5f * normal && flash[LQ_FLUORESCENT_STYLE_BASE] <= 2 * normal + 1e-5f);
    CHECK(LQ_FluorescentGain(&scene, &track, 1, 0, LQ_CAMERA_EYE, rgb) && rgb[2] > 1.4f * rgb[0]);
    /* Through the flash the tube and its fixture throw their light twice as far. */
    for (uint32_t index = 1; index <= 3; index += 2) {
        lq_runtime_light light; memset(&light, 0, sizeof(light));
        light.source_index = index; light.falloff[0] = sources[index].intensity;
        float range = light.position_radius[3] = LQ_LightSourceRange(&sources[index]);
        CHECK(range > 0 && LQ_FluorescentReach(&scene, &track, &light));
        CHECK(fabsf(light.position_radius[3] - 2 * range) < 1e-3f * range && light.falloff[0] > sources[index].intensity);
    }
    lq_runtime_light plain; memset(&plain, 0, sizeof(plain)); plain.source_index = 0;
    CHECK(!LQ_FluorescentReach(&scene, &track, &plain));
    track.position = 1.6;
    plain.source_index = 1; CHECK(!LQ_FluorescentReach(&scene, &track, &plain));
    puts("PASS fluorescent tubes: private failing-tube styles, camera-synced mains flicker and phosphor colour, mercury lines and cast, failure timeline, sound-synced sputter and strike flash");
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
    /* Unbounded falloffs stop at LQ_LIGHT_FADE_LEVEL after output scaling: 2 in
     * falloff units at this light's 0.5 output scale. */
    CHECK(LQ_LightFalloff(&falloff,256) == 100 && LQ_LightSourceRange(&falloff) == 12800);
    falloff.formula = LQ_LIGHT_INVERSE2;
    CHECK(LQ_LightFalloff(&falloff,256) == 50 && fabsf(LQ_LightSourceRange(&falloff) - 1280) < 0.01f);
    CHECK(fabsf(LQ_LightFalloff(&falloff,LQ_LightSourceRange(&falloff)) - 2) < 0.001f);
    falloff.gate = 4; /* a higher ericw-tools _gate stops it sooner */
    CHECK(fabsf(LQ_LightFalloff(&falloff,LQ_LightSourceRange(&falloff)) - 8) < 0.001f);
    falloff.gate = 0.001f; /* a lower one does not extend it */
    CHECK(fabsf(LQ_LightSourceRange(&falloff) - 1280) < 0.01f);
    falloff.gate = 0;
    falloff.formula = LQ_LIGHT_INVERSE2A;
    CHECK(LQ_LightFalloff(&falloff,128) == 50);
    CHECK(fabsf(LQ_LightFalloff(&falloff,LQ_LightSourceRange(&falloff)) - 2) < 0.001f);
    falloff.formula = LQ_LIGHT_INFINITE;
    CHECK(LQ_LightFalloff(&falloff,10000) == 200 && LQ_LightSourceRange(&falloff) == LQ_LIGHT_MAX_RANGE);
    falloff.formula = LQ_LIGHT_LOCAL_MINLIGHT;
    CHECK(LQ_LightFalloff(&falloff,0) == 0 && LQ_LightSourceRange(&falloff) == 0);
    falloff.formula = LQ_LIGHT_LINEAR; falloff.intensity = 1;
    CHECK(LQ_LightSourceRange(&falloff) == 0);
    puts("PASS linear, inverse, inverse-square, offset inverse-square and unattenuated falloff ranges");
    { /* A 20-degree downward beam reaches only boxes in or near its cone. */
        const float origin[3] = {0, 0, 0}, down[3] = {0, 0, -1};
        float outer = cosf(20 * (float)M_PI / 180);
        const float below[6] = {-8, -8, -208, 8, 8, -192}, beside[6] = {292, -8, -18, 308, 8, -2};
        const float above[6] = {-8, -8, 192, 8, 8, 208}, edge[6] = {80, -4, -204, 88, 4, -196};
        CHECK(LQ_SpotReachesBox(origin, down, outer, 24, below) && !LQ_SpotReachesBox(origin, down, outer, 24, beside));
        CHECK(!LQ_SpotReachesBox(origin, down, outer, 24, above));
        CHECK(LQ_SpotReachesBox(origin, down, outer, 24, edge)); /* about 11 units outside the cone edge, inside the 24-unit blur */
        CHECK(LQ_SpotReachesBox(origin, down, 0.0f, 24, beside)); /* near-hemisphere beams are not culled */
        puts("PASS spotlight beams cull boxes outside the blurred cone");
    }
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
    CHECK(InViewSelectionTest() == 0);
    CHECK(TurningKeepsSlotsTest() == 0);
    CHECK(SurfaceLightTest() == 0);
    CHECK(BounceRoomTest() == 0);
    CHECK(EffectTest() == 0);
    CHECK(FluorescentTest() == 0);
    puts("PASS renderer scene ownership, invalid geometry rejection and Quake/GPU camera projection"); return 0;
}
