/* Community pack models for Luminous: replacement MD3 and MDL meshes and model
 * skins, read as DarkPlaces reads them (docs/MATERIAL_PACKS.md).
 * MD3 layout: Quake III Arena, Copyright (C) 1999-2005 Id Software, Inc.
 * GPL-2.0-or-later. */
#include "quakedef.h"
#include "pack_models.h"
#include "quake_scene.h"
#include "pack_archive.h"
#include "pack_images.h"
#include <ctype.h>
extern float r_avertexnormals[162][3];

#define PACK_MODEL_FRAMES 1024
#define PACK_MODEL_VERTICES 65536
#define PACK_MODEL_CORNERS (3 * 131072)

static lq_pack_model *models[MAX_MODELS];
static const model_t *checked[MAX_MODELS]; /* models looked up, with or without a find */
static int model_count, checked_count;
static char loaded_pack[MAX_OSPATH];
static uint8_t placeholder; /* the 1x1 palette image of a mesh drawn only from pack images */

static void FreeModel(lq_pack_model *m)
{
    if (!m) return;
    for (uint32_t s = 0; m->surfaces && s < m->surface_count; ++s) {
        for (uint32_t k = 0; m->surfaces[s].skins && k < m->skin_count; ++k) {
            free(m->surfaces[s].skins[k].pack_color.pixels);
            free(m->surfaces[s].skins[k].pack_normal.pixels);
        }
        free(m->surfaces[s].skins);
    }
    free(m->surfaces); free(m->positions); free(m->normals); free(m->uv); free(m->corners);
    free(m);
}
void LQ_PackModelsClear(void)
{
    for (int i = 0; i < model_count; ++i) FreeModel(models[i]);
    model_count = checked_count = 0; loaded_pack[0] = 0;
}
const lq_pack_model *LQ_PackModel(const model_t *model)
{
    for (int i = 0; i < model_count; ++i) if (models[i]->source == model) return models[i];
    return NULL;
}
/* A file of the pack itself (never the game's own folders, which hold the
 * originals): from the .pk3 `archive`, or under the folder `pack`. */
static uint8_t *PackRead(const char *pack, lq_archive *archive, const char *name, size_t *size)
{
    if (archive) {
        lq_archive_blob blob = {0};
        if (!LQ_ArchiveReadRaw(archive, name, &blob) || !LQ_ArchiveExpand(&blob)) return NULL;
        *size = blob.size; return blob.data;
    }
    char path[MAX_OSPATH];
    if (snprintf(path, sizeof(path), "%s/%s", pack, name) >= (int)sizeof(path)) return NULL;
    int h, length = Sys_FileOpenRead(path, &h);
    if (length <= 0) { if (length == 0) Sys_FileClose(h); return NULL; }
    uint8_t *bytes = malloc((size_t)length);
    if (bytes && Sys_FileRead(h, bytes, length) == length) { Sys_FileClose(h); *size = (size_t)length; return bytes; }
    free(bytes); Sys_FileClose(h);
    return NULL;
}
static int32_t Int(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return LittleLong(v); }
static float Float(const uint8_t *p) { float v; memcpy(&v, p, 4); return LittleFloat(v); }
static int Inside(size_t size, size_t offset, size_t length) { return offset <= size && length <= size - offset; }
/* Linear-light mean of a palette image's non-fullbright texels (0 when none),
 * the brightness a pack image is matched to (lq_pack_brightness). */
