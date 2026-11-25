/* V.22bis start-up handshake (6.3.1), retrain (6.4) and data-mode control,
 * both roles. */
#include "v22bis.h"
#include "clock.h"

#define MS(ms) ((int32_t)(ms) * 8)        /* 8 kHz sample clock */

/* The end of a received S1 is noticed this long after it really ended on the
 * line (filter delay + classifier smoothing); timers measured from "circuit
 * 112 ON" are started that much in the past. */
#define S1_END_LAG MS(13)

/* "scrambled binary 1 (or 0) for 270 +/- 40 ms" (6.3.1.1.1 c, 6.3.1.1.2 b) */
#define SB1_MS   270
#define SB1_BITS 48                         /* and the descrambler really shows a steady run */

/* ---- start-up on a poor line ----
 * Once S1 has been exchanged both modems go to 2400 bit/s; nothing in 6.3.1
 * lets either of them change its mind after that. The one decision the
 * answering modem has is whether to return S1 at all: if it does not, it is,
 * on the line, a modem "set to operate at 1200 bit/s" (6.3.1), and the caller
 * falls back by 6.3.1.1.1 c. So the caller's S1 is used to measure the line,
 * and below this signal-to-noise ratio in the receive band (a power ratio;
 * 63 = 18 dB, which is about 11 dB over the whole 0..4 kHz band) S1 is not
 * returned. Measured bit error rate at 2400 bit/s around that point: 3e-3
 * at 10 dB over 4 kHz, 1e-4 at 12 dB; at 8 dB the 16-point receiver does not
 * hold. The measurement on 100 ms of S1 scatters by about 1 dB either way. */
#define S1_SNR_2400 63

/* ---- retrain (6.4) ---- */
/* our retrain signal is repeated if no S1 came back during it or within the
 * "maximum expected two-way propagation delay", 1.2 s recommended */
#define RT_ANSWER_MS   1200
/* "If the modem fails to synchronize on the received retrain sequence, the
 * modem shall transmit another retrain signal": no 32 ones at 2400 bit/s this
 * long after the far end's S1 ended (they are due from 600 to 800 ms). Also
 * applied to the first start-up, in place of giving up. */
#define RT_SYNC_MS     2000
/* The Recommendation repeats the retrain signal for ever. We stop after this
 * many in a row without reaching data mode (about 15 s) and report failure. */
#define RT_MAX_TRIES   10
/* S1 is 100 ms long (6.3.1.2 / 6.4); one still on the line after this is not
 * waited for: a steady tone at the carrier looks like S1 for as long as it
 * lasts. When retraining that is a retrain signal of our own (RT_MAX_TRIES
 * then ends the call), in start-up back to the wait for the far end. */
#define V22_S1_MAX_MS  1000
/* Loss of equalisation: the long-term slicer error (v22rx mse_long: about 500
 * symbols, each counted for at most 8192; 4096 = the squared distance from a
 * point to its decision boundary) above LOSS_MSE for LOSS_MS without a break.
 * A 16-point receiver that still decides correctly at a useless 8 dB SNR
 * shows about 1300, one that has lost the constellation 2600 or more. Noise
 * alone therefore never asks for a retrain (which could not help), and a
 * noise burst has to last over a second to. Not looked at for the first two
 * seconds of data mode. */
#ifndef LOSS_MSE
#define LOSS_MSE       2000
#endif
#define LOSS_MS        1000
#define LOSS_HOLD_MS   2000

void v22m_init(v22bis_t *m, v22_role role, int allow2400, int level,
                 v22_getbit_fn getbit, v22_putbit_fn putbit, void *user)
{
    m->role = role; m->allow2400 = allow2400;
    v22tx_init(&m->tx, role == V22_CALLER ? V22_FC_LOW : V22_FC_HIGH, level, getbit, user);
    v22rx_init(&m->rx, role == V22_CALLER ? V22_FC_HIGH : V22_FC_LOW, putbit, user);
    m->state = V22S_WAIT; m->t = 0; m->t112 = -1; m->s1_tx = 0; m->seen = 0;
    m->bitrate = 0; m->tx_ready = m->rx_ready = 0;
    m->retraining = 0; m->rt_sent = 0; m->rt_tries = 0; m->retrains = 0; m->rt_lost = 0; m->t_s1 = 0;
    m->s1_snr = -1; m->low_snr_1200 = 0;
    if (role == V22_ANSWERER) v22tx_set_mode(&m->tx, V22_TX_USB1);
}

void v22m_set_guard(v22bis_t *m, int hz)
{
    v22tx_set_guard(&m->tx, m->role == V22_ANSWERER ? hz : 0);
}

