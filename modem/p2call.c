/* V.34 Phase 2, call modem. See p2call.h and V.34 11.2.1.1. */
#include <string.h>
#include "p2call.h"
#include "p2common.h"

#define TX_FLUSH 60             /* samples from the start of the last bit to its end on the line */
/* time limits: the Recommendation's where it gives one */
#define T_INFO0_MS   8000       /* INFO0a and Tone A (none given) */
#define T_REV1_MS   10000       /* 11.2.2.1.2: Tone B "until it does detect" one; but not for ever */
#define T_REV2_MS    2000       /* 11.2.2.1.3 */
#define T_RECOVER_MS 8000
#define T_REV3_MS     900       /* 11.2.2.1.4, plus a round trip */
#define T_L2_MS       650       /* 11.2.2.1.5, plus a round trip */
#define T_INFO1A_MS   700       /* 11.2.2.1.6, plus a round trip */
#define T_TONE_A_MS  2000       /* ... then this long for Tone A before we begin the retrain */
#define MAX_RESTARTS 4          /* returns to the start of the exchange (timed out, or an INFO0 again) */
/* the INFO1c decision */
#define FLOOR_DB10    360       /* signal-dependent noise assumed as a floor, as p2ans: a G.711
                                   codec's, and what our own receiver leaves */
#define HI_DB10        10       /* the high carrier must be better by 1 dB to be asked for */
#define PRE_DB10        2       /* a pre-emphasis filter must gain 0.2 dB over none */

static void go(p2call_t *p, p2call_state s) { p->state = s; p->t = 0; }

static void fail(p2call_t *p, int why)
{
    p->error = why; dpsktx_off(&p->tx); p->probing = 0; go(p, P2L_FAILED);
}

/* Table 14: what this modem can do, as p2ans says it: 3000 and 3200 on
 * either carrier, no 2743 / 2800 / 3429, power reduction, one symbol rate
 * for both directions, up to 1664 points, internal transmit clock */
static void send_info0(p2call_t *p, int ack, int v92)
{
    info0_t i;
    uint8_t bits[INFO_MAX_BITS];
    int n;
    memset(&i, 0, sizeof i);
    i.lo3000 = i.hi3000 = i.lo3200 = i.hi3200 = 1;
    i.allow3429 = 0;
    i.pwr_reduce = 1;
    i.max_sr_diff = 0;
    i.c1664 = 1;
    i.ack = (uint8_t)ack;
    i.v92 = (uint8_t)(v92 != 0);            /* bit 26 of Table 16/V.92; reserved (0) elsewhere */
    n = info0a_build(&i, bits);             /* INFO0c and INFO0a share Table 14 */
    dpsktx_send(&p->tx, bits, n);
    p->n_info0c++;
}

static void send_info0c(p2call_t *p, int ack) { send_info0(p, ack, 0); }

static void common_init(p2call_t *p, const p2call_cfg *cfg)
{
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    if (!p->cfg.sr_mask) p->cfg.sr_mask = P2N_SR_2400 | P2N_SR_3000 | P2N_SR_3200;
    p->state = P2L_INFO0; p->error = P2L_OK;
    dpsktx_init(&p->tx, DPSK_FC_B, cfg->level, 0);
    dpskrx_init(&p->rx, DPSK_FC_A);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));   /* INFO0a: Table 14, 49 bits */
    probetx_init(&p->ptx, cfg->level);
    proberx_init(&p->prx);
    p->t_rev1 = p->t_rev2 = p->t_rev3 = p->t_tone_a2 = p->t_info1a = -1;
}

void p2call_init(p2call_t *p, const p2call_cfg *cfg)
{
    /* 11.2.1.1.1: INFO0c once the 75 ms of silence that end Phase 1 are over */
    common_init(p, cfg);
    send_info0c(p, 0);
}

void p2call_init_retrain(p2call_t *p, const p2call_cfg *cfg, const info0_t *info0a)
{
    /* 11.5.1.1, 11.5.1.2: Tone B, and the Tone A reversal (or INFO0a) */
    common_init(p, cfg);
    p->info0a = *info0a;
    p->got0a = 1;
    dpsktx_on(&p->tx);
    go(p, P2L_WAIT_REV1);
}

/* a retrain we begin, or answer (11.2.2.1.5, 11.2.2.1.6): silence, then as
 * the retrain entry */
static void self_retrain(p2call_t *p, int why)
{
    if (p->retrains >= P2L_MAX_RETRAINS) { fail(p, why); return; }
    p->retrains++;
    dpsktx_off(&p->tx);
    p->probing = 0;
    go(p, P2L_RT_SILENCE);
}

