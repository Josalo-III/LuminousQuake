/* Independent 72 Hz timeline. GPL-2.0-or-later. */
#ifndef LQ_TICK_CLOCK_H
#define LQ_TICK_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

/* AUTO wakes early by a load-dependent margin, SLEEP wakes at the deadline,
 * SPIN never sleeps (and so cannot hold real-time scheduling). */
enum { LQ_CLOCK_AUTO, LQ_CLOCK_SLEEP, LQ_CLOCK_SPIN };
/* tick is the clock's published tick unless the clock has fallen more than a
 * tick behind; fraction is the position within that tick at the read. */
typedef struct { uint64_t tick, published; double fraction; } lq_clock_sample;
typedef struct {
    bool running, spinning, realtime, load_valid;
    unsigned cores, switches, busy_percent;
    unsigned margin_us, max_late_us;
    unsigned late, skipped, fallbacks;
    uint64_t published;
} lq_clock_stats;
/* Engine bootstrap registers console controls before Host_Init completes. */
void LQ_TickClockRegister(void);
bool LQ_TickClockStart(void);
void LQ_TickClockConfigure(int mode, unsigned workers, bool active);
bool LQ_TickClockRead(lq_clock_sample *sample);
lq_clock_stats LQ_TickClockStats(void);
void LQ_TickClockStop(void);
#endif