static void go(v22bis_t *m, v22_state s) { m->state = s; m->t = 0; m->seen = 0; }

static int steady_run(const v22rx_t *r) { return r->ones_run >= SB1_BITS || r->zeros_run >= SB1_BITS; }

/* far end is sending scrambled 1200 bit/s: count how long */
static int sb1_seen(v22bis_t *m)
{
    if (m->rx.sig == V22_SIG_OTHER) m->seen++; else m->seen = 0;
    /* a converged 4-point constellation, not just any in-band energy (a
     * calling tone or speech must not pass for a V.22 caller) */
    return m->seen >= MS(SB1_MS) && steady_run(&m->rx) && m->rx.mse < 2500;
}

/* 6.4 a, as the modem that starts the retrain: circuit 106 OFF, 100 ms of
 * S1, then scrambled ones at 1200 bit/s (the s1_tx timer in v22m_tick does
 * that switch). Circuit 104 "may be clamped": it is, at once, if the receiver
 * has lost equalisation. A receiver that is still good goes on delivering
 * the far end's data until its S1 is heard (as when the far end starts the
 * retrain), so that nothing the far end sent before it noticed is lost. */
static void retrain_send(v22bis_t *m, int lost)
{
    if (m->rt_tries >= RT_MAX_TRIES) { go(m, V22S_FAILED); return; }
    m->rt_tries++;
    m->retraining = 1; m->rt_sent = 1;
    m->tx_ready = 0;
    if (lost || !m->rx_ready) {
        m->rx_ready = 0;
        v22rx_clamp(&m->rx, 1);
        v22rx_retrain(&m->rx, lost);
    }
    v22tx_set_mode(&m->tx, V22_TX_S1); m->s1_tx = MS(100); m->t_s1 = 0;
    m->t112 = -1;
    go(m, V22S_RT_WAIT);
}

/* S1 from the far end in (or on the way to) data mode: it wants a retrain.
 * Circuit 104 is clamped and circuit 106 turned OFF at once, as 6.6.2 a has
 * it for the same signal (6.4 a only says "following the end" of S1; data
 * taken from the DTE meanwhile would be lost at the far end, whose receiver
 * is already clamped). With 106 OFF the line carries scrambled ones at 2400
 * bit/s until the S1 has ended and ours is due. */
static void retrain_heard(v22bis_t *m)
{
    /* an S1 during ours, or within the answer window after it, is the answer
     * to ours and not a new request: it gets no second S1 from us */
    m->rt_sent = m->retraining && m->rt_sent && m->t_s1 < MS(100 + RT_ANSWER_MS);
    m->retraining = 1;
    m->rx_ready = 0;
    v22rx_clamp(&m->rx, 1);
    if (m->tx_ready) { m->tx_ready = 0; v22tx_set_mode(&m->tx, V22_TX_SB1_2400); }
    m->t112 = -1;
    go(m, V22S_S1_RX);
}

void v22m_retrain(v22bis_t *m)
{
    if (m->state == V22S_DATA && m->bitrate == 2400) retrain_send(m, 0);
}

