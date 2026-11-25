/* V.90 analogue modem: the downstream sequences. See v90a.h, v90a_priv.h. */
#include <string.h>
#include "v90a_priv.h"

/* ---- the DIL as known symbols for the receiver ---- */

int v90a__dil_known(void *ctx, int32_t m, int *u, int *pos, int32_t *lev)
{
    v90a_t *a = (v90a_t *)ctx;
    int32_t l;
    if (a->dil_m0 < 0 || m < a->dil_m0 || (a->dil_end >= 0 && m >= a->dil_end)) return 0;
    while (a->dil_mnext <= m) {
        int uu, pp, f = v90dil_next(&a->dil, &a->dcur, &uu, &pp);
        int i = a->dil_mnext & 63;
        a->dsu[i] = (uint8_t)uu; a->dsp[i] = (uint8_t)pp; a->dsf[i] = (uint8_t)f;
        a->dil_mnext++;
    }
    if (a->dsf[m & 63]) a->cb_seg = m;
    /* after we asked for the end of the DIL: a segment's fourth symbol is a
     * positive training symbol, Ri's is -UINFO (v90dil.h). The third one's
     * output is known by now, from this tick or an earlier one. */
    if (a->ds == D_DILEND && m == a->cb_seg + 4) {
        int32_t L = v90_ucode_linear(a->alaw, (unsigned)a->cfg.uinfo) * (1 << YF), y3;
        y3 = a->rx.nout > 0 && a->rx.out[a->rx.nout - 1].m == m - 1 ? a->rx.out[a->rx.nout - 1].y : a->ry[(m - 1) & 63];
        if (y3 < -L / 2) {
            a->ri_m0 = a->cb_seg; a->dil_end = a->cb_seg; a->t_ri = a->now;
            a->hold_until = m + V90ARX_NB;          /* until the DIL's levels have left the feedback line */
            v90arx_hold(&a->rx, 1);
            a->ri_units = 0;
            a->ds = D_RI;
            return 0;
        }
    }
    *u = a->dsu[m & 63]; *pos = a->dsp[m & 63];
    /* the level it arrives at, as far as it has been measured: what the
     * decision feedback cancels (Table 1's would leave the error of a pad
     * or a robbed bit in every later symbol) */
    l = a->acc.cnt[m % 6][*u] ? v90dil_acc_mean(&a->acc, (int)(m % 6), *u) : v90_ucode_linear(a->alaw, (unsigned)*u) * (1 << YF);
    *lev = *pos ? l : -l;
    return 1;
}

/* the receiver's constellations for a CPt / CP: its points at the levels the
 * DIL measured */
void v90a__set_cons(v90a_t *a, const v90_cp_t *cp, int32_t m)
{
    v90arx_cons_t c;
    int s, u, n;
    memset(&c, 0, sizeof c);
    for (s = 0; s < 6; s++) {
        const uint8_t *mk = cp->mask[cp->idx[s] > 5 ? 0 : cp->idx[s]];
        for (n = 0, u = 0; u < 128; u++)
            if ((mk[u >> 3] >> (u & 7)) & 1) {
                c.u[s][n] = (uint8_t)u;
                c.lv[s][n] = a->res.lv[s][u] >= 0 ? a->res.lv[s][u] : v90_ucode_linear(a->alaw, (unsigned)u) * (1 << YF);
                n++;
            }
        c.n[s] = (uint8_t)n;
    }
    v90arx_set_cons(&a->rx, &c, m);
}

/* ---- downstream ---- */

/* Jd, Jd': the sign of UINFO, differentially, through the GPC descrambler
 * that has run since TRN1d began (8.4.2, 8.4.3) */
