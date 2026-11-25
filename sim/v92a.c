/* Test peer: V.92 analogue modem, PCM upstream. See v92a.h. */
#include <string.h>
#include <math.h>
#include "v92a.h"
#include "../v90/v90_pcm.h"

static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }
static int  getbitm(const uint8_t *m, int u) { return (m[u >> 3] >> (u & 7)) & 1; }

static void cons(uint8_t *mask, int M, int top, int step)
{
    int k;
    memset(mask, 0, 16);
    for (k = 0; k < M; k++) setbit(mask, top - k * step);
}

/* CPu: D = drn + 20 = Kd + 6 - Sr */
static void build_cpu(v92a_t *p, int Kd)
{
    const v92a_cfg *cfg = &p->cfg;
    v90_cp_t v;
    int i;
    memset(&p->cpu, 0, sizeof p->cpu);
    p->cpu.type = V92_UP_CPU; p->cpu.drn = (uint8_t)(Kd + 6 - cfg->sr_d - 20);
    p->cpu.sr = (uint8_t)cfg->sr_d; p->cpu.ld = (uint8_t)cfg->ld_d; p->cpu.alaw = (uint8_t)cfg->alaw;
    p->cpu.rms_ratio = 0x2000;
    if (cfg->sr_d) { p->cpu.a1 = 60; p->cpu.a2 = -58; p->cpu.b1 = 10; p->cpu.b2 = -10; }
    for (i = 0; i < 6; i++) p->cpu.idx[i] = (uint8_t)(i % 3);
    cons(p->cpu.mask[0], cfg->Md, 106, 1);
    cons(p->cpu.mask[1], cfg->Md, 105, 1);
    cons(p->cpu.mask[2], cfg->Md, 107, 1);
    v92_cpu_to_v90(&p->cpu, &v);
    (void)v90_map_from_cp(&p->dmap, &v);
}

/* ------------------------------------------------------------------ */
/* upstream                                                            */

static void unext(v92a_t *p, v92a_ustate s) { p->us = s; p->un = 0; }

enum { F_JA, F_CPT, F_SUVU, F_CPU };
static void load_frame(v92a_t *p, int which, int ack)
{
    int unit = p->ml.nbits * 12;
    if (p->in_reneg && p->cfg.reneg_no_ack) ack = 0;
    p->frpos = 0; p->fr_kind = which; p->fr_ack = ack;
    if (which == F_JA) p->frn = v92_ja_build(&p->ja, p->fr);
    else if (which == F_CPT) p->frn = v92_cpu_build(&p->cpt, 12, p->fr);
    else if (which == F_CPU) {
        v92_cpu_t c = p->cpu;
        c.ack = (uint8_t)ack;
        if (p->cleardown) c.drn = 0;
        p->frn = v92_cpu_build(&c, unit, p->fr);
    } else {
        v92_suvu_t s;
        s.wait_cpu = (uint8_t)p->cfg.wait_cpu; s.level = V92_SUVU_NO_LEVEL; s.silence = 0; s.ack = (uint8_t)ack;
        p->frn = v92_suvu_build(&s, unit, p->fr);
    }
}

static int enc_get(void *u)
{
    v92a_t *p = (v92a_t *)u;
    if (p->b1_bits > 0) { if (--p->b1_bits == 0) p->tx_ready = 1; return 1; }     /* B1u: ones (8.7.1) */
    return p->cfg.getbit ? p->cfg.getbit(p->cfg.user) : 1;
}

static double two(v92a_t *p, int sbit) { p->last_sbit = sbit; return (double)V92_UP_SIGN(sbit); }

static void start_trn2u(v92a_t *p, int reneg)
{
    v92_ml_init(&p->ml, reneg ? p->jp.pt8_reneg : p->jp.pt8_train, p->last_sbit);
    unext(p, VA_TRN2U);
}

