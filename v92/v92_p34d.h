/* V.92 Phases 3 and 4 and data mode with PCM upstream, digital modem side
 * (V.92 9.5.1, 9.6.1, 9.7.1, 9.8.1, 9.11).
 *
 *   silent: Ru, Ru-bar, TRN1u, Ja from the caller (channel estimate, training
 *   equaliser, DIL descriptor and upstream rates) -> Sd, Sd-bar, TRN1d (the
 *   caller is silent; we learn our echo and the noise floor), Jd until Su and
 *   its reversal, Jp (the timing phase the caller shall move to) until the
 *   next reversal, Jp', the DIL or SCR while the caller sends TRN1u again
 *   (estimate and equaliser from scratch; data frame count from its first
 *   symbol) -> Ri on CPt, Ri-bar on E1u (DIL) or Ri when trained, Ri-bar on
 *   CPt (no DIL) -> TRN2d; on TRN2u the design of CPd; SUVd until SUVu, one
 *   CPd, SUVd; acknowledge once CPu is in; Ed, B1d, data; upstream E2u, B1u,
 *   data through v92_dec.
 *
 * One received linear sample in, one PCM octet out, at 8 kHz, exactly as
 * ../modem/p34d does for V.90: the owner runs Phase 2 (p2d with
 * p2d_offer_v92), and when its mode is P2D_MODE_V92 starts this instead of
 * p34d. A retrain ends this module in V92D_RETRAIN, silent: the owner sends
 * the 70 ms of silence, runs Phase 2 again and then a fresh v92d (or p34d,
 * if that Phase 2 decides for V.34 upstream).
 *
 * In data mode: rate renegotiation (9.8) started by either side, without the
 * silence option, and cleardown (9.11) by either side. An upstream signal
 * that stays away for half a second ends the module (V92D_ERR_CARRIER), as
 * in ../modem/p34d: the Recommendation has no procedure for a caller that
 * is gone. The signal is looked for in the line as well as after the echo
 * canceller, and a line that brings only the idle code has no Ru in it,
 * whatever the canceller gives out (it gives out its estimate of our echo).
 * Not here: quick connect, modem-on-hold, fast parameter exchange (9.9).
 */
#ifndef V90D_V92_P34D_H
#define V90D_V92_P34D_H
#include <stdint.h>
#include "../modem/seqhunt.h"
#include "../modem/ec.h"
#include "../modem/ecdly.h"
#include "../modem/tonedet.h"
#include "../v90/v90_tx.h"
#include "v92_rx.h"
#include "v92_design.h"
#include "v92_dec.h"
#include "v92_seq.h"

typedef int  (*v92d_getbit_fn)(void *user);
typedef void (*v92d_putbit_fn)(void *user, int bit);

typedef struct {
    int alaw;               /* law of our codec */
    int uinfo;              /* INFO1a (Table 18) bits 25:31 */
    int md_len;             /* INFO1a bits 18:24, units of 276 symbols */
    int sections;           /* INFO1a bits 12:13 (not used: z1 and p2 are never asked for) */
    int ltot, lmax;         /* INFO1a: 192 + 64 * code, 128 + 64 * code */
    int rtd;                /* round-trip delay from Phase 2, samples */
    int echo_delay;         /* samples from handing an octet to the line until its echo
                               starts to come back (driver buffering), 0..V92D_MAX_ECHO_DELAY;
                               the canceller covers the 12 ms after that. -1 = not
                               known: measured during TRN1d (../modem/ecdly.h) */
    uint32_t down_rates;    /* Jd: downstream rates allowed, as p34d_cfg; 0 = all */
    int max_up_rate;        /* upstream ceiling in bit/s; 0 = what the line gives */
    int down_rms;           /* the most our downstream data signal can have, linear RMS
                               (what INFO0d bits 33:37 told the caller); 0 = -9 dBm0.
                               The echo the upstream has to live with follows from it */
    int kappa_q4;           /* constellation spacing, see v92_design.h; 0 = default */
    int rx_linear;          /* the received samples did not pass a G.711 encoder (a codec
                               run in linear mode): no quantiser in the noise model */
    int dsn_pace;           /* the CPd design takes one of its steps every so many
                               samples; 0 or 1 = every sample. A step costs two to
                               four times what a sample of data mode costs (reading
                               56): a slow processor needs 4 */
    v92d_getbit_fn getbit;  /* data to send */
    v92d_putbit_fn putbit;  /* data received */
    void *user;
} v92d_cfg;

