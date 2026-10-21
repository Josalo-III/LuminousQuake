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
 * install's folder `pack` (NULL or "" for none), on this thread; `run`
 * decodes them in parallel. counts gets colour, normal and glow images found.
 * Returns the number of materials given any image. */
uint32_t LQ_QuakeSceneLoadPack(lq_scene *scene, const char *pack, lq_range_runner run, uint32_t counts[3]);
void LQ_QuakeSceneShutdown(void);
void LQ_QuakeLightstyles(float time, float values[256]);
#endif
