/* x2 as its server, after the marker. See x2d.h.
 *
 * What the client sends, and when, is docs/history/x2-facts.md ("Phase 3
 * script", "Records", "Upstream"): RE of D-Modem
 * (https://github.com/D-Modem/D-Modem, docs/dsp/x2-details.md) and of
 * courier-emu (https://github.com/cryan209/courier-emu). Facts only; no code
 * of either. The receiver is p34d's (v34up). */
#include <string.h>
#ifdef X2D_TRACE
#include <stdio.h>
#define TR(...) fprintf(stderr, __VA_ARGS__)
#else
#define TR(...) ((void)0)
#endif
#include "x2d.h"
#include "clock.h"
#include "x2rec.h"
#include "v34mp.h"
#include "p2ans.h"
#include "../v90/v90_pcm.h"
#ifdef P34D_TRACE
int16_t x2d_trace_tx(const x2d_t *d, int16_t y);    /* tools/trace.c: what the box really sent */
#endif

#define MS(ms) ((int32_t)(ms) * 8)

#define TRN_KNOWN      512      /* symbols of the client's TRN used as a known reference (as p34d) */
#define MP_WORDS       4
#define MP_MD          5        /* the downstream signs (MP word 3's high byte) of every bank ("Banks") */
#define MP_BITS        X2REC_BITS(MP_WORDS)
#define E_ONES         20

/* our echo canceller trains on our TRN while the client is quiet: it stops
 * its J some 150 ms after our preamble began ("Phase 3 script") */
#define EC_QUIET_MS    150
#define EC_GUARD_MS    20
#define EC_FAST        3000     /* samples at full NLMS step (as p34d) */

/* time limits: each ends the call well within 10 s of the client's silence */
#define T_J_MS         6000     /* the marker to the client's J */
#define T_S4_MS        3000     /* our J words (X2TX_LEAD_N + PRE + TRN after its J) to its S */
#define T_MP_MS        6000     /* its S-bar to its MP: its TRN is some 1.6 s */
#define T_E_MS         6000     /* our first record to its E */
#define T_B1_MS        3000     /* its E to B1 received and our start segment sent */
#define T_RENEG_MS     6000     /* the client's S in data mode to data again */

enum { RX_WAIT_S, RX_KNOWN, RX_J, RX_SRES, RX_P4, RX_RN, RX_DATA, RX_IDLE };

static void end(x2d_t *d, int err)
{
    TR("X2D %.3f end %d (state %d, rxmode %d)\n", d->now / 8000.0, err, (int)d->state, d->rxmode);
    d->state = X2D_ENDED; d->error = err; d->t = 0;
    d->tx_ready = d->up.rx_ready = 0;
    d->rxmode = RX_IDLE;
}

static void enter(x2d_t *d, x2d_state s) { d->state = s; d->t = 0; }

int x2d_init(x2d_t *d, const x2d_cfg *cfg)
{
    int idx = (cfg->marker >> 4) & 7;
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    if (d->cfg.echo_delay > X2D_MAX_ECHO_DELAY) d->cfg.echo_delay = X2D_MAX_ECHO_DELAY;
    x2tx_init(&d->tx);
    ec_init(&d->ec); d->ec_mu = -1;
    ecdly_init(&d->ecd, d->cfg.echo_delay < 0 ? 0 : d->cfg.echo_delay);
    ecdly_near(&d->ecd, d->cfg.rtd);
    d->sym_rate = v34_symbol_rate(idx);
    v34_carrier(idx, cfg->marker & 1, &d->car_num, &d->car_den);
    d->rxmode = RX_WAIT_S;
    d->state = X2D_WAIT_J;
    if (d->sym_rate != 3000 && d->sym_rate != 3200) { end(d, X2D_ERR_CFG); return -1; }
    sdet_init(&d->sdet, d->sym_rate, d->car_num, d->car_den);
    d->sdet.div = 16;               /* nothing but S can be on the line */
    qamrx_init(&d->up.rx, d->sym_rate, d->car_num, d->car_den);
    v34_tdem_init(&d->up.tdem, V34_GPA, 4);
    tonedet_init(&d->tone_a, TONEDET_2400);
    return 0;
}

