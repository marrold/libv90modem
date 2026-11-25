/* V.90 analogue modem (the calling side of a V.90 call), everything after V.8:
 *
 *   Phase 2 (p2call, V.90 9.2.2) ->
 *   Phase 3 (9.3.2): silence, S, S-bar, MD, S, S-bar, PP, TRN, Ja (the DIL
 *     descriptor of modem/v90dil) until Sd-bar is heard, silence; the
 *     downstream receiver (modem/v90arx) learns the clock and the channel on
 *     Sd and TRN1d; Jd; S until Jd', S-bar; the DIL, measured (silence
 *     upstream); S, S-bar ->
 *   Phase 4 (9.4.2): CPt until Ri-bar, SCR while TRN2d shows the noise with
 *     the CPt constellations, CP (designed from the DIL and that noise),
 *     CP' once MP is in, E once MP' or Ed is in; downstream Ri, Ri-bar,
 *     TRN2d, MP, MP', Ed, B1d ->
 *   data: the PCM downstream decoded (modem/v90adec), V.34 upstream at the
 *     rate MP allows.
 *
 * In data mode: a rate renegotiation begun by the digital modem (Rd) is
 * followed (S, S-bar, CP ... as Phase 4 from CP on); MP with drn = 0 is its
 * cleardown (9.7). v90a_cleardown() sends ours: S, S-bar, CP with drn = 0.
 * v90a_renegotiate() begins ours (9.6.2.1): S, S-bar, SCR until the digital
 * modem's TRN2d has shown the noise, then a CP designed on it (V.92, 9.8.2.1:
 * Ru, Ru-bar, TRN2u, SUVu ... a CPu designed the same way).
 *
 * Retrains (9.5/V.90, 9.7/V.92), either end, from Phase 3 on: the digital
 * modem's is its Tone B (more than 50 ms of it, after the 70 ms of silence;
 * modem/tonedet on what the echo canceller leaves); ours, v90a_retrain(), is
 * 70 ms of silence and Tone A. Either way: 70 ms of silence from us, then
 * Phase 2 again without INFO0 (p2call_init_v90_retrain), then Phases 3 and 4
 * with a fresh receiver (the DIL measured again) and, V.92, a fresh PCM
 * upstream. In data mode the receiver watches itself against the noise CP
 * was designed for (twice TRN2d's, by interval; data mode shows 0.2 to 0.97
 * of it on the test lines): 3 dB over it for 1 s is a rate renegotiation
 * down (only the noise rose) or a retrain (the line's level moved 3 dB or
 * more, or a renegotiation for the line did not help); 12 dB over for 250 ms
 * (the receiver lost) a retrain; 6 dB under it for 5 s, after the watch took
 * the rate down, a renegotiation back up. (6 dB over is already where the
 * decision feedback collapses: docs/history/PROGRESS.md, robust 2c.) A rate
 * renegotiation without Ed (9.6.2), and a Phase 3 or 4 that
 * fails once the call has carried data, are retrains too. Retrains are
 * limited as answer.c's: V90A_MAX_RETRAINS without a minute of data mode
 * between them (each minute forgets one); past that a retrain the far end
 * asks for, or one that fails, ends the call (V90A_ERR_RETRAINS), and one
 * the receiver needs is a cleardown (9.7) ending the same way.
 *
 * The echo canceller learns on MD: MD is "manufacturer defined" (V.34
 * 10.1.3.5) and the digital modem is silent through it, so it is white noise
 * at the transmit level, the only wideband signal the analogue modem sends
 * while the downstream is silent (L1 / L2 is a comb of 25 lines). INFO1a
 * asks for md_len x 35 ms of it.
 *
 * One linear sample in, one out, 8 kHz on our own clock (A/D and D/A share
 * it). Fixed-point, no allocation. */
#ifndef V90D_MODEM_V90A_H
#define V90D_MODEM_V90A_H
#include <stdint.h>
#include "seqhunt.h"
#include "p2call.h"
#include "v90arx.h"
#include "v90adec.h"
#include "v90dil.h"
#include "v34mod.h"
#include "../v34/v34.h"
#include "../v90/v90_seq.h"
#include "../v90/v90_enc.h"
#include "../v90/v90_scr.h"
#include "../v90/v90_tx.h"           /* v90_pack_bits */
#include "../v92/v92_seq.h"
#include "../v92/v92_sig.h"
#include "../v92/v92_enc.h"
#include "pcmtx.h"
#include "v92aseq.h"
#include "tonedet.h"

