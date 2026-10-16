/* Luminous fluorescent tubes: mains flicker as a camera records it, and
 * failing tubes that drop out and restrike. GPL-2.0-or-later. */
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
/* Write each failing tube's private style at `time`: its dropouts, restrikes
 * and strike flashes, at Quake's normal 264/256 when lit. */
void LQ_FluorescentStyles(const lq_scene *scene, double time, float styles[256]);
/* Per-channel gain on a tube's direct light: mains flicker through each
 * phosphor's persistence as `camera` records it, and a failing tube's colour
 * while it glows or flashes. A plain light in a tube's fixture follows that
 * tube. Returns 0 for any other source. */
int LQ_FluorescentGain(const lq_scene *scene, uint32_t source, double time, uint32_t camera, float rgb[3]);
#endif
