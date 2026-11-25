/* K56flex as its server, from our flagged INFO1a to data mode
 * (docs/history/k56-facts.md, "Training down", "Upstream and J24", "Phase
 * 4", "Data"). It plays the part for K56flex that x2d plays for x2: one
 * received linear sample in, one mu-law octet out, at 8 kHz.
 *
 *   K56D_TRAIN1: our silence 1, R1, TRN1A, TRN1B, TRN1C (k56tx); the client
 *   is silent and our echo canceller learns (measuring the echo delay when
 *   it is not given) -> our Js: from here we listen ->
 *   K56D_WAIT_J: our silence 2; the client's S, S-bar, PP, TRN (4 points,
 *   GPC: it is V.34's call modem) and its J24 (the V.34 J and an octet: the
 *   robbed symbols, the pad), two equal J words 24 bits apart ->
 *   K56D_TRAIN2: R2, TRN2A, MARK, TRN2B; the client's J', TRN (16 points:
 *   our Js asked for them), its MP. The MP decides the rates; ours goes into
 *   k56tx, and TRN2B ends on the grid of 24 ->
 *   K56D_MP: our MP, MP', E; the client's MP', E, B1 ->
 *   K56D_DATA: k56enc frames down (B1 first), V.34 up (16 states, linear,
 *   minimum shaping: what our MP asked; GPC) through v34up, as in
 *   x2d and p34d. A client whose B1 fails under that is tried under x2's
 *   configuration (64 states, non-linear, expanded shaping; up_hyp 1) on
 *   its points kept from E on (ring), and goes on under it if B1 passes
 *   there (k56d.c, "a client that ignores our MP"). The client's S here,
 *   or from its B1 on (in K56D_MP: ours not yet out; then our R2 after our
 *   first data frame), is its renegotiation ->
 *   K56D_RENEG ("Rate renegotiation"): our data stops at the frame's end,
 *   then R2, TRN2A 2048, MARK, TRN2B until its MP and the grid, MP and MP'
 *   at 1 bit a symbol, E 24 (k56tx_reneg); its S-bar, TRN, MP (4 points,
 *   V.34 11.6), MP', E; data again both ways (B1 first) at the rates of its
 *   new MP -> K56D_DATA. An MP with rate 0 both ways is V.34's cleardown
 *   (K56D_ERR_CLEARDOWN). We never start one (decision 13). The client's
 *   tone B (1200 Hz, more than 50 ms) here or in its renegotiation is its
 *   retrain ("Retrain") ->
 *   K56D_RETRAIN: silent; the owner goes on with V.34's Phase 2 of a
 *   retrain and a fresh k56d (decision 14). We never start one either.
 *
 * Our MP: type 0, rate_ac = r, rate_ca = the upstream rate, mask 0x3fff
 * (D-Modem's server's), asymmetric; for the client's transmitter 16 states,
 * linear, minimum shaping (decision 9). r = min(the client's rate_ac,
 * max_down's code, 13), max_down's code being (max_down - 30000) / 2000, at
 * least 1, none when max_down is 0. The upstream: the highest rate of the
 * client's mask that our V.34 receiver serves at 3200 baud, at most its
 * rate_ca and max_up_rate (decision 10, "Phase 4" Rates).
 *
 * From our flagged INFO1a there is no way back to V.34: every failure ends
 * here (K56D_ENDED, error saying why), each wait bounded well under 10 s.
 *
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_K56D_H
#define V90D_MODEM_K56D_H
#include <stdint.h>
#include "seqhunt.h"
#include "ec.h"
#include "ecdly.h"
#include "tonedet.h"
#include "v34up.h"
#include "v34mp.h"
#include "k56tx.h"

#define K56D_MAX_ECHO_DELAY 1919    /* the longest echo delay that can be given (as p34d) */
#define K56D_FBUF 192               /* an MP (88 bits) and a next frame sync */
#define K56D_RING 192               /* B1 (one data frame) and the Viterbi's delay, symbols */
#define K56D_DRAIN 2                /* catchup: its S came while catching up */

/* time limits, ms (each under 10 s) */
#define K56D_T_S    3000    /* the end of our Js to the client's S */
#define K56D_T_J    3200    /* its S to its J24: S, S-bar, PP and the longest TRN of
                               "Upstream and J24" (6430 T), 2.14 s, and 1 s (Ruling F4) */
#define K56D_T_MP   4000    /* the start of our TRN2B to its MP (Ruling F4) */
#define K56D_T_E    2000    /* the start of our MP' to its E */
#define K56D_T_B1   1000    /* its E to B1 received, our B1 sent */
#define K56D_T_RENEG 6000   /* its renegotiation's S to data again both ways (as x2d's) */

typedef struct {
    int rtd;                /* round-trip delay from Phase 2, samples */
    int echo_delay;         /* as p34d_cfg: samples until our echo starts to come back,
                               0..K56D_MAX_ECHO_DELAY; -1 = not known: measured on our
                               TRN1A while the client is silent */
    int hi_carrier;         /* the client's carrier (INFO1a bit 25, our choice): 1 high */
    int max_up_rate;        /* upstream ceiling in bit/s; 0 = what the client's MP allows */
    int max_down_rate;      /* downstream ceiling in bit/s; 0 = 56000 */
    int  (*getbit)(void *); /* data to send */
    void (*putbit)(void *, int);    /* data received */
    void *user;
} k56d_cfg;

