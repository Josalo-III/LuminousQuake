/* Standalone macOS timing experiment; not part of the engine runtime.
 * GPL-2.0-or-later.
 *
 * Build: clang -O2 -std=c11 -Wall -Wextra -pthread tools/tick_clock_bench.c -o /tmp/lq-clock-bench
 * Run:   /tmp/lq-clock-bench MODE seconds load_threads work_us hitch_us [interactive|precise|realtime [early_us]]
 * MODE: direct, threaded, publish-sleep, publish-spin, publish-early
 *
 * publish-early sleeps until early_us (default 2000) before each deadline and
 * spins the rest. realtime gives the clock thread Mach time-constraint
 * scheduling with a budget covering that spin; consumers keep interactive QoS.
 *
 * Compare a worker sleeping on its own deadlines with an independent clock
 * waking a worker through a condition variable. Both use the same absolute
 * 72 Hz schedule. A clock wakeup is measured separately from consumer start:
 * keeping time on another thread does not make an occupied consumer available.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { HZ = 72, MAX_LOAD = 16 };
typedef struct { uint64_t deadline, wake, start; } sample_t;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    sample_t *samples;
    size_t count, published;
    uint64_t work_us, hitch_us;
    int interactive;
    int precise;
    int realtime;
    int spin;
    uint64_t early_us;
    uint64_t epoch;
    /* Consumers reference this counter; publication modes have no condition
     * variable handoff. Keep the read-mostly counter off the mutex's cacheline. */
    _Alignas(128) atomic_uint_fast64_t published_tick;
    double timer_cpu_ms, worker_cpu_ms;
} trial_t;

static mach_timebase_info_data_t timebase;
static atomic_int stop_load;

static void check(int result, const char *operation)
{
    if (result) { fprintf(stderr, "%s failed: %d\n", operation, result); exit(1); }
}

static double us_between(uint64_t later, uint64_t earlier)
{
    return ((double)later - (double)earlier) * timebase.numer / timebase.denom / 1000.0;
}

static uint64_t ticks_for_ns(uint64_t ns)
{
    return ns * timebase.denom / timebase.numer;
}

static double thread_cpu_ms(void)
{
    struct timespec ts;
    check(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), "clock_gettime");
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void configure_timing(trial_t *trial)
{
    if (trial->interactive)
        check(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0), "interactive QoS");
    if (trial->precise) {
        /* Scheduling priority and allowable timer coalescing are separate
         * policies. Measure the low-latency tier explicitly, without giving
         * this experiment real-time scheduling or using a spinning timer. */
        thread_latency_qos_policy_data_t policy = { LATENCY_QOS_TIER_0 };
        thread_t thread = mach_thread_self();
        kern_return_t result = thread_policy_set(thread, THREAD_LATENCY_QOS_POLICY,
            (thread_policy_t)&policy, THREAD_LATENCY_QOS_POLICY_COUNT);
        check(mach_port_deallocate(mach_task_self(), thread), "thread port release");
        check(result, "timer latency policy");
    }
}

/* Only clock threads get real-time scheduling: its budget covers the early
 * spin plus publishing, and a consumer doing frame work would overrun it. */
static void configure_clock(trial_t *trial)
{
    configure_timing(trial);
    if (!trial->realtime) return;
    thread_time_constraint_policy_data_t policy = {
        (uint32_t)ticks_for_ns(1000000000ULL / HZ),
        (uint32_t)ticks_for_ns((trial->early_us + 50) * 1000),
        (uint32_t)ticks_for_ns((trial->early_us + 1000) * 1000),
        1 };
    thread_t thread = mach_thread_self();
    kern_return_t result = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    check(mach_port_deallocate(mach_task_self(), thread), "thread port release");
    check(result, "time-constraint policy");
}

static void wait_for(uint64_t deadline)
{
    /* Absolute deadlines prevent a delayed wakeup from moving later ticks.
     * Sleep rather than spin: this experiment includes OS scheduling jitter. */
    while (mach_absolute_time() < deadline)
        check(mach_wait_until(deadline), "mach_wait_until");
}

static void work(uint64_t us)
{
    uint64_t end = mach_absolute_time() + ticks_for_ns(us * 1000);
    while (mach_absolute_time() < end) { }
}

static void *load_main(void *unused)
{
    volatile uint64_t value = 1;
    (void)unused;
    while (!atomic_load_explicit(&stop_load, memory_order_relaxed))
        for (unsigned i = 0; i < 4096; ++i) value = value * 6364136223846793005ULL + 1;
    return NULL;
}