#define V90A_ECDL 512               /* echo canceller reference delay line */

typedef int  (*v90a_getbit_fn)(void *user);
typedef void (*v90a_putbit_fn)(void *user, int bit);

typedef struct {
    int level;                      /* nominal transmit power: RMS in sample units */
    int law;                        /* codec between us and the line: 0 none, 1 mu-law, 2 A-law
                                       (what we send is quantised to its values, as v34dx does) */
    int uinfo;                      /* INFO1a UINFO; 0 = 90 (even: a robbed bit leaves it alone) */
    int md_len;                     /* MD, 35 ms units; 0 = 22 (770 ms) */
    int echo_delay;                 /* samples from a sample handed out until its echo starts
                                       back (a driver's buffering), 0..V90A_ECDL - 1 */
    int max_tx_bps, max_rx_bps;     /* ceilings: upstream, downstream; 0 = none */
    int z_q4;                       /* CP: least distance between points / noise sigma, Q4;
                                       0 = 12 (V90A_Z_CP) */
    int table1;                     /* tests only: take Table 1's levels, not the DIL's */
    int v92;                        /* V.92: PCM upstream when the digital modem offers it (see top) */
    v90a_getbit_fn getbit;          /* data to send */
    v90a_putbit_fn putbit;          /* data received */
    void *user;
} v90a_cfg;

#define V90A_Z_CP   (12 * 16)       /* d >= 12 sigma: 2 Q(6) = 2e-9 for Gaussian noise */
#define V90A_Z_CPT  (20 * 16)       /* Phase 4's constellations: more margin still */

typedef enum {
    V90A_P2,                        /* Phase 2 */
    V90A_P3,                        /* Phase 3 */
    V90A_P4,                        /* Phase 4 */
    V90A_DATA,                      /* both directions carry data */
    V90A_RENEG,                     /* a rate renegotiation (data stopped) */
    V90A_ENDED,                     /* see error */
    V90A_RETRAIN                    /* the 70 ms of silence that begin a retrain; Phase 2 next */
} v90a_state;

#define V90A_MAX_RETRAINS 3         /* retrains without a minute of data mode between them */
#define V90A_RT_P2_MS   30000       /* a retrain's Phase 2 once the call has carried data: past
                                       this, V90A_ERR_RETRAINS */

enum {
    V90A_OK = 0,
    V90A_ERR_PHASE2,                /* p2.error says how */
    V90A_ERR_SD,                    /* no Sd-bar within 2000 ms of Ja (9.3.2.4 says 1500: v90a_priv.h) */
    V90A_ERR_TRAIN,                 /* the receiver found no channel on TRN1d */
    V90A_ERR_JD,                    /* no Jd within 4500 ms of the end of Ja (9.3.2.7) */
    V90A_ERR_JDP,                   /* no Jd' after our S */
    V90A_ERR_DIL,                   /* the DIL never came, or too little of it */
    V90A_ERR_RI,                    /* no Ri after the DIL */
    V90A_ERR_PHASE4,                /* no B1d within 15 s of INFO1a (9.4.2), or no MP / Ed */
    V90A_ERR_B1D,                   /* B1d decoded wrong */
    V90A_ERR_RATE,                  /* no downstream rate fits (we cleared down), or no upstream rate */
    V90A_ERR_CLEARDOWN,             /* the digital modem cleared down (MP with drn = 0) */
    V90A_ERR_HANGUP,                /* we cleared down (v90a_cleardown) */
    V90A_ERR_CARRIER,               /* the downstream went away (or the digital modem retrained) */
    V90A_ERR_JP,                    /* V.92: no Jp after our Su, or no Jp' after our second Su-bar */
    V90A_ERR_CPD,                   /* V.92: a CPd our transmitter cannot use */
    V90A_ERR_RETRAINS               /* a retrain failed, or one more was due past the limit */
};

