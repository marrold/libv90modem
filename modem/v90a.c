/* V.90 analogue modem: init, the sample tick, the public calls. See v90a.h;
 * the rest is in v90a_up90.c, v90a_up92.c, v90a_down.c, v90a_watch.c. */
#include <string.h>
#include "v90a_priv.h"

/* the call ends with err (V90A_ERR_*) */
void v90a__end(v90a_t *a, int err)
{
    if (err == V90A_ERR_HANGUP && a->clear_err) err = a->clear_err;
    /* once the call has carried data, a Phase 3 or 4 that fails (a retrain's)
     * and a renegotiation without Ed (9.6.2) are a retrain of ours (v90a_tick) */
    a->rt_check = a->t_data != CLK_NONE && (a->state == V90A_P3 || a->state == V90A_P4 || a->state == V90A_RENEG)
                  && err != V90A_ERR_CLEARDOWN && err != V90A_ERR_HANGUP && err != V90A_ERR_RATE
                  && err != V90A_ERR_RETRAINS && !a->clearing;
    a->state = V90A_ENDED; a->error = err; a->t = 0;
    a->tx_ready = a->rx_ready = 0;
    a->us = U_OFF; a->md_on = 0;
}

/* ---- start ---- */

static void p2_cfg(const v90a_t *a, p2call_cfg *pc)
{
    memset(pc, 0, sizeof *pc);
    pc->level = a->cfg.level; pc->io_delay = a->cfg.echo_delay;
    pc->uinfo = a->cfg.uinfo; pc->md_len = a->cfg.md_len;
    pc->max_rate = a->cfg.max_rx_bps;
    pc->v92 = a->cfg.v92;
}

void v90a_init(v90a_t *a, const v90a_cfg *cfg)
{
    p2call_cfg pc;
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    if (a->cfg.uinfo <= 0) a->cfg.uinfo = 90;
    if (a->cfg.md_len <= 0) a->cfg.md_len = 22;
    if (a->cfg.md_len > 127) a->cfg.md_len = 127;
    if (a->cfg.z_q4 <= 0) a->cfg.z_q4 = V90A_Z_CP;
    if (a->cfg.echo_delay < 0) a->cfg.echo_delay = 0;
    if (a->cfg.echo_delay >= V90A_ECDL) a->cfg.echo_delay = V90A_ECDL - 1;
    a->max_rx0 = a->cfg.max_rx_bps;
    p2_cfg(a, &pc);
    p2call_init_v90(&a->p2, &pc);
    tonedet_init(&a->tone_b, TONEDET_1200);
    a->state = V90A_P2;
    a->t_info1a = a->t_ja = a->t_ja_end = a->t_sd = a->t_jd = a->t_jdp = a->t_dil_end = CLK_NONE;
    a->t_ri = a->t_ribar = a->t_mp = a->t_ed = a->t_data = CLK_NONE;
    a->dil_m0 = a->dil_end = a->ri_m0 = a->ribar_m0 = a->m_trn2d = a->m_b1d = -1;
    a->t_su = a->t_jp = a->t_cpt = a->t_suvd = CLK_NONE;
}

static int16_t tick(v90a_t *a, int16_t rx)
{
    int32_t y;
    int16_t ref;
    int i, nd;
    a->now++; a->t++;
    if (a->state == V90A_ENDED) return 0;
    if (a->state == V90A_P2) {
        int16_t o = p2call_tick(&a->p2, rx);
        if (a->p2.state == P2L_DONE) { a->info0d = a->p2.info0d; v90a__start_p3(a); }
        else if (a->p2.state == P2L_FAILED) v90a__end(a, a->t_data != CLK_NONE ? V90A_ERR_RETRAINS : V90A_ERR_PHASE2);
        /* a retrain's Phase 2 (after data mode: orig's start-up limit is
         * over) has V90A_RT_P2_MS, whatever p2call's own limits do (D5) */
        else if (a->t_data != CLK_NONE && a->t > MS(V90A_RT_P2_MS)) v90a__end(a, V90A_ERR_RETRAINS);
        return o;
    }
    if (a->pcm) {
        /* V.92: the symbols on the network clock as the receiver keeps it */
        pcmtx_clock(&a->ptx);
        while (pcmtx_due(&a->ptx)) {
            int16_t sy = v90a__up92_symbol(a);
            if (a->state == V90A_ENDED) return 0;
            pcmtx_put(&a->ptx, sy, a->rx.step + a->rx.slew);
            if (a->slip) { pcmtx_slip(&a->ptx, a->slip); a->slip = 0; }
        }
        y = pcmtx_out(&a->ptx);
        if (a->ec_len > 0) {
            /* the canceller learns on TRN1u: full step for a quarter of it,
             * then 2^-2, then 2^-4, frozen at its end */
            int32_t q = a->ec_len / 4;
            if (a->ec_n == 0 || a->ec_n == q || a->ec_n == 2 * q) v90arx_ec(&a->rx, a->ec_n == 0 ? 0 : a->ec_n == q ? 2 : 4);
            if (++a->ec_n >= a->ec_len) {
                v90arx_ec(&a->rx, -1);
                a->erle_db10 = probe_db10(a->erle_rx + 1, a->erle_res + 1);
                a->ec_len = 0;
            }
        }
    } else {
        if (v34mod_due(&a->mod)) {
            int32_t re, im;
            v90a__up_symbol(a, &re, &im);
            v34mod_put(&a->mod, re, im);
            if (a->state == V90A_ENDED) return 0;
        }
        y = v34mod_tick(&a->mod);
    }
    if (a->md_on) {
        /* MD: uniform white noise at the transmit level (RMS of a uniform
         * variable is its peak over sqrt 3); the canceller at full step for
         * a quarter of it, then 2^-2, then 2^-4 */
        int32_t v, q = a->md_total / 4, mu;
        a->md_rng = a->md_rng * 1664525u + 1013904223u;
        v = (int32_t)(a->md_rng >> 16) - 32768;
        y += (int32_t)(((int64_t)v * a->cfg.level) / 18919);
        mu = a->md_samples < q ? 0 : a->md_samples < 2 * q ? 2 : 4;
        if (a->md_samples == 0 || a->md_samples == q || a->md_samples == 2 * q) v90arx_ec(&a->rx, mu);
        a->md_samples++;
    }
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    if (a->cfg.law) y = v90_octet_linear(a->cfg.law == 2, pcm_encode(a->cfg.law == 2, (int16_t)y));
    a->ecdl[a->ecpos] = (int16_t)y;
    ref = a->ecdl[(a->ecpos - a->cfg.echo_delay) & (V90A_ECDL - 1)];
    a->ecpos = (a->ecpos + 1) & (V90A_ECDL - 1);
    nd = v90arx_tick(&a->rx, rx, ref);
    /* the digital modem's retrain: Tone B for more than 50 ms (9.5.2.2 / 9.7.2.2) */
    if (tonedet_tick(&a->tone_b, a->rx.eout)) a->rt_due = 2;
    if ((a->md_on && a->md_samples > a->md_total * 3 / 4) || (a->ec_len > 0 && a->ec_n > a->ec_len * 3 / 4)) {
        a->erle_rx += (int64_t)rx * rx;
        a->erle_res += (int64_t)a->rx.eout * a->rx.eout;
    }
    for (i = 0; i < nd && a->state != V90A_ENDED; i++) v90a__on_dec(a, &a->rx.out[i]);
    if (a->state == V90A_DATA && a->ds == D_DATA && clk_diff(a->rx.n, a->rx.n0) >= CLK_REBASE_AT) v90a__rebase(a);
    if (a->state != V90A_ENDED) v90a__watch(a);
    if (a->job != JOB_NONE && a->state != V90A_ENDED && !a->rx.dsn_ran) {
        if (a->job_new) a->job_new = 0;         /* (begun this tick: the first step in the next) */
        else v90a__job_step(a);
    }
    return (int16_t)y;
}

