/* GPU buffers, textures, passes and readback. No Quake or VM types.
 * GPL-2.0-or-later. */
#include <SDL3/SDL.h>
#include "gpu_sdl.h"
#include "indexed_mips.h"
#include "pack_images.h"
#include "generated/world_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
/* Tone mapping (tonemap.frag): Luminous light below the shoulder is shown as
 * is; above it, highlights roll off to the ceiling, reached at the white point.
 * These are the defaults; lq_tone_shoulder and lq_tone_white set them live. */
#define LQ_TONE_SHOULDER 0.8f
#define LQ_TONE_WHITE 6.0f
struct lq_gpu {
    SDL_Window *window;
    SDL_GPUDevice *device;
    SDL_GPUGraphicsPipeline *pipeline;
    /* The world renders into a floating-point frame (scene); a tone-mapping
     * pass brings it to color, 8-bit for an SDR swapchain or half float of
     * linear light for macOS extended dynamic range, and the UI is drawn over
     * that. Index 1 of each pair targets the half-float color. */
    SDL_GPUGraphicsPipeline *overlay_pipelines[2], *tonemap_pipelines[2];
    SDL_GPUTexture *scene;
    float gamma, headroom; /* lq_gamma; HDR headroom above SDR white (1 on SDR) */
    float peak; /* lq_hdr_peak: highest light the display really shows; 0 trusts macOS */
    int test_pattern; /* lq_hdr_test: bands at known light levels instead of the scene */
    /* Luminous tone and emission (lq_tone_shoulder, lq_tone_white,
     * lq_emission, lq_sprite_emission; see LQ_GPUTone). */
    float tone_shoulder, tone_white, emission, sprite_emission;
    int hdr_request, hdr_active;
    SDL_GPUPresentMode present_mode;
    SDL_GPUGraphicsPipeline *multisample[2];
    SDL_GPUGraphicsPipeline *liquid[3]; /* translucent liquids: 1x, 2x, 4x samples */
    SDL_GPUGraphicsPipeline *smoke[3]; /* smoke puffs blended by their alpha: 1x, 2x, 4x */
    SDL_GPUTexture *multisample_color;
    int smooth, samples, mipmaps, anisotropy;
    uint8_t *submerged; /* per scene draw: the face lies inside a liquid volume */
    float (*planes)[4]; /* per scene draw: face normal and distance; zero for an empty draw */
    float (*draw_bounds)[6]; /* per scene draw: min xyz, max xyz */
    lq_mip_palette *mip_palette;
    SDL_GPUSampler *repeat, *point, *linear;
    SDL_GPUBuffer *vertices, *indices;
    SDL_GPUTexture **textures, *colormap, *lightmap, *bounce, *color, *depth, *overlay;
    /* Community texture packs: per world material, its colour (glow in alpha)
     * and normal map, mipmapped; placeholders where a material has none. */
    SDL_GPUTexture **pack_color, **pack_normal, *pack_blank_color, *pack_flat_normal;
    float *pack_gain, *pack_height; /* per world material: lq_material.pack_gain and pack_height */
    SDL_GPUSampler *pack_sampler;
    /* A skybox's six faces side by side (LQ_GPUSkybox), bound in the pack
     * colour slot for sky surfaces, which take no pack images. */
    SDL_GPUTexture *skybox;
    /* The overlay texture and its staging buffer persist and are rewritten
     * each frame; the copy is recorded in the frame's own command buffer. */
    SDL_GPUTransferBuffer *overlay_transfer;
    uint32_t overlay_width, overlay_height, overlay_transfer_bytes;
    int overlay_pending;
    float bounce_ratio; /* lightmap rows per bounce layer row */
    float sky_light[3]; /* sky brightness at full visibility (bounce layer 0 alpha) */
    float sun_direction[4], sun_light[4]; /* travel direction, angle scale; light at full visibility (layer 1 alpha) */
    /* Luminous irradiance probes: static layers per style, and a dynamic layer
     * rewritten each frame around gameplay lights. */
    SDL_GPUTexture *probe_static, *probe_dynamic;
    SDL_GPUTransferBuffer *probe_transfer;
    uint32_t probe_dynamic_bytes;
    float probe_origin[4], probe_size[3];
    uint8_t probe_styles[4];
    int probe_dynamic_active;
    uint32_t texture_count, draw_count, width, height;
    lq_draw *draws;
    SDL_GPUBuffer *frame_vertices, *frame_indices;
    SDL_GPUTransferBuffer *frame_transfer;
    SDL_GPUTexture *frame_textures[4096];
    lq_draw *frame_draws;
    uint32_t frame_vertex_bytes, frame_index_bytes, frame_transfer_bytes;
    uint32_t frame_draw_count, frame_draw_capacity;
    float (*frame_bounds)[6]; /* min xyz, max xyz of each frame draw */
    /* Luminous shadows: one reversed-depth cube per runtime-light slot, in the
     * slot's array (LQ_ShadowSlot). A slot is redrawn only when its
     * light changes; static world casters persist. */
    SDL_GPUGraphicsPipeline *shadow_pipeline;
    SDL_GPUTexture *shadow_maps[LQ_SHADOW_ARRAYS], *shadow_dummy;
    /* The same cubes with static casters only: a face that only moving casters
     * change is copied from here and the movers drawn over it. */
    SDL_GPUTexture *shadow_static[LQ_SHADOW_ARRAYS];
    SDL_GPUSampler *shadow_sampler, *depth_sampler;
    SDL_GPUBuffer *casters; /* static world casters, then brush models at compiled positions */
    uint32_t caster_count, caster_world_count, shadow_updates, shadow_face_updates;
    uint32_t submitted_faces; /* world faces the last main pass drew */
    uint32_t known_lead_faces; /* of those, faces whose brightest light was known in advance */
    /* The static casters by surface, with bounds: a cube face draws only those
     * in the light's range and its frustum, never the whole map. */
    struct lq_caster_range { float min[3], max[3]; uint32_t first, count; } *caster_ranges;
    uint32_t caster_range_count, caster_range_world_count;
    uint8_t *caster_range_faces; /* per range: cube faces it reaches for the light being rebuilt */
    /* Moving casters this frame (actors, brush models): bounds of frame draws. */
    struct lq_caster { float min[3], max[3]; uint32_t draw, rest; uint8_t moved; } *frame_casters;
    uint32_t frame_caster_count, frame_caster_capacity;
    /* Last frame's caster bounds: a caster found there unchanged is at rest. */
    float (*rest_bounds)[6], (*rest_scratch)[6];
    uint32_t rest_count, rest_capacity;
    /* Slots a moving caster this frame lies within reach of: their
     * shadow-free faces (LQ_SceneFindShadowFree) look their shadow up after all. */
    uint32_t movers[LQ_LIGHT_WORDS];
    uint8_t shadow_dynamic_faces[LQ_MAX_RUNTIME_LIGHTS]; /* faces last drawn with moving casters */
    uint8_t shadow_stale[LQ_MAX_RUNTIME_LIGHTS]; /* faces moving casters touched while the light was offscreen */
    uint8_t shadow_brushes[LQ_MAX_RUNTIME_LIGHTS]; /* static_brush_shadows used for the cached cube */
    float shadow_keys[LQ_MAX_RUNTIME_LIGHTS][4];
    uint32_t shadow_sources[LQ_MAX_RUNTIME_LIGHTS];
    uint8_t shadow_valid[LQ_MAX_RUNTIME_LIGHTS];
    SDL_GPUComputePipeline *bounce_pipeline; /* Luminous bounce rays; made on first use */
    /* Luminous dynamic probes traced on the GPU: the map's BSP, faces and probe
     * validity stay resident; each frame's lights and boxes wait in
     * probe_uniforms until the render writes probe_dynamic before drawing. */
    SDL_GPUComputePipeline *probe_pipeline;
    SDL_GPUBuffer *probe_scene;
    struct lq_probe_uniforms {
        uint32_t tree[4], tables[4]; float grid[4];
        uint32_t size[4], box_min[4], box_size[4], dispatch_min[4], dispatch_size[4];
        float light_position[LQ_MAX_DYNAMIC_LIGHTS][4], light_color[LQ_MAX_DYNAMIC_LIGHTS][4];
        float light_falloff[LQ_MAX_DYNAMIC_LIGHTS][4];
    } probe_uniforms;
    uint32_t probe_written_min[3], probe_written_size[3]; /* last traced box, cleared next time */
    int probe_storage, probe_pending;
    int claimed;
};
typedef struct {
    float lights[4], parameters[4], camera[4], dynamic[32][4];
    uint32_t bits[4];
    float bounce[4]; /* live style value per bounce layer */
    float face_light[4]; /* x: 1 + the slot that is brightest everywhere on the face; 0 unknown */
    uint32_t shadow_free[4]; /* slots whose shadow lookup on this face cannot find a blocker */
    float pack[4]; /* x: draw the texture pack's colour; y: shade by its normal map; z: colour gain */
    float relief[4]; /* x: parallax depth (0 off); y: the height held at the face */
    float entity[4]; /* x: a translucent entity's opacity (LQ_SURFACE_TRANSLUCENT) */
} lq_surface_uniforms;
typedef struct {
    float position_range[LQ_MAX_RUNTIME_LIGHTS][4], color_strength[LQ_MAX_RUNTIME_LIGHTS][4];
    float falloff[LQ_MAX_RUNTIME_LIGHTS][4], spot[LQ_MAX_RUNTIME_LIGHTS][4];
    float shape[LQ_MAX_RUNTIME_LIGHTS][4]; /* inner cosine, source radius, submerged, shadow array*1024 + cube */
    uint32_t parameters[4];
    float tuning[4]; /* exposure, bounce layer ratio, bounce enabled, bounce gain */
    float probe_origin[4]; /* xyz, spacing */
    float probe_size[4]; /* probes per axis; w: 1 static, 2 dynamic */
    float probe_weights[4]; /* live style per static layer */
    float liquid[4]; /* translucent liquid opacity; zero when liquids are opaque or unlit */
    float sky[4]; /* sky brightness at full visibility; zero without baked sky visibility */
    float sun_direction[4]; /* xyz: direction sunlight travels; w: incidence angle scale */
    float sun_light[4]; /* rgb: sunlight at normal incidence and full visibility; zero without a sun */
    float shadow_sizes[4]; /* cube face size per shadow array */
    float color_space[4]; /* x: exponent from summed brightness to linear light (Luminous) */
    float shadow_filter[4]; /* x: a light's brightness at which its shadow takes every sample; 0 always */
    float limit[LQ_MAX_RUNTIME_LIGHTS][4]; /* x: most brightness the light adds to a surface; 0 none */
    float effects[4]; /* x: sprite fullbright emission; y: Quake's palette gamma, for pack colours */
    float palette_shift[4]; /* screen tints on pack colours: colour * (1 - w) + rgb */
    float fog[4]; /* rgb: fog colour 0-1; w: density per unit (FitzQuake's / 64), 0 none */
    float sky_fog[4]; /* x: share of fog over the sky; y: 1 when a skybox replaces the sky texture */
} lq_light_uniforms;
typedef struct { float face_projection[16]; } lq_shadow_uniforms;
#define LQ_SHADOW_NEAR 2.0f
/* How far (in units) a caster's bounds may drift from where its shadow was
 * last drawn before it counts as moving. */