typedef struct {
    v90a_cfg   cfg;
    v90a_state state;
    int        error;
    uint32_t   now, t;              /* samples since the start; in this state (modem/clock.h) */
    uint32_t   t_info1a;            /* when Phase 2 ended */
    p2call_t   p2;
    /* ---- receive ---- */
    v90arx_t   rx; int rx_on;
    int16_t    ecdl[V90A_ECDL]; int ecpos;
    int        ds;                  /* downstream sub-state (v90a_priv.h) */
    int        alaw;                /* the downstream's law (INFO0d bit 39) */
    /* Jd, Jd' */
    v90_scr_t  jscr; uint8_t jprev;
    uint8_t    jb[V90_JD_BITS + 16]; int jbn; seqhunt_t jsh;
    v90_jd_t   jd; int jd_ok;
    int32_t    jdp_m; int jdp_zeros;
    /* DIL */
    v90_dil_t  dil; v90dil_cur_t dcur;
    int32_t    dil_m0, dil_end, dil_mnext;
    uint8_t    dsu[64], dsp[64], dsf[64];   /* DIL symbols by m & 63: Ucode, sign, segment start */
    int32_t    ry[64];              /* equaliser output by m & 63 */
    uint8_t    rsg[64];             /* decided sign by m & 63 */
    uint8_t    ru[64];              /* decided Ucode by m & 63 */
    int32_t    seg_m;               /* the start of the DIL-segment being received */
    int32_t    cb_seg;              /* ... being handed to the receiver as known */
    v90dil_acc_t acc; int acc_on;
    v90dil_res_t res;
    /* the DIL's analysis, CPt's and CP's designs, a step a tick (v90a_down.c) */
    int        job, job_new;
    union { v90dil_an_t an; v90dil_cpjob_t cp; } jw;
    uint32_t   sig_jd[6];           /* noise per interval at the end of Jd (Q8) */
    /* Ri, Ri-bar, TRN2d, MP, Ed, B1d, data */
    int32_t    ri_m0, ribar_m0, m_trn2d, m_b1d, hold_until;
    int        ri_units;
    int32_t    trn2d_syms;
    v90_cp_t   cpt, cp;             /* as sent */
    v90dil_choice_t cpt_ch, cp_ch;
    v90_map_t  tmap, dmap;
    v90adec_t  dec; v90_scr_t dscr;
    uint8_t    fu[6], fp[6];
    uint8_t    mb[V90_MP_MAX_BITS + 8]; int mbn; seqhunt_t msh;
    v90_mp_t   mp; int got_mp, got_mpa, after_mpa, got_ed;
    int        b1d_frames, b1d_bad, trn2d_frames, trn2d_bad, rd_frames, rd_pol, rd_units;
    int        rx_ready;
    uint8_t    dly[2][V90_MAX_D]; uint8_t dly_n[2]; int dly_cnt, dly_pos;   /* delivery, two frames late */
    uint64_t   mse_ref;             /* the equaliser's input power at the start of data mode */
    int32_t    bad_syms;
    /* ---- transmit ---- */
    v34mod_t   mod; v34_tmod_t tmod;
    int        us; int32_t un;      /* upstream sub-state, symbols in it */
    int        n_trn, n_md;         /* TRN and MD, symbols */
    uint32_t   md_rng; int md_on; int32_t md_samples, md_total;
    uint8_t    fr[(V92_JA_MAX_BITS + 7) / 8]; int frn, frpos, fr_ack;   /* the sequence being sent, packed
                                       (Ja is the longest: V.90's DIL descriptor, CPt, CPu fit) */
    int        cp_ack_sent, cps_sent;
    int        ribar_seen, cp_ready, clearing;
    v34_params_t vpar; v34_tx_t vtx; int32_t tx_dg;
    int        tx_ready;
    int        up_rate, down_rate;
    uint16_t   up_rates;            /* CP bits 36:48 */
    /* events as this end saw them (samples since the start; -1 = not yet), for tests */
    uint32_t   t_ja, t_ja_end, t_sd, t_jd, t_jdp, t_dil_end, t_ri, t_ribar, t_mp, t_ed, t_data;
    int        dil_passes;          /* the least symbols measured per interval and Ucode */
    int        erle_db10;           /* the canceller at the end of MD */
    int64_t    erle_rx, erle_res;
    int        renegs;              /* renegotiations completed */
    /* ---- V.92, PCM upstream (v92 on and INFO1a of Table 18) ---- */
    int        pcm;                 /* 1: this call's upstream is PCM */
    pcmtx_t    ptx;                 /* the symbols on to our clock */
    int32_t    lu;                  /* LU: the level of TRN1u (3.8/V.92), linear */
    int32_t    ec_n, ec_len;        /* the canceller learns on TRN1u: samples so far, of */
    v92_sgn_t  sgn;                 /* two-point signals (GPA) */
    v92_ml_t   ml;                  /* the TRN2u modulation */
    int        sbit;                /* the line bit (sign) of the last symbol sent */
    v92_jd_t   jd92;
    v92_jp_t   jp; int jp_ok;
    v92_cpu_t  cpt92;               /* CPt, Table 23 type 0 */
    int        cpt_ready, ri_seen, ru_due, ja_stop, in_reneg;
    uint32_t   slip;                /* a slip to make once the symbol being made is out, Q16; 0 none */
    uint32_t   up92_rates;          /* Ja: upstream rates, bit r = (r + 18) * 8000 / 6 */
    v92aseq_t  aseq;                /* SUVd, CPd, Ed */
    v92_cpd_t  cpd;                 /* the CPd in use */
    int        got_suvd, got_ack, got_cpd;     /* got_cpd 2: one with drn = 0 (the digital
                                                   modem's cleardown, 9.11) */
    int        cpu_sent, ack_sent, seq_ack, cpus_left;  /* cpus_left: our cleardown, CPus still to
                                                   send; the digital modem's, symbols of
                                                   acknowledging SUVu / CPu still to send */
    int32_t    since_cpu;
    v92_enc_t  enc;
    /* enc's map and filter, which it reads at every symbol: cpd's as
       start_b1u found them (both checked), the filter less the zeros at the
       ends of its sections (v92_filt_trim). Written there only, just before
       v92_enc_init: a CPd merged into cpd later (the downstream decodes CPds
       until Ed, after the upstream has started) does not reach the running
       encoder */
    v92_map_t  encm;
    v92_filt_t encf;
    int32_t    b1_bits;
    uint32_t   t_su, t_jp, t_cpt, t_suvd;
    /* ---- retrains, our renegotiation, the receiver's watch on itself ---- */
    tonedet_t  tone_b;              /* the digital modem's Tone B: its retrain */
    info0_t    info0d;              /* the first Phase 2's INFO0d (a retrain has none) */
    int        retrains;            /* begun this call, either end */
    int        retrains_ours;       /* ... of them, ours */
    int        rt_recent;           /* retrains lately: one forgotten per minute of data mode */
    int32_t    rt_clock;            /* data frames towards that minute */
    int        rt_due;              /* at the end of this tick: 1 our retrain, 2 the digital modem's */
    int        rt_check;            /* ended in Phase 3 / 4 or a renegotiation, after data: retrain */
    int        reneg_ours;          /* our renegotiation under way: 1 asked for, 2 for the line */
    int        clear_err;           /* a cleardown of ours ends with this error, not V90A_ERR_HANGUP */
    int        max_rx0;             /* cfg.max_rx_bps as configured */
    uint32_t   dsn[6];              /* the noise CP was designed for, by interval (as v90arx_mse_slot) */
    int32_t    deg_frames, good_frames;     /* data frames in a row with the error above / below it */
    int32_t    col_frames;          /* ... with the receiver lost (the error far above it) */
    int        col_hold;            /* the timing loop held since a collapse (line_watch) */
    int32_t    since_auto;          /* data frames since a renegotiation for the line ended */
    int        auto_down;           /* the receiver's watch took the rate down (or retrained) */
    int        best_rate;           /* the highest downstream of this call */
} v90a_t;

