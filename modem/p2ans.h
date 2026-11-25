/* V.34 Phase 2 (probing / ranging), ANSWER modem: V.34 11.2.1.2, recovery
 * 11.2.2.2, and the Phase 2 part of a retrain (11.5.2).
 *
 *   INFO0a, Tone A ... (INFO0c and Tone B from the caller) ... Tone A phase
 *   reversal ... the caller's reversal gives the round-trip delay ... our
 *   reversal 40 ms after it, 10 ms more of Tone A, L1, L2 until Tone B is
 *   heard under it ... Tone A for 50 ms, reversal, 10 ms, silence ... the
 *   caller's reversal, its L1 and L2 (analysed) ... Tone A, INFO1c from the
 *   caller, INFO1a.
 *
 * INFO1a is where the answer modem decides: the symbol rate (one for both
 * directions here), the carrier the caller shall use, and the projected
 * call -> answer rate. What the caller's receiver asks of our transmitter
 * (carrier, pre-emphasis filter, power reduction for the chosen symbol rate)
 * is taken from INFO1c and reported in tx_*.
 *
 * x2 (cfg.x2; docs/history/x2-facts.md "Phase 2 marker"): INFO0a says
 * max_sr_diff = 4 and cme, which a U.S. Robotics x2 client reads as an x2
 * server; from the start of our L1 (in the first Phase 2 of the call, not in
 * a retrain's) a receiver hunts for the client's marker,
 * one INFO frame with 7 bits (p2ans_x2_info_frame), sent in place of its L1,
 * L2 and INFO1c. One that checks out, carries 6 in bits 1..3 and begins by
 * 960 ms after the start of our L1 (800 ms after its end) ends Phase 2 in P2N_X2:
 * silent, x2_marker set. Anything else, and the V.34 path goes on as it
 * would without x2: L2 is sent as ever while the receiver listens.
 *
 * Sample-driven: one received sample in, one transmit sample out, 8 kHz.
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_P2ANS_H
#define V90D_MODEM_P2ANS_H
#include <stdint.h>
#include "dpsk.h"
#include "probe.h"
#include "info.h"

#define P2N_SR_2400 (1 << 0)    /* symbol rate indices of INFO1: 0 = 2400 ... 5 = 3429 */
#define P2N_SR_3000 (1 << 3)
#define P2N_SR_3200 (1 << 4)

typedef struct {
    int level;          /* nominal transmit power: RMS in 16-bit sample units */
    int io_delay;       /* round-trip delay of our own sample path to the line terminals
                           and back, samples (a driver's buffering) */
    int sr_mask;        /* symbol rates INFO1a may select; 0 = 2400, 3000 and 3200 */
    int max_rate;       /* ceiling for the projected call -> answer rate, bit/s; 0 = none */
    int x2;             /* offer x2 (see above): INFO0a bits, and the marker receiver */
    int k56;            /* K56flex was agreed in V.8bis (see above): INFO1a's flag, the nudge */
} p2ans_cfg;

typedef enum {
    P2N_INFO0,          /* INFO0a, then Tone A; waiting for INFO0c and Tone B */
    P2N_WAIT_BREV1,     /* our reversal sent; waiting for the Tone B reversal */
    P2N_REPLY,          /* timed Tone A reversal, 10 ms more */
    P2N_L1, P2N_L2,     /* probing signals; L2 until Tone B is heard */
    P2N_TONE50,         /* Tone A for 50 ms */
    P2N_REV_TAIL,       /* reversal, 10 ms more, then silence */
    P2N_WAIT_BREV2,     /* silent; waiting for the Tone B reversal */
    P2N_PROBE_RX,       /* receiving L1 / L2 */
    P2N_WAIT_INFO1C,    /* Tone A; waiting for INFO1c */
    P2N_INFO1A,         /* sending INFO1a */
    P2N_RT_SILENCE,     /* 11.5.2.1: 70 ms of silence before Tone A (a retrain we begin) */
    P2N_DONE,
    P2N_FAILED,
    P2N_X2,             /* the x2 marker came (x2_marker): silent; the owner goes on as x2 */
    P2N_K56             /* our INFO1a went out flagged (k56): silent; the owner goes on as
                           K56flex. The point of no return. */
} p2ans_state;

enum { P2N_OK = 0,
       P2N_ERR_INFO0,       /* no INFO0c / Tone B */
       P2N_ERR_REV,         /* the Tone B phase reversals never came */
       P2N_ERR_INFO1C,      /* no INFO1c, retrains used up */
       P2N_ERR_NO_RATE,     /* INFO1c leaves no symbol rate we implement */
       P2N_ERR_TIME };      /* the exchange ran past P2N_EXCHANGE_MS */