static void PaletteMean(const byte *pixels, size_t count, double *sum, size_t *texels)
{
    for (size_t i = 0; i < count; ++i) {
        if (pixels[i] >= 224) continue;
        const byte *c = host_basepal + 3 * pixels[i];
        *sum += 0.2126 * pow(c[0] / 255.0, 2.2) + 0.7152 * pow(c[1] / 255.0, 2.2) + 0.0722 * pow(c[2] / 255.0, 2.2);
        ++*texels;
    }
}
static float OriginalMean(model_t *model)
{
    double sum = 0; size_t texels = 0;
    if (model->type == mod_alias) {
        aliashdr_t *header = Mod_Extradata(model);
        mdl_t *mdl = (mdl_t *)((byte *)header + header->model);
        maliasskindesc_t *skin = (maliasskindesc_t *)((byte *)header + header->skindesc);
        if (mdl->numskins < 1) return 0;
        if (skin->type == ALIAS_SKIN_GROUP) skin = ((maliasskingroup_t *)((byte *)header + skin->skin))->skindescs;
        PaletteMean((byte *)header + skin->skin, (size_t)mdl->skinwidth * mdl->skinheight, &sum, &texels);
    } else if (model->type == mod_brush) {
        for (int i = 0; i < model->numtextures; ++i) {
            texture_t *t = model->textures[i];
            if (t) PaletteMean((byte *)t + t->offsets[0], (size_t)t->width * t->height, &sum, &texels);
        }
    }
    return texels ? (float)(sum / texels) : 0;
}
/* A pack material from the image `base` (a path without extension) and its
 * _norm and _glow or _luma companions, as world textures are found. */
static int LoadSkin(const char *pack, lq_archive *archive, const char *base, float original, lq_material *out)
{
    char name[MAX_QPATH];
    lq_image glow = {0};
    memset(out, 0, sizeof(*out));
    if (!LQ_QuakeLoadImage(pack, archive, base, &out->pack_color)) return 0;
    LQ_PackSetAlpha(&out->pack_color, 0);
    snprintf(name, sizeof(name), "%s_glow", base);
    if (!LQ_QuakeLoadImage(pack, archive, name, &glow)) {
        snprintf(name, sizeof(name), "%s_luma", base);
        LQ_QuakeLoadImage(pack, archive, name, &glow);
    }
    if (glow.pixels) LQ_PackFoldGlow(&out->pack_color, &glow);
    free(glow.pixels);
    snprintf(name, sizeof(name), "%s_norm", base);
    if (!LQ_QuakeLoadImage(pack, archive, name, &out->pack_normal)) {
        /* No _norm: a height-only _bump becomes one, as in DarkPlaces. */
        lq_image bump = {0};
        snprintf(name, sizeof(name), "%s_bump", base);
        if (LQ_QuakeLoadImage(pack, archive, name, &bump)) LQ_PackNormalFromBump(&bump, 4, &out->pack_normal);
        free(bump.pixels);
    }
    /* Its gloss rides in the normal map's blue, as for world textures. */
    lq_image gloss = {0};
    snprintf(name, sizeof(name), "%s_gloss", base);
    LQ_QuakeLoadImage(pack, archive, name, &gloss);
    LQ_PackGlossIntoNormal(&out->pack_normal, &gloss);
    free(gloss.pixels);
    /* At the size the world's pack was loaded (High-res textures). */
    for (int h = 0; h < LQ_QuakePackHalvings(); ++h) {
        lq_image *images[2] = {&out->pack_color, &out->pack_normal};
        for (int k = 0; k < 2; ++k) {
            lq_image next;
            if (images[k]->pixels && (images[k]->width > 4 || images[k]->height > 4) && LQ_PackHalve(images[k], k, &next)) {
                free(images[k]->pixels); *images[k] = next;
            }
        }
    }
    float mean = LQ_PackLinearMean(&out->pack_color);
    out->pack_gain = original > 0 && mean > 1e-5f ? fminf(fmaxf(original / mean, 0.5f), 2.0f) : 1;
    out->indices = (lq_image){1, 1, 1, &placeholder};
    return 1;
}
/* An MD3 surface's shader names its image: textures/<name>, then <name>, its
 * extension dropped, as DarkPlaces looks it up. */