/* ---- the INFO1c decision ---- */

/* The floor after r dB of power reduction. What the empty bins show beyond
 * FLOOR_DB10 is taken as overload of the third order: against the signal it
 * falls 2 dB for each dB less power. */
static int floor_db10(const p2call_t *p, int r)
{
    int sdr = p->sdr_db10 < 0 ? 0 : p->sdr_db10 > 600 ? 600 : p->sdr_db10;
    int64_t q = ((int64_t)1 << 40) / db10_lin(FLOOR_DB10), d = ((int64_t)1 << 40) / db10_lin(sdr);
    int64_t ovl = d > q ? d - q : 0;
    return probe_db10((int64_t)1 << 40, q + ovl / db10_lin(20 * r));
}

/* each tone against the whole signal spread over 4 kHz in the analyser's
 * 50 Hz (80 bins), 0.1 dB: where the floor of what follows the signal
 * stands for that tone */
static void shares(const p2call_t *p, int *share)
{
    int m;
    int64_t ptot = 0;
    for (m = 1; m <= PROBE_NBIN; m++) if (probe_has_tone(m)) ptot += (int64_t)p->rx_level[m] * p->rx_level[m];
    for (m = 0; m <= PROBE_NBIN; m++)
        share[m] = m && probe_has_tone(m) && p->rx_level[m] > 0 && ptot > 0
                   ? probe_db10((int64_t)p->rx_level[m] * p->rx_level[m] * 80, ptot) : 0;
}

/* Symbol rate k at r dB less power: the carrier the answerer shall use and,
 * with filters != 0, its pre-emphasis filter; returns the SNR so projected,
 * or -1 when k "cannot be used": a rate we do not implement or do not
 * allow, or one the answerer cannot send at all (INFO0a bits 15:18; both
 * carriers at 2400). */
static int choose(const p2call_t *p, int k, int r, const int *share, int filters, int *hi, int *pre)
{
    int c, idx, ok[2], cs[2] = { -1, -1 }, cp[2] = { 0, 0 }, f = floor_db10(p, r);
    int mask = p->cfg.sr_mask ? p->cfg.sr_mask : P2N_SR_2400 | P2N_SR_3000 | P2N_SR_3200;
    if ((k != 0 && k != 3 && k != 4) || !((mask >> k) & 1)) return -1;
    ok[0] = k == 3 ? p->info0a.lo3000 : k == 4 ? p->info0a.lo3200 : 1;
    ok[1] = k == 3 ? p->info0a.hi3000 : k == 4 ? p->info0a.hi3200 : 1;
    if (!ok[0] && !ok[1]) return -1;
    for (c = 0; c < 2; c++) {
        int none;
        if (!ok[c]) continue;
        cs[c] = none = band(p->snr, p->rx_level, k, c, 0, r, f, share);
        /* the filter that gives most, if it gives something over none */
        for (idx = 1; filters && idx <= 10; idx++) {
            int v = band(p->snr, p->rx_level, k, c, idx, r, f, share);
            if (v > cs[c]) { cs[c] = v; cp[c] = idx; }
        }
        if (cs[c] <= none + PRE_DB10) { cs[c] = none; cp[c] = 0; }
    }
    *hi = !ok[0] || (ok[1] && cs[1] > cs[0] + HI_DB10);
    *pre = cp[*hi];
    return cs[*hi];
}

/* the projected rate from that SNR, 16ths of 2400 bit/s within the caps:
 * 9 dB for the first bit, 3 dB for each further bit per symbol (as p2ans);
 * Table 8's top for the symbol rate; above 28800 only when the answerer can
 * do 1664 points (Table 15 NOTE); our ceiling; 2400 at least */
static int fine_rate(const p2call_t *p, int k, int snr)
{
    int fine = (int)((int64_t)(snr - 90) * p2_sr[k] * 16 / (30 * 2400));
    int top = p2_top[k], cap = p->info0a.c1664 ? 14 : 12;
    if (top > cap) top = cap;
    if (p->cfg.max_rate && top > p->cfg.max_rate / 2400) top = p->cfg.max_rate / 2400;
    if (top < 1) top = 1;
    if (fine > top * 16 + 15) fine = top * 16 + 15;
    return fine < 16 ? 16 : fine;
}

