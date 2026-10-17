/* Persistent SDL worker pool for independent row/range conversion jobs.
 * New LuminousQuake code, GPL-2.0-or-later. */
#include "quakedef.h"
#include "sdl_backend.h"
#include "jobs.h"
#include "performance.h"

#define LQ_MAX_WORKERS 8

typedef struct {
    SDL_Mutex *mutex;
    SDL_Condition *ready;
    SDL_Condition *finished;
    SDL_Thread *threads[LQ_MAX_WORKERS];
    unsigned worker_ids[LQ_MAX_WORKERS];
    lq_range_job_fn function;
    void *context;
    size_t count, grain, next;
    unsigned workers, active_workers, remaining;
    Uint64 generation;
    qboolean stopping;
} lq_job_pool;

static lq_job_pool pool;
static cvar_t lq_workers = {"lq_workers", "-1", true};
static qboolean jobs_registered;
static int configured = -2;

static int DesiredWorkers(void)
{
    float selection = lq_workers.value;
    int requested;
    if (!isfinite(selection)) return 0;
    if (selection < 0) {
        int cores = SDL_GetNumLogicalCPUCores();
        /* Conversion is memory-heavy and the game/driver remain on the main
         * thread. Measurements favor six workers on a 10-core M1 Max. */
        requested = cores > 6 ? cores - 4 : cores - 1;
        if (requested < 1) requested = 1;
    } else if (selection >= LQ_MAX_WORKERS) requested = LQ_MAX_WORKERS;
    else requested = (int)selection;
    if (requested > LQ_MAX_WORKERS) requested = LQ_MAX_WORKERS;
    return requested;
}

static qboolean TakeRange(size_t *begin, size_t *end)
{
    qboolean available = false;
    SDL_LockMutex(pool.mutex);
    if (pool.next < pool.count) {
        *begin = pool.next;
        pool.next += pool.grain;
        if (pool.next > pool.count) pool.next = pool.count;
        *end = pool.next;
        available = true;
    }
    SDL_UnlockMutex(pool.mutex);
    return available;
}

static int WorkerMain(void *unused)
{
    Uint64 seen = 0;
    unsigned id = *(unsigned *)unused;
    for (;;) {
        SDL_LockMutex(pool.mutex);
        while (!pool.stopping && seen == pool.generation)
            SDL_WaitCondition(pool.ready, pool.mutex);
        if (pool.stopping) {
            SDL_UnlockMutex(pool.mutex);
            return 0;
        }
        seen = pool.generation;
        /* Keep the pool alive across pressure changes. Only the budgeted
         * cohort participates in this job and its completion barrier. */
        if (id >= pool.active_workers) {
            SDL_UnlockMutex(pool.mutex);
            continue;
        }
        SDL_UnlockMutex(pool.mutex);

        size_t begin, end;
        while (TakeRange(&begin, &end)) pool.function(pool.context, begin, end);

        SDL_LockMutex(pool.mutex);
        if (--pool.remaining == 0) SDL_BroadcastCondition(pool.finished);
        SDL_UnlockMutex(pool.mutex);
    }
}

void LQ_JobsRegister(void)
{
    if (jobs_registered) return;
    Cvar_RegisterVariable(&lq_workers);
    jobs_registered = true;
}

void LQ_JobsShutdown(void)
{
    if (pool.mutex) {
        SDL_LockMutex(pool.mutex);
        pool.stopping = true;
        if (pool.ready) SDL_BroadcastCondition(pool.ready);
        SDL_UnlockMutex(pool.mutex);
    }
    for (unsigned i = 0; i < pool.workers; ++i)
        if (pool.threads[i]) SDL_WaitThread(pool.threads[i], NULL);
    SDL_DestroyCondition(pool.finished);
    SDL_DestroyCondition(pool.ready);
    SDL_DestroyMutex(pool.mutex);
    memset(&pool, 0, sizeof(pool));
    configured = -2;
}

static void Configure(void)
{
    int wanted = DesiredWorkers();
    if (wanted == configured) return;
    LQ_JobsShutdown();
    configured = wanted;
    if (!wanted) return;
    pool.mutex = SDL_CreateMutex();
    pool.ready = SDL_CreateCondition();
    pool.finished = SDL_CreateCondition();
    if (!pool.mutex || !pool.ready || !pool.finished) {
        Con_Printf("Worker pool unavailable: %s; using serial conversion.\n", SDL_GetError());
        LQ_JobsShutdown(); configured = 0; return;
    }
    for (int i = 0; i < wanted; ++i) {
        pool.worker_ids[i] = (unsigned)i;
        pool.threads[i] = SDL_CreateThread(WorkerMain, "LQ render worker", &pool.worker_ids[i]);
        if (!pool.threads[i]) {
            Con_Printf("Worker %d unavailable: %s\n", i + 1, SDL_GetError());
            break;
        }
        ++pool.workers;
    }
    /* Keep a partial pool stable if the platform refused one thread; do not
     * destroy and retry it on every frame. */
    configured = wanted;
    if (pool.workers) Con_Printf("Render conversion workers: %u.\n", pool.workers);
}

unsigned LQ_JobsWorkerCount(void)
{
    Configure();
    return pool.workers;
}

unsigned LQ_JobsDesiredWorkerCount(void)
{
    return (unsigned)DesiredWorkers();
}

static unsigned WorkerBudget(unsigned workers)
{
    lq_perf_snapshot performance = LQ_PerformanceRead();
    /* CPU contention reduces automatic parallelism without changing image
     * quality or work results. Explicit lq_workers settings remain overrides.
     * Unknown utilization is not evidence that fewer workers are beneficial. */
    if (lq_workers.value < 0 && performance.running &&
        performance.state == LQ_PERF_CONSTRAINED && workers > 1)
        return (workers + 1) / 2;
    return workers;
}

unsigned LQ_JobsWorkerBudget(void)
{
    return WorkerBudget((unsigned)DesiredWorkers());
}

void LQ_JobsRunRange(size_t count, size_t rows_per_job,
    lq_range_job_fn function, void *context)
{
    if (!count || !function) return;
    if (!rows_per_job) rows_per_job = 1;
    Configure();
    if (!pool.workers || count <= rows_per_job) {
        function(context, 0, count);
        return;
    }

    SDL_LockMutex(pool.mutex);
    pool.function = function;
    pool.context = context;
    pool.count = count;
    pool.grain = rows_per_job;
    pool.next = 0;
    pool.active_workers = WorkerBudget(pool.workers);
    pool.remaining = pool.active_workers;
    ++pool.generation;
    SDL_BroadcastCondition(pool.ready);
    SDL_UnlockMutex(pool.mutex);

    size_t begin, end;
    while (TakeRange(&begin, &end)) function(context, begin, end);

    SDL_LockMutex(pool.mutex);
    while (pool.remaining) SDL_WaitCondition(pool.finished, pool.mutex);
    SDL_UnlockMutex(pool.mutex);
}
