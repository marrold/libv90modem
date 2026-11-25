/* The V.34 upstream receiver the digital-side modems share (p34d, v34dx,
 * x2d, k56d): the far transmitter's PP and TRN as a known
 * reference, 4- and 16-point decisions for its sequences, and V.34 data
 * mode with its watches on the line. What to do about what it reports
 * (retrain, a lower rate, cleardown, the S of a renegotiation) is the
 * modem's own.
 *
 * Per symbol: v34up_tick on each sample, then on a symbol one of
 * v34up_known, v34up_dec4, v34up_dec16, v34up_data (or qamrx_update with
 * no flags, to keep the grid running). k56d's data mode, which keeps the
 * points of B1 and may decode them again (v34up_retry, v34up_replay_sym),
 * makes its own steps of v34up_data and ends each symbol with
 * v34up_watch. */
#ifndef V90D_MODEM_V34UP_H
#define V90D_MODEM_V34UP_H
#include <stdint.h>
#include "qamrx.h"
#include "ec.h"
#include "ecdly.h"
#include "v34train.h"
#include "../v34/v34.h"

/* is the line still good enough for the rate? (rx.mse in data mode is the
 * error power over the mean signal power, Q16) */
#define V34UP_SNR0_DB10  90     /* the rule of TRN: 9 dB + 3 dB per bit per symbol ... */
#define V34UP_SLACK_DB10 45     /* ... is missed by this much. The rule keeps about 3 dB in
                                   hand, and in data mode the error is some dB above what
                                   TRN promised (our own louder signal's echo through the
                                   codec): a rate that works must not be given up. */
#define V34UP_BAD_SYMS   3200   /* for about a second */
#define V34UP_DEAD_MSE   (65536 / 8)    /* -9 dB: nothing is being received */
#define V34UP_LOSS_SYMS  1600   /* about half a second without signal */

typedef struct {
    /* (in an order that leaves no padding) */
    qamrx_t      rx;
    v34_tdem_t   tdem;          /* the sequences' descrambler and differential decoder */
    v34trn_t     ref;           /* TRN reference (the owner picks its scrambler) */
    int32_t      quiet;         /* symbols without signal in data mode */
    int64_t      zpw;           /* smoothed |z|^2 */
    int32_t      q_dead, q_bad; /* symbols in a row above -9 dB; above mse_limit */
    int32_t      mse_limit;     /* receiver error above which the rate is too high */
    int32_t      gain;          /* data mode: v34_gain_q14 of the constellation */
    v34_params_t par;
    int          rx_ready;      /* B1 passed: what is decoded is data */
    v34_rx_t     vrx;
} v34up_t;

/* what v34up_data saw */
enum {
    V34UP_NONE,
    V34UP_B1_BAD,   /* B1 complete, more than one bit in 8 wrong */
    V34UP_DEAD,     /* nothing received for V34UP_BAD_SYMS (watch_dead) */
    V34UP_SLOW,     /* the error above mse_limit for V34UP_BAD_SYMS (watch_slow) */
    V34UP_LOST      /* no signal for V34UP_LOSS_SYMS */
};