/* one upstream symbol; returns its amplitude in units of LU */
static double up_symbol(v92a_t *p)
{
    const v92a_cfg *cfg = &p->cfg;
    double a = 0;
    long n = p->un++;

    switch (p->us) {
    case VA_SIL0:
        if (p->un >= cfg->sil0) unext(p, VA_RU);
        break;
    case VA_RU: case VA_RRU:
        a = two(p, v92_ru_sbit((unsigned)n, 0));
        if (p->un >= 384) unext(p, p->us == VA_RU ? VA_RUBAR : VA_RRUBAR);
        break;
    case VA_RUBAR: case VA_RRUBAR:
        a = two(p, v92_ru_sbit((unsigned)n, 1));
        if (p->un >= 24) {
            if (p->us == VA_RRUBAR) start_trn2u(p, 1);
            else { v92_sgn_init(&p->sgn, 0); unext(p, VA_TRN1U); }
        }
        break;
    case VA_TRN1U:
        a = two(p, v92_sgn_tx_trn(&p->sgn));
        if (p->un >= cfg->ntrn1) { load_frame(p, F_JA, 0); unext(p, VA_JA); }
        break;
    case VA_JA: case VA_CPT: {
        int bit = 1;
        if (n >= 24 || (p->us == VA_JA && cfg->ja_no_ones)) { bit = p->fr[p->frpos++]; if (p->frpos >= p->frn) p->frpos = 0; }
        a = two(p, v92_sgn_tx_bit(&p->sgn, bit));
        if (p->us == VA_JA) {
            /* 9.5.2.1.3: stop at the next 12 bit boundary after Sd-bar */
            if (p->sdbar_seen && p->un % 12 == 0) unext(p, VA_SIL1);
        } else if (n >= 24 && p->frpos == 0 && (cfg->dil_n > 0 ? p->ri_seen : p->ribar_seen)) {
            unext(p, VA_E1U);                   /* the current CPt is complete */
        }
        break;
    }
    case VA_SIL1:
        if (!p->jd_ok) p->un = 0;
        else if (p->un >= cfg->jd_wait) unext(p, VA_SU1);
        break;
    case VA_SU1:
        a = (double)v92_su_level((unsigned)n, 0, 1 << 20) / (double)(1 << 20);
        if (p->un >= 144) unext(p, VA_SUBAR1);
        break;
    case VA_SUBAR1:
        a = (double)v92_su_level((unsigned)n, 1, 1 << 20) / (double)(1 << 20);
        if (p->un >= 24) { p->phase += 0.5; unext(p, VA_SU2); }                 /* 24.5 T (9.5.2.1.7) */
        break;
    case VA_SU2:
        a = (double)v92_su_level((unsigned)n, 0, 1 << 20) / (double)(1 << 20);
        if (p->jp_ok && p->un % 6 == 0) unext(p, VA_SUBAR2);
        break;
    case VA_SUBAR2:
        a = (double)v92_su_level((unsigned)n, 1, 1 << 20) / (double)(1 << 20);
        if (p->un >= 24) {                                                      /* (24 + e) T (9.5.2.1.8) */
            p->phase += (double)p->jp.frac / 65536.0;
            v92_sgn_init(&p->sgn, 0);
            unext(p, VA_TRN1U2);
        }
        break;
    case VA_TRN1U2:
        a = two(p, v92_sgn_tx_trn(&p->sgn));
        if (p->un % 12 == 0 &&
            (cfg->dil_n > 0 ? (p->un >= cfg->ntrn1b && p->ds == VD_DIL && p->dn >= cfg->dil_syms) : (p->ri_seen && p->un >= 24))) {
            load_frame(p, F_CPT, 0);
            unext(p, VA_CPT);
        }
        break;
    case VA_E1U:
        a = two(p, v92_sgn_tx_bit(&p->sgn, 0));
        if (p->un >= V92_E1U_BITS) start_trn2u(p, 0);
        break;
    case VA_TRN2U:
        a = (double)v92_ml_tx(&p->ml, 0) / sqrt(p->ml.nbits == 3 ? 21.0 : 5.0);
        if (!p->in_reneg && cfg->suvd_wait > 0 && p->un >= cfg->suvd_wait && p->suvd_n == 0) { unext(p, VA_DOWN); break; }
        if (p->in_reneg && cfg->reneg_no_suvu) break;
        if (p->un % 12 == 0 && (p->un >= (p->in_reneg ? 2400 : cfg->trn2u_min) || p->suvd_n > 0)) {
            p->cpu_sent = 0; p->ack_sent = 0; p->since_cpu = 0; p->since_suvu = 0;
            load_frame(p, F_SUVU, p->cpd_n > 0);
            unext(p, VA_SEQ);
        }
        break;
    case VA_SEQ: {
        a = (double)v92_ml_tx(&p->ml, p->fr + p->frpos) / sqrt(p->ml.nbits == 3 ? 21.0 : 5.0);
        p->frpos += p->ml.nbits;
        if (p->cpu_sent) p->since_cpu++;
        if (!p->in_reneg && cfg->cpd_wait > 0 && ++p->since_suvu >= cfg->cpd_wait && p->cpd_n == 0) { unext(p, VA_DOWN); break; }
        if (p->frpos >= p->frn) {                           /* a sequence is complete */
            int got_ack = p->suvd_ack || p->cpd_ack;
            if (p->fr_ack) p->ack_sent = 1;
            if (p->ack_sent && (got_ack || p->ed_seen)) { unext(p, VA_E2U); break; }       /* 9.6.2.1.4 */
            if (p->suvd_n > 0 && !p->cpu_sent) { p->cpu_sent = 1; p->since_cpu = 0; load_frame(p, F_CPU, p->cpd_n > 0); }
            else if (p->cpu_sent && !got_ack && p->since_cpu > 2400) load_frame(p, F_CPU, p->cpd_n > 0);    /* 9.6.2.1.3 */
            else load_frame(p, F_SUVU, p->cpd_n > 0);
        }
        break;
    }
    case VA_E2U: {
        static const uint8_t z[3] = { 0, 0, 0 };
        a = (double)v92_ml_tx(&p->ml, z) / sqrt(p->ml.nbits == 3 ? 21.0 : 5.0);
        if (p->un >= 12 + (p->cpd.e2u_extend ? 1 : 0)) {
            if (cfg->z2_dead > 0 && cfg->z2_dead < p->cpd.filt.lz2)
                p->cpd.filt.coef[p->cpd.filt.lz1 + p->cpd.filt.lp1 + cfg->z2_dead] = 0;
            if (p->cleardown || p->cpd.drn == 0 || v92_enc_init(&p->enc, &p->cpd.map, &p->cpd.filt, enc_get, p) != 0) {
                unext(p, VA_DOWN);
                break;
            }
            p->b1_bits = (long)V92_B1U_FRAMES * p->cpd.map.K;
            p->lead = 12 * cfg->b1u_lead;
            if (!p->lead) p->enc.conv = (uint8_t)cfg->conv_start;
            p->up_rate = V92_UP_BPS(p->cpd.drn);
            p->data_starts++;
            if (p->in_reneg) { p->renegs++; p->in_reneg = 0; }
            unext(p, VA_DATA);
        }
        break;
    }
    case VA_DATA:
        if (p->in_reneg && p->enc.i == 0) {                 /* Ru begins on a data frame boundary (9.8.2.1.1) */
            p->tx_ready = 0;
            p->suvd_n = p->suvd_ack = p->cpd_n = p->cpd_ack = p->ed_seen = 0;
            unext(p, VA_RRU);
            return up_symbol(p);
        }
        if (p->lead > 0) {
            static const uint8_t k0[V92_DFRAME] = { 0 };
            if (p->lead % V92_DFRAME == 0) v92_enc_force_k(&p->enc, k0);
            a = (double)v92_enc_symbol(&p->enc, 0) / 16384.0;
            if (--p->lead == 0) {
                /* the mapping starts afresh, the filters run on */
                static v92_enc_t old;
                old = p->enc;
                (void)v92_enc_init(&p->enc, &p->cpd.map, &p->cpd.filt, enc_get, p);
                p->enc.hp = old.hp;
                memcpy(p->enc.uh, old.uh, sizeof old.uh);
                memcpy(p->enc.xh, old.xh, sizeof old.xh);
                memcpy(p->enc.vh, old.vh, sizeof old.vh);
                p->enc.conv = (uint8_t)cfg->conv_start;
            }
            break;
        }
        a = (double)v92_enc_symbol(&p->enc, 0) / 16384.0;
        p->pw_acc += a * a; p->pw_n++;
        a *= cfg->gain_err > 0 ? cfg->gain_err : 1.0;
        break;
    case VA_DOWN:
        p->tx_ready = 0;
        break;
    }
    return a;
}