static void consume(trial_t *trial, size_t index)
{
    trial->samples[index].start = mach_absolute_time();
    work(trial->work_us);
    /* Once per second, model a consumer occupied longer than one tick. The
     * clock must still publish every deadline; the consumer must retain the
     * backlog rather than treating multiple notifications as a single tick. */
    if (trial->hitch_us && (index + 1) % HZ == 0) work(trial->hitch_us);
}

static void *worker_main(void *context)
{
    trial_t *trial = context;
    configure_timing(trial);
    size_t consumed = 0;
    double cpu_start = thread_cpu_ms();
    while (consumed < trial->count) {
        check(pthread_mutex_lock(&trial->mutex), "mutex lock");
        while (trial->published == consumed)
            check(pthread_cond_wait(&trial->ready, &trial->mutex), "condition wait");
        size_t available = trial->published;
        check(pthread_mutex_unlock(&trial->mutex), "mutex unlock");
        /* The sequence number is authoritative. Condition signals can merge
         * while this worker is busy and are not themselves a count of ticks. */
        while (consumed < available) consume(trial, consumed++);
    }
    trial->worker_cpu_ms = thread_cpu_ms() - cpu_start;
    return NULL;
}

static void *timer_main(void *context)
{
    trial_t *trial = context;
    configure_clock(trial);
    double cpu_start = thread_cpu_ms();
    for (size_t i = 0; i < trial->count; ++i) {
        wait_for(trial->samples[i].deadline);
        trial->samples[i].wake = mach_absolute_time();
        check(pthread_mutex_lock(&trial->mutex), "mutex lock");
        trial->published = i + 1;
        check(pthread_cond_signal(&trial->ready), "condition signal");
        check(pthread_mutex_unlock(&trial->mutex), "mutex unlock");
    }
    trial->timer_cpu_ms = thread_cpu_ms() - cpu_start;
    return NULL;
}

static void *publisher_main(void *context)
{
    trial_t *trial = context;
    configure_clock(trial);
    /* Initialization/loading may wait for the shared epoch. In spin mode the
     * clock never deliberately sleeps, yields, or signals during gameplay. */
    wait_for(trial->epoch);
    double cpu_start = thread_cpu_ms();
    size_t published = 0;
    while (published < trial->count) {
        if (!trial->spin) {
            uint64_t deadline = trial->samples[published].deadline;
            uint64_t early = ticks_for_ns(trial->early_us * 1000);
            wait_for(deadline > early ? deadline - early : deadline);
            while (mach_absolute_time() < deadline) { }
        }
        uint64_t now = mach_absolute_time();
        size_t due = published;
        while (due < trial->count && now >= trial->samples[due].deadline) ++due;
        if (due == published) continue;
        /* Derive elapsed ticks from absolute deadlines even after preemption.
         * Publishing once per wakeup would lose elapsed ticks. The atomic is
         * only the tick counter; no other engine state is being synchronized. */
        atomic_store_explicit(&trial->published_tick, due, memory_order_release);
        uint64_t stamp = mach_absolute_time();
        while (published < due) trial->samples[published++].wake = stamp;
    }
    trial->timer_cpu_ms = thread_cpu_ms() - cpu_start;
    return NULL;
}

static void distribution(const char *name, double *values, size_t count);

