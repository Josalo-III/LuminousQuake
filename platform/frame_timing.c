/* Opt-in CPU frame profiling. GPU execution is deliberately not inferred. */
#include "quakedef.h"
#include "frame_timing.h"
#ifdef __APPLE__
#include <os/signpost.h>
#endif

typedef struct { double total, maximum, began; } lq_profile_sample;

static cvar_t lq_profile = {"lq_profile", "0"};
static lq_profile_sample samples[LQ_PROFILE_COUNT];
static unsigned long frames;
static qboolean collecting;
static double window_started;
static const char *const names[LQ_PROFILE_COUNT] = {
    "frame", "input/commands", "network", "simulation", "client", "scene preparation",
    "UI/compositing", "presentation/submission", "audio"
};
#ifdef __APPLE__
static os_log_t profile_log;
static os_signpost_id_t signposts[LQ_PROFILE_COUNT];
#endif

void LQ_ProfileReset(void)
{
    memset(samples, 0, sizeof(samples)); frames = 0;
    window_started = Sys_FloatTime();
}
static void Reset_f(void) { LQ_ProfileReset(); Con_Printf("lq_profile: reset\n"); }

void LQ_ProfileReport(void)
{
    double frame_total, wall_elapsed;
    unsigned i;
    if (!lq_profile.value) {
        Con_Printf("lq_profile: disabled (set lq_profile 1 to collect CPU timings)\n");
        return;
    }
    if (!frames) { Con_Printf("lq_profile: no completed frames\n"); return; }
    frame_total = samples[LQ_PROFILE_FRAME].total;
    wall_elapsed = Sys_FloatTime() - window_started;
    Con_Printf("lq_profile: %lu frames, %.3f ms CPU/frame, %.3f ms maximum, %.1f processed FPS over %.3f s\n",
        frames, frame_total * 1000.0 / frames, samples[LQ_PROFILE_FRAME].maximum * 1000.0,
        wall_elapsed > 0 ? frames / wall_elapsed : 0.0, wall_elapsed);
    for (i = 1; i < LQ_PROFILE_COUNT; ++i) {
        double total = samples[i].total;
        Con_Printf("  %-25s %8.3f ms avg  %8.3f ms max  %5.1f%% frame\n", names[i],
            total * 1000.0 / frames, samples[i].maximum * 1000.0,
            frame_total > 0 ? total * 100.0 / frame_total : 0.0);
    }
    Con_Printf("  CPU timings include submission and any present wait; they do not measure GPU execution.\n");
}

static void Report_f(void) { LQ_ProfileReport(); }

void LQ_ProfileInit(void)
{
    Cvar_RegisterVariable(&lq_profile);
    Cmd_AddCommand("lq_profile_report", Report_f);
    Cmd_AddCommand("lq_profile_reset", Reset_f);
#ifdef __APPLE__
    profile_log = os_log_create("org.josalo.luminousquake", "PointsOfInterest");
#endif
}

void LQ_ProfileBegin(lq_profile_stage stage)
{
    if (stage < 0 || stage >= LQ_PROFILE_COUNT) return;
    if (stage == LQ_PROFILE_FRAME) {
        if (!lq_profile.value) { collecting = false; return; }
        if (!collecting) { LQ_ProfileReset(); collecting = true; }
    }
    if (!collecting) return;
    samples[stage].began = Sys_FloatTime();
#ifdef __APPLE__
    if (profile_log) {
        signposts[stage] = os_signpost_id_generate(profile_log);
        os_signpost_interval_begin(profile_log, signposts[stage], "LQ stage", "%{public}s", names[stage]);
    }
#endif
}

void LQ_ProfileEnd(lq_profile_stage stage)
{
    double elapsed;
    if (!collecting || stage < 0 || stage >= LQ_PROFILE_COUNT || !samples[stage].began) return;
    elapsed = Sys_FloatTime() - samples[stage].began;
    samples[stage].began = 0;
#ifdef __APPLE__
    if (profile_log && signposts[stage] != OS_SIGNPOST_ID_INVALID) {
        os_signpost_interval_end(profile_log, signposts[stage], "LQ stage", "%{public}s", names[stage]);
        signposts[stage] = OS_SIGNPOST_ID_INVALID;
    }
#endif
    if (elapsed < 0) return;
    samples[stage].total += elapsed;
    if (elapsed > samples[stage].maximum) samples[stage].maximum = elapsed;
    if (stage == LQ_PROFILE_FRAME) ++frames;
}