void p2call_decide(p2call_t *p)
{
    info1c_t o;
    int r, a, k, hi, pre, best = -1, best_r = 0, rmax = p->info0a.pwr_reduce ? 7 : 0;
    int share[PROBE_NBIN + 1];
    shares(p, share);
    memset(&o, 0, sizeof o);
    o.freq_off = p->info1c.freq_off;
    /* bits 12:14: the power reduction that gives our receiver most, over
     * the symbol rates on their carriers (none unless it gains; none if the
     * answerer cannot reduce) */
    for (r = 0; r <= rmax; r++) {
        int total = 0;
        for (k = 0; k < 6; k++) {
            int v = choose(p, k, r, share, 0, &hi, &pre);
            if (v >= 0) total += fine_rate(p, k, v);
        }
        if (total > best) { best = total; best_r = r; }
    }
    /* at that reduction: carrier, filter and projected rate per symbol rate */
    for (k = 0; k < 6; k++) {
        int v = choose(p, k, best_r, share, 1, &hi, &pre);
        p->band_snr[k] = v < 0 ? 0 : v;
        if (v < 0) continue;            /* "0 indicates the symbol rate cannot be used" */
        o.sr[k].hi_carrier = (uint8_t)hi;
        o.sr[k].preemph = (uint8_t)pre;
        o.sr[k].max_rate = (uint8_t)(fine_rate(p, k, v) >> 4);
    }
    /* bits 15:17: how much more our receiver stands without a projected
     * rate falling */
    for (a = 0; rmax > 0 && a < 7; a++) {
        int same = 1;
        for (k = 0; k < 6 && same; k++) {
            if (!o.sr[k].max_rate) continue;
            same = fine_rate(p, k, band(p->snr, p->rx_level, k, o.sr[k].hi_carrier, o.sr[k].preemph, best_r + a + 1,
                                        floor_db10(p, best_r + a + 1), share)) >> 4 == o.sr[k].max_rate;
        }
        if (!same) break;
    }
    o.min_pwr_red = (uint8_t)best_r;
    o.add_pwr_red = (uint8_t)a;
    o.md_len = 0;                       /* we send no MD */
    p->info1c = o;
}

/* the answerer's L2 is in: per-tone levels and SNRs, the distortion in
 * the four empty bins (it repeats with the probing signal, as a codec's
 * quantisation and an overload do: on 26 lines 150 Hz apart over 4 kHz),
 * the frequency offset */
static void analyse(p2call_t *p)
{
    int m;
    int64_t ptot = 0, gap = 0;
    for (m = 1; m <= PROBE_NBIN; m++) {
        int snr, lvl;
        int64_t pw;
        proberx_bin(&p->prx, m, &snr, &lvl, &pw);
        if (probe_has_tone(m)) {
            p->snr[m] = snr;
            p->rx_level[m] = lvl;
            ptot += pw;
        } else {
            p->snr[m] = 0; p->rx_level[m] = 0;
            gap += pw;
        }
    }
    p->sdr_db10 = gap > 0 ? probe_db10(ptot * 12, gap * 80) : 600;
    if (p->sdr_db10 > 600) p->sdr_db10 = 600;
    if (p->sdr_db10 < 0) p->sdr_db10 = 0;
    p->info1c.freq_off = (int16_t)proberx_freq_off(&p->prx);
}

/* INFO1a is in (Table 16): what our transmitter does. 0, or -1 when it asks
 * for what we cannot do or did not offer. */
static int take_info1a(p2call_t *p)
{
    const info1a_t *i = &p->info1a;
    int k = i->down_sr;                     /* bits 37:39: call -> answer */
    if (i->v90 || i->up_sr != i->down_sr || (k != 0 && k != 3 && k != 4)
        || !((p->cfg.sr_mask >> k) & 1) || p->info1c.sr[k].max_rate == 0) return -1;
    p->sym_idx = k; p->sym_rate = p2_sr[k];
    p->tx_hi_carrier = i->hi_carrier;
    p->tx_preemph = i->preemph > 10 ? 0 : i->preemph;
    p->tx_pwr_red = i->min_pwr_red;
    p->rx_hi_carrier = p->info1c.sr[k].hi_carrier;
    p->md_len = i->md_len;
    return 0;
}

/* the reversal heard at the line at `line`: ours goes out so that it is at
 * the line terminals 40 ms later (11.2.1.1.3, 11.2.1.1.6) */
static void reply_at(p2call_t *p, int32_t line, p2call_state s)
{
    p->act_at = line + TURN - DPSK_TX_DELAY - p->cfg.io_delay;
    go(p, s);
}

static int16_t p2call_tick_v90(p2call_t *p, int16_t x);

