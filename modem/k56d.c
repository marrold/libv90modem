/* K56flex as its server, after our flagged INFO1a. See k56d.h.
 *
 * What the client sends, and when, is docs/history/k56-facts.md ("Upstream
 * and J24", "Phase 4"): D-Modem's reading of Motorola's SM56
 * (https://github.com/D-Modem/D-Modem at c06a380d, its notes on K56flex).
 * Facts only; no code of D-Modem. The transmit side is k56tx and k56enc;
 * the receiver is x2d's and v34dx's (v34up): the client is V.34's call
 * modem, so its scrambler is GPC. */
#include <string.h>
#ifdef K56D_TRACE
#include <stdio.h>
#define TR(...) fprintf(stderr, __VA_ARGS__)
#else
#define TR(...) ((void)0)
#endif
#include "k56d.h"
#include "clock.h"
#include "p2ans.h"
#include "../v90/v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)

#define SYM_RATE       3200     /* the upstream's ("Phase 2"; p2ans flags INFO1a at 3200 only) */
#define TRN_KNOWN      512      /* symbols of the client's TRN used as a known reference (as p34d) */
#define E_ONES         20
#define MP_MASK        0x3fff   /* our MP's rate mask ("Phase 4": D-Modem's server's) */

/* our echo canceller trains on our TRN1A to TRN1C, while the client is
 * silent ("Upstream and J24": until it has our Js) */
#define EC_SETTLE      80       /* samples of TRN1A before the canceller looks at it */
#define EC_FAST        3000     /* samples at full NLMS step (as p34d) */

enum { RX_OFF, RX_WAIT_S, RX_KNOWN, RX_J, RX_P4, RX_DATA, RX_SRES, RX_RN, RX_IDLE };

static void end(k56d_t *d, int err)
{
    TR("K56D %.3f end %d (state %d, rxmode %d)\n", d->now / 8000.0, err, (int)d->state, d->rxmode);
    d->state = K56D_ENDED; d->error = err; d->t = 0;
    d->tx_ready = d->up.rx_ready = 0;
    d->rxmode = RX_IDLE;
}

static void enter(k56d_t *d, k56d_state s) { d->state = s; d->t = 0; }

int k56d_init(k56d_t *d, const k56d_cfg *cfg)
{
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    if (d->cfg.echo_delay > K56D_MAX_ECHO_DELAY) d->cfg.echo_delay = K56D_MAX_ECHO_DELAY;
    k56tx_init(&d->tx);
    ec_init(&d->ec); d->ec_mu = -1;
    ecdly_init(&d->ecd, d->cfg.echo_delay < 0 ? 0 : d->cfg.echo_delay);
    ecdly_near(&d->ecd, d->cfg.rtd);
    v34_carrier(4, cfg->hi_carrier != 0, &d->car_num, &d->car_den);
    sdet_init(&d->sdet, SYM_RATE, d->car_num, d->car_den);
    d->sdet.div = 16;               /* nothing but S can be on the line */
    qamrx_init(&d->up.rx, SYM_RATE, d->car_num, d->car_den);
    d->rxmode = RX_OFF;
    d->t_mark = CLK_NONE;
    d->state = K56D_TRAIN1;
    if (d->cfg.rtd < 0 || d->cfg.max_up_rate < 0 || d->cfg.max_down_rate < 0) { end(d, K56D_ERR_CFG); return -1; }
    return 0;
}

int k56d_down_rate(const k56d_t *d) { return d->state == K56D_DATA || d->state == K56D_RENEG ? d->down_rate : 0; }
int k56d_up_rate(const k56d_t *d) { return d->state == K56D_DATA || d->state == K56D_RENEG ? d->up_rate : 0; }
uint8_t k56d_rbs(const k56d_t *d) { return d->rbs; }
int k56d_pad(const k56d_t *d) { return d->pad; }

