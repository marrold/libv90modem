/* The V.34 decoder's Viterbi decisions and its 8-way subset slicer, pinned
 * before audit Task 32 rewrote the add-compare-select and sliced the eight
 * subsets in one pass: both must give the same, to the bit.
 *
 * Viterbi: 10^6 noisy 2D symbols (a fixed LCG: data, noise, all integer)
 * through v34_rx_put at 16, 32 and 64 states, three noise levels each. A
 * hash takes every trellis step's survivor history (both symbols' nearest
 * point per subset, every state's predecessor and pair), the best state,
 * the smoothed path metric and every decoded bit; the program prints it.
 * The first noise level moves each coordinate by -1, 0 or +1 unit exactly:
 * received points on the midlines between lattice points, so that path
 * metrics tie all the time (all of them are multiples of one unit^2), and
 * the strict compare and the order of the predecessors decide.
 *
 * Slicer: v34_slice_subsets against eight v34_slice_subset calls, metric
 * and point, on the int16 grid, for every constellation the parameter sets
 * give without precoding (the inside test differs per L; eight of them
 * have 8 points or fewer) and for three precoded sets (no inside test;
 * ymax 9, 47 and 227). Both clamp their input to (ymax + 4) units first,
 * so the grid is taken up to two units beyond that, and on the plane's
 * edges.
 *
 * On the grid every coordinate is a multiple of 8, which hides some
 * roundings (there a squared distance in the coordinates re + im, re - im
 * is always a multiple of 128), so random points anywhere in the window are
 * taken too.
 *
 *   test_vit_hash            the suite's run: grid step 40 (136 on ARM;
 *                            points of the step-8 grid), and 10000 random
 *                            points a set (1000 on ARM)
 *   test_vit_hash full       the slicer alone, grid step 8, and the random
 *                            points (a minute)
 *   test_vit_hash plane K N  the slicer alone, the whole int16 plane in
 *                            steps of 8, the sets K mod N only (N processes
 *                            by hand; some 30 CPU minutes in all) */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "../v34.h"
#include "../v34_priv.h"

#ifdef TEST_SHORT
#define NSYM     20000UL        /* per run, 9 runs */
#define SL_SUITE 136
#define SL_RAND  1000
#else
#define NSYM     111112UL       /* per run: 9 runs, 1 000 008 symbols */
#define SL_SUITE 40
#define SL_RAND  10000
#endif

/* ---- integers only, so that every target computes the same ---- */
static uint32_t lcg = 1u;
static uint32_t lcg_next(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return lcg;
}

static uint32_t fnv;
static void hash_byte(unsigned b)
{
    fnv ^= b & 0xFFu;
    fnv *= 16777619u;
}
static void hash_u32(uint32_t v)
{
    hash_byte(v); hash_byte(v >> 8); hash_byte(v >> 16); hash_byte(v >> 24);
}
static void hash_i16(int v) { hash_u32((uint32_t)(uint16_t)(int16_t)v); }

static int data_bit(void *u)
{
    (void)u;
    return (int)(lcg_next() >> 31);
}
static unsigned long nbits;
static void put_bit(void *u, int bit)
{
    (void)u;
    hash_byte((unsigned)bit & 1);
    nbits++;
}

