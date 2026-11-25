/* V.92 analogue modem: the PCM upstream and the sequences of V.92 in the
 * downstream. See v90a.h, v90a_priv.h. */
#include <string.h>
#include "v90a_priv.h"

/* ==== V.92: PCM upstream ==================================================
 *
 * Phase 3 (9.5.2): silence 70 ms, Ru 384 T, Ru-bar 24 T, TRN1u (the echo
 * canceller learns on it: the digital modem is silent, and TRN1u is our
 * widest signal), Ja (24 ones, the DIL descriptor with our upstream rates)
 * until Sd-bar is heard, to the next 12 bit boundary; silence; the
 * downstream receiver learns on Sd and TRN1d; Jd; Su 144 T, Su-bar 24.5 T,
 * Su until Jp, Su-bar (24 + Jp's fraction) T; TRN1u again (the data frame
 * count begins) while the DIL is measured; CPt (24 ones first) until Ri,
 * E1u.
 * Phase 4 (9.6.2): TRN2u until SUVd (or 12000 T), SUVu; one CPu after the
 * first SUVd (CP of V.90's design, from the DIL and TRN2d's noise), SUVu
 * again, the acknowledge bit once CPd is in; E2u once an acknowledged
 * sequence has gone out and SUVd' / CPd' or Ed has come in; B1u and data
 * through modem/v92_enc with what CPd dictates (precoder, prefilter, gain,
 * moduli, constellations), every symbol on the network clock (pcmtx).
 * Downstream as V.90 but for Jd / Jp / Jp' (8.6.2 to 8.6.4) and SUVd / CPd /
 * Ed in TRN2d (modem/v92aseq).
 * Rate renegotiation begun by the digital modem (9.8.2.2): after its Rd-bar,
 * Ru on our next data frame boundary, Ru-bar, TRN2u (2400 T or until SUVd),
 * then as Phase 4. Cleardown (9.11): the same with a CPu of drn = 0. */

void v90a__start_p3_v92(v90a_t *a)
{
    v90arx_cfg_t rc;
    int r;
    a->pcm = 1;
    a->alaw = a->p2.info0d.alaw != 0;
    a->t_info1a = a->now;
    a->lu = a->cfg.level > 0 ? a->cfg.level : 3000;
    memset(&rc, 0, sizeof rc);
    rc.alaw = a->alaw; rc.uinfo = a->cfg.uinfo;
    rc.known_lv = v90a__dil_known; rc.known_ctx = a;
    v90arx_init(&a->rx, &rc);
    v90arx_deaf(&a->rx, 1);             /* until Ja: our own Ru has Sd's period */
    v90arx_clock(&a->rx, a->p2.clk_ppm_q8);     /* the symbol clock from the start (6.2/V.92) */
    a->rx_on = 1;
    v90dil_design(&a->dil, a->cfg.uinfo);
    a->up92_rates = 0;
    for (r = 0; r < V92_UP_NUM_RATES; r++)
        if (a->cfg.max_tx_bps <= 0 || V92_UP_BPS(r + 1) <= a->cfg.max_tx_bps) a->up92_rates |= 1u << r;
    if (!a->up92_rates) a->up92_rates = 1;          /* 24000 bit/s is the least there is */
    pcmtx_init(&a->ptx);
    pcmtx_wide(&a->ptx, 1);             /* the canceller learns on the whole band (pcmtx.h) */
    v92_sgn_init(&a->sgn, 0);
    a->ds = D_SD;
    ugo(a, P_SIL);
    a->state = V90A_P3; a->t = 0;
}

/* Jd (Table 21) repeated, then Jp (Table 22) repeated, then Jp' (12 zeros):
 * the sign of UINFO, differentially, through the GPC descrambler that has run
 * since TRN1d began, as in V.90 */
