/* Test peer: a K56flex client calling our answerer, written from
 * docs/history/k56-facts.md ("V.8bis signals", "V.8bis messages", "NS
 * block", "Phase 2") as SM56 does it: it checks that our pieces fit
 * together; it does not prove K56flex.
 *
 *   V.8bis as the responding station: the initiator's CRe or MRe (1375 +
 *   2002 Hz held 100 ms, then its second segment) answered, 175 ms after
 *   it ends, with CRd (1529 + 2225 Hz 400 ms, 1900 Hz 100 ms); our CL
 *   received on V.21 channel 1 and its NS block checked with SM56's test
 *   (v8bis_cl_is_k56); 10 ms after it, SM56's MS on channel 2; then ACK(1)
 *   or NAK(1) received on channel 1. Per cfg.v8bis: the whole exchange, CRd
 *   and no MS (a V.92 modem), no V.8bis at all, or the MS with a bad FCS.
 *   V.8 as a caller (sim/v8call) from the start, as v34peer's own: CM with
 *   V.34, or with cm_pcm V.34 and the PCM octet of an analogue modem (SM56
 *   in automode). When JM selects V.34, sim/v34peer from Phase 2 on
 *   (skip_v8), exactly as v34peer goes on after its own V.8; once V.8bis
 *   agreed (our ACK(1) came), its INFO1c carries 0x001 (info1c_k56) or 0x200
 *   (its codec check failed), and after our INFO1a flagged it stops
 *   (VP_K56). up_sr_3000: it cannot transmit at 3200 baud (INFO0c).
 *
 *   From the flagged INFO1a on (K56C_K56), K56flex's Phases 3 and 4 and
 *   data (docs/history/k56-facts.md "Training down", "Upstream and J24",
 *   "Phase 4", "Data"). Down, the server's octets pass the network's pad
 *   (pad_mode, x2call_pad's model) and robbed bits (rbs: the octet's last
 *   bit forced to 1 in the line samples whose count mod 6 has its bit set,
 *   "Robbed bits"); the client checks each training segment against the
 *   facts (seg_bad[]), finds Js (js_ok: the 16-level J), then reports the
 *   truth of the line in its J24 (or j_octet), takes the server's MP and
 *   MP' (mp_good, mpa_good: each checked for type 0, our rate and its up
 *   rate, a mask, asymmetric, 16 states, linear, minimum shaping), E
 *   (e_seen), drops B1, and decodes the data with k56enc_unframe and GPA
 *   at min(the server's rate_ac, rate). Up, as V.34's call modem (GPC, the
 *   carrier and 3200 baud of INFO1a): silent until it has our Js, then S,
 *   S-bar, PP, TRN (4 points, 4000 T), J24 (the 16-point J and the octet)
 *   until it sees our TRN2A, J', TRN (16 points), its MP (rate_ac = rate,
 *   rate_ca 13, up_mask, asymmetric; or mp_bad's) until it has decoded our
 *   MP, MP' until our MP', E, then B1 and V.34 data at our MP's rate_ca,
 *   with the options our MP asked for (heavy_up: x2's, whatever it asked).
 *   silent: where it falls quiet for good (the server's time limits).
 *
 *   Its renegotiation ("Rate renegotiation"), reneg_ms into data mode (and
 *   reneg_ms2 into the data after it): up, V.34's (11.6) S, S-bar, TRN, its
 *   MP (rate_ac reneg_rate, or 0 both ways with reneg_rate0) until it has
 *   decoded our MP, MP' until our MP', E, all on 4 points, then B1 and data
 *   at our new MP's rate_ca. Down, the server's answer is checked as the
 *   training is: R in place of a whole data frame (an R it did not ask for
 *   is counted, r_unasked), TRN2A 2048 (the server's bits a symbol tracked,
 *   nb), MARK, TRN2B, MP and MP' on the pad's 2 magnitudes, E 24, the data
 *   on the grid of 24 at min(the server's rate_ac, reneg_rate).
 *
 *   Its retrain ("Retrain"), retrain_ms into data mode and into the data
 *   after each one, retrains times (or, retrain_in_reneg, halfway through
 *   its renegotiation's TRN): V.34's (11.5): 70 ms of silence, then
 *   v34peer's Phase 2 of a retrain (tone B, ...; INFO1c 0x001 again, or
 *   0x200 with retrain_200), and after our INFO1a flagged again the whole
 *   training from silence 1 as above: what it found is counted on.
 *
 * The line is digital, as x2call's: one octet from the server in (mu-law,
 * as it left it), one linear sample back. Test side. */
#ifndef V90D_SIM_K56CALL_H
#define V90D_SIM_K56CALL_H
#include <stdint.h>
#include "v8call.h"
#include "v34peer.h"
#include "../modem/v21.h"
#include "../modem/v8bismsg.h"
#include "../modem/v34mod.h"
#include "../modem/v34mp.h"
#include "../modem/k56tx.h"
#include "../modem/k56enc.h"
#include "../v34/v34.h"
#include "../v90/v90_scr.h"

