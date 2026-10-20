/* Background queue: one low-priority thread for work that may finish frames
 * later (load-time proofs, caches, gradual refreshes). It never blocks a
 * frame. New LuminousQuake code, GPL-2.0-or-later.
 *
 * run executes on the background thread, in submission order. It must only
 * read data its owner keeps alive until the task finishes or is cancelled,
 * must not call engine services (console, cvars, LQ_JobsRunRange), and should
 * return early once *cancel becomes nonzero.
 *
 * finish executes on the main thread, from LQ_BackgroundPoll, once run has
 * returned, or from LQ_BackgroundCancel/Shutdown with cancelled set (run may
 * then not have started). It is where results are applied and the context is
 * freed. */
#ifndef LQ_BACKGROUND_H
#define LQ_BACKGROUND_H
#include <stdint.h>

typedef void (*lq_background_run_fn)(void *context, const volatile int *cancel);
typedef void (*lq_background_finish_fn)(void *context, int cancelled);

/* Queues a task for owner (any nonzero tag the caller chooses); 0 when the
 * queue is unavailable, in which case finish has already run cancelled. */
int LQ_BackgroundSubmit(uint32_t owner, lq_background_run_fn run, lq_background_finish_fn finish, void *context);
/* Cancels every task of owner: queued ones never run, a running one is asked
 * to stop and waited for. Their finish functions run before this returns. */
void LQ_BackgroundCancel(uint32_t owner);
/* Delivers finished tasks; call once per frame from the main thread. */
void LQ_BackgroundPoll(void);
/* Cancels everything and stops the thread; the next submit restarts it. */
void LQ_BackgroundShutdown(void);
/* Tasks queued or running. */
unsigned LQ_BackgroundPending(void);
#endif
