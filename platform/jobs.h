/* Small persistent range-job pool for snapshot-owned rendering work.
 * New LuminousQuake code, GPL-2.0-or-later. */
#ifndef LQ_JOBS_H
#define LQ_JOBS_H
#include <stddef.h>

typedef void (*lq_range_job_fn)(void *context, size_t begin, size_t end);

void LQ_JobsRegister(void);
void LQ_JobsRunRange(size_t count, size_t rows_per_job,
    lq_range_job_fn function, void *context);
void LQ_JobsShutdown(void);
unsigned LQ_JobsWorkerCount(void);

#endif
