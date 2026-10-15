/* Scene validation and Quake-to-GPU projection. GPL-2.0-or-later. */
#include "scene.h"
#include "scene_fixture.h"
#include "indexed_mips.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
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
    CHECK(interactions[0].light_bits == 1 && interactions[1].light_bits == 0);
    runtime[0].position_radius[3] = 200;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 2);
    CHECK(interactions[0].light_bits == 1 && interactions[1].light_bits == 1);
    runtime[0].position_radius[0] = 200;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 0);
    CHECK(interactions[0].light_bits == 0 && interactions[1].light_bits == 0);
    runtime[0].position_radius[0] = 0; scene.draws[0].flags = LQ_SURFACE_SKY;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 1);
    CHECK(interactions[0].light_bits == 0 && interactions[1].light_bits == 1);
    scene.draws[0].flags = 0;
    scene.light_visibility_words = 1;
    scene.light_visibility = calloc(2,sizeof(*scene.light_visibility));
    CHECK(scene.light_visibility);
    scene.light_visibility[0] = 1; /* draw 0 sees source 0; draw 1 is blocked. */
    runtime[0].source_index = 0;
    CHECK(LQ_BuildLightInteractions(&scene,runtime,1,interactions) == 1);
    CHECK(interactions[0].light_bits == 1 && interactions[1].light_bits == 0);
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
    memset(runtime,0,sizeof(runtime));
    CHECK(LQ_BuildRuntimeLights(&selection,styles,camera,runtime) == LQ_MAX_RUNTIME_LIGHTS);
    CHECK(runtime[0].position_radius[0] == 0);
    CHECK(runtime[LQ_MAX_RUNTIME_LIGHTS-1].position_radius[0] == 310);
    /* Moving the camera admits sources 32/33 and drops 0/1, but every source
     * still selected keeps the slot that owns its shadow cube. */
    camera[0] = 330;
    CHECK(LQ_BuildRuntimeLights(&selection,styles,camera,runtime) == LQ_MAX_RUNTIME_LIGHTS);
    CHECK(runtime[0].source_index == 33 && runtime[0].position_radius[0] == 330 && runtime[1].source_index == 32);
    for (uint32_t i = 2; i < LQ_MAX_RUNTIME_LIGHTS; ++i) CHECK(runtime[i].source_index == i);
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
    puts("PASS renderer scene ownership, invalid geometry rejection and Quake/GPU camera projection"); return 0;
}
