/* Test peer: V.90 Phase 2, analogue modem side (V.90 9.2.2). */
#include <string.h>
#include "p2a.h"

#define MS(x) ((x) * 8)
#define TONE_SEEN 160
#define TURN 320
#define TAIL 80
#define PROBE_BLOCKS 23

static void go(p2a_t *p, p2a_state s) { p->state = s; p->t = 0; }

static void send_info0a(p2a_t *p, int ack)
{
    info0_t i = p->cfg.info0a;
    uint8_t bits[INFO_MAX_BITS];
    int n;
    i.ack = (uint8_t)ack;
    n = info0a_build(&i, bits);
    dpsktx_send(&p->tx, bits, n);
    p->tone_since = -1;
    p->n_info0a++;
}

void p2a_init(p2a_t *p, const p2a_cfg *cfg)
{
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->state = P2A_INFO0;
    dpsktx_init(&p->tx, DPSK_FC_A, cfg->level, 1);
    dpskrx_init(&p->rx, DPSK_FC_B);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0D_BITS));
    probetx_init(&p->ptx, cfg->level);
    proberx_init(&p->prx);
    p->rep_left = cfg->info0_repeat;
    send_info0a(p, 0);
}

void p2a_init_retrain(p2a_t *p, const p2a_cfg *cfg)
{
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->state = P2A_INFO0;
    dpsktx_init(&p->tx, DPSK_FC_A, cfg->level, 1);
    dpskrx_init(&p->rx, DPSK_FC_B);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0D_BITS));
    probetx_init(&p->ptx, cfg->level);
    proberx_init(&p->prx);
    /* as if INFO0 had been exchanged: Tone A, and its reversal once Tone B
     * is there and Tone A has run for 50 ms */
    p->got0d = 1;
    p->info0d.v92 = (uint8_t)(cfg->pcm_up != 0);    /* as the INFO0d of the first Phase 2 said */
    dpsktx_on(&p->tx);
    p->tone_since = -1;
}

static int tone_under_probe(p2a_t *p, int16_t x, int m)
{
    proberx_tick(&p->prx, x);
    if (p->prx.n != 0) return p->tone_blocks >= 2;
    {
        int lvl = proberx_last(&p->prx, m), ref = proberx_last(&p->prx, 6);
        if (lvl > 30 && lvl > 4 * ref) p->tone_blocks++; else p->tone_blocks = 0;
    }
    return p->tone_blocks >= 2;
}

