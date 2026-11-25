/* The answering modem's x2 scheme (modem/ans_scheme.h): ANS_X2, from the
 * client's marker on (modem/x2d: PCM downstream, V.34 upstream). We never
 * retrain x2; the client's retrain ends the call. */
#include <string.h>
#include "ans_scheme.h"
#include "../v90/v90_pcm.h"

/* The x2 client's marker ended Phase 2 (p2ans P2N_X2): its symbol rate and
 * carrier, and the round trip, from there; silent until its J. A marker x2d
 * cannot serve ends the call, said as x2's. */
void ans_start_x2(answer_t *a)
{
    x2d_cfg c;
    memset(&c, 0, sizeof c);
    c.marker = a->v34.p2.x2_marker;
    c.rtd = a->v34.p2.rtd;
    c.echo_delay = a->echo_delay;
    c.max_up_rate = a->max_up;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    ans_enter(a, ANS_X2); a->t = 0; a->pcm = 0x7f;
    if (x2d_init(&a->u.x2, &c) != 0) ans_fail(a, ANS_END_X2_FAILED);
}

static int16_t tick(answer_t *a, int16_t rx)
{
    x2d_t *d = &a->u.x2;
    int16_t tx;
    a->pcm = x2d_tick(d, rx);
    tx = (int16_t)v90_octet_linear(0, a->pcm);
    if (d->state == X2D_ENDED) {
        /* there is no way back to V.34 once the marker came (design
         * "Calls made in the design"): a start-up that did not reach
         * data mode, or a rate change of the client's that did not
         * complete, is said as x2's; in data mode the line's loss,
         * our cleardown and the client's retrain (its Tone A, said as
         * a retrain that failed) end the call */
        if (d->error == X2D_ERR_RETRAIN) ans_fail(a, ANS_END_RETRAIN_FAILED);    /* the client's retrain */
        else if (!a->connected || (d->error != X2D_ERR_CARRIER && d->error != X2D_ERR_HANGUP)) ans_fail(a, ANS_END_X2_FAILED);
        else ans_fail(a, d->error == X2D_ERR_HANGUP ? ANS_END_CLEARDOWN_OURS : ANS_END_LOST);
        tx = 0;
    } else if (d->state == X2D_DATA) {
        a->connected = 1; a->modulation = ANS_MOD_X2;
        a->bitrate = x2d_down_rate(d); a->rx_bitrate = x2d_up_rate(d);
        a->retraining = 0;
    } else if (a->connected) {
        a->retraining = 0;              /* the client's rate change (X2D_RENEG): no data moves up */
    }
    return tx;
}

static int cleardown(answer_t *a) { return x2d_cleardown(&a->u.x2); }
static int moving(const answer_t *a) { return a->u.x2.state == X2D_DATA; }
static int snr_den(const answer_t *a, int64_t *num, int64_t *den)
{
    *num = 65536; *den = a->u.x2.up.rx.mse;          /* the V.34 upstream */
    return 0;
}
static int rtd(const answer_t *a) { return a->v34.p2.rtd; }
static int echo_delay(const answer_t *a) { return a->u.x2.ecd.delay; }

const ans_scheme_t ans_x2 = { tick, cleardown, NULL, moving, snr_den, rtd, echo_delay, 1 };