/* ------------------------------------------------------------------ */
/* downstream: reading the octets                                      */

static void dnext(v92a_t *p, v92a_dstate s) { p->ds = s; p->dn = 0; }

static int is_unit(const v92a_t *p, const uint8_t *o, unsigned u0, unsigned u1, int inv)
{
    int i;
    for (i = 0; i < 6; i++)
        if (o[i] != v90_pcm_octet(p->cfg.alaw, (i % 3 == 1) ? u1 : u0, ((i < 3) ? 1 : 0) ^ inv)) return 0;
    return 1;
}

/* +1: the six octets are an Rd unit (+ + + - - -), -1: the inverse, 0: neither */
static int r_unit(const v92a_t *p, const uint8_t *o)
{
    int i, pos = 1, neg = 1;
    for (i = 0; i < 6; i++) {
        unsigned u = p->dmap.C[i][0];
        if (o[i] != v90_pcm_octet(p->cfg.alaw, u, i < 3)) pos = 0;
        if (o[i] != v90_pcm_octet(p->cfg.alaw, u, i >= 3)) neg = 0;
    }
    return pos ? 1 : neg ? -1 : 0;
}

static void train_start(v92a_t *p)
{
    v90dec_init(&p->dec, &p->tmap, p->cfg.alaw); v90_scr_init(&p->dscr);
    p->fon = 0; p->mbn = 0; p->mones = 0; p->zero_frames = 0;
    dnext(p, VD_TRAIN);
}

