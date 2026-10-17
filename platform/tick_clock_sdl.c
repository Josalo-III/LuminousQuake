/* Real-time 72 Hz clock publisher. GPL-2.0-or-later. */
#include "tick_clock.h"
#include "performance.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#endif

#define NS_PER_SECOND UINT64_C(1000000000)
#define CLOCK_HZ 72
#define TICK_NS (NS_PER_SECOND / CLOCK_HZ)
/* Early-wake margins. The thread sleeps until this long before each deadline,
 * then spins the rest of the way. With headroom it can afford the wider
 * margin; under pressure or with few cores it takes the narrow one. */
#define MARGIN_HEADROOM_US 2000u
#define MARGIN_CONSTRAINED_US 500u
/* Real-time budget per tick: the margin's spin plus this much publishing work,
 * delivered within the margin plus the constraint slack. */
#define WORK_US 50u
#define SLACK_US 1000u
/* A publication this far past its deadline counts as late. */
#define LATE_US 250u

static struct {
    SDL_Thread *thread;
    SDL_Semaphore *ready;
    uint64_t epoch;
    atomic_bool stop, spinning, active, realtime;
    atomic_int mode;
    atomic_uint workers, switches, margin_us;
    atomic_uint late, skipped, fallbacks, max_late_us;
    _Alignas(128) atomic_uint_fast64_t published;
#ifdef __APPLE__
    mach_timebase_info_data_t timebase;
#endif
} clock_state;

static uint64_t TickAt(uint64_t elapsed)
{
    return elapsed / NS_PER_SECOND * CLOCK_HZ +
        elapsed % NS_PER_SECOND * CLOCK_HZ / NS_PER_SECOND;
}

static uint64_t Deadline(uint64_t tick)
{
    /* Ceil rather than floor: the publisher must not announce a tick before
     * its boundary. Integer whole seconds avoid multiplication overflow. */
    return clock_state.epoch + tick / CLOCK_HZ * NS_PER_SECOND +
        (tick % CLOCK_HZ * NS_PER_SECOND + CLOCK_HZ - 1) / CLOCK_HZ;
}

#ifdef __APPLE__
static uint32_t MachFromNS(uint64_t ns)
{
    return (uint32_t)(ns * clock_state.timebase.denom / clock_state.timebase.numer);
}

/* Time-constraint scheduling, as audio threads use: the kernel runs this
 * thread ahead of ordinary threads and wakes it on time, provided it stays
 * within the computation it declares. Spinning past that budget would get it
 * demoted, so continuous mode returns to the standard policy instead. */