#define K56C_V8BIS_FULL    0    /* CRd, MS with SM56's block */
#define K56C_V8BIS_NO_MS   1    /* CRd, no MS */
#define K56C_V8BIS_NONE    2    /* ignores CRe and MRe */
#define K56C_V8BIS_BAD_FCS 3    /* its MS with a bad FCS */

/* where the client falls silent for good (cfg.silent) */
enum { K56C_SIL_NONE, K56C_SIL_BEFORE_S, K56C_SIL_BEFORE_J, K56C_SIL_BEFORE_MP,
       K56C_SIL_BEFORE_E, K56C_SIL_DATA, K56C_SIL_RENEG, K56C_SIL_RETRAIN };
/* cfg.heavy_up */
enum { K56C_HEAVY_NONE, K56C_HEAVY_ALWAYS, K56C_HEAVY_FIRST };
/* an MP the server cannot take (cfg.mp_bad) */
enum { K56C_MP_GOOD, K56C_MP_RATE0, K56C_MP_MASK0, K56C_MP_UP_NONE };

typedef struct {
    int level;              /* transmit RMS, sample units */
    int v8bis;              /* K56C_V8BIS_* */
    int cm_pcm;             /* CM offers PCM (V.90 analogue modem) as well */
    int info1c_k56;         /* once agreed, INFO1c says 0x001 (1) or 0x200 (0); k56call_default: 1 */
    int up_sr_3000;         /* INFO0c without the 3200 carriers: at most 3000 baud up */
    /* K56flex (k56call_default: pad 0, rbs 0, j_octet -1, rate 13, up_mask 0x3fff) */
    int pad_mode;           /* the network's pad on our downstream: 0 none, 1 3 dB, 2 6 dB */
    int rbs;                /* robbed bits on the line: bit k, the samples whose count mod 6 is k */
    int j_octet;            /* the octet of its J24; -1: the truth of pad_mode and rbs */
    int rate;               /* its MP's rate_ac: the downstream rate code it asks, 1..13 */
    uint16_t up_mask;       /* its MP's upstream rates (bit n - 1 for n x 2400 bit/s) */
    int silent;             /* K56C_SIL_*: before its S, its J24, its J' (and MP), its E,
                               1 s into data mode, after its renegotiation's S, or after
                               200 ms of its retrain's tone B */
    int mp_bad;             /* K56C_MP_*: rate_ac 0, mask 0, or only 33600 (nothing a
                               3200-baud receiver takes) */
    int early_e;            /* sends E after this many MPs, without waiting for our MP' (not
                               conforming: our E detection before our MP state) */
    int b1_bad;             /* its B1 is wrong (every point of the first symbols inverted) */
    /* its renegotiation ("Rate renegotiation"): reneg_ms into data mode, its
     * S, S-bar, TRN and MP asking rate_ac reneg_rate; a second one reneg_ms2
     * into the data after it, asking reneg_rate2 (0: none). reneg_rate0: the
     * (first) renegotiation's MP has rate 0 both ways (V.34's cleardown) */
    int reneg_ms, reneg_rate, reneg_ms2, reneg_rate2, reneg_rate0;
    /* its retrain ("Retrain"): retrain_ms into data mode, and into the data
     * after each retrain, until it has begun retrains of them. retrain_200:
     * a retrain's INFO1c says 0x200 (no K56flex) */
    int retrain_ms, retrains, retrain_200;
    /* heavy_up: a client that ignores our MP's options for its transmitter:
     * its upstream data uses 64 states, non-linear, expanded shaping (x2's
     * configuration; GPC), whatever our MP asked (K56C_HEAVY_ALWAYS), or
     * only until its first renegotiation (K56C_HEAVY_FIRST; what our MP
     * asks after it). retrain_in_reneg: it gives
     * up its (first) renegotiation halfway through its TRN and retrains
     * (silence, tone B, as retrain_ms's) */
    int heavy_up, retrain_in_reneg;
    int  (*getbit)(void *user);
    void (*putbit)(void *user, int bit);
    void *user;
} k56call_cfg;

typedef enum {
    K56C_V8BIS,             /* V.8bis (and V.8 listening for ANSam all along) */
    K56C_V8,                /* V.8 under way */
    K56C_V34,               /* v34peer from Phase 2 on */
    K56C_K56,               /* K56flex, from our flagged INFO1a on */
    K56C_IDLE,              /* V.8 selected something else: silent */
    K56C_ENDED
} k56c_state;

