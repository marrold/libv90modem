/* V.90 analogue modem: the receiver of the PCM downstream (V.90 5, 8.4).
 *
 *   A/D samples, 8 kHz on our own clock, with the echo of what we send
 *   -> echo canceller (ec_t; our transmit samples are its reference)
 *   -> fractional interpolator on to the network's 8 kHz clock (the clock of
 *      the far codec, which is the digital modem's symbol clock)
 *   -> AGC (set once, from Sd)
 *   -> decision feedback equaliser: V90ARX_NF forward taps on the
 *      network-rate samples, V90ARX_NB feedback taps on the decided levels
 *   -> slicer: the nearest point of the constellation of the symbol's data
 *      frame interval (8.4.4: the first symbol of Sd is interval 0), giving
 *      the Ucode, its sign and the error.
 *
 * Training. The receiver watches for Sd (period 6, x(n) = -x(n-3)) and its
 * reversal into Sd-bar (8.4.4, 9.3.2.4); TRN1d follows Sd-bar's 48 symbols.
 * TRN1d is the codeword of Ucode UINFO with the signs of a GPC scrambler fed
 * with ones from the zero state (8.4.5), a known binary signal. From its
 * start an LMS estimate of the channel (V90ARX_NCH taps) runs for
 * V90ARX_EST0 symbols; the equaliser is then computed from it directly (the
 * MMSE decision feedback equaliser, by a fixed-point Cholesky solution)
 * rather than trained from scratch: LMS alone, on a line that passes little
 * below 200 Hz and above 3.7 kHz, needs seconds for what TRN1d gives in
 * 0.25 s. It refines on the rest of the known symbols (cfg.ntrain), then
 * decides by itself, on the two points of UINFO until it is given another
 * constellation. A caller that knows other symbols (DIL, TRN2d) can hand
 * them over through cfg.known.
 *
 * Tracking. The channel estimate goes on after training, decision directed
 * (its input is the symbol stream, white, so it follows every part of the
 * band equally fast), and the equaliser is computed from it again every
 * V90ARX_RECALC symbols (half or a quarter of that above 30 or 61 ppm), LMS
 * refining it in between. This matters at the band edge: our A/D samples
 * the line's 3.8 to 4.4 kHz content with a phase that drifts with the clock
 * offset, so what aliases there changes slowly, and the forward taps' LMS,
 * which sees little power there, does not keep up (about -48 dB instead of
 * -61 at 30 to 100 ppm, measured; an ideal 512-tap interpolator does no
 * better, so it is the aliasing and not the interpolator).
 *
 * Symbols whose error exceeds 4 sigma of what has been adapted on, and all
 * symbols of a slot whose error stands 9 dB above the best slot's, are left
 * out of the adaptation: a robbed bit or a pad moves points by a step, which
 * no linear equaliser can learn. The levels such impairments leave are for
 * the caller to measure (DIL) and give back in the constellation's levels.
 *
 * Timing. Sd repeats every 6 symbols of the network clock, so on our clock
 * its 1333 Hz line turns by -2 pi (offset) from one period to the next: the
 * clock offset is measured over Sd (each period against the one 16 before)
 * and set before TRN1d begins. From the equaliser's start a second order
 * loop steers the interpolator's step and a slow phase slew from how far
 * the channel estimate has moved since then (the phase of its DFT at 1 and
 * 2 kHz).
 *
 * Nothing here is specified by the Recommendation; it only fixes the
 * signals. Integer only, no allocation, no static state. The equaliser's
 * computation runs a step a symbol (v90arx_dsn_t) and takes little stack.
 */
#ifndef V90D_MODEM_V90ARX_H
#define V90D_MODEM_V90ARX_H
#include <stdint.h>
#include "ec.h"
#include "../v90/v90_scr.h"
#include "../v92/v92_rx.h"         /* v92_p6_t: the period-6 detector, for Sd */