int x2d_down_rate(const x2d_t *d) { return d->state == X2D_DATA || d->state == X2D_RENEG ? d->down_rate : 0; }
int x2d_up_rate(const x2d_t *d) { return d->state == X2D_DATA || d->state == X2D_RENEG ? d->up_rate : 0; }

int x2d_cleardown(x2d_t *d)
{
    if (d->state != X2D_DATA && d->state != X2D_RENEG) return -1;
    end(d, X2D_ERR_HANGUP);
    return 0;
}

/* ---- the client's MP ---- */

/* the client's upstream at rate (x2-facts.md "Upstream") */
static void up_cfg(const x2d_t *d, v34_cfg_t *c, int rate)
{
    memset(c, 0, sizeof *c);
    c->symbol_rate = (uint16_t)d->sym_rate; c->data_rate = (uint16_t)rate;
    c->trellis = V34_TRELLIS_64; c->expanded = 1; c->nonlinear = 1; c->scrambler = V34_GPA;
}

/* the highest rate of the client's upstream mask (word 1, bit n - 1 for
 * n x 2400 bit/s; at most 28800, "Upstream") that we can receive */
static int pick_up_rate(x2d_t *d, uint16_t w1)
{
    int n;
    for (n = 12; n >= 1; n--) {
        v34_cfg_t c;
        if (!((w1 >> (n - 1)) & 1)) continue;
        if (d->cfg.max_up_rate && n * 2400 > d->cfg.max_up_rate) continue;
        up_cfg(d, &c, n * 2400);
        if (v34_params_init(&d->up.par, &c) == 0) return n * 2400;   /* (set again at E) */
    }
    return 0;
}

/* an MP that checks out; taken when the next one says the same (TRN's
 * decisions make a record now and then) */
static void on_mp(x2d_t *d, const uint16_t *w)
{
    int k, same = d->cand_ok;
    TR("X2D %.3f MP %04X %04X %04X %04X (state %d)\n", d->now / 8000.0, w[0], w[1], w[2], w[3], (int)d->state);
    for (k = 0; k < MP_WORDS; k++) {
        uint16_t a = (uint16_t)(k == 0 ? w[k] & 0x7fff : w[k]), b = (uint16_t)(k == 0 ? d->cand[k] & 0x7fff : d->cand[k]);
        if (a != b) same = 0;
        d->cand[k] = w[k];
    }
    d->cand_ok = 1;
    if (!same || d->got_mp) return;
    memcpy(d->mp, w, sizeof d->mp);
    d->index = x2_mp_rate(w[0]);
    d->pad_mode = x2_mp_pad(w[0]);
    /* the exact index asked for, and only one we have a bank for ("Rate
     * change": a client given another rate's levels hangs up), with the
     * banks' MD */
    if (d->index < 1 || !((X2D_RATE_MASK >> (d->index - 1)) & 1)) { end(d, X2D_ERR_RATE); return; }
    if (x2_mp_md(w[3]) != MP_MD) { end(d, X2D_ERR_RATE); return; }
    if (d->pad_mode > 2) { end(d, X2D_ERR_PAD); return; }
    d->up_rate = pick_up_rate(d, w[1]);
    if (!d->up_rate) { end(d, X2D_ERR_UP); return; }
    d->got_mp = 1;
    x2tx_data(&d->tx, d->index, d->pad_mode, d->cfg.getbit, d->cfg.user);
    /* our record after the sync frames; in a rate change, our marker after
     * the data frame being sent ("Rate change": never before its MP) */
    x2tx_go(&d->tx);
}

/* ---- upstream data mode ---- */

static void put_data(void *user, int bit)
{
    x2d_t *d = (x2d_t *)user;
    /* from B1 on (as p34d), while our start segment may still be going down */
    if (d->up.rx_ready && d->cfg.putbit) d->cfg.putbit(d->cfg.user, bit);
}

