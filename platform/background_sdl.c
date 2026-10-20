/* Background queue on one low-priority SDL thread (see background.h).
 * New LuminousQuake code, GPL-2.0-or-later. */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include "background.h"

typedef struct lq_background_task {
    struct lq_background_task *next;
    uint32_t owner;
    lq_background_run_fn run;
    lq_background_finish_fn finish;
    void *context;
    volatile int cancel;
} lq_background_task;

static struct {
    SDL_Mutex *mutex;
    SDL_Condition *wake, *idle;
    SDL_Thread *thread;
    lq_background_task *queued, *queued_tail, *done, *running;
    unsigned pending;
    int stopping;
} queue;

static void Append(lq_background_task **head, lq_background_task **tail, lq_background_task *task)
{
    task->next = NULL;
    if (*tail) (*tail)->next = task; else *head = task;
    *tail = task;
}

static int BackgroundMain(void *unused)
{
    (void)unused;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    SDL_LockMutex(queue.mutex);
    for (;;) {
        while (!queue.stopping && !queue.queued) SDL_WaitCondition(queue.wake, queue.mutex);
        if (queue.stopping) break;
        lq_background_task *task = queue.queued;
        queue.queued = task->next;
        if (!queue.queued) queue.queued_tail = NULL;
        queue.running = task;
        SDL_UnlockMutex(queue.mutex);
        if (!task->cancel) task->run(task->context, &task->cancel);
        SDL_LockMutex(queue.mutex);
        queue.running = NULL;
        /* Finished tasks wait for the main thread, newest first; order of
         * delivery does not matter to their owners. */
        task->next = queue.done; queue.done = task;
        SDL_BroadcastCondition(queue.idle);
    }
    SDL_UnlockMutex(queue.mutex);
    return 0;
}

static int Start(void)
{
    if (queue.thread) return 1;
    if (!queue.mutex) queue.mutex = SDL_CreateMutex();
    if (!queue.wake) queue.wake = SDL_CreateCondition();
    if (!queue.idle) queue.idle = SDL_CreateCondition();
    if (!queue.mutex || !queue.wake || !queue.idle) return 0;
    queue.stopping = 0;
    queue.thread = SDL_CreateThread(BackgroundMain, "LQ background", NULL);
    return queue.thread != NULL;
}

int LQ_BackgroundSubmit(uint32_t owner, lq_background_run_fn run, lq_background_finish_fn finish, void *context)
{
    lq_background_task *task = run && owner && Start() ? calloc(1, sizeof(*task)) : NULL;
    if (!task) { if (finish) finish(context, 1); return 0; }
    task->owner = owner; task->run = run; task->finish = finish; task->context = context;
    SDL_LockMutex(queue.mutex);
    Append(&queue.queued, &queue.queued_tail, task);
    ++queue.pending;
    SDL_SignalCondition(queue.wake);
    SDL_UnlockMutex(queue.mutex);
    return 1;
}

static void Deliver(lq_background_task *list, int cancelled)
{
    while (list) {
        lq_background_task *next = list->next;
        if (list->finish) list->finish(list->context, cancelled || list->cancel);
        free(list); list = next;
    }
}

/* Removes owner's tasks (every task when owner is 0) from the queue and the
 * finished list, waiting out a running one; the caller delivers them. */
static lq_background_task *Withdraw(uint32_t owner)
{
    lq_background_task *taken = NULL, **link;
    if (!queue.mutex) return NULL;
    SDL_LockMutex(queue.mutex);
    /* Flag the queued ones first, so the thread skips them rather than
     * starting one while the running one is waited for. */
    for (lq_background_task *task = queue.queued; task; task = task->next)
        if (!owner || task->owner == owner) task->cancel = 1;
    if (queue.running && (!owner || queue.running->owner == owner)) queue.running->cancel = 1;
    while (queue.running && (!owner || queue.running->owner == owner))
        SDL_WaitCondition(queue.idle, queue.mutex);
    for (int list = 0; list < 2; ++list) {
        link = list ? &queue.done : &queue.queued;
        while (*link) {
            lq_background_task *task = *link;
            if (owner && task->owner != owner) { link = &task->next; continue; }
            *link = task->next;
            task->next = taken; taken = task;
            --queue.pending;
        }
    }
    queue.queued_tail = NULL;
    for (lq_background_task *task = queue.queued; task; task = task->next) queue.queued_tail = task;
    SDL_UnlockMutex(queue.mutex);
    return taken;
}

void LQ_BackgroundCancel(uint32_t owner)
{
    if (owner) Deliver(Withdraw(owner), 1);
}

void LQ_BackgroundPoll(void)
{
    if (!queue.mutex) return;
    SDL_LockMutex(queue.mutex);
    lq_background_task *done = queue.done;
    for (lq_background_task *task = done; task; task = task->next) --queue.pending;
    queue.done = NULL;
    SDL_UnlockMutex(queue.mutex);
    Deliver(done, 0);
}

void LQ_BackgroundShutdown(void)
{
    Deliver(Withdraw(0), 1);
    if (queue.thread) {
        SDL_LockMutex(queue.mutex);
        queue.stopping = 1;
        SDL_BroadcastCondition(queue.wake);
        SDL_UnlockMutex(queue.mutex);
        SDL_WaitThread(queue.thread, NULL);
        queue.thread = NULL;
    }
    /* Anything finished while stopping. */
    Deliver(Withdraw(0), 1);
}

unsigned LQ_BackgroundPending(void)
{
    if (!queue.mutex) return 0;
    SDL_LockMutex(queue.mutex);
    unsigned pending = queue.pending;
    SDL_UnlockMutex(queue.mutex);
    return pending;
}