void v90a__jd92_bit(v90a_t *a, const v90arx_dec_t *d)
{
    int c = d->sign ^ a->jprev, bit, kind;
    a->jprev = d->sign;
    bit = v90_descramble_gpc(&a->jscr, c);
    if (!a->jd_ok) {
        if (a->jbn > 0) {
            a->jb[a->jbn++] = (uint8_t)bit;
            if (a->jbn == V92_J_BITS) {
                if (v92_j_kind(a->jb, V92_J_BITS) == V92_J_JD && v92_jd_parse(a->jb, V92_J_BITS, &a->jd92) == V92_J_BITS) {
                    a->jd_ok = 1; a->t_jd = a->now; a->jdp_zeros = 0;
                    a->jd.rates = a->jd92.rates;
                    if (a->us == P_QUIET) { ugo(a, P_SU1); a->t_su = a->now; }    /* 9.5.2.1.6 */
                }
                a->jbn = 0; a->jsh.ones = 0;
            }
            return;
        }
        if (seqhunt_bit(&a->jsh, bit)) { memset(a->jb, 1, 17); a->jb[17] = 0; a->jbn = 18; }
        return;
    }
    /* in step with the sequences: more Jd, Jp, and after a Jp maybe Jp' */
    if (a->jbn == 0 && !bit && a->jp_ok) {
        a->jdp_m = d->m; a->jdp_zeros = 1;
        a->dil_m0 = d->m + 12; a->dil_mnext = a->dil_m0; a->dil_end = -1;
        v90dil_cur_init(&a->dcur);
        v90arx_hold(&a->rx, 1);
    } else if (a->jdp_zeros > 0) {
        if (bit) { a->jdp_zeros = 0; a->dil_m0 = -1; v90arx_hold(&a->rx, 0); }
        else if (++a->jdp_zeros == 12) {
            int s;
            a->t_jdp = a->now;
            for (s = 0; s < 6; s++) a->sig_jd[s] = (uint32_t)v90arx_mse_slot(&a->rx, s);
            v90dil_acc_init(&a->acc);
            a->acc_on = 1;
            a->ds = D_DIL;
            /* V.92: our transmitter runs on this loop's clock (6.2/V.92), and
             * the DIL's repetitive patterns, on levels still being measured,
             * pull the estimate the loop follows (0.01 to 0.03 sample on the
             * pad and robbed bit lines, back during TRN2d), moving our
             * symbols while the digital modem measures them for CPd. The
             * clock from L2 is within 0.1 ppm: the loop stands until Ri-bar
             * (docs/history/PROGRESS.md, robust 2b). V.90's clock comes from Sd (off by up
             * to 1.6 ppm) and nothing follows it: its loop runs on. */
            v90arx_timing_hold(&a->rx, 1);
            return;
        }
    }
    a->jb[a->jbn] = (uint8_t)bit;
    if (++a->jbn < V92_J_BITS) return;
    a->jbn = 0;
    kind = v92_j_kind(a->jb, V92_J_BITS);
    if (kind == V92_J_JP && !a->jp_ok && v92_jp_parse(a->jb, V92_J_BITS, &a->jp) == V92_J_BITS) {
        a->jp_ok = 1; a->t_jp = a->now;             /* 9.5.2.1.8: Su-bar with the fraction */
    }
}