static int LoadShader(const char *pack, lq_archive *archive, const char *shader, float original, lq_material *out)
{
    char base[MAX_QPATH], path[MAX_QPATH];
    snprintf(base, sizeof(base), "%s", shader);
    char *dot = strrchr(base, '.'), *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    if (!base[0]) return 0;
    for (int lower = 0; lower < 2; ++lower) {
        if (lower) for (char *c = base; *c; ++c) *c = (char)tolower((unsigned char)*c);
        snprintf(path, sizeof(path), "textures/%s", base);
        if (LoadSkin(pack, archive, path, original, out) || LoadSkin(pack, archive, base, original, out)) return 1;
    }
    return 0;
}
static lq_pack_model *NewModel(const model_t *source, uint32_t frames, uint32_t vertices, uint32_t corners,
    uint32_t surfaces, uint32_t skins)
{
    lq_pack_model *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->source = source; m->replaces = 1;
    m->frame_count = frames; m->vertex_count = vertices; m->corner_count = corners;
    m->surface_count = surfaces; m->skin_count = skins;
    m->positions = malloc((size_t)frames * vertices * sizeof(*m->positions));
    m->normals = malloc((size_t)frames * vertices * sizeof(*m->normals));
    m->uv = malloc((size_t)vertices * sizeof(*m->uv));
    m->corners = malloc((size_t)corners * sizeof(*m->corners));
    m->surfaces = calloc(surfaces, sizeof(*m->surfaces));
    int ok = m->positions && m->normals && m->uv && m->corners && m->surfaces;
    for (uint32_t s = 0; ok && s < surfaces; ++s) ok = (m->surfaces[s].skins = calloc(skins, sizeof(lq_material))) != NULL;
    if (!ok) { FreeModel(m); return NULL; }
    return m;
}
/* Quake III's MD3: surfaces of their own vertices, each frame a full copy of
 * them (x, y, z in 1/64 units and a latitude/longitude normal). */
