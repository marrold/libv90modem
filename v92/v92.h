/* v90d/v92 - common definitions for the V.92 PCM upstream building blocks.
 *
 * Written from ITU-T V.92 (11/2000). See README.md for the source map, the
 * spec ambiguities and the readings chosen.
 *
 * Integer only, no allocation, no globals: every object is a caller-owned
 * struct, any number of instances may coexist.
 *
 * FRAMING (Figure 1/V.92). The upstream symbol rate is 8000. A data frame is
 * 12 symbols, i = 0..11; a constellation frame is 6 symbols, j = i mod 6; a
 * trellis frame is 4 symbols, k = i mod 4. K bits enter the modulus encoder
 * per data frame.
 *
 * LINEAR SCALE. Constellation points, u(n) and the decoder input are plain
 * integers on the 16-bit linear scale of Table 1/V.90 (what v90_ucode_linear
 * returns: mu-law up to 32124, A-law up to 32256). V.92 does not name the
 * unit of the "linear value" fields of CPd; this is the reading chosen.
 */
#ifndef V90D_V92_H
#define V90D_V92_H

#include <stdint.h>

/* same values as the V90_ERR_ codes of ../v90/v90_seq.h */
#define V92_ERR_SHORT  (-1)   /* fewer than the needed bits available */
#define V92_ERR_SYNC   (-2)   /* frame sync, a start bit or a fixed bit is wrong */
#define V92_ERR_CRC    (-3)   /* CRC mismatch */
#define V92_ERR_RANGE  (-4)   /* a value cannot be encoded / is not allowed */

#define V92_DFRAME     12     /* symbols per data frame */
#define V92_CFRAME     6      /* symbols per constellation frame */
#define V92_TFRAME     4      /* symbols per trellis frame */

#define V92_NCONST     6      /* constellation sets CPd can carry */
#define V92_MAX_LC     128    /* positive points per constellation set (8.8.3) */
#define V92_MAX_LTOT   384    /* largest Ltot of INFO1a bits 14:15 */
#define V92_MAX_LSEC   320    /* largest Lmax of INFO1a bits 16:17 */
#define V92_MAX_K      72     /* bits per data frame at 48000 bit/s */

/* Trellis encoder select, same coding as CPd bits 27:28 (and as V34_TRELLIS_x). */
enum { V92_TRELLIS_16 = 0, V92_TRELLIS_32 = 1, V92_TRELLIS_64 = 2 };

/* Upstream data signalling rate: CPd drn = 1..19 <-> (drn + 17) * 8000 / 6
 * bit/s (24000 .. 48000) <-> K = 2 * (drn + 17) bits per data frame (36..72).
 * Bit r of an upstream rate mask (Ja) is the rate with drn = r + 1. */
#define V92_UP_NUM_RATES   19
#define V92_UP_K(drn)      (2 * ((drn) + 17))
#define V92_UP_BPS(drn)    (((drn) + 17) * 4000 / 3)

typedef int  (*v92_getbit_fn)(void *user);           /* returns 0 or 1 */
typedef void (*v92_putbit_fn)(void *user, int bit);

/* ---- data mode mapping parameters (the "what" of CPd) ------------------- */

/* Everything the upstream encoder and the decoder must agree on. The digital
 * modem dictates all of it in CPd (Table 30/V.92). */
typedef struct {
    uint8_t  K;                 /* bits per data frame, 1..72 (36..72 in a CPd) */
    uint8_t  trellis;           /* V92_TRELLIS_x */
    uint8_t  M[V92_DFRAME];     /* modulus encoder parameters M0..M11, >= 1 */
    uint8_t  cidx[V92_CFRAME];  /* constellation set used in data frame
                                   intervals j and j + 6 */
    uint8_t  lc[V92_NCONST];    /* LC: positive points in each set, 0 = absent */
    uint16_t pt[V92_NCONST][V92_MAX_LC];  /* positive points, smallest first,
                                   linear scale. The set is symmetric: with
                                   N = 2 * LC points a(eta), -LC <= eta < LC,
                                   a(eta) = pt[eta] for eta >= 0 and
                                   a(eta) = -pt[-eta - 1] for eta < 0 (6.4.2) */
} v92_map_t;

/* 0 if the parameters are usable, else V92_ERR_RANGE:
 *  - trellis is 0..2, K is 1..72, every Mi >= 1 and 2^K <= M0 * .. * M11 (6.4.1);
 *  - every cidx names a set with lc >= 1; points are > 0, <= 32767 and
 *    strictly ascending (8.8.3: no zero point);
 *  - every equivalence class of 6.4.2 has at least one point: Mi <= N in
 *    intervals with k = 0, 1, 2 and 2 * Mi <= N in intervals with k = 3. */
int v92_map_check(const v92_map_t *m);

/* a(eta) of the constellation used in data frame interval i. eta must be in
 * -LC .. LC - 1. */
static inline int32_t v92_point(const v92_map_t *m, int i, int eta)
{
    const uint16_t *p = m->pt[m->cidx[i % V92_CFRAME]];
    return eta >= 0 ? (int32_t)p[eta] : -(int32_t)p[-eta - 1];
}

/* LC of the constellation used in data frame interval i */
static inline int v92_lc(const v92_map_t *m, int i)
{
    return m->lc[m->cidx[i % V92_CFRAME]];
}

/* The modulus encoder output Ki that index eta stands for in data frame
 * interval i (inverse of the equivalence classes of 6.4.2):
 * eta mod Mi for k = 0, 1, 2 and floor(eta / 2) mod Mi for k = 3. */
int v92_eta_to_k(const v92_map_t *m, int i, int eta);

/* ---- precoder and prefilter (the "how" of CPd) -------------------------- */

/* 6.4.2:
 *   x(n) = u(n) + sum(k = 1..LZ1) u(n-k) z1(k) + sum(k = 1..LP1) x(n-k) p1(k)
 *   v(n) = sum(k = 0..LZ2-1) x(n-k) z2(k) + sum(k = 1..LP2) v(n-k) p2(k)
 * and the transmitter output is G * v(n).
 * coef holds, in the order CPd carries them: z1(1..LZ1) signed Q0.15,
 * p1(1..LP1) signed Q1.14, z2(0..LZ2-1) signed Q0.15, p2(1..LP2) signed Q1.14. */
typedef struct {
    uint16_t lz1, lp1, lz2, lp2;
    uint16_t gain4;             /* 4 * G in unsigned Q0.16: G = gain4 / 2^18 */
    int16_t  coef[V92_MAX_LTOT];
} v92_filt_t;

/* 0, or V92_ERR_RANGE: a section longer than 320, more than 384 taps in
 * all, LZ2 = 0 (no output at all) or gain4 = 0 (Table 30: 4 * G > 0). */
int v92_filt_check(const v92_filt_t *f);

/* *dst = *src without the zero coefficients at the end of each section
 * (z2(0) kept): the same precoder and prefilter, fewer taps to sum when a
 * server pads its sections. src must pass v92_filt_check(); dst may not be
 * src. */
void v92_filt_trim(v92_filt_t *dst, const v92_filt_t *src);

#endif
