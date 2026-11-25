/* V.90 analogue modem: time limits, the receiver's watch on itself,
 * renegotiation and retrain. See v90a.h, v90a_priv.h. */
#include "v90a_priv.h"

/* our rate renegotiation: 1 asked for, 2 for the line (see v90a.h) */
int v90a__renegotiate(v90a_t *a, int kind)
{
    if (a->state != V90A_DATA) return -1;
    a->reneg_ours = kind;
    a->cp_ready = 0;                    /* designed on the digital modem's TRN2d (rd_dec) */
    a->rx_ready = a->tx_ready = 0;
    a->state = V90A_RENEG; a->t = 0;
    if (a->pcm) {
        /* 9.8.2.1.1: Ru on our next data frame boundary; what Phase 4 heard
         * (SUVd, CPd, their acknowledge bits, Ed) is not this one's; the
         * CPd in use stays, for a CPd that carries only what changed */
        v92aseq_restart(&a->aseq); a->got_suvd = a->got_cpd = a->got_ack = a->got_ed = 0;
        a->ru_due = 1;
    } else go_s(a);                     /* 9.6.2.1.1: S, S-bar */
    return 0;
}

/* once a data frame in data mode: the receiver's error against the noise
 * CP was designed for (v90a.h). The sum over the intervals: a pad or a
 * robbed bit leaves one interval noisier than the others, by design. */
void v90a__line_watch(v90a_t *a)
{
    uint64_t e = 0, d = 0, p = (uint64_t)a->rx.pq;
    int s;
    if (++a->rt_clock >= RT_FORGET_FRAMES) { a->rt_clock = 0; if (a->rt_recent > 0) a->rt_recent--; }
    if (a->since_auto < AUTO_GRACE) a->since_auto++;
    for (s = 0; s < 6; s++) { e += v90arx_mse_slot(&a->rx, s); d += a->dsn[s]; }
    if (!d) return;
    a->deg_frames = e > 2 * d ? a->deg_frames + 1 : 0;
    a->good_frames = 4 * e < d ? a->good_frames + 1 : 0;
    /* collapsed: decisions on nothing (the error 12 dB over the design). Our
     * V.92 transmitter runs on the clock of this receiver's timing loop,
     * which now follows noise: it stands until the retrain, so that the PCM
     * upstream stays whole meanwhile */
    if (e > 16 * d) {
        if (a->col_frames++ == 0 && a->pcm) { v90arx_timing_hold(&a->rx, 1); a->col_hold = 1; }
    } else {
        a->col_frames = 0;
        /* the loop goes on once the error is back within twice the design's:
         * a receiver that only half recovers (its estimate regrown from
         * nothing, cpu 2) moved the clock some 25 ppm in 150 ms, and the
         * cleardown that followed went out where the digital modem did not
         * listen */
        if (a->col_hold && e <= 2 * d) { a->col_hold = 0; v90arx_timing_hold(&a->rx, 0); }
    }
    if (a->deg_frames >= DEG_FRAMES || a->col_frames >= COL_FRAMES) {
        /* the line's level moved by 3 dB: the DIL's levels no longer hold */
        int moved = p > 2 * a->mse_ref || 2 * p < a->mse_ref;
        a->deg_frames = 0;
        a->auto_down = 1;
        if (moved || a->col_frames || a->since_auto < AUTO_GRACE) {
            if (a->rt_recent < V90A_MAX_RETRAINS) a->rt_due = 1;
            else { a->clear_err = V90A_ERR_RETRAINS; (void)v90a_cleardown(a); }
        } else (void)v90a__renegotiate(a, 2);                         /* only the noise rose: CP for it */
    } else if (a->good_frames >= UP_FRAMES && a->auto_down && a->since_auto >= AUTO_GRACE && a->down_rate < a->best_rate
               && (a->cfg.max_rx_bps <= 0 || a->down_rate < a->cfg.max_rx_bps)) {
        /* what brought the rate down has gone again: once more, up */
        if (v90a__renegotiate(a, 2) == 0) a->auto_down = 0;
    }
}

