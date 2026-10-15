/* GPU buffers, textures, passes and readback. No Quake or VM types.
 * GPL-2.0-or-later. */
#include <SDL3/SDL.h>
#include "gpu_sdl.h"
#include "indexed_mips.h"
#include "generated/world_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
struct lq_gpu {
    SDL_Window *window;
    SDL_GPUDevice *device;
    SDL_GPUGraphicsPipeline *pipeline, *overlay_pipeline;
    SDL_GPUGraphicsPipeline *multisample[2];
    SDL_GPUGraphicsPipeline *liquid[3]; /* translucent liquids: 1x, 2x, 4x samples */
    SDL_GPUTexture *multisample_color;
    int smooth, samples, mipmaps, anisotropy;
    uint8_t *submerged; /* per scene draw: the face lies inside a liquid volume */
    float (*liquid_planes)[4]; /* per scene draw: liquid face normal and distance; zero otherwise */
    lq_mip_palette *mip_palette;
    SDL_GPUSampler *repeat, *point, *linear;
    SDL_GPUBuffer *vertices, *indices;
    SDL_GPUTexture **textures, *colormap, *lightmap, *bounce, *color, *depth, *overlay;
    float bounce_ratio; /* lightmap rows per bounce layer row */
    /* Enhanced irradiance probes: static layers per style, and a dynamic layer
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
    /* Enhanced shadows: one reversed-depth cube per runtime-light slot. A slot
     * is redrawn only when its light changes; static world casters persist. */
    SDL_GPUGraphicsPipeline *shadow_pipeline;
    SDL_GPUTexture *shadow_maps, *shadow_dummy;
    SDL_GPUSampler *shadow_sampler, *depth_sampler;
    SDL_GPUBuffer *casters; /* static world casters, then brush models at compiled positions */
    uint32_t caster_count, caster_world_count, shadow_size, shadow_updates, shadow_face_updates;
    /* Moving casters this frame (actors, brush models): bounds of frame draws. */
    struct lq_caster { float min[3], max[3]; uint32_t draw; } *frame_casters;
    uint32_t frame_caster_count, frame_caster_capacity;
    uint8_t shadow_dynamic_faces[LQ_MAX_RUNTIME_LIGHTS]; /* faces last drawn with moving casters */
    uint8_t shadow_brushes[LQ_MAX_RUNTIME_LIGHTS]; /* static_brush_shadows used for the cached cube */
    float shadow_keys[LQ_MAX_RUNTIME_LIGHTS][4];
    uint32_t shadow_sources[LQ_MAX_RUNTIME_LIGHTS];
    uint8_t shadow_valid[LQ_MAX_RUNTIME_LIGHTS];
    int claimed;
};
typedef struct {
    float lights[4], parameters[4], camera[4], dynamic[32][4];
    uint32_t bits[4];
    float bounce[4]; /* live style value per bounce layer */
} lq_surface_uniforms;
typedef struct {
    float position_range[LQ_MAX_RUNTIME_LIGHTS][4], color_strength[LQ_MAX_RUNTIME_LIGHTS][4];
    float falloff[LQ_MAX_RUNTIME_LIGHTS][4], spot[LQ_MAX_RUNTIME_LIGHTS][4];
    float shape[LQ_MAX_RUNTIME_LIGHTS][4]; /* inner cosine, source radius */
    uint32_t parameters[4];
    float tuning[4]; /* exposure, bounce layer ratio, bounce enabled, bounce gain */
    float probe_origin[4]; /* xyz, spacing */
    float probe_size[4]; /* probes per axis; w: 1 static, 2 dynamic */
    float probe_weights[4]; /* live style per static layer */
    float liquid[4]; /* translucent liquid opacity; zero when liquids are opaque or unlit */
} lq_light_uniforms;
typedef struct { float face_projection[16], light_origin[4]; } lq_shadow_uniforms;
#define LQ_SHADOW_NEAR 2.0f
#define LQ_SHADOW_SIZE 512
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
    if (overlay) return fragment ? SHADER(g, lq_overlay_frag, 1, 1, 0) : SHADER(g, lq_overlay_vert, 0, 0, 0);
    return fragment ? SHADER(g, lq_world_frag, 1, 8, 2) : SHADER(g, lq_world_vert, 0, 0, 1);
}
static SDL_GPUTexture *CubeArray(lq_gpu *g, uint32_t size, uint32_t cubes)
{
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY; info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
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
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(lq_vertex, position)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(lq_vertex, normal)}
    };
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    info.vertex_shader = SHADER(g, lq_shadow_vert, 0, 0, 1);
    info.fragment_shader = SHADER(g, lq_shadow_frag, 1, 0, 0);
    if (info.vertex_shader && info.fragment_shader) {
        info.vertex_input_state = (SDL_GPUVertexInputState){&buffer, 1, attributes, 2};
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
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.target_info = (SDL_GPUGraphicsPipelineTargetInfo){&color, 1, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, true, 0, 0, 0};
    g->pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    for (int i = 0; i < 2; ++i) {
        SDL_GPUSampleCount samples = i ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2;
        if (SDL_GPUTextureSupportsSampleCount(g->device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, samples) &&
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
    g->overlay_pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &info);
    SDL_ReleaseGPUShader(g->device, vertex); SDL_ReleaseGPUShader(g->device, fragment);
    vertex = fragment = NULL;
    g->repeat = Sampler(g, SDL_GPU_FILTER_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_REPEAT);
    g->point = Sampler(g, SDL_GPU_FILTER_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    g->linear = Sampler(g, SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    if (!g->pipeline || !g->liquid[0] || !g->overlay_pipeline || !g->repeat || !g->point || !g->linear) goto failed;
    if (!SDL_GPUTextureSupportsFormat(g->device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_CUBE_ARRAY,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        SDL_SetError("depth cube arrays are unsupported"); goto failed;
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
    if (!SDL_SetGPUSwapchainParameters(g->device, g->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
        return Error(error, size, "GPU options");
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
static SDL_GPUTexture *Upload3D(lq_gpu *g, uint32_t w, uint32_t h, uint32_t d, const uint8_t *pixels)
{
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_3D; info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width = w; info.height = h;
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
int LQ_GPUOverlay(lq_gpu *g, const lq_image *image, char *error, size_t size)
{
    SDL_GPUTexture *next = NULL;
    if (image) {
        if (image->channels != 4 || !image->pixels || !image->width || !image->height ||
            image->width > 8192 || image->height > 8192) {
            SDL_SetError("invalid overlay image"); return Error(error, size, "GPU overlay");
        }
        next = UploadImage(g, image);
        if (!next) return Error(error, size, "GPU overlay upload");
    }
    if (g->overlay) SDL_ReleaseGPUTexture(g->device, g->overlay);
    g->overlay = next; return 1;
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
    memset(g->shadow_dynamic_faces, 0, sizeof(g->shadow_dynamic_faces));
    memset(g->shadow_valid, 0, sizeof(g->shadow_valid));
    for (uint32_t i = 0; i < g->texture_count; ++i)
        if (g->textures[i]) SDL_ReleaseGPUTexture(g->device, g->textures[i]);
    if (g->colormap) SDL_ReleaseGPUTexture(g->device, g->colormap);
    if (g->lightmap) SDL_ReleaseGPUTexture(g->device, g->lightmap);
    if (g->bounce) SDL_ReleaseGPUTexture(g->device, g->bounce);
    g->bounce = NULL; g->bounce_ratio = 0;
    if (g->probe_static) SDL_ReleaseGPUTexture(g->device, g->probe_static);
    if (g->probe_dynamic) SDL_ReleaseGPUTexture(g->device, g->probe_dynamic);
    if (g->probe_transfer) SDL_ReleaseGPUTransferBuffer(g->device, g->probe_transfer);
    g->probe_static = g->probe_dynamic = NULL; g->probe_transfer = NULL;
    g->probe_dynamic_bytes = 0; g->probe_dynamic_active = 0; g->probe_origin[3] = 0;
    free(g->textures); free(g->draws); free(g->submerged); free(g->liquid_planes);
    g->textures = NULL; g->draws = NULL; g->submerged = NULL; g->liquid_planes = NULL; g->vertices = g->indices = NULL;
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
        g->frame_casters = next; g->frame_caster_capacity = f->draw_count;
    }
    g->frame_caster_count = 0;
    for (uint32_t i = 0; i < f->draw_count; ++i) {
        lq_draw d = f->draws[i];
        if (d.flags & ~(LQ_SURFACE_SKY|LQ_SURFACE_WATER|LQ_SURFACE_MODEL|LQ_SURFACE_MASKED|LQ_SURFACE_WEAPON|LQ_SURFACE_ACTOR) ||
            d.material >= f->material_count || d.first_index > f->index_count ||
            d.index_count > f->index_count - d.first_index || !d.index_count || d.index_count % 3) {
            SDL_SetError("invalid frame triangle range"); return Error(error, size, "GPU frame draws");
        }
        d.material = f->materials[d.material].resource_id; g->frame_draws[i] = d;
        /* Actors and moving brush faces cast; the view weapon, particles,
         * sprites, flames and liquids do not. */
        int brush = !(d.flags & LQ_SURFACE_MODEL), actor = (d.flags & LQ_SURFACE_ACTOR) != 0;
        if ((brush || actor) && !(d.flags & (LQ_SURFACE_WEAPON|LQ_SURFACE_MASKED|LQ_SURFACE_SKY|LQ_SURFACE_WATER))) {
            struct lq_caster *c = &g->frame_casters[g->frame_caster_count++];
            c->draw = i;
            for (int k = 0; k < 3; ++k) { c->min[k] = INFINITY; c->max[k] = -INFINITY; }
            for (uint32_t j = 0; j < d.index_count; ++j) {
                const float *p = f->vertices[f->indices[d.first_index + j]].position;
                for (int k = 0; k < 3; ++k) { c->min[k] = fminf(c->min[k], p[k]); c->max[k] = fmaxf(c->max[k], p[k]); }
            }
        }
    }
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
    /* Liquid faces come in coplanar front/back pairs; only the side facing the
     * camera is drawn, as in Quake, or blending picks one per face at random. */
    g->liquid_planes = calloc(s->draw_count ? s->draw_count : 1, sizeof(*g->liquid_planes));
    if (!g->liquid_planes) { SDL_SetError("out of memory"); goto failed; }
    for (uint32_t i = 0; i < s->draw_count; ++i) if (s->draws[i].flags & LQ_SURFACE_WATER) {
        const lq_vertex *v = &s->vertices[s->indices[s->draws[i].first_index]];
        memcpy(g->liquid_planes[i], v->normal, sizeof(v->normal));
        g->liquid_planes[i][3] = v->normal[0]*v->position[0] + v->normal[1]*v->position[1] + v->normal[2]*v->position[2];
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
    if (!g->vertices || !g->indices || !g->colormap || !g->lightmap || !g->bounce) goto failed;
    if (s->probe_static) {
        const uint32_t *n = s->probe_size;
        g->probe_static = Upload3D(g, n[0], n[1], n[2] * 24, s->probe_static);
        g->probe_dynamic = Upload3D(g, n[0], n[1], n[2] * 6, NULL);
        memcpy(g->probe_origin, s->probe_origin, sizeof(s->probe_origin)); g->probe_origin[3] = s->probe_spacing;
        for (int k = 0; k < 3; ++k) g->probe_size[k] = (float)n[k];
        memcpy(g->probe_styles, s->probe_styles, 4);
        g->probe_dynamic_bytes = n[0] * n[1] * n[2] * 6 * 4;
    } else {
        g->probe_static = Upload3D(g, 1, 1, 1, NULL); g->probe_dynamic = Upload3D(g, 1, 1, 1, NULL);
    }
    if (!g->probe_static || !g->probe_dynamic) goto failed;
    /* Every opaque world face may occlude; water and similar liquids do not. */
    uint32_t *casters = malloc((size_t)s->index_count * sizeof(*casters));
    if (!casters) { SDL_SetError("out of memory"); goto failed; }
    for (int brushes = 0; brushes < 2; ++brushes) {
        for (uint32_t i = 0; i < s->draw_count; ++i) {
            int world = !s->world_draw_end || (i >= s->world_draw_begin && i < s->world_draw_end);
            if ((s->draws[i].flags & LQ_SURFACE_WATER) || world == brushes) continue;
            memcpy(casters + g->caster_count, s->indices + s->draws[i].first_index, s->draws[i].index_count * sizeof(*casters));
            g->caster_count += s->draws[i].index_count;
        }
        if (!brushes) g->caster_world_count = g->caster_count;
    }
    if (g->caster_count) g->casters = UploadBuffer(g, casters, g->caster_count * sizeof(*casters), SDL_GPU_BUFFERUSAGE_INDEX);
    free(casters);
    if (g->caster_count && !g->casters) goto failed;
    for (uint32_t i = 0; i < g->texture_count; ++i) {
        g->textures[i] = UploadIndexed(g, &s->materials[i].indices);
        if (!g->textures[i]) goto failed;
    }
    return 1;
failed:
    Error(error, size, "GPU scene upload"); ReleaseScene(g); return 0;
}
static int Targets(lq_gpu *g, uint32_t width, uint32_t height)
{
    if (g->width == width && g->height == height) return 1;
    SDL_GPUTexture *color = Texture(g, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUSampleCount samples = g->samples == 4 ? SDL_GPU_SAMPLECOUNT_4 : g->samples == 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.width = width; info.height = height;
    info.layer_count_or_depth = info.num_levels = 1; info.sample_count = samples;
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    SDL_GPUTexture *depth = SDL_CreateGPUTexture(g->device, &info), *multisample = NULL;
    if (g->samples) {
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        multisample = SDL_CreateGPUTexture(g->device, &info);
    }
    if (!color || !depth || (g->samples && !multisample)) {
        if (multisample) SDL_ReleaseGPUTexture(g->device, multisample);
        if (color) SDL_ReleaseGPUTexture(g->device, color);
        if (depth) SDL_ReleaseGPUTexture(g->device, depth); return 0;
    }
    if (g->color) SDL_ReleaseGPUTexture(g->device, g->color);
    if (g->depth) SDL_ReleaseGPUTexture(g->device, g->depth);
    if (g->multisample_color) SDL_ReleaseGPUTexture(g->device, g->multisample_color);
    g->multisample_color = multisample;
    g->color = color; g->depth = depth; g->width = width; g->height = height; return 1;
}
static void SurfaceUniforms(lq_surface_uniforms *uniforms, const lq_gpu *g,
    const lq_view *view, const lq_draw *draw)
{
    memset(uniforms,0,sizeof(*uniforms));
    memcpy(uniforms->dynamic,view->dynamic_lights,sizeof(uniforms->dynamic));
    uniforms->bits[0] = draw->light_bits;
    if (view->enhanced_lighting && (draw->flags & LQ_SURFACE_ACTOR)) { /* every slot; range culls in the shader */
        uint32_t count = view->runtime_light_count > LQ_MAX_RUNTIME_LIGHTS ? LQ_MAX_RUNTIME_LIGHTS : view->runtime_light_count;
        uniforms->bits[0] = count >= 32 ? UINT32_MAX : (1u << count) - 1;
    }
    uniforms->parameters[0] = view->time; uniforms->parameters[1] = (float)draw->flags;
    uniforms->parameters[2] = (float)g->smooth; uniforms->parameters[3] = (float)g->mipmaps;
    memcpy(uniforms->camera,view->origin,sizeof(view->origin)); uniforms->camera[3] = (float)g->anisotropy;
    for (int j = 0; j < 4; ++j) {
        uniforms->lights[j] = draw->styles[j] == 255 ? 0 : view->lightstyles[draw->styles[j]];
        uniforms->bounce[j] = draw->bounce_styles[j] == 255 ? 0 : view->lightstyles[draw->bounce_styles[j]];
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
        uniforms->shape[i][0] = light->spot_inner; uniforms->shape[i][1] = light->source_radius;
        uniforms->shape[i][2] = light->submerged;
    }
    uniforms->parameters[0] = count;
    uniforms->parameters[1] = view->enhanced_lighting != 0;
    uniforms->parameters[2] = g->shadow_size ? g->shadow_size : 1;
    uniforms->parameters[3] = view->enhanced_lighting && g->shadow_maps && !view->disable_shadows;
    uniforms->tuning[0] = view->exposure > 0 ? view->exposure : 1;
    uniforms->tuning[1] = g->bounce_ratio;
    uniforms->tuning[2] = view->enhanced_lighting && g->bounce_ratio > 0 && !view->disable_bounce;
    uniforms->tuning[3] = view->bounce_gain > 0 ? view->bounce_gain : 1;
    memcpy(uniforms->probe_origin, g->probe_origin, sizeof(uniforms->probe_origin));
    for (int k = 0; k < 3; ++k) uniforms->probe_size[k] = g->probe_size[k];
    uniforms->probe_size[3] = view->enhanced_lighting && !view->disable_bounce && g->probe_origin[3] > 0 ?
        1.0f + (g->probe_dynamic_active ? 2.0f : 0.0f) : 0.0f;
    for (int k = 0; k < 4; ++k)
        uniforms->probe_weights[k] = g->probe_styles[k] == 255 ? 0 : view->lightstyles[g->probe_styles[k]];
    uniforms->liquid[0] = view->enhanced_lighting && view->water_alpha > 0 && view->water_alpha < 1 ? view->water_alpha : 0;
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
    if (g->shadow_maps) return 1;
    g->shadow_maps = CubeArray(g, LQ_SHADOW_SIZE, LQ_MAX_RUNTIME_LIGHTS);
    if (!g->shadow_maps) return 0;
    g->shadow_size = LQ_SHADOW_SIZE; memset(g->shadow_valid, 0, sizeof(g->shadow_valid));
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
/* A slot's cube is redrawn in full when its light changes. Otherwise only the
 * faces that moving casters touch now, or touched when last drawn, are redrawn
 * (the latter to erase their old shadows); static world shadows persist. */
static void RenderShadows(lq_gpu *g, SDL_GPUCommandBuffer *command, const lq_view *v)
{
    uint32_t count = v->runtime_light_count > LQ_MAX_RUNTIME_LIGHTS ? LQ_MAX_RUNTIME_LIGHTS : v->runtime_light_count;
    uint8_t brushes = v->static_brush_shadows != 0;
    g->shadow_updates = g->shadow_face_updates = 0;
    for (uint32_t slot = 0; slot < count; ++slot) {
        const lq_runtime_light *light = &v->runtime_lights[slot];
        if (light->position_radius[3] <= 0 || !g->casters) continue;
        int changed = !g->shadow_valid[slot] || g->shadow_sources[slot] != light->source_index ||
            g->shadow_brushes[slot] != brushes ||
            memcmp(g->shadow_keys[slot], light->position_radius, sizeof(g->shadow_keys[slot]));
        uint8_t touched = 0, caster_faces[64];
        for (uint32_t i = 0; i < g->frame_caster_count; ++i) {
            uint8_t faces = CasterFaces(&g->frame_casters[i], light->position_radius);
            if (i < 64) caster_faces[i] = faces;
            touched |= faces;
        }
        uint8_t redraw = changed ? 0x3f : (uint8_t)(touched | g->shadow_dynamic_faces[slot]);
        if (!redraw) continue;
        lq_shadow_uniforms uniforms = {{0}, {light->position_radius[0], light->position_radius[1], light->position_radius[2], 1}};
        for (int face = 0; face < 6; ++face) {
            if (!(redraw & (1 << face))) continue;
            SDL_GPUDepthStencilTargetInfo depth = {0};
            depth.texture = g->shadow_maps; depth.layer = (Uint8)(slot * 6 + face);
            depth.clear_depth = 0; depth.load_op = SDL_GPU_LOADOP_CLEAR; depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, NULL, 0, &depth);
            SDL_BindGPUGraphicsPipeline(pass, g->shadow_pipeline);
            SDL_GPUViewport viewport = {0, 0, (float)g->shadow_size, (float)g->shadow_size, 0, 1};
            SDL_SetGPUViewport(pass, &viewport);
            SDL_GPUBufferBinding vertex = {g->vertices, 0}, index = {g->casters, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
            SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            ShadowFace(face, light->position_radius, uniforms.face_projection);
            SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));
            SDL_DrawGPUIndexedPrimitives(pass, brushes ? g->caster_count : g->caster_world_count, 1, 0, 0, 0);
            if (touched & (1 << face)) {
                vertex = (SDL_GPUBufferBinding){g->frame_vertices, 0}; index = (SDL_GPUBufferBinding){g->frame_indices, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
                SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                for (uint32_t i = 0; i < g->frame_caster_count; ++i) {
                    if (i < 64 && !(caster_faces[i] & (1 << face))) continue;
                    const lq_draw *d = &g->frame_draws[g->frame_casters[i].draw];
                    SDL_DrawGPUIndexedPrimitives(pass, d->index_count, 1, d->first_index, 0, 0);
                }
            }
            SDL_EndGPURenderPass(pass);
            ++g->shadow_face_updates;
        }
        memcpy(g->shadow_keys[slot], light->position_radius, sizeof(g->shadow_keys[slot]));
        g->shadow_sources[slot] = light->source_index; g->shadow_valid[slot] = 1;
        g->shadow_brushes[slot] = brushes; g->shadow_dynamic_faces[slot] = touched;
        ++g->shadow_updates;
    }
}
uint32_t LQ_GPUShadowUpdates(const lq_gpu *g) { return g ? g->shadow_updates : 0; }
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
#define LQ_SHADER_SUBMERGED 64 /* shader-only surface flag: face lies inside a liquid */
static void DrawSurface(lq_gpu *g, SDL_GPUCommandBuffer *command, SDL_GPURenderPass *pass, const lq_view *v,
    const lq_draw *d, uint32_t extra_flags, SDL_GPUTexture *texture, SDL_GPUTexture *shadows)
{
    lq_surface_uniforms uniforms;
    SurfaceUniforms(&uniforms,g,v,d);
    uniforms.parameters[1] = (float)(d->flags | extra_flags);
    SDL_GPUTextureSamplerBinding samplers[] = {
        {texture, g->repeat}, {g->colormap, g->point}, {g->lightmap, g->linear},
        {shadows, g->shadow_sampler}, {shadows, g->depth_sampler}, {g->bounce, g->linear},
        {g->probe_static, g->linear}, {g->probe_dynamic, g->linear}
    };
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 8);
    SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
    SDL_DrawGPUIndexedPrimitives(pass, d->index_count, 1, d->first_index, 0, 0);
}
int LQ_GPURender(lq_gpu *g, const lq_view *v, uint32_t width, uint32_t height, char *error, size_t size)
{
    SDL_GPUCommandBuffer *command;
    SDL_GPUTexture *swapchain = NULL;
    uint32_t swap_width = 0, swap_height = 0;
    float matrix[16];
    if (!g->vertices || !g->indices) { SDL_SetError("scene not uploaded"); return Error(error, size, "GPU render"); }
    if (!width || !height || width > 8192 || height > 8192 || !Targets(g, width, height))
        return Error(error, size, "GPU render target size/allocation");
    if (v->enhanced_lighting && v->runtime_light_count && !v->disable_shadows && !ShadowMaps(g))
        return Error(error, size, "GPU shadow maps");
    command = SDL_AcquireGPUCommandBuffer(g->device);
    if (!command) return Error(error, size, "GPU command buffer");
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, g->window, &swapchain, &swap_width, &swap_height)) {
        SDL_CancelGPUCommandBuffer(command); return Error(error, size, "GPU swapchain");
    }
    g->shadow_updates = 0;
    if (v->enhanced_lighting && g->shadow_maps && !v->disable_shadows) RenderShadows(g, command, v);
    lq_light_uniforms light_uniforms;
    LightUniforms(&light_uniforms, g, v);
    SDL_PushGPUFragmentUniformData(command, 1, &light_uniforms, sizeof(light_uniforms));
    SDL_GPUTexture *shadows = g->shadow_maps && v->enhanced_lighting ? g->shadow_maps : g->shadow_dummy;
    SDL_GPUColorTargetInfo color = {0};
    SDL_GPUDepthStencilTargetInfo depth = {0};
    color.texture = g->samples ? g->multisample_color : g->color; color.clear_color = (SDL_FColor){0.02f, 0.02f, 0.025f, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR; color.store_op = SDL_GPU_STOREOP_STORE;
    if (g->samples) { color.resolve_texture = g->color; color.store_op = SDL_GPU_STOREOP_RESOLVE; }
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
            if (translucent && g->liquid_planes) {
                const float *p = g->liquid_planes[i];
                if (p[0]*v->origin[0] + p[1]*v->origin[1] + p[2]*v->origin[2] < p[3]) continue;
            }
            DrawSurface(g, command, pass, v, d, g->submerged && g->submerged[i] ? LQ_SHADER_SUBMERGED : 0,
                g->textures[d->material], shadows);
        }
        if (!g->frame_draw_count) continue;
        vertex = (SDL_GPUBufferBinding){g->frame_vertices, 0};
        index = (SDL_GPUBufferBinding){g->frame_indices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
        SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        for (uint32_t i = 0; i < g->frame_draw_count; ++i) {
            const lq_draw *d = &g->frame_draws[i];
            if (liquid && translucent != ((d->flags & LQ_SURFACE_WATER) != 0)) continue;
            viewport.max_depth = d->flags & LQ_SURFACE_WEAPON ? 0.3f : 1.0f;
            SDL_SetGPUViewport(pass, &viewport);
            DrawSurface(g, command, pass, v, d, 0, g->frame_textures[d->material], shadows);
        }
    }
    SDL_EndGPURenderPass(pass);
    if (g->overlay) {
        color.texture = g->color; color.resolve_texture = NULL; color.store_op = SDL_GPU_STOREOP_STORE;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        pass = SDL_BeginGPURenderPass(command, &color, 1, NULL);
        SDL_BindGPUGraphicsPipeline(pass, g->overlay_pipeline);
        SDL_GPUTextureSamplerBinding sampler = {g->overlay, g->point};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    if (swapchain) {
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
int LQ_GPUReadPixels(lq_gpu *g, uint8_t **pixels, uint32_t *width, uint32_t *height, char *error, size_t size)
{
    uint32_t bytes = g->width * g->height * 4;
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
    *pixels = malloc(bytes);
    if (*pixels) memcpy(*pixels, mapped, bytes);
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
        if (g->depth) SDL_ReleaseGPUTexture(g->device, g->depth);
        if (g->multisample_color) SDL_ReleaseGPUTexture(g->device, g->multisample_color);
        for (int i = 0; i < 2; ++i) if (g->multisample[i]) SDL_ReleaseGPUGraphicsPipeline(g->device, g->multisample[i]);
        for (int i = 0; i < 3; ++i) if (g->liquid[i]) SDL_ReleaseGPUGraphicsPipeline(g->device, g->liquid[i]);
        if (g->pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipeline);
        if (g->overlay_pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->overlay_pipeline);
        if (g->overlay) SDL_ReleaseGPUTexture(g->device, g->overlay);
        if (g->shadow_maps) SDL_ReleaseGPUTexture(g->device, g->shadow_maps);
        if (g->shadow_dummy) SDL_ReleaseGPUTexture(g->device, g->shadow_dummy);
        if (g->shadow_sampler) SDL_ReleaseGPUSampler(g->device, g->shadow_sampler);
        if (g->depth_sampler) SDL_ReleaseGPUSampler(g->device, g->depth_sampler);
        if (g->shadow_pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->shadow_pipeline);
        if (g->repeat) SDL_ReleaseGPUSampler(g->device, g->repeat);
        if (g->point) SDL_ReleaseGPUSampler(g->device, g->point);
        if (g->linear) SDL_ReleaseGPUSampler(g->device, g->linear);
        if (g->claimed) SDL_ReleaseWindowFromGPUDevice(g->device, g->window);
        SDL_DestroyGPUDevice(g->device);
    }
    free(g->mip_palette); free(g->frame_draws); free(g->frame_casters); free(g);
}