static int publication_trial(trial_t *trial, const char *mode, long seconds, long load)
{
    enum { READ_HZ = 240, READ_BATCH = 1000 };
    pthread_t publisher;
    size_t reads = 0, stale_reads = 0;
    uint64_t atomic_ticks = 0, reference_ticks = 0, checksum = 0;
    double max_stale_us = 0;
    check(pthread_create(&publisher, NULL, publisher_main, trial), "publisher create");
    configure_timing(trial);
    double cpu_start = thread_cpu_ms();
    for (size_t frame = 1; frame <= (size_t)seconds * READ_HZ; ++frame) {
        /* Model a consumer already paced by its own frame loop. It never
         * waits on publication or receives notifications from the clock. */
        wait_for(trial->epoch + ticks_for_ns(frame * 1000000000ULL / READ_HZ));
        uint64_t now = mach_absolute_time();
        uint64_t tick = atomic_load_explicit(&trial->published_tick, memory_order_acquire);
        ++reads;
        if (tick < trial->count && now > trial->samples[tick].deadline) {
            ++stale_reads;
            double age = us_between(now, trial->samples[tick].deadline);
            if (age > max_stale_us) max_stale_us = age;
        }
        /* Batch reads to amortize measurement overhead. Compare an atomic
         * reference with deriving the tick directly from the OS clock. */
        uint64_t begin = mach_absolute_time();
        for (int i = 0; i < READ_BATCH; ++i)
            checksum ^= atomic_load_explicit(&trial->published_tick, memory_order_acquire);
        atomic_ticks += mach_absolute_time() - begin;
        begin = mach_absolute_time();
        for (int i = 0; i < READ_BATCH; ++i) {
            uint64_t elapsed = mach_absolute_time() - trial->epoch;
            checksum ^= elapsed * timebase.numer / timebase.denom * HZ / 1000000000ULL;
        }
        reference_ticks += mach_absolute_time() - begin;
        work(trial->work_us);
        if (trial->hitch_us && frame % READ_HZ == 0) work(trial->hitch_us);
    }
    trial->worker_cpu_ms = thread_cpu_ms() - cpu_start;
    check(pthread_join(publisher, NULL), "publisher join");
    double *delays = calloc(trial->count, sizeof(*delays));
    if (!delays) return 1;
    for (size_t i = 0; i < trial->count; ++i) {
        if (!trial->samples[i].wake) return 1;
        delays[i] = us_between(trial->samples[i].wake, trial->samples[i].deadline);
    }
    double divisor = (double)reads * READ_BATCH;
    printf("{\"mode\":\"%s\",\"qos\":\"%s\",\"seconds\":%ld,\"load_threads\":%ld,"
           "\"ticks\":%zu,\"timer_cpu_ms\":%.3f,\"reader_cpu_ms\":%.3f,"
           "\"consumer_reads\":%zu,\"stale_reads\":%zu,\"max_stale_us\":%.3f,"
           "\"atomic_read_ns\":%.3f,\"os_tick_read_ns\":%.3f,\"checksum\":%llu,",
        mode, trial->realtime ? "realtime" : trial->precise ? "precise" : trial->interactive ? "interactive" : "inherited",
        seconds, load, trial->count, trial->timer_cpu_ms, trial->worker_cpu_ms,
        reads, stale_reads, max_stale_us, us_between(atomic_ticks, 0)*1000/divisor,
        us_between(reference_ticks, 0)*1000/divisor, (unsigned long long)checksum);
    distribution("publication_lateness", delays, trial->count); puts("}");
    free(delays);
    return 0;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void distribution(const char *name, double *values, size_t count)
{
    double sum = 0;
    size_t over_tick = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += values[i];
        if (values[i] >= 1e6 / HZ) ++over_tick;
    }
    qsort(values, count, sizeof(*values), compare_double);
    printf("\"%s\":{\"mean_us\":%.3f,\"p50_us\":%.3f,\"p95_us\":%.3f,"
           "\"p99_us\":%.3f,\"max_us\":%.3f,\"late_by_full_tick\":%zu}",
        name, sum/count, values[(count-1)*50/100], values[(count-1)*95/100],
        values[(count-1)*99/100], values[count-1], over_tick);
}

