/* Test peer: V.34 Phase 2, call modem. See p2c.h and V.34 11.2.1.1. */
#include <string.h>
#include "p2c.h"

#define MS(x) ((x) * 8)
#define TONE_SEEN 160
#define TURN 320
#define TAIL 80
#define PROBE_BLOCKS 23

static void go(p2c_t *p, p2c_state s) { p->state = s; p->t = 0; }
static void fail(p2c_t *p) { dpsktx_off(&p->tx); p->probing = 0; go(p, P2C_FAILED); }

static void send_info0c(p2c_t *p, int ack)
{
    info0_t i = p->cfg.info0c;
    uint8_t bits[INFO_MAX_BITS];
    int n;
    i.ack = (uint8_t)ack;
    n = info0a_build(&i, bits);         /* INFO0c and INFO0a share Table 14 */
    dpsktx_send(&p->tx, bits, n);
    p->n_info0c++;
}

static void common_init(p2c_t *p, const p2c_cfg *cfg)
{
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->level = cfg->level;
    p->state = P2C_INFO0;
    dpsktx_init(&p->tx, DPSK_FC_B, p->level, 0);
    dpskrx_init(&p->rx, DPSK_FC_A);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));
    probetx_init(&p->ptx, p->level);
    proberx_init(&p->prx);
    p->t_rev1 = p->t_rev2 = p->t_rev3 = p->t_tone_a2 = p->t_info1a = -1;
}

void p2c_init(p2c_t *p, const p2c_cfg *cfg)
{
    common_init(p, cfg);
    p->rep_left = cfg->info0_repeat;
    send_info0c(p, 0);
}

void p2c_init_retrain(p2c_t *p, const p2c_cfg *cfg)
{
    common_init(p, cfg);
    p->got0a = 1;
    dpsktx_on(&p->tx);
    go(p, P2C_WAIT_REV1);
}

static const int16_t SR[6] = { 2400, 2743, 2800, 3000, 3200, 3429 };
static const int16_t FC[6][2] = { {1600, 1800}, {1646, 1829}, {1680, 1867},
                                  {1800, 2000}, {1829, 1920}, {1959, 1959} };

static void probe_result(p2c_t *p)
{
    int m, k;
    for (m = 1; m <= PROBE_NBIN; m++) p->snr[m] = probe_has_tone(m) ? proberx_snr(&p->prx, m) : 0;
    p->info1c.min_pwr_red = (uint8_t)p->cfg.min_pwr_red;
    p->info1c.add_pwr_red = (uint8_t)p->cfg.add_pwr_red;
    p->info1c.md_len = (uint8_t)p->cfg.md_len;
    for (k = 0; k < 6; k++) {
        int hi = (p->cfg.hi_carrier_mask >> k) & 1;
        int lo_f = FC[k][hi] - SR[k] / 2, hi_f = FC[k][hi] + SR[k] / 2;
        int32_t sum = 0, n = 0, rate;
        p->info1c.sr[k].hi_carrier = (uint8_t)hi;
        p->info1c.sr[k].preemph = (uint8_t)p->cfg.preemph[k];
        if (p->cfg.max_rate[k] >= 0) { p->info1c.sr[k].max_rate = (uint8_t)p->cfg.max_rate[k]; continue; }
        for (m = 1; m <= PROBE_NBIN; m++) {
            if (!probe_has_tone(m) || 150 * m < lo_f || 150 * m > hi_f) continue;
            sum += p->snr[m]; n++;
        }
        rate = n ? ((sum / n - 90) * SR[k]) / (30 * 2400) : 0;
        if (rate > 14) rate = 14;
        if (rate < 1) rate = 1;
        p->info1c.sr[k].max_rate = (uint8_t)rate;
    }
    p->info1c.freq_off = (int16_t)proberx_freq_off(&p->prx);
    if (p->cfg.k56 == P2C_K56_ACCEPT) p->info1c.freq_off = 1;
    else if (p->cfg.k56 == P2C_K56_DECLINE) p->info1c.freq_off = -512;     /* 0x200 */
}

static int tone_under_probe(p2c_t *p, int16_t x, int m)
{
    proberx_tick(&p->prx, x);
    if (p->prx.n != 0) return p->tone_blocks >= 2;
    {
        int lvl = proberx_last(&p->prx, m), ref = proberx_last(&p->prx, 6);
        if (lvl > 30 && lvl > 4 * ref) p->tone_blocks++; else p->tone_blocks = 0;
    }
    return p->tone_blocks >= 2;
}

