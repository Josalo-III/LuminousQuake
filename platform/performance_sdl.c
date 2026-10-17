/* Shared CPU-pressure monitor, independent of clock scheduling.
 * GPL-2.0-or-later. */
#include "performance.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <stdio.h>
#endif
#define NS_PER_SECOND UINT64_C(1000000000)
static struct {
    SDL_Thread *thread;
    SDL_Semaphore *stop;
    unsigned cores;
    lq_perf_load_fn sampler;
    void *context;
    atomic_bool online;
    atomic_uint_fast64_t snapshot;
} performance;

/* One atomic word makes state, load validity and load percentage a coherent
 * observation. No consumer has to take a lock or query native CPU counters. */
static void Publish(lq_perf_state state, bool valid, unsigned busy, unsigned transitions)
{
    uint64_t word = (uint64_t)state | ((uint64_t)valid << 2) |
        ((uint64_t)busy << 3) | ((uint64_t)performance.cores << 10) |
        ((uint64_t)transitions << 26);
    atomic_store_explicit(&performance.snapshot, word, memory_order_release);
}

static bool NativeLoad(void *unused, double *busy)
{
    (void)unused;
    /* Only this resource monitor samples these cumulative counters. The first
     * observation establishes a baseline, not a utilization measurement. */
    static uint64_t previous_total, previous_idle;
    static uint64_t observed_at;
    static double previous_busy;
    static bool have_busy;
    uint64_t total = 0, idle = 0;
#ifdef __APPLE__
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    mach_port_t host = mach_host_self();
    kern_return_t result = host_statistics(host, HOST_CPU_LOAD_INFO,
        (host_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), host);
    if (result != KERN_SUCCESS) return false;
    /* Darwin's counters wrap at 32 bits. Extend each counter independently. */
    static natural_t previous[CPU_STATE_MAX];
    static uint64_t extended[CPU_STATE_MAX];
    for (unsigned i = 0; i < CPU_STATE_MAX; ++i) {
        extended[i] += (natural_t)(info.cpu_ticks[i] - previous[i]);
        previous[i] = info.cpu_ticks[i];
        total += extended[i];
    }
    idle = extended[CPU_STATE_IDLE];
#elif defined(_WIN32)
    FILETIME i, k, u;
    if (!GetSystemTimes(&i, &k, &u)) return false;
    idle = ((uint64_t)i.dwHighDateTime << 32) | i.dwLowDateTime;
    total = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
        (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
#elif defined(__linux__)
    unsigned long long user, nice, system, idle_ticks, iowait, irq, softirq, steal;
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return false;
    int fields = fscanf(file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &user, &nice, &system, &idle_ticks, &iowait, &irq, &softirq, &steal);
    fclose(file);
    if (fields != 8) return false;
    idle = idle_ticks + iowait;
    total = user + nice + system + idle + irq + softirq + steal;
#else
    return false;
#endif
    uint64_t now = SDL_GetTicksNS();
    bool valid = previous_total && total > previous_total && idle >= previous_idle;
    if (valid) {
        previous_busy = 1.0 - (double)(idle - previous_idle) / (double)(total - previous_total);
        observed_at = now;
        have_busy = true;
    } else if (total == previous_total && idle == previous_idle) {
        /* Native CPU snapshots can be cached longer than our sampling period.
         * An unchanged snapshot is not a failed query. Retain its last rate
         * briefly, then fall back to waits if fresh counters never arrive. */
        valid = have_busy && now - observed_at < 2 * NS_PER_SECOND;
    } else have_busy = false;
    if (valid) *busy = previous_busy;
    previous_total = total;
    previous_idle = idle;
    return valid;
}


static int MonitorMain(void *unused)
{
    (void)unused;
    unsigned hot = 0, cool = 0, transitions = 0;
    lq_perf_state state = LQ_PERF_UNKNOWN;
    do {
        double busy = 0;
        bool valid = performance.sampler(performance.context, &busy) &&
            isfinite(busy) && busy >= 0 && busy <= 1;
        lq_perf_state next = state;
        /* Hysteresis is shared evidence, not a clock-specific decision.
         * Consumers remain free to choose how to spend or conserve capacity. */
        if (!valid) { next = LQ_PERF_UNKNOWN; hot = cool = 0; }
        else if (busy >= 0.85) {
            cool = 0;
            if (++hot >= 2) { next = LQ_PERF_CONSTRAINED; hot = 2; }
        } else if (busy <= 0.65) {
            hot = 0;
            if (++cool >= 8) { next = LQ_PERF_HEADROOM; cool = 8; }
        } else hot = cool = 0;
        if (next != state) { state = next; ++transitions; }
        Publish(state, valid, valid ? (unsigned)(busy * 100) : 0, transitions);
        /* Shutdown signals this semaphore; it never has to wait a full sample
         * interval. Gameplay consumers are not signaled by the monitor. */
    } while (!SDL_WaitSemaphoreTimeout(performance.stop, 250));
    return 0;
}

bool LQ_PerformanceStart(unsigned cores, lq_perf_load_fn sampler, void *context)
{
    if (performance.thread) return true;
    performance.cores = cores ? cores : (unsigned)SDL_GetNumLogicalCPUCores();
    if (performance.cores > 65535) performance.cores = 65535;
    performance.sampler = sampler ? sampler : NativeLoad;
    performance.context = context;
    Publish(LQ_PERF_UNKNOWN, false, 0, 0);
    performance.stop = SDL_CreateSemaphore(0);
    if (!performance.stop) return false;
    performance.thread = SDL_CreateThread(MonitorMain, "LQ resource monitor", NULL);
    if (!performance.thread) {
        SDL_DestroySemaphore(performance.stop);
        performance.stop = NULL;
        return false;
    }
    atomic_store_explicit(&performance.online, true, memory_order_release);
    return true;
}

lq_perf_snapshot LQ_PerformanceRead(void)
{
    uint64_t word = atomic_load_explicit(&performance.snapshot, memory_order_acquire);
    lq_perf_snapshot s = { (lq_perf_state)(word & 3), atomic_load(&performance.online),
        (word & 4) != 0, (unsigned)((word >> 10) & 65535),
        (unsigned)((word >> 3) & 127), (unsigned)(word >> 26) };
    return s;
}

const char *LQ_PerformanceStateName(lq_perf_state state)
{
    switch (state) {
    case LQ_PERF_CONSTRAINED: return "constrained";
    case LQ_PERF_HEADROOM: return "headroom";
    default: return "unknown";
    }
}

void LQ_PerformanceStop(void)
{
    if (performance.thread) {
        SDL_SignalSemaphore(performance.stop);
        SDL_WaitThread(performance.thread, NULL);
        SDL_DestroySemaphore(performance.stop);
        performance.thread = NULL;
        performance.stop = NULL;
    }
    atomic_store(&performance.online, false);
    Publish(LQ_PERF_UNKNOWN, false, 0, 0);
}
