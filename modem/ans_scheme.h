/* The answering modem's schemes (modem/answer.c dispatches to them): what
 * one group of phases does on each sample and what it answers for the
 * public calls of modem/answer.h. answer_t.s is the scheme of the phase,
 * set with it wherever a phase is entered (ans_enter, by the phase table in
 * modem/answer.c; a scheme going from one of its phases to another may set
 * answer_t.phase alone):
 *   ans_legacy  ANS_V22, ANS_V21, ANS_V32_SIL, ANS_V32 (modem/ans_legacy.c)
 *   ans_p2      ANS_P2, ANS_RETRAIN: V.90 and V.92 Phase 2 (modem/ans_v90.c)
 *   ans_v90     ANS_P3, ANS_V90 (modem/ans_v90.c)
 *   ans_v92     ANS_P3X, ANS_V92 (modem/ans_v90.c)
 *   ans_v34     ANS_V34 (modem/ans_v34.c), the V.34 hand-off of V.90 included
 *   ans_x2      ANS_X2 (modem/ans_x2.c)
 *   ans_v8bis   ANS_V8BIS; ans_k56 ANS_K56; ans_k56rt ANS_K56RT (modem/ans_k56.c)
 *   ans_v8      ANS_V8; ans_ended ANS_FAILED (modem/answer.c)
 * The signal to noise, round trip and (between engines) echo delay are the
 * modulation's, not the phase's: answer.c finds those through the scheme of
 * answer_t.modulation (a retrain's Phase 2 answers with the figures of the
 * modulation it retrains). Internal to the answering modem. */
#ifndef V90D_MODEM_ANS_SCHEME_H
#define V90D_MODEM_ANS_SCHEME_H
#include <stdint.h>
#include "answer.h"

typedef struct ans_scheme {
    int16_t (*tick)(answer_t *, int16_t rx);
    int     (*cleardown)(answer_t *);        /* NULL: -1 */
    int     (*retrain)(answer_t *);          /* NULL: -1 */
    int     (*moving)(const answer_t *);     /* NULL: connected && !retraining */
    /* answer_snr_db10's ratio (num / den); -1: no figure. NULL: -1 */
    int     (*snr_den)(const answer_t *, int64_t *num, int64_t *den);
    int     (*rtd)(const answer_t *);        /* NULL: -1 */
    int     (*echo_delay)(const answer_t *); /* NULL: -1 */
    uint8_t pcm_out;                         /* the codeword of the sample is a->pcm (answer_tick_lin) */
} ans_scheme_t;
extern const ans_scheme_t ans_legacy, ans_v90, ans_v92, ans_v34, ans_x2, ans_k56;
extern const ans_scheme_t ans_v8, ans_v8bis, ans_p2, ans_k56rt;
/* enter phase p: answer_t.phase and the scheme of it (answer_t.t is the caller's) */
void ans_enter(answer_t *a, ans_phase p);

/* The retrain bookkeeping of a scheme (answer_t.retrains, rt_recent,
 * rt_clock): a line that needs limit retrains in a row is no line, and data
 * mode makes them not "in a row" again. The retrain due with limit already
 * in a row ends the call (ANS_END_RETRAIN_FAILED), else it is counted.
 * forget_ms: one is forgotten per this much data mode (a clock that runs
 * through every data mode sample of the call). K56flex's counting is not
 * this one (modem/ans_k56.c). */
typedef struct { int limit; int32_t forget_ms; } rt_policy_t;
/* a retrain is due: counted, or the call ends (phase ANS_FAILED) */
void ans_rt_begin(answer_t *, const rt_policy_t *);
/* a sample of data mode */
void ans_rt_data_tick(answer_t *, const rt_policy_t *);
/* V.90's: also V.92's, and V.34's after a V.90 Phase 2 (their retrains are
 * V.90's: ANS_RETRAIN) */
extern const rt_policy_t ans_rt_v90;

/* modem/answer.c */
/* the call ends here, for the reason why (ANS_END_*) */
void ans_fail(answer_t *a, int why);
/* a loss: once connected (a retrain or renegotiation included) the connection was lost */
int  ans_lost_or(const answer_t *a, int why);
/* V.90, V.92, V.34: a retrain of a connection that does not get back to data
 * mode failed, as on the calling side; any other loss as ans_lost_or */
int  ans_retrain_or(const answer_t *a, int why);
/* the echo delay of the modulation (ans_p2, ans_k56rt, ANS_FAILED: between
 * engines) */
int  ans_mod_echo_delay(const answer_t *a);

/* the phases' starts, from the phase before (V.8's result, Phase 2's end, a
 * marker) */
void ans_start_v22(answer_t *a);        /* modem/ans_legacy.c */
void ans_start_v21(answer_t *a);
void ans_start_v23(answer_t *a);
void ans_start_v32(answer_t *a);
void ans_start_p2(answer_t *a);         /* modem/ans_v90.c */
void ans_v90_retrain(answer_t *a);      /* a V.90 retrain is due: ANS_RETRAIN, or the end */
void ans_v34_cfg(const answer_t *a, v34ans_cfg *c);   /* modem/ans_v34.c */
void ans_start_v34(answer_t *a);
void ans_start_x2(answer_t *a);         /* modem/ans_x2.c */
void ans_start_k56(answer_t *a);        /* modem/ans_k56.c */

#endif