int16_t p2call_tick(p2call_t *p, int16_t x)
{
    int ev;
    int32_t out;
    if (p->v90) return p2call_tick_v90(p, x);
    ev = dpskrx_tick(&p->rx, x);

    switch (p->state) {
    case P2L_INFO0:
        if (ev & DPSK_EV_FRAME) {
            int again = p->got0a;
            info0a_parse(p->rx.frame, &p->info0a);
            p->got0a = 1;
            /* a repeated INFO0a without the acknowledge bit: the answerer has not got ours */
            p->resend = (again && !p->info0a.ack);
        }
        if (!dpsktx_busy(&p->tx)) {
            if (p->resend || (!p->got0a && p->rx.steady >= MS(50))) {
                send_info0c(p, p->got0a);           /* 11.2.2.1.1 */
                p->resend = 0;
            } else if (p->got0a && p->rx.steady >= TONE_SEEN) {
                go(p, P2L_WAIT_REV1);               /* 11.2.1.1.2: Tone B goes on */
            }
        }
        if (p->t > MS(T_INFO0_MS)) fail(p, p->got0a ? P2L_ERR_REV : P2L_ERR_INFO0);
        break;

    case P2L_WAIT_REV1:
        if (ev & DPSK_EV_FRAME) {                   /* a repeated INFO0a (11.2.2.1.1; 11.5.1.1) */
            /* back to 11.2.1.1.1, a restart: an answerer that never stops
             * sending INFO0a must not keep us here (spec D5) */
            if (p->restarts >= MAX_RESTARTS) { fail(p, P2L_ERR_REV); break; }
            p->restarts++;
            info0a_parse(p->rx.frame, &p->info0a);
            p->got0a = 1;
            p->resend = !p->info0a.ack;
            go(p, P2L_INFO0);
        } else if (ev & DPSK_EV_REVERSAL) {
            p->t_rev1 = p->now - DPSK_RX_REV_DELAY;
            reply_at(p, p->t_rev1, P2L_REPLY1);
        } else if (p->t > MS(T_REV1_MS)) {
            fail(p, P2L_ERR_REV);
        }
        break;

    case P2L_REPLY1:
        /* 11.2.1.1.3: our reversal, 10 ms more of Tone B, silence */
        if (p->now == p->act_at || (p->t == 0 && p->now > p->act_at)) {
            dpsktx_reverse(&p->tx);
            p->act_at = p->now;
            p->b_rev_line = p->now + DPSK_TX_DELAY;
        }
        if (p->t > 0 && p->now == p->act_at + TAIL) {
            dpsktx_off(&p->tx);
            go(p, P2L_WAIT_REV2);
        }
        break;

    case P2L_WAIT_REV2:
        if (ev & DPSK_EV_REVERSAL) {
            /* 11.2.1.1.4: the round trip; then L1 and L2 come */
            int32_t line = p->now - DPSK_RX_REV_DELAY;
            p->t_rev2 = line;
            p->rtd = line - p->b_rev_line - TURN - p->cfg.io_delay;
            if (p->rtd < 0) p->rtd = 0;
            p->rev2_at = line;
            p->probe_from = line + TAIL + MS(160) + MS(10);     /* 11.2.1.1.5: L2 only */
            proberx_init(&p->prx);
            go(p, P2L_PROBE_RX);
        } else if (p->now - p->t_rev1 > MS(T_REV2_MS)) {
            if (p->restarts >= MAX_RESTARTS) fail(p, P2L_ERR_REV);
            else { p->restarts++; go(p, P2L_RECOVER); }         /* 11.2.2.1.3 */
        }
        break;

    case P2L_RECOVER:
        /* silent until Tone A, then Tone B and its reversal again */
        if (p->rx.steady >= TONE_SEEN) { dpsktx_on(&p->tx); go(p, P2L_WAIT_REV1); }
        else if (p->t > MS(T_RECOVER_MS)) fail(p, P2L_ERR_REV);
        break;

    case P2L_PROBE_RX:
        if (p->now >= p->probe_from) {
            proberx_tick(&p->prx, x);
            if (p->prx.nblk >= PROBE_BLOCKS_CALL) {
                analyse(p);
                p2call_decide(p);
                dpsktx_on(&p->tx);                  /* 11.2.1.1.5: Tone B */
                go(p, P2L_WAIT_REV3);
            }
        }
        break;

    case P2L_WAIT_REV3:
        if (p->t_tone_a2 < 0 && p->rx.steady >= TONE_SEEN) p->t_tone_a2 = p->now - TONE_SEEN;
        if (ev & DPSK_EV_REVERSAL) {
            p->t_rev3 = p->now - DPSK_RX_REV_DELAY;
            reply_at(p, p->t_rev3, P2L_REPLY3);     /* 11.2.1.1.6 */
        } else if (p->now - p->rev2_at > MS(T_REV3_MS) + p->rtd) {
            /* 11.2.2.1.4: wait 40 ms, then the reversal all the same */
            p->rev3_timeouts++;
            p->act_at = p->now + TURN;
            go(p, P2L_REPLY3);
        }
        break;

    case P2L_REPLY3:
        if (p->now == p->act_at || (p->t == 0 && p->now > p->act_at)) { dpsktx_reverse(&p->tx); p->act_at = p->now; }
        if (p->t > 0 && p->now == p->act_at + TAIL) dpsktx_off(&p->tx);
        if (p->t > 0 && p->now == p->act_at + TAIL + DPSK_TX_DELAY) {
            probetx_init(&p->ptx, p->cfg.level);
            proberx_init(&p->prx);
            p->tone_blocks = 0;
            p->probing = 1;
            go(p, P2L_L1);
        }
        break;

    case P2L_L1:
        (void)tone_under_probe(&p->prx, &p->tone_blocks, x, 16);
        if (p->t >= MS(160) - 1) { p->probing = 2; p->tone_blocks = 0; p->l2_at = p->now; go(p, P2L_L2); }
        break;

    case P2L_L2:
        if (tone_under_probe(&p->prx, &p->tone_blocks, x, 16)) {           /* 11.2.1.1.7 */
            uint8_t bits[INFO_MAX_BITS];
            int n;
            p->probing = 0;
            dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO1A_BITS));   /* INFO1a: Table 16, 70 bits */
            n = info1c_build(&p->info1c, bits);
            dpsktx_send(&p->tx, bits, n);
            go(p, P2L_INFO1C);
        } else if (p->t > MS(T_L2_MS) + p->rtd) {
            self_retrain(p, P2L_ERR_INFO1A);        /* 11.2.2.1.5 */
        }
        break;

    case P2L_INFO1C:
        if (dpsktx_busy(&p->tx)) p->t = 0;
        else if (p->t == TX_FLUSH) { dpsktx_off(&p->tx); p->info1c_end = p->now; go(p, P2L_WAIT_INFO1A); }
        break;

    case P2L_WAIT_INFO1A:
        if (ev & DPSK_EV_FRAME) {                   /* 11.2.1.1.8 */
            info1a_parse(p->rx.frame, &p->info1a);
            p->t_info1a = p->now;
            if (take_info1a(p) != 0) fail(p, P2L_ERR_NO_RATE);
            else go(p, P2L_DONE);
        } else if (p->t > MS(T_INFO1A_MS) + p->rtd) {
            /* 11.2.2.1.6: Tone A is the answerer's retrain, answered
             * (11.5.1.2); nothing, or INFOMARKSa (which we do not tell from
             * other signals): we begin one (11.5.1.1) */
            if (p->rx.steady >= MS(50)) self_retrain(p, P2L_ERR_INFO1A);
            else if (p->t > MS(T_INFO1A_MS + T_TONE_A_MS) + p->rtd) self_retrain(p, P2L_ERR_INFO1A);
        }
        break;

    case P2L_RT_SILENCE:
        if (p->t >= MS(70)) {                       /* 11.5.1: 70 ms of silence, Tone B */
            dpsktx_on(&p->tx);
            dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));
            p->restarts = 0;
            go(p, P2L_WAIT_REV1);
        }
        break;

    case P2L_DONE:
    case P2L_FAILED:
    default:                    /* the V.90 states: p2call_tick_v90 */
        break;
    }

    out = dpsktx_tick(&p->tx);
    if (p->probing) out += probetx_tick(&p->ptx, p->probing == 1);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    p->now++; p->t++;
    return (int16_t)out;
}