int k56d_cleardown(k56d_t *d)
{
    if (d->state != K56D_DATA && d->state != K56D_RENEG) return -1;
    end(d, K56D_ERR_HANGUP);
    return 0;
}

/* ---- the client's MP ---- */

/* our V.34 receiver at 3200 baud, with what our MP asks of the client's
 * transmitter (decision 9): 16 states, minimum shaping, linear; GPC. With
 * up_hyp 1, x2's configuration instead (64 states, non-linear, expanded
 * shaping; still GPC): a client that ignores our MP (docs/answering.md) */
static void up_cfg(v34_cfg_t *c, int rate, int hyp)
{
    memset(c, 0, sizeof *c);
    c->symbol_rate = SYM_RATE; c->data_rate = (uint16_t)rate;
    c->trellis = V34_TRELLIS_16; c->expanded = 0; c->nonlinear = 0; c->scrambler = V34_GPC;
    if (hyp) { c->trellis = V34_TRELLIS_64; c->expanded = 1; c->nonlinear = 1; }
}

/* the highest rate of the client's mask (bit n - 1 for n x 2400 bit/s), at
 * most its rate_ca and our max_up_rate, that we receive */
static int pick_up_rate(k56d_t *d, const v34_mp_t *m)
{
    int n;
    for (n = 14; n >= 1; n--) {
        v34_cfg_t c;
        if (!((m->rates >> (n - 1)) & 1) || n > m->max_ca) continue;
        if (d->cfg.max_up_rate && n * 2400 > d->cfg.max_up_rate) continue;
        up_cfg(&c, n * 2400, 0);
        if (v34_params_init(&d->up.par, &c) == 0) return n * 2400;   /* (set again at its E) */
    }
    return 0;
}

/* the downstream rate code: the client's, our ceiling's, 13 at most */
static int pick_rate(const k56d_t *d, int asked)
{
    int r = asked > 13 ? 13 : asked;
    if (d->cfg.max_down_rate > 0) {
        int cap = (d->cfg.max_down_rate - 30000) / 2000;
        if (cap < 1) cap = 1;           /* below 32000: 32000, the lowest there is */
        if (r > cap) r = cap;
    }
    return r;
}

/* an MP that checks out, twice in a row (MP and MP' differ in the
 * acknowledge bit only): what TRN looks like through the differential
 * decoder passes the CRC once in 65536 tries (as v34dx) */
static void on_mp(k56d_t *d, const v34_mp_t *m)
{
    v34_mp_t a = *m, b = d->cand;
    a.ack = b.ack = 0;
    if (!d->cand_ok || memcmp(&a, &b, sizeof a) != 0) {
        d->cand = *m; d->cand_ok = 1;
        return;
    }
    if (d->got_mp) return;
    TR("K56D %.3f MP rate_ca %d rate_ac %d mask %04X\n", d->now / 8000.0, m->max_ca, m->max_ac, m->rates);
    d->far_mp = *m;
    /* in a renegotiation, rate 0 both ways is V.34's cleardown (11.7; "Rate
     * renegotiation") */
    if (d->state == K56D_RENEG && !m->max_ac && !m->max_ca) { end(d, K56D_ERR_CLEARDOWN); return; }
    /* the client clears down unless all three are non-zero ("Phase 4");
     * we cannot go on either */
    if (!m->max_ac || !m->rates) { end(d, K56D_ERR_RATE); return; }
    d->up_rate = pick_up_rate(d, m);
    if (!d->up_rate) { end(d, K56D_ERR_RATE); return; }
    d->rate = pick_rate(d, m->max_ac);
    d->got_mp = 1;
    memset(&d->mp, 0, sizeof d->mp);
    d->mp.type = 0;
    d->mp.max_ca = (uint8_t)(d->up_rate / 2400);
    d->mp.max_ac = (uint8_t)d->rate;
    d->mp.trellis = V34_TRELLIS_16; d->mp.nonlinear = 0; d->mp.shaping = 0;
    d->mp.rates = MP_MASK;
    d->mp.asym = 1;
    k56tx_data(&d->tx, d->rate, d->pad, d->rbs, d->cfg.getbit, d->cfg.user);
    k56tx_set_mp(&d->tx, &d->mp);
}

