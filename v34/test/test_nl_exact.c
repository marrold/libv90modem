/* The non-linear decoder (v34_nl_decode), its Phi (v34_nl_phi_q16) and the
 * encoder (v34_nl_encode) against their code of v0.5.0, before audit Task 31
 * took most of the decoder's 64-bit divisions out; and the exact divisions
 * that change uses (modem/fixdiv.h, modem/compiler.h) against C's.
 *
 * Each ref_* function is the v0.5.0 code, kept here verbatim (only renamed)
 * as the oracle; the library must give the same point, the same Phi:
 *   - for every symbol rate, data rate and shaping (every v34_params_t the
 *     decoder can be given; trellis and precoder do not enter it), on a grid
 *     over the int16 plane, step 256 (step 1 with `full`), on the plane's
 *     edges, and on 20000 random points about the constellation (200000
 *     with `full`);
 *   - Phi also for energies of any size, and negative ones down to
 *     -2^32 (below that zeta^2 overflows int64 in the v0.5.0 code too);
 *   - divlu and div_s64_u32 against / on 10^7 random cases each and their
 *     edges; V90M_CLZ32 against a count of the bits.
 * The Makefile builds this twice: test_nl_exact, and test_nl_exact_nb with
 * -DV90M_NO_BUILTINS (the library's v34_nl.c and this file both), so that
 * the builtins and their C fallbacks are both checked.
 *
 *   test_nl_exact            the suite's run
 *   test_nl_exact full       the whole plane, step 1 (hours)
 *   test_nl_exact full K N   only the grid's columns re = -32768 + K mod N
 *                            (N processes by hand; the random points and the
 *                            divisions in K = 0 only)
 *   test_nl_exact dsp        the audit's (dsp's) scope, minutes: the plane
 *                            in steps of 97 (re) by 89 (im) and 200000
 *                            random points a set (90 767 560 of the two,
 *                            besides the edges) */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "../v34.h"
#include "../../modem/compiler.h"
#include "../../modem/fixdiv.h"

#ifdef TEST_SHORT
#define GRID_STEP 1024
#define N_RAND    2000
#define N_DIV     1000000L
#else
#define GRID_STEP 256
#define N_RAND    20000
#define N_DIV     10000000L
#endif

static uint64_t rs = 0x9e3779b97f4a7c15u;
static uint32_t rnd32(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 32);
}
static uint64_t rnd64(void) { uint64_t h = rnd32(); return (h << 32) | rnd32(); }

/* ---------------- reference copy of v0.5.0 v34/v34_nl.c:12 ---------------- */
static int64_t ref_nl_phi_q16(uint32_t energy_q8, int64_t e)
{
    /* zeta = (5 / 16) e / (energy_q8 * 2^6), in Q16 */
    int64_t z = (e * 320) / (int64_t)energy_q8;
    if (z > 8 * 65536) z = 8 * 65536;
    return 65536 + z / 6 + ((z * z) >> 16) / 120;
}

/* ---------------- reference copy of v0.5.0 v34/v34_nl.c:20 ---------------- */
static int64_t ref_phi_q16(const v34_params_t *p, int64_t e) { return ref_nl_phi_q16(p->energy_q8, e); }

