/* Enhanced fluorescent tubes. GPL-2.0-or-later.
 *
 * Mains flicker. A tube on a magnetic ballast lights on both halves of each
 * 60 Hz cycle, so its drive is a rectified sine: relative to its mean,
 * 1 - (2/3)cos(2wt) - (2/15)cos(4wt) - ..., plus a 60 Hz term when its worn
 * electrodes light one half-cycle more than the other. Each phosphor smooths
 * the drive like a first-order low-pass with its own decay time; blue (and
 * the mercury lines) follows almost instantly while red and green lag, so a
 * tube is bluer at its peaks and warmer in its troughs.
 *
 * The camera. A shutter open for T seconds keeps sinc(pi f T) of a term at
 * f Hz, and frames taken fps times a second see it at its alias, the gap to
 * the nearest multiple of fps. Evaluating the alias at continuous time gives
 * exactly the camera's samples at its frame instants and a smooth beat
 * between them, so the result does not depend on the display's rate.
 *
 * Failing tubes. Each failing tube's time is cut into windows; a window holds
 * nothing, a dropout, a burst of chatter or a restrike, where the starter
 * heats the cathodes (a dim orange glow) and tries to strike the arc, failing
 * with a flash before it holds. A struck arc overshoots briefly, bluish. The
 * timeline is a pure function of time and the tube's position. */
#include "fluorescent.h"
#include <math.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define WINDOW 1.6 /* seconds; every event and its overshoot fits in one */
#define NORMAL (264.0f / 256) /* Quake's 'm' */
#define GLOW_LEVEL 0.05f
#define FLASH_LEVEL 0.8f
#define OVERSHOOT_TIME 0.06f
#define FIXTURE 24.0f /* a plain light this close to a tube is part of its fixture */
static const float persistence[3] = {0.004f, 0.002f, 0.0005f}; /* red, green, blue phosphor decay, seconds */
static const float warm[3] = {1.9f, 0.75f, 0.25f}; /* heated cathodes; unit luminance */
static const float arc[3] = {0.85f, 1.0f, 1.35f}; /* a fresh arc's mercury lines; unit luminance */
static const struct { double fps, shutter; } cameras[LQ_CAMERA_COUNT] = {
    {0, 0}, {24000.0/1001, 1001.0/48000}, {30000.0/1001, 1.0/60}, {30000.0/1001, 1.0/250}, {25, 1.0/50}
};
int LQ_LightFluorescent(const lq_light_source *source)
{
    if (!source) return 0;
    if (!strcmp(source->classname, "light_fluoro")) return 1;
    return !strcmp(source->classname, "light_fluorospark") ? 2 : 0;
}
/* The tube a source belongs to: itself, or the tube whose fixture holds it
 * (the rerelease adds a spotlight beside each tube for its beam). */
