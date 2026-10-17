/* Exercise the live clock thread with controlled system-load samples.
 * GPL-2.0-or-later. */
#include "tick_clock.h"
#include "performance.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define SPIN ~0u	/* ExpectMargin: continuous instead of an early-wake margin */
static atomic_int utilization;

static bool SampleLoad(void *unused, double *busy)
{
    (void)unused;
    int value = atomic_load(&utilization);
    *busy = value / 100.0;
    return value >= 0;
}

static bool Matches(unsigned margin)
{
    lq_clock_stats s = LQ_TickClockStats();
    return margin == SPIN ? s.spinning : !s.spinning && s.margin_us == margin;
}

static void ExpectMargin(unsigned margin, unsigned timeout_ms)
{
    uint64_t end = SDL_GetTicks() + timeout_ms;
    lq_clock_sample previous = {0}, current;
    do {
        REQUIRE(LQ_TickClockRead(&current));
        REQUIRE(current.tick >= previous.tick);
        REQUIRE(current.fraction >= 0 && current.fraction < 1);
        previous = current;
        if (Matches(margin)) return;
        SDL_Delay(2);
    } while (SDL_GetTicks() < end);
    REQUIRE(Matches(margin));
}

static void ExpectPressure(lq_perf_state state, unsigned timeout_ms)
{
    uint64_t end = SDL_GetTicks() + timeout_ms;
    while (LQ_PerformanceRead().state != state && SDL_GetTicks() < end) SDL_Delay(2);
    REQUIRE(LQ_PerformanceRead().state == state);
}

/* The game must receive the clock's own tick, not one derived around it. */
static void ExpectAuthority(unsigned ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    unsigned before = LQ_TickClockStats().fallbacks;
    while (SDL_GetTicks() < end) {
        lq_clock_sample s;
        REQUIRE(LQ_TickClockRead(&s));
        REQUIRE(s.tick == s.published);
        SDL_Delay(1);
    }
    REQUIRE(LQ_TickClockStats().fallbacks == before);
}

int main(void)
{
    REQUIRE(SDL_Init(0));
    atomic_store(&utilization, 20);
    LQ_TickClockConfigure(LQ_CLOCK_AUTO, 6, true);
    REQUIRE(LQ_PerformanceStart(10, SampleLoad, NULL));
    REQUIRE(LQ_TickClockStart());
    ExpectMargin(2000, 3500);
#ifdef __APPLE__
    REQUIRE(LQ_TickClockStats().realtime);
#endif
    ExpectAuthority(300);
    lq_clock_sample before, after;
    REQUIRE(LQ_TickClockRead(&before));
    uint64_t started = SDL_GetTicksNS();

    /* A short pressure burst must not immediately change policy. */
    atomic_store(&utilization, 95);
    SDL_Delay(50);
    REQUIRE(LQ_TickClockStats().margin_us == 2000);
    ExpectMargin(500, 1000);
    REQUIRE(LQ_PerformanceRead().state == LQ_PERF_CONSTRAINED);
    /* A clock override must not alter the shared evidence seen by workers.
     * Continuous running gives up real-time scheduling rather than overrun it. */
    LQ_TickClockConfigure(LQ_CLOCK_SPIN, 6, true);
    ExpectMargin(SPIN, 200);
    REQUIRE(!LQ_TickClockStats().realtime);
    REQUIRE(LQ_PerformanceRead().state == LQ_PERF_CONSTRAINED);
    LQ_TickClockConfigure(LQ_CLOCK_AUTO, 6, true);
    ExpectMargin(500, 200);
#ifdef __APPLE__
    REQUIRE(LQ_TickClockStats().realtime);
#endif
    atomic_store(&utilization, 20);
    ExpectMargin(2000, 3000);
    REQUIRE(LQ_TickClockRead(&after));
    double elapsed_ticks = (double)(SDL_GetTicksNS() - started) * 72 / 1e9;
    REQUIRE((double)(after.tick - before.tick) > elapsed_ticks - 2);
    REQUIRE((double)(after.tick - before.tick) < elapsed_ticks + 2);
    REQUIRE(LQ_TickClockStats().switches >= 2);
    puts("PASS sustained-load margin, recovery, published-tick authority, and timeline continuity");

    /* More workers or an inactive game remove the spare-core entitlement. */
    LQ_TickClockConfigure(LQ_CLOCK_AUTO, 8, true);
    ExpectMargin(500, 200);
    LQ_TickClockConfigure(LQ_CLOCK_AUTO, 6, true);
    ExpectMargin(2000, 200);
    LQ_TickClockConfigure(LQ_CLOCK_AUTO, 6, false);
    ExpectMargin(0, 200);
    LQ_TickClockConfigure(LQ_CLOCK_SLEEP, 6, true);
    ExpectMargin(0, 200);
    ExpectAuthority(200);
    LQ_TickClockConfigure(LQ_CLOCK_SPIN, 6, true);
    ExpectMargin(SPIN, 200);
    puts("PASS worker capacity, inactivity, and explicit policy overrides");

    atomic_store(&utilization, -1);
    LQ_TickClockConfigure(LQ_CLOCK_AUTO, 6, true);
    ExpectMargin(500, 500);
    REQUIRE(!LQ_TickClockStats().load_valid);
    LQ_TickClockStop();
    LQ_TickClockStop();
    REQUIRE(!LQ_TickClockRead(&after));
    REQUIRE(LQ_PerformanceRead().running);
    atomic_store(&utilization, 95);
    ExpectPressure(LQ_PERF_CONSTRAINED, 1000);
    LQ_PerformanceStop();
    LQ_PerformanceStop();
    REQUIRE(!LQ_PerformanceRead().running);

    atomic_store(&utilization, 20);
    LQ_TickClockConfigure(LQ_CLOCK_AUTO, 0, true);
    REQUIRE(LQ_PerformanceStart(4, SampleLoad, NULL));
    REQUIRE(LQ_TickClockStart());
    ExpectPressure(LQ_PERF_HEADROOM, 3500);
    /* Four cores never earn the wide margin, even with headroom. */
    ExpectMargin(500, 200);
    LQ_TickClockStop();
    LQ_PerformanceStop();
    puts("PASS independent resource policy, unavailable sampling, low-core margin, restart, and shutdown");
    SDL_Quit();
    return 0;
}