/* ---- upstream data mode ---- */

static void put_data(void *user, int bit)
{
    k56d_t *d = (k56d_t *)user;
    if (d->up.rx_ready && d->cfg.putbit) d->cfg.putbit(d->cfg.user, bit);
}

static void s_drain(k56d_t *d);

/* its E has just ended: B1 starts with the next symbol, under the
 * hypothesis that held last (a renegotiation's client is the same one);
 * its points are kept until B1 is decided */
static void rx_begin_data(k56d_t *d)
{
    v34_cfg_t c;
    up_cfg(&c, d->up_rate, d->up_hyp);
    if (v34up_data_begin(&d->up, &c, put_data, d) != 0) { end(d, K56D_ERR_RATE); return; }
    d->up_retried = d->catchup = d->catch_odd = d->ring_full = 0;
    d->up.rx_ready = 0;                                         /* (a drain to its last S given up) */
    memcpy(d->eq_cr, d->up.rx.cr, sizeof d->eq_cr); memcpy(d->eq_ci, d->up.rx.ci, sizeof d->eq_ci);
    d->ring_w = d->ring_r = d->ring_fill = 0;
    d->rxmode = RX_DATA;
    d->t_mark = d->now;
    sdet_init(&d->sdet, SYM_RATE, d->car_num, d->car_den);     /* its S from here on: a renegotiation */
    tonedet_init(&d->tone_b, TONEDET_1200);                     /* its tone B: a retrain */
}

/* ---- descrambled bits ---- */

/* Phase 3: the J24, the J word and an octet, round and round: two equal J
 * words 24 bits apart, the octet the 8 bits between them. Then J', 24 bits
 * after the last J word ("Upstream and J24"). */
static void j_bit(k56d_t *d, int bit)
{
    uint16_t w;
    d->jsr = (d->jsr >> 1) | ((uint32_t)bit << 31); d->jbits++;
    w = (uint16_t)(d->jsr >> 16);
    if (d->got_j) {
        if (d->jbits - d->j_at < 24) return;
        if (w == word16(v34_j16)) { d->j_at = d->jbits; return; }
        if (w == word16(v34_jprime) && (d->jbits - d->j_at) % 24 == 0) {
            /* TRN, MP, MP' and E on the 16 points our Js asked for */
            TR("K56D %.3f J'\n", d->now / 8000.0);
            d->got_jp = 1;
            v34_tdem_set_points(&d->up.tdem, 16);
            d->fn = d->sh.ones = 0;
            d->rxmode = RX_P4;
        }
        return;
    }
    if (w != word16(v34_j16)) return;
    if (d->j_seen && d->jbits - d->j_at == 24) {
        d->j_octet = (uint8_t)(d->jsr >> 8);
        d->got_j = 1;
        TR("K56D %.3f J24 octet %02X\n", d->now / 8000.0, d->j_octet);
    }
    d->j_seen = 1; d->j_at = d->jbits;
}

/* Phase 4: MP and MP' (frame sync 17 ones, a start bit, ...) and E (20 ones) */
static void seq_try(k56d_t *d)
{
    v34_mp_t m;
    memset(&m, 0, sizeof m);
    if (d->fn < V34_MP0_BITS - 3) return;
    if (v34_mp_parse(d->fbuf, d->fn, &m) > 0) { d->fn = 0; on_mp(d, &m); }
}