void    v90a_init(v90a_t *a, const v90a_cfg *cfg);
int16_t v90a_tick(v90a_t *a, int16_t rx);
/* data signalling rates, bit/s, 0 outside data mode: tx = upstream, rx = downstream */
int     v90a_tx_rate(const v90a_t *a);
int     v90a_rx_rate(const v90a_t *a);
/* the downstream receiver's signal to error ratio in data mode, 0.1 dB, or -1 */
int     v90a_snr_db10(const v90a_t *a);
/* From data mode: clear down (9.7): S, S-bar, CP with drn = 0. Ends in
 * V90A_ENDED / V90A_ERR_HANGUP. 0, or -1 when not in data mode. */
int     v90a_cleardown(v90a_t *a);
/* From data mode or a rate renegotiation: begin a retrain (9.5.2.1 /
 * 9.7.2.1). 0, or -1 (not that far, or V90A_MAX_RETRAINS reached). */
int     v90a_retrain(v90a_t *a);
/* From data mode: a rate renegotiation of ours (9.6.2.1 / 9.8.2.1), CP
 * designed anew on the noise of the digital modem's TRN2d. max_rx_bps > 0:
 * a new downstream ceiling (never above the configured one); 0: the ceiling
 * stays. 0, or -1 when not in data mode. */
int     v90a_renegotiate(v90a_t *a, int max_rx_bps);

#endif