/* bits of TRN2d / SUVd / CPd / Ed, descrambled */
static void train_bit(v92a_t *p, int bit)
{
    if (p->mbn > 0 && p->mbn < (int)sizeof p->mb) p->mb[p->mbn++] = (uint8_t)bit;
    if (bit) { p->mones++; return; }
    if (p->mones >= 17 && p->mbn == 0) { memset(p->mb, 1, 17); p->mb[17] = 0; p->mbn = 18; }
    p->mones = 0;
}

static void train_frame_done(v92a_t *p, const uint8_t *bits, int D)
{
    int k, zeros = 0, r;
    for (k = 0; k < D; k++) if (!bits[k]) zeros++;
    if (p->suvd_n == 0 && p->cpd_n == 0 && p->mbn == 0) {
        p->trn2d_frames++;
        if (zeros && p->mones < 17) p->trn2d_bad++;
    }
    if (p->mbn >= V92_SUV_BITS) {
        if (v92_dn_kind(p->mb, p->mbn) == V92_DN_SUVD) {
            v92_suvd_t s;
            r = v92_suvd_parse(p->mb, p->mbn, D, &s);
            if (r > 0) { p->suvd_n++; if (s.ack) p->suvd_ack = 1; }
        } else {
            r = v92_cpd_parse(p->mb, p->mbn, D, &p->cpd_new);
            if (r > 0 && p->cpd_dropped < p->cfg.drop_cpd) p->cpd_dropped++;     /* missed */
            else if (r > 0) {
                p->cpd = p->cpd_new; p->cpd_n++;
                if (p->cfg.first_cpd_plain && p->cpd_n == 1 && p->cpd.ack) p->us = VA_DOWN;
                if (p->cpd.ack) p->cpd_ack = 1;
                if (p->cpd.drn == 0) p->clear_seen = 1;
            }
        }
        if (r > 0) { if (r != p->mbn) p->seq_bad++; p->mbn = 0; p->mones = 0; }
        else if (r != V92_ERR_SHORT) { p->seq_bad++; p->mbn = 0; p->mones = 0; }
    }
    /* Ed: two data frames of zeros, outside any sequence */
    if (zeros == D && p->mbn == 0) { if (++p->zero_frames == 2) p->ed_seen = 1; }
    else p->zero_frames = 0;
}