static void seq_bit(k56d_t *d, int bit)
{
    if (d->fn > 0 && d->fn < K56D_FBUF) d->fbuf[d->fn++] = (uint8_t)bit;
    int sync = seqhunt_bit(&d->sh, bit);
    if (bit) {
        /* inside an MP a start bit breaks every run at 17 */
        if (d->sh.ones == E_ONES && d->got_mp && !d->got_e) {
            d->got_e = 1;
            TR("K56D %.3f E\n", d->now / 8000.0);
            rx_begin_data(d);
        }
        return;
    }
    if (sync) {
        if (d->fn > 18) { d->fn -= 18; seq_try(d); if (d->state == K56D_ENDED) return; }
        memset(d->fbuf, 1, 17); d->fbuf[17] = 0; d->fn = 18;
    } else if (d->fn > 0 && (d->fn % 17) == 0) {
        seq_try(d);
        if (d->state == K56D_ENDED) return;
    }
    if (d->fn >= K56D_FBUF) d->fn = 0;
}

/* The client's S in data mode: its renegotiation ("Rate renegotiation").
 * Our data stops at the end of the frame being sent and R2 follows
 * (k56tx_reneg); its S-bar, TRN, MP and MP' and E as V.34's renegotiation
 * (11.6); our answer once its MP is in (on_mp), its E, then data both ways.
 * The carrier is locked: S is believed at once (as x2d). */
static void reneg_begin(k56d_t *d)
{
    /* the frames its V.34 decoder still holds from before S are data: they
     * go on now, but for a frame of margin (as p34d's s_heard). Called on
     * each sample from S heard on: an S too new in the decoder is waited
     * for (SDET_RUN_WAIT). Catching up, the decoder is behind the line (its
     * newest input is a stored point, S is still in the ring): the stored
     * points go on into it at the same pace during S and S-bar, up to where
     * S begins (s_drain). B1 not passed: nothing of it is data */
    if (d->catchup) {
        d->catchup = K56D_DRAIN;
    } else if (!v34up_s_flush(&d->up, &d->sdet)) {
        return;
    }
    d->tx_ready = 0;
    tonedet_init(&d->tone_b, TONEDET_1200);                     /* its tone B here too: a retrain */
    d->got_mp = d->got_e = 0; d->cand_ok = 0;
    d->fn = d->sh.ones = 0;
    sres_resume(&d->sres);
    d->rxmode = RX_SRES;
    k56tx_reneg(&d->tx);
    TR("K56D %.3f S in data mode\n", d->now / 8000.0);
    enter(d, K56D_RENEG);
}

/* ---- one received symbol ---- */

/* ---- B1, and a client that ignores our MP (docs/answering.md; D12) ----
 *
 * Our MP asks the client's transmitter for 16 states, linear, minimum
 * shaping, and nothing in-band says what it will send. A client may send
 * x2's upstream regardless (64 states, non-linear, expanded shaping). Its
 * points from E on are kept (ring) until B1 is decided; if B1 fails under
 * the hypothesis we started with, the receiver starts again under the other
 * (v34up_retry), the equaliser back as it was at E (B1 under the wrong one
 * pulled it), and decodes the stored points (v34up_replay_sym), 3 every
 * 2 symbol periods (a 10 ms block of the box stays under 10 ms of its CPU:
 * docs/limits.md), the live ones joining the queue, the equaliser left as
 * it is meanwhile. B1 passes: the data goes on under it. It fails too: K56D_ERR_B1, as before. A call whose
 * B1 passes at once is decoded exactly as without the ring. */

static void ring_put(k56d_t *d, v34_pt_t p)
{
    if (d->ring_fill >= K56D_RING) { d->ring_full = 1; return; }
    d->ring[d->ring_w][0] = p.re; d->ring[d->ring_w][1] = p.im;
    if (++d->ring_w == K56D_RING) d->ring_w = 0;
    d->ring_fill++;
}

/* B1 is in (vrx.b1_bits): 1/8 of its bits wrong at most (all ones) */
static int b1_ok(const k56d_t *d) { return d->up.vrx.b1_errors * 8 <= d->up.vrx.b1_bits; }

/* the oldest stored point into the receiver: 1 when B1 is then in and not
 * decided (v34up_replay_sym) */