/* ---- V.90 Phase 2, analogue modem (9.2.2) ---- */

#define T_A_INFO0_MS  8000      /* INFO0d and Tone B (none given) */
#define T_A_REV1_MS   2000      /* 9.2.2.2.2 */
#define T_A_L2_MS      600      /* 9.2.2.2.3, plus a round trip, from the start of L2 */
#define T_A_REV2_MS   3000      /* none given: the digital modem's L1 / L2 analysis is <= 660 ms */
#define T_A_INFO1D_MS 2000      /* 9.2.2.2.4, plus two round trips */
#define TONE_B_BIN       8      /* 1200 Hz in the probe analyser's 150 Hz bins */

static void send_info0a_v90(p2call_t *p, int ack)
{
    send_info0(p, ack, p->cfg.v92);     /* Table 8/V.90 is Table 14/V.34; V.92: Table 16 */
    p->tone_since = -1;
}

void p2call_init_v90(p2call_t *p, const p2call_cfg *cfg)
{
    /* 9.2.2.1.1: INFO0a after the 75 ms of silence that end Phase 1, then
     * Tone A; the digital modem's INFO0d and Tone B are listened for */
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    if (!p->cfg.sr_mask) p->cfg.sr_mask = P2N_SR_3000 | P2N_SR_3200;
    p->v90 = 1;
    p->state = P2L_A_INFO0; p->error = P2L_OK;
    dpsktx_init(&p->tx, DPSK_FC_A, cfg->level, 1);
    dpskrx_init(&p->rx, DPSK_FC_B);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0D_BITS));
    probetx_init(&p->ptx, cfg->level);
    proberx_init(&p->prx);
    p->t_rev1 = p->t_rev2 = p->t_rev3 = p->t_tone_a2 = p->t_info1a = -1;
    send_info0a_v90(p, 0);
}