int16_t p2a_tick(p2a_t *p, int16_t x)
{
    int ev = dpskrx_tick(&p->rx, x);
    int32_t out;

    switch (p->state) {
    case P2A_INFO0:
        if (ev & DPSK_EV_FRAME) {
            int again = p->got0d;
            info0d_parse(p->rx.frame, &p->info0d);
            p->got0d = 1;
            p->resend = (again && !p->info0d.ack);
        }
        if (!dpsktx_busy(&p->tx)) {
            if (p->tone_since < 0) p->tone_since = p->now;
            if (p->resend || (!p->got0d && p->rx.steady >= MS(50))) {
                send_info0a(p, p->got0d);
                p->resend = 0;
            } else if (p->got0d && p->rx.steady >= TONE_SEEN && p->now - p->tone_since >= MS(50) + 60
                       && p->rep_left != 0) {
                send_info0a(p, 1);          /* cfg.info0_repeat: INFO0a once more */
                if (p->rep_left > 0) p->rep_left--;
                p->t = 0;
            } else if (p->got0d && p->rx.steady >= TONE_SEEN && p->now - p->tone_since >= MS(50) + 60) {
                dpsktx_reverse(&p->tx);
                p->a_rev_line = p->now + DPSK_TX_DELAY;
                go(p, P2A_WAIT_BREV1);
            }
        }
        if (p->t > MS(5000)) go(p, P2A_FAILED);
        break;

    case P2A_WAIT_BREV1:
        if (p->restart_at > 0) {            /* cfg.no_rev2: Tone A on, then INFO0 again */
            if (p->now >= p->restart_at) { p->restart_at = 0; send_info0a(p, 1); go(p, P2A_INFO0); }
            break;
        }
        if (ev & DPSK_EV_FRAME) {           /* repeated INFO0d */
            info0d_parse(p->rx.frame, &p->info0d);
            p->resend = !p->info0d.ack;
            p->tone_since = p->now;
            go(p, P2A_INFO0);
        } else if ((ev & DPSK_EV_REVERSAL) && p->cfg.no_rev2) {
            p->restart_at = p->now + MS(2500);
        } else if (ev & DPSK_EV_REVERSAL) {
            p->rtd = p->now - DPSK_RX_REV_DELAY - p->a_rev_line - TURN;
            p->act_at = p->now - DPSK_RX_REV_DELAY + TURN - DPSK_TX_DELAY;
            go(p, P2A_REPLY);
        } else if (p->t > MS(3000)) {       /* our reversal went unseen: tone, try again */
            p->tone_since = p->now;
            go(p, P2A_INFO0);
        }
        break;

    case P2A_REPLY:
        if (p->now == p->act_at) dpsktx_reverse(&p->tx);
        if (p->now == p->act_at + TAIL) dpsktx_off(&p->tx);
        if (p->now == p->act_at + TAIL + DPSK_TX_DELAY) {
            probetx_init(&p->ptx, p->cfg.level);
            proberx_init(&p->prx);
            p->tone_blocks = 0;
            p->probing = 1;
            go(p, P2A_L1);
        }
        break;

    case P2A_L1:
        (void)tone_under_probe(p, x, 8);
        if (p->t >= MS(160) - 1) { p->probing = 2; p->tone_blocks = 0; go(p, P2A_L2); }
        break;

    case P2A_L2:
        if (tone_under_probe(p, x, 8)) {
            p->probing = 0;
            dpsktx_on(&p->tx);
            go(p, P2A_TONE50);
        } else if (p->t > MS(1500)) go(p, P2A_FAILED);
        break;

    case P2A_TONE50:
        if (p->t == MS(50) + DPSK_TX_DELAY) { dpsktx_reverse(&p->tx); go(p, P2A_REV_TAIL); }
        break;

    case P2A_REV_TAIL:
        if (p->t == TAIL) { dpsktx_off(&p->tx); go(p, P2A_WAIT_BREV2); }
        break;

    case P2A_WAIT_BREV2:
        if (ev & DPSK_EV_REVERSAL) {
            p->probe_from = p->now - DPSK_RX_REV_DELAY + TAIL + MS(160) + MS(10);
            proberx_init(&p->prx);
            go(p, P2A_PROBE_RX);
        } else if (p->t > MS(3000)) go(p, P2A_FAILED);
        break;

    case P2A_PROBE_RX:
        if (p->now >= p->probe_from) {
            proberx_tick(&p->prx, x);
            if (p->prx.nblk >= PROBE_BLOCKS) {
                dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO1D_BITS));
                dpsktx_on(&p->tx);
                go(p, P2A_WAIT_INFO1D);
            }
        }
        break;

    case P2A_WAIT_INFO1D:
        if (ev & DPSK_EV_FRAME) {
            uint8_t bits[INFO_MAX_BITS];
            int n, k, best = -1;
            info1d_parse(p->rx.frame, &p->info1d);
            for (k = 3; k <= 5; k++)
                if (p->info1d.sr[k].max_rate && (best < 0 || p->info1d.sr[k].max_rate >= p->info1d.sr[best].max_rate))
                    best = k;
            memset(&p->info1a, 0, sizeof p->info1a);
            p->info1a.v90 = p->cfg.v90;
            p->info1a.uinfo = (uint8_t)(p->cfg.v90 ? p->cfg.uinfo : 0);
            p->info1a.up_sr = (uint8_t)(p->cfg.up_sr >= 0 ? p->cfg.up_sr : best < 0 ? 3 : best);
            p->info1a.down_sr = 4;
            p->info1a.max_rate = 12;
            if (!p->cfg.v90) {              /* Table 11: one symbol rate, and the digital modem's carrier and filter */
                p->info1a.down_sr = p->info1a.up_sr;
                p->info1a.hi_carrier = (uint8_t)(p->cfg.v34_hi_carrier != 0);
                p->info1a.preemph = (uint8_t)p->cfg.v34_preemph;
            }
            p->info1a.freq_off = (int16_t)proberx_freq_off(&p->prx);
            /* 9.3/V.92: both V.92 and bit 70 of INFO1d (parsed into the 3429 carrier bit) */
            if (p->cfg.pcm_force || (p->cfg.pcm_up && p->cfg.info0a.v92 && p->info0d.v92 && p->info1d.sr[5].hi_carrier)) {
                p->info1a.v90 = 1; p->info1a.pcm = 1; p->info1a.up_sr = 6;
                p->info1a.uinfo = (uint8_t)p->cfg.uinfo;
                p->info1a.sections = (uint8_t)p->cfg.sections;
                p->info1a.ltot_code = (uint8_t)p->cfg.ltot_code;
                p->info1a.lmax_code = (uint8_t)p->cfg.lmax_code;
                p->info1a.md_len = (uint8_t)p->cfg.md_len;
            }
            n = info1a_build(&p->info1a, bits);
            dpsktx_send(&p->tx, bits, n);
            go(p, P2A_INFO1A);
        } else if (p->t > MS(3000)) go(p, P2A_FAILED);
        break;

    case P2A_INFO1A:
        if (dpsktx_busy(&p->tx)) p->t = 0;
        else if (p->t == 60) { dpsktx_off(&p->tx); p->done_at = p->now; go(p, P2A_DONE); }
        break;

    case P2A_DONE:
    case P2A_FAILED:
        break;
    }

    out = dpsktx_tick(&p->tx);
    if (p->probing) out += probetx_tick(&p->ptx, p->probing == 1);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    p->now++; p->t++;
    return (int16_t)out;
}