static uint32_t isqrt(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

/* noise per coordinate, Q7: level 0 -1, 0 or +1 unit (0 twice as often);
 * otherwise about Gaussian (four uniforms) with standard deviation sigma */
static int noise(int level, uint32_t sigma)
{
    if (level == 0) {
        static const int u[4] = { -V34_ONE, 0, 0, V34_ONE };
        return u[lcg_next() >> 30];
    } else {
        int32_t s = 0, k;
        for (k = 0; k < 4; k++)
            s += (int32_t)(lcg_next() >> 20);
        return (int)((s - 8190) * (int32_t)sigma / 2365);     /* 4 * 4096^2 / 12 = 2365^2 */
    }
}

static v34_params_t p;
static v34_tx_t tx;
static v34_rx_t rx;

static int setup(unsigned S, unsigned R, int trellis, int expanded, const int16_t (*h)[2])
{
    v34_cfg_t c;
    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R;
    c.trellis = (uint8_t)trellis; c.expanded = (uint8_t)expanded; c.scrambler = V34_GPA;
    if (h) memcpy(c.h, h, sizeof c.h);
    return v34_params_init(&p, &c);
}

/* h(1..3) {re, im} in 2.14 */
static const int16_t H_MILD[3][2] = { { -9830, 3277 }, { 4096, -1638 }, { -819, 410 } };
static const int16_t H_BIG[3][2]  = { { 21299, -8192 }, { -13107, 9830 }, { 6554, 3277 } };

/* one run of NSYM symbols; esn0_ratio: Es/N0 (not in dB) of the Gaussian levels */
static void vit_run(unsigned S, unsigned R, int trellis, int expanded, const int16_t (*h)[2],
                    int level, unsigned esn0_ratio)
{
    unsigned long n, steps = 0;
    uint32_t sigma = 0, last = 0;
    unsigned ns, i, j;

    CHECK_EQ_I(setup(S, R, trellis, expanded, h), 0);
    ns = p.nstates;
    if (level)  /* sigma^2 = E / (2 Es/N0) units^2, in Q7: E_q8 * 2^14 / 2^8 / (2 ratio) */
        sigma = isqrt((uint64_t)p.energy_q8 * 32u / esn0_ratio);
    lcg = 0x9E3779B9u ^ (uint32_t)(R * 7u + (unsigned)trellis * 131u + (unsigned)level);
    fnv = 2166136261u;
    nbits = 0;
    v34_tx_init(&tx, &p, data_bit, NULL);
    v34_rx_init(&rx, &p, put_bit, NULL);
    for (n = 0; n < NSYM; n++) {
        v34_pt_t x, r;
        int32_t re, im;
        v34_tx_symbol(&tx, &x, NULL);
        re = x.re + noise(level, sigma);
        im = x.im + noise(level, sigma);
        r.re = (int16_t)(re > 32767 ? 32767 : re < -32768 ? -32768 : re);
        r.im = (int16_t)(im > 32767 ? 32767 : im < -32768 ? -32768 : im);
        v34_rx_put(&rx, r, NULL);
        if (rx.nsteps != last) {
            const v34_vstep_t *st = &rx.hist[rx.head ? rx.head - 1u : V34_VIT_DEPTH - 1u];
            last = rx.nsteps;
            steps++;
            for (i = 0; i < 2; i++)
                for (j = 0; j < 8; j++) {
                    hash_i16(st->pt[i][j][0]);
                    hash_i16(st->pt[i][j][1]);
                }
            for (i = 0; i < ns; i++) {
                hash_byte(st->prev[i]);
                hash_byte(st->pair[i]);
            }
            hash_byte(rx.best);
            hash_u32(rx.err_avg);
        }
    }
    v34_rx_flush(&rx);
    hash_u32(rx.b1_errors); hash_u32(rx.bad_points); hash_u32(rx.shell_errors); hash_u32(rx.reseeds);
    printf("  %u/%u %2u states %s%s, noise %d: %lu symbols, %lu steps, %lu bits, hash %08lx\n",
           S, R, ns, expanded ? "expanded" : "minimum", h ? ", precoded" : "", level,
           n, steps, nbits, (unsigned long)fnv);
    CHECK(steps == NSYM / 2);
}

/* ---- the slicer ---- */
static unsigned long sl_pts, sl_diff;
static uint32_t sl_hash = 2166136261u;

static void sl_point(int re, int im)
{
    uint32_t m[8], mr;
    int16_t pt[8][2], pr[2];
    unsigned s;
    v34_slice_subsets(&p, re, im, m, pt);
    sl_pts++;
    for (s = 0; s < 8; s++) {
        mr = v34_slice_subset(&p, re, im, s, pr);
        if (m[s] != mr || pt[s][0] != pr[0] || pt[s][1] != pr[1]) {
            if (sl_diff < 5)
                printf("  (%d, %d) subset %u: %lu (%d, %d), 8 calls %lu (%d, %d)\n", re, im, s,
                       (unsigned long)m[s], pt[s][0], pt[s][1], (unsigned long)mr, pr[0], pr[1]);
            sl_diff++;
        }
        /* the hash is of the eight-call reference (mr, pr), not of
         * v34_slice_subsets' output: the one-pass slicer is held to it
         * point by point (sl_diff == 0) */
        fnv = sl_hash;
        hash_u32(mr); hash_i16(pr[0]); hash_i16(pr[1]);
        sl_hash = fnv;
    }
}

/* the grid -32768 + step k on both axes: the points within the clamping
 * window and two units beyond, and the plane's edges; all of them (plane) */
static int sl_step, sl_plane;
static int sl_take(int v, int lim)
{
    return sl_plane || v == -32768 || v > 32767 - sl_step ||
           (v >= -lim - 2 * V34_ONE && v <= lim + 2 * V34_ONE);
}
static void sl_set(void)
{
    int lim = (p.ymax + 4) * V34_ONE, re, im;
    for (re = -32768; re <= 32767; re += sl_step) {
        if (!sl_take(re, lim))
            continue;
        for (im = -32768; im <= 32767; im += sl_step)
            if (sl_take(im, lim))
                sl_point(re, im);
    }
    /* points anywhere (off the grid) within the window and two units beyond */
    if (!sl_plane) {
        uint32_t w = (uint32_t)(2 * lim + 4 * V34_ONE + 1), k;
        for (k = 0; k < SL_RAND; k++) {
            re = (int)(lcg_next() % w) - lim - 2 * V34_ONE;
            im = (int)(lcg_next() % w) - lim - 2 * V34_ONE;
            sl_point(re < -32768 ? -32768 : re > 32767 ? 32767 : re,
                     im < -32768 ? -32768 : im > 32767 ? 32767 : im);
        }
    }
}

/* the parameter sets: every distinct constellation without precoding,
 * then three precoded; set k is sliced if k mod n == part */
static void slicer(unsigned part, unsigned n)
{
    static const struct { unsigned S, R; int e; const int16_t (*h)[2]; } pre[3] = {
        { 2400, 4800, 0, H_MILD }, { 3200, 24000, 1, H_MILD }, { 3429, 33600, 1, H_BIG } };
    unsigned keys[256], nkeys = 0, small = 0, si, k, R, set = 0;
    int e;
    lcg = 0x51CE5EEDu;
    for (si = 0; si < 6; si++) {
        unsigned S = tu_srates[si];
        for (R = tu_min_rate(S); R <= tu_max_rate(S); R += 2400)
            for (e = 0; e <= 1; e++) {
                unsigned key;
                if (setup(S, R, V34_TRELLIS_64, e, NULL) != 0)
                    continue;
                key = (unsigned)p.npts << 8 | p.ymax;
                for (k = 0; k < nkeys && keys[k] != key; k++)
                    ;
                if (k < nkeys || nkeys == 256)
                    continue;
                keys[nkeys++] = key;
                if (p.npts <= 8) small++;
                if (set++ % n == part)
                    sl_set();
            }
    }
    printf("  slicer: grid step %d%s, %lu random points a set; %u constellations without precoding (%u of 8 points or fewer)\n",
           sl_step, sl_plane ? ", whole plane" : "", sl_plane ? 0UL : (unsigned long)SL_RAND, nkeys, small);
    printf("  slicer: precoded, ymax");
    for (k = 0; k < 3; k++) {
        CHECK_EQ_I(setup(pre[k].S, pre[k].R, V34_TRELLIS_64, pre[k].e, pre[k].h), 0);
        printf(" %u", p.ymax);
        if (set++ % n == part)
            sl_set();
    }
    printf("\n  slicer: %lu points, %lu differences, hash %08lx\n", sl_pts, sl_diff, (unsigned long)sl_hash);
    CHECK_EQ_I(sl_diff, 0);
}

int main(int argc, char **argv)
{
    int full = argc == 2 && strcmp(argv[1], "full") == 0;
    int plane = argc == 4 && strcmp(argv[1], "plane") == 0, level;
    unsigned part = 0, n = 1;
    if (plane) {
        part = (unsigned)strtoul(argv[2], NULL, 10);
        n = (unsigned)strtoul(argv[3], NULL, 10);
    }
    if (argc > 1 && !full && !(plane && n > 0 && part < n)) {
        printf("usage: %s [full | plane K N]\n", argv[0]);
        return 2;
    }
    sl_step = (full || plane) ? 8 : SL_SUITE;
    sl_plane = plane;
    if (!full && !plane)
        for (level = 0; level < 3; level++) {
            /* Es/N0 20 dB (100) and 13 dB (20): about the waterfall and
             * well below it */
            unsigned ratio = level == 1 ? 100u : 20u;
            vit_run(3200, 19200, V34_TRELLIS_16, 0, NULL, level, ratio);
            vit_run(3200, 24000, V34_TRELLIS_32, 1, H_MILD, level, ratio);
            vit_run(3000, 21600, V34_TRELLIS_64, 0, NULL, level, ratio);
        }
    slicer(part, n);
    T_REPORT("vit_hash");
}