/* a data frame of TRN2d (SUVd, CPd, Ed in it) */
void v90a__train92(v90a_t *a, const uint8_t *bits, int D, int32_t m0)
{
    v92_suvd_t sv;
    int ev, i, ones = 0;
    if (!a->got_suvd && !a->got_cpd && !a->aseq.in) {
        for (i = 0; i < D; i++) ones += bits[i];
        a->trn2d_frames++;
        if (ones != D) a->trn2d_bad++;
    }
    ev = v92aseq_frame(&a->aseq, bits, D, &sv, &a->cpd);
    if (ev & V92AS_SUVD) {
        a->got_suvd = 1;
        if (sv.ack) a->got_ack = 1;
        if (a->t_suvd == CLK_NONE) a->t_suvd = a->now;
    }
    if (ev & V92AS_CPD) {
        a->got_cpd = 1;
        if (a->cpd.ack) a->got_ack = 1;
        if (a->t_mp == CLK_NONE) a->t_mp = a->now;
        if (a->cpd.drn == 0) {                      /* 9.11: the digital modem clears down */
            if (a->clearing) { v90a__end(a, V90A_ERR_HANGUP); return; }
            /* 9.6.2.1.2: what we send from now on carries the acknowledge
             * bit; seq_next ends the call once it has been answered, or
             * after 100 ms + a round trip (9.6.2.1.3's wait for the
             * answer, as our CPu repeat) */
            if (a->got_cpd != 2) { a->got_cpd = 2; a->cpus_left = MS(100) + a->p2.rtd + 600; }
        }
    }
    if ((ev & V92AS_ED) && (a->got_suvd || a->got_cpd)) {
        /* Ed (8.8.2): this frame and the next; B1d after them, with our CPu */
        a->got_ed = 1; a->t_ed = a->now;
        a->m_b1d = m0 + 12;
        v90a__set_cons(a, &a->cp, a->m_b1d);
        a->after_mpa = 3;
    }
}

/* ---- the upstream symbols ---- */

static int16_t two(v90a_t *a, int sbit)
{
    a->sbit = sbit;
    return (int16_t)(sbit ? -a->lu : a->lu);
}

static void start_trn2u(v90a_t *a, int reneg)
{
    v92_ml_init(&a->ml, reneg ? a->jp.pt8_reneg : a->jp.pt8_train, a->sbit);
    a->in_reneg = reneg;
    a->cpu_sent = a->ack_sent = a->seq_ack = 0;
    ugo(a, P_TRN2U);
}

static int enc_get(void *user)
{
    v90a_t *a = (v90a_t *)user;
    if (a->b1_bits > 0) { if (--a->b1_bits == 0) a->tx_ready = 1; return 1; }    /* B1u: ones (8.7.1) */
    return a->cfg.getbit ? a->cfg.getbit(a->cfg.user) & 1 : 1;
}

/* E2u is over: B1u and data with CPd's parameters (8.7.1), as stored: the
 * parts a later CPd left out are the earlier ones' (v92aseq) */
static void start_b1u(v90a_t *a)
{
    const v92_cpd_t *c = &a->cpd;
    if (!a->got_cpd || c->drn < 1 || c->drn > V92_UP_NUM_RATES || !((a->up92_rates >> (c->drn - 1)) & 1)
        || !v92aseq_cpd_complete(&a->aseq) || v92_map_check(&c->map) != 0 || v92_filt_check(&c->filt) != 0) {
        v90a__end(a, V90A_ERR_CPD);
        return;
    }
    a->encm = c->map;
    v92_filt_trim(&a->encf, &c->filt);
    if (v92_enc_init(&a->enc, &a->encm, &a->encf, enc_get, a) != 0) { v90a__end(a, V90A_ERR_CPD); return; }
    a->b1_bits = (int32_t)V92_B1U_FRAMES * c->map.K;
    a->up_rate = V92_UP_BPS(c->drn);
    a->tx_ready = 0;
    ugo(a, P_DATA);
}