/* time limits and the transitions that do not come with a symbol */
void v90a__watch(v90a_t *a)
{
    int32_t rtd = a->p2.rtd;
    if (a->ds == D_SD) {
        if (a->rx.state != V90ARX_WAIT) {
            /* 9.3.2.4: Sd-bar ends Ja */
            a->t_sd = a->now;
            if (a->us == U_JA) { ugo(a, U_QUIET); a->t_ja_end = a->now; }
            if (a->us == P_JA) a->ja_stop = 1;          /* V.92 9.5.2.1.3: at the next 12 bit boundary */
            v90_scr_init(&a->jscr); a->jprev = 0; a->jbn = 0; a->jsh.ones = 0;
            a->ds = D_JD;
        } else if (a->t_ja != CLK_NONE && clk_diff(a->now, a->t_ja) > MS(T_SD_MS)) { v90a__end(a, V90A_ERR_SD); return; }
    }
    if (a->rx.state == V90ARX_FAIL) { v90a__end(a, V90A_ERR_TRAIN); return; }
    if (a->ds == D_JD && !a->jd_ok && a->t_ja_end != CLK_NONE && clk_diff(a->now, a->t_ja_end) > MS(T_JD_MS)) { v90a__end(a, V90A_ERR_JD); return; }
    if (a->ds == D_JD && a->jd_ok && !a->pcm && clk_diff(a->now, a->t_jd) > MS(T_JDP_MS)) { v90a__end(a, V90A_ERR_JDP); return; }
    if (a->pcm && a->ds == D_JD && a->t_su != CLK_NONE && !a->jp_ok && clk_diff(a->now, a->t_su) > MS(T_JP_MS)) { v90a__end(a, V90A_ERR_JP); return; }
    if (a->pcm && a->ds == D_JD && a->jp_ok && a->us == P_TRN1B && clk_diff(a->now, a->t_jp) > MS(T_JP_MS) + MS(100)) { v90a__end(a, V90A_ERR_JP); return; }
    if (a->ds == D_DIL && clk_diff(a->now, a->t_jdp) > MS(T_DIL_MS)) {
        if (v90dil_acc_min(&a->acc) >= 2) v90a__dil_finish(a); else v90a__end(a, V90A_ERR_DIL);
        if (a->state == V90A_ENDED) return;
    }
    if (a->ds == D_DILEND && clk_diff(a->now, a->t_dil_end) > MS(T_RI_MS)) { v90a__end(a, V90A_ERR_RI); return; }
    if ((a->state == V90A_P3 || a->state == V90A_P4)
        && clk_diff(a->now, a->t_info1a) > (a->pcm ? MS(T_P4_92_MS) + 6 * rtd : MS(T_P4_MS) + 5 * rtd)) { v90a__end(a, V90A_ERR_PHASE4); return; }
    if (a->state == V90A_RENEG && (int64_t)a->t > (a->pcm ? MS(T_RENEG92_MS) : MS(T_RENEG_MS)) + 2 * rtd) { v90a__end(a, V90A_ERR_CARRIER); return; }
    if ((a->state == V90A_P4 || a->state == V90A_RENEG) && a->rx_ready && a->tx_ready && !a->clearing) {
        if (a->state == V90A_RENEG) a->renegs++;
        if (a->reneg_ours == 2) a->since_auto = 0;
        a->reneg_ours = 0;
        a->state = V90A_DATA; a->t = 0;
        if (a->t_data == CLK_NONE) a->t_data = a->now;
    }
}

/* a retrain, ours or the digital modem's: everything as at the start but the
 * call's counts, our clock and the first INFO0d; 70 ms of silence first */
void v90a__begin_retrain(v90a_t *a, int ours)
{
    v90a_cfg cfg = a->cfg;
    info0_t i0 = a->info0d;
    uint32_t now = a->now, t_data = a->t_data;
    int32_t rt_clock = a->rt_clock;
    int retrains = a->retrains, ro = a->retrains_ours, rr = a->rt_recent, renegs = a->renegs, max0 = a->max_rx0;
    int best = a->best_rate, adown = a->auto_down;
    if (rr >= V90A_MAX_RETRAINS) {
        a->rt_due = 0;
        a->state = V90A_P2;             /* (so that v90a__end() asks for nothing more) */
        v90a__end(a, V90A_ERR_RETRAINS);
        return;
    }
    v90a_init(a, &cfg);
    a->max_rx0 = max0;
    a->now = now; a->t_data = t_data; a->rt_clock = rt_clock; a->info0d = i0;
    a->retrains = retrains + 1; a->retrains_ours = ro + (ours != 0); a->rt_recent = rr + 1;
    a->renegs = renegs; a->best_rate = best; a->auto_down = adown;
    a->state = V90A_RETRAIN; a->t = 0;
}
