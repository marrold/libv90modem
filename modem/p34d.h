/* V.90 Phases 3 and 4 and data mode, digital modem side (V.90 9.3.1, 9.4.1).
 *
 *   silent: S, S-bar, PP, TRN, Ja from the caller (equaliser training, DIL
 *   descriptor) -> Sd, Sd-bar, TRN1d (the caller trains; we learn our echo),
 *   Jd until S is heard, Jd', the DIL until the second S / S-bar -> Ri until
 *   CPt, Ri-bar, TRN2d, MP until CP, MP' until CP' or E, Ed, B1d, data.
 *
 * One received linear sample in, one PCM octet out, at 8 kHz. The transmit
 * side is v90/ (integer logic), the upstream receiver is v34up (qamrx + v34/).
 *
 * What we impose on the caller's transmitter: 4-point CP / E / SCR (Jd bit
 * 47 = 0), 16-state trellis code, no non-linear encoder, minimum shaping, no
 * precoding (MP type 0).
 *
 * In data mode: rate renegotiation (9.6) started by either side, and
 * cleardown (9.7) by either side. A retrain (9.5) ends this module in
 * P34_RETRAIN, silent: the owner sends the 70 ms of silence, runs Phase 2
 * again (p2d_init_retrain) and then a fresh p34d. */
#ifndef V90D_MODEM_P34D_H
#define V90D_MODEM_P34D_H
#include <stdint.h>
#include "seqhunt.h"
#include "ec.h"
#include "ecdly.h"
#include "tonedet.h"
#include "v34up.h"
#include "../v90/v90_tx.h"
#include "../v34/v34.h"

typedef int  (*p34d_getbit_fn)(void *user);
typedef void (*p34d_putbit_fn)(void *user, int bit);

typedef struct {
    int alaw;               /* law of our codec */
    int sym_rate;           /* upstream symbol rate from INFO1a: 3000 or 3200 */
    int car_num, car_den;   /* its carrier as a ratio of the symbol rate */
    int uinfo;              /* INFO1a bits 25:31 */
    int md_len;             /* INFO1a bits 18:24, units of 35 ms */
    int rtd;                /* round-trip delay from Phase 2, samples */
    int echo_delay;         /* samples from handing an octet to the line until its echo
                               starts to come back (driver buffering), 0..P34_MAX_ECHO_DELAY;
                               the canceller covers the 12 ms after that. -1 = not
                               known: measured during TRN1d (ecdly.h) */
    uint32_t down_rates;    /* Jd: downstream rates allowed, bit r = (r + 21) * 8000 / 6 bit/s; 0 = all */
    int max_up_rate;        /* upstream ceiling in bit/s; 0 = what the line gives */
    p34d_getbit_fn getbit;  /* data to send */
    p34d_putbit_fn putbit;  /* data received */
    void *user;
} p34d_cfg;

typedef enum {
    P34_WAIT_S,     /* silent, waiting for S / S-bar */
    P34_MD,         /* waiting out the caller's MD */
    P34_TRAIN,      /* PP, TRN: equaliser training; then hunting the DIL descriptor in Ja */
    P34_SD,         /* Sd, Sd-bar, TRN1d: echo canceller training */
    P34_JD,         /* Jd until S */
    P34_DIL,        /* Jd', the DIL, until the S-bar that ends Phase 3 */
    P34_RI,         /* Ri until CPt */
    P34_TRN2D,      /* Ri-bar, TRN2d */
    P34_MP,         /* MP until CP */
    P34_MP_ACK,     /* MP' until CP' or E */
    P34_ED,         /* Ed, B1d; waiting for both directions */
    P34_DATA,
    P34_RENEG_S,    /* the caller's S heard in data mode: waiting for its S-bar */
    P34_RD,         /* Rd, Rd-bar, TRN2d of a rate renegotiation; then P34_MP as above */
    P34_SIL,        /* the caller asked for silence (CPs): Ed, silence until CP */
    P34_RT,         /* Rt, Rt-bar; then P34_MP */
    P34_RETRAIN,    /* over: a retrain is due (the caller's Tone A, or our own wish) */
    P34_FAILED
} p34d_state;

