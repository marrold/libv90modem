/* V.92 digital modem: the training receiver for the analogue modem's Phase 3
 * and Phase 4 signals (V.92 8.5, 8.7), which arrive as 8000 linear samples
 * per second from the G.711 decoder with the echo of our own signal removed.
 *
 *   v92_p6_t   Ru and Su (8.5.5, 8.5.6) and their reversals: both repeat
 *              with a period of 6 symbols, the inverse is the same signal 3
 *              symbols later.
 *   v92_trn_t  from the first symbol of a TRN1u (8.5.7) onwards: an
 *              estimate of the upstream channel at the network's sampling
 *              instants (what the CPd design works from), and a decision
 *              feedback equaliser that turns TRN1u, Ja, CPt, E1u (2 levels)
 *              and TRN2u, SUVu, CPu, E2u (4 levels, Table 28) into symbol
 *              decisions.
 *
 * Nothing here is specified by the Recommendation; it only fixes the signals.
 * Integer only, no allocation.
 */
#ifndef V90D_V92_RX_H
#define V90D_V92_RX_H
#include "v92.h"
#include "v92_sig.h"

/* ---- Ru / Su detector ---------------------------------------------------- */

#define V92_P6_ON       1   /* a period-6 signal is there (and has been for 12 samples) */
#define V92_P6_REV      2   /* it has just turned into its inverse */
#define V92_P6_REV_LAG  4   /* ... the first inverted sample was about this many samples ago */

typedef struct {
    int16_t x[64];
    uint8_t pos, run, armed;
    int64_t c48, e48;       /* 2 * sum x(n) x(n-6) and sum x(n)^2 + x(n-6)^2, 48 samples */
    int64_t a48, e3;        /* 2 * sum x(n) x(n-3) and sum x(n)^2 + x(n-3)^2, 48 samples */
    int64_t c6, e6;         /* as c48, e48 over 6 samples */
} v92_p6_t;

void v92_p6_init(v92_p6_t *d);
/* One sample. floor_rms: signals weaker than this (linear RMS) are ignored.
 * After V92_P6_REV the detector reports nothing until the signal has been
 * steady again for about 60 samples, so the return from a 24T inverse (and
 * the half symbol slip of 9.5.2.1.7) is not reported. */
int  v92_p6_tick(v92_p6_t *d, int16_t x, int32_t floor_rms);

/* ---- training receiver ---------------------------------------------------- */

#define V92_CH_NTAP   96    /* channel estimate: 12 ms */
#define V92_CH_LEAD   24    /* tap at which a symbol sent "now" would sit */
#define V92_TRN_DL    80    /* the estimator works this many samples behind */
#define V92_EQ_NF     24    /* forward taps */
#define V92_EQ_DFF    18    /* ... of which this many look ahead of the cursor */
#define V92_EQ_NB     32    /* feedback taps */
#define V92_TRN_REF   1900  /* symbols of TRN1u taken as known (it lasts >= 2040) */
#define V92_TRN_EST0  600   /* estimator steps before the cursor is chosen */
#define V92_SYM_UNIT  4096  /* LU in the symbol scale used here (Q12) */

typedef struct {
    int16_t  rb[256];           /* received samples, by k & 255 */
    int16_t  sb[256];           /* symbols (V92_SYM_UNIT = LU), by m & 255; in data
                                   mode (v92_trn_store only) not read here: the V.92
                                   answerer keeps the baseline of each sample in it,
                                   by k & 255, and v92_trn_resume fills it again */
    int16_t  qb[32];            /* equaliser input, by k & 31 */
    int32_t  k;                 /* samples taken */
    int32_t  mref;              /* reference symbols generated */
    v92_sgn_t ref;
    /* channel estimate: r(k) = sum h[j] s(k + LEAD - j) / V92_SYM_UNIT */
    int64_t  h[V92_CH_NTAP];    /* linear, Q24 */
    int32_t  nest;              /* estimator steps */
    uint32_t res;               /* smoothed squared residual, linear^2 */
    uint8_t  est_off;           /* 1: estimator frozen */
    /* equaliser */
    uint8_t  eq_on, levels;
    int16_t  cpk;               /* cursor tap of h */
    int32_t  hpk;               /* h[cpk], linear, signed */
    int32_t  gq;                /* input gain */
    int32_t  f[V92_EQ_NF], b[V92_EQ_NB];    /* Q20 */
    int32_t  neq;               /* symbols equalised */
    uint32_t mse;               /* smoothed squared error, symbol scale */
    /* the decision just made (valid when v92_trn_tick returned 1) */
    int32_t  dec_m;             /* symbol number, 0 = first symbol of TRN1u */
    int16_t  dec_level;         /* +-1 (2 levels) or +-1, +-3 (4 levels) */
    int32_t  dec_y;             /* equaliser output, symbol scale */
} v92_trn_t;

/* Start. The next sample given to v92_trn_tick() is taken as the one at
 * which the first symbol of TRN1u arrives, give or take: an error of a few
 * samples either way and the delay of the channel (up to about 30 samples)
 * are absorbed and measured (cpk). */
void v92_trn_init(v92_trn_t *t);

/* One sample in. Returns 1 when a symbol was decided (dec_m, dec_level,
 * dec_y), which is once per sample from the moment the cursor is known,
 * about V92_TRN_EST0 + V92_TRN_DL samples after the start; -1 if no signal
 * was found by then (the receiver stays dead); else 0.
 * Symbol m is decided on sample k = m + cpk - V92_CH_LEAD + V92_EQ_DFF. */
int  v92_trn_tick(v92_trn_t *t, int16_t r);

/* 2 or 4: the constellation of the symbols decided from now on. */
void v92_trn_levels(v92_trn_t *t, int levels);

/* Freeze (on != 0) or release the channel estimator. */
void v92_trn_hold(v92_trn_t *t, int on);

/* In data mode the receiver only keeps its sample clock and the last 255
 * samples: */
static inline void v92_trn_store(v92_trn_t *t, int16_t r) { t->rb[t->k & 255] = r; t->k++; }

/* Back from data mode for a rate renegotiation (9.8): the analogue modem
 * sends Ru for 384T, Ru-bar for 24T and then TRN2u, on its data frame grid.
 * m_t is the number of the first TRN2u symbol on the receiver's symbol
 * clock (the caller knows the grid and has just seen the Ru to Ru-bar
 * change). The equaliser, kept from start-up, resumes on the known symbols
 * of Ru-bar and decides 4-level symbols from m_t on; v92_trn_tick() is to
 * be called again from the next sample. */
void v92_trn_resume(v92_trn_t *t, int32_t m_t);

/* Channel estimate, tap j in linear units * 256 for a symbol of level LU. */
static inline int32_t v92_trn_h(const v92_trn_t *t, int j) { return (int32_t)(t->h[j] >> 16); }

/* Position of the peak of the channel response in 1/256 of a sample
 * relative to tap V92_CH_LEAD (parabola through the three taps round cpk). */
int32_t v92_trn_peak(const v92_trn_t *t);

/* The received sample k (must be one of the last 255). */
static inline int16_t v92_trn_sample(const v92_trn_t *t, int32_t k) { return t->rb[k & 255]; }

#endif
