/* Sample clocks that outlast 2^31 samples (74.6 hours at 8 kHz): a call in
 * data mode, or a host that goes on ticking after the end, runs that long.
 *
 *   A counter that only grows (samples in a phase, compared with a limit)
 *   stops at INT32_MAX: t = clk_sat_inc(t).
 *
 *   A clock read through differences (now - when) is a uint32_t that wraps
 *   modulo 2^32, and every difference is clk_diff(now, when): the distance
 *   as an int32_t, right while the two are less than 2^31 samples apart.
 *
 *   A time not set is CLK_NONE, the -1 of the signed clocks: a difference
 *   from it is what it was (now + 1). A time set at the one sample where the
 *   clock reads CLK_NONE is taken as not set, and set again a sample later.
 *
 * All of it is defined C on every target; below 2^31 samples they give what the
 * signed clocks gave. */
#ifndef V90D_MODEM_CLOCK_H
#define V90D_MODEM_CLOCK_H
#include <stdint.h>

#define CLK_NONE UINT32_MAX

/* t + 1, held at INT32_MAX */
static inline int32_t clk_sat_inc(int32_t t) { return t < INT32_MAX ? t + 1 : INT32_MAX; }

/* a - b modulo 2^32, as an int32_t in [INT32_MIN, INT32_MAX] (no
 * implementation-defined conversion of a value above INT32_MAX) */
static inline int32_t clk_diff(uint32_t a, uint32_t b)
{
    uint32_t d = a - b;
    return d <= (uint32_t)INT32_MAX ? (int32_t)d : -(int32_t)(UINT32_MAX - d) - 1;
}

/* Symbol and sample numbers that are int32_t and run on through data mode
 * (a receiver's k and m): once one reaches CLK_REBASE_AT, its owner takes
 * CLK_REBASE off it and off every number counted on it. 3 * 2^28: a
 * multiple of 3, of the data frames (6, 12) and of the rings (64, 256), so
 * that every number keeps its place modulo these. clk_back() moves one: -1
 * (none) stays; one from before CLK_REBASE, long past, becomes one long
 * past (under 768, while the current one is at least 2^28) with the same
 * value modulo 768. */
#define CLK_REBASE_AT 0x40000000
#define CLK_REBASE    0x30000000
static inline int32_t clk_back(int32_t m)
{
    return m < 0 ? m : m >= CLK_REBASE ? m - CLK_REBASE : m % 768;
}

#endif
