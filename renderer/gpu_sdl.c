/* GPU buffers, textures, passes and readback. No Quake or VM types.
 * GPL-2.0-or-later. */
#include <SDL3/SDL.h>
#include "gpu_sdl.h"
#include "generated/world_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
struct lq_gpu {
    SDL_Window *window;
    SDL_GPUDevice *device;
    SDL_GPUGraphicsPipeline *pipeline, *overlay_pipeline;
    SDL_GPUSampler *repeat, *point, *linear;
    SDL_GPUBuffer *vertices, *indices;
    SDL_GPUTexture **textures, *colormap, *lightmap, *color, *depth, *overlay;
    uint32_t texture_count, draw_count, width, height;
    lq_draw *draws;
    int claimed;
};
static int Error(char *error, size_t size, const char *what)
{ snprintf(error, size, "%s: %s", what, SDL_GetError()); return 0; }
static SDL_GPUShader *Shader(lq_gpu *g, int fragment, int overlay)
{
    int metal = (SDL_GetGPUShaderFormats(g->device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
    SDL_GPUShaderCreateInfo info = {0};
    if (fragment) {
        info.code = overlay ? (metal ? lq_overlay_frag_msl : lq_overlay_frag_spv) : (metal ? lq_world_frag_msl : lq_world_frag_spv);
        info.code_size = overlay ? (metal ? sizeof(lq_overlay_frag_msl) : sizeof(lq_overlay_frag_spv)) : (metal ? sizeof(lq_world_frag_msl) : sizeof(lq_world_frag_spv));
    } else {
        info.code = overlay ? (metal ? lq_overlay_vert_msl : lq_overlay_vert_spv) : (metal ? lq_world_vert_msl : lq_world_vert_spv);
        info.code_size = overlay ? (metal ? sizeof(lq_overlay_vert_msl) : sizeof(lq_overlay_vert_spv)) : (metal ? sizeof(lq_world_vert_msl) : sizeof(lq_world_vert_spv));
    }
    info.entrypoint = metal ? "main0" : "main";
    info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
    info.num_samplers = fragment ? (overlay ? 1 : 3) : 0; info.num_uniform_buffers = overlay ? 0 : 1;
    return SDL_CreateGPUShader(g->device, &info);
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
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(lq_vertex, light_uv)}
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
    info.vertex_input_state = (SDL_GPUVertexInputState){&buffer, 1, attributes, 3};
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
    if (!g->pipeline || !g->overlay_pipeline || !g->repeat || !g->point || !g->linear) goto failed;
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
    if (g->vertices) SDL_ReleaseGPUBuffer(g->device, g->vertices);
    if (g->indices) SDL_ReleaseGPUBuffer(g->device, g->indices);
    for (uint32_t i = 0; i < g->texture_count; ++i)
        if (g->textures[i]) SDL_ReleaseGPUTexture(g->device, g->textures[i]);
    if (g->colormap) SDL_ReleaseGPUTexture(g->device, g->colormap);
    if (g->lightmap) SDL_ReleaseGPUTexture(g->device, g->lightmap);
    free(g->textures); free(g->draws);
    g->textures = NULL; g->draws = NULL; g->vertices = g->indices = NULL;
    g->colormap = g->lightmap = NULL; g->texture_count = g->draw_count = 0;
}
int LQ_GPUUpload(lq_gpu *g, const lq_scene *s, char *error, size_t size)
{
    if (!LQ_SceneCheck(s, error, size)) return 0;
    if (!SDL_WaitForGPUIdle(g->device)) return Error(error, size, "GPU wait before scene upload");
    ReleaseScene(g);
    g->textures = calloc(s->material_count, sizeof(*g->textures));
    g->draws = malloc((size_t)s->draw_count * sizeof(*g->draws));
    if (!g->textures || !g->draws) { SDL_SetError("out of memory"); goto failed; }
    g->texture_count = s->material_count; g->draw_count = s->draw_count;
    memcpy(g->draws, s->draws, (size_t)s->draw_count * sizeof(*g->draws));
    g->vertices = UploadBuffer(g, s->vertices, s->vertex_count * sizeof(*s->vertices), SDL_GPU_BUFFERUSAGE_VERTEX);
    g->indices = UploadBuffer(g, s->indices, s->index_count * sizeof(*s->indices), SDL_GPU_BUFFERUSAGE_INDEX);
    g->colormap = UploadImage(g, &s->colormap); g->lightmap = UploadImage(g, &s->lightmap);
    if (!g->vertices || !g->indices || !g->colormap || !g->lightmap) goto failed;
    for (uint32_t i = 0; i < g->texture_count; ++i) {
        g->textures[i] = UploadImage(g, &s->materials[i].indices);
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
    SDL_GPUTexture *depth = Texture(g, width, height, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    if (!color || !depth) {
        if (color) SDL_ReleaseGPUTexture(g->device, color);
        if (depth) SDL_ReleaseGPUTexture(g->device, depth); return 0;
    }
    if (g->color) SDL_ReleaseGPUTexture(g->device, g->color);
    if (g->depth) SDL_ReleaseGPUTexture(g->device, g->depth);
    g->color = color; g->depth = depth; g->width = width; g->height = height; return 1;
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
    command = SDL_AcquireGPUCommandBuffer(g->device);
    if (!command) return Error(error, size, "GPU command buffer");
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, g->window, &swapchain, &swap_width, &swap_height)) {
        SDL_CancelGPUCommandBuffer(command); return Error(error, size, "GPU swapchain");
    }
    SDL_GPUColorTargetInfo color = {0};
    SDL_GPUDepthStencilTargetInfo depth = {0};
    color.texture = g->color; color.clear_color = (SDL_FColor){0.02f, 0.02f, 0.025f, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR; color.store_op = SDL_GPU_STOREOP_STORE;
    depth.texture = g->depth; depth.clear_depth = 1; depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE; depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &color, 1, &depth);
    SDL_BindGPUGraphicsPipeline(pass, g->pipeline);
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
    LQ_ViewMatrix(v, (float)vw / vh, matrix);
    SDL_PushGPUVertexUniformData(command, 0, matrix, sizeof(matrix));
    for (uint32_t i = 0; i < g->draw_count; ++i) {
        const lq_draw *d = &g->draws[i];
        struct { float lights[4], parameters[4], camera[4]; } uniforms = {0};
        for (int j = 0; j < 4; ++j) uniforms.lights[j] = d->styles[j] == 255 ? 0 : v->lightstyles[d->styles[j]];
        uniforms.parameters[0] = v->time; uniforms.parameters[1] = (float)d->flags;
        memcpy(uniforms.camera, v->origin, sizeof(v->origin));
        SDL_GPUTextureSamplerBinding samplers[] = {
            {g->textures[d->material], g->repeat}, {g->colormap, g->point}, {g->lightmap, g->linear}
        };
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
        SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
        SDL_DrawGPUIndexedPrimitives(pass, d->index_count, 1, d->first_index, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    if (g->overlay) {
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
        if (g->color) SDL_ReleaseGPUTexture(g->device, g->color);
        if (g->depth) SDL_ReleaseGPUTexture(g->device, g->depth);
        if (g->pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->pipeline);
        if (g->overlay_pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->overlay_pipeline);
        if (g->overlay) SDL_ReleaseGPUTexture(g->device, g->overlay);
        if (g->repeat) SDL_ReleaseGPUSampler(g->device, g->repeat);
        if (g->point) SDL_ReleaseGPUSampler(g->device, g->point);
        if (g->linear) SDL_ReleaseGPUSampler(g->device, g->linear);
        if (g->claimed) SDL_ReleaseWindowFromGPUDevice(g->device, g->window);
        SDL_DestroyGPUDevice(g->device);
    }
    free(g);
}