#define LQ_REST_DRIFT 2.0f
static int NearBounds(const float a[6], const float b[6])
{
    for (int k = 0; k < 6; ++k) if (fabsf(a[k] - b[k]) > LQ_REST_DRIFT) return 0;
    return 1;
}
static int Error(char *error, size_t size, const char *what)
{ snprintf(error, size, "%s: %s", what, SDL_GetError()); return 0; }
static SDL_GPUShader *ShaderCode(lq_gpu *g, const unsigned char *spv, size_t spv_size,
    const unsigned char *msl, size_t msl_size, int fragment, uint32_t samplers, uint32_t uniforms)
{
    int metal = (SDL_GetGPUShaderFormats(g->device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
    SDL_GPUShaderCreateInfo info = {0};
    info.code = metal ? msl : spv; info.code_size = metal ? msl_size : spv_size;
    info.entrypoint = metal ? "main0" : "main";
    info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
    info.num_samplers = samplers; info.num_uniform_buffers = uniforms;
    return SDL_CreateGPUShader(g->device, &info);
}
#define SHADER(g, name, fragment, samplers, uniforms) ShaderCode(g, name##_spv, sizeof(name##_spv), \
    name##_msl, sizeof(name##_msl), fragment, samplers, uniforms)
static SDL_GPUShader *Shader(lq_gpu *g, int fragment, int overlay)
{
    if (overlay == 2) return SHADER(g, lq_tonemap_frag, 1, 1, 1);
    if (overlay) return fragment ? SHADER(g, lq_overlay_frag, 1, 1, 1) : SHADER(g, lq_overlay_vert, 0, 0, 0);
    return fragment ? SHADER(g, lq_world_frag, 1, 16, 2) : SHADER(g, lq_world_vert, 0, 0, 1);
}
/* Shadow cubes are six layers each of a 2D array texture; the shader picks
 * the face (see faceTap in world.frag). SDL 3.4's Metal backend passes a cube
 * array's layer count as MTLTextureDescriptor.arrayLength, which Metal reads
 * as a number of cubes, so cube arrays would be allocated six times over and
 * capped at 56 real cubes. 2D arrays size identically on every backend. */
static SDL_GPUTexture *CubeArray(lq_gpu *g, uint32_t size, uint32_t cubes)
{
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY; info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = info.height = size; info.layer_count_or_depth = 6 * cubes; info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(g->device, &info);
}
/* Depth-only cube-face pass for the sealed BSP world (see shadow.frag). */
static int ShadowPipeline(lq_gpu *g)
{
    SDL_GPUVertexBufferDescription buffer = {0, sizeof(lq_vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(lq_vertex, position)}
    };
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    info.vertex_shader = SHADER(g, lq_shadow_vert, 0, 0, 1);
    info.fragment_shader = SHADER(g, lq_shadow_frag, 1, 0, 0);
    if (info.vertex_shader && info.fragment_shader) {
        info.vertex_input_state = (SDL_GPUVertexInputState){&buffer, 1, attributes, 1};
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.depth_stencil_state.enable_depth_test = info.depth_stencil_state.enable_depth_write = true;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        g->shadow_pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    }
    if (info.vertex_shader) SDL_ReleaseGPUShader(g->device, info.vertex_shader);
    if (info.fragment_shader) SDL_ReleaseGPUShader(g->device, info.fragment_shader);
    SDL_GPUSamplerCreateInfo sampler = {0};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.enable_compare = true; sampler.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    g->shadow_sampler = SDL_CreateGPUSampler(g->device, &sampler);
    /* Raw nearest depth for the soft-shadow blocker search. */
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST; sampler.enable_compare = false;
    g->depth_sampler = SDL_CreateGPUSampler(g->device, &sampler);
    /* Classic draws never sample shadows but the shader binding must exist. */
    g->shadow_dummy = CubeArray(g, 1, 1);
    return g->shadow_pipeline && g->shadow_sampler && g->depth_sampler && g->shadow_dummy;
}
static SDL_GPUSampler *Sampler(lq_gpu *g, SDL_GPUFilter filter, SDL_GPUSamplerAddressMode mode)
{
    SDL_GPUSamplerCreateInfo info = {0};
    info.min_filter = info.mag_filter = filter; info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = info.address_mode_v = info.address_mode_w = mode; info.max_lod = 0;
    return SDL_CreateGPUSampler(g->device, &info);
}
lq_gpu *LQ_GPUCreate(SDL_Window *window, int debug, char *error, size_t size)
{
    lq_gpu *g = calloc(1, sizeof(*g));
    SDL_GPUShader *vertex = NULL, *fragment = NULL;
    SDL_GPUColorTargetDescription color = {0};
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    SDL_GPUVertexBufferDescription buffer = {0, sizeof(lq_vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(lq_vertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(lq_vertex, uv)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(lq_vertex, light_uv)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(lq_vertex, shade)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(lq_vertex, normal)}
    };
    if (!g) { SDL_SetError("out of memory"); Error(error, size, "GPU allocation"); return NULL; }
    g->window = window;
    g->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, debug != 0, NULL);
    if (!g->device) goto failed;
    if (!SDL_ClaimWindowForGPUDevice(g->device, window)) goto failed;
    g->claimed = 1;
    if (!SDL_SetGPUSwapchainParameters(g->device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC)) goto failed;
    g->present_mode = SDL_GPU_PRESENTMODE_VSYNC; g->gamma = 2.2f; g->headroom = 1; g->hdr_request = 1;
    g->tone_shoulder = LQ_TONE_SHOULDER; g->tone_white = LQ_TONE_WHITE;
    g->emission = g->sprite_emission = LQ_EMISSION_GAIN;
    SDL_Log("LuminousQuake GPU driver: %s", SDL_GetGPUDeviceDriver(g->device));
    vertex = Shader(g, 0, 0); fragment = Shader(g, 1, 0);
    if (!vertex || !fragment) goto failed;
    info.vertex_shader = vertex; info.fragment_shader = fragment;
    info.vertex_input_state = (SDL_GPUVertexInputState){&buffer, 1, attributes, 5};
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = info.depth_stencil_state.enable_depth_write = true;
    color.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    info.target_info = (SDL_GPUGraphicsPipelineTargetInfo){&color, 1, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, true, 0, 0, 0};
    g->pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    for (int i = 0; i < 2; ++i) {
        SDL_GPUSampleCount samples = i ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2;
        if (SDL_GPUTextureSupportsSampleCount(g->device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, samples) &&
            SDL_GPUTextureSupportsSampleCount(g->device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, samples)) {
            info.multisample_state.sample_count = samples;
            g->multisample[i] = SDL_CreateGPUGraphicsPipeline(g->device, &info);
        }
    }
    /* Liquids blend over what is behind them by a constant opacity and leave
     * depth unwritten, so a surface seen through water is not hidden by it. */
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    color.blend_state.dst_color_blendfactor = color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    color.blend_state.color_blend_op = color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    info.depth_stencil_state.enable_depth_write = false;
    for (int i = 0; i < 3; ++i) if (!i || g->multisample[i - 1]) {
        info.multisample_state.sample_count = i == 2 ? SDL_GPU_SAMPLECOUNT_4 : i ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
        g->liquid[i] = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    }
    /* Smoke blends by its own alpha, over everything solid, depth unwritten. */
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    for (int i = 0; i < 3; ++i) if (!i || g->multisample[i - 1]) {
        info.multisample_state.sample_count = i == 2 ? SDL_GPU_SAMPLECOUNT_4 : i ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
        g->smoke[i] = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    }
    color = (SDL_GPUColorTargetDescription){0}; color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_ReleaseGPUShader(g->device, vertex); SDL_ReleaseGPUShader(g->device, fragment);
    vertex = fragment = NULL;
    vertex = Shader(g, 0, 1); fragment = Shader(g, 1, 1);
    if (!vertex || !fragment) goto failed;
    memset(&info, 0, sizeof(info));
    info.vertex_shader = vertex; info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    for (int f = 0; f < 2; ++f) {
        color.format = f ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        g->overlay_pipelines[f] = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    }
    SDL_ReleaseGPUShader(g->device, fragment);
    fragment = Shader(g, 1, 2);
    if (!fragment) goto failed;
    info.fragment_shader = fragment;
    color.blend_state = (SDL_GPUColorTargetBlendState){0};
    for (int f = 0; f < 2; ++f) {
        color.format = f ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        g->tonemap_pipelines[f] = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    }
    SDL_ReleaseGPUShader(g->device, vertex); SDL_ReleaseGPUShader(g->device, fragment);
    vertex = fragment = NULL;
    g->repeat = Sampler(g, SDL_GPU_FILTER_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_REPEAT);
    g->point = Sampler(g, SDL_GPU_FILTER_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    g->linear = Sampler(g, SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    if (!g->pipeline || !g->liquid[0] || !g->overlay_pipelines[0] || !g->overlay_pipelines[1] ||
        !g->tonemap_pipelines[0] || !g->tonemap_pipelines[1] || !g->repeat || !g->point || !g->linear) goto failed;
    if (!SDL_GPUTextureSupportsFormat(g->device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D_ARRAY,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        SDL_SetError("depth array textures are unsupported"); goto failed;
    }
    if (!ShadowPipeline(g)) goto failed;
    return g;
failed:
    Error(error, size, "GPU initialization");
    if (vertex) SDL_ReleaseGPUShader(g->device, vertex);
    if (fragment) SDL_ReleaseGPUShader(g->device, fragment);
    LQ_GPUDestroy(g); return NULL;
}
static SDL_GPUTexture *Texture(lq_gpu *g, uint32_t w, uint32_t h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage)
{
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.format = format; info.usage = usage;
    info.width = w; info.height = h; info.layer_count_or_depth = info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(g->device, &info);
}
int LQ_GPUOptions(lq_gpu *g, int smooth, int samples, int vsync, char *error, size_t size)
{
    if ((samples != 0 && samples != 2 && samples != 4) ||
        (samples && !g->multisample[samples == 4])) {
        SDL_SetError("requested anti-aliasing is unsupported"); return Error(error, size, "GPU options");
    }
    SDL_GPUPresentMode mode = vsync ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
    if (!SDL_WindowSupportsGPUPresentMode(g->device, g->window, mode)) {
        SDL_SetError("requested presentation mode is unsupported"); return Error(error, size, "GPU options");
    }
    if (!SDL_SetGPUSwapchainParameters(g->device, g->window,
            g->hdr_active ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR : SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
        return Error(error, size, "GPU options");
    g->present_mode = mode;
    g->smooth = smooth != 0;
    if (g->samples != samples) { g->samples = samples; g->width = 0; }
    return 1;
}
int LQ_GPUTextureOptions(lq_gpu *g, int mipmaps, int anisotropy, char *error, size_t size)
{
    if (mipmaps < 0 || mipmaps > 2 ||
        (anisotropy != 1 && anisotropy != 2 && anisotropy != 4 && anisotropy != 8 && anisotropy != 16)) {
        SDL_SetError("invalid mipmap/anisotropic setting"); return Error(error, size, "GPU texture options");
    }
    g->mipmaps = mipmaps; g->anisotropy = anisotropy; return 1;
}
int LQ_GPUSamplesSupported(lq_gpu *g, int samples)
{ return samples == 0 || (samples == 2 && g->multisample[0]) || (samples == 4 && g->multisample[1]); }
static SDL_GPUTransferBuffer *Transfer(lq_gpu *g, uint32_t bytes, SDL_GPUTransferBufferUsage usage, const void *data)
{
    SDL_GPUTransferBufferCreateInfo info = {usage, bytes, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(g->device, &info);
    if (transfer && data) {
        void *mapped = SDL_MapGPUTransferBuffer(g->device, transfer, false);
        if (!mapped) { SDL_ReleaseGPUTransferBuffer(g->device, transfer); return NULL; }
        memcpy(mapped, data, bytes); SDL_UnmapGPUTransferBuffer(g->device, transfer);
    }
    return transfer;
}
static int UploadProbeScene(lq_gpu *g, const lq_scene *s);
static void ProbeCompute(lq_gpu *g, SDL_GPUCommandBuffer *command);
static SDL_GPUTexture *UploadImage(lq_gpu *g, const lq_image *image)
{
    SDL_GPUTexture *texture = Texture(g, image->width, image->height,
        image->channels == 1 ? SDL_GPU_TEXTUREFORMAT_R8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTransferBuffer *transfer = Transfer(g, image->width * image->height * image->channels,
        SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, image->pixels);
    SDL_GPUCommandBuffer *command = NULL;
    if (texture && transfer) command = SDL_AcquireGPUCommandBuffer(g->device);
    if (command) {
        SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
        SDL_GPUTextureTransferInfo source = {transfer, 0, image->width, image->height};
        SDL_GPUTextureRegion dest = {0};
        dest.texture = texture; dest.w = image->width; dest.h = image->height; dest.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &dest, false); SDL_EndGPUCopyPass(pass);
        if (!SDL_SubmitGPUCommandBuffer(command)) { SDL_ReleaseGPUTexture(g->device, texture); texture = NULL; }
    } else if (texture) { SDL_ReleaseGPUTexture(g->device, texture); texture = NULL; }
    if (transfer) SDL_ReleaseGPUTransferBuffer(g->device, transfer);
    return texture;
}
/* An RGBA8 pack image with its whole mip chain. The levels are made here, in
 * linear light for colour and renormalized for normal maps (LQ_PackHalve):
 * averaging encoded values, as a GPU blit does, darkens distant texels. */
static SDL_GPUTexture *UploadMipmapped(lq_gpu *g, const lq_image *image, int normal_map)
{
    uint32_t levels = 1;
    while ((image->width >> levels) || (image->height >> levels)) ++levels;
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = image->width; info.height = image->height; info.layer_count_or_depth = 1;
    info.num_levels = levels; info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(g->device, &info);
    if (!texture) return NULL;
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(g->device);
    SDL_GPUTransferBuffer *transfers[16] = {0};
    int ok = command != NULL;
    SDL_GPUCopyPass *pass = ok ? SDL_BeginGPUCopyPass(command) : NULL;
    lq_image level = *image, next = {0};
    for (uint32_t l = 0; ok && l < levels && l < 16; ++l) {
        transfers[l] = Transfer(g, level.width * level.height * 4, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, level.pixels);
        if (!transfers[l]) { ok = 0; break; }
        SDL_GPUTextureTransferInfo source = {transfers[l], 0, level.width, level.height};
        SDL_GPUTextureRegion dest = {0};
        dest.texture = texture; dest.mip_level = l; dest.w = level.width; dest.h = level.height; dest.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &dest, false);
        if (l + 1 < levels) {
            if (!LQ_PackHalve(&level, normal_map, &next)) { ok = 0; break; }
            if (level.pixels != image->pixels) free(level.pixels);
            level = next;
        }
    }
    if (level.pixels != image->pixels) free(level.pixels);
    if (pass) SDL_EndGPUCopyPass(pass);
    if (command) {
        if (ok) ok = SDL_SubmitGPUCommandBuffer(command);
        else SDL_CancelGPUCommandBuffer(command);
    }
    for (int l = 0; l < 16; ++l) if (transfers[l]) SDL_ReleaseGPUTransferBuffer(g->device, transfers[l]);
    if (!ok) { SDL_ReleaseGPUTexture(g->device, texture); return NULL; }
    return texture;
}
static SDL_GPUTexture *Upload3D(lq_gpu *g, uint32_t w, uint32_t h, uint32_t d, const uint8_t *pixels,
    SDL_GPUTextureUsageFlags usage)
{
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_3D; info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | usage; info.width = w; info.height = h;
    info.layer_count_or_depth = d; info.num_levels = 1; info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(g->device, &info);
    uint32_t bytes = w * h * d * 4;
    uint8_t *zeros = pixels ? NULL : calloc(bytes, 1);
    SDL_GPUTransferBuffer *transfer = texture && (pixels || zeros) ?
        Transfer(g, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, pixels ? pixels : zeros) : NULL;
    free(zeros);
    SDL_GPUCommandBuffer *command = transfer ? SDL_AcquireGPUCommandBuffer(g->device) : NULL;
    if (command) {
        SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
        SDL_GPUTextureTransferInfo source = {transfer, 0, w, h};
        SDL_GPUTextureRegion dest = {0};
        dest.texture = texture; dest.w = w; dest.h = h; dest.d = d;
        SDL_UploadToGPUTexture(pass, &source, &dest, false); SDL_EndGPUCopyPass(pass);
        if (!SDL_SubmitGPUCommandBuffer(command)) { SDL_ReleaseGPUTexture(g->device, texture); texture = NULL; }
    } else if (texture) { SDL_ReleaseGPUTexture(g->device, texture); texture = NULL; }
    if (transfer) SDL_ReleaseGPUTransferBuffer(g->device, transfer);
    return texture;
}
/* Indexed textures require color-aware mip generation, not hardware averaging
 * of palette numbers. Shaders filter the resulting palette colors explicitly. */
static SDL_GPUTexture *UploadIndexed(lq_gpu *g, const lq_image *image)
{
    lq_mip_chain mip;
    if (!LQ_IndexedMips(image, g->mip_palette, &mip)) {
        SDL_SetError("cannot build indexed mip chain"); return NULL;
    }
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width = image->width; info.height = image->height;
    info.layer_count_or_depth = 1; info.num_levels = mip.count; info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(g->device, &info);
    /* Tight rows match the original upload path (older SDL Metal versions
     * ignore pixels_per_row). Align each level start for buffer copies. */
    uint32_t offsets[14], pitches[14], bytes = 0;
    for (unsigned n = 0; n < mip.count; ++n) {
        pitches[n] = mip.widths[n]; offsets[n] = (bytes+511)&~511u;
        bytes = offsets[n]+pitches[n]*mip.heights[n];
    }
    uint8_t *data = calloc(1, bytes);
    if (data) for (unsigned n = 0; n < mip.count; ++n)
        for (unsigned y = 0; y < mip.heights[n]; ++y)
            memcpy(data+offsets[n]+y*pitches[n], mip.pixels+mip.offsets[n]+y*mip.widths[n], mip.widths[n]);
    SDL_GPUTransferBuffer *transfer = data ? Transfer(g, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, data) : NULL;
    free(data); free(mip.pixels);
    SDL_GPUCommandBuffer *command = texture && transfer ? SDL_AcquireGPUCommandBuffer(g->device) : NULL;
    if (command) {
        SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
        for (unsigned n = 0; n < mip.count; ++n) {
            SDL_GPUTextureTransferInfo source = {transfer, offsets[n], pitches[n], mip.heights[n]};
            SDL_GPUTextureRegion dest = {0};
            dest.texture = texture; dest.mip_level = n; dest.w = mip.widths[n]; dest.h = mip.heights[n]; dest.d = 1;
            SDL_UploadToGPUTexture(pass, &source, &dest, false);
        }
        SDL_EndGPUCopyPass(pass);
        if (!SDL_SubmitGPUCommandBuffer(command)) { SDL_ReleaseGPUTexture(g->device, texture); texture = NULL; }
    } else if (texture) { SDL_ReleaseGPUTexture(g->device, texture); texture = NULL; }
    if (transfer) SDL_ReleaseGPUTransferBuffer(g->device, transfer);
    return texture;
}
static SDL_GPUBuffer *UploadBuffer(lq_gpu *g, const void *data, uint32_t bytes, SDL_GPUBufferUsageFlags usage)
{
    SDL_GPUBufferCreateInfo info = {usage, bytes, 0};
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(g->device, &info);
    SDL_GPUTransferBuffer *transfer = Transfer(g, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, data);
    SDL_GPUCommandBuffer *command = buffer && transfer ? SDL_AcquireGPUCommandBuffer(g->device) : NULL;
    if (command) {
        SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
        SDL_GPUTransferBufferLocation source = {transfer, 0};
        SDL_GPUBufferRegion dest = {buffer, 0, bytes};
        SDL_UploadToGPUBuffer(pass, &source, &dest, false); SDL_EndGPUCopyPass(pass);
        if (!SDL_SubmitGPUCommandBuffer(command)) { SDL_ReleaseGPUBuffer(g->device, buffer); buffer = NULL; }
    } else if (buffer) { SDL_ReleaseGPUBuffer(g->device, buffer); buffer = NULL; }
    if (transfer) SDL_ReleaseGPUTransferBuffer(g->device, transfer);
    return buffer;
}
/* The overlay is staged here and copied at the start of the next render.
 * Mapping and uploading both cycle, so a frame still in flight keeps the
 * pixels it was given. */
int LQ_GPUOverlay(lq_gpu *g, const lq_image *image, char *error, size_t size)
{
    g->overlay_pending = 0;
    if (!image) {
        if (g->overlay) SDL_ReleaseGPUTexture(g->device, g->overlay);
        g->overlay = NULL; g->overlay_width = g->overlay_height = 0; return 1;
    }
    if (image->channels != 4 || !image->pixels || !image->width || !image->height ||
        image->width > 8192 || image->height > 8192) {
        SDL_SetError("invalid overlay image"); return Error(error, size, "GPU overlay");
    }
    if (!g->overlay || g->overlay_width != image->width || g->overlay_height != image->height) {
        SDL_GPUTexture *next = Texture(g, image->width, image->height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (!next) return Error(error, size, "GPU overlay texture");
        if (g->overlay) SDL_ReleaseGPUTexture(g->device, g->overlay);
        g->overlay = next; g->overlay_width = image->width; g->overlay_height = image->height;
    }
    uint32_t bytes = image->width * image->height * 4;
    if (g->overlay_transfer_bytes < bytes) {
        SDL_GPUTransferBuffer *next = Transfer(g, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, NULL);
        if (!next) return Error(error, size, "GPU overlay transfer");
        if (g->overlay_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->overlay_transfer);
        g->overlay_transfer = next; g->overlay_transfer_bytes = bytes;
    }
    void *mapped = SDL_MapGPUTransferBuffer(g->device, g->overlay_transfer, true);
    if (!mapped) return Error(error, size, "GPU overlay map");
    memcpy(mapped, image->pixels, bytes);
    SDL_UnmapGPUTransferBuffer(g->device, g->overlay_transfer);
    g->overlay_pending = 1; return 1;
}
static void OverlayCopy(lq_gpu *g, SDL_GPUCommandBuffer *command)
{
    if (!g->overlay_pending || !g->overlay) return;
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source = {g->overlay_transfer, 0, g->overlay_width, g->overlay_height};
    SDL_GPUTextureRegion dest = {0};
    dest.texture = g->overlay; dest.w = g->overlay_width; dest.h = g->overlay_height; dest.d = 1;
    SDL_UploadToGPUTexture(pass, &source, &dest, true); SDL_EndGPUCopyPass(pass);
    g->overlay_pending = 0;
}
int LQ_GPUColormap(lq_gpu *g, const lq_image *image, char *error, size_t size)
{
    if (!image || image->channels != 4 || image->width != 256 || image->height != 64 || !image->pixels) {
        SDL_SetError("invalid colormap image"); return Error(error, size, "GPU palette");
    }
    SDL_GPUTexture *next = UploadImage(g, image);
    if (!next) return Error(error, size, "GPU palette upload");
    if (g->colormap) SDL_ReleaseGPUTexture(g->device, g->colormap);
    g->colormap = next; return 1;
}
static void ReleaseScene(lq_gpu *g)
{
    for (unsigned i = 0; i < 4096; ++i) {
        if (g->frame_textures[i]) SDL_ReleaseGPUTexture(g->device, g->frame_textures[i]);
        g->frame_textures[i] = NULL;
    }
    g->frame_draw_count = 0;
    if (g->vertices) SDL_ReleaseGPUBuffer(g->device, g->vertices);
    if (g->indices) SDL_ReleaseGPUBuffer(g->device, g->indices);
    if (g->casters) SDL_ReleaseGPUBuffer(g->device, g->casters);
    g->casters = NULL; g->caster_count = g->caster_world_count = 0;
    free(g->caster_ranges); g->caster_ranges = NULL; g->caster_range_count = g->caster_range_world_count = 0;
    free(g->caster_range_faces); g->caster_range_faces = NULL;
    memset(g->shadow_dynamic_faces, 0, sizeof(g->shadow_dynamic_faces));
    memset(g->shadow_valid, 0, sizeof(g->shadow_valid));
    for (uint32_t i = 0; i < g->texture_count; ++i) {
        if (g->textures[i]) SDL_ReleaseGPUTexture(g->device, g->textures[i]);
        if (g->pack_color && g->pack_color[i]) SDL_ReleaseGPUTexture(g->device, g->pack_color[i]);
        if (g->pack_normal && g->pack_normal[i]) SDL_ReleaseGPUTexture(g->device, g->pack_normal[i]);
    }
    free(g->pack_color); free(g->pack_normal); free(g->pack_gain); free(g->pack_height);
    g->pack_color = g->pack_normal = NULL; g->pack_gain = g->pack_height = NULL;
    if (g->pack_sampler) SDL_ReleaseGPUSampler(g->device, g->pack_sampler);
    g->pack_sampler = NULL;
    if (g->colormap) SDL_ReleaseGPUTexture(g->device, g->colormap);
    if (g->lightmap) SDL_ReleaseGPUTexture(g->device, g->lightmap);
    if (g->bounce) SDL_ReleaseGPUTexture(g->device, g->bounce);
    g->bounce = NULL; g->bounce_ratio = 0; memset(g->sky_light, 0, sizeof(g->sky_light));
    memset(g->sun_direction, 0, sizeof(g->sun_direction)); memset(g->sun_light, 0, sizeof(g->sun_light));
    if (g->probe_static) SDL_ReleaseGPUTexture(g->device, g->probe_static);
    if (g->probe_dynamic) SDL_ReleaseGPUTexture(g->device, g->probe_dynamic);
    if (g->probe_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->probe_transfer);
    g->probe_static = g->probe_dynamic = NULL; g->probe_transfer = NULL;
    g->probe_dynamic_bytes = 0; g->probe_dynamic_active = 0; g->probe_origin[3] = 0;
    if (g->probe_scene) SDL_ReleaseGPUBuffer(g->device, g->probe_scene);
    g->probe_scene = NULL; g->probe_storage = g->probe_pending = 0;
    memset(g->probe_written_size, 0, sizeof(g->probe_written_size));
    free(g->textures); free(g->draws); free(g->submerged); free(g->planes); free(g->draw_bounds);
    g->textures = NULL; g->draws = NULL; g->submerged = NULL; g->planes = NULL; g->draw_bounds = NULL;
    g->vertices = g->indices = NULL;
    g->colormap = g->lightmap = NULL; g->texture_count = g->draw_count = 0;
}
static int FrameBuffer(lq_gpu *g, SDL_GPUBuffer **buffer, uint32_t *capacity,
                       uint32_t bytes, SDL_GPUBufferUsageFlags usage)
{
    if (*capacity >= bytes) return 1;
    uint32_t count = 16384;
    while (count < bytes) count *= 2;
    SDL_GPUBufferCreateInfo info = {usage, count, 0};
    SDL_GPUBuffer *next = SDL_CreateGPUBuffer(g->device, &info);
    if (!next) return 0;
    if (*buffer) SDL_ReleaseGPUBuffer(g->device, *buffer);
    *buffer = next; *capacity = count; return 1;
}
int LQ_GPUFrame(lq_gpu *g, const lq_frame *f, char *error, size_t size)
{
    g->frame_draw_count = 0; g->frame_caster_count = 0;
    if (!f || !f->draw_count) return 1;
    if (!f->vertices || !f->indices || !f->draws || !f->materials ||
        !f->vertex_count || f->vertex_count > 2000000 || !f->index_count ||
        f->index_count > 6000000 || f->draw_count > 65536 || f->material_count > 4096) {
        SDL_SetError("invalid frame geometry"); return Error(error, size, "GPU frame");
    }
    for (uint32_t i = 0; i < f->index_count; ++i) if (f->indices[i] >= f->vertex_count) {
        SDL_SetError("frame index outside vertices"); return Error(error, size, "GPU frame");
    }
    for (uint32_t i = 0; i < f->vertex_count; ++i) {
        const lq_vertex *v = &f->vertices[i];
        if (!isfinite(v->shade) || !isfinite(v->position[0]) || !isfinite(v->position[1]) ||
            !isfinite(v->position[2]) || !isfinite(v->uv[0]) || !isfinite(v->uv[1]) ||
            !isfinite(v->light_uv[0]) || !isfinite(v->light_uv[1]) ||
            !isfinite(v->normal[0]) || !isfinite(v->normal[1]) || !isfinite(v->normal[2])) {
            SDL_SetError("nonfinite frame vertex"); return Error(error,size,"GPU frame");
        }
    }
    for (uint32_t i = 0; i < f->material_count; ++i) {
        const lq_material *m = &f->materials[i];
        if (!m->resource_id || m->resource_id >= 4096 || !m->indices.pixels ||
            !m->indices.width || !m->indices.height || m->indices.width > 8192 ||
            m->indices.height > 8192 || m->indices.channels != 1) {
            SDL_SetError("invalid frame material"); return Error(error, size, "GPU frame");
        }
        if (!g->frame_textures[m->resource_id]) {
            g->frame_textures[m->resource_id] = UploadIndexed(g, &m->indices);
            if (!g->frame_textures[m->resource_id]) return Error(error, size, "GPU frame texture");
        }
    }
    if (g->frame_draw_capacity < f->draw_count) {
        lq_draw *next = realloc(g->frame_draws, f->draw_count * sizeof(*next));
        if (!next) { SDL_SetError("out of memory"); return Error(error, size, "GPU frame draws"); }
        g->frame_draws = next; g->frame_draw_capacity = f->draw_count;
    }
    if (g->frame_caster_capacity < f->draw_count) {
        struct lq_caster *next = realloc(g->frame_casters, f->draw_count * sizeof(*next));
        if (!next) { SDL_SetError("out of memory"); return Error(error, size, "GPU frame casters"); }
        g->frame_casters = next;
        float (*bounds)[6] = realloc(g->frame_bounds, f->draw_count * sizeof(*bounds));
        if (!bounds) { SDL_SetError("out of memory"); return Error(error, size, "GPU frame bounds"); }
        g->frame_bounds = bounds; g->frame_caster_capacity = f->draw_count;
    }
    g->frame_caster_count = 0;
    uint32_t rest_shift = 0; /* last frame's index minus this frame's, at the last match */
    for (uint32_t i = 0; i < f->draw_count; ++i) {
        lq_draw d = f->draws[i];
        if (d.flags & ~(LQ_SURFACE_SKY|LQ_SURFACE_WATER|LQ_SURFACE_MODEL|LQ_SURFACE_MASKED|LQ_SURFACE_WEAPON|LQ_SURFACE_ACTOR|LQ_SURFACE_DIFFUSE_EFFECT|LQ_SURFACE_SOLID_EFFECT|LQ_SURFACE_NOSHADOW|LQ_SURFACE_ERODE|LQ_SURFACE_SPRITE|
                LQ_SURFACE_SUBMERGED|LQ_SURFACE_TRANSLUCENT) || !(d.alpha >= 0 && d.alpha <= 1) ||
            d.material >= f->material_count || d.first_index > f->index_count ||
            d.index_count > f->index_count - d.first_index || !d.index_count || d.index_count % 3) {
            SDL_SetError("invalid frame triangle range"); return Error(error, size, "GPU frame draws");
        }
        d.material = f->materials[d.material].resource_id; g->frame_draws[i] = d;
        float *bounds = g->frame_bounds[i];
        for (int k = 0; k < 3; ++k) { bounds[k] = INFINITY; bounds[k+3] = -INFINITY; }
        for (uint32_t j = 0; j < d.index_count; ++j) {
            const float *p = f->vertices[f->indices[d.first_index + j]].position;
            for (int k = 0; k < 3; ++k) { bounds[k] = fminf(bounds[k], p[k]); bounds[k+3] = fmaxf(bounds[k+3], p[k]); }
        }
        /* Actors and moving brush faces cast; the view weapon, particles,
         * sprites, flames, liquids and translucent entities do not. */
        int brush = !(d.flags & LQ_SURFACE_MODEL), actor = (d.flags & LQ_SURFACE_ACTOR) != 0;
        if ((brush || actor) && !(d.flags & (LQ_SURFACE_WEAPON|LQ_SURFACE_MASKED|LQ_SURFACE_SKY|LQ_SURFACE_WATER|LQ_SURFACE_DIFFUSE_EFFECT|LQ_SURFACE_SOLID_EFFECT|LQ_SURFACE_NOSHADOW|LQ_SURFACE_TRANSLUCENT))) {
            struct lq_caster *c = &g->frame_casters[g->frame_caster_count++];
            c->draw = i;
            memcpy(c->min, g->frame_bounds[i], sizeof(c->min)); memcpy(c->max, g->frame_bounds[i] + 3, sizeof(c->max));
            c->moved = 1; c->rest = UINT32_MAX;
            /* Casters arrive in much the same order each frame: try the entry
             * that follows the last match first, and search only when it fails. */
            uint32_t n = g->frame_caster_count - 1, guess = n + rest_shift;
            if (guess < g->rest_count && NearBounds(g->rest_bounds[guess], g->frame_bounds[i])) {
                c->moved = 0; c->rest = guess;
            } else for (uint32_t j = 0; j < g->rest_count; ++j)
                if (NearBounds(g->rest_bounds[j], g->frame_bounds[i])) {
                    c->moved = 0; c->rest = j; rest_shift = j - n; break;
                }
        }
    }
    /* Remember where each caster's shadow was last drawn for the next frame's
     * rest test. A caster at rest keeps that position, so an idle monster's
     * blended poses (which shift its bounds a little every frame) redraw its
     * shadows only once they have drifted LQ_REST_DRIFT units, not every frame;
     * a moving one is recorded where it is now. */
    if (g->rest_capacity < g->frame_caster_count) {
        float (*next)[6] = realloc(g->rest_bounds, g->frame_caster_count * sizeof(*next));
        float (*scratch)[6] = realloc(g->rest_scratch, g->frame_caster_count * sizeof(*scratch));
        if (next) g->rest_bounds = next;
        if (scratch) g->rest_scratch = scratch;
        if (!next || !scratch) { SDL_SetError("out of memory"); return Error(error, size, "GPU rest bounds"); }
        g->rest_capacity = g->frame_caster_count;
    }
    for (uint32_t i = 0; i < g->frame_caster_count; ++i) {
        const struct lq_caster *c = &g->frame_casters[i];
        memcpy(g->rest_scratch[i], c->rest != UINT32_MAX ? g->rest_bounds[c->rest] : g->frame_bounds[c->draw],
            sizeof(g->rest_scratch[i]));
    }
    if (g->frame_caster_count)
        memcpy(g->rest_bounds, g->rest_scratch, g->frame_caster_count * sizeof(*g->rest_bounds));
    g->rest_count = g->frame_caster_count;
    uint32_t vb = f->vertex_count * sizeof(lq_vertex), ib = f->index_count * sizeof(uint32_t);
    if (!FrameBuffer(g, &g->frame_vertices, &g->frame_vertex_bytes, vb, SDL_GPU_BUFFERUSAGE_VERTEX) ||
        !FrameBuffer(g, &g->frame_indices, &g->frame_index_bytes, ib, SDL_GPU_BUFFERUSAGE_INDEX))
        return Error(error, size, "GPU frame buffers");
    if (g->frame_transfer_bytes < vb+ib) {
        SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vb+ib, 0};
        SDL_GPUTransferBuffer *next = SDL_CreateGPUTransferBuffer(g->device, &info);
        if (!next) return Error(error, size, "GPU frame transfer");
        if (g->frame_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->frame_transfer);
        g->frame_transfer = next; g->frame_transfer_bytes = vb+ib;
    }
    uint8_t *mapped = SDL_MapGPUTransferBuffer(g->device, g->frame_transfer, true);
    if (!mapped) return Error(error, size, "GPU frame map");
    memcpy(mapped, f->vertices, vb); memcpy(mapped+vb, f->indices, ib);
    SDL_UnmapGPUTransferBuffer(g->device, g->frame_transfer);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(g->device);
    if (!command) return Error(error, size, "GPU frame command");
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation source = {g->frame_transfer, 0};
    SDL_GPUBufferRegion destination = {g->frame_vertices, 0, vb};
    SDL_UploadToGPUBuffer(pass, &source, &destination, true);
    source.offset = vb; destination = (SDL_GPUBufferRegion){g->frame_indices, 0, ib};
    SDL_UploadToGPUBuffer(pass, &source, &destination, true);
    SDL_EndGPUCopyPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(command)) return Error(error, size, "GPU frame submit");
    g->frame_draw_count = f->draw_count; return 1;
}
int LQ_GPUUpload(lq_gpu *g, const lq_scene *s, char *error, size_t size)
{
    if (!LQ_SceneCheck(s, error, size)) return 0;
    if (!SDL_WaitForGPUIdle(g->device)) return Error(error, size, "GPU wait before scene upload");
    ReleaseScene(g);
    g->textures = calloc(s->material_count, sizeof(*g->textures));
    g->draws = malloc((size_t)s->draw_count * sizeof(*g->draws));
    if (!g->textures || !g->draws) { SDL_SetError("out of memory"); goto failed; }
    if (!g->mip_palette) g->mip_palette = malloc(sizeof(*g->mip_palette));
    if (!g->mip_palette) { SDL_SetError("out of memory for mip palette"); goto failed; }
    LQ_MipPaletteInit(g->mip_palette, s->colormap.pixels);
    g->texture_count = s->material_count; g->draw_count = s->draw_count;
    memcpy(g->draws, s->draws, (size_t)s->draw_count * sizeof(*g->draws));
    /* A point just in front of each lit face tells whether it lies under a liquid. */
    g->submerged = calloc(s->draw_count ? s->draw_count : 1, 1);
    if (!g->submerged) { SDL_SetError("out of memory"); goto failed; }
    /* Each world draw is one planar BSP face. Its plane rejects it when the
     * camera is behind it, as Quake does; liquid faces come in coplanar
     * front/back pairs, so this also keeps blending from picking one per face
     * at random. Its bounds reject it when it lies outside the view. */
    g->planes = calloc(s->draw_count ? s->draw_count : 1, sizeof(*g->planes));
    g->draw_bounds = calloc(s->draw_count ? s->draw_count : 1, sizeof(*g->draw_bounds));
    if (!g->planes || !g->draw_bounds) { SDL_SetError("out of memory"); goto failed; }
    for (uint32_t i = 0; i < s->draw_count; ++i) {
        const lq_draw *d = &s->draws[i];
        if (!d->index_count) continue;
        const lq_vertex *v = &s->vertices[s->indices[d->first_index]];
        memcpy(g->planes[i], v->normal, sizeof(v->normal));
        g->planes[i][3] = v->normal[0]*v->position[0] + v->normal[1]*v->position[1] + v->normal[2]*v->position[2];
        float *bounds = g->draw_bounds[i];
        for (int k = 0; k < 3; ++k) { bounds[k] = INFINITY; bounds[k+3] = -INFINITY; }
        for (uint32_t j = 0; j < d->index_count; ++j) {
            const float *p = s->vertices[s->indices[d->first_index + j]].position;
            for (int k = 0; k < 3; ++k) { bounds[k] = fminf(bounds[k], p[k]); bounds[k+3] = fmaxf(bounds[k+3], p[k]); }
        }
    }
    for (uint32_t i = 0; i < s->draw_count && s->leaf_contents; ++i) {
        const lq_draw *d = &s->draws[i];
        if (d->flags & (LQ_SURFACE_SKY|LQ_SURFACE_WATER)) continue;
        float point[3] = {0};
        for (uint32_t j = 0; j < d->index_count; ++j)
            for (int k = 0; k < 3; ++k) point[k] += s->vertices[s->indices[d->first_index+j]].position[k] / d->index_count;
        const float *normal = s->vertices[s->indices[d->first_index]].normal;
        for (int k = 0; k < 3; ++k) point[k] += normal[k] * 2;
        g->submerged[i] = (uint8_t)LQ_SceneLiquid(s, point);
    }
    g->vertices = UploadBuffer(g, s->vertices, s->vertex_count * sizeof(*s->vertices), SDL_GPU_BUFFERUSAGE_VERTEX);
    g->indices = UploadBuffer(g, s->indices, s->index_count * sizeof(*s->indices), SDL_GPU_BUFFERUSAGE_INDEX);
    g->colormap = UploadImage(g, &s->colormap); g->lightmap = UploadImage(g, &s->lightmap);
    static uint8_t black[4];
    lq_image none = {1, 1, 4, black};
    g->bounce = UploadImage(g, s->bounce.pixels ? &s->bounce : &none);
    g->bounce_ratio = s->bounce.pixels ? (float)s->lightmap.height / s->atlas_height : 0;
    if (s->bounce.pixels) {
        memcpy(g->sky_light, s->sky_light, sizeof(g->sky_light));
        memcpy(g->sun_direction, s->sun_direction, sizeof(s->sun_direction)); g->sun_direction[3] = s->sun_angle_scale;
        memcpy(g->sun_light, s->sun_light, sizeof(s->sun_light));
    }
    if (!g->vertices || !g->indices || !g->colormap || !g->lightmap || !g->bounce) goto failed;
    if (s->probe_static) {
        const uint32_t *n = s->probe_size;
        g->probe_static = Upload3D(g, n[0], n[1], n[2] * 24, s->probe_static, 0);
        g->probe_storage = SDL_GPUTextureSupportsFormat(g->device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
        g->probe_dynamic = Upload3D(g, n[0], n[1], n[2] * 6, NULL,
            g->probe_storage ? SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE : 0);
        if (g->probe_storage && !UploadProbeScene(g, s)) g->probe_storage = 0;
        memcpy(g->probe_origin, s->probe_origin, sizeof(s->probe_origin)); g->probe_origin[3] = s->probe_spacing;
        for (int k = 0; k < 3; ++k) g->probe_size[k] = (float)n[k];
        memcpy(g->probe_styles, s->probe_styles, 4);
        g->probe_dynamic_bytes = n[0] * n[1] * n[2] * 6 * 4;
    } else {
        g->probe_static = Upload3D(g, 1, 1, 1, NULL, 0); g->probe_dynamic = Upload3D(g, 1, 1, 1, NULL, 0);
    }
    if (!g->probe_static || !g->probe_dynamic) goto failed;
    /* Every opaque world face may occlude. As in LIGHT, where only solid
     * space blocks a ray, sky and liquid faces do not. */
    uint32_t *casters = malloc((size_t)s->index_count * sizeof(*casters));
    g->caster_ranges = malloc((size_t)(s->draw_count ? s->draw_count : 1) * sizeof(*g->caster_ranges));
    g->caster_range_faces = malloc(s->draw_count ? s->draw_count : 1);
    if (!casters || !g->caster_ranges || !g->caster_range_faces) { free(casters); SDL_SetError("out of memory"); goto failed; }
    for (int brushes = 0; brushes < 2; ++brushes) {
        for (uint32_t i = 0; i < s->draw_count; ++i) {
            int world = !s->world_draw_end || (i >= s->world_draw_begin && i < s->world_draw_end);
            if ((s->draws[i].flags & (LQ_SURFACE_SKY|LQ_SURFACE_WATER)) || world == brushes || !s->draws[i].index_count) continue;
            /* Brush models cast from where they were compiled (the viewer), but
             * triggers are invisible in the game and cast nothing there. */
            if (brushes && s->draws[i].material < s->material_count &&
                !SDL_strcasecmp(s->materials[s->draws[i].material].name, "trigger")) continue;
            struct lq_caster_range *range = &g->caster_ranges[g->caster_range_count++];
            range->first = g->caster_count; range->count = s->draws[i].index_count;
            for (int k = 0; k < 3; ++k) { range->min[k] = INFINITY; range->max[k] = -INFINITY; }
            for (uint32_t j = 0; j < range->count; ++j) {
                const float *p = s->vertices[s->indices[s->draws[i].first_index + j]].position;
                for (int k = 0; k < 3; ++k) { range->min[k] = fminf(range->min[k], p[k]); range->max[k] = fmaxf(range->max[k], p[k]); }
            }
            memcpy(casters + g->caster_count, s->indices + s->draws[i].first_index, s->draws[i].index_count * sizeof(*casters));
            g->caster_count += s->draws[i].index_count;
        }
        if (!brushes) { g->caster_world_count = g->caster_count; g->caster_range_world_count = g->caster_range_count; }
    }
    if (g->caster_count) g->casters = UploadBuffer(g, casters, g->caster_count * sizeof(*casters), SDL_GPU_BUFFERUSAGE_INDEX);
    free(casters);
    if (g->caster_count && !g->casters) goto failed;
    for (uint32_t i = 0; i < g->texture_count; ++i) {
        g->textures[i] = UploadIndexed(g, &s->materials[i].indices);
        if (!g->textures[i]) goto failed;
    }
    /* Community texture packs: placeholders read as no glow and a flat normal. */
    if (!g->pack_blank_color) {
        uint8_t blank[4] = {0, 0, 0, 0}, flat[4] = {128, 128, 255, 255};
        lq_image b = {1, 1, 4, blank}, f = {1, 1, 4, flat};
        g->pack_blank_color = UploadImage(g, &b); g->pack_flat_normal = UploadImage(g, &f);
        if (!g->pack_blank_color || !g->pack_flat_normal) goto failed;
    }
    g->pack_color = calloc(g->texture_count ? g->texture_count : 1, sizeof(*g->pack_color));
    g->pack_normal = calloc(g->texture_count ? g->texture_count : 1, sizeof(*g->pack_normal));
    g->pack_gain = calloc(g->texture_count ? g->texture_count : 1, sizeof(*g->pack_gain));
    g->pack_height = calloc(g->texture_count ? g->texture_count : 1, sizeof(*g->pack_height));
    if (!g->pack_color || !g->pack_normal || !g->pack_gain || !g->pack_height) { SDL_SetError("out of memory"); goto failed; }
    for (uint32_t i = 0; i < g->texture_count; ++i) {
        g->pack_gain[i] = s->materials[i].pack_gain; g->pack_height[i] = s->materials[i].pack_height;
    }
    {
        SDL_GPUSamplerCreateInfo info = {0};
        info.min_filter = info.mag_filter = SDL_GPU_FILTER_LINEAR; info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        info.address_mode_u = info.address_mode_v = info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        info.max_lod = 1000; info.enable_anisotropy = g->anisotropy > 1; info.max_anisotropy = (float)g->anisotropy;
        g->pack_sampler = SDL_CreateGPUSampler(g->device, &info);
        if (!g->pack_sampler) goto failed;
    }
    for (uint32_t i = 0; i < g->texture_count; ++i) {
        const lq_material *m = &s->materials[i];
        if (m->pack_color.pixels && !(g->pack_color[i] = UploadMipmapped(g, &m->pack_color, 0))) goto failed;
        if (m->pack_normal.pixels && !(g->pack_normal[i] = UploadMipmapped(g, &m->pack_normal, 1))) goto failed;
    }
    return 1;
failed:
    Error(error, size, "GPU scene upload"); ReleaseScene(g); return 0;
}
/* HDR output follows the window's display: extended-range linear light when
 * macOS reports the display in HDR (and lq_hdr allows it), SDR otherwise.
 * Checked every frame, so moving the window between displays switches it. */
static void DisplayMode(lq_gpu *g)
{
    int want = 0;
    if (g->hdr_request) {
        SDL_DisplayID display = SDL_GetDisplayForWindow(g->window);
        want = display && SDL_GetBooleanProperty(SDL_GetDisplayProperties(display), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false) &&
            SDL_WindowSupportsGPUSwapchainComposition(g->device, g->window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    }
    if (want != g->hdr_active && SDL_SetGPUSwapchainParameters(g->device, g->window,
            want ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR : SDL_GPU_SWAPCHAINCOMPOSITION_SDR, g->present_mode)) {
        g->hdr_active = want; g->width = 0; /* the color target changes format */
        SDL_Log("LuminousQuake display: %s", want ? "HDR (extended dynamic range)" : "SDR");
    }
    g->headroom = 1;
    if (g->hdr_active) {
        float headroom = SDL_GetFloatProperty(SDL_GetWindowProperties(g->window), SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1);
        if (isfinite(headroom) && headroom > 1) g->headroom = fminf(headroom, 16);
        /* A mirrored display can be told the master's headroom (a MacBook's
         * XDR panel reports 10x) and clip what it can't show; lq_hdr_peak caps it. */
        if (g->peak >= 1 && g->headroom > g->peak) g->headroom = g->peak;
    }
    {   static float logged = -1;
        if (fabsf(g->headroom - logged) > 0.05f) { SDL_Log("LuminousQuake HDR headroom: %.2f", g->headroom); logged = g->headroom; }
    }
}
void LQ_GPUTestPattern(lq_gpu *g, int on) { g->test_pattern = on != 0; }
void LQ_GPUTone(lq_gpu *g, float shoulder, float white, float emission, float sprite_emission)
{
    g->tone_shoulder = isfinite(shoulder) && shoulder >= 0.1f && shoulder <= 0.95f ? shoulder : LQ_TONE_SHOULDER;
    g->tone_white = isfinite(white) && white >= 1.5f && white <= 64 ? white : LQ_TONE_WHITE;
    g->emission = isfinite(emission) && emission >= 1 && emission <= 16 ? emission : LQ_EMISSION_GAIN;
    g->sprite_emission = isfinite(sprite_emission) && sprite_emission >= 1 && sprite_emission <= 16 ?
        sprite_emission : LQ_EMISSION_GAIN;
}
void LQ_GPUDisplay(lq_gpu *g, float gamma, int hdr, float peak)
{
    g->gamma = isfinite(gamma) && gamma >= 1 && gamma <= 3.5f ? gamma : 2.2f;
    g->hdr_request = hdr != 0;
    g->peak = isfinite(peak) && peak >= 1 ? peak : 0;
}
int LQ_GPUDisplayHDR(const lq_gpu *g, float *headroom)
{
    if (headroom) *headroom = g->headroom;
    return g->hdr_active;
}
static int Targets(lq_gpu *g, uint32_t width, uint32_t height)
{
    if (g->width == width && g->height == height) return 1;
    SDL_GPUTexture *color = Texture(g, width, height,
        g->hdr_active ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTexture *scene = Texture(g, width, height, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUSampleCount samples = g->samples == 4 ? SDL_GPU_SAMPLECOUNT_4 : g->samples == 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.width = width; info.height = height;
    info.layer_count_or_depth = info.num_levels = 1; info.sample_count = samples;
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    SDL_GPUTexture *depth = SDL_CreateGPUTexture(g->device, &info), *multisample = NULL;
    if (g->samples) {
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        multisample = SDL_CreateGPUTexture(g->device, &info);
    }
    if (!color || !scene || !depth || (g->samples && !multisample)) {
        if (multisample) SDL_ReleaseGPUTexture(g->device, multisample);
        if (scene) SDL_ReleaseGPUTexture(g->device, scene);
        if (color) SDL_ReleaseGPUTexture(g->device, color);
        if (depth) SDL_ReleaseGPUTexture(g->device, depth); return 0;
    }
    if (g->color) SDL_ReleaseGPUTexture(g->device, g->color);
    if (g->scene) SDL_ReleaseGPUTexture(g->device, g->scene);
    if (g->depth) SDL_ReleaseGPUTexture(g->device, g->depth);
    if (g->multisample_color) SDL_ReleaseGPUTexture(g->device, g->multisample_color);
    g->multisample_color = multisample; g->scene = scene;
    g->color = color; g->depth = depth; g->width = width; g->height = height; return 1;
}
/* lightValue and the range taper in world.frag's lightReach. */
static float FaceFalloff(const lq_runtime_light *light, float distance)
{
    const float *f = light->falloff;
    float scaled = f[1] * distance, value, range = light->position_radius[3];
    int formula = (int)(f[3] + 0.5f);
    if (formula == 0) value = fmaxf(f[0] - scaled, 0);
    else if (formula == 1) value = f[0] * 128 / fmaxf(scaled, 1);
    else if (formula == 2) value = f[0] * 16384 / fmaxf(scaled * scaled, 1);
    else if (formula == 3) value = f[0];
    else if (formula == 5) value = f[0] * 16384 / ((scaled + 128) * (scaled + 128));
    else return 0;
    if (formula == 1 || formula == 2 || formula == 5) {
        float taper = fminf(fmaxf((range - distance) / (LQ_LIGHT_TAPER * range), 0), 1);
        value *= taper * taper;
    }
    return value;
}
/* The least and most a light's unshadowed value (lightReach times its
 * brightest colour channel) can be anywhere on a world face. The face lies in
 * its bounds, so its distance from the light lies between the box's nearest
 * and farthest points, and every distance term falls with distance. A
 * spotlight's cone may be anything from 0 to 1 over the face. */
static void FaceLightRange(const lq_runtime_light *light, const float plane[4], const float bounds[6],
    float opacity, float *low, float *high)
{
    const float *p = light->position_radius;
    *low = *high = 0;
    if (p[3] <= 0) return;
    float h = plane[0]*p[0] + plane[1]*p[1] + plane[2]*p[2] - plane[3];
    if (h <= 0) return; /* behind the face: lightReach gives nothing */
    float near2 = 0, far2 = 0;
    for (int k = 0; k < 3; ++k) {
        float lo = bounds[k] - 1, hi = bounds[k+3] + 1;
        float d = p[k] < lo ? lo - p[k] : p[k] > hi ? p[k] - hi : 0, f = fmaxf(fabsf(p[k] - lo), fabsf(p[k] - hi));
        near2 += d * d; far2 += f * f;
    }
    float nearest = fmaxf(sqrtf(near2), h), farthest = sqrtf(far2);
    if (nearest >= p[3]) return;
    float color = fmaxf(light->color_strength[0], fmaxf(light->color_strength[1], light->color_strength[2]));
    float strength = light->color_strength[3], z = light->falloff[2];
    /* Incidence (1 - z) + z * facing, with facing from h / farthest to 1. */
    float most = FaceFalloff(light, nearest) * fmaxf(1, 1 - z) * strength;
    if (light->limit > 0) most = fminf(most, light->limit);
    *high = most * color;
    if (farthest >= p[3] || light->spot[3] >= -1) return;
    float least = FaceFalloff(light, farthest) * fminf(1, (1 - z) + z * h / farthest) * strength;
    if (light->limit > 0) least = fminf(least, light->limit);
    if (opacity > 0) least *= 1 - opacity;
    *low = least * color;
}
/* The slot that is brightest at every point of a world face, if one is: its
 * least value there beats every other light's most. The shader then skips
 * its search for the brightest light. 1 + the slot, or 0. */
static uint32_t KnownLead(const lq_view *v, const lq_draw *d, const float plane[4], const float bounds[6])
{
    if (!v->luminous_lighting || v->shadow_dominance < 0 || (d->flags & (LQ_SURFACE_SKY|LQ_SURFACE_WATER))) return 0;
    uint32_t count = v->runtime_light_count > LQ_MAX_RUNTIME_LIGHTS ? LQ_MAX_RUNTIME_LIGHTS : v->runtime_light_count;
    float opacity = v->water_alpha > 0 && v->water_alpha < 1 ? v->water_alpha : 0;
    uint32_t lead = UINT32_MAX, top = UINT32_MAX;
    float lead_low = 0, top_high = 0, second_high = 0;
    for (uint32_t word = 0; word < LQ_LIGHT_WORDS; ++word)
        for (uint32_t bits = d->light_bits[word]; bits; bits &= bits - 1) {
            uint32_t i = word * 32 + (uint32_t)__builtin_ctz(bits);
            if (i >= count) break;
            float low, high;
            FaceLightRange(&v->runtime_lights[i], plane, bounds, opacity, &low, &high);
            if (low > lead_low) { lead_low = low; lead = i; }
            if (high > top_high) { second_high = top_high; top_high = high; top = i; }
            else if (high > second_high) second_high = high;
        }
    if (lead == UINT32_MAX) return 0;
    float rival = top == lead ? second_high : top_high;
    /* A margin for interpolated positions and normals in the shader. */
    return lead_low * 0.98f > rival * 1.02f + 0.01f ? lead + 1 : 0;
}
static void SurfaceUniforms(lq_surface_uniforms *uniforms, const lq_gpu *g,
    const lq_view *view, const lq_draw *draw, uint32_t lead)
{
    memset(uniforms,0,sizeof(*uniforms));
    memcpy(uniforms->dynamic,view->dynamic_lights,sizeof(uniforms->dynamic));
    memcpy(uniforms->bits, draw->light_bits, sizeof(uniforms->bits));
    /* Actors, effects and moving brushes: the lights whose range meets the draw's bounds. */
    ptrdiff_t index = g->frame_draws ? draw - g->frame_draws : -1;
    const float *bounds = index >= 0 && (uint32_t)index < g->frame_draw_count ? g->frame_bounds[index] : NULL;
    if (view->luminous_lighting && bounds) {
        uint32_t count = view->runtime_light_count > LQ_MAX_RUNTIME_LIGHTS ? LQ_MAX_RUNTIME_LIGHTS : view->runtime_light_count;
        memset(uniforms->bits, 0, sizeof(uniforms->bits));
        for (uint32_t i = 0; i < count; ++i) {
            const float *light = view->runtime_lights[i].position_radius;
            float outside = 0;
            for (int k = 0; k < 3; ++k) {
                float d = light[k] < bounds[k] ? bounds[k] - light[k] : light[k] > bounds[k+3] ? light[k] - bounds[k+3] : 0;
                outside += d * d;
            }
            if (light[3] > 0 && outside < light[3] * light[3]) uniforms->bits[i / 32] |= 1u << (i % 32);
        }
    }
    if (view->luminous_lighting && !bounds && !view->disable_shadow_free)
        for (int w = 0; w < 4; ++w) uniforms->shadow_free[w] = draw->shadow_free_bits[w] & ~g->movers[w];
    uniforms->face_light[0] = (float)lead;
    uniforms->entity[0] = draw->flags & LQ_SURFACE_TRANSLUCENT ? draw->alpha : 1;
    uniforms->parameters[0] = view->time; uniforms->parameters[1] = (float)draw->flags;
    uniforms->parameters[2] = (float)g->smooth; uniforms->parameters[3] = (float)g->mipmaps;
    memcpy(uniforms->camera,view->origin,sizeof(view->origin)); uniforms->camera[3] = (float)g->anisotropy;
    for (int j = 0; j < 4; ++j) {
        uniforms->lights[j] = draw->styles[j] == 255 ? 0 : view->lightstyles[draw->styles[j]];
        uniforms->bounce[j] = draw->bounce_styles[j] == 255 ? 0 : view->lightstyles[draw->bounce_styles[j]];
    }
}
/* A slot's shadow cube: drawn from its light, or for a panel group from the
 * group's centre and reach, read by every member (LQ_ShareGroupShadows).
 * Lights made without those fields use their own position and radius. */
static const float *ShadowCentre(const lq_runtime_light *light)
{ return light->shadow_origin_range[3] > 0 ? light->shadow_origin_range : light->position_radius; }
static float ShadowRadius(const lq_runtime_light *light)
{ return light->shadow_origin_range[3] > 0 ? light->shadow_radius : light->source_radius; }
static uint32_t ShadowOwner(const lq_runtime_light *light, uint32_t slot, uint32_t count)
{ return light->shadow_slot && light->shadow_slot - 1 < count && light->shadow_slot - 1 != slot ? light->shadow_slot - 1 : slot; }
static uint8_t CasterFaces(const struct lq_caster *c, const float light[4]);
/* Slots any caster of this frame (actor or moving brush, resting or not) could
 * shadow for: its bounds meet the light's range. */
static void MoverSlots(lq_gpu *g, const lq_view *v)
{
    memset(g->movers, 0, sizeof(g->movers));
    uint32_t count = v->runtime_light_count > LQ_MAX_RUNTIME_LIGHTS ? LQ_MAX_RUNTIME_LIGHTS : v->runtime_light_count;
    for (uint32_t i = 0; i < count; ++i) {
        const float *light = v->runtime_lights[i].position_radius;
        if (light[3] <= 0) continue;
        for (uint32_t c = 0; c < g->frame_caster_count; ++c)
            if (CasterFaces(&g->frame_casters[c], light)) { g->movers[i / 32] |= 1u << (i % 32); break; }
    }
}
static void LightUniforms(lq_light_uniforms *uniforms, const lq_gpu *g, const lq_view *view)
{
    memset(uniforms,0,sizeof(*uniforms));
    uint32_t count = view->runtime_light_count > LQ_MAX_RUNTIME_LIGHTS ? LQ_MAX_RUNTIME_LIGHTS : view->runtime_light_count;
    for (uint32_t i = 0; i < count; ++i) {
        const lq_runtime_light *light = &view->runtime_lights[i];
        memcpy(uniforms->position_range[i],light->position_radius,4*sizeof(float));
        memcpy(uniforms->color_strength[i],light->color_strength,4*sizeof(float));
        memcpy(uniforms->falloff[i],light->falloff,4*sizeof(float));
        memcpy(uniforms->spot[i],light->spot,4*sizeof(float));
        /* Panel lights read their group's cube, drawn from the group's centre
         * with the panel's emitter radius (LQ_ShareGroupShadows). */
        uniforms->shape[i][0] = light->spot_inner; uniforms->shape[i][1] = ShadowRadius(light);
        uniforms->shape[i][2] = light->submerged;
        uniforms->limit[i][0] = light->limit;
        memcpy(&uniforms->limit[i][1], ShadowCentre(light), 3 * sizeof(float));
        uint32_t array, cube; LQ_ShadowSlot(ShadowOwner(light, i, count), &array, &cube);
        uniforms->shape[i][3] = (float)(array * 1024 + cube);
    }
    uniforms->parameters[0] = count;
    uniforms->parameters[1] = view->luminous_lighting != 0;
    uniforms->parameters[2] = LQ_SHADOW_ARRAYS;
    uniforms->parameters[3] = view->luminous_lighting && g->shadow_maps[0] && !view->disable_shadows;
    static const uint32_t sizes[] = LQ_SHADOW_ARRAY_SIZES;
    for (int t = 0; t < LQ_SHADOW_ARRAYS; ++t) uniforms->shadow_sizes[t] = (float)sizes[t];
    uniforms->tuning[0] = view->exposure > 0 ? view->exposure : 1;
    uniforms->color_space[0] = view->light_gamma > 0 ? view->light_gamma : LQ_LIGHT_GAMMA;
    uniforms->color_space[1] = view->shadow_faint < 0 ? 0 : view->shadow_faint > 0 ? view->shadow_faint : LQ_SHADOW_FAINT;
    uniforms->color_space[2] = view->shadow_dominance < 0 ? 0 : view->shadow_dominance > 0 ? view->shadow_dominance : LQ_SHADOW_DOMINANCE;
    uniforms->color_space[3] = g->emission;
    uniforms->effects[0] = g->sprite_emission;
    uniforms->effects[1] = view->palette_gamma > 0 ? view->palette_gamma : 1;
    memcpy(uniforms->palette_shift, view->palette_shift, sizeof(uniforms->palette_shift));
    if (view->fog[0] > 0) {
        for (int k = 0; k < 3; ++k) uniforms->fog[k] = fminf(fmaxf(view->fog[k + 1], 0), 1);
        uniforms->fog[3] = view->fog[0] / 64;
        uniforms->sky_fog[0] = fminf(fmaxf(view->sky_fog, 0), 1);
    }
    uniforms->sky_fog[1] = g->skybox != NULL;
    uniforms->shadow_filter[0] = view->shadow_detail < 0 ? 0 : view->shadow_detail > 0 ? view->shadow_detail : LQ_SHADOW_DETAIL;
    uniforms->tuning[1] = g->bounce_ratio;
    uniforms->tuning[2] = view->luminous_lighting && g->bounce_ratio > 0 && !view->disable_bounce;
    uniforms->tuning[3] = view->bounce_gain > 0 ? view->bounce_gain : 1;
    memcpy(uniforms->probe_origin, g->probe_origin, sizeof(uniforms->probe_origin));
    for (int k = 0; k < 3; ++k) uniforms->probe_size[k] = g->probe_size[k];
    uniforms->probe_size[3] = view->luminous_lighting && !view->disable_bounce && g->probe_origin[3] > 0 ?
        1.0f + (g->probe_dynamic_active ? 2.0f : 0.0f) : 0.0f;
    for (int k = 0; k < 4; ++k)
        uniforms->probe_weights[k] = g->probe_styles[k] == 255 ? 0 : view->lightstyles[g->probe_styles[k]];
    uniforms->liquid[0] = view->luminous_lighting && view->water_alpha > 0 && view->water_alpha < 1 ? view->water_alpha : 0;
    if (view->luminous_lighting && g->bounce_ratio > 0) {
        memcpy(uniforms->sky, g->sky_light, sizeof(g->sky_light));
        memcpy(uniforms->sun_direction, g->sun_direction, sizeof(g->sun_direction));
        memcpy(uniforms->sun_light, g->sun_light, sizeof(g->sun_light));
    }
}
static const float shadow_basis[6][3][3] = { /* forward, right, up per cube face */
    {{1,0,0},{0,0,-1},{0,1,0}}, {{-1,0,0},{0,0,1},{0,1,0}},
    {{0,1,0},{1,0,0},{0,0,-1}}, {{0,-1,0},{1,0,0},{0,0,1}},
    {{0,0,1},{1,0,0},{0,1,0}}, {{0,0,-1},{-1,0,0},{0,1,0}}
};
/* Reversed-depth 90-degree face projection. Faces follow the cube-map table
 * (+X,-X,+Y,-Y,+Z,-Z; screen right/up per face) used by sampling hardware,
 * applied directly to Quake world directions. */
static void ShadowFace(int face, const float origin[3], float out[16])
{
    const float *forward = shadow_basis[face][0], *right = shadow_basis[face][1], *up = shadow_basis[face][2];
    memset(out, 0, 16 * sizeof(float));
    for (int j = 0; j < 3; ++j) {
        out[j * 4] = right[j]; out[j * 4 + 1] = up[j]; out[j * 4 + 3] = forward[j];
        out[12] -= right[j] * origin[j]; out[13] -= up[j] * origin[j]; out[15] -= forward[j] * origin[j];
    }
    out[14] = LQ_SHADOW_NEAR;
}
static int ShadowMaps(lq_gpu *g)
{
    if (g->shadow_maps[0]) return 1;
    static const uint32_t sizes[] = LQ_SHADOW_ARRAY_SIZES, cubes[] = LQ_SHADOW_ARRAY_CUBES;
    for (int t = 0; t < LQ_SHADOW_ARRAYS; ++t) {
        if (cubes[t] * 6 > 255) { SDL_SetError("shadow array exceeds SDL's 8-bit depth layer"); return 0; }
        g->shadow_maps[t] = CubeArray(g, sizes[t], cubes[t]);
        g->shadow_static[t] = CubeArray(g, sizes[t], cubes[t]);
        if (!g->shadow_maps[t] || !g->shadow_static[t]) {
            for (int u = 0; u <= t; ++u) {
                if (g->shadow_maps[u]) SDL_ReleaseGPUTexture(g->device, g->shadow_maps[u]);
                if (g->shadow_static[u]) SDL_ReleaseGPUTexture(g->device, g->shadow_static[u]);
                g->shadow_maps[u] = g->shadow_static[u] = NULL;
            }
            return 0;
        }
    }
    memset(g->shadow_valid, 0, sizeof(g->shadow_valid));
    return 1;
}
/* Cube faces a box can reach: inside the light's range and on the inner side
 * of the face frustum's four planes through the light. */
static uint8_t CasterFaces(const struct lq_caster *c, const float light[4])
{
    float outside = 0;
    for (int k = 0; k < 3; ++k) {
        float d = light[k] < c->min[k] ? c->min[k] - light[k] : light[k] > c->max[k] ? light[k] - c->max[k] : 0;
        outside += d * d;
    }
    if (outside > light[3] * light[3]) return 0;
    uint8_t faces = 0;
    for (int face = 0; face < 6; ++face) {
        const float *forward = shadow_basis[face][0], *right = shadow_basis[face][1], *up = shadow_basis[face][2];
        int inside = 1;
        for (int plane = 0; plane < 4 && inside; ++plane) {
            const float *side = plane < 2 ? right : up;
            float sign = plane & 1 ? 1.0f : -1.0f, normal[3], reach = 0;
            for (int k = 0; k < 3; ++k) normal[k] = forward[k] + sign * side[k];
            for (int k = 0; k < 3; ++k) reach += normal[k] * ((normal[k] >= 0 ? c->max[k] : c->min[k]) - light[k]);
            inside = reach >= 0;
        }
        if (inside) faces |= (uint8_t)(1 << face);
    }
    return faces;
}
/* Cube faces a spotlight's beam can reach. The shader blurs the cone edge by
 * a world-space band, which near the lamp spans wide angles; faces are kept
 * for the cone plus the angle that band subtends at BEAM_NEAR units. Closer
 * than that, edge spill outside these faces goes unshadowed. A face's frustum
 * lies within 54.7 degrees of its axis, so a face is skipped only when its axis
 * is farther than that from every direction the widened cone covers. */
#define BEAM_NEAR 64.0f
static uint8_t BeamFaces(const lq_runtime_light *light)
{
    float outer = light->spot[3];
    if (outer < -1 || outer <= 0.01f || outer > 1) return 0x3f;
    float widen = asinf(fminf((LQ_SPOT_EDGE_BLUR + light->source_radius) / BEAM_NEAR, 1));
    float reach = acosf(outer) + widen + 0.9553166f;
    uint8_t faces = 0;
    for (int face = 0; face < 6; ++face) {
        const float *forward = shadow_basis[face][0];
        float c = forward[0]*light->spot[0] + forward[1]*light->spot[1] + forward[2]*light->spot[2];
        if (acosf(fminf(fmaxf(c, -1), 1)) < reach) faces |= (uint8_t)(1 << face);
    }
    return faces;
}
/* A slot's cube is redrawn in full when its light changes. Otherwise only the
 * faces that moving casters touch now, or touched when last drawn, are redrawn
 * (the latter to erase their old shadows); static world shadows persist. A
 * caster whose bounds match last frame's is at rest: it is drawn into any face
 * that is redrawn, but does not itself cause a redraw. Standing doors, lifts
 * and idle actors no longer redraw every cube they reach every frame. */
static void RenderShadows(lq_gpu *g, SDL_GPUCommandBuffer *command, const lq_view *v)
{
    uint32_t count = v->runtime_light_count > LQ_MAX_RUNTIME_LIGHTS ? LQ_MAX_RUNTIME_LIGHTS : v->runtime_light_count;
    uint8_t brushes = v->static_brush_shadows != 0;
    g->shadow_updates = g->shadow_face_updates = 0;
    for (uint32_t slot = 0; slot < count; ++slot) {
        const lq_runtime_light *light = &v->runtime_lights[slot];
        if (light->position_radius[3] <= 0 || !g->casters) continue;
        /* A panel group's members read the cube of its lowest slot; their own
         * cubes go undrawn and must be rebuilt if they draw again. */
        if (ShadowOwner(light, slot, count) != slot) { g->shadow_valid[slot] = 0; continue; }
        const float *centre = ShadowCentre(light);
        int changed = !g->shadow_valid[slot] || g->shadow_sources[slot] != light->source_index ||
            g->shadow_brushes[slot] != brushes ||
            memcmp(g->shadow_keys[slot], centre, sizeof(g->shadow_keys[slot]));
        uint8_t beam = BeamFaces(light), touched = 0, present = 0, caster_faces[64];
        for (uint32_t i = 0; i < g->frame_caster_count; ++i) {
            uint8_t faces = CasterFaces(&g->frame_casters[i], centre) & beam;
            if (i < 64) caster_faces[i] = faces;
            present |= faces;
            if (g->frame_casters[i].moved) touched |= faces;
        }
        /* A light that lights nothing in view keeps its cube as it was: a new
         * light's cube is built, and faces casters moved through are redrawn,
         * once it lights something in view again. */
        if (light->offscreen) { g->shadow_stale[slot] |= touched; continue; }
        uint8_t redraw = changed ? 0x3f : (uint8_t)(touched | g->shadow_dynamic_faces[slot] | g->shadow_stale[slot]);
        if (!redraw) continue;
        lq_shadow_uniforms uniforms = {{0}};
        static const uint32_t sizes[] = LQ_SHADOW_ARRAY_SIZES;
        uint32_t ranges = brushes ? g->caster_range_count : g->caster_range_world_count;
        int classified = 0; /* static caster ranges' faces, found once per rebuild */
        uint32_t array, cube; LQ_ShadowSlot(slot, &array, &cube);
        for (int face = 0; face < 6; ++face) {
            if (!(redraw & (1 << face))) continue;
            Uint8 layer = (Uint8)(cube * 6 + (uint32_t)face);
            ShadowFace(face, centre, uniforms.face_projection);
            SDL_GPUDepthStencilTargetInfo depth = {0};
            depth.clear_depth = 0; depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            if (!(beam & (1 << face))) { /* outside a spotlight's beam: cleared, reads as unshadowed */
                depth.texture = g->shadow_maps[array]; depth.layer = layer; depth.load_op = SDL_GPU_LOADOP_CLEAR;
                SDL_EndGPURenderPass(SDL_BeginGPURenderPass(command, NULL, 0, &depth));
                continue;
            }
            SDL_GPUViewport viewport = {0, 0, (float)sizes[array], (float)sizes[array], 0, 1};
            if (changed) { /* the static casters, once per light change */
                depth.texture = g->shadow_static[array]; depth.layer = layer; depth.load_op = SDL_GPU_LOADOP_CLEAR;
                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, NULL, 0, &depth);
                SDL_BindGPUGraphicsPipeline(pass, g->shadow_pipeline);
                SDL_SetGPUViewport(pass, &viewport);
                SDL_GPUBufferBinding vertex = {g->vertices, 0}, index = {g->casters, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
                SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));
                /* Only surfaces this face can see within range; neighbours in
                 * the buffer merge into one draw. */
                if (!classified) {
                    for (uint32_t r = 0; r < ranges; ++r) {
                        const struct lq_caster_range *range = &g->caster_ranges[r];
                        struct lq_caster box; memcpy(box.min, range->min, sizeof(box.min)); memcpy(box.max, range->max, sizeof(box.max));
                        g->caster_range_faces[r] = CasterFaces(&box, centre);
                    }
                    classified = 1;
                }
                uint32_t first = 0, count = 0;
                for (uint32_t r = 0; r < ranges; ++r) {
                    const struct lq_caster_range *range = &g->caster_ranges[r];
                    if (!(g->caster_range_faces[r] & (1 << face))) continue;
                    if (count && first + count == range->first) { count += range->count; continue; }
                    if (count) SDL_DrawGPUIndexedPrimitives(pass, count, 1, first, 0, 0);
                    first = range->first; count = range->count;
                }
                if (count) SDL_DrawGPUIndexedPrimitives(pass, count, 1, first, 0, 0);
                SDL_EndGPURenderPass(pass);
            }
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
            SDL_GPUTextureLocation from = {g->shadow_static[array], 0, layer, 0, 0, 0}, to = {g->shadow_maps[array], 0, layer, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(copy, &from, &to, sizes[array], sizes[array], 1, false);
            SDL_EndGPUCopyPass(copy);
            if (present & (1 << face)) { /* moving and resting casters over the static face */
                depth.texture = g->shadow_maps[array]; depth.layer = layer; depth.load_op = SDL_GPU_LOADOP_LOAD;
                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, NULL, 0, &depth);
                SDL_BindGPUGraphicsPipeline(pass, g->shadow_pipeline);
                SDL_SetGPUViewport(pass, &viewport);
                SDL_GPUBufferBinding vertex = {g->frame_vertices, 0}, index = {g->frame_indices, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
                SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));
                for (uint32_t i = 0; i < g->frame_caster_count; ++i) {
                    if (i < 64 && !(caster_faces[i] & (1 << face))) continue;
                    const lq_draw *d = &g->frame_draws[g->frame_casters[i].draw];
                    SDL_DrawGPUIndexedPrimitives(pass, d->index_count, 1, d->first_index, 0, 0);
                }
                SDL_EndGPURenderPass(pass);
            }
            ++g->shadow_face_updates;
        }
        memcpy(g->shadow_keys[slot], centre, sizeof(g->shadow_keys[slot]));
        g->shadow_sources[slot] = light->source_index; g->shadow_valid[slot] = 1;
        g->shadow_brushes[slot] = brushes; g->shadow_dynamic_faces[slot] = touched; g->shadow_stale[slot] = 0;
        ++g->shadow_updates;
    }
}
uint32_t LQ_GPUShadowUpdates(const lq_gpu *g) { return g ? g->shadow_updates : 0; }
uint32_t LQ_GPUSubmittedFaces(const lq_gpu *g) { return g ? g->submitted_faces : 0; }
uint32_t LQ_GPUKnownLeadFaces(const lq_gpu *g) { return g ? g->known_lead_faces : 0; }
uint32_t LQ_GPUShadowFaceUpdates(const lq_gpu *g) { return g ? g->shadow_face_updates : 0; }
size_t LQ_GPUDynamicProbeBytes(const lq_gpu *g) { return g ? g->probe_dynamic_bytes : 0; }
int LQ_GPUDynamicProbes(lq_gpu *g, const uint8_t *pixels, char *error, size_t size)
{
    g->probe_dynamic_active = 0;
    if (!pixels || !g->probe_dynamic_bytes) return 1;
    if (!g->probe_transfer) {
        SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, g->probe_dynamic_bytes, 0};
        g->probe_transfer = SDL_CreateGPUTransferBuffer(g->device, &info);
        if (!g->probe_transfer) return Error(error, size, "GPU probe transfer");
    }
    void *mapped = SDL_MapGPUTransferBuffer(g->device, g->probe_transfer, true);
    if (!mapped) return Error(error, size, "GPU probe map");
    memcpy(mapped, pixels, g->probe_dynamic_bytes);
    SDL_UnmapGPUTransferBuffer(g->device, g->probe_transfer);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(g->device);
    if (!command) return Error(error, size, "GPU probe command");
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source = {g->probe_transfer, 0, (uint32_t)g->probe_size[0], (uint32_t)g->probe_size[1]};
    SDL_GPUTextureRegion dest = {0};
    dest.texture = g->probe_dynamic; dest.w = (uint32_t)g->probe_size[0]; dest.h = (uint32_t)g->probe_size[1];
    dest.d = (uint32_t)g->probe_size[2] * 6;
    SDL_UploadToGPUTexture(pass, &source, &dest, true); SDL_EndGPUCopyPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(command)) return Error(error, size, "GPU probe submit");
    g->probe_dynamic_active = 1; return 1;
}
#define LQ_SHADER_SUBMERGED LQ_SURFACE_SUBMERGED /* face lies inside a liquid */
/* Frame draws name textures by resource id; inline brush models use world
 * materials, whose ids are their index plus one (LQ_EntitiesBegin). */
static uint32_t FramePack(const lq_gpu *g, const lq_draw *d)
{ return d->material && d->material - 1 < g->texture_count && !(d->flags & LQ_SURFACE_MODEL) ? d->material - 1 : UINT32_MAX; }
/* `pack` is the world material whose texture-pack images the surface uses,
 * or UINT32_MAX for none. */
static void DrawSurface(lq_gpu *g, SDL_GPUCommandBuffer *command, SDL_GPURenderPass *pass, const lq_view *v,
    const lq_draw *d, uint32_t extra_flags, SDL_GPUTexture *texture, SDL_GPUTexture *const *shadows, uint32_t lead,
    uint32_t pack)
{
    lq_surface_uniforms uniforms;
    SurfaceUniforms(&uniforms,g,v,d,lead);
    uniforms.parameters[1] = (float)(d->flags | extra_flags);
    SDL_GPUTexture *pack_color = g->pack_blank_color, *pack_normal = g->pack_flat_normal;
    if ((d->flags & LQ_SURFACE_SKY) && g->skybox) pack_color = g->skybox;
    if (pack < g->texture_count && g->pack_color && v->luminous_lighting &&
            !(d->flags & (LQ_SURFACE_SKY|LQ_SURFACE_WATER|LQ_SURFACE_MASKED))) {
        if (v->pack_textures && g->pack_color[pack]) {
            pack_color = g->pack_color[pack]; uniforms.pack[0] = 1;
            uniforms.pack[2] = v->pack_match && g->pack_gain[pack] > 0 ? g->pack_gain[pack] : 1;
        }
        if (v->pack_bumps && g->pack_normal[pack]) {
            pack_normal = g->pack_normal[pack]; uniforms.pack[1] = 1;
            uniforms.relief[0] = v->pack_parallax; uniforms.relief[1] = g->pack_height[pack];
        }
    }
    SDL_GPUTextureSamplerBinding samplers[] = {
        {texture, g->repeat}, {g->colormap, g->point}, {g->lightmap, g->linear},
        {shadows[0], g->shadow_sampler}, {shadows[0], g->depth_sampler}, {g->bounce, g->linear},
        {g->probe_static, g->linear}, {g->probe_dynamic, g->linear},
        {shadows[1], g->shadow_sampler}, {shadows[1], g->depth_sampler},
        {shadows[2], g->shadow_sampler}, {shadows[2], g->depth_sampler},
        {shadows[3], g->shadow_sampler}, {shadows[3], g->depth_sampler},
        {pack_color, g->pack_sampler ? g->pack_sampler : g->linear}, {pack_normal, g->pack_sampler ? g->pack_sampler : g->linear}
    };
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 16);
    SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
    SDL_DrawGPUIndexedPrimitives(pass, d->index_count, 1, d->first_index, 0, 0);
}
int LQ_GPUSkybox(lq_gpu *g, const lq_image faces[6], char *error, size_t size)
{
    if (!g) return 0;
    if (g->skybox) { SDL_WaitForGPUIdle(g->device); SDL_ReleaseGPUTexture(g->device, g->skybox); }
    g->skybox = NULL;
    if (!faces) return 1;
    /* Faces of different sizes are resampled to the largest. */
    uint32_t side = 0;
    for (int f = 0; f < 6; ++f) if (faces[f].pixels && faces[f].channels == 4) {
        if (faces[f].width > side) side = faces[f].width;
        if (faces[f].height > side) side = faces[f].height;
    }
    if (!side) return 1;
    if (side > 2048) side = 2048; /* six faces side by side stay within 12288 */
    lq_image atlas = {side * 6, side, 4, calloc((size_t)side * 6 * side, 4)};
    if (!atlas.pixels) { SDL_SetError("out of memory"); return Error(error, size, "GPU skybox"); }
    for (int f = 0; f < 6; ++f) {
        const lq_image *face = &faces[f];
        if (!face->pixels || face->channels != 4 || !face->width || !face->height) continue;
        for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x) {
            const uint8_t *p = face->pixels + ((size_t)(y * face->height / side) * face->width + x * face->width / side) * 4;
            uint8_t *q = atlas.pixels + ((size_t)y * atlas.width + f * side + x) * 4;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = 255;
        }
    }
    g->skybox = UploadImage(g, &atlas);
    free(atlas.pixels);
    return g->skybox ? 1 : Error(error, size, "GPU skybox upload");
}
int LQ_GPURender(lq_gpu *g, const lq_view *v, uint32_t width, uint32_t height, char *error, size_t size)
{
    SDL_GPUCommandBuffer *command;
    SDL_GPUTexture *swapchain = NULL;
    uint32_t swap_width = 0, swap_height = 0;
    float matrix[16];
    if (!g->vertices || !g->indices) { SDL_SetError("scene not uploaded"); return Error(error, size, "GPU render"); }
    DisplayMode(g);
    if (!width || !height || width > 8192 || height > 8192 || !Targets(g, width, height))
        return Error(error, size, "GPU render target size/allocation");
    if (v->luminous_lighting && v->runtime_light_count && !v->disable_shadows && !ShadowMaps(g))
        return Error(error, size, "GPU shadow maps");
    command = SDL_AcquireGPUCommandBuffer(g->device);
    if (!command) return Error(error, size, "GPU command buffer");
    /* The swapchain image is waited for only once the frame is recorded, so
     * recording overlaps the wait for the display rather than following it. */
    OverlayCopy(g, command);
    g->shadow_updates = 0;
    if (g->probe_pending) ProbeCompute(g, command);
    if (v->luminous_lighting && g->shadow_maps[0] && !v->disable_shadows) RenderShadows(g, command, v);
    MoverSlots(g, v);
    lq_light_uniforms light_uniforms;
    LightUniforms(&light_uniforms, g, v);
    SDL_PushGPUFragmentUniformData(command, 1, &light_uniforms, sizeof(light_uniforms));
    SDL_GPUTexture *shadows[LQ_SHADOW_ARRAYS];
    for (int t = 0; t < LQ_SHADOW_ARRAYS; ++t)
        shadows[t] = g->shadow_maps[t] && v->luminous_lighting ? g->shadow_maps[t] : g->shadow_dummy;
    SDL_GPUColorTargetInfo color = {0};
    SDL_GPUDepthStencilTargetInfo depth = {0};
    color.texture = g->samples ? g->multisample_color : g->scene; color.clear_color = (SDL_FColor){0.02f, 0.02f, 0.025f, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR; color.store_op = SDL_GPU_STOREOP_STORE;
    if (g->samples) { color.resolve_texture = g->scene; color.store_op = SDL_GPU_STOREOP_RESOLVE; }
    depth.texture = g->depth; depth.clear_depth = 1; depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE; depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &color, 1, &depth);
    SDL_BindGPUGraphicsPipeline(pass, g->samples ? g->multisample[g->samples == 4] : g->pipeline);
    SDL_GPUBufferBinding vertex = {g->vertices, 0}, index = {g->indices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1); SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    uint32_t vw = v->viewport[2] ? v->viewport[2] : width;
    uint32_t vh = v->viewport[3] ? v->viewport[3] : height;
    if (v->viewport[0] > width || vw > width - v->viewport[0] ||
        v->viewport[1] > height || vh > height - v->viewport[1]) {
        SDL_EndGPURenderPass(pass); SDL_CancelGPUCommandBuffer(command);
        SDL_SetError("invalid view rectangle"); return Error(error, size, "GPU viewport");
    }
    SDL_GPUViewport viewport = {(float)v->viewport[0], (float)v->viewport[1], (float)vw, (float)vh, 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    lq_view camera = *v;
    float aspect = (float)vw / vh;
    if (v->water_warp) { camera.horizontal_fov *= 1+sinf(v->time*1.5f)*0.01f; aspect *= 1+cosf(v->time*1.7f)*0.01f; }
    LQ_ViewMatrix(&camera, aspect, matrix);
    SDL_PushGPUVertexUniformData(command, 0, matrix, sizeof(matrix));
    /* Camera culling: faces behind the eye or outside the view are not
     * submitted. It applies to this pass only; shadow casters are chosen
     * per light, since geometry off screen still casts on screen. */
    int cull = !v->disable_culling;
    g->submitted_faces = g->known_lead_faces = 0;
    /* Opaque surfaces first; with translucent liquids, water goes last over them. */
    int liquid = v->water_alpha > 0 && v->water_alpha < 1;
    for (int translucent = 0; translucent <= liquid; ++translucent) {
        if (translucent) {
            SDL_BindGPUGraphicsPipeline(pass, g->liquid[g->samples == 4 ? 2 : g->samples ? 1 : 0]);
            SDL_SetGPUBlendConstants(pass, (SDL_FColor){v->water_alpha, v->water_alpha, v->water_alpha, v->water_alpha});
            viewport.max_depth = 1; SDL_SetGPUViewport(pass, &viewport);
            vertex = (SDL_GPUBufferBinding){g->vertices, 0}; index = (SDL_GPUBufferBinding){g->indices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1); SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        }
        for (uint32_t i = 0; i < g->draw_count; ++i) {
            const lq_draw *d = v->world_draws && v->world_draw_count == g->draw_count ? &v->world_draws[i] : &g->draws[i];
            if (!d->index_count || d->material >= g->texture_count) continue;
            if (liquid && translucent != ((d->flags & LQ_SURFACE_WATER) != 0)) continue;
            const float *p = g->planes[i];
            float side = p[0]*v->origin[0] + p[1]*v->origin[1] + p[2]*v->origin[2] - p[3];
            if (translucent && side < 0) continue;
            if (cull && (side < -0.01f || !LQ_BoundsInClip(matrix, g->draw_bounds[i]))) continue;
            ++g->submitted_faces;
            uint32_t lead = KnownLead(v, d, p, g->draw_bounds[i]);
            g->known_lead_faces += lead != 0;
            DrawSurface(g, command, pass, v, d, g->submerged && g->submerged[i] ? LQ_SHADER_SUBMERGED : 0,
                g->textures[d->material], shadows, lead, d->material);
        }
        if (!g->frame_draw_count) continue;
        vertex = (SDL_GPUBufferBinding){g->frame_vertices, 0};
        index = (SDL_GPUBufferBinding){g->frame_indices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
        SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        for (uint32_t i = 0; i < g->frame_draw_count; ++i) {
            const lq_draw *d = &g->frame_draws[i];
            if (liquid && translucent != ((d->flags & LQ_SURFACE_WATER) != 0)) continue;
            if ((d->flags & (LQ_SURFACE_ERODE|LQ_SURFACE_TRANSLUCENT)) || ((d->flags & LQ_SURFACE_SPRITE) && v->luminous_lighting))
                continue; /* translucent entities, smoke and Luminous's feathered sprites: blended last */
            if (cull && !(d->flags & LQ_SURFACE_WEAPON) && !LQ_BoundsInClip(matrix, g->frame_bounds[i])) continue;
            viewport.max_depth = d->flags & LQ_SURFACE_WEAPON ? 0.3f : 1.0f;
            SDL_SetGPUViewport(pass, &viewport);
            DrawSurface(g, command, pass, v, d, 0, g->frame_textures[d->material], shadows, 0, FramePack(g, d));
        }
    }
    {   /* Translucent entities (farthest first, as LQ_EntitiesBuild orders
         * them), smoke and Luminous sprites last, blended by their alpha over
         * everything solid and liquid. */
        SDL_GPUGraphicsPipeline *smoke = g->smoke[g->samples == 4 ? 2 : g->samples ? 1 : 0];
        int bound = 0;
        for (uint32_t i = 0; smoke && i < g->frame_draw_count; ++i) {
            const lq_draw *d = &g->frame_draws[i];
            if (!(d->flags & (LQ_SURFACE_ERODE|LQ_SURFACE_TRANSLUCENT)) && !((d->flags & LQ_SURFACE_SPRITE) && v->luminous_lighting)) continue;
            if (cull && !(d->flags & LQ_SURFACE_WEAPON) && !LQ_BoundsInClip(matrix, g->frame_bounds[i])) continue;
            float depth = d->flags & LQ_SURFACE_WEAPON ? 0.3f : 1.0f;
            if (bound && viewport.max_depth != depth) { viewport.max_depth = depth; SDL_SetGPUViewport(pass, &viewport); }
            if (!bound) {
                SDL_BindGPUGraphicsPipeline(pass, smoke);
                viewport.max_depth = depth; SDL_SetGPUViewport(pass, &viewport);
                vertex = (SDL_GPUBufferBinding){g->frame_vertices, 0};
                index = (SDL_GPUBufferBinding){g->frame_indices, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
                SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                bound = 1;
            }
            DrawSurface(g, command, pass, v, d, 0, g->frame_textures[d->material], shadows, 0, FramePack(g, d));
        }
    }
    SDL_EndGPURenderPass(pass);
    {   /* Tone mapping: the float frame to the display (see tonemap.frag). */
        struct { float tone[4], target[4]; } display = {
            {g->test_pattern ? 3.0f : v->luminous_lighting ? 1.0f : 0.0f, g->gamma / 2.2f, g->hdr_active ? g->headroom : 1.0f,
             g->hdr_active ? fmaxf(g->tone_white, 2.0f * g->headroom) : g->tone_white},
            {g->hdr_active ? 1.0f : 0.0f, g->gamma, g->tone_shoulder,
             v->luminous_lighting && v->palette_gamma > 0 ? v->palette_gamma : 1.0f}};
        SDL_GPUColorTargetInfo target = {0};
        target.texture = g->color; target.load_op = SDL_GPU_LOADOP_DONT_CARE; target.store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(command, &target, 1, NULL);
        SDL_BindGPUGraphicsPipeline(pass, g->tonemap_pipelines[g->hdr_active != 0]);
        SDL_GPUTextureSamplerBinding frame = {g->scene, g->point};
        SDL_BindGPUFragmentSamplers(pass, 0, &frame, 1);
        SDL_PushGPUFragmentUniformData(command, 0, &display, sizeof(display));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    if (g->overlay) {
        float transfer[4] = {g->gamma / 2.2f, 0, 0, 0};
        color.texture = g->color; color.resolve_texture = NULL; color.store_op = SDL_GPU_STOREOP_STORE;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        pass = SDL_BeginGPURenderPass(command, &color, 1, NULL);
        SDL_BindGPUGraphicsPipeline(pass, g->overlay_pipelines[g->hdr_active != 0]);
        SDL_PushGPUFragmentUniformData(command, 0, transfer, sizeof(transfer));
        SDL_GPUTextureSamplerBinding sampler = {g->overlay, g->point};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, g->window, &swapchain, &swap_width, &swap_height)) {
        SDL_CancelGPUCommandBuffer(command); return Error(error, size, "GPU swapchain");
    }
    if (swapchain && g->hdr_active &&
            SDL_GetGPUSwapchainTextureFormat(g->device, g->window) == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
        /* HDR: display values become linear light (extended range) as they
         * are drawn into the swapchain, letterboxed like the SDR blit. */
        float scale = (float)swap_width / width;
        if ((float)swap_height / height < scale) scale = (float)swap_height / height;
        struct { float tone[4], target[4]; } decode = {{2, 1, 1, 1}, {1, 2.2f, 1, 0}};
        SDL_GPUColorTargetInfo target = {0};
        target.texture = swapchain; target.load_op = SDL_GPU_LOADOP_CLEAR; target.store_op = SDL_GPU_STOREOP_STORE;
        target.clear_color = (SDL_FColor){0, 0, 0, 1};
        pass = SDL_BeginGPURenderPass(command, &target, 1, NULL);
        SDL_GPUViewport box = {(swap_width - width * scale) / 2, (swap_height - height * scale) / 2,
            width * scale, height * scale, 0, 1};
        SDL_SetGPUViewport(pass, &box);
        SDL_BindGPUGraphicsPipeline(pass, g->tonemap_pipelines[1]);
        SDL_GPUTextureSamplerBinding frame = {g->color, g->point};
        SDL_BindGPUFragmentSamplers(pass, 0, &frame, 1);
        SDL_PushGPUFragmentUniformData(command, 0, &decode, sizeof(decode));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    } else if (swapchain) {
        SDL_GPUBlitInfo blit = {0};
        blit.source.texture = g->color; blit.source.w = width; blit.source.h = height;
        float scale = (float)swap_width / width;
        if ((float)swap_height / height < scale) scale = (float)swap_height / height;
        blit.destination.texture = swapchain;
        blit.destination.w = (uint32_t)(width * scale); blit.destination.h = (uint32_t)(height * scale);
        blit.destination.x = (swap_width - blit.destination.w) / 2;
        blit.destination.y = (swap_height - blit.destination.h) / 2;
        blit.load_op = SDL_GPU_LOADOP_CLEAR; blit.clear_color = (SDL_FColor){0, 0, 0, 1};
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command, &blit);
    }
    if (!SDL_SubmitGPUCommandBuffer(command)) return Error(error, size, "GPU submit");
    return 1;
}
static float HalfToFloat(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exponent = (h >> 10) & 31, mantissa = h & 1023, bits;
    if (!exponent) {
        if (!mantissa) bits = sign;
        else { float f = ldexpf((float)mantissa, -24); return sign ? -f : f; }
    } else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
    else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f; memcpy(&f, &bits, sizeof(f)); return f;
}
/* Screenshots are 8-bit display values. An HDR frame holds display values
 * that continue past SDR white; those highlights clip. */
int LQ_GPUReadPixels(lq_gpu *g, uint8_t **pixels, uint32_t *width, uint32_t *height, char *error, size_t size)
{
    uint32_t texel = g->hdr_active ? 8 : 4, bytes = g->width * g->height * texel;
    SDL_GPUTransferBuffer *transfer;
    SDL_GPUCommandBuffer *command;
    SDL_GPUFence *fence;
    *pixels = NULL;
    if (!g->color || !bytes) { SDL_SetError("no rendered frame"); return Error(error, size, "GPU readback"); }
    transfer = Transfer(g, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, NULL);
    command = transfer ? SDL_AcquireGPUCommandBuffer(g->device) : NULL;
    if (!command) { if (transfer) SDL_ReleaseGPUTransferBuffer(g->device, transfer); return Error(error, size, "GPU readback allocation"); }
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureRegion source = {0}; source.texture = g->color; source.w = g->width; source.h = g->height; source.d = 1;
    SDL_GPUTextureTransferInfo destination = {transfer, 0, g->width, g->height};
    SDL_DownloadFromGPUTexture(pass, &source, &destination); SDL_EndGPUCopyPass(pass);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (!fence || !SDL_WaitForGPUFences(g->device, true, &fence, 1)) goto failed;
    void *mapped = SDL_MapGPUTransferBuffer(g->device, transfer, false);
    if (!mapped) goto failed;
    *pixels = malloc((size_t)g->width * g->height * 4);
    if (*pixels && texel == 4) memcpy(*pixels, mapped, bytes);
    else if (*pixels) {
        const uint16_t *half = mapped;
        float peak = 0; size_t above = 0;
        for (size_t i = 0; i < (size_t)g->width * g->height * 4; ++i) {
            float v = HalfToFloat(half[i]);
            if ((i & 3) != 3) { if (v > peak) peak = v; if (v > 1) ++above; }
            (*pixels)[i] = (uint8_t)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f);
        }
        SDL_Log("LuminousQuake HDR frame: peak %.2f of SDR white (light %.2f), %zu channel values above white; headroom %.2f",
            peak, powf(peak, 2.2f), above, g->headroom);
    }
    SDL_UnmapGPUTransferBuffer(g->device, transfer);
    if (!*pixels) { SDL_SetError("out of memory"); goto failed; }
    *width = g->width; *height = g->height;
    SDL_ReleaseGPUFence(g->device, fence); SDL_ReleaseGPUTransferBuffer(g->device, transfer); return 1;
failed:
    Error(error, size, "GPU readback");
    if (fence) SDL_ReleaseGPUFence(g->device, fence);
    SDL_ReleaseGPUTransferBuffer(g->device, transfer); return 0;
}
void LQ_GPUDestroy(lq_gpu *g)
{
    if (!g) return;
    if (g->device) {
        SDL_WaitForGPUIdle(g->device); ReleaseScene(g);
        if (g->frame_vertices) SDL_ReleaseGPUBuffer(g->device, g->frame_vertices);
        if (g->frame_indices) SDL_ReleaseGPUBuffer(g->device, g->frame_indices);
        if (g->frame_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->frame_transfer);
        if (g->color) SDL_ReleaseGPUTexture(g->device, g->color);
        if (g->scene) SDL_ReleaseGPUTexture(g->device, g->scene);
        if (g->depth) SDL_ReleaseGPUTexture(g->device, g->depth);
        if (g->multisample_color) SDL_ReleaseGPUTexture(g->device, g->multisample_color);
        for (int i = 0; i < 2; ++i) if (g->multisample[i]) SDL_ReleaseGPUGraphicsPipeline(g->device, g->multisample[i]);
        for (int i = 0; i < 3; ++i) if (g->liquid[i]) SDL_ReleaseGPUGraphicsPipeline(g->device, g->liquid[i]);
        if (g->pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipeline);
        for (int i = 0; i < 3; ++i) if (g->smoke[i]) SDL_ReleaseGPUGraphicsPipeline(g->device, g->smoke[i]);
        for (int f = 0; f < 2; ++f) {
            if (g->overlay_pipelines[f]) SDL_ReleaseGPUGraphicsPipeline(g->device, g->overlay_pipelines[f]);
            if (g->tonemap_pipelines[f]) SDL_ReleaseGPUGraphicsPipeline(g->device, g->tonemap_pipelines[f]);
        }
        if (g->overlay) SDL_ReleaseGPUTexture(g->device, g->overlay);
        if (g->overlay_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->overlay_transfer);
        for (int t = 0; t < LQ_SHADOW_ARRAYS; ++t) {
            if (g->shadow_maps[t]) SDL_ReleaseGPUTexture(g->device, g->shadow_maps[t]);
            if (g->shadow_static[t]) SDL_ReleaseGPUTexture(g->device, g->shadow_static[t]);
        }
        if (g->shadow_dummy) SDL_ReleaseGPUTexture(g->device, g->shadow_dummy);
        if (g->pack_blank_color) SDL_ReleaseGPUTexture(g->device, g->pack_blank_color);
        if (g->pack_flat_normal) SDL_ReleaseGPUTexture(g->device, g->pack_flat_normal);
        if (g->skybox) SDL_ReleaseGPUTexture(g->device, g->skybox);
        if (g->shadow_sampler) SDL_ReleaseGPUSampler(g->device, g->shadow_sampler);
        if (g->depth_sampler) SDL_ReleaseGPUSampler(g->device, g->depth_sampler);
        if (g->shadow_pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->shadow_pipeline);
        if (g->bounce_pipeline) SDL_ReleaseGPUComputePipeline(g->device, g->bounce_pipeline);
        if (g->probe_pipeline) SDL_ReleaseGPUComputePipeline(g->device, g->probe_pipeline);
        if (g->repeat) SDL_ReleaseGPUSampler(g->device, g->repeat);
        if (g->point) SDL_ReleaseGPUSampler(g->device, g->point);
        if (g->linear) SDL_ReleaseGPUSampler(g->device, g->linear);
        if (g->claimed) SDL_ReleaseWindowFromGPUDevice(g->device, g->window);
        SDL_DestroyGPUDevice(g->device);
    }
    free(g->mip_palette); free(g->frame_draws); free(g->frame_casters); free(g->frame_bounds); free(g->rest_bounds); free(g->rest_scratch); free(g);
}

/* ---- Luminous bounce rays (renderer/shaders/bounce.comp) ------------------- */
enum { BOUNCE_NODE_WORDS = 8, BOUNCE_FACE_WORDS = 24, BOUNCE_LIGHT_WORDS = 20, BOUNCE_CHUNK = 65536 };
typedef struct { uint32_t job[4], tree[4], tables[4], lights[4]; float sun[4]; } lq_bounce_uniforms;
static void PutFloat(uint32_t *word, float value) { memcpy(word, &value, sizeof(value)); }
/* The scene words the shader walks; tables[] receives the section offsets. */
static uint32_t *BounceWords(const lq_scene *s, const uint8_t (*emit_styles)[4], lq_bounce_uniforms *u, size_t *bytes)
{
    size_t nodes = 0, leaves = nodes + (size_t)s->bsp_node_count * BOUNCE_NODE_WORDS;
    size_t faces = leaves + s->leaf_count, vertices = faces + (size_t)s->draw_count * BOUNCE_FACE_WORDS;
    size_t samples = vertices + (size_t)s->vertex_count * 3, lights = samples + (size_t)s->sample_count * 4;
    size_t reach = lights + (size_t)s->light_source_count * BOUNCE_LIGHT_WORDS;
    uint32_t reach_words = s->light_visibility ? s->light_visibility_words : 0;
    size_t total = reach + (size_t)s->draw_count * reach_words;
    if (total * 4 > UINT32_MAX) return NULL;
    uint32_t *w = calloc(total, 4);
    if (!w) return NULL;
    for (uint32_t i = 0; i < s->bsp_node_count; ++i) {
        const lq_bsp_node *n = &s->bsp_nodes[i]; uint32_t *o = w + nodes + (size_t)i * BOUNCE_NODE_WORDS;
        for (int k = 0; k < 3; ++k) PutFloat(&o[k], n->normal[k]);
        PutFloat(&o[3], n->dist); o[4] = (uint32_t)n->children[0]; o[5] = (uint32_t)n->children[1];
        o[6] = n->first_face; o[7] = n->face_count;
    }
    for (uint32_t i = 0; i < s->leaf_count; ++i) w[leaves + i] = s->leaf_contents[i];
    for (uint32_t f = 0; f < s->draw_count; ++f) {
        const lq_draw *d = &s->draws[f]; const lq_face_samples *grid = &s->face_samples[f];
        uint32_t *o = w + faces + (size_t)f * BOUNCE_FACE_WORDS;
        if (d->first_index >= s->index_count) continue; /* kind 0: never hit */
        uint32_t base = s->indices[d->first_index];
        int sky = (d->flags & LQ_SURFACE_SKY) != 0;
        o[0] = sky ? 2 : !d->flags && grid->width ? 1 : 0;
        o[1] = base; o[2] = d->index_count / 3 + 2;
        o[3] = emit_styles ? (uint32_t)emit_styles[f][0] | (uint32_t)emit_styles[f][1] << 8 |
            (uint32_t)emit_styles[f][2] << 16 | (uint32_t)emit_styles[f][3] << 24 : 0xffffffffu;
        for (int k = 0; k < 3; ++k) PutFloat(&o[4 + k], s->vertices[base].normal[k]);
        o[7] = grid->width | (uint32_t)grid->height << 16; o[8] = grid->first_sample;
        for (int k = 0; k < 4; ++k) { PutFloat(&o[9 + k], grid->s[k]); PutFloat(&o[13 + k], grid->t[k]); }
        PutFloat(&o[17], grid->mins[0]); PutFloat(&o[18], grid->mins[1]);
        for (int k = 0; k < 3; ++k) PutFloat(&o[19 + k], grid->albedo[k]);
    }
    for (uint32_t i = 0; i < s->vertex_count; ++i)
        for (int k = 0; k < 3; ++k) PutFloat(&w[vertices + (size_t)i * 3 + k], s->vertices[i].position[k]);
    for (size_t i = 0; i < s->sample_count; ++i) w[samples + i * 4 + 3] = 0xffffffffu;
    for (uint32_t f = 0; f < s->draw_count; ++f) {
        const lq_face_samples *grid = &s->face_samples[f];
        for (uint32_t i = 0; grid->width && i < (uint32_t)grid->width * grid->height; ++i) {
            size_t sample = grid->first_sample + i;
            const float *p = s->sample_positions[sample];
            if (sample >= s->sample_count || !isfinite(p[0])) continue;
            for (int k = 0; k < 3; ++k) PutFloat(&w[samples + sample * 4 + k], p[k]);
            w[samples + sample * 4 + 3] = f;
        }
    }
    u->tree[0] = (uint32_t)nodes; u->tree[1] = s->bsp_node_count; u->tree[2] = (uint32_t)leaves; u->tree[3] = s->leaf_count;
    /* Lights with no direct reach (local minlight, too faint) get range 0. */
    for (uint32_t i = 0; i < s->light_source_count; ++i) {
        const lq_light_source *l = &s->light_sources[i]; uint32_t *o = w + lights + (size_t)i * BOUNCE_LIGHT_WORDS;
        for (int k = 0; k < 3; ++k) {
            PutFloat(&o[k], l->origin[k]); PutFloat(&o[9 + k], l->color[k]); PutFloat(&o[13 + k], l->spot_direction[k]);
        }
        PutFloat(&o[3], LQ_LightSourceRange(l)); PutFloat(&o[4], l->intensity); PutFloat(&o[5], l->attenuation);
        o[6] = l->formula; PutFloat(&o[7], l->angle_scale); PutFloat(&o[8], l->output_scale);
        o[12] = l->spotlight != 0; PutFloat(&o[16], l->spot_outer); PutFloat(&o[17], l->spot_inner);
        PutFloat(&o[18], l->source_radius); o[19] = (uint8_t)l->style;
    }
    if (reach_words) memcpy(w + reach, s->light_visibility, (size_t)s->draw_count * reach_words * 4);
    u->tables[0] = (uint32_t)faces; u->tables[1] = s->draw_count; u->tables[2] = (uint32_t)vertices; u->tables[3] = (uint32_t)samples;
    u->lights[0] = (uint32_t)lights; u->lights[1] = s->light_source_count; u->lights[2] = (uint32_t)reach; u->lights[3] = reach_words;
    for (int k = 0; k < 3; ++k) u->sun[k] = s->sun_direction[k];
    u->sun[3] = s->sun_penumbra;
    *bytes = total * 4;
    return w;
}
static SDL_GPUBuffer *StorageBuffer(lq_gpu *g, SDL_GPUBufferUsageFlags usage, uint32_t bytes)
{
    SDL_GPUBufferCreateInfo info = {usage, bytes, 0};
    return SDL_CreateGPUBuffer(g->device, &info);
}
/* Traces every sample (mode 0: sky visibility, 1: gather, 2: sun visibility,
 * 3: direct light) and reads back
 * `output_bytes`. Work is split across command buffers so no single
 * submission runs long enough to trip a GPU watchdog. */
static int BounceRun(lq_gpu *g, const lq_scene *s, uint32_t mode, uint32_t rays,
    const float (*emit)[4][3], const uint8_t (*emit_styles)[4], void *output, size_t output_bytes)
{
    if (!s->bsp_nodes || !s->face_samples || !s->sample_count || output_bytes > UINT32_MAX) return 0;
    if (!g->bounce_pipeline) {
        int metal = (SDL_GetGPUShaderFormats(g->device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
        SDL_GPUComputePipelineCreateInfo info = {0};
        info.code = metal ? lq_bounce_comp_msl : lq_bounce_comp_spv;
        info.code_size = metal ? sizeof(lq_bounce_comp_msl) : sizeof(lq_bounce_comp_spv);
        info.entrypoint = metal ? "main0" : "main";
        info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
        info.num_readonly_storage_buffers = 2; info.num_readwrite_storage_buffers = 1; info.num_uniform_buffers = 1;
        info.threadcount_x = 64; info.threadcount_y = info.threadcount_z = 1;
        g->bounce_pipeline = SDL_CreateGPUComputePipeline(g->device, &info);
        if (!g->bounce_pipeline) return 0;
    }
    lq_bounce_uniforms u = {{mode, 0, 0, rays}, {0}, {0}, {0}, {0}};
    size_t scene_bytes, emit_bytes = emit ? (size_t)s->sample_count * sizeof(*emit) : 16;
    uint32_t *words = BounceWords(s, emit_styles, &u, &scene_bytes);
    if (!words || emit_bytes > UINT32_MAX || scene_bytes + emit_bytes > UINT32_MAX) { free(words); return 0; }
    static const float empty[4];
    SDL_GPUBuffer *scene = StorageBuffer(g, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, (uint32_t)scene_bytes);
    SDL_GPUBuffer *values = StorageBuffer(g, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, (uint32_t)emit_bytes);
    SDL_GPUBuffer *results = StorageBuffer(g, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, (uint32_t)output_bytes);
    SDL_GPUTransferBuffer *upload = Transfer(g, (uint32_t)(scene_bytes + emit_bytes), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, NULL);
    SDL_GPUTransferBuffer *download = Transfer(g, (uint32_t)output_bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, NULL);
    SDL_GPUFence *fence = NULL;
    int ok = 0;
    void *mapped = upload ? SDL_MapGPUTransferBuffer(g->device, upload, false) : NULL;
    if (!scene || !values || !results || !download || !mapped) goto done;
    memcpy(mapped, words, scene_bytes);
    memcpy((uint8_t *)mapped + scene_bytes, emit ? (const void *)emit : empty, emit_bytes);
    SDL_UnmapGPUTransferBuffer(g->device, upload);
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(g->device);
    if (!command) goto done;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation from = {upload, 0};
    SDL_GPUBufferRegion to = {scene, 0, (uint32_t)scene_bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, false);
    from.offset = (uint32_t)scene_bytes; to = (SDL_GPUBufferRegion){values, 0, (uint32_t)emit_bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    for (uint32_t first = 0; first < s->sample_count; first += BOUNCE_CHUNK) {
        if (!command && !(command = SDL_AcquireGPUCommandBuffer(g->device))) goto done;
        u.job[1] = first; u.job[2] = s->sample_count - first < BOUNCE_CHUNK ? s->sample_count - first : BOUNCE_CHUNK;
        SDL_GPUStorageBufferReadWriteBinding writes = {results, false};
        SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, NULL, 0, &writes, 1);
        if (!pass) { SDL_CancelGPUCommandBuffer(command); goto done; }
        SDL_GPUBuffer *reads[2] = {scene, values};
        SDL_BindGPUComputePipeline(pass, g->bounce_pipeline);
        SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 2);
        SDL_PushGPUComputeUniformData(command, 0, &u, sizeof(u));
        SDL_DispatchGPUCompute(pass, (u.job[2] + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(pass);
        if (first + BOUNCE_CHUNK >= s->sample_count) break;
        if (!SDL_SubmitGPUCommandBuffer(command)) { command = NULL; goto done; }
        command = NULL;
    }
    copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUBufferRegion source = {results, 0, (uint32_t)output_bytes};
    SDL_GPUTransferBufferLocation destination = {download, 0};
    SDL_DownloadFromGPUBuffer(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (!fence || !SDL_WaitForGPUFences(g->device, true, &fence, 1)) goto done;
    void *read = SDL_MapGPUTransferBuffer(g->device, download, false);
    if (!read) goto done;
    memcpy(output, read, output_bytes);
    SDL_UnmapGPUTransferBuffer(g->device, download);
    ok = 1;
done:
    if (fence) SDL_ReleaseGPUFence(g->device, fence);
    if (scene) SDL_ReleaseGPUBuffer(g->device, scene);
    if (values) SDL_ReleaseGPUBuffer(g->device, values);
    if (results) SDL_ReleaseGPUBuffer(g->device, results);
    if (upload) SDL_ReleaseGPUTransferBuffer(g->device, upload);
    if (download) SDL_ReleaseGPUTransferBuffer(g->device, download);
    free(words);
    return ok;
}
static int BounceSky(void *context, const lq_scene *s, uint32_t rays, float *visible)
{ return BounceRun(context, s, 0, rays, NULL, NULL, visible, (size_t)s->sample_count * sizeof(float)); }
static int BounceSun(void *context, const lq_scene *s, float *visible)
{ return BounceRun(context, s, 2, 1, NULL, NULL, visible, (size_t)s->sample_count * sizeof(float)); }
static int BounceDirect(void *context, const lq_scene *s, lq_bounce_entry *entries)
{ return BounceRun(context, s, 3, 1, NULL, NULL, entries, (size_t)s->sample_count * LQ_BOUNCE_ENTRIES * sizeof(*entries)); }
static int BounceGather(void *context, const lq_scene *s, uint32_t rays, const float (*emit)[4][3],
    const uint8_t (*emit_styles)[4], lq_bounce_entry *entries)
{
    return BounceRun(context, s, 1, rays, emit, emit_styles, entries,
        (size_t)s->sample_count * LQ_BOUNCE_ENTRIES * sizeof(*entries));
}
lq_bounce_tracer LQ_GPUBounceTracer(lq_gpu *g)
{
    lq_bounce_tracer tracer = {g, BounceSky, BounceSun, BounceDirect, BounceGather, 0};
    return tracer;
}

/* ---- Luminous dynamic probes (bounce.comp built with LQ_PROBES) ----------- */
/* The resident words the probe kernel walks: the bounce scene's BSP, faces and
 * vertices, then one byte of validity per probe. */
static int UploadProbeScene(lq_gpu *g, const lq_scene *s)
{
    if (!s->bsp_nodes || !s->face_samples || !s->probe_valid) return 0;
    lq_bounce_uniforms u = {{0}, {0}, {0}, {0}, {0}};
    size_t bytes, probes = (size_t)s->probe_size[0] * s->probe_size[1] * s->probe_size[2];
    uint32_t *words = BounceWords(s, NULL, &u, &bytes);
    size_t valid = bytes / 4, total = valid + (probes + 3) / 4;
    uint32_t *grown = words && total * 4 <= UINT32_MAX ? realloc(words, total * 4) : NULL;
    if (!grown) { free(words); return 0; }
    words = grown;
    memset(words + valid, 0, (total - valid) * 4);
    for (size_t p = 0; p < probes; ++p) if (s->probe_valid[p]) words[valid + p / 4] |= 1u << (8 * (p % 4));
    g->probe_scene = StorageBuffer(g, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, (uint32_t)(total * 4));
    SDL_GPUTransferBuffer *transfer = g->probe_scene ? Transfer(g, (uint32_t)(total * 4), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, words) : NULL;
    SDL_GPUCommandBuffer *command = transfer ? SDL_AcquireGPUCommandBuffer(g->device) : NULL;
    int ok = 0;
    if (command) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
        SDL_GPUTransferBufferLocation from = {transfer, 0};
        SDL_GPUBufferRegion to = {g->probe_scene, 0, (uint32_t)(total * 4)};
        SDL_UploadToGPUBuffer(copy, &from, &to, false); SDL_EndGPUCopyPass(copy);
        ok = SDL_SubmitGPUCommandBuffer(command);
    }
    if (transfer) SDL_ReleaseGPUTransferBuffer(g->device, transfer);
    free(words);
    if (!ok && g->probe_scene) { SDL_ReleaseGPUBuffer(g->device, g->probe_scene); g->probe_scene = NULL; }
    if (!ok) return 0;
    struct lq_probe_uniforms *pu = &g->probe_uniforms;
    memset(pu, 0, sizeof(*pu));
    memcpy(pu->tree, u.tree, sizeof(pu->tree)); memcpy(pu->tables, u.tables, sizeof(pu->tables));
    pu->tables[3] = (uint32_t)valid;
    memcpy(pu->grid, s->probe_origin, sizeof(s->probe_origin)); pu->grid[3] = s->probe_spacing;
    memcpy(pu->size, s->probe_size, sizeof(s->probe_size));
    float scale = s->bounce_scale; memcpy(&pu->dispatch_size[3], &scale, sizeof(scale));
    return 1;
}
static void ProbeCompute(lq_gpu *g, SDL_GPUCommandBuffer *command)
{
    g->probe_pending = 0;
    const uint32_t *n = g->probe_uniforms.dispatch_size;
    SDL_GPUStorageTextureReadWriteBinding target = {g->probe_dynamic, 0, 0, false};
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &target, 1, NULL, 0);
    if (!pass) return;
    SDL_BindGPUComputePipeline(pass, g->probe_pipeline);
    SDL_BindGPUComputeStorageBuffers(pass, 0, &g->probe_scene, 1);
    SDL_PushGPUComputeUniformData(command, 0, &g->probe_uniforms, sizeof(g->probe_uniforms));
    /* Sixteen threads per probe, one per ray (bounce.comp's RAYS). */
    SDL_DispatchGPUCompute(pass, (n[0] * n[1] * n[2] * 16 + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(pass);
}
int LQ_GPUTraceDynamicProbes(lq_gpu *g, const lq_runtime_light *const *lights, uint32_t count,
    const uint32_t box_min[3], const uint32_t box_size[3], uint32_t rays)
{
    if (!g || !g->probe_storage || !g->probe_scene || !count || count > LQ_MAX_DYNAMIC_LIGHTS || rays != 16) return 0;
    if (!g->probe_pipeline) {
        int metal = (SDL_GetGPUShaderFormats(g->device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
        SDL_GPUComputePipelineCreateInfo info = {0};
        info.code = metal ? lq_probes_comp_msl : lq_probes_comp_spv;
        info.code_size = metal ? sizeof(lq_probes_comp_msl) : sizeof(lq_probes_comp_spv);
        info.entrypoint = metal ? "main0" : "main";
        info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
        info.num_readonly_storage_buffers = 1; info.num_readwrite_storage_textures = 1; info.num_uniform_buffers = 1;
        info.threadcount_x = 64; info.threadcount_y = info.threadcount_z = 1;
        g->probe_pipeline = SDL_CreateGPUComputePipeline(g->device, &info);
        if (!g->probe_pipeline) { g->probe_storage = 0; return 0; }
    }
    struct lq_probe_uniforms *pu = &g->probe_uniforms;
    pu->size[3] = count; pu->box_min[3] = rays;
    for (int k = 0; k < 3; ++k) {
        pu->box_min[k] = box_min[k]; pu->box_size[k] = box_size[k];
        /* Write the union of this box and the last one, so its probes are cleared. */
        uint32_t low = box_min[k], high = box_min[k] + box_size[k];
        if (g->probe_written_size[0]) {
            if (g->probe_written_min[k] < low) low = g->probe_written_min[k];
            if (g->probe_written_min[k] + g->probe_written_size[k] > high) high = g->probe_written_min[k] + g->probe_written_size[k];
        }
        pu->dispatch_min[k] = low; pu->dispatch_size[k] = high - low;
        g->probe_written_min[k] = box_min[k]; g->probe_written_size[k] = box_size[k];
    }
    for (uint32_t i = 0; i < count; ++i) {
        memcpy(pu->light_position[i], lights[i]->position_radius, sizeof(pu->light_position[i]));
        memcpy(pu->light_color[i], lights[i]->color_strength, sizeof(pu->light_color[i]));
        memcpy(pu->light_falloff[i], lights[i]->falloff, sizeof(pu->light_falloff[i]));
    }
    g->probe_pending = 1; g->probe_dynamic_active = 1;
    return 1;
}
int LQ_GPUReadDynamicProbes(lq_gpu *g, uint8_t *pixels)
{
    if (!g || !g->probe_dynamic_bytes || !pixels) return 0;
    SDL_GPUTransferBuffer *download = Transfer(g, g->probe_dynamic_bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, NULL);
    SDL_GPUCommandBuffer *command = download ? SDL_AcquireGPUCommandBuffer(g->device) : NULL;
    int ok = 0;
    if (command) {
        if (g->probe_pending) ProbeCompute(g, command);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
        SDL_GPUTextureRegion source = {0};
        source.texture = g->probe_dynamic; source.w = (uint32_t)g->probe_size[0]; source.h = (uint32_t)g->probe_size[1];
        source.d = (uint32_t)g->probe_size[2] * 6;
        SDL_GPUTextureTransferInfo destination = {download, 0, source.w, source.h};
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
        if (fence && SDL_WaitForGPUFences(g->device, true, &fence, 1)) {
            void *read = SDL_MapGPUTransferBuffer(g->device, download, false);
            if (read) { memcpy(pixels, read, g->probe_dynamic_bytes); SDL_UnmapGPUTransferBuffer(g->device, download); ok = 1; }
        }
        if (fence) SDL_ReleaseGPUFence(g->device, fence);
    }
    if (download) SDL_ReleaseGPUTransferBuffer(g->device, download);
    return ok;
}
