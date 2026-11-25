/* V.90 analogue modem: Phase 3's start and the V.34 upstream of V.90.
 * See v90a.h, v90a_priv.h. */
#include <string.h>
#include "v90a_priv.h"

/* Phase 2 is over: INFO1a has gone (9.3.2.1) */
void v90a__start_p3(v90a_t *a)
{
    v90arx_cfg_t rc;
    int num, den, k, sr = a->p2.sym_rate;
    if (a->p2.pcm_up) { v90a__start_p3_v92(a); return; }
    a->alaw = a->p2.info0d.alaw != 0;
    a->t_info1a = a->now;
    v34_carrier(a->p2.sym_idx, a->p2.tx_hi_carrier, &num, &den);
    if (v34mod_init(&a->mod, sr, num, den, a->p2.tx_preemph, a->cfg.level, a->p2.tx_pwr_red) != 0) { v90a__end(a, V90A_ERR_PHASE2); return; }
    memset(&rc, 0, sizeof rc);
    rc.alaw = a->alaw; rc.uinfo = a->cfg.uinfo;
    rc.known_lv = v90a__dil_known; rc.known_ctx = a;
    v90arx_init(&a->rx, &rc);
    a->rx_on = 1;
    v90dil_design(&a->dil, a->cfg.uinfo);
    for (k = 0; k < 13; k++) {          /* CP bits 36:48: bit k = 4800 + 2400 k bit/s */
        v34_map_t mp;
        int rate = 4800 + 2400 * k;
        if (a->cfg.max_tx_bps > 0 && rate > a->cfg.max_tx_bps) break;
        if (v34_map_params((unsigned)sr, (unsigned)rate, 0, &mp) == 0) a->up_rates |= (uint16_t)(1u << k);
    }
    a->n_md = a->cfg.md_len * 35 * sr / 1000;
    a->n_trn = TRN_SYMS;
    a->md_rng = 0x2545F491u;
    a->ds = D_SD;
    ugo(a, U_SIL);
    a->state = V90A_P3; a->t = 0;
}

/* ---- upstream ---- */

/* a sequence built one bit per octet, kept packed */
void v90a__load_bits(v90a_t *a, const uint8_t *bits, int n)
{
    a->frn = n > 0 ? n : 0; a->frpos = 0;
    memset(a->fr, 0, sizeof a->fr);
    v90_pack_bits(bits, a->frn, a->fr);
}

static void load_cp(v90a_t *a, const v90_cp_t *cp, int ack)
{
    v90_cp_t c = *cp;
    uint8_t bits[V90_CP_MAX_BITS];
    c.ack = (uint8_t)(ack != 0);
    v90a__load_bits(a, bits, v90_cp_build(&c, bits));
    a->fr_ack = ack != 0;
}


/* the next two bits of the sequence being sent; Ja wraps (it may stop anywhere) */
static unsigned bits2(v90a_t *a, int wrap)
{
    unsigned b0 = 0, b1 = 0;
    if (wrap && a->frpos >= a->frn) a->frpos = 0;
    if (a->frpos < a->frn) b0 = fr_bit(a, a->frpos++);
    if (wrap && a->frpos >= a->frn) a->frpos = 0;
    if (a->frpos < a->frn) b1 = fr_bit(a, a->frpos++);
    return b0 | (b1 << 1);
}

/* E is over: B1 and data at the rate MP allows (9.4.2.4) */
static void start_data(v90a_t *a)
{
    v34_cfg_t c;
    unsigned common = (unsigned)a->mp.rates & a->up_rates;
    int k;
    a->up_rate = 0;
    for (k = 12; k >= 0; k--)
        if ((common >> k) & 1 && 4800 + 2400 * k <= a->mp.max_rate * 2400) { a->up_rate = 4800 + 2400 * k; break; }
    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)a->p2.sym_rate; c.data_rate = (uint16_t)a->up_rate;
    c.trellis = a->mp.trellis; c.expanded = a->mp.shaping; c.nonlinear = a->mp.nonlinear;
    c.scrambler = V34_GPA;
    if (a->mp.type) memcpy(c.h, a->mp.h, sizeof c.h);
    if (!a->up_rate || v34_params_init(&a->vpar, &c) != 0) { v90a__end(a, V90A_ERR_RATE); return; }
    v34_tx_init(&a->vtx, &a->vpar, a->cfg.getbit, a->cfg.user);
    a->tx_dg = (int32_t)v34_gain_q14(a->vpar.energy_tx_q8);
    ugo(a, U_DATA);
}