int16_t v90a_tick(v90a_t *a, int16_t rx)
{
    int16_t y;
    if (a->state == V90A_RETRAIN) {
        /* 9.5.2 / 9.7.2: 70 ms of silence, then Tone A and Phase 2 */
        a->now++; a->t++;
        if (a->t >= MS(70)) {
            p2call_cfg pc;
            p2_cfg(a, &pc);
            p2call_init_v90_retrain(&a->p2, &pc, &a->info0d);
            a->state = V90A_P2; a->t = 0;
        }
        return 0;
    }
    y = tick(a, rx);
    /* our cleardown goes on to its end (V90A_ERR_HANGUP) whatever the
     * digital modem does meanwhile: its Tone B is not a retrain then */
    if (a->clearing) a->rt_due = 0;
    if (a->rt_due || (a->state == V90A_ENDED && a->rt_check)) {
        v90a__begin_retrain(a, a->rt_due != 2);
        return 0;
    }
    return y;
}

int v90a_tx_rate(const v90a_t *a) { return a->state == V90A_DATA ? a->up_rate : 0; }
int v90a_rx_rate(const v90a_t *a) { return a->state == V90A_DATA ? a->down_rate : 0; }

int v90a_snr_db10(const v90a_t *a)
{
    /* the mean power of the data constellations' levels over the error */
    int64_t pw = 0;
    int s, u, n = 0, v;
    if (a->state != V90A_DATA) return -1;
    for (s = 0; s < 6; s++)
        for (u = 0; u < 128; u++)
            if ((a->cp.mask[a->cp.idx[s]][u >> 3] >> (u & 7)) & 1 && a->res.lv[s][u] > 0) {
                pw += (int64_t)a->res.lv[s][u] * a->res.lv[s][u];
                n++;
            }
    if (!n) return -1;
    v = probe_db10(pw / n, v90arx_mse(&a->rx) > 0 ? (int64_t)v90arx_mse(&a->rx) : 1);
    return v < 0 ? 0 : v > 600 ? 600 : v;
}

int v90a_cleardown(v90a_t *a)
{
    if (a->state != V90A_DATA) return -1;
    a->clearing = 1;
    a->rx_ready = a->tx_ready = 0;
    a->state = V90A_RENEG; a->t = 0;
    if (a->pcm) { a->ru_due = 1; a->cpus_left = CLEAR_CPS; }    /* 9.11: a renegotiation, CPu with drn = 0 */
    else go_s(a);
    return 0;
}

int v90a_retrain(v90a_t *a)
{
    if ((a->state != V90A_DATA && a->state != V90A_RENEG) || a->clearing || a->rt_recent >= V90A_MAX_RETRAINS) return -1;
    v90a__begin_retrain(a, 1);
    return 0;
}

int v90a_renegotiate(v90a_t *a, int max_rx_bps)
{
    if (a->state != V90A_DATA) return -1;
    if (max_rx_bps > 0) a->cfg.max_rx_bps = a->max_rx0 > 0 && max_rx_bps > a->max_rx0 ? a->max_rx0 : max_rx_bps;
    return v90a__renegotiate(a, 1);
}