void p2call_init_v90_retrain(p2call_t *p, const p2call_cfg *cfg, const info0_t *info0d)
{
    /* 9.5.2 (V.92 9.7.2): after the 70 ms of silence, Tone A; no INFO0
     * exchange (the digital modem's p2d_init_retrain sends none either; 9.7
     * of V.92 asks for "the full Phase 2", which is full against the short
     * Phase 2 of 9.4, entered at the tones as V.90's 9.5 enters it, and no
     * real caller of the recordings sent INFO0a in a retrain), so the first
     * Phase 2's INFO0d holds. Then 9.2.2.1.3 on: our reversal once Tone B
     * is heard and Tone A has run 50 ms. A digital modem that sends INFO0d
     * all the same gets our INFO0a (P2L_A_INFO0: an INFO0d without the
     * acknowledge bit is one that has not heard ours) and its INFO0d is
     * the one that holds. */
    p2call_init_v90(p, cfg);
    dpsktx_init(&p->tx, DPSK_FC_A, cfg->level, 1);     /* drop the INFO0a queued */
    p->n_info0c = 0;
    p->info0d = *info0d;
    p->got0d = 1;
    dpsktx_on(&p->tx);
    p->tone_since = 0;
}

/* INFO1d is in: the upstream symbol rate of INFO1a (3000 or 3200, the one
 * INFO1d projects the higher rate for; 3200 on a tie), and what INFO1d asks
 * of our transmitter at it. With V.92 on both sides and PCM upstream
 * allowed (INFO0d bit 27, INFO1d bit 70 of Table 17, which info1d_parse
 * puts where the 3429 carrier bit was), INFO1a of Table 18/V.92 instead:
 * our precoder and prefilter have all four sections and take the longest
 * filters there are (Ltot 384, Lmax 320), and no MD is sent (the V.92
 * analogue modem's canceller learns on TRN1u). -1 when it offers nothing we
 * can send. */
static int take_info1d(p2call_t *p)
{
    const info1d_t *d = &p->info1d;
    int k, best = -1;
    p->pcm_up = p->cfg.v92 && p->info0d.v92 && d->sr[5].hi_carrier;
    for (k = 3; k <= 4; k++) {
        if (!((p->cfg.sr_mask >> k) & 1) || d->sr[k].max_rate == 0) continue;
        if (best < 0 || d->sr[k].max_rate >= d->sr[best].max_rate) best = k;
    }
    memset(&p->info1a_v90, 0, sizeof p->info1a_v90);
    p->info1a_v90.v90 = 1;
    p->info1a_v90.md_len = (uint8_t)(p->cfg.md_len & 127);
    p->info1a_v90.uinfo = (uint8_t)(p->cfg.uinfo & 127);
    if (p->pcm_up) {
        p->info1a_v90.pcm = 1;
        p->info1a_v90.md_len = 0;           /* no MD: the canceller learns on TRN1u */
        p->info1a_v90.up_sr = 6;
        p->info1a_v90.down_sr = 6;
        p->info1a_v90.sections = 3;
        p->info1a_v90.ltot_code = 3;
        p->info1a_v90.lmax_code = 3;
        return 0;
    }
    if (best < 0) return -1;
    p->sym_idx = best; p->sym_rate = p2_sr[best];
    p->tx_hi_carrier = d->sr[best].hi_carrier;
    p->tx_preemph = d->sr[best].preemph > 10 ? 0 : d->sr[best].preemph;
    p->tx_pwr_red = d->min_pwr_red;
    p->md_len = d->md_len;
    p->info1a_v90.up_sr = (uint8_t)best;
    p->info1a_v90.down_sr = 6;
    p->info1a_v90.freq_off = p->info1c.freq_off;
    return 0;
}