static void jd_bit(v90a_t *a, const v90arx_dec_t *d)
{
    int c = d->sign ^ a->jprev, bit;
    a->jprev = d->sign;
    bit = v90_descramble_gpc(&a->jscr, c);
    if (!a->jd_ok) {
        if (a->jbn > 0) {
            a->jb[a->jbn++] = (uint8_t)bit;
            if (a->jbn == V90_JD_BITS) {
                if (v90_jd_parse(a->jb, V90_JD_BITS, &a->jd) == V90_JD_BITS) {
                    a->jd_ok = 1; a->t_jd = a->now; a->jdp_zeros = 0; a->jbn = 0;
                    if (a->us == U_QUIET) ugo(a, U_SJ);         /* 9.3.2.7: S */
                } else a->jbn = 0;
                a->jsh.ones = 0;
            }
            return;
        }
        if (seqhunt_bit(&a->jsh, bit)) { memset(a->jb, 1, 17); a->jb[17] = 0; a->jbn = 18; }
        return;
    }
    /* after a good Jd: another Jd (it begins with ones), or Jd' (12 zeros) */
    if (a->jbn == 0 && !bit) {
        a->jdp_m = d->m; a->jdp_zeros = 1;
        /* the DIL begins 12 symbols on, at a data frame boundary: known from
         * here, nothing learnt while it runs */
        a->dil_m0 = d->m + 12; a->dil_mnext = a->dil_m0; a->dil_end = -1;
        v90dil_cur_init(&a->dcur);
        v90arx_hold(&a->rx, 1);
    } else if (a->jdp_zeros > 0) {
        if (bit) { a->jdp_zeros = 0; a->dil_m0 = -1; v90arx_hold(&a->rx, 0); }     /* a Jd after all */
        else if (++a->jdp_zeros == 12) {
            int s;
            a->t_jdp = a->now;
            for (s = 0; s < 6; s++) a->sig_jd[s] = (uint32_t)v90arx_mse_slot(&a->rx, s);
            v90dil_acc_init(&a->acc);
            a->acc_on = 1;
            a->ds = D_DIL;
            return;
        }
    }
    if (++a->jbn >= V90_JD_BITS) a->jbn = 0;
}

/* a CP of V.90 as the CPt / CPu of V.92 (Table 23: the same fields) */
void v90a__cp_to_cpu(const v90_cp_t *cp, int type, v92_cpu_t *u)
{
    memset(u, 0, sizeof *u);
    u->type = (uint8_t)type;
    u->drn = cp->drn; u->sr = cp->sr; u->ack = cp->ack; u->alaw = cp->alaw; u->ld = cp->ld;
    u->rms_ratio = cp->rms_ratio;
    u->a1 = cp->a1; u->a2 = cp->a2; u->b1 = cp->b1; u->b2 = cp->b2;
    memcpy(u->idx, cp->idx, sizeof u->idx);
    u->codec_differs = cp->codec_differs;
    memcpy(u->mask, cp->mask, sizeof u->mask);
    memcpy(u->codec_mask, cp->codec_mask, sizeof u->codec_mask);
}

/* ---- the DIL's analysis and the CPt / CP designs, as jobs ----
 *
 * Each runs a step a tick (v90dil_analyse_step, v90dil_design_step: some
 * tens of thousands of ARM instructions each; done whole they made one
 * tick of 9 M, later 1.1 M, and of 0.6 to 0.9 M): the DIL's end asks for S
 * at once, as before, and the analysis and CPt's design follow in the next
 * ticks (some 60, 8 ms); CP's design likewise from TRN2d's noise. Where a
 * result is wanted before its job is done (our S, S-bar has gone out; Ri-bar
 * has come; our SCR waited 2 s) the rest runs at once. A tick in which the
 * receiver took a step of its equaliser's design takes none. */
static void job_cpt(v90a_t *a);

void v90a__job_step(v90a_t *a)
{
    switch (a->job) {
    case JOB_DIL:
        if (v90dil_analyse_step(&a->jw.an, &a->res)) job_cpt(a);
        break;
    case JOB_CPT:
        if (v90dil_design_step(&a->jw.cp, &a->res, &a->cpt, &a->cpt_ch)) {
            a->job = JOB_NONE;
            if (a->jw.cp.k == 0 || v90_map_from_cp(&a->tmap, &a->cpt) != 0) { v90a__end(a, V90A_ERR_RATE); return; }
            if (a->pcm) { v90a__cp_to_cpu(&a->cpt, V92_UP_CPT, &a->cpt92); a->cpt_ready = 1; }   /* V.92: CPt goes out next */
        }
        break;
    case JOB_CP:
        if (v90dil_design_step(&a->jw.cp, &a->res, &a->cp, &a->cp_ch)) {
            a->job = JOB_NONE;
            if (a->jw.cp.k == 0 || v90_map_from_cp(&a->dmap, &a->cp) != 0) {
                /* nothing fits: CP with drn = 0 is the cleardown (8.5.2; V.92: CPu, 9.11) */
                a->clearing = 1; a->error = V90A_ERR_RATE; a->cpus_left = CLEAR_CPS;
            }
            a->cp_ready = 1;
        }
        break;
    default:
        a->job = JOB_NONE;
        break;
    }
}

