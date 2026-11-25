/* Shared helpers for the v34 tests: deterministic PRNG, Gaussian noise, bit
 * source / sink. Test side only (uses double). */
#ifndef V34_TESTUTIL_H
#define V34_TESTUTIL_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../v34.h"

static uint32_t tu_seed = 0x12345678u;
static inline uint32_t tu_rand(void)        /* xorshift32 */
{
    tu_seed ^= tu_seed << 13; tu_seed ^= tu_seed >> 17; tu_seed ^= tu_seed << 5;
    return tu_seed;
}
static inline double tu_uniform(void) { return (tu_rand() + 0.5) / 4294967296.0; }
static inline double tu_gauss(void)         /* Box-Muller, unit variance */
{
    double u = tu_uniform(), v = tu_uniform();
    return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v);
}

/* bit source: an independent LFSR so sent and received data can be compared
 * without storing them */
typedef struct { uint32_t s; unsigned long n; } tu_src_t;
static inline int tu_src_bit(tu_src_t *g)
{
    g->s ^= g->s << 13; g->s ^= g->s >> 17; g->s ^= g->s << 5;
    g->n++;
    return (g->s >> 7) & 1;
}
static inline int tu_getbit(void *u) { return tu_src_bit((tu_src_t *)u); }

/* bit sink: compares against a second copy of the source */
typedef struct { tu_src_t ref; unsigned long n, errs; unsigned long skip; } tu_sink_t;
static inline void tu_putbit(void *u, int bit)
{
    tu_sink_t *k = (tu_sink_t *)u;
    int want = tu_src_bit(&k->ref);
    k->n++;
    if (k->n > k->skip && (bit & 1) != want) k->errs++;
}
static inline void tu_pair_init(tu_src_t *src, tu_sink_t *sink, uint32_t seed)
{
    src->s = seed; src->n = 0;
    memset(sink, 0, sizeof *sink);
    sink->ref.s = seed;
}

static inline v34_pt_t tu_add_noise(v34_pt_t p, double sigma_units)
{
    double re = p.re + sigma_units * V34_ONE * tu_gauss();
    double im = p.im + sigma_units * V34_ONE * tu_gauss();
    v34_pt_t r;
    if (re > 32767) re = 32767;
    if (re < -32768) re = -32768;
    if (im > 32767) im = 32767;
    if (im < -32768) im = -32768;
    r.re = (int16_t)lrint(re); r.im = (int16_t)lrint(im);
    return r;
}

/* data rates (multiples of 2400) valid per symbol rate, Table 8/V.34 */
static const unsigned tu_srates[6] = { 2400, 2743, 2800, 3000, 3200, 3429 };
static inline unsigned tu_min_rate(unsigned s) { return s == 2400 ? 2400 : 4800; }
static inline unsigned tu_max_rate(unsigned s)
{
    switch (s) {
    case 2400: return 21600; case 2743: case 2800: return 26400;
    case 3000: return 28800; case 3200: return 31200; default: return 33600;
    }
}
#endif