static inline int16_t clip16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* 16 bits, the first the least significant (J, J') */
static inline uint16_t word16(const uint8_t *b)
{
    uint16_t w = 0; int i;
    for (i = 0; i < 16; i++) w |= (uint16_t)(b[i] << i);
    return w;
}

/* One received sample (after the echo canceller): 1 when a symbol is ready
 * in rx.zr / rx.zi. In data mode (data != 0) a click is blanked
 * (qamrx_blank); the S and tone detectors are given x as it is. */
static inline int v34up_tick(v34up_t *u, int16_t x, int data)
{
    qamrx_t *r = &u->rx;
    if (!qamrx_tick(r, data ? qamrx_blank(r, x) : x)) return 0;
    u->zpw += (((int64_t)r->zr * r->zr + (int64_t)r->zi * r->zi) - u->zpw) / 16;
    return 1;
}

/* B1 is complete in the decoder and not yet decided */
static inline int v34up_b1_in(const v34up_t *u)
{
    return !u->rx_ready && u->vrx.b1_bits >= u->par.map.N;
}

/* Data mode's scaling: the equaliser's output (QRX_REF units) to the
 * constellation's (v34_pt_t), and a decision back */
static inline v34_pt_t v34up_scale_in(const v34up_t *u)
{
    v34_pt_t in;
    int64_t g = u->gain;
    in.re = clip16((int32_t)(((int64_t)u->rx.zr * 512) / g)); in.im = clip16((int32_t)(((int64_t)u->rx.zi * 512) / g));
    return in;
}
static inline void v34up_scale_out(const v34up_t *u, v34_pt_t pt, int32_t *dr, int32_t *di)
{
    int64_t g = u->gain;
    *dr = (int32_t)(((int64_t)pt.re * g) / 512); *di = (int32_t)(((int64_t)pt.im * g) / 512);
}

/* S / S-bar heard (sdet_tick): PP(0) is centred 16.5 symbols after the
 * boundary, and the event came SDET_DELAY samples after it */
void     v34up_sync_pp(v34up_t *u, const sdet_t *sd, int sym_rate);
/* PP, then TRN from u->ref, as the known reference; 1 on the last of the
 * n_trn symbols of TRN taken so */
int      v34up_known(v34up_t *u, int n_trn);
/* 4-point decision, the loops updated with upd_flags (QRX_*); returns the
 * two bits through u->tdem */
unsigned v34up_dec4(v34up_t *u, unsigned upd_flags);
/* 16-point decision; gate_eq: the equaliser learns only from decisions
 * within half a step of a point (a 4-point signal at 16-point power lands
 * between them). Returns the four bits through u->tdem */
unsigned v34up_dec16(v34up_t *u, int gate_eq);
/* E has ended, B1 starts with the next symbol: data mode with c (and the
 * putbit given to v34_rx_init); mse_limit from c's rate. -1: c cannot be
 * done (v34_params_init) */
int      v34up_data_begin(v34up_t *u, const v34_cfg_t *c, v34_putbit_fn put, void *user);
/* One symbol of data mode. B1 is checked as it completes, rx_ready set
 * when it passes; once ready, watch_dead / watch_slow count V34UP_DEAD /
 * V34UP_SLOW. Returns V34UP_*: the first that happened */
int      v34up_data(v34up_t *u, int watch_dead, int watch_slow);
/* The watches of one data-mode symbol, after its qamrx_update: once B1
 * has passed (rx_ready), watch_dead / watch_slow count V34UP_DEAD /
 * V34UP_SLOW; the loss of the signal is counted always. Returns V34UP_*:
 * the first that happened */
int      v34up_watch(v34up_t *u, int watch_dead, int watch_slow);
/* B1 failed under one configuration: data mode again from B1 under alt
 * (the same putbit; the gain alt's). The equaliser, the watches and
 * mse_limit are left as they are (the rate is the same). -1: alt cannot
 * be done (v34_params_init) */
int      v34up_retry(v34up_t *u, const v34_cfg_t *alt);
/* One stored point (v34up_scale_in's) into the decoder, the loops not
 * updated: 1 when B1 is then complete and not yet decided (v34up_b1_in) */
int      v34up_replay_sym(v34up_t *u, int16_t re, int16_t im);
/* S in data mode (v34_rx_s_run): the frames the decoder holds from before
 * S are data and go on now, but for a frame of margin
 * (v34_rx_flush_before); rx_ready ends. 0: an S too new in the decoder is
 * waited for (SDET_RUN_WAIT samples): call again on the next sample */
int      v34up_s_flush(v34up_t *u, const sdet_t *sd);
/* The echo canceller's training on our own signal, the far end silent, n
 * samples into it. The first *from samples are left alone; when the echo
 * delay is not known (measure), the ECDLY_SAMPLES after them find it
 * (ecdly_measure with diff; ec is reset when found) and *from moves past
 * them. Returns the NLMS step: -1 (none) before *from, mu_fast for the
 * fast samples after it, then 3. */
int      v34up_ec_schedule(ec_t *ec, ecdly_t *ecd, int measure, int diff, int16_t rxs,
                           int32_t n, int32_t *from, int32_t fast, int mu_fast);

#endif
