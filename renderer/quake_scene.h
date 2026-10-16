/* Legacy loader adapter. GPL-2.0-or-later. */
#ifndef LQ_QUAKE_SCENE_H
#define LQ_QUAKE_SCENE_H
#include "scene.h"
int LQ_QuakeSceneLoad(int argc, char **argv, const char *map, lq_scene *scene, char *error, size_t size);
int LQ_QuakeSceneFromWorld(lq_scene *scene, char *error, size_t size);
/* Marks (nonzero) the draws in the camera leaf's PVS; needs a loaded scene. */
int LQ_QuakeSceneVisibleDraws(const lq_scene *scene, const float camera[3], uint8_t *visible);
void LQ_QuakeSceneShutdown(void);
void LQ_QuakeLightstyles(float time, float values[256]);
#endif
