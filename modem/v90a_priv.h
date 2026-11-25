/* The V.90 / V.92 analogue modem's insides, shared by its files:
 *   modem/v90a.c        init, the sample tick, the public calls, the end
 *   modem/v90a_up90.c   Phase 3's start and the V.34 upstream of V.90 (S, S-bar,
 *                       MD, PP, TRN, Ja, CPt, SCR, CP, E, data)
 *   modem/v90a_up92.c   V.92: Phase 3's start, Jd / Jp / Jp', TRN2d's SUVd, CPd,
 *                       Ed, and the PCM upstream (Ru ... CPt, E1u, TRN2u, SUVu,
 *                       CPu, E2u, B1u, data)
 *   modem/v90a_down.c   the downstream sequences: Jd, Jd', the DIL, Ri, TRN2d,
 *                       MP, Ed, B1d, data frames, Rd; the DIL's analysis and the
 *                       CPt / CP designs, as jobs
 *   modem/v90a_watch.c  time limits, the receiver's watch on itself in data
 *                       mode, our renegotiation, the retrain
 * Internal to the analogue modem (modem/v90a.h is its interface). */
#ifndef V90D_MODEM_V90A_PRIV_H
#define V90D_MODEM_V90A_PRIV_H
#include <stdint.h>
#include "v90a.h"
#include "clock.h"
#include "v34train.h"
#include "probe.h"
#include "pcmenc.h"
#include "../v90/v90_pcm.h"

#define MS(x) ((int32_t)(x) * 8)
#define YF V90ARX_YF