static int ring_pop_put(k56d_t *d)
{
    int16_t re = d->ring[d->ring_r][0], im = d->ring[d->ring_r][1];
    if (++d->ring_r == K56D_RING) d->ring_r = 0;
    d->ring_fill--;
    return v34up_replay_sym(&d->up, re, im);
}

/* B1 is in: it passes (as b1_ok). Else the
 * other hypothesis once, on what the ring holds. -1: ended */
static int b1_check(k56d_t *d)
{
    int32_t g0 = d->up.gain, g1;
    uint16_t i, k;
    v34_cfg_t c;
    TR("K56D %.3f B1 %d of %d bits wrong\n", d->now / 8000.0, (int)d->up.vrx.b1_errors, (int)d->up.vrx.b1_bits);
    if (b1_ok(d)) { d->up.rx_ready = 1; return 0; }
    /* (the ring keeps one place for the next symbol) */
    if (d->up_retried || d->ring_full || d->ring_fill >= K56D_RING) { end(d, K56D_ERR_B1); return -1; }
    d->up_retried = 1;
    d->up_hyp ^= 1;
    up_cfg(&c, d->up_rate, d->up_hyp);
    if (v34up_retry(&d->up, &c) != 0) { end(d, K56D_ERR_B1); return -1; }
    memcpy(d->up.rx.cr, d->eq_cr, sizeof d->eq_cr); memcpy(d->up.rx.ci, d->eq_ci, sizeof d->eq_ci);
    g1 = d->up.gain;
    /* the stored points to the new scale: the same received signal */
    for (i = 0, k = d->ring_r; i < d->ring_fill; i++) {
        d->ring[k][0] = clip16((int32_t)(((int64_t)d->ring[k][0] * g0) / g1));
        d->ring[k][1] = clip16((int32_t)(((int64_t)d->ring[k][1] * g0) / g1));
        if (++k == K56D_RING) k = 0;
    }
    d->catchup = 1;
    TR("K56D %.3f k56 upstream: hypothesis %c\n", d->now / 8000.0, d->up_hyp ? 'B' : 'A');
    return 0;
}

/* 1 and 2 stored points in turn (3 every 2 symbol periods; the live one
 * joins the queue each period, so it shrinks by 1 every 2) into the
 * receiver, B1 checked as it completes; done when the ring is empty (the
 * ring never fills: one in, at least one out). -1: ended */
static int catch_up(k56d_t *d)
{
    int k, n = d->catch_odd ? 2 : 1;
    d->catch_odd ^= 1;
    for (k = 0; k < n && d->ring_fill; k++) {
        if (ring_pop_put(d) && b1_check(d) != 0) return -1;
    }
    if (!d->ring_fill) d->catchup = 0;
    return 0;
}

/* Its S came while catching up (reneg_begin): one symbol period of the
 * drain, 1 and 2 stored points in turn as catch_up, B1 checked as it
 * completes (failed: nothing of it is data), until the decoder's newest
 * inputs are a run of S_DRAIN_RUN of S (a run that long is not data), or
 * the ring is empty; then the frames before S go on but for the margin
 * (v34_rx_flush_before, as reneg_begin), the rest is dropped */
#define S_DRAIN_RUN 8
static void s_drain(k56d_t *d)
{
    unsigned run = 0;
    int k, n = d->catch_odd ? 2 : 1;
    d->catch_odd ^= 1;
    for (k = 0; k < n && d->ring_fill; k++) {
        if (ring_pop_put(d)) {
            TR("K56D %.3f B1 %d of %d bits wrong (to its S)\n", d->now / 8000.0, (int)d->up.vrx.b1_errors, (int)d->up.vrx.b1_bits);
            if (!b1_ok(d)) break;
            d->up.rx_ready = 1;
        }
        if (!d->up.rx_ready) continue;
        run = v34_rx_s_run(&d->up.vrx);
        if (run >= S_DRAIN_RUN) break;
    }
    if (k == n && d->ring_fill) return;
    if (d->up.rx_ready && run > 0) v34_rx_flush_before(&d->up.vrx, run);
    TR("K56D %.3f caught up to its S: run %u, %u stored points dropped\n", d->now / 8000.0, run, (unsigned)d->ring_fill);
    d->up.rx_ready = 0;
    d->catchup = 0;
    d->ring_fill = 0;
}