#define V90ARX_NF    48         /* forward taps (6 ms) */
#define V90ARX_DFF   32         /* ... of which this many look ahead of the cursor */
#define V90ARX_NB    128        /* feedback taps (16 ms: the line's low frequency tail) */
#define V90ARX_NCH   128        /* channel estimate (16 ms) */
#define V90ARX_LEAD  16         /* tap of the estimate where a symbol sent "now" sits */
#define V90ARX_EST0  1700       /* estimator steps when the equaliser is first computed */
#define V90ARX_SDLAG 16         /* periods of Sd between the two ends of a phase step */
#define V90ARX_RECALC 2048      /* symbols between computations of the equaliser */
#define V90ARX_YF    4          /* fraction bits of the equaliser output (Table 1 units) */
#define V90ARX_NTRAIN 2000      /* default: TRN1d symbols taken as known (TRN1d is >= 2040) */

#define V90ARX_NT    (V90ARX_NF * (V90ARX_NF + 1) / 2)    /* the equaliser's R, packed lower triangle */

/* states */
#define V90ARX_WAIT  0          /* looking for Sd and its reversal */
#define V90ARX_EST   1          /* channel estimate on TRN1d */
#define V90ARX_TRAIN 2          /* equaliser on known symbols */
#define V90ARX_DD    3          /* decision directed */
#define V90ARX_FAIL  4          /* no channel found */

/* A constellation for each data frame interval: the points' Ucodes and the
 * level each is expected to arrive at, ascending, in Table 1 units with
 * V90ARX_YF fraction bits, on the scale on which UINFO arrives at its own
 * Table 1 level (that is what training sets). v90arx_cons_mask() fills a
 * slot from a CP-style mask with Table 1's levels; a caller that has
 * measured the levels (DIL analysis: pads, robbed bits) writes lv[][]
 * itself, keeping the order. */
typedef struct {
    uint8_t n[6];
    uint8_t u[6][128];
    int32_t lv[6][128];
} v90arx_cons_t;

void v90arx_cons_mask(v90arx_cons_t *c, int alaw, int slot, const uint8_t mask[16]);

/* A known symbol: return 1 and the Ucode and sign (1 = positive) of symbol
 * m, or 0 if m is not known. Its level is taken from Table 1. */
typedef int (*v90arx_known_fn)(void *ctx, int32_t m, int *ucode, int *positive);
/* The same with the level it arrives at (Table 1 units, V90ARX_YF fraction
 * bits, signed): for known symbols whose level has been measured (a DIL
 * through a pad or a robbed bit). Asked before `known`. */
typedef int (*v90arx_known_lv_fn)(void *ctx, int32_t m, int *ucode, int *positive, int32_t *level);

typedef struct {
    int alaw;
    int uinfo;                  /* Ucode of TRN1d, from INFO1a */
    int ntrain;                 /* TRN1d symbols taken as known; 0: V90ARX_NTRAIN */
    v90arx_known_fn known;      /* optional: further known symbols */
    void *known_ctx;
    v90arx_known_lv_fn known_lv;    /* optional: ... with their levels (same ctx) */
} v90arx_cfg_t;

/* one decision */
typedef struct {
    int32_t m;                  /* symbol number, 0 = the first symbol of TRN1d */
    uint8_t slot;               /* data frame interval, m mod 6 */
    uint8_t ucode;              /* decided (or known) Ucode */
    uint8_t sign;               /* 1 = positive */
    uint8_t known;              /* 1: a training symbol, ucode and sign are the reference */
    int32_t y;                  /* equaliser output, Table 1 units, V90ARX_YF fraction bits */
    int32_t e;                  /* y less the level of the decision, the same units */
} v90arx_dec_t;

/* The equaliser's design under way (v90arx.c, dsn_start): one step a symbol */
typedef struct {
    int      step;              /* 0: none under way */
    int      a, hs, rs, col;    /* cursor in the window, scales, next Cholesky column */
    int32_t  hh[V90ARX_NCH];    /* the estimate it is designed from, >> hs */
    int32_t  G[V90ARX_NT];      /* R, then its Cholesky factor in place */
    int32_t  z[V90ARX_NF], fs[V90ARX_NF], fn[V90ARX_NF];   /* solutions; fn: the forward taps */
    uint64_t rc[V90ARX_NF];     /* reciprocals of G's diagonal */
    int64_t  c0[V90ARX_NF], ev[V90ARX_NF];  /* R's first column; what is added along each diagonal */
    int64_t  bias; uint64_t rb; /* the solution's gain on the symbol, its reciprocal */
} v90arx_dsn_t;

