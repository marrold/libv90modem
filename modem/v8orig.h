/* V.8 call-side start-up (V.8 8.1), from the moment the far end has answered:
 *
 *   [CI on V.21(L), ON 3 sequences / OFF 1 s, optional (7.1, 8.1.1)]
 *   listening for the answer tone (modem/anstone):
 *     ANS (V.25)   -> V8O_RES_ANS: no CM may be sent (7.2); the owner goes the
 *                     pre-V.8 way (V.32bis Annex A, V.22bis)
 *     ANSam (V.8)  -> silent for Te (8.1.1) -> CM on V.21(L), repeated, while
 *                     listening for JM on V.21(H) (8.1.2) -> after 2 identical
 *                     JM: the current octet completed, CJ, 75 ms of silence
 *                     -> V8O_RES_OK, see jm and selected
 *
 * CI is sent with its silent periods long enough (1 s, within the 0.4 to 2 s
 * of 7.1) for the answer tone detector to decide in one of them: it listens
 * only while CI is off, where the echo of our own signal is not in the way.
 *
 * Sample-driven, fixed-point, 8000 samples per second, no allocation. */
#ifndef V90D_MODEM_V8ORIG_H
#define V90D_MODEM_V8ORIG_H
#include "anstone.h"
#include "v21.h"
#include "v8rx.h"

typedef enum { V8O_LISTEN, V8O_TE, V8O_CM, V8O_CJ, V8O_POST, V8O_DONE } v8o_state;

#define V8O_RES_NONE      0
#define V8O_RES_OK        1   /* CM / JM / CJ done: see jm and selected */
#define V8O_RES_ANS       2   /* the answer tone was ANS: a pre-V.8 answerer */
#define V8O_RES_NO_TONE   3   /* no answer tone within tone_ms */
#define V8O_RES_NO_JM     4   /* CM sent for jm_ms without two identical JM */
#define V8O_RES_NO_COMMON 5   /* JM came with no modulation mode in common (8.1.2): CJ was sent */

#define V8O_POST_MS 75
#define V8O_MAXBITS (20 + 10 * V8_MAX_OCTETS)

typedef struct {
    int16_t   amp;            /* transmit peak amplitude */
    v8_menu_t local;          /* what CM offers */
    int       send_ci;        /* CI before the answer tone */
    int       te_ms;          /* Te, 500 or more (8.1.1); 0 = 1000 (V.25 echo control) */
    int       tone_ms;        /* how long to wait for the answer tone; 0 = 10000 */
    int       jm_ms;          /* how long to send CM without JM; 0 = 5000 */
} v8orig_cfg;

typedef struct {
    v8orig_cfg cfg;
    v8o_state  state;
    int32_t    t;             /* samples in the current state */
    long       n;             /* samples since v8orig_init */
    anst_t     tone;
    v21mod_t   mod;
    v21demod_t dem;
    v8rx_t     rx;
    int64_t    squelch;
    /* the answer tone under the JM receiver (V8O_CM): 2100 Hz in blocks of
     * 5 ms, as modem/anstone; while it lasts the receiver reads idle mark */
    uint32_t   g_ph;
    int32_t    g_i, g_q;
    int64_t    g_pow;
    int        g_n, g_hold;
    uint8_t    bits[V8O_MAXBITS];   /* the sequence being sent: CI or CM, then CJ */
    int        nbits, pos;
    int        ci_on, ci_seqs;      /* CI: in an ON period, sequences sent in it */
    int32_t    ci_off;              /* samples of the current OFF period */
    long       ci_sent;             /* CI sequences sent in all */
    int        jm_seen;             /* two identical JM: CJ follows the current octet */
    v8_menu_t  jm;
    int        result, selected;    /* V8O_RES_*, v8_selected_caller(jm) */
    anst_result answer_tone;
    long       at_tone, at_cm, at_cj, at_post;  /* sample numbers, for the tests */
} v8orig_t;

void      v8orig_init(v8orig_t *v, const v8orig_cfg *cfg);
/* one sample in, one out; returns the state after this sample */
v8o_state v8orig_tick(v8orig_t *v, int16_t rx, int16_t *tx);

#endif
