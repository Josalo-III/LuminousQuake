/* Luminous fluorescent tubes: mercury's green as a camera records it, mains
 * flicker, and failing tubes that drop out and restrike in time with their
 * crackle. GPL-2.0-or-later. */
#ifndef LQ_FLUORESCENT_H
#define LQ_FLUORESCENT_H
#include "scene.h"
/* Failing tubes (light_fluorospark) get private styles from this base so each
 * one's bounce layer goes dark with it. Quake's own styles stop at 63. */
#define LQ_FLUORESCENT_STYLE_BASE 64
/* The camera the view stands for. A 60 Hz tube flickers at 120 Hz; the eye
 * fuses that, but a camera samples it, and what it keeps beats at the gap
 * between 120 Hz and the nearest multiple of its frame rate. */
enum { LQ_CAMERA_EYE,        /* steady: the eye fuses 120 Hz */
       LQ_CAMERA_FILM,       /* 23.976 fps, 180-degree shutter: slow breathing */
       LQ_CAMERA_NTSC,       /* 29.97 fps, 1/60 shutter: in sync, steady */
       LQ_CAMERA_NTSC_FAST,  /* 29.97 fps, 1/250 shutter: deep slow breathing */
       LQ_CAMERA_PAL,        /* 25 fps, 1/50 shutter: a 5 Hz flutter */
       LQ_CAMERA_COUNT };
/* 1 for a steady tube (light_fluoro), 2 for a failing one (light_fluorospark). */
int LQ_LightFluorescent(const lq_light_source *source);
/* Give each failing tube on style 0 or 10 its own style, before bounce is
 * computed, shared with the plain lights of its fixture (within 24 units).
 * Returns how many tubes were assigned. */
uint32_t LQ_SceneAssignFluorescentStyles(lq_scene *scene);
/* Colour every tube and its fixture's lights as `camera` records them, before
 * bounce so the tubes' bounce carries it too. Mercury's line spectrum is rich
 * in green and poor in red; the eye adapts most of that away, film balanced
 * for daylight keeps it, and video's white balance keeps some. Luminance is
 * unchanged. Apply once to a freshly loaded scene. */
void LQ_SceneTintFluorescents(lq_scene *scene, uint32_t camera);
/* Which colour `camera` records tubes in; cameras sharing one share a bounce. */
uint32_t LQ_FluorescentCast(uint32_t camera);
/* A failing tube's arc follows the crackle in its looping sound: each slot
 * holds the tube's output relative to normal, read from the sound's loudness
 * against its steady hum: below 1 where it drops out, 1.5 to 2 through the
 * one burst big enough to be a hard strike that flashes. `position` is where the loop is playing,
 * in seconds; the caller keeps it current. */
#define LQ_TUBE_TRACK_SLOTS 4096
typedef struct {
    float arc[LQ_TUBE_TRACK_SLOTS];
    float slot, length; /* seconds */
    uint32_t count;
    double position;
} lq_tube_track;
/* Read `count` mono samples (signed, `width` 1 or 2 bytes) at `rate` Hz.
 * Returns the slot count, or 0 when the sound is unusable. */
uint32_t LQ_TubeTrackFromSound(lq_tube_track *track, const void *samples, int width, uint32_t count, uint32_t rate);
/* Write each failing tube's private style at `time`: its dropouts, restrikes
 * and strike flashes, at Quake's normal 264/256 when lit. With a `track`, all
 * failing tubes sputter with its crackle, as Quake plays one buzz for them
 * all; without one, each tube follows its own random timeline. */
void LQ_FluorescentStyles(const lq_scene *scene, const lq_tube_track *track, double time, float styles[256]);
/* Through a failing tube's strike flash its light, and its fixture's, reaches
 * twice as far: raise `light`'s falloff and range to match. The range changes
 * only as a flash begins and ends, so its shadow is redrawn only then.
 * Returns 1 while flashing. */
int LQ_FluorescentReach(const lq_scene *scene, const lq_tube_track *track, lq_runtime_light *light);
/* A flash, as a failing tube's strike: the light shines at `factor` times its
 * output, and its falloff and range reach `factor` times as far. A strike
 * peaks at 2 (twice the output; LQ_FluorescentReach doubles the reach). */
void LQ_FlashRuntimeLight(lq_runtime_light *light, float factor);
/* Per-channel gain on a tube's direct light: mains flicker through each
 * phosphor's persistence (and the instant mercury lines) as `camera` records
 * it, and a failing tube's colour while it glows or flashes. A plain light in
 * a tube's fixture follows that tube. Returns 0 for any other source. */
int LQ_FluorescentGain(const lq_scene *scene, const lq_tube_track *track, uint32_t source, double time,
    uint32_t camera, float rgb[3]);
#endif
