/* The shared V.34 upstream receiver. See v34up.h. */
#include "v34up.h"

void v34up_sync_pp(v34up_t *u, const sdet_t *sd, int sym_rate)
{
    int32_t d = (int32_t)(((int64_t)132000 * 256) / sym_rate) - (SDET_DELAY(sd) + 1) * 256;
    qamrx_sync(&u->rx, d);
}

int v34up_known(v34up_t *u, int n_trn)
{
    qamrx_t *r = &u->rx;
    int32_t dr, di;
    int n = r->nsym - 1;
    if (n < V34_PP_LEN) v34_pp(n, &dr, &di); else v34trn_next(&u->ref, &dr, &di);
    qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | (n > 300 ? QRX_TIMING : 0), n < 600 ? 10 : 11);
    return n >= V34_PP_LEN + n_trn - 1;
}

unsigned v34up_dec4(v34up_t *u, unsigned upd_flags)
{
    qamrx_t *r = &u->rx;
    v34_pt_t pt;
    int32_t dr = r->zr < 0 ? -QRX_REF : QRX_REF, di = r->zi < 0 ? -QRX_REF : QRX_REF;
    qamrx_update(r, dr, di, upd_flags, 12);
    pt.re = (int16_t)(dr < 0 ? -V34_ONE : V34_ONE); pt.im = (int16_t)(di < 0 ? -V34_ONE : V34_ONE);
    return v34_tdem_put(&u->tdem, pt);
}

unsigned v34up_dec16(v34up_t *u, int gate_eq)
{
    qamrx_t *r = &u->rx;
    v34_pt_t pt;
    int32_t lev = (int32_t)(((int64_t)QRX_REF * v34_gain_q14(V34_ENERGY_16PT_Q8)) >> 14);
    int qr = r->zr >= 2 * lev ? 3 : r->zr >= 0 ? 1 : r->zr >= -2 * lev ? -1 : -3;
    int qi = r->zi >= 2 * lev ? 3 : r->zi >= 0 ? 1 : r->zi >= -2 * lev ? -1 : -3;
    int32_t dr = qr * lev, di = qi * lev;
    unsigned eq = QRX_EQ;
    if (gate_eq) {
        int32_t er = r->zr - dr, ei = r->zi - di;
        if (er < 0) er = -er;
        if (ei < 0) ei = -ei;
        if (!(er < lev / 2 && ei < lev / 2)) eq = 0;
    }
    qamrx_update(r, dr, di, eq | QRX_CARRIER | QRX_TIMING | QRX_DENSE, 12);
    pt.re = (int16_t)(qr * V34_ONE); pt.im = (int16_t)(qi * V34_ONE);
    return v34_tdem_put(&u->tdem, pt);
}

int v34up_data_begin(v34up_t *u, const v34_cfg_t *c, v34_putbit_fn put, void *user)
{
    int32_t d;
    if (v34_params_init(&u->par, c) != 0) return -1;
    v34_rx_init(&u->vrx, &u->par, put, user);
    u->gain = (int32_t)v34_gain_q14(u->par.energy_tx_q8);
    /* the error this rate tolerates: SNR = 9 dB + 3 dB per bit per symbol,
     * less the slack; 3 dB is a factor of two */
    d = V34UP_SNR0_DB10 + 30 * c->data_rate / c->symbol_rate - V34UP_SLACK_DB10;
    u->mse_limit = 65536 >> (d / 30);
    u->mse_limit -= u->mse_limit * (d % 30) / 60;
    u->q_bad = u->q_dead = 0;
    u->quiet = 0;
    return 0;
}

int v34up_data(v34up_t *u, int watch_dead, int watch_slow)
{
    qamrx_t *r = &u->rx;
    v34_pt_t in, tent, sent;
    int32_t dr, di;
    /* (v34_nl_decode and v34_nl_encode are identities at nonlinear 0) */
    in = v34up_scale_in(u);
    if (u->par.cfg.nonlinear) in = v34_nl_decode(&u->par, in);
    v34_rx_put(&u->vrx, in, &tent);
    sent = u->par.cfg.nonlinear ? v34_nl_encode(&u->par, tent) : tent;
    v34up_scale_out(u, sent, &dr, &di);
    qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | QRX_TIMING | QRX_DENSE, 13);
    if (v34up_b1_in(u)) {
        if (u->vrx.b1_errors * 8 > u->vrx.b1_bits) return V34UP_B1_BAD;
        u->rx_ready = 1;
    }
    return v34up_watch(u, watch_dead, watch_slow);
}

int v34up_watch(v34up_t *u, int watch_dead, int watch_slow)
{
    const qamrx_t *r = &u->rx;
    if (u->rx_ready && watch_dead) {
        if (r->mse > V34UP_DEAD_MSE) { if (++u->q_dead > V34UP_BAD_SYMS) return V34UP_DEAD; }
        else u->q_dead = 0;
    }
    if (u->rx_ready && watch_slow) {
        if (r->mse > u->mse_limit) { if (++u->q_bad > V34UP_BAD_SYMS) { u->q_bad = 0; return V34UP_SLOW; } }
        else u->q_bad = 0;
    }
    /* loss of the upstream signal */
    if (u->zpw < (int64_t)QRX_REF * QRX_REF / 8) {
        if (++u->quiet > V34UP_LOSS_SYMS) return V34UP_LOST;
    } else {
        u->quiet = 0;
    }
    return V34UP_NONE;
}

int v34up_retry(v34up_t *u, const v34_cfg_t *alt)
{
    v34_putbit_fn put = u->vrx.put;
    void *user = u->vrx.user;
    if (v34_params_init(&u->par, alt) != 0) return -1;
    v34_rx_init(&u->vrx, &u->par, put, user);
    u->gain = (int32_t)v34_gain_q14(u->par.energy_tx_q8);
    return 0;
}

int v34up_replay_sym(v34up_t *u, int16_t re, int16_t im)
{
    v34_pt_t x;
    x.re = re; x.im = im;
    v34_rx_put(&u->vrx, v34_nl_decode(&u->par, x), NULL);
    return v34up_b1_in(u);
}

int v34up_s_flush(v34up_t *u, const sdet_t *sd)
{
    if (u->rx_ready) {
        unsigned n = v34_rx_s_run(&u->vrx);
        if (n == 0 && v34_rx_s_begins(&u->vrx) && sd->present < SDET_PRESENT + SDET_RUN_WAIT) return 0;
        if (n > 0) v34_rx_flush_before(&u->vrx, n);
    }
    u->rx_ready = 0;
    return 1;
}

int v34up_ec_schedule(ec_t *ec, ecdly_t *ecd, int measure, int diff, int16_t rxs,
                      int32_t n, int32_t *from, int32_t fast, int mu_fast)
{
    if (measure) {
        if (ecdly_measure(ecd, n - *from, rxs, diff)) ec_init(ec);
        *from += ECDLY_SAMPLES;
    }
    if (n < *from) return -1;
    if (n < *from + fast) return mu_fast;
    return 3;
}
