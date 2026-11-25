/* Test peer: a V.92 analogue modem with PCM upstream, Phases 3 and 4 and data
 * mode (V.92 9.5.2, 9.6.2, 9.8.2, 9.11), as far as a test of the digital
 * modem needs it.
 *
 * Upstream it is a real transmitter at the symbol level: Ru, TRN1u, Ja, Su
 * with the half symbol slip and the slip asked for in Jp, TRN1u, CPt, E1u,
 * TRN2u, SUVu, CPu, E2u, B1u and data through the precoder and prefilter the
 * digital modem dictates in CPd (v92_enc). Each call gives one symbol
 * amplitude (1 = LU) and the fraction of a sample by which it is late; the
 * channel (v92chan) makes a line signal of it.
 * Downstream it cheats, as p34a does: it reads the digital modem's octets as
 * sent and checks what is sent and when. Host only. */
#ifndef V90D_SIM_V92A_H
#define V90D_SIM_V92A_H
#include <stdint.h>
#include "../v90/v90_tx.h"
#include "../v90/test/v90_dec.h"
#include "../v92/v92_seq.h"
#include "../v92/v92_sig.h"
#include "../v92/v92_enc.h"

typedef struct {
    int alaw, uinfo;
    int sil0;               /* symbols of silence before Ru (560 nominal) */
    int ntrn1;              /* first TRN1u, >= 2040 */
    int ntrn1b;             /* second TRN1u (multiple of 12; with no DIL it lasts until Ri) */
    int jd_wait;            /* symbols between receiving Jd and sending Su */
    int dil_n;              /* DIL-segments asked for (0 = SCR instead) */
    int dil_syms;           /* symbols of DIL wanted before CPt */
    int trn2u_min;          /* TRN2u before SUVu when no SUVd came (12000 nominal) */
    int suvd_wait;          /* symbols of TRN2u after which a modem that has seen no SUVd gives up
                               and falls silent, 0 = it waits (a U.S. Robotics 5686: 3.8 s) */
    int cpd_wait;           /* the same from its first SUVu, for CPd (the 5686: 3.5 s) */
    int first_cpd_plain;    /* a first CPd that has the acknowledge bit set is answered with
                               silence (the 5686, seven calls of seven) */
    uint32_t up_rates;      /* Ja: upstream rates supported, bit r = (r + 18) * 8000 / 6 */
    int wait_cpu;           /* SUVu bit 26 */
    int Kt, Mt;             /* CPt: K and points per constellation */
    int Kd, Md, sr_d, ld_d; /* CPu */
    double gain_err;        /* the data mode transmitter is this factor off (1 = exact) */
    int b1u_lead;           /* data frames with every Ki = 0 sent before B1u, the encoder
                               starting afresh after them (the CX93001: 1) */
    int ja_no_ones;         /* Ja follows TRN1u without the 24 ones of 8.5.4 (the CX93001) */
    int conv_start;         /* state of the convolutional encoder at the first symbol of
                               B1u (8.7.1: 0; the CX93001 after a rate renegotiation: any) */
    int z2_dead;            /* a prefilter coefficient z2(k) the modem takes as zero,
                               0 = none (the CX93001: 64) */
    int drop_cpd;           /* the first so many CPds of the call are missed (read as
                               nothing): the repeat of 9.6.1.1.3 is needed */
    int reneg_no_suvu;      /* in a rate renegotiation TRN2u goes on and no SUVu follows */
    int reneg_no_ack;       /* in a rate renegotiation no CPu or SUVu carries the acknowledge bit */
    int  (*getbit)(void *user);
    void (*putbit)(void *user, int bit);
    void *user;
} v92a_cfg;

typedef enum {
    VA_SIL0, VA_RU, VA_RUBAR, VA_TRN1U, VA_JA, VA_SIL1, VA_SU1, VA_SUBAR1, VA_SU2, VA_SUBAR2,
    VA_TRN1U2, VA_CPT, VA_E1U, VA_TRN2U, VA_SEQ, VA_E2U, VA_DATA,
    VA_RRU, VA_RRUBAR,      /* rate renegotiation: Ru, Ru-bar, then TRN2u */
    VA_DOWN                 /* cleared down: silent */
} v92a_ustate;

typedef enum {
    VD_WAIT_SD, VD_SDBAR, VD_SIGN, VD_DIL, VD_SCR, VD_RI, VD_RIBAR, VD_TRAIN, VD_B1D, VD_DATA,
    VD_RD, VD_RDBAR
} v92a_dstate;

typedef struct {
    v92a_cfg cfg;
    /* upstream */
    v92a_ustate us; long un;
    double phase; int skip;
    v92_sgn_t sgn; v92_ml_t ml;
    int last_sbit;
    uint8_t fr[V92_JA_MAX_BITS + 64]; int frn, frpos;
    int fr_kind, fr_ack;            /* sequence being sent: 0 SUVu, 1 CPu */
    int cpu_sent, ack_sent, cpu_repeat; long since_cpu;
    long since_suvu;                /* symbols since the first SUVu of this Phase 4 */
    v92_enc_t enc; long b1_bits; int lead;
    /* downstream */
    v92a_dstate ds; long dn;
    uint8_t look[24]; long nlook;
    v90dec_sign_t dt, dj, dq;
    uint8_t sb[400]; int sbn, sones;
    int seg, segm;
    v90dec_t dec; v90_scr_t dscr;
    uint8_t fo[6]; int fon;
    uint8_t mb[V92_CPD_MAX_BITS + 64]; int mbn, mones;
    int zero_frames;
    /* parameters */
    v92_ja_t ja; v92_cpu_t cpt, cpu; v90_map_t tmap, dmap;
    v92_jd_t jd; v92_jp_t jp; v92_cpd_t cpd, cpd_new;
    /* events and results */
    int sdbar_seen, jd_ok, jp_ok, jpp_seen, ri_seen, ribar_seen, ed_seen;
    int suvd_n, suvd_ack, cpd_n, cpd_ack, cpd_dropped;
    int trn1d_n, trn1d_bad, sign_bad, dil_n_ok, dil_bad, scr_n, scr_bad, ri_n, ri_bad, trn2d_frames, trn2d_bad;
    int b1d_bad, b1d_frames, dec_bad, seq_bad;
    int tx_ready, rx_ready, up_rate, down_rate;
    long sd_at, tick;
    int rd_pol, rd_units, rdbar_units, rd_bad;
    int renegs, clear_seen, cleardown, data_starts, in_reneg;
    double pw_acc; long pw_n;       /* mean square of G v(n) in data mode */
} v92a_t;

void v92a_init(v92a_t *p, const v92a_cfg *cfg);
/* One PCM octet from the digital modem in. Returns 1 and a symbol (*amp, in
 * units of LU, *frac of a sample late), or 0 when there is none this
 * sample. */
int  v92a_tick(v92a_t *p, uint8_t octet, double *amp, double *frac);
/* From data mode: start a rate renegotiation (9.8.2.1). Kd > 0 asks for a new
 * downstream K; cleardown sends CPu with drn = 0 (9.11). Returns 0, or -1
 * when not in data mode. */
int  v92a_renegotiate(v92a_t *p, int Kd, int cleardown);

#endif
