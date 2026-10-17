/* Shared CPU resource policy; consumers choose their own response.
 * GPL-2.0-or-later. */
#ifndef LQ_PERFORMANCE_H
#define LQ_PERFORMANCE_H
#include <stdbool.h>

typedef enum { LQ_PERF_UNKNOWN, LQ_PERF_CONSTRAINED, LQ_PERF_HEADROOM } lq_perf_state;
typedef struct {
    lq_perf_state state;
    bool running, load_valid;
    unsigned cores, busy_percent, transitions;
} lq_perf_snapshot;
typedef bool (*lq_perf_load_fn)(void *context, double *busy);

/* Lifecycle belongs to the main thread. Sampling runs on a low-frequency
 * monitor; any consumer may read the coherent atomic snapshot without waiting.
 * This policy measures CPU pressure, not GPU pressure or frame-budget misses. */
bool LQ_PerformanceStart(unsigned cores, lq_perf_load_fn sampler, void *context);
void LQ_PerformanceStop(void);
lq_perf_snapshot LQ_PerformanceRead(void);
const char *LQ_PerformanceStateName(lq_perf_state state);
#endif