static bool SetSchedule(bool realtime, unsigned margin_us)
{
    thread_t self = mach_thread_self();
    kern_return_t result;
    if (realtime) {
        thread_time_constraint_policy_data_t policy = {
            MachFromNS(TICK_NS),
            MachFromNS((uint64_t)(margin_us + WORK_US) * 1000),
            MachFromNS((uint64_t)(margin_us + SLACK_US) * 1000),
            TRUE };
        result = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY,
            (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    } else {
        thread_standard_policy_data_t policy = {0};
        result = thread_policy_set(self, THREAD_STANDARD_POLICY,
            (thread_policy_t)&policy, THREAD_STANDARD_POLICY_COUNT);
    }
    mach_port_deallocate(mach_task_self(), self);
    return realtime && result == KERN_SUCCESS;
}
#else
static bool SetSchedule(bool realtime, unsigned margin_us)
{
    (void)margin_us;
    /* SDL maps this to MMCSS on Windows and to SCHED_RR (directly or through
     * rtkit) on Linux; without permission it stays an ordinary thread. */
    return realtime && SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
}
#endif

static void SleepUntil(uint64_t target)
{
    uint64_t now = SDL_GetTicksNS();
    if (now >= target) return;
#ifdef __APPLE__
    uint64_t when = mach_absolute_time() + MachFromNS(target - now);
    if (mach_wait_until(when) == KERN_SUCCESS) return;
#endif
    SDL_DelayNS(target - now);
}

static unsigned Margin(int mode, bool active, unsigned workers)
{
    if (mode == LQ_CLOCK_SLEEP || !active) return 0;
    lq_perf_snapshot performance = LQ_PerformanceRead();
    /* Reserve main, clock and two spare logical CPUs for the wider margin. */
    bool capacity = performance.cores >= 8 && workers <= performance.cores - 4;
    return capacity && performance.running && performance.state == LQ_PERF_HEADROOM ?
        MARGIN_HEADROOM_US : MARGIN_CONSTRAINED_US;
}

static int ClockMain(void *unused)
{
    (void)unused;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    bool spinning = false, realtime = SetSchedule(true, 0);
    unsigned margin = 0;
    atomic_store(&clock_state.realtime, realtime);
    uint64_t next = TickAt(SDL_GetTicksNS() - clock_state.epoch) + 1;
    SDL_SignalSemaphore(clock_state.ready);
    while (!atomic_load_explicit(&clock_state.stop, memory_order_relaxed)) {
        /* Policy only changes between ticks. Resource sampling belongs to the
         * shared monitor; this loop reads its snapshot and does no other work. */
        int mode = atomic_load(&clock_state.mode);
        bool active = atomic_load(&clock_state.active);
        bool spin = active && mode == LQ_CLOCK_SPIN;
        unsigned wanted = spin ? 0 : Margin(mode, active, atomic_load(&clock_state.workers));
        if (spin != spinning || wanted != margin) {
            if (spin != spinning) atomic_fetch_add(&clock_state.switches, 1);
            spinning = spin;
            margin = wanted;
            realtime = SetSchedule(!spin, margin);
            atomic_store(&clock_state.spinning, spinning);
            atomic_store(&clock_state.margin_us, margin);
            atomic_store(&clock_state.realtime, realtime);
        }
        uint64_t deadline = Deadline(next);
        if (!spinning) SleepUntil(deadline - (uint64_t)margin * 1000);
        uint64_t now;
        while ((now = SDL_GetTicksNS()) < deadline)
            SDL_CPUPauseInstruction();
        /* Publish whatever tick is due, so a stall loses no elapsed ticks.
         * Only this counter is shared: no gameplay state crosses threads. */
        uint64_t tick = TickAt(now - clock_state.epoch);
        atomic_store_explicit(&clock_state.published, tick, memory_order_release);
        unsigned late_us = (unsigned)((now - deadline) / 1000);
        if (late_us >= LATE_US) atomic_fetch_add(&clock_state.late, 1);
        if (late_us > atomic_load(&clock_state.max_late_us)) atomic_store(&clock_state.max_late_us, late_us);
        if (tick > next) atomic_fetch_add(&clock_state.skipped, (unsigned)(tick - next));
        next = tick + 1;
    }
    return 0;
}

bool LQ_TickClockStart(void)
{
    if (clock_state.thread) return true;
    atomic_store(&clock_state.stop, false);
    atomic_store(&clock_state.spinning, false);
    atomic_store(&clock_state.published, 0);
    atomic_store(&clock_state.switches, 0);
    atomic_store(&clock_state.late, 0);
    atomic_store(&clock_state.skipped, 0);
    atomic_store(&clock_state.fallbacks, 0);
    atomic_store(&clock_state.max_late_us, 0);
#ifdef __APPLE__
    if (mach_timebase_info(&clock_state.timebase) != KERN_SUCCESS) return false;
#endif
    clock_state.ready = SDL_CreateSemaphore(0);
    if (!clock_state.ready) return false;
    clock_state.epoch = SDL_GetTicksNS();
    clock_state.thread = SDL_CreateThread(ClockMain, "LQ tick clock", NULL);
    if (!clock_state.thread) { SDL_DestroySemaphore(clock_state.ready); clock_state.ready = NULL; return false; }
    /* Startup is the only consumer barrier. Consumers read the published
     * tick directly; there are no per-tick notifications or waits. */
    SDL_WaitSemaphore(clock_state.ready);
    return true;
}

void LQ_TickClockConfigure(int mode, unsigned workers, bool active)
{
    if (mode < LQ_CLOCK_AUTO || mode > LQ_CLOCK_SPIN) mode = LQ_CLOCK_AUTO;
    /* Avoid repeatedly invalidating the publisher's configuration cacheline
     * when a render/main-loop iteration has no policy changes. */
    if (atomic_load(&clock_state.mode) != mode) atomic_store(&clock_state.mode, mode);
    if (atomic_load(&clock_state.workers) != workers) atomic_store(&clock_state.workers, workers);
    if (atomic_load(&clock_state.active) != active) atomic_store(&clock_state.active, active);
}

bool LQ_TickClockRead(lq_clock_sample *sample)
{
    if (!clock_state.thread) return false;
    uint64_t published = atomic_load_explicit(&clock_state.published, memory_order_acquire);
    uint64_t now = SDL_GetTicksNS();
    uint64_t tick = published;
    /* The clock is the master: its tick stands even while its next one is a
     * moment away. Only a clock more than a whole tick behind is overruled,
     * so a preempted publisher cannot hold the game back indefinitely. */
    if (TickAt(now - clock_state.epoch) > published + 1) {
        tick = TickAt(now - clock_state.epoch);
        atomic_fetch_add(&clock_state.fallbacks, 1);
    }
    uint64_t start = Deadline(tick);
    double fraction = now > start ? (double)(now - start) / TICK_NS : 0;
    sample->tick = tick;
    sample->published = published;
    sample->fraction = fraction < 1 ? fraction : 0.999999;
    return true;
}

lq_clock_stats LQ_TickClockStats(void)
{
    lq_perf_snapshot performance = LQ_PerformanceRead();
    lq_clock_stats s = {
        .running = clock_state.thread != NULL,
        .spinning = atomic_load(&clock_state.spinning),
        .realtime = atomic_load(&clock_state.realtime),
        .load_valid = performance.load_valid,
        .cores = performance.cores,
        .switches = atomic_load(&clock_state.switches),
        .busy_percent = performance.busy_percent,
        .margin_us = atomic_load(&clock_state.margin_us),
        .max_late_us = atomic_load(&clock_state.max_late_us),
        .late = atomic_load(&clock_state.late),
        .skipped = atomic_load(&clock_state.skipped),
        .fallbacks = atomic_load(&clock_state.fallbacks),
        .published = atomic_load(&clock_state.published) };
    return s;
}

void LQ_TickClockStop(void)
{
    if (!clock_state.thread) return;
    atomic_store(&clock_state.stop, true);
    SDL_WaitThread(clock_state.thread, NULL);
    SDL_DestroySemaphore(clock_state.ready);
    clock_state.thread = NULL;
    clock_state.ready = NULL;
    atomic_store(&clock_state.spinning, false);
    atomic_store(&clock_state.realtime, false);
    atomic_store(&clock_state.margin_us, 0);
}