static void rx_symbol(k56d_t *d)
{
    v34up_t *u = &d->up;
    qamrx_t *r = &u->rx;
    int k;

    if (d->catchup == K56D_DRAIN) s_drain(d);
    switch (d->rxmode) {
    case RX_KNOWN:
        if (v34up_known(u, TRN_KNOWN)) {
            v34_tdem_init(&u->tdem, V34_GPC, 4);
            d->jsr = 0; d->jbits = d->j_at = 0; d->j_seen = 0;
            d->rxmode = RX_J;
        }
        break;

    case RX_J: {
        /* 4-point decisions: TRN, J24, J' */
        unsigned bits = v34up_dec4(u, QRX_EQ | QRX_CARRIER | QRX_TIMING);
        for (k = 0; k < 2 && d->rxmode == RX_J; k++) j_bit(d, (int)((bits >> k) & 1));
        break;
    }

    case RX_SRES:
        if (!sres_symbol(&d->sres, r)) break;
        /* its TRN, MP, MP' and E on 4 points; the descrambler finds its way
         * within 23 bits */
        v34_tdem_init(&u->tdem, V34_GPC, 4);
        d->fn = d->sh.ones = 0;
        TR("K56D %.3f S-bar ended (renegotiation)\n", d->now / 8000.0);
        d->rxmode = RX_RN;
        break;

    case RX_RN: {
        /* 4-point decisions. The equaliser stays as data mode left it (the
         * signal's scale is not data's); carrier and timing follow (as x2d) */
        unsigned bits = v34up_dec4(u, QRX_CARRIER | QRX_TIMING);
        for (k = 0; k < 2 && d->rxmode == RX_RN; k++) seq_bit(d, (int)((bits >> k) & 1));
        break;
    }

    case RX_P4: {
        /* 16-point decisions (as x2d): the equaliser learns only from
         * decisions close to a point */
        unsigned bits = v34up_dec16(u, 1);
        for (k = 0; k < 4 && d->rxmode == RX_P4; k++) seq_bit(d, (int)((bits >> k) & 1));
        break;
    }

    case RX_DATA: {
        /* v34up_data's steps, with the ring (B1's retry) between them */
        v34_pt_t in, tent, sent;
        int32_t dr, di;
        int flags = QRX_EQ | QRX_CARRIER | QRX_TIMING | QRX_DENSE;
        in = v34up_scale_in(u);
        if (!u->rx_ready || d->catchup) ring_put(d, in);
        if (d->catchup) {
            /* the stored points, this one last, 3 every 2 symbols under the other
             * hypothesis; this one's decision on its own (no precoding: the
             * plain slicer, as v34_rx_put's own tentative decision) */
            tent = v34_slice(&u->par, v34_nl_decode(&u->par, in));
            if (catch_up(d) != 0) return;
        } else {
            in = v34_nl_decode(&u->par, in);
            v34_rx_put(&u->vrx, in, &tent);
        }
        sent = v34_nl_encode(&u->par, tent);
        v34up_scale_out(u, sent, &dr, &di);
        if (!d->catchup && v34up_b1_in(u) && b1_check(d) != 0) return;
        /* catching up: the equaliser is left as it is */
        if (d->catchup) flags = QRX_CARRIER | QRX_TIMING | QRX_DENSE;
        qamrx_update(r, dr, di, flags, 13);
        /* nothing being received in data mode ends the call: we never
         * retrain (decision 14) */
        if (v34up_watch(u, d->state == K56D_DATA, 0) != V34UP_NONE) end(d, K56D_ERR_CARRIER);
        break;
    }

    default:    /* RX_IDLE: keep the grid running, change nothing */
        qamrx_update(r, r->zr, r->zi, 0, 12);
        break;
    }
}