/* A caller that repeats INFO0c keeps taking WAIT_BREV1 back to INFO0, which
 * starts that state's own limit again: the exchange as a whole has this
 * long from p2ans_init, p2ans_init_retrain or a counted restart (restarts,
 * retrains). Far above a normal Phase 2 (3 to 5 s). */
#define P2N_EXCHANGE_MS 20000

#define P2N_MAX_RETRAINS 2      /* retrains p2ans begins by itself before giving up */

typedef struct {
    p2ans_cfg   cfg;
    p2ans_state state;
    int         error;
    int32_t     now, t;             /* sample clock; samples in the current state */
    int32_t     t_x;                /* samples since the exchange began (P2N_EXCHANGE_MS) */
    dpsktx_t    tx; dpskrx_t rx;
    probetx_t   ptx; proberx_t prx;
    int         probing;            /* 0 none, 1 L1, 2 L2 */
    info0_t     info0c; int got0c, resend, n_info0a;
    info1d_t    info1c;             /* INFO1c has the layout of V.90's INFO1d */
    info1a_t    info1a;             /* as sent */
    int32_t     act_at, a_rev_line, probe_from, tone_since, tone_b_at;
    int32_t     rtd;                /* round-trip delay estimate RTDEa, samples */
    int         tone_blocks;
    int         retrains;           /* begun by p2ans itself (11.2.2.2.4) */
    int         restarts;           /* returns to 11.2.1.2.3 (11.2.2.2.2, 11.2.2.2.3) */
    int         snr[PROBE_NBIN + 1];        /* per 150 Hz bin, 0.1 dB */
    int         rx_level[PROBE_NBIN + 1];
    /* ---- the outcome, valid in P2N_DONE ---- */
    int         sym_idx;            /* symbol rate index for both directions: 0, 3 or 4 */
    int         sym_rate;           /* 2400, 3000 or 3200 */
    int         rx_hi_carrier;      /* call -> answer carrier (our choice, INFO1a bit 25) */
    int         rx_snr_db10;        /* projected receiver SNR at that symbol rate, 0.1 dB */
    int         tx_hi_carrier;      /* answer -> call: what INFO1c asked for */
    int         tx_preemph;         /* pre-emphasis filter index 0..10 */
    int         tx_pwr_red;         /* power reduction, dB */
    int         md_len;             /* the caller's MD, units of 35 ms */
    int         x2_marker;          /* the marker's 7 bits (0x00..0x7f) in P2N_X2, else -1 */
    int32_t     x2_until;           /* a marker frame may begin until this sample; -1 = off */
    int         x2_hunt;            /* cfg.x2 in the first Phase 2 of the call: not in a
                                       retrain's (p2ans_init_retrain, or one begun here) */
    int         k56;                /* 1 when INFO1a went out flagged (0x001) */
} p2ans_t;

void    p2ans_init(p2ans_t *p, const p2ans_cfg *cfg);
/* Phase 2 of a retrain, after the 70 ms of silence (11.5.2.1, 11.5.2.2):
 * Tone A at once, no INFO0 exchange unless the caller sends INFO0c. info0c
 * is the caller's INFO0c of the first Phase 2 (it is not sent again). */
void    p2ans_init_retrain(p2ans_t *p, const p2ans_cfg *cfg, const info0_t *info0c);
int16_t p2ans_tick(p2ans_t *p, int16_t rx);
/* An x2 client's marker frame, as the line carries it (600 bit/s DPSK, one
 * bit per element, 1 = reversal): 2 lead-in ones, fill 1111, sync 01110010,
 * the 7 bits of marker7 LSB first, their INFO CRC LSB first, fill 1111.
 * Returns the count, P2N_X2_FRAME_BITS. For the tests and the simulated
 * client. */
#define P2N_X2_FRAME_BITS (2 + 4 + 8 + 7 + 16 + 4)
int     p2ans_x2_info_frame(uint8_t marker7, uint8_t *bits);

/* K56flex: a measured frequency offset (0.02 Hz units, -512 = none) as
 * INFO1a carries it in an agreed call that stays V.34: one ending in binary
 * 01 (which the client reads as K56flex) moved one step toward zero. */
int     p2ans_k56_nudge(int off);

/* V.34 Tables 1 and 2 for a symbol rate index: the rate, and the carrier as
 * the ratio num/den of it. Returns 0 for an index outside 0..5. */
int     v34_symbol_rate(int idx);
void    v34_carrier(int idx, int hi, int *num, int *den);

#endif