static const lq_light_source *Tube(const lq_scene *scene, const lq_light_source *source)
{
    if (LQ_LightFluorescent(source)) return source;
    if (strcmp(source->classname, "light")) return NULL;
    for (uint32_t i = 0; i < scene->light_source_count; ++i) {
        const lq_light_source *tube = &scene->light_sources[i];
        float dx = tube->origin[0]-source->origin[0], dy = tube->origin[1]-source->origin[1], dz = tube->origin[2]-source->origin[2];
        if (LQ_LightFluorescent(tube) && dx*dx + dy*dy + dz*dz <= FIXTURE*FIXTURE) return tube;
    }
    return NULL;
}
static uint32_t Mix(uint32_t h, uint32_t v) { h ^= v; h *= 16777619u; return h ^ h >> 15; }
static uint32_t Seed(const float origin[3], float cell)
{
    uint32_t h = 2166136261u;
    for (int k = 0; k < 3; ++k) h = Mix(h, (uint32_t)(int32_t)floorf(origin[k] / cell + 0.5f));
    return h;
}
static float Uniform(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return (*s >> 8) / 16777216.0f; }
uint32_t LQ_SceneAssignFluorescentStyles(lq_scene *scene)
{
    uint32_t assigned = 0;
    for (uint32_t i = 0; scene && i < scene->light_source_count; ++i) {
        lq_light_source *source = &scene->light_sources[i];
        if (LQ_LightFluorescent(source) != 2 || (source->style && source->style != 10)) continue;
        if (LQ_FLUORESCENT_STYLE_BASE + assigned >= 255) break;
        source->style = LQ_FLUORESCENT_STYLE_BASE + assigned++;
    }
    /* A failing tube's fixture lights go dark with it. */
    for (uint32_t i = 0; scene && i < scene->light_source_count; ++i) {
        lq_light_source *source = &scene->light_sources[i];
        const lq_light_source *tube = LQ_LightFluorescent(source) ? NULL : Tube(scene, source);
        if (tube && LQ_LightFluorescent(tube) == 2 && tube->style >= LQ_FLUORESCENT_STYLE_BASE &&
            (!source->style || source->style == 10)) source->style = tube->style;
    }
    return assigned;
}
typedef struct { float begin, end, level; const float *tint; } segment;
/* The window's events, in order, and the overshoot after each. */
static uint32_t Events(uint32_t seed, int64_t window, segment out[12], float *overshoot)
{
    uint32_t s = Mix(Mix(seed, (uint32_t)window), (uint32_t)((uint64_t)window >> 32)) | 1, n = 0;
    float kind = Uniform(&s), t;
    *overshoot = 0;
    if (kind < 0.35f) return 0;
    if (kind < 0.70f) { /* the arc goes out and reignites */
        t = Uniform(&s) * (float)(WINDOW - 0.5);
        out[n++] = (segment){t, t + 0.035f + 0.105f*Uniform(&s), 0, NULL};
        *overshoot = 0.3f;
    } else if (kind < 0.85f) { /* chatter: a burst of short dropouts */
        t = Uniform(&s) * (float)(WINDOW - 1.2);
        for (uint32_t count = 3 + (uint32_t)(Uniform(&s) * 4); n < count; ) {
            float dark = 0.025f + 0.035f*Uniform(&s);
            out[n++] = (segment){t, t + dark, 0, NULL};
            t += dark + 0.03f + 0.06f*Uniform(&s);
        }
        *overshoot = 0.2f;
    } else { /* restrike: out, then the starter's attempts */
        t = 0.05f + 0.25f*Uniform(&s);
        float dark = 0.05f + 0.05f*Uniform(&s);
        out[n++] = (segment){t, t + dark, 0, NULL}; t += dark;
        for (uint32_t attempts = 1 + (Uniform(&s) < 0.5f), a = 0; a < attempts; ++a) {
            float glow = 0.12f + 0.13f*Uniform(&s), flash = 0.04f + 0.04f*Uniform(&s);
            out[n++] = (segment){t, t + glow, GLOW_LEVEL, warm}; t += glow;
            out[n++] = (segment){t, t + flash, FLASH_LEVEL, arc}; t += flash;
        }
        float glow = 0.12f + 0.13f*Uniform(&s);
        out[n++] = (segment){t, t + glow, GLOW_LEVEL, warm};
        *overshoot = 0.45f;
    }
    return n;
}
/* A failing tube's output relative to normal, and its colour (unit luminance). */
static float Failure(uint32_t seed, double time, float tint[3])
{
    double shifted = time + (seed >> 8) / 16777216.0 * WINDOW;
    int64_t window = (int64_t)floor(shifted / WINDOW);
    float u = (float)(shifted - (double)window * WINDOW), overshoot, since = INFINITY;
    segment events[12];
    uint32_t count = Events(seed, window, events, &overshoot);
    tint[0] = tint[1] = tint[2] = 1;
    for (uint32_t i = 0; i < count; ++i) {
        if (u >= events[i].begin && u < events[i].end) {
            if (events[i].tint) memcpy(tint, events[i].tint, 3 * sizeof(*tint));
            return events[i].level;
        }
        if (u >= events[i].end && (i + 1 == count || u < events[i+1].begin)) since = u - events[i].end;
    }
    if (!isfinite(since)) return 1;
    float extra = overshoot * expf(-since / OVERSHOOT_TIME), mix = extra / 0.45f;
    for (int k = 0; k < 3; ++k) tint[k] += (arc[k] - 1) * mix;
    return 1 + extra;
}
void LQ_FluorescentStyles(const lq_scene *scene, double time, float styles[256])
{
    float tint[3];
    for (uint32_t i = 0; scene && i < scene->light_source_count; ++i) {
        const lq_light_source *source = &scene->light_sources[i];
        if (LQ_LightFluorescent(source) == 2 && source->style >= LQ_FLUORESCENT_STYLE_BASE && source->style < 255)
            styles[source->style] = Failure(Seed(source->origin, 1), time, tint) * NORMAL;
    }
}
static double Sinc(double x) { return fabs(x) < 1e-9 ? 1 : sin(x) / x; }
/* Cycles of the 60 Hz mains since time zero; the grid wanders by 0.015 Hz. */
static double MainsCycles(double t) { return 60*t - 0.015*97/(2*M_PI)*cos(2*M_PI*t/97); }
static void Flicker(double time, uint32_t camera, float asymmetry, uint32_t phase, float rgb[3])
{
    const struct { double harmonic, amplitude; } terms[] = {{1, asymmetry}, {2, -2.0/3}, {4, -2.0/15}};
    rgb[0] = rgb[1] = rgb[2] = 1;
    if (camera == LQ_CAMERA_EYE || camera >= LQ_CAMERA_COUNT) return;
    double fps = cameras[camera].fps, cycles = MainsCycles(time) + phase / 3.0;
    for (size_t j = 0; j < sizeof(terms) / sizeof(terms[0]); ++j) {
        double f = 60 * terms[j].harmonic, kept = terms[j].amplitude * Sinc(M_PI * f * cameras[camera].shutter);
        double alias = terms[j].harmonic * cycles - round(f / fps) * fps * time;
        alias -= floor(alias);
        for (int k = 0; k < 3; ++k) {
            double w = 2 * M_PI * f * persistence[k];
            rgb[k] += (float)(kept / sqrt(1 + w*w) * cos(2 * M_PI * alias - atan(w)));
        }
    }
}
int LQ_FluorescentGain(const lq_scene *scene, uint32_t source_index, double time, uint32_t camera, float rgb[3])
{
    if (!scene || source_index >= scene->light_source_count) return 0;
    const lq_light_source *source = Tube(scene, &scene->light_sources[source_index]);
    if (!source) return 0;
    int kind = LQ_LightFluorescent(source);
    /* Neighbouring tubes share a circuit; circuits sit on one of three phases. */
    Flicker(time, camera, kind == 2 ? 0.2f : 0.03f, Seed(source->origin, 256) % 3, rgb);
    if (kind == 2 && source->style >= LQ_FLUORESCENT_STYLE_BASE && source->style < 255) {
        float tint[3];
        Failure(Seed(source->origin, 1), time, tint);
        for (int k = 0; k < 3; ++k) rgb[k] *= tint[k];
    }
    return 1;
}