/* the rest of a job (and of the CPt design after the analysis), now */
void v90a__job_finish(v90a_t *a, int kind)
{
    while (a->job != JOB_NONE && (kind == JOB_NONE || a->job <= kind) && a->state != V90A_ENDED) v90a__job_step(a);
}

/* the analysis done: what it says, then CPt's design */
static void job_cpt(v90a_t *a)
{
    v90dil_req_t q;
    int s;
    int32_t L = v90_ucode_linear(a->alaw, (unsigned)a->cfg.uinfo) * (1 << YF);
    a->job = JOB_NONE;
    if (a->cfg.table1) {
        int u;
        for (s = 0; s < 6; s++) for (u = 0; u < V90DIL_NU; u++) a->res.lv[s][u] = v90_ucode_linear(a->alaw, (unsigned)u) * (1 << YF);
    }
    /* UINFO itself must have arrived where training put it */
    for (s = 0; s < 6; s++) {
        int32_t d = a->res.lv[s][a->cfg.uinfo] - L;
        if (d < 0) d = -d;
        if (d > L / 4) { v90a__end(a, V90A_ERR_DIL); return; }
    }
    memset(&q, 0, sizeof q);
    q.alaw = a->alaw; q.is_cp = 0; q.z_q4 = V90A_Z_CPT;
    q.max_k = 16;                       /* CPt carries MP: 29333 bit/s at most, wide apart */
    q.max_tx_code = a->p2.info0d.max_tx_power;
    for (s = 0; s < 6; s++) q.sigma2_q8[s] = a->sig_jd[s];
    q.up_rates = a->up_rates;
    v90dil_design_begin(&a->jw.cp, &a->res, &q, &a->cpt, &a->cpt_ch);
    a->job = JOB_CPT;
}

/* enough of the DIL: S, S-bar to end it (9.3.2.10), and what it says, CPt
 * (as jobs) */
void v90a__dil_finish(v90a_t *a)
{
    a->acc_on = 0;
    a->dil_passes = v90dil_acc_min(&a->acc);
    v90dil_analyse_begin(&a->jw.an, &a->acc, a->alaw, a->cfg.uinfo, &a->res);
    a->job = JOB_DIL; a->job_new = 1;
    a->t_dil_end = a->now;
    a->ds = D_DILEND;
}

static void dil_dec(v90a_t *a, const v90arx_dec_t *d)
{
    int32_t m = d->m;
    if (a->dil_m0 < 0 || m < a->dil_m0 || (a->dil_end >= 0 && m >= a->dil_end)) return;
    if (a->dsf[m & 63]) {
        int mn = a->acc_on ? v90dil_acc_min(&a->acc) : 0;
        a->seg_m = m;
        /* every level measured twice: the receiver may learn again, on
         * those levels (at 100 ppm what the band edge aliases turns by a
         * fifth of a sample in one pass) */
        if (a->acc_on && mn >= 2 && a->rx.hold) v90arx_hold(&a->rx, 0);
        if (a->acc_on && mn >= DIL_MIN) { v90a__dil_finish(a); if (a->state == V90A_ENDED) return; }
    }
    if (a->acc_on) v90dil_acc_add(&a->acc, d->slot, a->dsu[m & 63], a->dsp[m & 63], d->y);
}