/* E has just ended: B1 starts with the next symbol */
static void rx_begin_data(x2d_t *d)
{
    v34_cfg_t c;
    up_cfg(d, &c, d->up_rate);
    if (v34up_data_begin(&d->up, &c, put_data, d) != 0) { end(d, X2D_ERR_UP); return; }
    d->rxmode = RX_DATA;
    d->t_mark = d->now;
    sdet_init(&d->sdet, d->sym_rate, d->car_num, d->car_den);     /* watches for S from here on */
    tonedet_init(&d->tone_a, TONEDET_2400);
}

/* ---- descrambled bits ---- */

/* Phase 3: J (V.34's, either kind) twice in a row */
static void j_bit(x2d_t *d, int bit)
{
    int ty;
    d->jsr = (uint16_t)((d->jsr >> 1) | (bit << 15)); d->jbits++;
    ty = d->jsr == word16(v34_j4) ? 1 : d->jsr == word16(v34_j16) ? 2 : 0;
    if (!ty) return;
    if (ty == d->j_type && d->jbits - d->j_at == 16) {
        /* our training from the next octet; the client's Phase 4 begins with
         * S after a silence */
        x2tx_start(&d->tx);
        sres_init(&d->sres);
        d->rxmode = RX_SRES;
        d->t_mark = d->now;
        enter(d, X2D_TRAIN);
        return;
    }
    d->j_type = ty; d->j_at = d->jbits;
}

/* Phase 4: records (17 ones, a zero, ...) and E (20 ones) */
static void seq_bit(x2d_t *d, int bit)
{
    if (d->fn > 0) {
        d->fbuf[d->fn++] = (uint8_t)bit;
        if (d->fn == MP_BITS) {
            uint16_t w[MP_WORDS];
            if (x2rec_parse(d->fbuf, d->fn, w, MP_WORDS) == MP_WORDS) on_mp(d, w);
            d->fn = 0;
            if (d->state == X2D_ENDED) return;
        }
    }
    int sync = seqhunt_bit(&d->sh, bit);
    if (bit) {
        /* inside a record a zero breaks every run at 16 */
        if (d->sh.ones == E_ONES && d->got_mp && !d->got_e) {
            d->got_e = 1;
            TR("X2D %.3f E\n", d->now / 8000.0);
            rx_begin_data(d);
        }
        return;
    }
    if (sync) {
        memset(d->fbuf, 1, 17); d->fbuf[17] = 0; d->fn = 18;
    }
}

/* The client's S in data mode: its rate change ("Rate change"). Its S,
 * S-bar, TRN and MP as in its Phase 4; our answer after the MP (on_mp); its
 * E, and its V.34 data at the new upstream rate (rx_begin_data). The
 * carrier is locked: S is believed at once (as p34d). */
static void reneg_begin(x2d_t *d)
{
    /* the frames its V.34 decoder still holds from before S are data: they
     * go on now, but for a frame of margin (as p34d's s_heard). Called on
     * each sample from S heard on: an S too new in the decoder is waited
     * for (SDET_RUN_WAIT) */
    if (!v34up_s_flush(&d->up, &d->sdet)) return;
    /* its S before our data ever moved (our start segment going out): no
     * bit of the host's goes down until our answer's start segment is out
     * (ones in its place, should the start end before its MP is in) */
    if (d->state == X2D_START) x2tx_data(&d->tx, d->tx.index, d->tx.pad_mode, NULL, NULL);
    d->tx_ready = 0;
    d->got_mp = d->got_e = 0; d->cand_ok = 0;
    d->fn = d->sh.ones = 0;
    sres_resume(&d->sres);
    d->rxmode = RX_SRES;
    TR("X2D %.3f S in data mode\n", d->now / 8000.0);
    enter(d, X2D_RENEG);
}

/* ---- one received symbol ---- */