typedef enum {
    K56D_TRAIN1,    /* silence 1 .. TRN1C, then Js; the echo canceller learns */
    K56D_WAIT_J,    /* silence 2: the client's S .. J24 */
    K56D_TRAIN2,    /* R2 .. TRN2B: its J', TRN, MP */
    K56D_MP,        /* our MP, MP', E until its E (and B1 both ways) */
    K56D_DATA,      /* data both ways */
    K56D_RENEG,     /* the client's renegotiation (k56 plan, Task 11) */
    K56D_RETRAIN,   /* its tone B in data mode or its renegotiation: the client's
                       retrain; silent, the owner's Phase 2 next */
    K56D_ENDED      /* silent; error says why */
} k56d_state;

enum {
    K56D_OK = 0,
    K56D_ERR_CFG,       /* a configuration that cannot work */
    K56D_ERR_NO_S,      /* the client's S did not come after our Js */
    K56D_ERR_NO_J,      /* nor its J24 after its S */
    K56D_ERR_J,         /* its J24's pad code is 3 (not defined) */
    K56D_ERR_NO_MP,     /* its MP did not come */
    K56D_ERR_RATE,      /* its MP: rate_ac 0, mask 0, or no upstream rate we receive */
    K56D_ERR_NO_E,      /* its E did not come after our MP' */
    K56D_ERR_B1,        /* B1 not received in time, or wrong */
    K56D_ERR_CARRIER,   /* the upstream lost in data mode */
    K56D_ERR_RENEG,     /* a renegotiation not back in data mode in time */
    K56D_ERR_CLEARDOWN, /* a renegotiation MP with rate 0 */
    K56D_ERR_HANGUP     /* we cleared down (k56d_cleardown) */
};

typedef struct {
    k56d_cfg   cfg;
    k56d_state state;
    int        error;
    uint32_t   now, t;              /* sample clock; samples in this state (modem/clock.h) */
    uint32_t   t_mark;              /* a deadline's reference: when the current wait began (CLK_NONE: none) */
    int        car_num, car_den;
    /* transmit */
    k56tx_t    tx;
    v34_mp_t   mp;                  /* ours */
    /* receive */
    ec_t       ec;
    int        ec_mu;
    int32_t    ec_n;                /* samples of our TRN1A.. TRN1C seen by the canceller */
    ecdly_t    ecd;                 /* what we sent; ecd.delay the echo delay in use */
    sdet_t     sdet;
    sres_t     sres;                /* its S out of data mode: a renegotiation */
    tonedet_t  tone_b;              /* its tone B in data mode or a renegotiation: a retrain */
    v34up_t    up;                  /* the client's V.34: its Phase 3 and 4, its data */
    int        rxmode;
    /* J24, J', MP, E */
    uint32_t   jsr; int32_t jbits, j_at; int j_seen;
    int        got_j, got_jp;
    uint8_t    j_octet;
    uint8_t    fbuf[K56D_FBUF]; int fn; seqhunt_t sh;
    v34_mp_t   far_mp, cand; int cand_ok;
    int        got_mp, got_e;
    int        pad;                 /* from the J24: 0..2 */
    uint8_t    rbs;                 /* ... robbed symbols, bits 0-5 */
    int        rate;                /* the downstream rate code r, 1..13 */
    /* data mode, upstream: B1's retry */
    uint8_t    up_hyp;              /* the upstream's options: 0 ours (what our MP asked),
                                       1 x2's (64 states, non-linear, expanded shaping) */
    uint8_t    up_retried;          /* B1 failed under the first: the other one tried */
    uint8_t    catchup;             /* decoding the stored points under the other;
                                     * K56D_DRAIN: up to its S (a renegotiation) */
    uint8_t    catch_odd;           /* catching up: 1 or 2 points this symbol period */
    uint8_t    ring_full;           /* points lost: no retry */
    uint16_t   ring_w, ring_r, ring_fill;   /* the ring's next write, next read, unread */
    int16_t    ring[K56D_RING][2];  /* its points from E on (clipped, before v34_nl_decode),
                                       while B1 is not decided or being caught up */
    int32_t    eq_cr[QRX_NTAP], eq_ci[QRX_NTAP];  /* the equaliser at its E: back at a retry */
    int        tx_ready;
    int        down_rate, up_rate;  /* bit/s */
} k56d_t;

/* Returns 0, or -1 (state K56D_ENDED, K56D_ERR_CFG) */
int      k56d_init(k56d_t *d, const k56d_cfg *cfg);
uint8_t  k56d_tick(k56d_t *d, int16_t rx);
/* bit/s in data mode (and in a renegotiation), else 0: down is ours */
int      k56d_down_rate(const k56d_t *d);
int      k56d_up_rate(const k56d_t *d);
/* From data mode or a renegotiation: no K56flex cleardown is known (design
 * decision 15); we go silent (octet 0xff) and the call ends (K56D_ENDED,
 * K56D_ERR_HANGUP). Returns 0, or -1 in any other state. */
int      k56d_cleardown(k56d_t *d);
/* the client's J24: its robbed symbols (bits 0-5, counted from TRN1B's
 * first symbol) and its pad (0 none, 1 3 dB, 2 6 dB); 0 before the J */
uint8_t  k56d_rbs(const k56d_t *d);
int      k56d_pad(const k56d_t *d);

#endif