typedef struct {
    v90arx_cfg_t cfg;
    int state;
    /* echo canceller, on our own clock */
    ec_t ec; int ec_mu;
    int16_t eout;               /* the last sample out of the canceller */
    /* interpolator: input ring, time to the next output (Q32 input samples) */
    int16_t ring[64]; int rpos;
    int64_t tnext;
    int64_t step;               /* input samples per output, Q32: the clock ratio */
    int64_t slew;               /* phase correction per output, Q32 */
    int32_t gain;               /* AGC, Q12 */
    uint32_t n;                 /* network-rate samples made (modem/clock.h) */
    /* Sd */
    v92_p6_t sd;
    int64_t sd_pow; int32_t sd_cnt;         /* (sd_cnt and sd_np saturate) */
    int32_t sd_br, sd_bi; int sd_i, sd_np;  /* Sd's 1333 Hz line in the period being summed */
    int32_t sd_x[V90ARX_SDLAG + 3][2];      /* ... in the last periods */
    int64_t sd_yr, sd_yi;       /* sum of the line times its conjugate SDLAG periods before */
    uint32_t n0;                /* network sample at which TRN1d's first symbol is expected */
    int32_t level;              /* RMS of Sd at the A/D, A/D units */
    /* training reference, channel estimate */
    v90_scr_t scr; int32_t mref; int16_t L;
    uint64_t l_rcp;             /* floor((2^64 - 1) / L), for divisions by L */
    uint64_t ll_rcp;            /* ... / L^2: the estimate's step while the symbols are known */
    int16_t ls[6];              /* UINFO's level by interval on TRN1d (L, or what ls_set found) */
    int64_t ls_num[6], ls_den[6];
    int16_t sb[256];            /* symbol levels (Table 1 units) by m & 255: reference or decided */
    int16_t qb[256];            /* equaliser input by k & 255 (k = n - n0) */
    int32_t h[V90ARX_NCH];      /* channel estimate, Q16, input units per symbol of level L */
    int32_t href[V90ARX_NCH];   /* ... when the equaliser was first computed: the timing reference */
    int32_t nest;               /* estimator steps (saturates) */
    int64_t res;                /* smoothed squared residual of the estimate, input units^2 */
    int16_t cpk;                /* cursor: the estimate's largest tap */
    int64_t hnext;              /* the estimate's next sum but its newest tap, formed by its last step ... */
    int32_t hnext_m0;           /* ... for the step on m0 = hnext_m0; -1: none (code that writes
                                   h or sb[] from outside sets it to -1) */
    /* equaliser */
    int16_t x[2 * V90ARX_NF]; int xpos;    /* forward line, newest first, doubled */
    int16_t d[2 * V90ARX_NB]; int dpos;    /* decided levels, newest first, doubled */
    int32_t f[V90ARX_NF];       /* Q20, to an output in Table 1 units Q YF */
    int32_t b[V90ARX_NB];       /* Q16 */
    int64_t fnext, bnext;       /* the next symbol's sums but their newest taps, formed by the last LMS step ... */
    int32_t eqnext_m;           /* ... for symbol eqnext_m; -1: none (code that writes f, b,
                                   x or d from outside sets it to -1) */
    uint32_t neq;               /* symbols equalised */
    int64_t pq, pd;             /* smoothed input and decision power, Q8 */
    int32_t kf, kb;             /* normalised steps, recomputed every 64 symbols */
    int     shf, shb;
    int     hold;               /* 1: no adaptation */
    uint64_t mse;               /* smoothed e^2, Table 1 units^2 Q8 */
    uint64_t mse_slot[6];       /* ... by data frame interval */
    int64_t gmse;               /* ... of the symbols adapted on (the gate) */
    /* timing */
    int32_t tblk;               /* symbols into the timing block */
    int32_t delta;              /* last measured shift, Q16 samples */
    /* constellations: active, and one waiting for its first symbol */
    v90arx_cons_t cons, next; int32_t next_m; int have_next;
    int deaf;                   /* 1: Sd is not looked for (v90arx_deaf) */
    int clk_set;                /* 1: the clock was given (v90arx_clock) */
    int edge;                   /* 1: the band edge's image in the design (v90arx_edge) */
    int thold;                  /* 1: the timing loop stands (v90arx_timing_hold) */
    int redo;                   /* 1: compute the equaliser at the next symbol adapted on */
    v90arx_dsn_t dsn;           /* the equaliser's design under way */
    int dsn_ran;                /* 1: a step of it ran in the last v90arx_tick() */
    /* the decisions of the last tick */
    v90arx_dec_t out[2]; int nout;
} v90arx_t;