typedef enum {
    V92D_WAIT_RU,   /* silent, waiting for Ru and Ru-bar */
    V92D_MD,        /* waiting out the caller's MD */
    V92D_TRN1U,     /* TRN1u: channel estimate, equaliser; then Ja */
    V92D_SD,        /* Sd, Sd-bar, TRN1d: echo canceller training, noise floor */
    V92D_JD,        /* Jd until Su and Su-bar */
    V92D_JP,        /* Jp until the second Su-bar */
    V92D_TRAIN,     /* Jp', DIL or SCR; second TRN1u, CPt; Ri; until E1u */
    V92D_PH4,       /* Ri-bar, TRN2d, SUVd / CPd */
    V92D_ED,        /* Ed, B1d; waiting for both directions */
    V92D_DATA,
    V92D_RENEG,     /* rate renegotiation: Rd / waiting for Ru-bar; then V92D_PH4 */
    V92D_RETRAIN,   /* over: a retrain is due (the caller's Tone A, or our own wish) */
    V92D_FAILED
} v92d_state;

enum {
    V92D_OK = 0,
    V92D_ERR_NO_RU,     /* the caller's Ru never came */
    V92D_ERR_JA,        /* no channel, or no valid DIL descriptor */
    V92D_ERR_UINFO,     /* UINFO outside what Sd can carry */
    V92D_ERR_NO_SU,     /* no Su after Jd (9.5.1.2.2) */
    V92D_ERR_CPT,       /* no usable CPt */
    V92D_ERR_DESIGN,    /* the channel does not carry the lowest upstream rate */
    V92D_ERR_CPU,       /* no usable CPu */
    V92D_ERR_B1,        /* B1u not received in time, or wrong */
    V92D_ERR_CLEARDOWN, /* the caller asked to clear down */
    V92D_ERR_HANGUP,    /* we cleared down (v92d_cleardown) */
    V92D_ERR_CARRIER    /* upstream signal lost in data mode */
};

#define V92D_MAX_ECHO_DELAY 1919 /* the longest echo delay that can be given */
#define V92D_FBUF  2752     /* longest received sequence (Ja) plus a next frame sync */
#define V92D_B1U_LEAD 2     /* data frames of Ki = 0 passed over before B1u at most (reading 57) */
#define V92D_UP_EST 144     /* B1u symbols the caller's gain is measured on (within the
                               255 samples the training receiver keeps) */
#define V92D_SBUF  (324 + 17 * (V92_DSN_LZ2 + V92_DSN_LP1 + V92_MAX_LC) + 72)  /* our CPd, bits */

typedef struct {
    v92d_cfg   cfg;
    v92d_state state;
    int        error;
    uint32_t   now, t;              /* sample clock; samples in this state (modem/clock.h) */
    uint32_t   t_trn1d;
    /* transmit */
    v90tx_t    tx;
    v90_jd_t   jd;
    v92_jp_t   jp;
    uint8_t    xjp[9], xsuvd[2][16], xcpd[2][(V92D_SBUF + 7) / 8];
    int        ncpd;                /* bits of our CPd */
    uint8_t    sbits[V92D_SBUF];    /* scratch for building sequences */
    /* echo */
    ec_t       ec;
    int        ec_mu;
    int32_t    et_s[2], et_g[2];    /* slow echo tail: low-pass states, coefficients (Q16) */
    int64_t    et_pw;               /* ... and the smoothed power of its two inputs */
    ecdly_t    ecd;                 /* what we sent; ecd.delay is the echo delay in use, given or measured */
    int32_t    base_q4;             /* data mode: the baseline under the caller's symbols, Q4 */
    int32_t    up_scale;            /* data mode: what the received samples are multiplied by
                                       before the decoder, Q14: 1 / the caller's gain over
                                       the design, measured on B1u; 0 = not yet (1) */
    int64_t    nz_acc, nq_acc, ne_acc; int32_t nz_n;    /* TRN1d: residual, quantiser, echo power */
    uint32_t   noise, q_quiet, echo_rms;
    uint32_t   q_trn;               /* smoothed quantiser variance while the caller trains */
    /* receive */
    v92_p6_t   p6;
    int32_t    rev_at;              /* sample of the last reversal, 0 = none */
    int        revs;
    v92_trn_t  trn;
    uint32_t   trn_at;              /* sample at which the training receiver starts, 0 = not armed
                                       (one that falls on 0 at the clock's wrap, every 2^32 samples,
                                       is not armed: that start-up fails on its limit) */
    int        trn_on;              /* 1: v92_trn_tick, 2: data mode (v92_trn_store) */
    int32_t    peak1;               /* v92_trn_peak() of the first TRN1u */
    v92_sgn_t  sgn;                 /* two-level sequences */
    v92_ml_t   ml;                  /* TRN2u modulation */
    int        four;                /* the caller is sending 4-level symbols */
    int32_t    m_trn2u;             /* number of the first TRN2u symbol */
    uint8_t    fbuf[V92D_FBUF];
    int        fn, zeros; seqhunt_t sh;    /* bits collected (0 = hunting), runs */
    int        want;                /* which sequences: 0 Ja, 1 CPt, 2 SUVu / CPu */
    int        after_seq;           /* bits since a sequence was completed, -1 = none yet */
    v92_ja_t   ja;
    v92_cpu_t  cpx;                 /* CPt, later CPu */
    int        got_ja, got_cpt, got_e1u, got_suvu, got_cpu, got_ack, got_e2u;
    int        ri_sent, cpd_sent; uint32_t t_cpd;
    uint32_t   t_clear;             /* sample our cleardown began */
    /* design */
    v92_dsn_t  dsn;
    int        dsn_state;           /* 0 idle, 1 running, 2 done */
    /* data mode */
    v92_cpd_t  cpd;                 /* what we told the caller to use */
    v92_dec_t  dec;
    int32_t    data_k;              /* next sample (trn.k clock) for the decoder; valid when rx_on */
    int32_t    m_b1u;               /* symbol number of the first B1u symbol */
    int        rx_on;
    int32_t    b1_left;             /* bits of B1u still to come */
    int32_t    b1_bad;
    int        b1_sym;              /* samples of the first data frame taken, -1: it has been judged */
    int        b1_zero, b1_leads;   /* ... those with Ki = 0; data frames passed over before B1u */
    int        tx_ready, rx_ready;
    int        down_rate, up_rate;  /* bit/s */
    int        in_reneg, clearing;
    int        reneg_ours;          /* 0 the caller's, 1 ours, 2 ours to drop the upstream rate
                                       (its design one step, 4000 bit/s, below up_rate) */
    int        renegs;              /* renegotiations completed */
    int        retrain_ours;
    int        up_floor;            /* the retrain is ours because the upstream did not hold
                                       even the lowest PCM rate in data mode */
    tonedet_t  tone_a;              /* Tone A (2400 Hz) watch */
    /* line quality in data mode */
    uint32_t   q_limit; int32_t q_bad; uint32_t bad_seen;
    int        rate_drops;          /* our rate drops so far (a count) */
    /* loss of the upstream signal in data mode, 10 ms blocks */
    uint32_t   sig_ms;              /* mean square of the samples that carried B1u */
    int64_t    cd_acc;              /* sum of squares after the canceller (B1u: its samples) */
    uint32_t   cd_line;             /* ... of the line itself, each square >> CD_LINE_SHIFT */
    int16_t    cd_n;                /* samples in cd_acc */
    uint8_t    cd_quiet;            /* blocks in a row without the caller's signal */
    uint8_t    cd_idle;             /* samples in a row (to 255) in which the line brought the
                                       idle code: no far end, whatever the canceller gives out */
} v92d_t;