static void ri_dec(v90a_t *a, const v90arx_dec_t *d)
{
    int32_t m = d->m;
    int k, r = 0;
    if (a->hold_until >= 0 && m >= a->hold_until) { v90arx_hold(&a->rx, 0); a->hold_until = -1; }
    if ((m - a->ri_m0) % 6 != 5 || m < a->ri_m0 + 11) return;   /* the first unit had a DIL symbol fed back */
    for (k = 0; k < 6; k++) r += a->rsg[(m - 5 + k) & 63] == (k < 3);
    if (r >= 5) { if (++a->ri_units >= 2 && a->pcm) a->ri_seen = 1; return; }
    if (r <= 1 && a->ri_units >= 2) {
        /* Ri-bar, 24 T; TRN2d after it with CPt's constellations (8.6.5) */
        v90a__job_finish(a, JOB_CPT);
        if (a->state == V90A_ENDED) return;
        a->ribar_m0 = m - 5; a->m_trn2d = a->ribar_m0 + 24; a->t_ribar = a->now;
        a->ribar_seen = 1;
        v90a__set_cons(a, &a->cpt, a->m_trn2d);
        /* the DIL's levels are measured: from here the equaliser leaves the
         * band edge's turning image out (v90arx_edge), TRN2d's noise, which
         * CP is designed on, included */
        v90arx_edge(&a->rx, 1);
        v90arx_timing_hold(&a->rx, 0);
        v90adec_init(&a->dec, &a->tmap);
        v90_scr_init(&a->dscr);
        a->mbn = a->msh.ones = 0; a->got_mp = a->got_mpa = a->after_mpa = a->got_ed = 0;
        a->trn2d_syms = 0;
        if (a->pcm) {
            /* a Phase 4: its first CPd must carry all three parts */
            v92aseq_phase4(&a->aseq); a->got_suvd = a->got_cpd = a->got_ack = 0;
        }
        a->ds = D_TRAIN;
    }
}

/* CP from the noise the receiver shows on TRN2d (a job) */
static void design_cp(v90a_t *a)
{
    v90dil_req_t q;
    int s;
    memset(&q, 0, sizeof q);
    q.alaw = a->alaw; q.is_cp = 1; q.z_q4 = a->cfg.z_q4;
    q.max_tx_code = a->p2.info0d.max_tx_power;
    /* data mode shows 2 to 4 dB more error than TRN2d with CPt's sparser
     * points (measured): the noise taken as twice what TRN2d shows */
    for (s = 0; s < 6; s++) {
        uint64_t v = 2 * v90arx_mse_slot(&a->rx, s);
        q.sigma2_q8[s] = v > 0xffffffffu ? 0xffffffffu : (uint32_t)v;
        a->dsn[s] = q.sigma2_q8[s];         /* what the receiver's watch compares with */
    }
    q.rates = a->jd.rates; q.max_bps = a->cfg.max_rx_bps; q.up_rates = a->up_rates;
    v90a__job_finish(a, JOB_NONE);            /* (none is under way by now) */
    if (a->state == V90A_ENDED) return;
    v90dil_design_begin(&a->jw.cp, &a->res, &q, &a->cp, &a->cp_ch);
    a->job = JOB_CP; a->job_new = 1;
}

static void deliver(v90a_t *a, const uint8_t *bits, int n)
{
    int i;
    if (!a->cfg.putbit) return;
    for (i = 0; i < n; i++) a->cfg.putbit(a->cfg.user, bits[i]);
}

/* one MP bit (TRN2d, MP, MP', Ed: descrambled) */
static void mp_bit(v90a_t *a, int bit)
{
    if (a->mbn > 0) {
        a->mb[a->mbn++] = (uint8_t)bit;
        if (a->mbn == V90_MP0_BITS || a->mbn == V90_MP1_BITS) {
            v90_mp_t mp;
            int r = v90_mp_parse(a->mb, a->mbn, &mp);
            if (r > 0) {
                a->mp = mp;
                if (mp.ack) { a->got_mpa = 1; a->after_mpa = 1; } else a->got_mp = 1;
                if (a->t_mp == CLK_NONE) a->t_mp = a->now;
                a->mbn = 0; a->msh.ones = 0;
                if (mp.max_rate == 0) v90a__end(a, a->clearing ? V90A_ERR_HANGUP : V90A_ERR_CLEARDOWN);   /* 9.7 */
            } else if (r != V90_ERR_SHORT || a->mbn >= V90_MP1_BITS) {
                a->mbn = 0; a->msh.ones = 0;
            }
        }
        return;
    }
    if (seqhunt_bit(&a->msh, bit)) { memset(a->mb, 1, 17); a->mb[17] = 0; a->mbn = 18; }
}

