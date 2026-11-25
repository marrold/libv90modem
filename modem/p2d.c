/* V.90 Phase 2, digital modem. See p2d.h and V.90 9.2.1. */
#include "p2d.h"
#include "p2common.h"

int p2_level(int dbm0)
{
    /* 0 dBm0 is an RMS of 15770 in 16-bit linear units; 1 dB = 0.8913 */
    int64_t v = 15770 << 4;
    for (; dbm0 < 0; dbm0++) v = (v * 29205) >> 15;
    return (int)(v >> 4);
}

static void go(p2d_t *p, p2d_state s) { p->state = s; p->t = 0; }
static void fail(p2d_t *p, int why) { p->error = why; dpsktx_off(&p->tx); p->probing = 0; go(p, P2D_FAILED); }

static void send_info0d(p2d_t *p, int ack)
{
    info0_t i;
    uint8_t bits[INFO_MAX_BITS];
    int k, n;
    for (k = 0; k < (int)sizeof i; k++) ((uint8_t *)&i)[k] = 0;
    /* bits 15..18 describe a V.34 transmitter; see v90d's PROGRESS.md (V.34 fallback) */
    i.lo3000 = i.hi3000 = i.lo3200 = i.hi3200 = 1;
    i.max_sr_diff = 5;
    i.ack = (uint8_t)ack;
    i.tx_power = (uint8_t)(-6 - p->cfg.tx_dbm0);
    i.max_tx_power = (uint8_t)p->cfg.max_tx_code;
    i.alaw = (uint8_t)p->cfg.alaw;
    i.v92 = (uint8_t)(p->v92 != 0);     /* Table 15/V.92 bit 27; short Phase 2 (bit 26) is not asked for */
    n = info0d_build(&i, bits);
    dpsktx_send(&p->tx, bits, n);
    p->n_info0d++;
}

void p2d_init(p2d_t *p, const p2d_cfg *cfg)
{
    int k;
    p->cfg = *cfg;
    if (p->cfg.tx_dbm0 > -6) p->cfg.tx_dbm0 = -6;
    if (p->cfg.tx_dbm0 < -21) p->cfg.tx_dbm0 = -21;
    p->level = p2_level(p->cfg.tx_dbm0);
    p->state = P2D_INFO0; p->error = P2D_OK; p->now = 0; p->t = 0;
    p->t_x = 0; p->n_recover = 0; p->n_restart = 0;
    dpsktx_init(&p->tx, DPSK_FC_B, p->level, 0);
    dpskrx_init(&p->rx, DPSK_FC_A);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));
    probetx_init(&p->ptx, p->level);
    proberx_init(&p->prx);
    p->probing = 0;
    p->got0a = 0; p->resend = 0; p->n_info0d = 0;
    p->act_at = p->b_rev_line = p->rev2_at = p->probe_from = p->l2_from = 0;
    p->rtd = 0; p->tone_blocks = 0;
    p->v92 = 0; p->pcm_up = 0; p->peer_v92 = 0; p->mode = P2D_MODE_V90;
    for (k = 0; k <= PROBE_NBIN; k++) p->snr[k] = p->rx_level[k] = 0;
    for (k = 0; k < (int)sizeof p->info0a; k++) ((uint8_t *)&p->info0a)[k] = 0;
    for (k = 0; k < (int)sizeof p->info1d; k++) ((uint8_t *)&p->info1d)[k] = 0;
    for (k = 0; k < (int)sizeof p->info1a; k++) ((uint8_t *)&p->info1a)[k] = 0;
    send_info0d(p, 0);
}

void p2d_init_retrain(p2d_t *p, const p2d_cfg *cfg)
{
    p2d_init(p, cfg);
    /* drop the INFO0d that p2d_init queued: a retrain has none */
    dpsktx_init(&p->tx, DPSK_FC_B, p->level, 0);
    p->n_info0d = 0;
    p->got0a = 1;
    dpsktx_on(&p->tx);
    go(p, P2D_WAIT_REV1);
}