/* Returns 0, or -1 if the configuration cannot work (state is V92D_FAILED):
 * a UINFO that Sd cannot carry, or an echo delay beyond V92D_MAX_ECHO_DELAY. */
int     v92d_init(v92d_t *p, const v92d_cfg *cfg);
uint8_t v92d_tick(v92d_t *p, int16_t rx);
/* From data mode: start a rate renegotiation (9.8.1.1). The state leaves
 * V92D_DATA and comes back to it when both directions carry data again.
 * Returns 0, or -1 when not in data mode. */
int     v92d_renegotiate(v92d_t *p);
/* From data mode: clear down (9.11): a rate renegotiation (9.8.1.1) whose
 * CPd, sent on the caller's SUVu, carries drn = 0; over once the caller has
 * acknowledged it (where 9.6.1.1.4 would send Ed), else after 3.5 s; then
 * silence. Ends in V92D_FAILED with V92D_ERR_HANGUP whatever happens. Returns
 * 0, or -1 when not in data mode. */
int     v92d_cleardown(v92d_t *p);
/* From data mode or a rate renegotiation: retrain (9.7.1.1). The state goes
 * to V92D_RETRAIN. Returns 0, or -1 when the call is not that far. */
int     v92d_retrain(v92d_t *p);

/* The caller's gain in data mode (v92_p34d.c, "the caller's gain"): from
 * the V92D_UP_EST samples of t from k0 on (the first symbols of B1u, data
 * frame interval 0 at k0), sliced on m, with eu2 the mean square of u(n)
 * the design expects (linear^2), what the samples are to be multiplied by
 * before the decoder, Q14: 16384 when they are taken as they are. Used by
 * v92d_tick; open for the tests. */
int32_t v92d_up_scale(const v92_map_t *m, const v92_trn_t *t, int32_t k0, uint32_t eu2);

#ifdef P34D_TRACE
/* Debug hook for a replay tool (../live/replay.c), called once per sample:
 * the received sample and what the echo canceller left of it. Not compiled
 * into the product. */
void    v92d_trace(const v92d_t *p, int16_t rxs, int16_t x);
/* ... and once per decided training symbol (p->trn.dec_*), with the bit(s)
 * it gave after the descrambler */
void    v92d_trace_sym(const v92d_t *p, int bits);
/* ... and once per sample handed to the data mode decoder */
void    v92d_trace_data(const v92d_t *p, int16_t sample);
/* ... and what goes into the echo canceller's history for the octet just
 * sent: y, or what a recording says was sent (the data of a recorded call
 * is not the replay's) */
int16_t v92d_trace_tx(const v92d_t *p, int16_t y);
#endif

#endif
