/* V.34 Phase 2 (probing / ranging), CALL modem: V.34 11.2.1.1, recovery
 * 11.2.2.1, and the Phase 2 part of a retrain (11.5.1).
 *
 *   INFO0c, Tone B ... (INFO0a and Tone A from the answerer) ... the Tone A
 *   reversal ... our Tone B reversal 40 ms after it at the line, 10 ms more,
 *   silence ... the second Tone A reversal gives the round-trip delay ...
 *   the answerer's L1 and L2 (analysed) ... Tone B ... Tone A, its reversal
 *   ... our reversal 40 ms after it, 10 ms more, L1, L2 until Tone A is
 *   heard under it ... INFO1c, silence ... INFO1a.
 *
 * INFO1c is where the call modem's receiver asks: for each symbol rate, the
 * carrier the answerer shall use, its pre-emphasis filter and the projected
 * answer -> call rate; once, the power reduction it shall make. All from the
 * analysis of the answerer's L2 and from what INFO0a says the answerer's
 * transmitter can do (p2call_decide). INFO1a, the answerer's choice, gives
 * our transmitter its symbol rate, carrier, filter and power reduction.
 *
 * V.90 (p2call_init_v90): the analogue modem's Phase 2 of V.90 9.2.2, which
 * is the V.34 answer modem's procedure with V.90's INFO sequences: INFO0a and
 * Tone A ... the digital modem's INFO0d and Tone B ... our Tone A reversal
 * once Tone B is there and Tone A has run 50 ms ... the Tone B reversal (the
 * round trip, RTDEa) ... ours 40 ms after it at the line, 10 ms more, L1, L2
 * until Tone B is heard under it (`probing` says when, for an echo
 * canceller that learns meanwhile: the digital modem is silent) ... Tone A
 * 50 ms, its reversal, 10 ms, silence ... the Tone B reversal, the digital
 * modem's L1 and L2 (analysed as for INFO1c) ... Tone A ... INFO1d ...
 * INFO1a of Table 10 (V.90: UINFO, the MD we will send, the upstream symbol
 * rate: of 3000 and 3200, the one INFO1d projects most for) ... silence.
 * Recovery: 9.2.2.2.1 to .3; 9.2.2.2.4 (no INFO1d) ends in P2L_FAILED
 * (P2L_ERR_INFO1A) rather than a retrain.
 *
 * Sample-driven: one received sample in, one transmit sample out, 8 kHz.
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_P2CALL_H
#define V90D_MODEM_P2CALL_H
#include <stdint.h>
#include "dpsk.h"
#include "probe.h"
#include "info.h"
#include "p2ans.h"          /* P2N_SR_*, v34_symbol_rate, v34_carrier */

typedef struct {
    int level;          /* nominal transmit power: RMS in 16-bit sample units */
    int io_delay;       /* round-trip delay of our own sample path to the line terminals
                           and back, samples (a driver's buffering) */
    int sr_mask;        /* symbol rates INFO1c may offer (P2N_SR_*); 0 = 2400, 3000 and 3200 */
    int max_rate;       /* ceiling for the projected answer -> call rate, bit/s; 0 = none */
    /* V.90 (p2call_init_v90) only */
    int uinfo;          /* INFO1a bits 25:31 */
    int md_len;         /* INFO1a bits 18:24: the MD we send in Phase 3, units of 35 ms
                           (with PCM upstream: units of 276 symbols, Table 18/V.92) */
    int v92;            /* V.92 (9.3/V.92): INFO0a bit 26; INFO1a of Table 18 (PCM upstream)
                           when INFO0d bit 27 and INFO1d bit 70 allow it */
} p2call_cfg;

typedef enum {
    P2L_INFO0,          /* INFO0c, then Tone B; waiting for INFO0a and Tone A */
    P2L_WAIT_REV1,      /* Tone B; waiting for the Tone A reversal */
    P2L_REPLY1,         /* timed Tone B reversal, 10 ms more, then silence */
    P2L_WAIT_REV2,      /* silent; waiting for the second Tone A reversal */
    P2L_RECOVER,        /* 11.2.2.1.3: silent until Tone A is heard again */
    P2L_PROBE_RX,       /* receiving L1 / L2 */
    P2L_WAIT_REV3,      /* Tone B; waiting for Tone A and its reversal */
    P2L_REPLY3,         /* timed Tone B reversal, 10 ms more */
    P2L_L1, P2L_L2,     /* probing signals; L2 until Tone A is heard */
    P2L_INFO1C,         /* sending INFO1c */
    P2L_WAIT_INFO1A,    /* silent; waiting for INFO1a */
    P2L_RT_SILENCE,     /* 11.5.1: 70 ms of silence before Tone B (a retrain begun or answered) */
    P2L_DONE,
    P2L_FAILED,
    /* V.90 analogue modem (9.2.2) */
    P2L_A_INFO0,        /* INFO0a, Tone A; waiting for INFO0d, Tone B, then our reversal */
    P2L_A_WAIT_REV1,    /* waiting for the Tone B reversal */
    P2L_A_REPLY,        /* timed Tone A reversal, 10 ms more */
    P2L_A_L1, P2L_A_L2, /* probing signals; L2 until Tone B is heard */
    P2L_A_TONE50,       /* Tone A 50 ms, its reversal, 10 ms more */
    P2L_A_WAIT_REV2,    /* silent; waiting for the Tone B reversal */
    P2L_A_PROBE_RX,     /* receiving L1 / L2 */
    P2L_A_WAIT_INFO1D,  /* Tone A; waiting for INFO1d */
    P2L_A_INFO1A        /* sending INFO1a */
} p2call_state;