void p2d_offer_v92(p2d_t *p, int pcm_up)
{
    p->v92 = 1; p->pcm_up = pcm_up != 0;
    if (p->state == P2D_INFO0 && p->now == 0) {
        /* replace the INFO0d that p2d_init queued by one with bit 27 set */
        dpsktx_init(&p->tx, DPSK_FC_B, p->level, 0);
        p->n_info0d = 0;
        send_info0d(p, 0);
    }
}

void p2d_retrain_v92(p2d_t *p, int pcm_up)
{
    p->v92 = 1; p->pcm_up = pcm_up != 0; p->peer_v92 = 1;
}

/* 9.2.1.2.1: the caller's INFO0a again (in p->rx.frame) once the INFO0
 * exchange is behind us. It has not heard us: INFO0d with bit 28 set, as
 * INFO0a came in correctly, then Tone B, and P2D_INFO0 goes on from there
 * (INFO0d again while INFO0a comes without bit 28; on Tone A, 9.2.1.1.3).
 * Past P2D_MAX_RESTART the INFO0a is ignored, as it was before 0.6.1, and
 * the state's own time-out ends the call. */
static void info0a_again(p2d_t *p)
{
    if (p->n_restart >= P2D_MAX_RESTART) return;
    p->n_restart++;
    info0a_parse(p->rx.frame, &p->info0a);
    p->got0a = 1; p->resend = 0;
    p->probing = 0;
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));
    send_info0d(p, 1);
    go(p, P2D_INFO0);
}

/* turn the probing measurements into INFO1d */
static void probe_result(p2d_t *p)
{
    int m, k;
    for (m = 1; m <= PROBE_NBIN; m++) {
        p->snr[m] = probe_has_tone(m) ? proberx_snr(&p->prx, m) : 0;
        p->rx_level[m] = proberx_level(&p->prx, m);
    }
    p->info1d.min_pwr_red = 0;
    p->info1d.add_pwr_red = 0;
    p->info1d.md_len = 0;
    for (k = 0; k < 6; k++) {
        int hi = (p->cfg.up_hi_carrier >> k) & 1;
        int fc = p2_carrier_hz(k, hi), lo_f = fc - p2_sr[k] / 2, hi_f = fc + p2_sr[k] / 2;
        int32_t sum = 0, n = 0, rate;
        p->info1d.sr[k].hi_carrier = (uint8_t)hi;
        p->info1d.sr[k].preemph = 0;
        p->info1d.sr[k].max_rate = 0;
        if (!((p->cfg.up_sr_mask >> k) & 1)) continue;
        for (m = 1; m <= PROBE_NBIN; m++) {
            if (!probe_has_tone(m) || 150 * m < lo_f || 150 * m > hi_f) continue;
            sum += p->snr[m]; n++;
        }
        if (!n) continue;
        /* bits per symbol = (mean SNR - 9 dB gap) / 3.01 dB; rate in 2400 bit/s units.
         * 12 is the ceiling without 1664-point constellations (Table 9 note 1). */
        rate = ((sum / n - 90) * p2_sr[k]) / (30 * 2400);
        if (rate > 12) rate = 12;
        if (rate < 0) rate = 0;
        p->info1d.sr[k].max_rate = (uint8_t)rate;
    }
    p->info1d.freq_off = (int16_t)proberx_freq_off(&p->prx);
}

static void send_info1d(p2d_t *p)
{
    uint8_t bits[INFO_MAX_BITS];
    int n;
    if (p->n_info0d > 0) p->peer_v92 = p->info0a.v92;   /* a retrain has no INFO0a */
    /* 9.3/V.92: Table 17 when both sides indicated V.92, else 8.2.3.2/V.90 */
    if (p->v92 && p->peer_v92) n = info1d_build_v92(&p->info1d, p->pcm_up, bits);
    else n = info1d_build(&p->info1d, bits);
    dpsktx_send(&p->tx, bits, n);
}