int main(int argc, char **argv)
{
    if (argc < 6 || argc > 8 ||
        (strcmp(argv[1], "direct") && strcmp(argv[1], "threaded") &&
         strcmp(argv[1], "publish-sleep") && strcmp(argv[1], "publish-spin") &&
         strcmp(argv[1], "publish-early")) ||
        (argc >= 7 && strcmp(argv[6], "interactive") && strcmp(argv[6], "precise") &&
         strcmp(argv[6], "realtime")) ||
        (argc == 8 && (strcmp(argv[1], "publish-early") || atol(argv[7]) <= 0 || atol(argv[7]) >= 13000))) {
        fprintf(stderr, "Usage: %s direct|threaded|publish-sleep|publish-spin|publish-early seconds load_threads work_us hitch_us [interactive|precise|realtime [early_us]]\n", argv[0]);
        return 2;
    }
    char *ends[4];
    long options[4];
    for (int i = 0; i < 4; ++i) {
        options[i] = strtol(argv[i+2], &ends[i], 10);
        if (!argv[i+2][0] || *ends[i] || options[i] < 0) return 2;
    }
    if (options[0] < 1 || options[0] > 300 || options[1] > MAX_LOAD ||
        options[2] > 100000 || options[3] > 100000) return 2;
    check(mach_timebase_info(&timebase), "mach_timebase_info");
    trial_t trial = {0};
    trial.count = (size_t)options[0] * HZ;
    trial.work_us = (uint64_t)options[2];
    trial.hitch_us = (uint64_t)options[3];
    trial.interactive = argc >= 7;
    trial.precise = argc >= 7 && !strcmp(argv[6], "precise");
    trial.realtime = argc >= 7 && !strcmp(argv[6], "realtime");
    trial.spin = !strcmp(argv[1], "publish-spin");
    if (!strcmp(argv[1], "publish-early"))
        trial.early_us = argc == 8 ? (uint64_t)atol(argv[7]) : 2000;
    atomic_init(&trial.published_tick, 0);
    trial.samples = calloc(trial.count, sizeof(*trial.samples));
    double *wake = calloc(trial.count, sizeof(*wake));
    double *handoff = calloc(trial.count, sizeof(*handoff));
    double *start = calloc(trial.count, sizeof(*start));
    if (!trial.samples || !wake || !handoff || !start) return 1;
    check(pthread_mutex_init(&trial.mutex, NULL), "mutex init");
    check(pthread_cond_init(&trial.ready, NULL), "condition init");
    pthread_t load[MAX_LOAD], worker, timer;
    for (long i = 0; i < options[1]; ++i)
        check(pthread_create(&load[i], NULL, load_main, NULL), "load thread create");

    /* Allow thread creation and CPU load to settle before measuring. Compute
     * each deadline from the same epoch, avoiding rounding drift at 72 Hz. */
    uint64_t epoch = mach_absolute_time() + ticks_for_ns(250000000);
    trial.epoch = epoch;
    for (size_t i = 0; i < trial.count; ++i)
        trial.samples[i].deadline = epoch + ticks_for_ns((i+1) * 1000000000ULL / HZ);
    if (!strncmp(argv[1], "publish-", 8)) {
        int result = publication_trial(&trial, argv[1], options[0], options[1]);
        atomic_store_explicit(&stop_load, 1, memory_order_relaxed);
        for (long i = 0; i < options[1]; ++i) check(pthread_join(load[i], NULL), "load join");
        check(pthread_cond_destroy(&trial.ready), "condition destroy");
        check(pthread_mutex_destroy(&trial.mutex), "mutex destroy");
        free(trial.samples); free(wake); free(handoff); free(start);
        return result;
    }
    int threaded = !strcmp(argv[1], "threaded");
    if (threaded) {
        check(pthread_create(&worker, NULL, worker_main, &trial), "worker create");
        check(pthread_create(&timer, NULL, timer_main, &trial), "timer create");
        check(pthread_join(timer, NULL), "timer join");
        check(pthread_join(worker, NULL), "worker join");
    } else {
        /* Match the threaded trial's consumer priority, while leaving the
         * synthetic load at its inherited priority in both configurations. */
        configure_timing(&trial);
        double cpu_start = thread_cpu_ms();
        for (size_t i = 0; i < trial.count; ++i) {
            wait_for(trial.samples[i].deadline);
            trial.samples[i].wake = mach_absolute_time();
            consume(&trial, i);
        }
        trial.worker_cpu_ms = thread_cpu_ms() - cpu_start;
    }
    atomic_store_explicit(&stop_load, 1, memory_order_relaxed);
    for (long i = 0; i < options[1]; ++i) check(pthread_join(load[i], NULL), "load join");
    for (size_t i = 0; i < trial.count; ++i) {
        if (!trial.samples[i].wake || !trial.samples[i].start) return 1;
        wake[i] = us_between(trial.samples[i].wake, trial.samples[i].deadline);
        handoff[i] = us_between(trial.samples[i].start, trial.samples[i].wake);
        start[i] = us_between(trial.samples[i].start, trial.samples[i].deadline);
    }
    printf("{\"mode\":\"%s\",\"qos\":\"%s\",\"seconds\":%ld,\"load_threads\":%ld,\"work_us\":%ld,"
           "\"hitch_us\":%ld,\"ticks\":%zu,\"timer_cpu_ms\":%.3f,\"worker_cpu_ms\":%.3f,",
        argv[1], trial.realtime ? "realtime" : trial.precise ? "precise" : trial.interactive ? "interactive" : "inherited", options[0], options[1], options[2], options[3], trial.count,
        trial.timer_cpu_ms, trial.worker_cpu_ms);
    distribution("clock_lateness", wake, trial.count); putchar(',');
    distribution("consumer_after_clock", handoff, trial.count); putchar(',');
    distribution("consumer_lateness", start, trial.count); puts("}");
    check(pthread_cond_destroy(&trial.ready), "condition destroy");
    check(pthread_mutex_destroy(&trial.mutex), "mutex destroy");
    free(trial.samples); free(wake); free(handoff); free(start);
    return 0;
}