/* a whole data frame of the downstream */
static void frame(v90a_t *a, int32_t m0)
{
    uint8_t bits[V90_MAX_D];
    int D = a->dec.D, i, ones = 0;
    (void)v90adec_frame(&a->dec, a->fu, a->fp, bits);
    v90_descramble_gpc_bits(&a->dscr, bits, D);
    for (i = 0; i < D; i++) ones += bits[i];
    switch (a->ds) {
    case D_TRAIN:
        if (a->pcm && a->after_mpa != 3) { v90a__train92(a, bits, D, m0); break; }
        if (a->after_mpa == 2) {
            if (ones == 0) {
                /* Ed (8.6.2): two frames of zeros after MP'; B1d after them */
                a->got_ed = 1; a->t_ed = a->now;
                a->m_b1d = m0 + 12;
                v90a__set_cons(a, &a->cp, a->m_b1d);
                a->after_mpa = 3;
                return;
            }
            a->after_mpa = 0;
        } else if (a->after_mpa == 3) {
            /* the second frame of Ed: B1d next, everything from zero (8.6.1) */
            v90adec_init(&a->dec, &a->dmap);
            v90_scr_init(&a->dscr);
            a->b1d_frames = a->b1d_bad = 0;
            a->ds = D_B1D;
            return;
        }
        for (i = 0; i < D && a->state != V90A_ENDED; i++) mp_bit(a, bits[i]);
        if (a->t_mp == CLK_NONE && a->mbn == 0) {
            /* TRN2d: scrambled ones, the descrambler in step from the start */
            a->trn2d_frames++;
            if (ones != D) a->trn2d_bad++;
        }
        if (a->after_mpa == 1) a->after_mpa = 2;
        break;
    case D_B1D:
        a->b1d_bad += D - ones;
        if (++a->b1d_frames >= 48) {
            if (a->b1d_bad * 8 > 48 * D) { v90a__end(a, V90A_ERR_B1D); return; }
            a->ds = D_DATA; a->rx_ready = 1;
            a->down_rate = (a->cp.drn + 20) * 4000 / 3;
            if (a->state == V90A_P4) a->since_auto = AUTO_GRACE;
            if (a->down_rate > a->best_rate) a->best_rate = a->down_rate;
            a->mse_ref = (uint64_t)a->rx.pq;   /* (the equaliser's input power) */
            a->rd_frames = 0; a->bad_syms = 0;
            a->deg_frames = a->good_frames = 0;
            a->dly_cnt = 0;
        }
        break;
    case D_DATA: {
        /* Rd (8.6.4): the largest point of every interval, + + + - - - or
         * the inverse. Delivery runs two frames late so that Rd is never
         * handed on as data. */
        int k, pol = 0;
        for (k = 0; k < 6; k++) if (a->fu[k] != a->dmap.C[k][0]) break;
        if (k == 6) {
            int p = 1, q = 1;
            for (k = 0; k < 6; k++) { if (a->fp[k] != (k < 3)) p = 0; if (a->fp[k] != (k >= 3)) q = 0; }
            pol = p ? 1 : q ? -1 : 0;
        }
        if (pol && (a->rd_frames == 0 || pol == a->rd_pol)) { a->rd_pol = pol; a->rd_frames++; }
        else a->rd_frames = 0;
        if (a->rd_frames >= 2 && !a->col_frames) {      /* (a lost receiver decides anything) */
            /* 9.6.2.2: the digital modem renegotiates; S, S-bar, CP from us.
             * V.92 (9.8.2.2): our Ru waits for its Rd-bar. Of the two frames
             * held back the newer is the first Rd; the older is data (unless
             * Rd ran on while the receiver was lost, rd_frames > 2) and goes
             * on now, as B1d starts the delay afresh */
            if (a->rd_frames == 2 && a->dly_cnt >= 2) {
                k = a->dly_pos & 1;
                deliver(a, a->dly[k], a->dly_n[k]);
            }
            a->rx_ready = 0; a->tx_ready = 0;
            a->rd_units = 2;
            if (!a->pcm && a->reneg_ours) { if (a->state != V90A_RENEG) { a->state = V90A_RENEG; a->t = 0; } }
            else if (!a->pcm) { a->state = V90A_RENEG; a->t = 0; go_s(a); }
            else if (a->state != V90A_RENEG) { a->state = V90A_RENEG; a->t = 0; }
            a->ds = D_RD;
            return;
        }
        k = a->dly_pos & 1;
        if (a->dly_cnt >= 2) deliver(a, a->dly[k], a->dly_n[k]);
        memcpy(a->dly[k], bits, (size_t)D);
        a->dly_n[k] = (uint8_t)D;
        a->dly_pos ^= 1;
        if (a->dly_cnt < 2) a->dly_cnt++;
        break;
    }
    default:
        break;
    }
}