enum {
    P34_OK = 0,
    P34_ERR_NO_S,       /* the caller's first S never came */
    P34_ERR_JA,         /* no valid DIL descriptor */
    P34_ERR_UINFO,      /* UINFO outside what Sd can carry */
    P34_ERR_NO_S2,      /* no S after Jd (9.3.1.5) */
    P34_ERR_DIL_END,    /* the DIL was never ended */
    P34_ERR_CPT,        /* no usable CPt */
    P34_ERR_CP,         /* no usable CP */
    P34_ERR_RATE,       /* no common upstream rate */
    P34_ERR_B1,         /* B1 not received in time, or wrong */
    P34_ERR_CLEARDOWN,  /* the caller asked to clear down */
    P34_ERR_CARRIER,    /* upstream signal lost in data mode */
    P34_ERR_HANGUP      /* we cleared down (p34d_cleardown) */
} ;

#define P34_MAX_ECHO_DELAY 1919  /* the longest echo delay that can be given */
#define P34_FBUF 2720       /* longest sequence (DIL descriptor) plus a next frame sync */

typedef struct {
    p34d_cfg   cfg;
    p34d_state state;
    int        error;
    uint32_t   now, t;              /* sample clock; samples in this state (modem/clock.h) */
    uint32_t   t_trn1d;             /* when TRN1d began */
    /* transmit */
    v90tx_t    tx;
    v90_jd_t   jd;
    v90_mp_t   mp;
    /* receive */
    ec_t       ec;
    int        ec_mu;
    ecdly_t    ecd;                 /* what we sent, for the delayed echo reference; ecd.delay
                                       is the echo delay in use, given or measured */
    sdet_t     sdet;
    v34up_t    up;                  /* the receiver (up.ref: TRN reference); up.rx_ready: data */
    int        rxmode;
    sres_t     sres;
    int        s_events;            /* S / S-bar boundaries seen after Jd */
    /* sequence framing (Ja, CPt, CP) */
    uint8_t    fbuf[P34_FBUF];
    int        fn;                  /* bits collected, 0 = hunting for a frame sync */
    seqhunt_t  sh;                  /* run of ones */
    int        want_cp;             /* 0: DIL descriptor, 1: CP sequences */
    int        got_cpt, got_cp, got_cp_ack, got_e;
    int        got_cps, got_cps_ack;    /* CP with the request for silence (bit 30) */
    int32_t    snr_db10;            /* upstream slicer SNR when CPt arrived, 0.1 dB */
    v90_dil_t  dil;
    v90_cp_t   cp;                  /* scratch / last CP */
    uint16_t   peer_up_rates;       /* CP bits 36:48 */
    /* data mode */
    int        b1_left;             /* symbols of B1 still to come, <0 = not started */
    int        tx_ready;
    int        down_rate, up_rate;  /* bit/s */
    int        in_reneg;            /* a rate renegotiation is under way */
    int        clearing;            /* ... of ours, to send MP with drn = 0 */
    tonedet_t  tone_a;              /* Tone A (2400 Hz) watch */
    int        retrain_ours;        /* P34_RETRAIN: we asked for it */
    int        rate_drops;          /* renegotiations we began to lower the rate */
} p34d_t;

/* Returns 0, or -1 if the configuration cannot work (state is P34_FAILED). */
int     p34d_init(p34d_t *p, const p34d_cfg *cfg);
uint8_t p34d_tick(p34d_t *p, int16_t rx);
/* From data mode: start a rate renegotiation (9.6.1.1). The state leaves
 * P34_DATA and comes back to it when both directions carry data again; no
 * user bit is lost downstream. Returns 0, or -1 when not in data mode. */
int     p34d_renegotiate(p34d_t *p);
/* From data mode: clear down (9.7): a rate renegotiation whose MP carries
 * drn = 0, then silence. Ends in P34_FAILED with P34_ERR_HANGUP. Returns 0,
 * or -1 when not in data mode (the caller of this just stops then). */
int     p34d_cleardown(p34d_t *p);
/* From data mode or a rate renegotiation: retrain (9.5.1.1). The state goes
 * to P34_RETRAIN. Returns 0, or -1 when the call is not that far. */
int     p34d_retrain(p34d_t *p);

#endif