/* our choices where the Recommendation leaves them open */
#define TRN_SYMS     1024           /* TRN: >= 512 T (9.3.2.3) */
#define DIL_MIN      8              /* DIL symbols per interval and Ucode before we end it (4 passes) */
#define TRN2D_MEAS   4000           /* TRN2d symbols decided with CPt's points before CP is designed
                                       (the digital modem's MP comes after 7200 here) */
#define CLEAR_CPS    4              /* CP with drn = 0 sent before we go silent (9.7) */
#define LOSS_SYMS    4000           /* data mode: half a second of an input 12 dB under the start's */
/* time limits */
/* Sd-bar within 2000 ms of the start of Ja: a stated deviation from V.90
 * 9.3.2.4 (V.92 9.5.2.2.1), which says 1500 ms and adds no round trip. The
 * digital modem may wait 500 ms after receiving a whole Ja (9.3.1.3), then
 * sends 54 ms of Sd and Sd-bar; on a round trip of about 0.9 s or more one
 * that follows the text cannot meet 1500 ms, and a live server was seen
 * answering about 2 s after the start of Ja on a VoIP path (reported by an
 * independent softmodem). Waiting longer is bounded and costs nothing when
 * Sd comes. (docs/limits.md) */
#define T_SD_MS      2000
#define T_JD_MS      4500           /* 9.3.2.7: Jd within 4500 ms of the end of Ja */
#define T_JDP_MS     3000           /* Jd' after our S (Jd is 9 ms; the digital modem answers at once) */
#define T_DIL_MS     4000           /* enough DIL within this of Jd' (9.3.2.10: 5000 ms) */
#define T_RI_MS      3000           /* Ri after our S-bar */
#define T_P4_MS      15000          /* 9.4.2: B1d within 15 s (+ 5 round trips) of INFO1a */
#define T_RENEG_MS   6000           /* 9.6.2: Ed within 5000 ms (+ 2 round trips) */
/* V.92 */
#define TRN1U_SYMS   6000           /* the first TRN1u (>= 2040 T): the canceller learns on it */
#define TRN1U2_MIN   2040           /* the second, before CPt (9.5.2.1.9) */
#define TRN2U_SYMS   12000          /* TRN2u when no SUVd has come (9.6.2.1.1) */
#define TRN2U_RENEG  2400           /* ... in a rate renegotiation (9.8.2.1.2) */
#define T_JP_MS      3000           /* Jp after our Su, Jp' after our second Su-bar */
#define T_P4_92_MS   20000          /* 9.6.2.2.1: B1d within 20 s (+ 6 round trips) of INFO1a */
#define T_RENEG92_MS 12000          /* the digital modem designs CPd again: several seconds */
/* retrains, and the receiver's watch on itself in data mode (data frames: 1333 a second) */
#define RT_FORGET_FRAMES 80000      /* a minute of data mode forgets a retrain */
#define DEG_FRAMES   1333           /* 1 s with the error 3 dB over the design noise: act */
#define COL_FRAMES   333            /* 250 ms with it 12 dB over (the receiver lost): retrain */
#define UP_FRAMES    6667           /* 5 s 6 dB under it, after a renegotiation down: back up */
#define AUTO_GRACE   6667           /* still degraded within 5 s of a renegotiation for the line: retrain */
#define T_SCR_MS     2000           /* 9.6.2.1.2: SCR for no more than 2000 ms */

enum { U_SIL, U_S1, U_SB1, U_MD, U_S2, U_SB2, U_PP, U_TRN, U_JA, U_QUIET, U_SJ, U_SBJ, U_DILQ, U_S4, U_SB4,
       U_CPT, U_SCR, U_CP, U_E, U_DATA, U_DATA_END, U_RS, U_RSB, U_OFF };
enum { D_SD, D_JD, D_DIL, D_DILEND, D_RI, D_TRAIN, D_B1D, D_DATA, D_RD };
/* the V.92 upstream (the same `us`) */
enum { P_SIL = 64, P_RU, P_RUB, P_TRN1, P_JA, P_QUIET, P_SU1, P_SUB1, P_SU2, P_SUB2, P_TRN1B, P_CPT, P_E1U,
       P_TRN2U, P_SEQ, P_E2U, P_DATA, P_RRU, P_RRUB };

/* jobs (v90a_down.c): the DIL's analysis, CPt's design, CP's design */
enum { JOB_NONE, JOB_DIL, JOB_CPT, JOB_CP };

/* the upstream to sub-state s */
static inline void ugo(v90a_t *a, int s) { a->us = s; a->un = 0; }
/* S (a renegotiation, a cleardown): in data mode once the mapping frame
 * under way is out, so that every bit taken from getbit is sent whole */
static inline void go_s(v90a_t *a) { if (a->us != U_DATA_END) ugo(a, a->us == U_DATA ? U_DATA_END : U_RS); }
static inline unsigned fr_bit(const v90a_t *a, int i) { return (unsigned)(a->fr[i >> 3] >> (i & 7)) & 1u; }

/* modem/v90a.c */
void    v90a__end(v90a_t *a, int err);
/* modem/v90a_up90.c */
void    v90a__start_p3(v90a_t *a);                  /* Phase 2 is over (V.92: v90a__start_p3_v92) */
void    v90a__load_bits(v90a_t *a, const uint8_t *bits, int n);   /* the sequence to send */
void    v90a__up_symbol(v90a_t *a, int32_t *re, int32_t *im);
/* modem/v90a_up92.c */
void    v90a__start_p3_v92(v90a_t *a);
void    v90a__jd92_bit(v90a_t *a, const v90arx_dec_t *d);
void    v90a__train92(v90a_t *a, const uint8_t *bits, int D, int32_t m0);
int16_t v90a__up92_symbol(v90a_t *a);
/* modem/v90a_down.c */
int     v90a__dil_known(void *ctx, int32_t m, int *u, int *pos, int32_t *lev);   /* v90arx_cfg_t.known_lv */
void    v90a__set_cons(v90a_t *a, const v90_cp_t *cp, int32_t m);
void    v90a__cp_to_cpu(const v90_cp_t *cp, int type, v92_cpu_t *u);
void    v90a__job_step(v90a_t *a);
void    v90a__job_finish(v90a_t *a, int kind);
void    v90a__dil_finish(v90a_t *a);
void    v90a__rebase(v90a_t *a);
void    v90a__on_dec(v90a_t *a, const v90arx_dec_t *d);
/* modem/v90a_watch.c */
int     v90a__renegotiate(v90a_t *a, int kind);
void    v90a__line_watch(v90a_t *a);
void    v90a__watch(v90a_t *a);
void    v90a__begin_retrain(v90a_t *a, int ours);

#endif
