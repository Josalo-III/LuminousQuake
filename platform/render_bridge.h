/* Classic GPU frame adaptation and scaled UI coverage. GPL-2.0-or-later. */
#ifndef LQ_RENDER_BRIDGE_H
#define LQ_RENDER_BRIDGE_H
#include <stddef.h>
void LQ_RenderBegin(void);
void LQ_RenderAfterCommands(void);
int LQ_RenderBridgeActive(void);
void LQ_RenderMapChanged(void);
/* Registers the bridge's settings (lq_scene_cache_mb). */
void LQ_RenderBridgeRegister(void);
/* Texture packs holding DarkPlaces material maps: a .pk3 beside the app,
 * then a .pk3 in id1 or the mod's folder, then a folder of the install, each
 * place sorted by name. These are the packs Options -> Luminous Options
 * offers, and "auto" takes the first. `places` (optional) gets where each is,
 * as a sentence. Returns how many were written. */
int LQ_TexturePacks(char names[][64], const char *places[], int capacity);
size_t LQ_FreeRAM(void); /* free RAM, counting what macOS reclaims on demand; 0 if unknown */
/* The RAM slider's shares now, and the shadow cube scale the lighting share gives (-1, 0, 1). */
void LQ_RamShares(size_t *caching, size_t *lighting, int *shadow_shift);
int LQ_RenderGPUView(void);
/* Particle kinds for LQ_RenderGPUParticle. For smoke, scale is the puff's
 * growth (1 when emitted) and erode the fraction its surface has worn away.
 * Trail smoke (grenades, cooled rocket fire) stays within a Software
 * particle's size. */
enum { LQ_PARTICLE_DROPLET = 1, LQ_PARTICLE_SMOKE = 2, LQ_PARTICLE_TRAIL_SMOKE = 3 };
int LQ_RenderGPUParticle(const float origin[3], int color, int emissive, int kind, float scale, float erode,
    unsigned int seed);
/* A flame model's light strength now (about 0.8-1.2, 1 on average) at a
 * static flame's origin; 0 when there is none or no GPU renderer runs. */
float LQ_RenderFlameGain(const float origin[3]);
void LQ_RenderSpark(const float origin[3], int color);
void LQ_RenderUIRegion(int x, int y, int width, int height, const unsigned char *pixels, int stride, int transparent);
int LQ_RenderUIPainted(int x, int y);
const unsigned char *LQ_RenderUICoverage(void);
void LQ_UIBegin(void);
void LQ_UIEnd(void);
int LQ_UIScale(void);
int LQ_UIActive(void);
void LQ_UIShutdown(void);
#endif
