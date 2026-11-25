/* V.92 digital modem: the design of what CPd carries (8.8.3, Table 30), from
 * the measured upstream channel: prefilter z2, precoder feedback p1, gain G,
 * constellation, moduli and data rate.
 *
 * The Recommendation fixes the format only. The method here:
 *
 *  1. Prefilter. With h the measured response from the analogue modem's
 *     symbols to our samples and g = h * w the response with a prefilter w
 *     of LZ2 taps: w minimises
 *         (g(D) - 1)^2 + sum of g(k)^2 over k < D and k > D + LP1
 *         + lambda sum of w(j)^2
 *     with the cursor D the prefilter's length less V92_DSN_POST after the
 *     peak of h, and w(V92_DSN_Z2_SKIP) held at zero (a prefilter of 64
 *     taps or fewer has no such coefficient; its cursor is half its length
 *     after the peak).
 *     The LP1 samples after the cursor are left free: the precoder takes
 *     them. lambda weighs transmit power against residual interference; it
 *     is the noise variance over the squared peak of h. The normal
 *     equations are solved by conjugate gradients, each product with the
 *     matrix being a convolution and a correlation with h.
 *  2. Precoder. g is computed as the analogue modem will produce it, with
 *     the coefficients as sent; with the cursor scaled to 1,
 *     p1(k) = -g(D + k): the feedback removes the interference of the LP1
 *     symbols after the cursor exactly, so that sample m + D holds u(m).
 *     What is left elsewhere is counted as noise. LZ1 = LP2 = 0.
 *  3. Gain. 4G is the smallest value that keeps every z2 coefficient inside
 *     Q0.15; z2 is then scaled so that G z2 is exact. The power limit of
 *     8.8.3 (mean square of G v(n) at most 1) is met by the size of the
 *     constellation.
 *  4. Constellation: one set. Points from the smallest up, each at kappa
 *     noise deviations from its neighbour, the noise at amplitude a being
 *     N0 + rho q(a + 1.5 echo)^2 / 12 + ISI: q the step of the G.711 law at
 *     that amplitude (the echo of our own signal lifts the operating point;
 *     1.5 times its RMS because the errors are made at its peaks);
 *     N0 what was measured with the analogue modem silent less the
 *     quantiser's share, plus an allowance for residual echo and estimation
 *     error in data mode (see ST_POINTS); rho the factor by which the noise seen during
 *     training exceeds the quantiser's share there (digital pads, the
 *     estimate's own error); ISI what the taps of g outside the cursor and
 *     the feedback leave.
 *     The interference term is first taken from the taps of g with x(n)
 *     white, then measured in a trial run of the analogue modem's
 *     transmitter (x(n) is not white), and the constellation built again.
 *     Every candidate up to the top of the codec's range is in the set: the
 *     outer ones are there for the precoder, which uses a point only when
 *     that makes its output small.
 *  5. Moduli and rate. With a base modulus M: Mi = M where k = 0, 1, 2 and
 *     M / 2 where k = 3; K is the highest rate allowed by Ja with
 *     2^K <= M0 .. M11; the moduli are then lowered while that holds. M is
 *     the largest for which the analogue modem's transmitter, run here on
 *     random data (v92_enc, the "minimum power" rule the NOTE of 8.8.3
 *     prescribes), stays within the power limit: bisection with trials of
 *     V92_DSN_MC symbols.
 *  6. A final trial of V92_DSN_VERIFY symbols. The members of an
 *     equivalence class are not evenly spaced, so the precoder output is
 *     not bounded as with a true modulo. If the largest interference sample
 *     stands far out of the variance assumed, the constellation is built
 *     again for it (at most twice); if the power limit is broken or x(n)
 *     makes excursions beyond V92_DSN_XPEAK times its RMS, the modulus is
 *     lowered and the trial repeated.
 *
 * The work is cut into steps of bounded cost, one per received sample.
 * Integer only, no allocation.
 */
#ifndef V90D_V92_DESIGN_H
#define V90D_V92_DESIGN_H
#include "v92.h"
#include "v92_seq.h"
#include "v92_enc.h"
#include "v92_rx.h"