void v90arx_init(v90arx_t *r, const v90arx_cfg_t *cfg);

/* One A/D sample rx, and what we send at the same tick (the echo canceller's
 * reference). Returns the number of decisions made (0..2), in r->out[]. */
int  v90arx_tick(v90arx_t *r, int16_t rx, int16_t tx);

/* Echo canceller step: 2^-mu_shift (0 fastest, while the downstream is
 * silent), negative to freeze. Initially frozen. */
void v90arx_ec(v90arx_t *r, int mu_shift);

/* The constellation that applies from symbol m on (the active one until then). */
void v90arx_set_cons(v90arx_t *r, const v90arx_cons_t *c, int32_t m);

/* Symbol numbers (m, and the sample number k = n - n0 it is counted on) are
 * int32_t: a call of 2^31 symbols (74.6 hours) would take them past
 * INT32_MAX. Once k reaches CLK_REBASE_AT (modem/clock.h) in data mode, the
 * owner calls v90arx_rebase(): every symbol number from then on is
 * CLK_REBASE less, and m % 6 and m & 255 are what they were. The owner
 * moves its own symbol numbers back with clk_back(). */
void v90arx_rebase(v90arx_t *r);

/* Before Sd: the clock offset as known from elsewhere (1/256 ppm, as
 * v90arx_ppm_q8). A V.92 analogue modem's transmitter runs on this clock
 * from its first symbol. Sd's own measurement (a few hundred symbols, off by
 * up to 1.6 ppm on the test lines) then replaces it only when the two
 * differ by more than 5 ppm; the timing loop goes on from there. */
void v90arx_clock(v90arx_t *r, int32_t ppm_q8);

/* While waiting for Sd: stop looking for it (1), or look again from scratch
 * (0). For a V.92 analogue modem, whose own Ru (+ + + - - -) has Sd's
 * period: its echo, before the canceller has learnt, is no Sd. */
void v90arx_deaf(v90arx_t *r, int on);

/* From now on (the equaliser is computed again at once): the band edge (3.8 to 4
 * kHz), where what folds from above 4 kHz turns with the clock offset, is
 * taken as interference (1), not as channel (0, the default). A caller sets
 * it once what it measures with the plain equaliser (the DIL's levels) is
 * done: it trades a little SNR on most lines for an equaliser that does not
 * amplify a band that will not stay as it was estimated (v90arx.c). */
void v90arx_edge(v90arx_t *r, int on);

/* Hold (1) or release (0) the timing loop alone: the step stays, no phase
 * correction; the equaliser and the estimate go on. */
void v90arx_timing_hold(v90arx_t *r, int on);

/* Freeze (1) or release (0) all adaptation: equaliser, estimate, timing. */
void v90arx_hold(v90arx_t *r, int on);

/* Clock offset of our A/D from the network, as measured, in 1/256 ppm. */
int32_t v90arx_ppm_q8(const v90arx_t *r);
/* Smoothed squared error, Table 1 units squared, Q8 (all slots, or one). */
static inline uint64_t v90arx_mse(const v90arx_t *r) { return r->mse; }
static inline uint64_t v90arx_mse_slot(const v90arx_t *r, int s) { return r->mse_slot[s]; }

#endif