/* the next sequence in the TRN2u modulation (9.6.2.1.2 to .4, 9.11) */
static void seq_next(v90a_t *a)
{
    uint8_t bits[V92_CPU_MAX_BITS];
    int unit = a->ml.nbits * 12, n;
    if (a->seq_ack) a->ack_sent = 1;
    a->seq_ack = 0;
    if (a->got_cpd == 2) {
        /* the digital modem's cleardown: 9.6.2.1.4's point (we have sent a
         * sequence with the acknowledge bit and received one, or Ed) ends
         * it. There 9.6.2.1.4 sends E2u, but E2u leads to B1u and data
         * (9.6.2.1.5), which drn = 0 rules out: silence instead, after one
         * more sequence (what our transmitter still holds of the
         * acknowledging one then leaves the line whole). Without the
         * answer, the same once cpus_left symbols have gone out. */
        a->cpus_left -= a->frn / a->ml.nbits;
        if (a->cpus_left <= 0) { v90a__end(a, V90A_ERR_CLEARDOWN); return; }
        if (a->ack_sent && (a->got_ack || a->got_ed)) a->cpus_left = 1;
    }
    if (a->clearing) {
        /* CPu with drn = 0, CLEAR_CPS of them, then silence */
        v92_cpu_t u;
        if (a->cpus_left-- <= 0) { v90a__end(a, V90A_ERR_HANGUP); return; }
        v90a__cp_to_cpu(&a->cp, V92_UP_CPU, &u);
        u.drn = 0; u.ack = 0;
        v90a__load_bits(a, bits, v92_cpu_build(&u, unit, bits));
        return;
    }
    if (a->ack_sent && (a->got_ack || a->got_ed) && a->got_cpd != 2) { ugo(a, P_E2U); return; }
    if (a->got_suvd && a->cp_ready && (!a->cpu_sent || (!a->got_ack && a->since_cpu > MS(100) + a->p2.rtd + 600))) {
        v92_cpu_t u;
        v90a__cp_to_cpu(&a->cp, V92_UP_CPU, &u);
        u.ack = (uint8_t)(a->got_cpd != 0);
        n = v92_cpu_build(&u, unit, bits);
        v90a__load_bits(a, bits, n);
        a->cpu_sent = 1; a->since_cpu = -(n / a->ml.nbits);    /* counted from its end */
        a->seq_ack = u.ack;
        return;
    }
    {
        v92_suvu_t v;
        v.wait_cpu = 0; v.level = V92_SUVU_NO_LEVEL; v.silence = 0; v.ack = (uint8_t)(a->got_cpd != 0);
        v90a__load_bits(a, bits, v92_suvu_build(&v, unit, bits));
        a->seq_ack = v.ack;
    }
}

static int16_t ml_sym(v90a_t *a, const uint8_t *b)
{
    int lvl = v92_ml_tx(&a->ml, b);
    return (int16_t)v92_trn2u_level(lvl, a->ml.nbits == 3, a->lu);
}

