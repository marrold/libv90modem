/* Shared helpers for the v92 tests. */
#ifndef V92_TESTUTIL_H
#define V92_TESTUTIL_H
#include <string.h>
#include "../v92.h"

static inline uint32_t tu_lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

/* One constellation set of lc positive points (2e + 1) * half, e = 0..lc-1
 * (uniform PAM with spacing 2 * half), used in every interval. */
static inline void tu_map_uniform(v92_map_t *m, int K, int trellis,
                                  const uint8_t M[12], int lc, int half)
{
    int e;
    memset(m, 0, sizeof *m);
    m->K = (uint8_t)K;
    m->trellis = (uint8_t)trellis;
    memcpy(m->M, M, 12);
    m->lc[0] = (uint8_t)lc;
    for (e = 0; e < lc; e++) m->pt[0][e] = (uint16_t)((2 * e + 1) * half);
}

#endif