typedef struct {
    int      alaw;
    int      rx_linear;         /* the codec hands us linear samples: q() below is 0, the
                                   echo no longer sets the spacing of the points, and
                                   q_quiet and q_trn are 0 */
    int32_t  h[V92_CH_NTAP];    /* channel: v92_trn_h() */
    uint32_t noise;             /* variance with the analogue modem silent, linear^2 */
    uint32_t q_quiet;           /* mean of v92_dsn_qvar() over the codec samples of that time */
    uint32_t echo_rms;          /* RMS of our echo at the codec, linear */
    uint32_t res;               /* variance of the estimator's residual (v92_trn_t.res) */
    uint32_t q_trn;             /* mean of v92_dsn_qvar() over the codec samples of that time */
    uint16_t ltot, lmax;        /* INFO1a: total and per-section tap limits */
    uint32_t rates;             /* Ja: bit r = (r + 18) * 8000 / 6 bit/s */
    int      max_rate;          /* bit/s, 0 = no limit of ours */
    int      kappa_q4;          /* point spacing in noise deviations, Q4; 0 = V92_DSN_KAPPA */
} v92_dsn_cfg;

#define V92_DSN_KAPPA   112     /* 7.0 */
#define V92_DSN_LZ2     128
#define V92_DSN_LP1     64
#define V92_DSN_Z2_SKIP 64      /* z2(64) is kept at zero: a real analogue modem takes it
                                   as zero whatever CPd says (reading 58) */
#define V92_DSN_POST    16      /* ... and the cursor sits this far from the end of z2,
                                   to have that coefficient among the small ones */
#define V92_DSN_ITER    160     /* conjugate gradient iterations at most */
#define V92_DSN_RESTART 20      /* ... with a fresh residual every so many */
#define V92_DSN_VERIFY  12000   /* symbols of the final trial */
#define V92_DSN_XPEAK   56      /* 3.5: largest |x(n)| over its RMS allowed in it, Q4 */
#define V92_DSN_NG      (V92_CH_NTAP + V92_DSN_LZ2)
#define V92_DSN_MC      1200    /* symbols of each transmit power trial */

enum {
    V92_DSN_ERR_CHANNEL = -1,   /* no usable channel response */
    V92_DSN_ERR_RATE = -2       /* not even the lowest rate allowed fits */
};

typedef struct {
    v92_dsn_cfg cfg;
    int      st, i, sub;
    int32_t  hn[V92_CH_NTAP];   /* the channel, peak = 1, Q14 */
    int32_t  hpk; int pk;       /* the peak (as cfg.h) and its tap */
    int32_t  cw[V92_DSN_LZ2], cr[V92_DSN_LZ2], cp[V92_DSN_LZ2], cq[V92_DSN_LZ2];   /* conjugate gradient, Q24 */
    int64_t  rr; int iter, resid;
    int32_t  w[V92_DSN_LZ2];    /* the prefilter before scaling, Q30 */
    int32_t  g[V92_DSN_NG];     /* work space; then h * z2 before scaling, Q14 */
    int      lz2, lp1, s0;
    int      skip;              /* the z2 coefficient kept at zero, 0 = none */
    int32_t  g0;
    uint32_t lambda;
    uint32_t rho_q8;            /* noise seen in training over the model */
    uint32_t isi_q28;           /* residual ISI power per unit of precoder output power */
    uint32_t xrms_max;          /* precoder output RMS at the power limit, linear */
    int      ncand, lo, hi, lc, base;
    int      cut;               /* times the final trial failed */
    uint32_t xpk_q4;            /* largest |x(n)| over its RMS in the final trial, Q4 */
    uint32_t rng;
    uint32_t usum;              /* sum of u(n)^2 / 2^14 in the trial */
    int64_t  pacc, iacc, xacc, imax;
    int32_t  xh[256];           /* x(n) of a trial, linear * 2^V92_XQ */
    int      pass;              /* 0: first trial, measuring the interference; then
                                   the number of constellations built on a measurement */
    uint32_t isi_var;           /* ... its variance, linear^2 */
    v92_enc_t enc;
    /* results */
    v92_cpd_t cpd;              /* has_mod = has_filt = has_const = 1 */
    int      dly;               /* u(m) is in the sample taken V92_CH_LEAD + dly ... see below */
    uint32_t power_q14;         /* mean square of G v(n) in the last trial, 16384 = 1 */
    int      steps;             /* steps taken */
} v92_dsn_t;

void v92_dsn_start(v92_dsn_t *d, const v92_dsn_cfg *cfg);
/* 0: call again; 1: done, cpd and dly are valid; < 0: V92_DSN_ERR_x. */
int  v92_dsn_step(v92_dsn_t *d);

/* dly: with the sample clock of the v92_trn_t the channel estimate came from
 * (sample k carries symbol m = k through tap V92_CH_LEAD), u(m) is to be
 * read from sample k = m + dly. */

/* Variance of the G.711 quantiser at amplitude a (linear): step^2 / 12. */
uint32_t v92_dsn_qvar(int alaw, int32_t a);

#endif