/* the next symbol; linear */
int16_t v90a__up92_symbol(v90a_t *a)
{
    int32_t n = a->un;
    int16_t v;
    a->un = clk_sat_inc(a->un);         /* (P_DATA counts on) */
    switch (a->us) {
    case P_SIL:
        if (a->un >= MS(70)) ugo(a, P_RU);                  /* 9.5.2.1.1 */
        return 0;
    case P_RU: case P_RRU:
        v = two(a, v92_ru_sbit((unsigned)n, 0));
        if (a->un >= 384) ugo(a, a->us == P_RU ? P_RUB : P_RRUB);
        return v;
    case P_RUB: case P_RRUB:
        v = two(a, v92_ru_sbit((unsigned)n, 1));
        if (a->un >= 24) {
            if (a->us == P_RRUB) start_trn2u(a, 1);
            else {
                v92_sgn_init(&a->sgn, 0);
                a->ec_n = 0; a->ec_len = TRN1U_SYMS;         /* the canceller learns (v90a_tick) */
                a->erle_rx = a->erle_res = 0;
                ugo(a, P_TRN1);
            }
        }
        return v;
    case P_TRN1:
        v = two(a, v92_sgn_tx_trn(&a->sgn));
        if (a->un >= TRN1U_SYMS) {
            uint8_t bits[V92_JA_MAX_BITS];
            v92_ja_t ja;
            ja.dil = a->dil; ja.rates = a->up92_rates;
            v90a__load_bits(a, bits, v92_ja_build(&ja, bits));
            a->t_ja = a->now; a->ja_stop = 0;
            v90arx_deaf(&a->rx, 0);     /* Sd may come now (9.5.2.1.3) */
            ugo(a, P_JA);
        }
        return v;
    case P_JA: {
        int bit = 1;
        if (n >= V92_JA_PREAMBLE) {
            if (a->frpos >= a->frn) a->frpos = 0;
            bit = (int)fr_bit(a, a->frpos++);
        }
        v = two(a, v92_sgn_tx_bit(&a->sgn, bit));
        if (a->ja_stop && a->un % 12 == 0) {
            ugo(a, P_QUIET); a->t_ja_end = a->now;
            pcmtx_wide(&a->ptx, 0);     /* silence: the low-pass from here on */
        }
        return v;
    }
    case P_QUIET:
        return 0;
    case P_SU1: case P_SU2:
        v = (int16_t)v92_su_level((unsigned)n, 0, a->lu);
        if (a->us == P_SU1 ? a->un >= 144 : a->jp_ok && a->un % 12 == 0) ugo(a, a->us == P_SU1 ? P_SUB1 : P_SUB2);
        return v;
    case P_SUB1: case P_SUB2:
        v = (int16_t)v92_su_level((unsigned)n, 1, a->lu);
        if (a->un >= 24) {
            if (a->us == P_SUB1) { a->slip = 0x8000; ugo(a, P_SU2); }      /* 24.5 T (9.5.2.1.7) */
            else {
                a->slip = a->jp.frac;                               /* (24 + fraction) T (9.5.2.1.8) */
                v92_sgn_init(&a->sgn, 0);
                ugo(a, P_TRN1B);
            }
        }
        return v;
    case P_TRN1B:
        v = two(a, v92_sgn_tx_trn(&a->sgn));
        if (a->un % 12 == 0 && a->un >= TRN1U2_MIN && a->cpt_ready) {
            uint8_t bits[V92_CPU_MAX_BITS];
            v90a__load_bits(a, bits, v92_cpu_build(&a->cpt92, 12, bits));
            a->t_cpt = a->now;
            ugo(a, P_CPT);
        }
        return v;
    case P_CPT: {
        int bit = 1;
        if (n >= 24) bit = (int)fr_bit(a, a->frpos++);       /* 8.5.1: 24 ones first */
        v = two(a, v92_sgn_tx_bit(&a->sgn, bit));
        if (n >= 24 && a->frpos >= a->frn) {
            if (a->ri_seen) ugo(a, P_E1U);                  /* 9.5.2.1.11: complete the current one */
            else a->frpos = 0;
        }
        return v;
    }
    case P_E1U:
        v = two(a, v92_sgn_tx_bit(&a->sgn, 0));
        if (a->un >= V92_E1U_BITS) { start_trn2u(a, 0); a->state = V90A_P4; }     /* Phase 4 (9.6.2) */
        return v;
    case P_TRN2U:
        v = ml_sym(a, NULL);
        if (a->un % 12 == 0 && (a->un >= (a->in_reneg ? TRN2U_RENEG : TRN2U_SYMS) || a->got_suvd)) {
            seq_next(a);
            if (a->state != V90A_ENDED && a->us == P_TRN2U) ugo(a, P_SEQ);
        }
        return v;
    case P_SEQ: {
        uint8_t b[3];
        int k;
        for (k = 0; k < a->ml.nbits; k++) b[k] = (uint8_t)(a->frpos < a->frn ? fr_bit(a, a->frpos++) : 0);
        v = ml_sym(a, b);
        a->since_cpu++;
        if (a->frpos >= a->frn) seq_next(a);
        return v;
    }
    case P_E2U: {
        static const uint8_t z[3] = { 0, 0, 0 };
        v = ml_sym(a, z);
        if (a->un >= 12 + (a->cpd.e2u_extend ? 1 : 0)) start_b1u(a);
        return v;
    }
    case P_DATA: {
        int64_t x;
        if (a->ru_due && a->enc.i == 0) {                   /* 9.8.2.1.1: on a data frame boundary */
            a->ru_due = 0;
            a->tx_ready = 0;
            ugo(a, P_RRU);
            return v90a__up92_symbol(a);
        }
        x = (int64_t)v92_enc_symbol(&a->enc, NULL) * a->lu;   /* G v(n), Q14: 1.0 is LU (reading 35) */
        x = (x + 8192) >> 14;
        return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
    default:
        return 0;
    }
}
