/* Luminous fluorescent tubes. GPL-2.0-or-later.
 *
 * Colour. Mercury vapour emits lines, strongest at 546 nm in the green and in
 * the blue-violet, and almost nothing in the red; the phosphor fills in only
 * part of the gap. The eye adapts most of the resulting green away, but a
 * camera balanced for other light records it, and red surfaces go dull.
 *
 * Mains flicker. A tube on a magnetic ballast lights on both halves of each
 * 60 Hz cycle, so its drive is a rectified sine: relative to its mean,
 * 1 - (2/3)cos(2wt) - (2/15)cos(4wt) - ..., plus a 60 Hz term when its worn
 * electrodes light one half-cycle more than the other. Each phosphor smooths
 * the drive like a first-order low-pass with its own decay time; blue (and
 * the mercury lines) follows almost instantly while red and green lag, so a
 * tube is bluer at its peaks and warmer in its troughs. The lines come from
 * the arc itself and have no persistence at all; they carry part of the green
 * and blue, so the 546 nm line spikes green at each peak.
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
 * timeline is a pure function of time and the tube's position. Given the
 * tube's sound, the arc instead sputters with its crackle: a loud slot is a
 * slot the arc spends out, averaged over a short exposure, and the one big
 * buzz is a hard strike that flashes. */
