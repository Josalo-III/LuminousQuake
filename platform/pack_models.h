/* Community pack models for Luminous (docs/MATERIAL_PACKS.md). GPL-2.0-or-later.
 * A pack may replace a model's mesh (DarkPlaces reads an MD3 or a new MDL
 * under the original's name: progs/armor.mdl, maps/b_batt0.bsp) or only give
 * its skins (progs/backpack.mdl_0.tga with _norm and _luma or _glow). Both
 * are for drawing only: the game keeps the original models for everything
 * else (collision, bounds, flame lights, Software and Classic GPU). */
#ifndef LQ_PACK_MODELS_H
#define LQ_PACK_MODELS_H
#include "scene.h"

/* One textured part of a mesh: its triangles' corners in the model's corner
 * list, and a material per skin (pack_color, pack_normal and pack_gain set;
 * indices is the replaced MDL's own skin, or a 1x1 placeholder). */
typedef struct {
    uint32_t first_corner, corner_count;
    lq_material *skins;
} lq_pack_surface;

typedef struct {
    const struct model_s *source; /* the model it stands for */
    int replaces; /* 1: draw this mesh instead; 0: skins only, on the original mesh */
    uint32_t frame_count, vertex_count, corner_count, surface_count, skin_count;
    float (*positions)[3], (*normals)[3]; /* frame_count * vertex_count, model space */
    float (*uv)[2]; /* vertex_count, 0-1 over the image */
    uint32_t *corners; /* triangle corners: vertex indices */
    lq_pack_surface *surfaces; /* skins-only: one surface, no corners */
} lq_pack_model;

/* Reads what the pack at `pack` (a folder or .pk3; NULL or "" none) holds for
 * each model the client has precached. Models read for an earlier map with
 * the same pack are kept. Prints a summary. */
void LQ_PackModelsLoad(const char *pack);
/* What the pack holds for `model`, or NULL. */
const lq_pack_model *LQ_PackModel(const struct model_s *model);
/* Frees everything; materials drawn from these must be gone first. */
void LQ_PackModelsClear(void);

#endif