typedef struct {
    k56call_cfg cfg;
    k56c_state  state;
    int32_t     now, t;
    /* V.8bis */
    int         bst;                /* the responder's step */
    double      gr[2], gi[2], gph[2], gpw; int gn, gblocks;
    double      ph[3];
    int32_t     crd_at;             /* when its CRd goes out, -1 not yet */
    v21demod_t  dem; int64_t squelch; int level; int32_t clk;
    v8bis_rx_t  rx; uint8_t msg[V8B_MAXOCT];
    v21mod_t    mod; uint8_t bits[V8B_MAXBITS]; int nbits, pos; int32_t ms_at;
    int         got_cre;            /* the initiator's dual tone held 100 ms */
    int         got_cl, cl_ok;      /* a CL came; its NS block passed SM56's test */
    int         sent_ms;
    int         got_ack, got_nak;
    /* V.8 and V.34 */
    v8call_t    v8;
    int         v8_sel;             /* what JM selected (v8_selected), -1 before */
    v34peer_t   peer;
    /* ---- K56flex: downstream ---- */
    int32_t     line_n;             /* line samples, for the robbed-bit model */
    uint8_t     sent;               /* this octet as it left the server: breaks a robbed tie
                                       in the training checks only (which()) */
    int         dseg;               /* the server's segment (k56tx_seg) being received */
    int32_t     dn;                 /* its symbols so far */
    int32_t     alt;                /* the alternating count (from TRN1B, from TRN2A) */
    int32_t     trn1b_at;           /* line sample of TRN1B's first symbol */
    int64_t     sum;                /* the training's running sum, of what was sent */
    uint32_t    hist;               /* the training's scrambled bits, newest in bit 0 */
    v90_scr_t   tdscr;              /* the training's descrambler (self-synchronising) */
    int32_t     tknown;             /* scrambled bits known in a row */
    int         tones;              /* TRN2A: descrambled ones in a row */
    int32_t     sil2_n, trn2a_n, trn2b_n;   /* lengths seen */
    uint8_t     mpb[192]; int mpfn, mpones; /* the server's MP being collected */
    v34_mp_t    our_mp;             /* the server's, as last decoded */
    int         got_our_mp, got_our_mpa;
    int         r_used;             /* the downstream rate code in data */
    k56enc_t    dec; v90_scr_t ddscr; uint8_t dfr[K56ENC_N]; int dfpos; int32_t dframes;
    int32_t     data_at;            /* line sample of the first data symbol, -1 before */
    /* what it found (test side) */
    long        seg_bad[K56TX_DATA];    /* octets of each training segment against the facts */
    int         js_ok;              /* our Js descrambled to the 16-level J, 4 times */
    int         mp_good, mpa_good;  /* our MPs and MP's decoded and as expected */
    int         mp_wrong;           /* ... decoded but not as expected */
    int         e_seen;             /* our E: 24 ones */
    long        b1_bad;             /* B1 frames that were not ones */
    long        data_frames, data_lost; /* data frames; ones that did not decode */
    /* ---- K56flex: upstream ---- */
    v34mod_t    umod;
    int         sig; int32_t sn;    /* signal and symbol count in it */
    v34_tmod_t  tmod; int32_t g16;
    uint8_t     mpbits[V34_MP0_BITS]; int mpn, mp_ack, mps_sent;
    uint8_t     j24[24];            /* the J24's bits, time order */
    int         js_seen, trn2a_seen;
    int32_t     j24_at;             /* when its first J24 went out, -1 not yet */
    int         up_rate;
    v34_params_t tpar; v34_tx_t vtx; int32_t tx_dg;
    int         quiet;              /* cfg.silent has struck */
    int32_t     quiet_at;           /* when, -1 not yet */
    int32_t     e_at;               /* when its E began, -1 not yet */
    /* ---- its renegotiation ---- */
    int         rate_now;           /* the rate_ac its MPs ask now */
    int         rn;                 /* its renegotiation under way: its S to its data */
    int         renegs;             /* renegotiations begun (its S) */
    int32_t     rn_at;              /* when its last S began, -1 none */
    int         rn_down;            /* the server's Phase 4 being received is a renegotiation's */
    int         nb;                 /* the server's bits a symbol in TRN2A and MARK's even
                                       symbols (k56-facts.md "Bits a symbol") */
    int         rmatch;             /* octets of this data frame that are R's */
    long        r_unasked;          /* an R in the data it did not ask for */
    /* ---- its retrain ---- */
    int         rt_due;             /* due at this data symbol: begun after it */
    int         retrains;           /* retrains begun (its silence, then tone B) */
    int32_t     rt_at;              /* when its last one began, -1 none */
    int         k56_starts;         /* flagged INFO1a received (the first, then a retrain's) */
} k56call_t;

/* a client that takes K56flex up: the whole V.8bis exchange, INFO1c 0x001 */
void       k56call_default(k56call_cfg *cfg, int level);
void       k56call_init(k56call_t *c, const k56call_cfg *cfg);
/* one octet from the server in (as it left it), one line sample out */
int16_t    k56call_tick(k56call_t *c, uint8_t octet_from_server);
k56c_state k56call_state(const k56call_t *c);
/* the downstream rate it decodes in data mode, bit/s (0 before) */
int        k56call_down_rate(const k56call_t *c);

#endif
