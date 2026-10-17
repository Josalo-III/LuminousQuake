/* Non-SDL executables retain their existing synchronous timing path. */
#include "tick_clock.h"
bool LQ_TickClockStart(void) { return false; }
void LQ_TickClockConfigure(int m, unsigned w, bool a) { (void)m; (void)w; (void)a; }
bool LQ_TickClockRead(lq_clock_sample *s) { (void)s; return false; }
lq_clock_stats LQ_TickClockStats(void) { lq_clock_stats s = {0}; return s; }
void LQ_TickClockStop(void) { }