static void rx_symbol(x2d_t *d)
{
    qamrx_t *r = &d->up.rx;
    int k;

    switch (d->rxmode) {
    case RX_KNOWN:
        if (v34up_known(&d->up, TRN_KNOWN)) {
            v34_tdem_init(&d->up.tdem, V34_GPA, 4);
            d->jsr = 0; d->jbits = d->j_at = 0; d->j_type = 0;
            d->rxmode = RX_J;
        }
        break;

    case RX_J: {
        /* 4-point decisions: TRN, then J */
        unsigned bits = v34up_dec4(&d->up, QRX_EQ | QRX_CARRIER | QRX_TIMING);
        for (k = 0; k < 2 && d->rxmode == RX_J; k++) j_bit(d, (int)((bits >> k) & 1));
        break;
    }

    case RX_SRES:
        if (!sres_symbol(&d->sres, r)) break;
        d->fn = d->sh.ones = 0;
        if (d->state == X2D_RENEG) {
            /* a rate change: its TRN, MP, MP' and E on 4 points (the
             * Courier's: x2-facts.md "Rate change") */
            v34_tdem_init(&d->up.tdem, V34_GPA, 4);
            TR("X2D %.3f S-bar ended (rate change)\n", d->now / 8000.0);
            d->rxmode = RX_RN;
            break;
        }
        /* TRN (4 points, then 16), the MP and E on 16 points (the J
         * words: our disassembly's "0343 must equal 09B0", courier-emu);
         * the descrambler finds its way within 23 bits */
        v34_tdem_init(&d->up.tdem, V34_GPA, 16);
        TR("X2D %.3f S-bar ended\n", d->now / 8000.0);
        d->rxmode = RX_P4;
        d->t_mark = d->now;
        break;

    case RX_RN: {
        /* 4-point decisions on what follows the client's S-bar in a rate
         * change. The equaliser stays as data mode left it (the signal's
         * scale is not data's); carrier and timing follow */
        unsigned bits = v34up_dec4(&d->up, QRX_CARRIER | QRX_TIMING);
        for (k = 0; k < 2 && d->rxmode == RX_RN; k++) seq_bit(d, (int)((bits >> k) & 1));
        break;
    }

    case RX_P4: {
        /* 16-point decisions. The 4-point part of TRN lands between them
         * (its points are the 16-point signal's power, not its inner ring):
         * the equaliser learns only from decisions close to a point */
        unsigned bits = v34up_dec16(&d->up, 1);
        for (k = 0; k < 4 && d->rxmode == RX_P4; k++) seq_bit(d, (int)((bits >> k) & 1));
        break;
    }

    case RX_DATA: {
        /* nothing being received in data mode ends the call: there is no
         * retrain in x2 */
        int was_ready = d->up.rx_ready;
        int ev = v34up_data(&d->up, d->state == X2D_DATA, 0);
        if (ev == V34UP_B1_BAD || (!was_ready && d->up.rx_ready))
            TR("X2D %.3f B1 %d of %d bits wrong\n", d->now / 8000.0, (int)d->up.vrx.b1_errors, (int)d->up.vrx.b1_bits);
        if (ev == V34UP_B1_BAD) end(d, X2D_ERR_B1);
        else if (ev != V34UP_NONE) end(d, X2D_ERR_CARRIER);
        break;
    }

    default:    /* RX_IDLE: keep the grid running, change nothing */
        qamrx_update(r, r->zr, r->zi, 0, 12);
        break;
    }
}

/* the echo canceller on our TRN: the client is quiet from some 150 ms into
 * our preamble; first the delay (when not known), then the canceller */
static void ec_schedule(x2d_t *d, int16_t rxs)
{
    int32_t from = MS(EC_QUIET_MS) + d->cfg.rtd + MS(EC_GUARD_MS);
    d->ec_mu = v34up_ec_schedule(&d->ec, &d->ecd, d->cfg.echo_delay < 0, 1, rxs, d->tx.n, &from, EC_FAST, 0);
}

/* the waits of the client's Phase 4 */
static int p4_late(const x2d_t *d)
{
    if (d->rxmode == RX_SRES)
        return clk_diff(d->now, d->t_mark) > X2TX_LEAD_N + X2TX_PRE_N + X2TX_TRN_N + MS(T_S4_MS) + 2 * d->cfg.rtd;
    return !d->got_mp && clk_diff(d->now, d->t_mark) > MS(T_MP_MS);
}