int16_t p2d_tick(p2d_t *p, int16_t x)
{
    int ev = dpskrx_tick(&p->rx, x);
    int32_t out;

    /* the exchange as a whole (spec D5): the states' own limits start again
     * on every return to INFO0 or WAIT_REV1 */
    if (p->state != P2D_DONE && p->state != P2D_FAILED && p->t_x >= MS(P2D_EXCHANGE_MS)) fail(p, P2D_ERR_TIME);

    switch (p->state) {
    case P2D_INFO0:
        if (ev & DPSK_EV_FRAME) {
            int again = p->got0a;
            info0a_parse(p->rx.frame, &p->info0a);
            p->got0a = 1;
            /* a repeated INFO0a without the acknowledge bit: the far end has not got ours */
            p->resend = (again && !p->info0a.ack);
        }
        if (!dpsktx_busy(&p->tx)) {
            if (p->resend || (!p->got0a && p->rx.steady >= MS(50))) {
                send_info0d(p, p->got0a);
                p->resend = 0;
            } else if (p->got0a && p->rx.steady >= TONE_SEEN) {
                go(p, P2D_WAIT_REV1);
            }
        }
        if (p->t > MS(5000)) fail(p, P2D_ERR_INFO0);
        break;

    case P2D_WAIT_REV1:
        if (ev & DPSK_EV_FRAME) {           /* 9.2.1.2.1: INFO0a again */
            info0a_parse(p->rx.frame, &p->info0a);
            p->resend = !p->info0a.ack;
            go(p, P2D_INFO0);
        } else if (ev & DPSK_EV_REVERSAL) {
            p->act_at = p->now - DPSK_RX_REV_DELAY + TURN - DPSK_TX_DELAY - p->cfg.io_delay;
            go(p, P2D_REPLY1);
        } else if (p->t > MS(10000)) fail(p, P2D_ERR_REV);
        break;

    case P2D_REPLY1:
        if (p->now == p->act_at || (p->t == 0 && p->now > p->act_at)) {
            dpsktx_reverse(&p->tx);
            p->act_at = p->now;
            p->b_rev_line = p->now + DPSK_TX_DELAY;
        }
        if (p->now == p->act_at + TAIL && p->t > 0) {
            dpsktx_off(&p->tx);
            go(p, P2D_WAIT_REV2);
        }
        break;

    case P2D_WAIT_REV2:
        if (ev & DPSK_EV_FRAME) {
            info0a_again(p);
        } else if (ev & DPSK_EV_REVERSAL) {
            int32_t line = p->now - DPSK_RX_REV_DELAY;
            p->rtd = line - p->b_rev_line - TURN - p->cfg.io_delay;
            if (p->rtd < 0) p->rtd = 0;
            p->rev2_at = line;
            /* tone A runs 10 ms on, then L1 for 160 ms, then L2 */
            p->probe_from = line + TAIL + MS(160) + MS(10);
            proberx_init(&p->prx);
            go(p, P2D_PROBE_RX);
        } else if (p->t > MS(2000)) {
            if (p->n_recover >= P2D_MAX_RECOVER) fail(p, P2D_ERR_REV);
            else { p->n_recover++; go(p, P2D_RECOVER); }    /* 9.2.1.2.3 */
        }
        break;

    case P2D_RECOVER:
        if (ev & DPSK_EV_FRAME) info0a_again(p);
        else if (p->rx.steady >= TONE_SEEN) { dpsktx_on(&p->tx); go(p, P2D_WAIT_REV1); }
        else if (p->t > MS(5000)) fail(p, P2D_ERR_REV);
        break;

    case P2D_PROBE_RX:
        if (p->now >= p->probe_from) {
            proberx_tick(&p->prx, x);
            if (p->prx.nblk >= PROBE_BLOCKS) {
                probe_result(p);
                dpsktx_on(&p->tx);
                go(p, P2D_WAIT_REV3);
            }
        }
        break;

    case P2D_WAIT_REV3:
        if (ev & DPSK_EV_REVERSAL) {
            p->act_at = p->now - DPSK_RX_REV_DELAY + TURN - DPSK_TX_DELAY - p->cfg.io_delay;
            go(p, P2D_REPLY3);
        } else if (p->now - p->rev2_at > MS(900) + p->rtd + MS(700)) {
            /* 9.2.1.2.4 (the 900 ms run from the end of our probing reception here) */
            p->act_at = p->now + TURN;
            go(p, P2D_REPLY3);
        }
        break;

    case P2D_REPLY3:
        if (p->now == p->act_at || (p->t == 0 && p->now > p->act_at)) {
            dpsktx_reverse(&p->tx);
            p->act_at = p->now;
        }
        if (p->now == p->act_at + TAIL && p->t > 0) dpsktx_off(&p->tx);
        if (p->now == p->act_at + TAIL + DPSK_TX_DELAY && p->t > 0) {
            probetx_init(&p->ptx, p->level);
            proberx_init(&p->prx);
            p->tone_blocks = 0;
            p->probing = 1;
            go(p, P2D_L1);
        }
        break;

    case P2D_L1:
        (void)tone_under_probe(&p->prx, &p->tone_blocks, x, 16);
        /* one sample short of P2N_L1's: kept (p2common.h) */
        if (p->t >= MS(160) - 1) { p->probing = 2; p->tone_blocks = 0; go(p, P2D_L2); }
        break;

    case P2D_L2:
        if (tone_under_probe(&p->prx, &p->tone_blocks, x, 16)) {
            p->probing = 0;
            dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO1A_BITS));
            dpskrx_expect_also(&p->rx, INFO_PAYLOAD(INFO0A_BITS));     /* 9.2.1.2.1 */
            send_info1d(p);
            go(p, P2D_INFO1D);
        } else if (p->t > MS(650) + p->rtd + MS(100)) {
            fail(p, P2D_ERR_TONE_A);        /* 9.2.1.2.5 asks for a retrain; not implemented */
        }
        break;

    case P2D_INFO1D:
        if (dpsktx_busy(&p->tx)) p->t = 0;
        else if (p->t == 60) { dpsktx_off(&p->tx); go(p, P2D_WAIT_INFO1A); }
        break;

    case P2D_WAIT_INFO1A:
        if ((ev & DPSK_EV_FRAME) && p->rx.flen == INFO_PAYLOAD(INFO0A_BITS)) {
            info0a_again(p);                /* the caller lost our INFO1d and starts over */
        } else if (ev & DPSK_EV_FRAME) {
            info1a_parse(p->rx.frame, &p->info1a);
            switch (info1a_kind(&p->info1a)) {
            case INFO1A_PCM:
                /* Table 18/V.92 needs V.92 at both ends and bit 70 of INFO1d */
                if (!(p->v92 && p->peer_v92 && p->pcm_up)) { fail(p, P2D_ERR_PCM_UP); break; }
                p->mode = P2D_MODE_V92; go(p, P2D_DONE);
                break;
            case INFO1A_V90: p->mode = P2D_MODE_V90; go(p, P2D_DONE); break;
            default:         p->mode = P2D_MODE_V34; go(p, P2D_DONE); break;
            }
        } else if (p->t > MS(700) + p->rtd + MS(300)) {
            fail(p, P2D_ERR_INFO1A);        /* 9.2.1.2.6 (Tone A, INFOMARKSa) not implemented */
        }
        break;

    case P2D_DONE:
    case P2D_FAILED:
        break;
    }

    out = dpsktx_tick(&p->tx);
    if (p->probing) out += probetx_tick(&p->ptx, p->probing == 1);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    p->now++; p->t++;
    if (p->state != P2D_DONE && p->state != P2D_FAILED) p->t_x++;
    return (int16_t)out;
}