/* the echo canceller on our TRN1A .. TRN1C, the client silent: first the
 * delay (when not known), then the canceller */
static void ec_schedule(k56d_t *d, int16_t rxs)
{
    int32_t from = EC_SETTLE;
    /* the search ends a little past the round trip (ecdly_near), so its
     * window comes that much later: when the echo of TRN1A is back at
     * every lag it tries */
    if (d->cfg.echo_delay < 0) from += d->ecd.base;
    d->ec_mu = v34up_ec_schedule(&d->ec, &d->ecd, d->cfg.echo_delay < 0, 1, rxs, ++d->ec_n, &from, EC_FAST, 0);
}

/* the client's S: PP(0) is centred 16.5 symbols after the S / S-bar
 * boundary, and the event came SDET_DELAY samples after it (as x2d) */
static void wait_s(k56d_t *d, int16_t x)
{
    (void)qamrx_tick(&d->up.rx, x);             /* input level for the AGC */
    if (!sdet_tick(&d->sdet, x)) return;
    v34up_sync_pp(&d->up, &d->sdet, SYM_RATE);
    v34trn_init(&d->up.ref, V34T_GPC);
    d->rxmode = RX_KNOWN;
    d->t_mark = d->now;                         /* K56D_T_J from here */
    TR("K56D %.3f S\n", d->now / 8000.0);
}

/* its tone B, in data mode or in its renegotiation: its retrain (V.34
 * 11.5, "Retrain"): silent from here; the owner's Phase 2 next */
static uint8_t retrain(k56d_t *d)
{
    TR("K56D %.3f tone B (state %d)\n", d->now / 8000.0, (int)d->state);
    d->tx_ready = d->up.rx_ready = 0;
    d->rxmode = RX_IDLE;
    enter(d, K56D_RETRAIN);
    return 0xff;
}