static lq_pack_model *ParseMD3(const model_t *source, const uint8_t *d, size_t size, const char *pack,
    lq_archive *archive, float original)
{
    if (size < 108 || Int(d + 4) != 15) return NULL;
    int32_t frames = Int(d + 76), surfaces = Int(d + 84), first = Int(d + 100);
    if (frames < 1 || frames > PACK_MODEL_FRAMES || surfaces < 1 || surfaces > 32 || first < 0) return NULL;
    int64_t vertices = 0, corners = 0;
    size_t at = (size_t)first;
    for (int32_t s = 0; s < surfaces; ++s) {
        if (!Inside(size, at, 108) || memcmp(d + at, "IDP3", 4) || Int(d + at + 72) != frames) return NULL;
        int32_t nv = Int(d + at + 80), nt = Int(d + at + 84), end = Int(d + at + 104);
        if (nv < 0 || nt < 0 || end < 108 || !Inside(size, at, (size_t)end)) return NULL;
        vertices += nv; corners += 3 * (int64_t)nt; at += (size_t)end;
    }
    if (!vertices || vertices > PACK_MODEL_VERTICES || !corners || corners > PACK_MODEL_CORNERS) return NULL;
    lq_pack_model *m = NewModel(source, (uint32_t)frames, (uint32_t)vertices, (uint32_t)corners, (uint32_t)surfaces, 1);
    if (!m) return NULL;
    uint32_t vertex = 0, corner = 0, kept = 0;
    at = (size_t)first;
    for (int32_t s = 0; s < surfaces; ++s) {
        const uint8_t *h = d + at;
        int32_t shaders = Int(h + 76), nv = Int(h + 80), nt = Int(h + 84);
        size_t triangles = at + (size_t)Int(h + 88), names = at + (size_t)Int(h + 92);
        size_t st = at + (size_t)Int(h + 96), xyz = at + (size_t)Int(h + 100);
        if (Int(h + 88) < 0 || Int(h + 92) < 0 || Int(h + 96) < 0 || Int(h + 100) < 0 ||
            !Inside(size, triangles, (size_t)nt * 12) || !Inside(size, st, (size_t)nv * 8) ||
            !Inside(size, xyz, (size_t)nv * frames * 8) || (shaders > 0 && !Inside(size, names, 68))) { FreeModel(m); return NULL; }
        lq_pack_surface *surface = &m->surfaces[kept];
        char shader[65] = {0};
        if (shaders > 0) memcpy(shader, d + names, 64);
        int textured = LoadShader(pack, archive, shader, original, &surface->skins[0]);
        for (int32_t v = 0; v < nv; ++v) {
            m->uv[vertex + v][0] = Float(d + st + v * 8); m->uv[vertex + v][1] = Float(d + st + v * 8 + 4);
            for (int32_t f = 0; f < frames; ++f) {
                const uint8_t *p = d + xyz + ((size_t)f * nv + v) * 8;
                float *out = m->positions[(size_t)f * vertices + vertex + v], *n = m->normals[(size_t)f * vertices + vertex + v];
                for (int k = 0; k < 3; ++k) { int16_t c; memcpy(&c, p + 2 * k, 2); out[k] = LittleShort(c) / 64.0f; }
                float lat = p[7] * (float)M_PI / 128, lng = p[6] * (float)M_PI / 128;
                n[0] = cosf(lat) * sinf(lng); n[1] = sinf(lat) * sinf(lng); n[2] = cosf(lng);
            }
        }
        /* A surface without an image is left out: DarkPlaces would draw it
         * untextured (v_light.mdl's bolt guides, eyes.mdl's "01 - Default"). */
        if (textured) {
            surface->first_corner = corner;
            for (int32_t t = 0; t < nt * 3; ++t) {
                int32_t index = Int(d + triangles + (size_t)t * 4);
                if (index < 0 || index >= nv) { FreeModel(m); return NULL; }
                m->corners[corner++] = vertex + (uint32_t)index;
            }
            surface->corner_count = corner - surface->first_corner;
            ++kept;
        }
        vertex += (uint32_t)nv; at += (size_t)Int(h + 104);
    }
    for (int32_t s = (int32_t)kept; s < surfaces; ++s) { free(m->surfaces[s].skins); m->surfaces[s].skins = NULL; }
    m->surface_count = kept; m->corner_count = corner;
    if (!kept) { FreeModel(m); return NULL; }
    return m;
}
/* A replacement MDL: Quake's own format, with skins from the pack
 * (progs/<model>.mdl_<n>) or its own. Each frame shows its first pose;
 * seam vertices on back faces get their own copy at the skin's right half. */