static void frame_dec(v90a_t *a, const v90arx_dec_t *d)
{
    int32_t mref = a->ds == D_TRAIN ? a->m_trn2d : a->m_b1d, k;
    if (d->m < mref) return;
    k = (d->m - mref) % 6;
    a->fu[k] = d->ucode; a->fp[k] = d->sign;
    if (a->ds == D_TRAIN && ++a->trn2d_syms == TRN2D_MEAS && !a->cp_ready) design_cp(a);
    if (a->ds == D_DATA) {
        /* the downstream gone: the equaliser's input 12 dB under what it was
         * for 0.5 s (its error says nothing: with nothing coming in, the
         * decision feedback goes on deciding its own echo) */
        if ((uint64_t)a->rx.pq < a->mse_ref / 16) { if (++a->bad_syms > LOSS_SYMS) { v90a__end(a, V90A_ERR_CARRIER); return; } }
        else a->bad_syms = 0;
    }
    if (k == 5) {
        frame(a, d->m - 5);
        if (a->ds == D_DATA && a->state == V90A_DATA) v90a__line_watch(a);
    }
}

/* the digital modem's Rd, Rd-bar in a renegotiation: TRN2d (CPt's points),
 * then MP ... as in Phase 4 */
static void rd_dec(v90a_t *a, const v90arx_dec_t *d)
{
    int32_t m = d->m;
    int k, p = 0, q = 0;
    if ((m - a->m_b1d) % 6 != 5) return;
    for (k = 0; k < 6; k++) {
        int s = a->rsg[(m - 5 + k) & 63];
        p += s == (k < 3); q += s == (k >= 3);
    }
    if ((a->rd_pol > 0 ? q : p) >= 5) {
        a->m_trn2d = m - 5 + 24;
        v90a__set_cons(a, &a->cpt, a->m_trn2d);
        v90adec_init(&a->dec, &a->tmap);
        v90_scr_init(&a->dscr);
        a->mbn = a->msh.ones = 0; a->got_mp = a->got_mpa = a->after_mpa = a->got_ed = 0;
        a->trn2d_syms = TRN2D_MEAS;     /* CP stays as it was */
        if (a->reneg_ours) { a->trn2d_syms = 0; a->cp_ready = 0; }   /* ours: designed on TRN2d's noise */
        if (a->pcm) {
            /* 9.8.2.2.2: Ru on our next data frame boundary; the CPd in
             * use stays (v92aseq_restart) */
            v92aseq_restart(&a->aseq); a->got_suvd = a->got_cpd = a->got_ack = 0;
            if (a->us == P_DATA) a->ru_due = 1;
        }
        a->ds = D_TRAIN;
    }
}

/* 2^30 symbols into the call: the symbol numbers back (v90arx_rebase) */
void v90a__rebase(v90a_t *a)
{
    v90arx_rebase(&a->rx);
    a->jdp_m = clk_back(a->jdp_m);
    a->dil_m0 = clk_back(a->dil_m0); a->dil_end = clk_back(a->dil_end); a->dil_mnext = clk_back(a->dil_mnext);
    a->seg_m = clk_back(a->seg_m); a->cb_seg = clk_back(a->cb_seg);
    a->ri_m0 = clk_back(a->ri_m0); a->ribar_m0 = clk_back(a->ribar_m0);
    a->m_trn2d = clk_back(a->m_trn2d); a->m_b1d = clk_back(a->m_b1d); a->hold_until = clk_back(a->hold_until);
}

void v90a__on_dec(v90a_t *a, const v90arx_dec_t *d)
{
    a->ry[d->m & 63] = d->y; a->rsg[d->m & 63] = d->sign;
    switch (a->ds) {
    case D_JD: if (a->pcm) v90a__jd92_bit(a, d); else jd_bit(a, d); break;
    case D_DIL: case D_DILEND: dil_dec(a, d); break;
    case D_RI: ri_dec(a, d); break;
    case D_TRAIN: case D_B1D: case D_DATA: frame_dec(a, d); break;
    case D_RD: rd_dec(a, d); break;
    default: break;
    }
}