static void down_octet(v92a_t *p, uint8_t o, const uint8_t *ahead)
{
    int alaw = p->cfg.alaw;
    unsigned W = (unsigned)p->cfg.uinfo + 16, U = (unsigned)p->cfg.uinfo;
    int sign = v90_octet_sign(o);
    unsigned uc = v90_octet_ucode(alaw, o);
    const v90_dil_t *dil = &p->ja.dil;

    switch (p->ds) {
    case VD_WAIT_SD:
        if (is_unit(p, ahead, W, 0, 0) && is_unit(p, ahead + 6, W, 0, 1)) {
            p->sd_at = p->tick;
            dnext(p, VD_SDBAR); p->dn = -5;
        }
        break;

    case VD_SDBAR:
        if (p->dn == 5) p->sdbar_seen = 1;
        if (++p->dn >= 48) {
            v90dec_sign_init(&p->dt); v90dec_sign_init(&p->dj);
            p->sbn = 0; p->sones = 0;
            dnext(p, VD_SIGN);
        }
        break;

    case VD_SIGN: {
        /* TRN1d, Jd, Jp, Jp': UINFO with a sign */
        int bt = v90dec_trn1d(&p->dt, sign), bj = v90dec_jd(&p->dj, sign);
        if (uc != U) p->sign_bad++;
        if (!p->jd_ok && p->sbn == 0 && p->dn < 2040) {
            p->trn1d_n++;
            if (p->dn >= 23 && bt != 1) p->trn1d_bad++;
        }
        p->dn++;
        if (p->sbn > 0 && p->sbn < (int)sizeof p->sb) p->sb[p->sbn++] = (uint8_t)bj;
        if (p->sbn == 0) {
            if (bj) p->sones++;
            else { if (p->sones >= 17) { memset(p->sb, 1, 17); p->sb[17] = 0; p->sbn = 18; } p->sones = 0; }
        } else if (p->sbn == V92_J_BITS) {
            int kind = v92_j_kind(p->sb, p->sbn), ok;
            if (kind == V92_J_JD) { ok = v92_jd_parse(p->sb, p->sbn, &p->jd) == V92_J_BITS; if (ok) p->jd_ok++; }
            else { ok = v92_jp_parse(p->sb, p->sbn, &p->jp) == V92_J_BITS; if (ok) p->jp_ok++; }
            if (!ok) { p->sbn = 0; p->sones = 0; p->seq_bad++; }
        } else if (p->sbn == V92_J_BITS + 12) {
            /* another Jd / Jp (17 ones) or Jp' (12 zeros) */
            int k, z = 0;
            for (k = 0; k < 12; k++) if (!p->sb[V92_J_BITS + k]) z++;
            if (z == 12 && p->jp_ok) {
                p->jpp_seen = 1;
                p->seg = 0; p->segm = 0;
                v90dec_sign_init(&p->dq);
                dnext(p, dil->n > 0 ? VD_DIL : VD_SCR);
            } else {
                memmove(p->sb, p->sb + V92_J_BITS, 12); p->sbn = 12;
            }
        }
        break;
    }

    case VD_DIL: {
        int u = dil->train[p->seg], c = u / 16, L = (dil->h[c] + 1) * 6;
        if (p->segm == 0 && is_unit(p, ahead, U, U, 0) && is_unit(p, ahead + 6, U, U, 0) && is_unit(p, ahead + 12, U, U, 0)) {
            dnext(p, VD_RI);
            down_octet(p, o, ahead);
            return;
        }
        {
            int sgn = getbitm(dil->sp, p->segm % dil->lsp);
            int mag = getbitm(dil->tp, p->segm % dil->ltp) ? u : dil->ref[c];
            if (o != v90_pcm_octet(alaw, (unsigned)mag, sgn)) p->dil_bad++;
        }
        p->dn++;
        if (++p->segm >= L) { p->segm = 0; p->seg = (p->seg + 1) % dil->n; p->dil_n_ok++; }
        break;
    }

    case VD_SCR:
        /* 8.6.6: UINFO, the sign is the scrambler output for ones */
        if (p->dn % 6 == 0 && is_unit(p, ahead, U, U, 0) && is_unit(p, ahead + 6, U, U, 0)
            && is_unit(p, ahead + 12, U, U, 0) && is_unit(p, ahead + 18, U, U, 0)) {
            dnext(p, VD_RI);
            down_octet(p, o, ahead);
            return;
        }
        if (uc != U) p->scr_bad++;
        if (v90dec_trn1d(&p->dq, sign) != 1 && p->dn >= 23) p->scr_bad++;
        p->dn++; p->scr_n++;
        break;

    case VD_RI:
        p->ri_seen = 1;
        if (p->dn % 6 == 0 && is_unit(p, ahead, U, U, 1)) {
            dnext(p, VD_RIBAR);
            down_octet(p, o, ahead);
            return;
        }
        if (o != v90_pcm_octet(alaw, U, (p->dn % 6) < 3)) p->ri_bad++;
        p->dn++; p->ri_n++;
        break;

    case VD_RIBAR:
        if (o != v90_pcm_octet(alaw, U, (p->dn % 6) >= 3)) p->ri_bad++;
        if (p->dn == 5) p->ribar_seen = 1;
        if (++p->dn >= 24) train_start(p);
        break;

    case VD_RD:
        /* 8.8.4: the largest point of each interval, + + + - - - or the inverse */
        if (p->dn % 6 == 0 && r_unit(p, ahead) == -p->rd_pol) {
            dnext(p, VD_RDBAR);
            down_octet(p, o, ahead);
            return;
        }
        if (p->dn % 6 == 0) { if (r_unit(p, ahead) == p->rd_pol) p->rd_units++; else p->rd_bad++; }
        p->dn++;
        break;

    case VD_RDBAR:
        if (p->dn % 6 == 0) { if (r_unit(p, ahead) == -p->rd_pol) p->rdbar_units++; else p->rd_bad++; }
        if (++p->dn >= 24) train_start(p);
        break;

    case VD_TRAIN: case VD_B1D: case VD_DATA:
        if (p->ds == VD_DATA && p->fon == 0 && r_unit(p, ahead) != 0 && r_unit(p, ahead + 6) == r_unit(p, ahead)) {
            p->rd_pol = r_unit(p, ahead);
            p->rx_ready = 0;
            p->b1d_frames = 0;
            if (!p->in_reneg) p->in_reneg = 1;          /* 9.8.2.2: the digital modem began it */
            dnext(p, VD_RD);
            down_octet(p, o, ahead);
            return;
        }
        p->fo[p->fon++] = o;
        if (p->fon == 6) {
            uint8_t fb[V90_MAX_D], db[V90_MAX_D];
            const v90_map_t *m = p->ds == VD_TRAIN ? &p->tmap : &p->dmap;
            int D = V90_MAP_D(m), k;
            p->fon = 0;
            if (v90dec_frame(&p->dec, p->fo, fb) != D) { p->dec_bad++; break; }
            for (k = 0; k < D; k++) db[k] = (uint8_t)v90_descramble_gpc(&p->dscr, fb[k]);
            if (p->ds == VD_TRAIN) {
                for (k = 0; k < D; k++) train_bit(p, db[k]);
                train_frame_done(p, db, D);
                if (p->ed_seen) {
                    /* 8.8.1: everything from zero before B1d */
                    v90dec_init(&p->dec, &p->dmap, alaw); v90_scr_init(&p->dscr);
                    dnext(p, VD_B1D);
                }
            } else if (p->ds == VD_B1D) {
                for (k = 0; k < D; k++) if (db[k] != 1) p->b1d_bad++;
                if (++p->b1d_frames >= 48) { p->rx_ready = 1; p->down_rate = D * 8000 / 6; dnext(p, VD_DATA); }
            } else if (p->cfg.putbit) {
                for (k = 0; k < D; k++) p->cfg.putbit(p->cfg.user, db[k]);
            }
        }
        break;
    }
}