/* ---------------- reference copy of v0.5.0 v34/v34_nl.c:22 ---------------- */
static int16_t ref_sat(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
/* ---------------- reference copy of v0.5.0 v34/v34_nl.c:23 ---------------- */
static int64_t ref_rsh16(int64_t v) { return (v + 32768) >> 16; }

/* ---------------- reference copy of v0.5.0 v34/v34_nl.c:25 ---------------- */
static v34_pt_t ref_nl_encode(const v34_params_t *p, v34_pt_t x)
{
    v34_pt_t y = x;
    if (p->cfg.nonlinear) {
        int64_t phi = ref_phi_q16(p, (int64_t)x.re * x.re + (int64_t)x.im * x.im);
        y.re = ref_sat(ref_rsh16(x.re * phi)); y.im = ref_sat(ref_rsh16(x.im * phi));
    }
    return y;
}

/* ---------------- reference copy of v0.5.0 v34/v34_nl.c:35 ---------------- */
static v34_pt_t ref_nl_decode(const v34_params_t *p, v34_pt_t xp)
{
    v34_pt_t y = xp;
    if (p->cfg.nonlinear) {
        /* |x'|^2 = |x|^2 Phi(|x|^2)^2: fixed-point iteration on |x|^2, which
         * closes the error by a factor of three or more each round for any
         * point a constellation holds (up to four times the mean energy) */
        int64_t ep = (int64_t)xp.re * xp.re + (int64_t)xp.im * xp.im, e = ep, phi;
        int k;
        for (k = 0; k < 8; k++) {
            phi = ref_phi_q16(p, e);
            e = (ep << 28) / ((phi * phi) >> 4);
        }
        phi = ref_phi_q16(p, e);
        y.re = ref_sat(((int64_t)xp.re * 131072 / phi + (xp.re < 0 ? -1 : 1)) / 2);
        y.im = ref_sat(((int64_t)xp.im * 131072 / phi + (xp.im < 0 ? -1 : 1)) / 2);
    }
    return y;
}

/* ---------------- the comparisons ---------------- */

static long n_dec, d_dec, n_enc, d_enc, n_phi, d_phi;

static void one_point(const v34_params_t *p, int re, int im)
{
    v34_pt_t x, a, b;
    int64_t e;
    x.re = (int16_t)re; x.im = (int16_t)im;
    a = v34_nl_decode(p, x); b = ref_nl_decode(p, x);
    n_dec++;
    if (a.re != b.re || a.im != b.im) {
        if (d_dec < 5) printf("  decode (%d, %d), energy_q8 %lu: %d %d, v0.5.0 %d %d\n", re, im,
                              (unsigned long)p->energy_q8, a.re, a.im, b.re, b.im);
        d_dec++;
    }
    a = v34_nl_encode(p, x); b = ref_nl_encode(p, x);
    n_enc++;
    if (a.re != b.re || a.im != b.im) {
        if (d_enc < 5) printf("  encode (%d, %d), energy_q8 %lu: %d %d, v0.5.0 %d %d\n", re, im,
                              (unsigned long)p->energy_q8, a.re, a.im, b.re, b.im);
        d_enc++;
    }
    e = (int64_t)re * re + (int64_t)im * im;
    n_phi++;
    if (v34_nl_phi_q16(p->energy_q8, e) != ref_nl_phi_q16(p->energy_q8, e)) d_phi++;
}

static void phi_any(const v34_params_t *p, int64_t e)
{
    n_phi++;
    if (v34_nl_phi_q16(p->energy_q8, e) != ref_nl_phi_q16(p->energy_q8, e)) {
        if (d_phi < 5) printf("  phi(%lld), energy_q8 %lu\n", (long long)e, (unsigned long)p->energy_q8);
        d_phi++;
    }
}

static long n_lu, d_lu, n_sd, d_sd, n_clz, d_clz;

static void one_divlu(uint32_t u1, uint32_t u0, uint32_t v)
{
    uint64_t want = (((uint64_t)u1 << 32) | u0) / v;
    uint32_t got = divlu(u1, u0, v);
    n_lu++;
    if (got != want) {
        if (d_lu < 5) printf("  divlu(%lu, %lu, %lu) = %lu, want %llu\n", (unsigned long)u1,
                             (unsigned long)u0, (unsigned long)v, (unsigned long)got, (unsigned long long)want);
        d_lu++;
    }
}

static void one_sdiv(int64_t n, uint32_t d)
{
    int64_t want = n / (int64_t)d, got = div_s64_u32(n, d);
    n_sd++;
    if (got != want) {
        if (d_sd < 5) printf("  div_s64_u32(%lld, %lu) = %lld, want %lld\n", (long long)n,
                             (unsigned long)d, (long long)got, (long long)want);
        d_sd++;
    }
}

static void one_clz(uint32_t x)
{
    int want = 0;
    uint32_t m = 0x80000000u;
    while (!(x & m)) { want++; m >>= 1; }
    n_clz++;
    if (V90M_CLZ32(x) != want) {
        if (d_clz < 5) printf("  V90M_CLZ32(%#lx) = %d, want %d\n", (unsigned long)x, V90M_CLZ32(x), want);
        d_clz++;
    }
}

/* a random 32-bit number of a random length: every normalising shift */
static uint32_t rnd_len32(void) { return rnd32() >> (rnd32() & 31); }

static void divisions(void)
{
    static const uint32_t ve[] = { 1, 2, 3, 5, 7, 0xFFFFu, 0x10000u, 0x10001u, 0x7FFFu, 0x8000u, 0x8001u,
                                   0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFF0000u, 0xFFFF0001u,
                                   0x8000FFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu, 23456789u, 186283u };
    static const uint32_t ue[] = { 0, 1, 2, 0xFFFFu, 0x10000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFF0000u,
                                   0xFFFFFFFEu, 0xFFFFFFFFu };
    static const int64_t ne[] = { 0, 1, -1, 2, -2, INT64_MAX, INT64_MIN, INT64_MIN + 1, INT64_MAX - 1,
                                  (int64_t)1 << 32, -((int64_t)1 << 32), ((int64_t)1 << 32) - 1,
                                  -(((int64_t)1 << 32) - 1), ((int64_t)1 << 32) + 1, (int64_t)1 << 62,
                                  -((int64_t)1 << 62), ((int64_t)1 << 40) - 1 };
    unsigned i, j, k, s;
    long n;
    /* edges: v, u1 below it, u0; and every power of two, one above and below */
    for (i = 0; i < sizeof ve / sizeof ve[0]; i++) {
        uint32_t v = ve[i], u1s[6];
        u1s[0] = 0; u1s[1] = v - 1; u1s[2] = v / 2; u1s[3] = v > 1 ? 1 : 0;
        u1s[4] = v > 2 ? v - 2 : 0; u1s[5] = v / 3;
        for (j = 0; j < 6; j++) for (k = 0; k < sizeof ue / sizeof ue[0]; k++) one_divlu(u1s[j], ue[k], v);
        for (k = 0; k < sizeof ne / sizeof ne[0]; k++) one_sdiv(ne[k], v);
    }
    for (s = 0; s < 32; s++) {
        uint32_t p2 = (uint32_t)1 << s, vs[3];
        vs[0] = p2; vs[1] = p2 + 1; vs[2] = p2 - 1;
        one_clz(p2); one_clz(p2 | 1u); one_clz(p2 | (p2 - 1));
        for (j = 0; j < 3; j++) {
            if (vs[j] == 0) continue;
            for (k = 0; k < sizeof ue / sizeof ue[0]; k++) {
                one_divlu(0, ue[k], vs[j]); one_divlu(vs[j] - 1, ue[k], vs[j]);
            }
            for (k = 0; k < sizeof ne / sizeof ne[0]; k++) one_sdiv(ne[k], vs[j]);
        }
    }
    /* random: v of every length, u1 anywhere below it or just below it;
     * n of every length and either sign, d of every length */
    for (n = 0; n < N_DIV; n++) {
        uint32_t v = rnd_len32(), u1;
        int64_t sn;
        uint32_t d;
        if (v == 0) v = 1;
        u1 = (n & 1) ? rnd32() % v : v - 1 - rnd32() % (v < 16 ? v : 16);
        one_divlu(u1, rnd32(), v);
        sn = (int64_t)(rnd64() >> (rnd32() & 63));
        if (rnd32() & 1) sn = -sn;
        d = rnd_len32();
        if (d == 0) d = 1;
        one_sdiv(sn, d);
        if (v) one_clz(v);
    }
    printf("  divlu: %ld cases, %ld differences\n", n_lu, d_lu);
    printf("  div_s64_u32: %ld cases, %ld differences\n", n_sd, d_sd);
    printf("  V90M_CLZ32: %ld cases, %ld differences\n", n_clz, d_clz);
    CHECK_EQ_I(d_lu, 0); CHECK_EQ_I(d_sd, 0); CHECK_EQ_I(d_clz, 0);
}

int main(int argc, char **argv)
{
    static const int syms[] = { 2400, 2743, 2800, 3000, 3200, 3429 };
    static const int edge[] = { -32768, -32767, -32766, -16384, -12345, -2, -1, 0, 1, 2, 12345, 16384,
                                32766, 32767 };
    int full = argc > 1 && strcmp(argv[1], "full") == 0, dsp = argc == 2 && strcmp(argv[1], "dsp") == 0;
    int step = full ? 1 : dsp ? 97 : GRID_STEP, step_im = dsp ? 89 : step;
    long shard = 0, nshard = 1, nrand = full || dsp ? 200000 : N_RAND;
    int si, r, ex, nsets = 0;
    if (full && argc == 4) { shard = strtol(argv[2], NULL, 10); nshard = strtol(argv[3], NULL, 10); }
    if ((argc > 1 && !full && !dsp) || (full && argc != 2 && argc != 4) || nshard < 1 || shard < 0 || shard >= nshard) {
        printf("usage: %s [full [K N] | dsp]\n", argv[0]);
        return 2;
    }
    for (si = 0; si < 6; si++) for (r = 2400; r <= 33600; r += 2400) for (ex = 0; ex < 2; ex++) {
        static v34_params_t p;
        v34_cfg_t c;
        long col = 0, k;
        int re, im, i, j, R;
        memset(&c, 0, sizeof c);
        c.symbol_rate = (uint16_t)syms[si]; c.data_rate = (uint16_t)r; c.expanded = (uint8_t)ex;
        c.scrambler = V34_GPA; c.nonlinear = 1;
        if (v34_params_init(&p, &c) != 0) continue;
        nsets++;
        /* the grid, in columns of re (a shard takes every N-th) */
        for (re = -32768; re <= 32767; re += step, col++) {
            if (col % nshard != shard) continue;
            for (im = -32768; im <= 32767; im += step_im) one_point(&p, re, im);
        }
        if (shard != 0) continue;
        /* the plane's edges */
        for (i = 0; i < (int)(sizeof edge / sizeof edge[0]); i++)
            for (j = 0; j < (int)(sizeof edge / sizeof edge[0]); j++) one_point(&p, edge[i], edge[j]);
        /* about the constellation: |coordinate| up to 1.5 (ymax + 2) units
         * (the non-linear encoder takes a point at most 1.25 times out) */
        R = (p.ymax + 2) * 192;
        for (k = 0; k < nrand; k++) {
            re = (int)(rnd32() % (uint32_t)(2 * R + 1)) - R;
            im = (int)(rnd32() % (uint32_t)(2 * R + 1)) - R;
            one_point(&p, re, im);
        }
        /* Phi of energies of any size (e 320 within int64), and of
         * negative ones above -2^32 (zeta^2 within int64, energy_q8 >= 512) */
        for (k = 0; k < nrand; k++) {
            int64_t e = (int64_t)(rnd64() >> (10 + (rnd32() % 54)));
            phi_any(&p, (k & 3) == 3 ? -(e >> 22) : e);
        }
        if (c.symbol_rate == 3200 && r == 28800 && !ex) {
            /* Theta = 0 leaves every point as it is, both ways */
            v34_params_t q = p;
            q.cfg.nonlinear = 0;
            for (re = -32768; re <= 32767; re += 4096) for (im = -32768; im <= 32767; im += 4096) one_point(&q, re, im);
        }
    }
    if (step_im != step) printf("  %d parameter sets; grid steps %d x %d\n", nsets, step, step_im);
    else printf("  %d parameter sets; grid step %d%s\n", nsets, step,
           nshard > 1 ? " (one shard)" : "");
    printf("  v34_nl_decode: %ld points, %ld differences\n", n_dec, d_dec);
    printf("  v34_nl_encode: %ld points, %ld differences\n", n_enc, d_enc);
    printf("  v34_nl_phi_q16: %ld energies, %ld differences\n", n_phi, d_phi);
    CHECK_EQ_I(nsets, 130);
    CHECK_EQ_I(d_dec, 0); CHECK_EQ_I(d_enc, 0); CHECK_EQ_I(d_phi, 0);
    if (shard == 0) divisions();
    printf("  %ld differences\n", d_dec + d_enc + d_phi + d_lu + d_sd + d_clz);
    T_REPORT("nl_exact");
}
