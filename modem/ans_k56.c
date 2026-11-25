/* The answering modem's K56flex schemes (modem/ans_scheme.h): V.8bis
 * before V.8 (ans_v8bis: ANS_V8BIS), K56flex from our flagged INFO1a on
 * (ans_k56: ANS_K56, modem/k56d) and the client's retrain (ans_k56rt:
 * ANS_K56RT). We never retrain K56flex (k56 design, decision 14). */
#include <string.h>
#include "ans_scheme.h"
#include "../v90/v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)
#define K56_MAX_RETRAINS 3   /* the client's third retrain in a row ends the call ... */
#define K56_RT_DATA_MS  2000 /* ... and data mode this long: its retrains are not "in a row" */
#define K56_RT_MS       9000 /* the client's retrain to our Phase 2's end */

/* ---- ans_v8bis: ANS_V8BIS ---- */

static int16_t tick_v8bis(answer_t *a, int16_t rx)
{
    int16_t tx = 0;
    /* K56flex's V.8bis; V.8 follows from its own silence, whatever came
     * of it. An agreement turns x2 off for the call (k56 design,
     * decision 5): no x2 JM, no x2 INFO0a. */
    if (v8bis_tick(&a->u.v8b, rx, &tx) != V8B_DONE) return tx;
    a->k56_agreed = v8bis_result(&a->u.v8b) == V8B_RES_AGREED;
    if (a->k56_agreed) a->v8.x2_offer = 0;
    ans_enter(a, ANS_V8); a->t = 0;
    return tx;
}

const ans_scheme_t ans_v8bis = { tick_v8bis, NULL, NULL, NULL, NULL, NULL, NULL, 0 };

/* ---- ans_k56: ANS_K56 ---- */

/* Our flagged INFO1a ended Phase 2 (p2ans P2N_K56): no way back to V.34.
 * The upstream's carrier (our choice in INFO1a) and the round trip from
 * there; a configuration k56d cannot serve ends the call, said as
 * K56flex's. */
void ans_start_k56(answer_t *a)
{
    k56d_cfg c;
    memset(&c, 0, sizeof c);
    c.rtd = a->v34.p2.rtd;
    c.echo_delay = a->echo_delay;
    c.hi_carrier = a->v34.p2.info1a.hi_carrier;
    c.max_up_rate = a->max_up; c.max_down_rate = a->max_down;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    ans_enter(a, ANS_K56); a->t = 0; a->pcm = 0xff;
    if (a->v34.p2.sym_rate != 3200 || k56d_init(&a->u.k56, &c) != 0) ans_fail(a, ANS_END_K56_FAILED);
}

static int16_t tick_k56(answer_t *a, int16_t rx)
{
    k56d_t *d = &a->u.k56;
    int16_t tx;
    a->pcm = k56d_tick(d, rx);
    tx = (int16_t)v90_octet_linear(0, a->pcm);
    if (d->state == K56D_ENDED) {
        /* past the point of no return (k56 design, decision 7): a
         * start-up or a renegotiation that did not reach data mode is
         * said as K56flex's; in data mode the upstream's loss, our
         * cleardown and the client's (an MP with rate 0) end it */
        ans_fail(a, d->error == K56D_ERR_HANGUP ? ANS_END_CLEARDOWN_OURS
                    : d->error == K56D_ERR_CLEARDOWN ? ANS_END_CLEARDOWN_FAR     /* its renegotiation MP, rate 0 */
                    : a->connected && d->error == K56D_ERR_CARRIER ? ANS_END_LOST : ANS_END_K56_FAILED);
        tx = 0;
    } else if (d->state == K56D_RETRAIN) {
        /* its tone B in data mode or in its renegotiation (decision 14):
         * retrains in a row, the third ends the call as K56flex's.
         * K56flex's own counting, not an rt_policy_t: the retrain is
         * counted (retrains too) and the clock restarted before the limit
         * is checked, the end is K56flex's, and data mode forgets them all
         * at once after K56_RT_DATA_MS on a clock that runs only while
         * there are any */
        a->retrains++; a->retraining = 1; a->rt_clock = 0;
        if (++a->rt_recent >= K56_MAX_RETRAINS) { ans_fail(a, ANS_END_K56_FAILED); return 0; }
        ans_enter(a, ANS_K56RT); a->t = 0; tx = 0;
    } else if (d->state == K56D_DATA) {
        a->connected = 1; a->modulation = ANS_MOD_K56;
        a->bitrate = k56d_down_rate(d); a->rx_bitrate = k56d_up_rate(d);
        a->retraining = 0;
        /* data again: once it holds, the retrains before it are not in a row */
        if (a->rt_recent && ++a->rt_clock >= MS(K56_RT_DATA_MS)) { a->rt_clock = 0; a->rt_recent = 0; }
    } else if (a->connected) {
        /* no data moves: the client's renegotiation, or its retrain (from ANS_K56RT) */
        a->retraining = d->state != K56D_RENEG;
    }
    return tx;
}

static int k56_cleardown(answer_t *a) { return k56d_cleardown(&a->u.k56); }
static int k56_moving(const answer_t *a) { return a->u.k56.state == K56D_DATA; }
static int k56_snr_den(const answer_t *a, int64_t *num, int64_t *den)
{
    *num = 65536; *den = a->u.k56.up.rx.mse;         /* the V.34 upstream */
    return 0;
}
static int k56_rtd(const answer_t *a) { return a->v34.p2.rtd; }
static int k56_echo_delay(const answer_t *a) { return a->u.k56.ecd.delay; }

const ans_scheme_t ans_k56 = { tick_k56, k56_cleardown, NULL, k56_moving, k56_snr_den, k56_rtd, k56_echo_delay, 1 };

/* ---- ans_k56rt: ANS_K56RT ---- */

static int16_t tick_k56rt(answer_t *a, int16_t rx)
{
    int16_t tx;
    /* the client's retrain (k56 design, decision 14; V.34 11.5): 70 ms
     * of silence, then Phase 2 of a retrain with INFO1a flagged again
     * (no INFO0: its INFO0c of the first holds); our flagged INFO1a
     * starts K56flex afresh. Anything else (INFO1c 0x200, Phase 2
     * failed, too long) ends the call: no way back to V.34. */
    if (a->t <= MS(70)) {
        if (a->t == MS(70)) {
            p2ans_cfg c;
            memset(&c, 0, sizeof c);
            c.level = a->v34.cfg.level;
            c.io_delay = a->v34.cfg.io_delay > 0 ? a->v34.cfg.io_delay : 0;
            c.sr_mask = a->v34.cfg.sr_mask;
            c.max_rate = a->v34.cfg.max_rx_rate;
            c.k56 = 1;
            p2ans_init_retrain(&a->v34.p2, &c, &a->v34.info0c);
        }
        return 0;
    }
    tx = p2ans_tick(&a->v34.p2, rx);
    if (a->v34.p2.state == P2N_K56) {
        ans_start_k56(a); tx = 0;
    } else if (a->v34.p2.state == P2N_DONE || a->v34.p2.state == P2N_FAILED || a->v34.p2.state == P2N_X2
               || a->t > MS(K56_RT_MS)) {
        ans_fail(a, ANS_END_K56_FAILED); tx = 0;
    }
    return tx;
}

/* between engines: the echo delay is the modulation's */
const ans_scheme_t ans_k56rt = { tick_k56rt, NULL, NULL, NULL, NULL, NULL, ans_mod_echo_delay, 0 };