#include "fluorescent.h"
#include <math.h>
#include <stdlib.h>
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
static const float mercury[3] = {0.05f, 0.25f, 0.4f}; /* share of each channel from the arc's own lines */
static const float casts[3][3] = { /* a tube as recorded, before scaling to unit luminance */
    {0.94f, 1.0f, 0.97f}, /* the adapted eye: faintly cool, a little short of red */
    {0.72f, 1.0f, 0.80f}, /* daylight film, unfiltered: the classic green */
    {0.84f, 1.0f, 0.88f}, /* video: white balance corrects warmth, not green */
};
#define TRACK_EXPOSURE 0.02 /* seconds of a track averaged into one view */
#define TRACK_OVERSHOOT 0.3f
#define TRACK_FLASH 1.0f /* a hard strike's extra output at full crackle; half of it at none */
#define TRACK_FLASHING 1.1f /* output above which the tube is in a strike flash */
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
uint32_t LQ_FluorescentCast(uint32_t camera)
{
    return camera == LQ_CAMERA_EYE || camera >= LQ_CAMERA_COUNT ? 0 : camera == LQ_CAMERA_FILM ? 1 : 2;
}
void LQ_SceneTintFluorescents(lq_scene *scene, uint32_t camera)
{
    const float *cast = casts[LQ_FluorescentCast(camera)];
    float luminance = 0.2126f*cast[0] + 0.7152f*cast[1] + 0.0722f*cast[2];
    for (uint32_t i = 0; scene && i < scene->light_source_count; ++i) {
        lq_light_source *source = &scene->light_sources[i];
        if (!Tube(scene, source)) continue;
        float c[3], peak = 0;
        for (int k = 0; k < 3; ++k) { c[k] = source->color[k] * cast[k] / luminance; peak = fmaxf(peak, c[k]); }
        if (peak <= 0) continue;
        /* Colours stay within 0..1; the brightest channel's excess goes to the output. */
        for (int k = 0; k < 3; ++k) source->color[k] = c[k] / peak;
        source->output_scale *= peak; source->output_base *= peak;
    }
}
static int CompareFloat(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}
uint32_t LQ_TubeTrackFromSound(lq_tube_track *track, const void *samples, int width, uint32_t count, uint32_t rate)
{
    static float sorted[LQ_TUBE_TRACK_SLOTS];
    if (!track) return 0;
    track->count = 0; track->position = 0;
    if (!samples || !count || !rate || (width != 1 && width != 2)) return 0;
    double seconds = (double)count / rate, slot = fmax(0.005, seconds / LQ_TUBE_TRACK_SLOTS);
    uint32_t slots = (uint32_t)(seconds / slot);
    if (slots < 2) return 0;
    if (slots > LQ_TUBE_TRACK_SLOTS) slots = LQ_TUBE_TRACK_SLOTS;
    for (uint32_t i = 0; i < slots; ++i) {
        uint32_t first = (uint32_t)(i * slot * rate), last = i + 1 == slots ? count : (uint32_t)((i + 1) * slot * rate);
        double sum = 0;
        for (uint32_t j = first; j < last; ++j) {
            double v = width == 1 ? ((const signed char *)samples)[j] / 128.0 : ((const int16_t *)samples)[j] / 32768.0;
            sum += v * v;
        }
        track->arc[i] = last > first ? (float)sqrt(sum / (last - first)) : 0;
    }
    /* The steady hum is the median loudness; a slot's crackle runs from none
     * at twice it to full at eight times it. */
    memcpy(sorted, track->arc, slots * sizeof(*sorted));
    qsort(sorted, slots, sizeof(*sorted), CompareFloat);
    float hum = fmaxf(sorted[slots / 2], 1e-6f);
    for (uint32_t i = 0; i < slots; ++i) track->arc[i] = fminf(fmaxf((track->arc[i] / hum - 2) / 6, 0), 1);
    /* Bursts of crackle, joined across gaps shorter than 30 ms, scored by
     * their crackle. The arc drops out through a burst, but a burst standing
     * well above the rest (within 90% of the biggest and twice the median) is
     * the arc striking hard: it flashes instead. */
    static uint32_t begin[LQ_TUBE_TRACK_SLOTS / 2], end[LQ_TUBE_TRACK_SLOTS / 2];
    static float score[LQ_TUBE_TRACK_SLOTS / 2];
    uint32_t bursts = 0, gap = (uint32_t)ceil(0.03 / slot);
    for (uint32_t i = 0; i < slots; ) {
        if (track->arc[i] <= 0) { ++i; continue; }
        uint32_t j = i, quiet = 0;
        float sum = 0;
        for (; j < slots && quiet <= gap; ++j) {
            quiet = track->arc[j] > 0 ? 0 : quiet + 1;
            sum += track->arc[j];
        }
        begin[bursts] = i; end[bursts] = j - quiet; score[bursts++] = sum; i = j;
    }
    float biggest = 0, median = 0;
    if (bursts) {
        memcpy(sorted, score, bursts * sizeof(*sorted));
        qsort(sorted, bursts, sizeof(*sorted), CompareFloat);
        biggest = sorted[bursts - 1]; median = sorted[bursts / 2];
    }
    for (uint32_t i = 0; i < slots; ++i) track->arc[i] = 1 - track->arc[i];
    for (uint32_t b = 0; b < bursts; ++b) {
        if (score[b] < 0.9f * biggest || score[b] < 2 * median) continue;
        for (uint32_t i = begin[b]; i < end[b]; ++i) track->arc[i] = 1 + TRACK_FLASH * (1 - track->arc[i] / 2);
    }
    track->slot = (float)slot; track->length = (float)seconds; track->count = slots;
    return slots;
}
static float Arc(const lq_tube_track *track, double t)
{
    t = fmod(t, track->length);
    if (t < 0) t += track->length;
    uint32_t i = (uint32_t)(t / track->slot);
    return track->arc[i < track->count ? i : track->count - 1];
}
/* The arc's output over the exposure ending where the track is playing. */
static float Exposed(const lq_tube_track *track)
{
    float level = 0;
    for (int k = 0; k < 8; ++k) level += Arc(track, track->position - TRACK_EXPOSURE * (k + 0.5) / 8) / 8;
    return level;
}
/* The tube's output relative to normal where the track is playing, and its colour. */
static float Sputter(const lq_tube_track *track, float tint[3])
{
    double u = track->position;
    float level = Exposed(track), since = INFINITY;
    /* Each relight overshoots, bluish, as a fresh strike does. */
    for (uint32_t k = 0; k < 60; ++k)
        if (Arc(track, u - k * (double)track->slot) < 0.5f) { since = k * track->slot; break; }
    float extra = isfinite(since) ? TRACK_OVERSHOOT * expf(-since / OVERSHOOT_TIME) : 0;
    float mix = fmaxf(extra / 0.45f, (level - 1) / TRACK_FLASH); /* a flash is a fresh arc too */
    for (int k = 0; k < 3; ++k) tint[k] = 1 + (arc[k] - 1) * mix;
    return fminf(level * (1 + extra), 1 + TRACK_FLASH);
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
int LQ_FluorescentReach(const lq_scene *scene, const lq_tube_track *track, lq_runtime_light *light)
{
    if (!scene || !track || !track->count || !light || light->source_index >= scene->light_source_count) return 0;
    lq_light_source source = scene->light_sources[light->source_index];
    const lq_light_source *tube = Tube(scene, &source);
    if (!tube || LQ_LightFluorescent(tube) != 2 || tube->style < LQ_FLUORESCENT_STYLE_BASE || tube->style >= 255 ||
        Exposed(track) <= TRACK_FLASHING) return 0;
    /* The intensity at which each falloff reaches twice as far. */
    float cutoff = 0.5f / source.output_scale, value = source.intensity;
    switch (source.formula) {
    case LQ_LIGHT_LINEAR: value = 2 * value - cutoff; break;
    case LQ_LIGHT_INVERSE: value *= 2; break;
    case LQ_LIGHT_INVERSE2: value *= 4; break;
    case LQ_LIGHT_INVERSE2A: { float root = 2 * sqrtf(value / cutoff) - 1; value = cutoff * root * root; } break;
    default: return 0;
    }
    if (!(value > source.intensity)) return 0;
    source.intensity = value;
    light->falloff[0] = value; light->position_radius[3] = LQ_LightSourceRange(&source);
    return 1;
}
static float Output(const lq_tube_track *track, const lq_light_source *tube, double time, float tint[3])
{
    return track && track->count ? Sputter(track, tint) : Failure(Seed(tube->origin, 1), time, tint);
}
void LQ_FluorescentStyles(const lq_scene *scene, const lq_tube_track *track, double time, float styles[256])
{
    float tint[3];
    for (uint32_t i = 0; scene && i < scene->light_source_count; ++i) {
        const lq_light_source *source = &scene->light_sources[i];
        if (LQ_LightFluorescent(source) == 2 && source->style >= LQ_FLUORESCENT_STYLE_BASE && source->style < 255)
            styles[source->style] = Output(track, source, time, tint) * NORMAL;
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
            double phosphor = kept / sqrt(1 + w*w) * cos(2 * M_PI * alias - atan(w)), line = kept * cos(2 * M_PI * alias);
            rgb[k] += (float)(mercury[k] * line + (1 - mercury[k]) * phosphor);
        }
    }
}
int LQ_FluorescentGain(const lq_scene *scene, const lq_tube_track *track, uint32_t source_index, double time,
    uint32_t camera, float rgb[3])
{
    if (!scene || source_index >= scene->light_source_count) return 0;
    const lq_light_source *source = Tube(scene, &scene->light_sources[source_index]);
    if (!source) return 0;
    int kind = LQ_LightFluorescent(source);
    /* Neighbouring tubes share a circuit; circuits sit on one of three phases. */
    Flicker(time, camera, kind == 2 ? 0.2f : 0.03f, Seed(source->origin, 256) % 3, rgb);
    if (kind == 2 && source->style >= LQ_FLUORESCENT_STYLE_BASE && source->style < 255) {
        float tint[3];
        Output(track, source, time, tint);
        for (int k = 0; k < 3; ++k) rgb[k] *= tint[k];
    }
    return 1;
}