/* V.92: our clock against the network's from the digital modem's L2. Each
 * of its tones, at 150 m Hz on the network's clock, turns from one 20 ms
 * block to the next by -2 pi 3 m e on ours (e the offset), small enough
 * (0.05 rad at 100 ppm, m = 25) that the turn is ci / cr; weighted by cr, the
 * least squares slope over m is e = -sum m ci / (6 pi sum m^2 cr). Tones with
 * an SNR under 20 dB are left out. 1/256 ppm, within +-1000 ppm; 0 when
 * there is nothing to go on. */
int32_t p2call_clock_q8(const proberx_t *r)
{
    int64_t num = 0, den = 0, q;
    int m;
    for (m = 1; m <= PROBE_NBIN; m++) {
        if (!probe_has_tone(m) || proberx_snr(r, m) < 200 || r->cr[m] <= 0) continue;
        num += (int64_t)m * (r->ci[m] >> 8);
        den += (int64_t)m * m * (r->cr[m] >> 8);
    }
    if (den <= 0) return 0;
    while (den > 0x7fffffff) { den >>= 1; num /= 2; }
    if (den == 0) return 0;
    /* past 20 times den the clamp holds whatever the product (20 * 13581342
     * > 256000), and below it the product fits 64 bits: |num| <= 20 * 2^31 */
    if (num > den * 20) return -256000;
    if (num < -den * 20) return 256000;
    /* 256e6 / (6 pi) = 13581342 */
    q = -(num * 13581342) / den;
    return (int32_t)(q > 256000 ? 256000 : q < -256000 ? -256000 : q);
}