uint8_t x2d_tick(x2d_t *d, int16_t rxs)
{
    uint8_t octet;
    int16_t x;

    d->now++; d->t++;
    if (d->state == X2D_ENDED) return 0x7f;
    octet = x2tx_tick(&d->tx);
    /* the echo of this octet begins ecd.delay samples from now */
#ifdef P34D_TRACE
    ecdly_put(&d->ecd, x2d_trace_tx(d, (int16_t)v90_octet_linear(0, octet)));
#else
    ecdly_put(&d->ecd, (int16_t)v90_octet_linear(0, octet));
#endif
    x = ec_tick(&d->ec, ecdly_ref(&d->ecd, 0), rxs, d->ec_mu);
    ecdly_next(&d->ecd);

    /* receiver */
    if (d->rxmode == RX_WAIT_S) {
        (void)qamrx_tick(&d->up.rx, x);         /* input level for the AGC */
        if (sdet_tick(&d->sdet, x)) {
            v34up_sync_pp(&d->up, &d->sdet, d->sym_rate);
            v34trn_init(&d->up.ref, V34T_GPA);
            d->rxmode = RX_KNOWN;
        }
    } else {
        if (v34up_tick(&d->up, x, d->rxmode == RX_DATA)) rx_symbol(d);
        if (d->state == X2D_ENDED) return 0x7f;
        if (d->rxmode == RX_DATA && (d->state == X2D_DATA || d->state == X2D_START || d->state == X2D_RENEG)) {
            /* the client's S in data mode: its rate change (facts "Rate
             * change"); from our start segment on (its B1; its E is a record
             * before it), ours not yet out too (else its S would be decoded
             * as data until heard: Task 10d) */
            (void)sdet_tick(&d->sdet, x);
            if (d->sdet.present >= SDET_PRESENT) reneg_begin(d);
        }
        /* its Tone A from data mode on: a retrain, which x2 does not have */
        if ((d->state == X2D_DATA || d->state == X2D_RENEG) && tonedet_tick(&d->tone_a, x)) {
            end(d, X2D_ERR_RETRAIN);
            return 0x7f;
        }
    }

    switch (d->state) {
    case X2D_WAIT_J:
        if (d->t > MS(T_J_MS)) end(d, X2D_ERR_NO_J);
        break;
    case X2D_TRAIN:
        if (d->tx.seg == X2TX_TRN) ec_schedule(d, rxs);
        else if (d->tx.seg > X2TX_TRN) d->ec_mu = -1;
        if (p4_late(d)) end(d, d->rxmode == RX_SRES ? X2D_ERR_NO_S4 : X2D_ERR_NO_MP);
        else if (d->tx.seg >= X2TX_SYNC) enter(d, X2D_MP);
        break;
    case X2D_MP:
        if (p4_late(d)) end(d, d->rxmode == RX_SRES ? X2D_ERR_NO_S4 : X2D_ERR_NO_MP);
        else if (d->tx.seg == X2TX_REC) { d->t_mark = d->now; enter(d, X2D_REC); }
        break;
    case X2D_REC:
        if (d->got_e && !d->e_sent) { x2tx_go(&d->tx); d->e_sent = 1; }   /* the start after this record */
        if (d->tx.seg == X2TX_START) enter(d, X2D_START);
        else if (!d->got_e && clk_diff(d->now, d->t_mark) > MS(T_E_MS)) end(d, X2D_ERR_NO_E);
        break;
    case X2D_START:
        /* data moves from our start's last frame: the owner hears of it
         * (V90M_EV_DATA_MOVING) before the first bit of data is taken */
        if (x2tx_data_next(&d->tx)) d->tx_ready = 1;
        if (d->tx_ready && d->up.rx_ready) {
            d->down_rate = x2tx_rate(&d->tx);
            enter(d, X2D_DATA);
        } else if (clk_diff(d->now, d->t_mark) > MS(T_B1_MS)) {
            end(d, X2D_ERR_B1);
        }
        break;
    case X2D_RENEG:
        /* our answer is out once x2tx is back in data with no change pending */
        if (d->got_mp && x2tx_data_next(&d->tx)) d->tx_ready = 1;
        if (d->tx_ready && d->up.rx_ready) {
            d->down_rate = x2tx_rate(&d->tx);
            enter(d, X2D_DATA);
        } else if (d->t > MS(T_RENEG_MS)) {
            end(d, X2D_ERR_RENEG);
        }
        break;
    default:
        break;
    }
    return octet;
}