enum { P2L_OK = 0,
       P2L_ERR_INFO0,       /* no INFO0a / Tone A */
       P2L_ERR_REV,         /* the Tone A phase reversals never came */
       P2L_ERR_INFO1A,      /* no INFO1a, retrains used up */
       P2L_ERR_NO_RATE };   /* INFO1a asks for what we do not do (or INFO1c had nothing to offer) */

#define P2L_MAX_RETRAINS 2      /* retrains p2call begins or answers by itself before giving up */

typedef struct {
    p2call_cfg   cfg;
    p2call_state state;
    int          error;
    int32_t      now, t;            /* sample clock; samples in the current state */
    dpsktx_t     tx; dpskrx_t rx;
    probetx_t    ptx; proberx_t prx;
    int          probing;           /* 0 none, 1 L1, 2 L2 */
    info0_t      info0a; int got0a, resend, n_info0c;
    info1c_t     info1c;            /* as sent */
    info1a_t     info1a;            /* as received */
    int32_t      act_at, b_rev_line, rev2_at, probe_from, l2_at, info1c_end;
    int32_t      rtd;               /* round-trip delay estimate RTDEc, samples */
    int          tone_blocks;
    int          retrains;          /* begun or answered by p2call itself (11.2.2.1.5, .6) */
    int          restarts;          /* returns to 11.2.1.1.3 (11.2.2.1.3) */
    int          rev3_timeouts;     /* 11.2.2.1.4: reversal sent without the answerer's */
    /* ---- the probe analysis (p2call_decide works on these) ---- */
    int          snr[PROBE_NBIN + 1];       /* per tone: repeating against non-repeating part, 0.1 dB */
    int          rx_level[PROBE_NBIN + 1];  /* per tone: RMS in sample units */
    int          sdr_db10;          /* the whole received L2 against the repeating part of the
                                       four empty bins, spread over 4 kHz: distortion, 0.1 dB */
    int          band_snr[6];       /* projected SNR per symbol rate index, as INFO1c asks, 0.1 dB */
    /* ---- the outcome, valid in P2L_DONE ---- */
    int          sym_idx;           /* symbol rate index for both directions: 0, 3 or 4 */
    int          sym_rate;          /* 2400, 3000 or 3200 */
    int          tx_hi_carrier;     /* call -> answer: INFO1a bit 25 */
    int          tx_preemph;        /* INFO1a bits 26:29, 0..10 */
    int          tx_pwr_red;        /* INFO1a bits 12:14, dB */
    int          rx_hi_carrier;     /* answer -> call: what our INFO1c asked at sym_idx */
    int          md_len;            /* the answerer's MD, units of 35 ms */
    /* sample times of events as this end saw them (tests) */
    int32_t      t_rev1, t_rev2, t_rev3, t_tone_a2, t_info1a;
    /* ---- V.90 (p2call_init_v90) ---- */
    int          v90;
    info0_t      info0d; int got0d;
    info1d_t     info1d;            /* as received */
    info1a_t     info1a_v90;        /* as sent (Table 10, or Table 18 of V.92) */
    int          pcm_up;            /* INFO1a selected PCM upstream (Table 18/V.92) */
    int32_t      clk_ppm_q8;        /* our clock against the network's, 1/256 ppm, from the digital
                                       modem's L2 (each tone turns, from block to block, by its
                                       frequency times the offset); 0 when not measured */
    int32_t      a_rev_line, tone_since, l2_from;
} p2call_t;

void    p2call_init(p2call_t *p, const p2call_cfg *cfg);
/* V.92: our clock against the network's from the digital modem's L2 (the
 * probe's results r), 1/256 ppm, within +-1000 ppm; 0 when there is nothing
 * to go on. Exported for the tests; p2call sets clk_ppm_q8 from it. */
int32_t p2call_clock_q8(const proberx_t *r);
/* Phase 2 of a retrain, after the 70 ms of silence (11.5.1.1, 11.5.1.2):
 * Tone B at once, no INFO0 exchange unless the answerer sends INFO0a.
 * info0a is the answerer's INFO0a of the first Phase 2. */
void    p2call_init_retrain(p2call_t *p, const p2call_cfg *cfg, const info0_t *info0a);
int16_t p2call_tick(p2call_t *p, int16_t rx);
/* V.90 Phase 2, analogue modem (9.2.2): INFO0a at once. In P2L_DONE the
 * outcome is in sym_idx / sym_rate (the upstream symbol rate), tx_hi_carrier,
 * tx_preemph, tx_pwr_red (what INFO1d asks of our transmitter at that rate),
 * rtd (RTDEa), info0d, info1d and info1a_v90. */
void    p2call_init_v90(p2call_t *p, const p2call_cfg *cfg);
/* The same for a retrain (9.5.2/V.90, 9.7.2/V.92), after its 70 ms of
 * silence: Tone A at once, no INFO0 exchange (info0d: the digital modem's
 * from the first Phase 2), our reversal once Tone B has been heard and Tone
 * A has run 50 ms. With cfg.v92 and V.92 in info0d, INFO1d / INFO1a of
 * V.92 as in the first Phase 2. A digital modem that sends INFO0d in its
 * retrain all the same is answered with INFO0a, and its new INFO0d holds. */
void    p2call_init_v90_retrain(p2call_t *p, const p2call_cfg *cfg, const info0_t *info0d);

/* The INFO1c decision (Table 15) from snr[], rx_level[], sdr_db10 and the
 * answerer's INFO0a: fills info1c and band_snr[]. Run by p2call_tick when
 * the answerer's L2 has been analysed; open to tests with made-up probe
 * results. */
void    p2call_decide(p2call_t *p);

#endif