int16_t p2c_tick(p2c_t *p, int16_t x)
{
    int ev = dpskrx_tick(&p->rx, x);
    int32_t out;

    switch (p->state) {
    case P2C_INFO0:
        if (ev & DPSK_EV_FRAME) {
            int again = p->got0a;
            info0a_parse(p->rx.frame, &p->info0a);
            p->got0a = 1;
            p->resend = (again && !p->info0a.ack);
        }
        if (!dpsktx_busy(&p->tx)) {
            if (p->resend || (!p->got0a && p->rx.steady >= MS(50))) {
                send_info0c(p, p->got0a);
                p->resend = 0;
            } else if (p->got0a && p->rx.steady >= TONE_SEEN && p->rep_left != 0) {
                /* cfg.info0_repeat: INFO0c once more, 300 ms after the last
                 * began (Tone B long enough between them for the answer
                 * modem to send its Tone A reversal) */
                if (p->t < MS(300)) break;
                send_info0c(p, 1);
                if (p->rep_left > 0) p->rep_left--;
                p->t = 0;
            } else if (p->got0a && p->rx.steady >= TONE_SEEN) {
                go(p, P2C_WAIT_REV1);
            }
        }
        if (p->t > MS(8000)) fail(p);
        break;

    case P2C_WAIT_REV1:
        if (ev & DPSK_EV_FRAME) {
            info0a_parse(p->rx.frame, &p->info0a);
            p->resend = !p->info0a.ack;
            go(p, P2C_INFO0);
        } else if (ev & DPSK_EV_REVERSAL) {
            p->t_rev1 = p->now - DPSK_RX_REV_DELAY;
            p->act_at = p->now - DPSK_RX_REV_DELAY + TURN - DPSK_TX_DELAY;
            go(p, P2C_REPLY1);
        } else if (p->t > MS(10000)) fail(p);
        break;

    case P2C_REPLY1:
        if (p->now == p->act_at) {
            dpsktx_reverse(&p->tx);
            p->b_rev_line = p->now + DPSK_TX_DELAY;
        }
        if (p->now == p->act_at + TAIL) {
            dpsktx_off(&p->tx);
            go(p, P2C_WAIT_REV2);
        }
        break;

    case P2C_WAIT_REV2:
        if (ev & DPSK_EV_REVERSAL) {
            int32_t line = p->now - DPSK_RX_REV_DELAY;
            p->t_rev2 = line;
            p->rtd = line - p->b_rev_line - TURN;
            if (p->rtd < 0) p->rtd = 0;
            p->rev2_at = line;
            p->probe_from = line + TAIL + MS(160) + MS(10);
            proberx_init(&p->prx);
            go(p, P2C_PROBE_RX);
        } else if (p->t > MS(2000)) {
            go(p, P2C_RECOVER);             /* 11.2.2.1.3 */
        }
        break;

    case P2C_RECOVER:
        if (p->rx.steady >= TONE_SEEN) { dpsktx_on(&p->tx); go(p, P2C_WAIT_REV1); }
        else if (p->t > MS(8000)) fail(p);
        break;

    case P2C_PROBE_RX:
        if (p->now >= p->probe_from) {
            proberx_tick(&p->prx, x);
            if (p->prx.nblk >= PROBE_BLOCKS) {
                probe_result(p);
                dpsktx_on(&p->tx);
                go(p, P2C_WAIT_REV3);
            }
        }
        break;

    case P2C_WAIT_REV3:
        if (p->t_tone_a2 < 0 && p->rx.steady >= TONE_SEEN) p->t_tone_a2 = p->now - TONE_SEEN;
        if (ev & DPSK_EV_REVERSAL) {
            p->t_rev3 = p->now - DPSK_RX_REV_DELAY;
            p->act_at = p->now - DPSK_RX_REV_DELAY + TURN - DPSK_TX_DELAY;
            go(p, P2C_REPLY3);
        } else if (p->now - p->rev2_at > MS(900) + p->rtd + MS(700)) {
            p->act_at = p->now + TURN;      /* 11.2.2.1.4 */
            go(p, P2C_REPLY3);
        }
        break;

    case P2C_REPLY3:
        if (p->now == p->act_at) dpsktx_reverse(&p->tx);
        if (p->now == p->act_at + TAIL) dpsktx_off(&p->tx);
        if (p->now == p->act_at + TAIL + DPSK_TX_DELAY) {
            probetx_init(&p->ptx, p->level);
            proberx_init(&p->prx);
            p->tone_blocks = 0;
            p->probing = 1;
            go(p, P2C_L1);
        }
        break;

    case P2C_L1:
        (void)tone_under_probe(p, x, 16);
        if (p->t >= MS(160) - 1) { p->probing = 2; p->tone_blocks = 0; go(p, P2C_L2); }
        break;

    case P2C_L2:
        if (tone_under_probe(p, x, 16)) {
            uint8_t bits[INFO_MAX_BITS];
            int n;
            p->probing = 0;
            dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO1A_BITS));
            n = info1d_build(&p->info1c, bits);     /* Table 15 has the layout of INFO1d */
            dpsktx_send(&p->tx, bits, n);
            go(p, P2C_INFO1C);
        } else if (p->t > MS(650) + p->rtd + MS(100)) {
            fail(p);                        /* 11.2.2.1.5: a retrain; the test starts it */
        }
        break;

    case P2C_INFO1C:
        if (dpsktx_busy(&p->tx)) p->t = 0;
        else if (p->t == 60) { dpsktx_off(&p->tx); go(p, P2C_WAIT_INFO1A); }
        break;

    case P2C_WAIT_INFO1A:
        if (ev & DPSK_EV_FRAME) {
            info1a_parse(p->rx.frame, &p->info1a);
            p->t_info1a = p->now;
            go(p, P2C_DONE);
        } else if (p->t > MS(700) + p->rtd + MS(300)) {
            fail(p);                        /* 11.2.2.1.6 */
        }
        break;

    case P2C_DONE:
    case P2C_FAILED:
        break;
    }

    out = dpsktx_tick(&p->tx);
    if (p->probing) out += probetx_tick(&p->ptx, p->probing == 1);
    if (p->mute) out = 0;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    p->now++; p->t++;
    return (int16_t)out;
}