int16_t v22m_tick(v22bis_t *m, int16_t in)
{
    v22rx_tick(&m->rx, in);
    m->t = clk_sat_inc(m->t);
    if (m->t_s1 < MS(60000)) m->t_s1++;

    /* our own 100 ms of S1, then scrambled ones at 1200 */
    if (m->s1_tx > 0 && --m->s1_tx == 0) v22tx_set_mode(&m->tx, V22_TX_SB1_1200);

    switch (m->state) {
    case V22S_WAIT:
        if (m->role == V22_CALLER) {
            /* 155 ms of unscrambled binary 1, then stay silent 456 ms more */
            if (m->rx.sig == V22_SIG_USB1) m->seen++; else m->seen = 0;
            if (m->seen >= MS(155)) go(m, V22S_SILENT);
            break;
        }
        /* answerer */
        if (m->allow2400 && m->rx.sig == V22_SIG_S1) { go(m, V22S_S1_RX); break; }
        if (sb1_seen(m)) {                       /* V.22 caller: 6.3.1.2.2 b */
            v22tx_set_mode(&m->tx, V22_TX_SB1_1200);
            m->bitrate = 1200;
            go(m, V22S_UP_1200);
        }
        break;

    case V22S_SILENT:
        if (m->t >= MS(456)) {
            if (m->allow2400) { v22tx_set_mode(&m->tx, V22_TX_S1); m->s1_tx = MS(100); }
            else v22tx_set_mode(&m->tx, V22_TX_SB1_1200);
            go(m, V22S_TRAIN);
        }
        break;

    case V22S_TRAIN:                             /* caller only */
        if (m->allow2400 && m->rx.sig == V22_SIG_S1) { go(m, V22S_S1_RX); break; }
        if (sb1_seen(m)) {                       /* 6.3.1.2.1 c: ready to receive at 1200 */
            m->bitrate = 1200;
            m->rx.data_on = 1; m->rx_ready = 1;
            go(m, V22S_UP_1200);
        }
        break;

    case V22S_S1_RX:
        if (m->rx.sig == V22_SIG_S1) {
            if (m->t >= MS(V22_S1_MAX_MS)) {
                if (m->retraining) retrain_send(m, 0);
                else go(m, m->role == V22_CALLER ? V22S_TRAIN : V22S_WAIT);
            }
            break;
        }
        if (m->rx.sig == V22_SIG_NONE) {
            if (m->retraining) retrain_send(m, 0);
            else go(m, m->role == V22_CALLER ? V22S_TRAIN : V22S_WAIT);
            break;
        }
        if (!m->retraining && m->role == V22_ANSWERER) {
            /* too noisy for 16 points: do not return S1, be a 1200 bit/s modem */
            m->s1_snr = v22rx_s1_snr(&m->rx);
            /* under some 40 ms it was no S1 (a V.22 caller's scrambled ones
             * can pass for one for a moment) */
            if (m->s1_snr < 0) { go(m, V22S_WAIT); break; }
            if (m->s1_snr < S1_SNR_2400) {
                m->allow2400 = 0; m->low_snr_1200 = 1;
                go(m, V22S_WAIT);
                break;
            }
        }
        /* end of S1: circuit 112 ON (start-up); the timed part of a retrain (6.4 b) */
        m->t112 = S1_END_LAG;
        m->bitrate = 2400;
        if (m->retraining) {
            m->tx_ready = 0;                     /* circuit 106 OFF */
            v22rx_retrain(&m->rx, 0);            /* the far end is at 1200 bit/s now */
            if (!m->rt_sent) { v22tx_set_mode(&m->tx, V22_TX_S1); m->s1_tx = MS(100); m->t_s1 = 0; m->rt_sent = 1; }
        }
        else if (m->role == V22_ANSWERER) { v22tx_set_mode(&m->tx, V22_TX_S1); m->s1_tx = MS(100); }
        go(m, V22S_UP_2400);
        break;

    case V22S_UP_2400:
        if (v22rx_s1_certain(&m->rx)) { retrain_heard(m); break; }  /* S1 (again) from the far end */
        m->t112++;
        if (m->t112 == MS(450)) { m->rx.mode16 = 1; m->rx.ones_run = 0; }      /* 16-way decisions */
        if (m->t112 == MS(600)) v22tx_set_mode(&m->tx, V22_TX_SB1_2400);
        if (m->t112 == MS(800)) { v22tx_set_mode(&m->tx, V22_TX_DATA_2400); m->tx_ready = 1; }
        if (!m->rx_ready && m->rx.mode16 && m->rx.ones_run >= 32) {
            m->rx.data_on = 1; v22rx_clamp(&m->rx, 0); m->rx_ready = 1;
        }
        if (m->tx_ready && m->rx_ready) {
            if (m->retraining) m->retrains++;
            m->retraining = 0; m->rt_tries = 0;
            m->rx.mse_long = 0;
            go(m, V22S_DATA);
        }
        else if (m->t112 > MS(RT_SYNC_MS)) retrain_send(m, 0);
        break;

    case V22S_RT_WAIT:
        if (v22rx_s1_certain(&m->rx)) {
            if (m->rx_ready) { m->rx_ready = 0; v22rx_clamp(&m->rx, 1); }
            go(m, V22S_S1_RX);
            break;
        }
        if (m->t >= MS(100 + RT_ANSWER_MS)) retrain_send(m, 0);     /* not answered: once more */
        break;

    case V22S_UP_1200:
        if (m->t >= MS(765)) {
            v22tx_set_mode(&m->tx, V22_TX_DATA_1200);
            m->tx_ready = 1;
            m->rx.data_on = 1; m->rx_ready = 1;
            go(m, V22S_DATA);
        }
        break;

    case V22S_DATA:
        if (m->bitrate != 2400) break;           /* 6.4 is for 2400 bit/s operation */
        if (v22rx_s1_certain(&m->rx)) retrain_heard(m);
        else if (m->t > MS(LOSS_HOLD_MS) && m->rx.carrier && m->rx.mse_long > LOSS_MSE) {
            if (++m->seen >= MS(LOSS_MS)) { m->rt_lost++; retrain_send(m, 1); }
        }
        else m->seen = 0;
        break;

    case V22S_FAILED:
        break;
    }
    return v22tx_tick(&m->tx);
}