uint8_t k56d_tick(k56d_t *d, int16_t rxs)
{
    uint8_t octet;
    int16_t x;
    k56tx_seg seg;

    d->now++; d->t++;
    if (d->state == K56D_ENDED || d->state == K56D_RETRAIN) return 0xff;
    seg = k56tx_segment(&d->tx);
    octet = k56tx_tick(&d->tx);
    /* the echo of this octet begins ecd.delay samples from now */
    ecdly_put(&d->ecd, (int16_t)v90_octet_linear(0, octet));
    x = ec_tick(&d->ec, ecdly_ref(&d->ecd, 0), rxs, d->ec_mu);
    ecdly_next(&d->ecd);

    if (seg >= K56TX_TRN1A && seg <= K56TX_TRN1C) ec_schedule(d, rxs);
    else d->ec_mu = -1;

    /* receiver: from our Js on (the client answers it) */
    if (d->rxmode == RX_OFF) {
        if (seg >= K56TX_JS) {
            d->rxmode = RX_WAIT_S;
            wait_s(d, x);
        }
    } else if (d->rxmode == RX_WAIT_S) {
        wait_s(d, x);
    } else {
        if (v34up_tick(&d->up, x, d->rxmode == RX_DATA)) rx_symbol(d);
        if (d->state == K56D_ENDED) return 0xff;
        if (d->rxmode == RX_DATA && d->state != K56D_TRAIN2) {
            /* its S from its data's first symbol on (B1, and its data before
             * ours has begun): else S would be decoded as data until heard.
             * Not with its E early (our MP still to go) */
            (void)sdet_tick(&d->sdet, x);
            if (d->sdet.present >= SDET_PRESENT) reneg_begin(d);
            else if ((d->state == K56D_DATA || d->state == K56D_RENEG) && tonedet_tick(&d->tone_b, x)) return retrain(d);
        } else if (d->state == K56D_RENEG && tonedet_tick(&d->tone_b, x)) {
            /* it gave up its renegotiation for a retrain (R-3) */
            return retrain(d);
        }
    }

    switch (d->state) {
    case K56D_TRAIN1:
        if (seg == K56TX_SIL2) {
            if (d->rxmode == RX_WAIT_S) d->t_mark = d->now;   /* K56D_T_S from the end of our Js */
            enter(d, K56D_WAIT_J);
        }
        break;
    case K56D_WAIT_J:
        if (d->got_j) {
            int pad = d->j_octet >> 6;
            /* a pad code we have no levels for (Ruling F3: refused here,
             * k56tx_got_j would only keep silence 2 going) */
            if (pad > 2) { end(d, K56D_ERR_J); break; }
            d->pad = pad; d->rbs = (uint8_t)(d->j_octet & 0x3f);
            k56tx_got_j(&d->tx, d->j_octet);
            d->t_mark = CLK_NONE;
            enter(d, K56D_TRAIN2);
        } else if (d->rxmode == RX_WAIT_S) {
            if (clk_diff(d->now, d->t_mark) > MS(K56D_T_S)) end(d, K56D_ERR_NO_S);
        } else if (clk_diff(d->now, d->t_mark) > MS(K56D_T_J)) {
            end(d, K56D_ERR_NO_J);
        }
        break;
    case K56D_TRAIN2:
        /* K56D_T_MP from the start of our TRN2B (Ruling F4) */
        if (seg == K56TX_TRN2B && d->t_mark == CLK_NONE) d->t_mark = d->now;
        if (seg >= K56TX_MP) {
            /* its E may be in already (rx_begin_data set t_mark for T_B1): keep that */
            if (!d->got_e) d->t_mark = CLK_NONE;
            enter(d, K56D_MP);
        }
        else if (!d->got_mp && d->t_mark != CLK_NONE && clk_diff(d->now, d->t_mark) > MS(K56D_T_MP)) end(d, K56D_ERR_NO_MP);
        break;
    case K56D_MP:
        if (!d->got_e) {
            /* K56D_T_E from the start of our MP' */
            if (seg == K56TX_MPA && d->t_mark == CLK_NONE) d->t_mark = d->now;
            if (d->t_mark != CLK_NONE && clk_diff(d->now, d->t_mark) > MS(K56D_T_E)) end(d, K56D_ERR_NO_E);
            break;
        }
        /* its E came (rx_begin_data set t_mark): B1 both ways */
        if (seg == K56TX_DATA && d->tx.b1 == 0 && d->tx.fpos == K56ENC_N) d->tx_ready = 1;   /* B1 sent */
        if (d->tx_ready && d->up.rx_ready) {
            d->down_rate = k56tx_rate(&d->tx);
            TR("K56D %.3f data %d / %d\n", d->now / 8000.0, d->down_rate, d->up_rate);
            enter(d, K56D_DATA);
        } else if (clk_diff(d->now, d->t_mark) > MS(K56D_T_B1)) {
            end(d, K56D_ERR_B1);
        }
        break;
    case K56D_RENEG:
        /* its E comes after its MP, long after our R2: B1 sent is the new data's */
        if (d->got_e && seg == K56TX_DATA && d->tx.b1 == 0 && d->tx.fpos == K56ENC_N) d->tx_ready = 1;
        if (d->tx_ready && d->up.rx_ready) {
            d->down_rate = k56tx_rate(&d->tx);
            TR("K56D %.3f data again %d / %d\n", d->now / 8000.0, d->down_rate, d->up_rate);
            enter(d, K56D_DATA);
        } else if (d->t > MS(K56D_T_RENEG)) {
            end(d, K56D_ERR_RENEG);
        }
        break;
    default:
        break;
    }
    return d->state == K56D_ENDED ? 0xff : octet;
}
