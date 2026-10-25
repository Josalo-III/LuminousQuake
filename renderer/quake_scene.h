/* Legacy loader adapter. GPL-2.0-or-later. */
#ifndef LQ_QUAKE_SCENE_H
#define LQ_QUAKE_SCENE_H
#include "scene.h"
int LQ_QuakeSceneLoad(int argc, char **argv, const char *map, lq_scene *scene, char *error, size_t size);
int LQ_QuakeSceneFromWorld(lq_scene *scene, char *error, size_t size);
/* Marks (nonzero) the draws in the camera leaf's PVS; needs a loaded scene. */
int LQ_QuakeSceneVisibleDraws(const lq_scene *scene, const float camera[3], uint8_t *visible);
/* Community texture packs (docs/MATERIAL_PACKS.md): for each solid world
 * texture of the last scene built, the pack's colour image (with its _glow or
 * _luma folded into alpha) and _norm map, found as DarkPlaces finds them:
 * textures/<map>/<name>, then textures/<name>, as .tga, .png then .jpg, with
 * '*' written '#'. Files come from the game's search path, then from the
 * pack at path `pack` (NULL or "" for none): a folder holding textures/, or
 * a .pk3. They are read on this thread; `run` inflates and decodes them in
 * parallel. counts gets colour, normal and glow images found.
 * Returns the number of materials given any image. */
uint32_t LQ_QuakeSceneLoadPack(lq_scene *scene, const char *pack, lq_range_runner run, uint32_t counts[4]); /* colour, bump, glow, gloss */
/* An image named `base` (a path without extension) as .tga, .png then .jpg,
 * from the game's search path, then the pack at `pack` (a folder, or the
 * .pk3 opened as `archive`), decoded to RGBA8. Returns 0 when none decodes.
 * LQ_QuakePackArchive opens `pack` when it is a .pk3 (else NULL); close it
 * with LQ_ArchiveClose. */
struct lq_archive;
struct lq_archive *LQ_QuakePackArchive(const char *pack);
int LQ_QuakeLoadImage(const char *pack, struct lq_archive *archive, const char *base, lq_image *out);
void LQ_QuakeSceneShutdown(void);
void LQ_QuakeLightstyles(float time, float values[256]);
#endif