/* ------------------------------------------------------------------ */

void v92a_init(v92a_t *p, const v92a_cfg *cfg)
{
    v90_dil_t *dil;
    v90_cp_t v;
    int i;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->us = VA_SIL0; p->ds = VD_WAIT_SD;
    p->ml.nbits = 2;

    /* the DIL we ask for */
    dil = &p->ja.dil;
    dil->n = (uint8_t)cfg->dil_n; dil->lsp = 5; dil->ltp = 12;
    dil->sp[0] = 0x16;
    dil->tp[0] = 0xDB; dil->tp[1] = 0x06;
    for (i = 0; i < 8; i++) { dil->h[i] = (uint8_t)(i % 3); dil->ref[i] = (uint8_t)(16 * i + 3); }
    for (i = 0; i < dil->n; i++) dil->train[i] = (uint8_t)((i * 29 + 7) & 127);
    if (dil->n == 0) { dil->lsp = 1; dil->ltp = 1; }
    p->ja.rates = cfg->up_rates;

    /* CPt: D = drn + 8 = Kt + 6 */
    p->cpt.type = V92_UP_CPT; p->cpt.drn = (uint8_t)(cfg->Kt + 6 - 8);
    p->cpt.alaw = (uint8_t)cfg->alaw; p->cpt.rms_ratio = 0x2000;
    for (i = 0; i < 6; i++) p->cpt.idx[i] = (uint8_t)(i & 1);
    cons(p->cpt.mask[0], cfg->Mt, 90, 9);
    cons(p->cpt.mask[1], cfg->Mt, 85, 8);
    v92_cpu_to_v90(&p->cpt, &v);
    (void)v90_map_from_cp(&p->tmap, &v);

    build_cpu(p, cfg->Kd);
}

int v92a_renegotiate(v92a_t *p, int Kd, int cleardown)
{
    if (p->us != VA_DATA || p->ds != VD_DATA || p->in_reneg) return -1;
    if (Kd > 0) build_cpu(p, Kd);
    if (cleardown) p->cleardown = 1;
    p->in_reneg = 1;
    return 0;
}

int v92a_tick(v92a_t *p, uint8_t octet, double *amp, double *frac)
{
    /* a 24-octet delay line gives the downstream reader its look-ahead */
    p->tick++;
    memmove(p->look, p->look + 1, 23); p->look[23] = octet;
    if (++p->nlook >= 24) down_octet(p, p->look[0], p->look);
    /* a slip of the symbol clock: phase is how late the symbols are; a whole
     * sample of lateness is a sample without a symbol */
    if (p->phase >= 1.0) { p->phase -= 1.0; *amp = 0; *frac = 0; return 0; }
    *frac = p->phase;
    *amp = up_symbol(p);
    return 1;
}