static lq_pack_model *ParseMDL(const model_t *source, const uint8_t *d, size_t size, const char *pack,
    lq_archive *archive, float original)
{
    if (size < 84 || Int(d + 4) != ALIAS_VERSION) return NULL;
    float scale[3], origin[3];
    for (int k = 0; k < 3; ++k) { scale[k] = Float(d + 8 + 4 * k); origin[k] = Float(d + 20 + 4 * k); }
    int32_t skins = Int(d + 48), width = Int(d + 52), height = Int(d + 56);
    int32_t nv = Int(d + 60), nt = Int(d + 64), frames = Int(d + 68);
    if (skins < 1 || skins > 32 || width < 1 || height < 1 || width > 4096 || height > 4096 ||
        nv < 3 || nv * 2 > PACK_MODEL_VERTICES || nt < 1 || nt * 3 > PACK_MODEL_CORNERS ||
        frames < 1 || frames > PACK_MODEL_FRAMES) return NULL;
    size_t at = 84, skin_size = (size_t)width * height;
    const uint8_t *own[32];
    for (int32_t s = 0; s < skins; ++s) {
        if (!Inside(size, at, 4)) return NULL;
        if (Int(d + at) == ALIAS_SKIN_SINGLE) { at += 4; own[s] = d + at; at += skin_size; }
        else {
            int32_t n = Inside(size, at + 4, 4) ? Int(d + at + 4) : 0;
            if (n < 1 || n > 256) return NULL;
            at += 8 + 4 * (size_t)n; own[s] = d + at; at += skin_size * (size_t)n;
        }
        if (at > size) return NULL;
    }
    size_t stverts = at, triangles = stverts + (size_t)nv * 12; at = triangles + (size_t)nt * 16;
    if (!Inside(size, stverts, at - stverts)) return NULL;
    lq_pack_model *m = NewModel(source, (uint32_t)frames, (uint32_t)nv * 2, (uint32_t)nt * 3, 1, (uint32_t)skins);
    if (!m) return NULL;
    uint32_t vertices = (uint32_t)nv * 2;
    for (int32_t f = 0; f < frames; ++f) {
        if (!Inside(size, at, 4)) { FreeModel(m); return NULL; }
        size_t pose;
        if (Int(d + at) == ALIAS_SINGLE) { pose = at + 4 + 24; at = pose + (size_t)nv * 4; }
        else {
            int32_t n = Inside(size, at + 4, 4) ? Int(d + at + 4) : 0;
            if (n < 1 || n > 256) { FreeModel(m); return NULL; }
            pose = at + 16 + 4 * (size_t)n + 24;
            at += 16 + 4 * (size_t)n + (size_t)n * (24 + (size_t)nv * 4);
        }
        if (!Inside(size, pose, (size_t)nv * 4) || at > size) { FreeModel(m); return NULL; }
        for (int32_t v = 0; v < nv; ++v) {
            const uint8_t *p = d + pose + 4 * (size_t)v;
            float *out = m->positions[(size_t)f * vertices + v], *n = m->normals[(size_t)f * vertices + v];
            for (int k = 0; k < 3; ++k) out[k] = p[k] * scale[k] + origin[k];
            memcpy(n, r_avertexnormals[p[3] < 162 ? p[3] : 0], sizeof(float[3]));
            memcpy(m->positions[(size_t)f * vertices + nv + v], out, sizeof(float[3]));
            memcpy(m->normals[(size_t)f * vertices + nv + v], n, sizeof(float[3]));
        }
    }
    for (int32_t v = 0; v < nv; ++v) {
        const uint8_t *p = d + stverts + 12 * (size_t)v;
        float s = (float)Int(p + 4), t = (float)Int(p + 8);
        m->uv[v][0] = (s + 0.5f) / width; m->uv[v][1] = (t + 0.5f) / height;
        m->uv[nv + v][0] = (s + width * 0.5f + 0.5f) / width; m->uv[nv + v][1] = m->uv[v][1];
    }
    for (int32_t t = 0; t < nt; ++t) {
        const uint8_t *p = d + triangles + 16 * (size_t)t;
        int front = Int(p) != 0;
        for (int c = 0; c < 3; ++c) {
            int32_t v = Int(p + 4 + 4 * c);
            if (v < 0 || v >= nv) { FreeModel(m); return NULL; }
            int seam = Int(d + stverts + 12 * (size_t)v) != 0;
            m->corners[t * 3 + c] = (uint32_t)v + (!front && seam ? (uint32_t)nv : 0);
        }
    }
    m->surfaces[0].first_corner = 0; m->surfaces[0].corner_count = (uint32_t)nt * 3;
    for (int32_t s = 0; s < skins; ++s) {
        char base[MAX_QPATH];
        lq_material *skin = &m->surfaces[0].skins[s];
        snprintf(base, sizeof(base), "%s_%d", source->name, s);
        if (LoadSkin(pack, archive, base, original, skin)) continue;
        /* Its own palette skin, fullbrights glowing. */
        lq_image image = {(uint32_t)width, (uint32_t)height, 4, malloc(skin_size * 4)};
        if (!image.pixels) { FreeModel(m); return NULL; }
        for (size_t i = 0; i < skin_size; ++i) {
            const byte *c = host_basepal + 3 * own[s][i];
            image.pixels[i * 4] = c[0]; image.pixels[i * 4 + 1] = c[1]; image.pixels[i * 4 + 2] = c[2];
            image.pixels[i * 4 + 3] = own[s][i] >= 224 ? 255 : 0;
        }
        *skin = (lq_material){.indices = {1, 1, 1, &placeholder}, .pack_color = image, .pack_gain = 1};
    }
    return m;
}
/* Skins only, for a model the pack does not replace: progs/<model>.mdl_<n>. */
static lq_pack_model *LoadSkins(model_t *source, const char *pack, lq_archive *archive, float original)
{
    aliashdr_t *header = Mod_Extradata(source);
    mdl_t *mdl = (mdl_t *)((byte *)header + header->model);
    if (mdl->numskins < 1 || mdl->numskins > 32) return NULL;
    lq_material skins[32];
    int found = 0;
    for (int s = 0; s < mdl->numskins; ++s) {
        char base[MAX_QPATH];
        snprintf(base, sizeof(base), "%s_%d", source->name, s);
        found += LoadSkin(pack, archive, base, original, &skins[s]);
    }
    if (!found) return NULL;
    lq_pack_model *m = calloc(1, sizeof(*m));
    if (m) m->surfaces = calloc(1, sizeof(*m->surfaces));
    if (m && m->surfaces) m->surfaces[0].skins = malloc((size_t)mdl->numskins * sizeof(lq_material));
    if (!m || !m->surfaces || !m->surfaces[0].skins) {
        for (int s = 0; s < mdl->numskins; ++s) { free(skins[s].pack_color.pixels); free(skins[s].pack_normal.pixels); }
        FreeModel(m); return NULL;
    }
    memcpy(m->surfaces[0].skins, skins, (size_t)mdl->numskins * sizeof(lq_material));
    m->source = source; m->surface_count = 1; m->skin_count = (uint32_t)mdl->numskins;
    return m;
}
void LQ_PackModelsLoad(const char *pack)
{
    if (!pack) pack = "";
    if (strcmp(pack, loaded_pack)) { LQ_PackModelsClear(); snprintf(loaded_pack, sizeof(loaded_pack), "%s", pack); }
    if (!pack[0]) return;
    double started = Sys_FloatTime();
    lq_archive *archive = LQ_QuakePackArchive(pack);
    int meshes = 0, skinned = 0, looked = 0;
    for (int i = 1; i < MAX_MODELS && cl.model_precache[i]; ++i) {
        model_t *source = cl.model_precache[i];
        if (source == cl.worldmodel || source->name[0] == '*' || (source->type != mod_alias && source->type != mod_brush)) continue;
        int seen = 0;
        for (int j = 0; j < checked_count && !seen; ++j) seen = checked[j] == source;
        if (seen || checked_count >= MAX_MODELS) continue;
        checked[checked_count++] = source; ++looked;
        float original = OriginalMean(source);
        size_t size = 0;
        uint8_t *bytes = PackRead(pack, archive, source->name, &size);
        lq_pack_model *m = NULL;
        if (bytes && size >= 4 && !memcmp(bytes, "IDP3", 4)) m = ParseMD3(source, bytes, size, pack, archive, original);
        else if (bytes && size >= 4 && !memcmp(bytes, "IDPO", 4)) m = ParseMDL(source, bytes, size, pack, archive, original);
        if (bytes && !m) Con_DPrintf("Pack model %s: unreadable, or no surface with an image; drawing the original.\n", source->name);
        free(bytes);
        if (!m && source->type == mod_alias) m = LoadSkins(source, pack, archive, original);
        if (!m) continue;
        models[model_count++] = m;
        if (m->replaces) ++meshes; else ++skinned;
    }
    LQ_ArchiveClose(archive);
    if (meshes || skinned)
        Con_DPrintf("Pack models: %d replaced, %d with pack skins, of %d new to this map, in %.2f s.\n",
            meshes, skinned, looked, Sys_FloatTime() - started);
}
