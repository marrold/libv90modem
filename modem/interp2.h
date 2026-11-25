/* Two neighbouring branches of a fractional-delay bank laid on a ring of
 * samples (audit Task 33): qamrx, pcmtx and v90arx each interpolate between
 * the outputs of branches p and p + 1.
 *
 *   interp2_32(ring, idx, mask, h0, h1, n, &acc0, &acc1)
 *   interp2_64(...)                     the same with int64 accumulators
 *
 * acc0 = sum ring[(idx - i) & mask] h0[i], acc1 the same with h1, over
 * i = 0 .. n - 1: the newest sample (idx) first. The ring has mask + 1
 * entries (a power of two, at least n). The sums run in the two straight
 * runs that do not wrap, ring[idx & mask] down to ring[0], then
 * ring[mask] down, with no index masking, four taps a pass; each
 * accumulator receives the same products in the same order as the plain
 * loop over i.
 *
 * interp2_32 is for a bank and samples whose sums fit an int32: with
 * |sample| <= 32768, a bank whose every row has sum |h| < 2^16 (v34_interp:
 * 46728, test/test_interp_bound.c). */
#ifndef V90M_INTERP2_H
#define V90M_INTERP2_H

#include <stdint.h>

/* the body of both: T is the accumulators' type. In each run s walks down
 * from one past the run's newest sample to its oldest, so that it never
 * points before the ring; h0 and h1 walk up. Four taps a pass, then the
 * run's last 0 to 3 one at a time: ARMv5 code (GCC 12.3) takes 6
 * instructions a tap in the first loop, 7 in the second; two taps a pass
 * took 7 as well. */
#define INTERP2_TAP                                                         \
    s0 = *--s; a0 += s0 * *h0++; a1 += s0 * *h1++
#define INTERP2_RUN(len)                                                    \
    do {                                                                    \
        const int16_t *stop = s - ((len) & ~3);                             \
        int32_t s0;                                                         \
        while (s != stop) {                                                 \
            INTERP2_TAP; INTERP2_TAP; INTERP2_TAP; INTERP2_TAP;             \
        }                                                                   \
        stop = s - ((len) & 3);                                             \
        while (s != stop) {                                                 \
            INTERP2_TAP;                                                    \
        }                                                                   \
    } while (0)
#define INTERP2_BODY(T)                                                     \
    T a0 = 0, a1 = 0;                                                       \
    int j = idx & mask, n1 = j + 1 < n ? j + 1 : n;                         \
    const int16_t *s = ring + j + 1;                                        \
    INTERP2_RUN(n1);                              /* ring[idx] down */      \
    s = ring + mask + 1;                                                    \
    INTERP2_RUN(n - n1);                          /* ring[mask] down */     \
    *acc0 = a0; *acc1 = a1

static inline void interp2_32(const int16_t *ring, int idx, int mask,
                              const int16_t *h0, const int16_t *h1, int n,
                              int32_t *acc0, int32_t *acc1)
{
    INTERP2_BODY(int32_t);
}

static inline void interp2_64(const int16_t *ring, int idx, int mask,
                              const int16_t *h0, const int16_t *h1, int n,
                              int64_t *acc0, int64_t *acc1)
{
    INTERP2_BODY(int64_t);
}

#undef INTERP2_BODY
#undef INTERP2_RUN
#undef INTERP2_TAP

#endif