static int16_t p2call_tick_v90(p2call_t *p, int16_t x)
{
    int ev = dpskrx_tick(&p->rx, x);
    int32_t out;

    switch (p->state) {
    case P2L_A_INFO0:
        if (ev & DPSK_EV_FRAME) {
            int again = p->got0d;
            info0d_parse(p->rx.frame, &p->info0d);
            p->got0d = 1;
            /* a repeated INFO0d without the acknowledge bit: ours was not heard */
            p->resend = again && !p->info0d.ack;
        }
        if (!dpsktx_busy(&p->tx)) {
            if (p->tone_since < 0) p->tone_since = p->now;
            if (p->resend || (!p->got0d && p->rx.steady >= MS(50))) {
                send_info0a_v90(p, p->got0d);           /* 9.2.2.2.1 */
                p->resend = 0;
            } else if (p->got0d && p->rx.steady >= TONE_SEEN && p->now - p->tone_since >= MS(50)) {
                /* 9.2.2.1.3: Tone B, and Tone A for 50 ms: our reversal */
                dpsktx_reverse(&p->tx);
                p->a_rev_line = p->now + DPSK_TX_DELAY + p->cfg.io_delay / 2;
                go(p, P2L_A_WAIT_REV1);
            }
        }
        if (p->t > MS(T_A_INFO0_MS)) fail(p, p->got0d ? P2L_ERR_REV : P2L_ERR_INFO0);
        break;

    case P2L_A_WAIT_REV1:
        if (ev & DPSK_EV_FRAME) {                       /* a repeated INFO0d (9.2.2.2.1) */
            /* back to 9.2.2.1.3, a restart, as the time-out below */
            if (p->restarts >= MAX_RESTARTS) { fail(p, P2L_ERR_REV); break; }
            p->restarts++;
            info0d_parse(p->rx.frame, &p->info0d);
            p->resend = !p->info0d.ack;
            p->tone_since = p->now;
            go(p, P2L_A_INFO0);
        } else if (ev & DPSK_EV_REVERSAL) {
            /* 9.2.2.1.4, .5: RTDEa, and our reversal 40 ms after theirs at the line */
            int32_t line = p->now - DPSK_RX_REV_DELAY - (p->cfg.io_delay - p->cfg.io_delay / 2);
            p->t_rev1 = line;
            p->rtd = line - p->a_rev_line - TURN;
            if (p->rtd < 0) p->rtd = 0;
            p->act_at = line + TURN - DPSK_TX_DELAY - p->cfg.io_delay / 2;
            go(p, P2L_A_REPLY);
        } else if (p->t > MS(T_A_REV1_MS)) {
            /* 9.2.2.2.2: listen for Tone B again, then 9.2.2.1.3 */
            if (p->restarts >= MAX_RESTARTS) { fail(p, P2L_ERR_REV); break; }
            p->restarts++;
            p->tone_since = p->now;
            go(p, P2L_A_INFO0);
        }
        break;

    case P2L_A_REPLY:
        if (p->now == p->act_at || (p->t == 0 && p->now > p->act_at)) { dpsktx_reverse(&p->tx); p->act_at = p->now; }
        if (p->t > 0 && p->now == p->act_at + TAIL) dpsktx_off(&p->tx);
        if (p->t > 0 && p->now == p->act_at + TAIL + DPSK_TX_DELAY) {
            probetx_init(&p->ptx, p->cfg.level);
            proberx_init(&p->prx);
            p->tone_blocks = 0;
            p->probing = 1;
            go(p, P2L_A_L1);
        }
        break;

    case P2L_A_L1:
        (void)tone_under_probe(&p->prx, &p->tone_blocks, x, TONE_B_BIN);
        if (p->t >= MS(160) - 1) { p->probing = 2; p->tone_blocks = 0; p->l2_from = p->now; go(p, P2L_A_L2); }
        break;

    case P2L_A_L2:
        if (tone_under_probe(&p->prx, &p->tone_blocks, x, TONE_B_BIN)) {
            /* 9.2.2.1.6: Tone A 50 ms, its reversal, 10 ms more */
            p->probing = 0;
            dpsktx_on(&p->tx);
            go(p, P2L_A_TONE50);
        } else if (p->t > MS(T_A_L2_MS) + p->rtd) {
            /* 9.2.2.2.3: Tone A, and 9.2.2.1.3 again */
            p->probing = 0;
            if (p->restarts >= MAX_RESTARTS) { fail(p, P2L_ERR_INFO1A); break; }
            p->restarts++;
            dpsktx_on(&p->tx);
            p->tone_since = p->now;
            go(p, P2L_A_INFO0);
        }
        break;

    case P2L_A_TONE50:
        if (p->t == MS(50) + DPSK_TX_DELAY) { dpsktx_reverse(&p->tx); p->act_at = p->now; }
        if (p->t == MS(50) + DPSK_TX_DELAY + TAIL) { dpsktx_off(&p->tx); go(p, P2L_A_WAIT_REV2); }
        break;

    case P2L_A_WAIT_REV2:
        if (ev & DPSK_EV_REVERSAL) {
            /* 9.2.2.1.7, .8: L1 160 ms, then L2 (analysed 280 ms; at most 500 ms) */
            int32_t line = p->now - DPSK_RX_REV_DELAY;
            p->t_rev2 = line;
            p->probe_from = line + TAIL + MS(160) + MS(10);
            proberx_init(&p->prx);
            go(p, P2L_A_PROBE_RX);
        } else if (p->t > MS(T_A_REV2_MS)) {
            fail(p, P2L_ERR_REV);
        }
        break;

    case P2L_A_PROBE_RX:
        if (p->now >= p->probe_from) {
            proberx_tick(&p->prx, x);
            if (p->prx.nblk >= PROBE_BLOCKS_CALL) {
                analyse(p);                             /* levels, SNRs, frequency offset */
                p->clk_ppm_q8 = p2call_clock_q8(&p->prx); /* (V.92's transmitter needs it, 6.2/V.92) */
                dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO1D_BITS));
                dpsktx_on(&p->tx);                      /* Tone A; INFO1d comes */
                go(p, P2L_A_WAIT_INFO1D);
            }
        } else if (p->t > MS(1000)) {
            fail(p, P2L_ERR_REV);
        }
        break;

    case P2L_A_WAIT_INFO1D:
        if (ev & DPSK_EV_FRAME) {
            uint8_t bits[INFO_MAX_BITS];
            int n;
            info1d_parse(p->rx.frame, &p->info1d);
            if (take_info1d(p) != 0) { fail(p, P2L_ERR_NO_RATE); break; }
            n = info1a_build(&p->info1a_v90, bits);
            dpsktx_send(&p->tx, bits, n);
            p->t_info1a = p->now;
            go(p, P2L_A_INFO1A);
        } else if (p->t > MS(T_A_INFO1D_MS) + 2 * p->rtd) {
            fail(p, P2L_ERR_INFO1A);                    /* 9.2.2.2.4: a retrain is not done here */
        }
        break;

    case P2L_A_INFO1A:
        if (dpsktx_busy(&p->tx)) p->t = 0;
        else if (p->t == TX_FLUSH) { dpsktx_off(&p->tx); p->info1c_end = p->now; go(p, P2L_DONE); }
        break;

    default:
        break;
    }

    out = dpsktx_tick(&p->tx);
    if (p->probing) out += probetx_tick(&p->ptx, p->probing == 1);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    p->now++; p->t++;
    return (int16_t)out;
}