/* one upstream symbol, in the modulator's units */
void v90a__up_symbol(v90a_t *a, int32_t *re, int32_t *im)
{
    v34_pt_t pt;
    int32_t g = 16384;
    int n = (int)a->un;
    *re = *im = 0;
    pt.re = pt.im = 0;
    switch (a->us) {
    case U_SIL:
        if (++a->un >= a->p2.sym_rate * 70 / 1000) ugo(a, U_S1);    /* 9.3.2.1: 70 ms */
        return;
    case U_S1: case U_S2: case U_S4: case U_RS:
        v34_s(n, V34_S_LEN, re, im);
        if (++a->un >= V34_S_LEN)
            ugo(a, a->us == U_S1 ? U_SB1 : a->us == U_S2 ? U_SB2 : a->us == U_S4 ? U_SB4 : U_RSB);
        return;
    case U_SB1: case U_SB2: case U_SBJ: case U_SB4: case U_RSB:
        v34_sbar(n, re, im);
        if (++a->un < V34_SBAR_LEN) return;
        if (a->us == U_SB1) {
            if (a->n_md > 0) {
                /* MD: white noise, the canceller learns (v90a.h) */
                ugo(a, U_MD);
                a->md_on = 1; a->md_samples = 0;
                a->md_total = a->n_md * 8000 / a->p2.sym_rate;
                a->erle_rx = a->erle_res = 0;
            } else ugo(a, U_PP);
        } else if (a->us == U_SB2) ugo(a, U_PP);
        else if (a->us == U_SBJ) ugo(a, U_DILQ);
        else if (a->us == U_SB4) {
            /* Phase 4: scrambler and differential encoder from zero (8.5.2) */
            v90a__job_finish(a, JOB_CPT);
            if (a->state == V90A_ENDED) return;
            v34_tmod_init(&a->tmod, V34_GPA, 4);
            load_cp(a, &a->cpt, 0);
            a->state = V90A_P4;
            ugo(a, U_CPT);
        } else {                        /* U_RSB: renegotiation or cleardown, CP follows */
            a->cp_ack_sent = 0; a->cps_sent = 0;
            if (a->reneg_ours && !a->clearing) {
                /* ours (9.6.2.1.2): SCR until the digital modem's TRN2d has
                 * shown the noise CP is designed on */
                ugo(a, U_SCR);
                return;
            }
            if (a->clearing) { v90_cp_t c = a->cp; c.drn = 0; load_cp(a, &c, 0); }
            else load_cp(a, &a->cp, 0);
            ugo(a, U_CP);
        }
        return;
    case U_MD:
        if (++a->un >= a->n_md) {
            a->md_on = 0;
            v90arx_ec(&a->rx, -1);
            a->erle_db10 = probe_db10(a->erle_rx + 1, a->erle_res + 1);
            ugo(a, U_S2);
        }
        return;
    case U_PP:
        v34_pp(n, re, im);
        if (++a->un >= V34_PP_LEN) { v34_tmod_init(&a->tmod, V34_GPA, 4); ugo(a, U_TRN); }
        return;
    case U_TRN:
        pt = v34_tmod_trn(&a->tmod);
        if (++a->un >= a->n_trn) {
            uint8_t bits[V90_DIL_MAX_BITS];
            v90a__load_bits(a, bits, v90_dil_build(&a->dil, bits));
            a->t_ja = a->now;
            ugo(a, U_JA);
        }
        break;
    case U_JA:
        pt = v34_tmod_put(&a->tmod, bits2(a, 1));
        a->un++;
        break;
    case U_QUIET: case U_OFF:
        return;
    case U_DILQ:
        if (a->t_dil_end != CLK_NONE) { ugo(a, U_S4); v90a__up_symbol(a, re, im); }
        return;
    case U_SJ:
        /* S until Jd', ending on its rotated point (then S-bar) */
        v34_s(n & 1, 2, re, im);
        a->un++;
        if (a->t_jdp != CLK_NONE && a->un >= 64 && (a->un & 1) == 0) ugo(a, U_SBJ);
        return;
    case U_CPT:
        if (a->frpos >= a->frn) {
            if (a->ribar_seen) { ugo(a, U_SCR); v90a__up_symbol(a, re, im); return; }   /* 9.4.2.2 */
            load_cp(a, &a->cpt, 0);
        }
        pt = v34_tmod_put(&a->tmod, bits2(a, 0));
        break;
    case U_SCR:
        /* our renegotiation: no TRN2d in 2000 ms, CP as it was */
        if (a->reneg_ours && ++a->un >= a->p2.sym_rate * T_SCR_MS / 1000) { v90a__job_finish(a, JOB_CP); a->cp_ready = 1; }
        if (a->cp_ready) { load_cp(a, &a->cp, a->got_mp || a->got_mpa); ugo(a, U_CP); v90a__up_symbol(a, re, im); return; }
        pt = v34_tmod_put(&a->tmod, 3);
        break;
    case U_CP:
        if (a->frpos >= a->frn) {
            if (a->clearing) {
                if (++a->cps_sent >= CLEAR_CPS) { v90a__end(a, V90A_ERR_HANGUP); return; }
                { v90_cp_t c = a->cp; c.drn = 0; load_cp(a, &c, 0); }
            } else {
                if (a->fr_ack) a->cp_ack_sent++;
                /* 9.4.2.4: a CP' sent and MP' or Ed received: E */
                if (a->cp_ack_sent >= 1 && (a->got_mpa || a->got_ed)) { ugo(a, U_E); v90a__up_symbol(a, re, im); return; }
                load_cp(a, &a->cp, a->got_mp || a->got_mpa);
            }
        }
        pt = v34_tmod_put(&a->tmod, bits2(a, 0));
        break;
    case U_E:
        pt = v34_tmod_put(&a->tmod, 3);         /* 20 ones */
        if (++a->un >= 10) start_data(a);
        break;
    case U_DATA: case U_DATA_END:
        if (a->us == U_DATA_END && a->vtx.pos >= 8) { ugo(a, U_RS); v90a__up_symbol(a, re, im); return; }
        if (a->us == U_DATA && !v34_tx_in_b1(&a->vtx)) a->tx_ready = 1;
        v34_tx_symbol(&a->vtx, &pt, NULL);
        g = a->tx_dg;
        break;
    default:
        return;
    }
    *re = (pt.re * g) >> 9; *im = (pt.im * g) >> 9;
}
