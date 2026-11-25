/* V.92 Phases 3 and 4 with PCM upstream, digital modem. See v92_p34d.h. */
#include <string.h>
#include "v92_p34d.h"
#include "clock.h"
#include "v90_pcm.h"
#include "fixmath.h"

#define MS(ms) ((int32_t)(ms) * 8)

/* our choices for the lengths the Recommendation leaves open */
#define TRN1D_SYMS     4800     /* TRN1d: >= 2040 T; 600 ms gives the echo canceller time */
#define EC_GUARD_MS    20       /* after TRN1d began + round trip: the caller has stopped Ja */
#define EC_FAST        3000     /* samples at full NLMS step */
#define NOISE_SYMS     1000     /* end of TRN1d: noise floor measurement */
#define TRN_LEAD       4        /* the training receiver starts this early */
#define TRAINED_SYMS   2400     /* no DIL: second TRN1u before Ri (9.5.1.1.13) */
#define TRN2D_FRAMES   400      /* TRN2d >= 2040 T = 340 data frames */
#define REFINE_SYMS    2400     /* TRN2u before the design starts */
#define RENEG_TRN2D    48       /* data frames of TRN2d in a rate renegotiation */
#define RENEG_REFINE   600
#define RD_UNITS       64       /* Rd for 384 T */
#define RU_FLOOR       24       /* linear RMS below which Ru / Su are not looked for */
#define DOWN_RMS       5600     /* downstream data power assumed for the echo: -9 dBm0 */

/* time limits */
#define T_RU_MS        6000     /* INFO1a to Ru: 70 ms nominal */
#define T_JA_MS        6000     /* 9.5.1.2.1: 4500 ms + round trip from the end of INFO1a */
#define T_SU_MS        5100     /* 9.5.1.2.2, from the start of TRN1d, plus a round trip */
#define T_TRAIN_MS     8000     /* 9.5.2.1.11: CPt within 5000 ms of Su-bar */
#define T_PHASE4_MS    20000    /* 9.6.1.2.1: B1u within 20 s (+ 6 round trips) */
#define T_RENEG_MS     8000
/* our cleardown from its start, the caller there or not. What a caller that
 * follows the text may take: our Rd and its Ru, about 0.1 s; TRN2u up to
 * 16008 T, 2.0 s (9.8.2.2.3, 9.8.2.1.2); SUVu, our CPd, its acknowledge and
 * two round trips: some 2.5 s in all. Under the 4 s T21.1 set; with a
 * caller that acknowledges at once some 0.3 to 0.4 s (simulation). */
#define T_CLEAR_MS     3500
#define BASE_MAX_Q4    (1024 << 4)  /* the baseline is followed this far from zero, linear units Q4 */
#define SUVD_HOLD_MS   2500     /* SUVd at the latest so long into Phase 4, the design done or not */
#define T_PACE_MS      2500     /* more for each sample the design waits between steps */

#define RENEG_DROP     2        /* reneg_ours: ours, to drop the upstream rate */

#define UP_UNITY       16384    /* up_scale of a caller at the level designed for */

#define CD_BLOCK       80       /* carrier watch: 10 ms */
#define CD_BLOCKS      50       /* about half a second without signal in data mode */
#define CD_SHIFT       6        /* "without": 18 dB under the power of B1u */
#define CD_LINE_SHIFT  5        /* the line's squares are summed >> 5: 80 of 2^30 fit 32 bits */
#define LINE_IDLE      8        /* |line| up to this is the idle code (A-law 0xD5, 0x55: +-8; mu-law: 0) */
#define LINE_IDLE_RUN  24       /* ... for 4 periods of Ru: nobody sends Ru, whatever the detector reads */

static void fail(v92d_t *p, int err)
{
    /* the host has asked to hang up: however the call ends now, it is our
     * cleardown */
    p->state = V92D_FAILED; p->error = p->clearing ? V92D_ERR_HANGUP : err; p->t = 0;
    p->tx_ready = p->rx_ready = 0; p->rx_on = 0; p->trn_on = 0;
    (void)v90tx_select(&p->tx, V90TX_SILENCE);
}

static void enter(v92d_t *p, v92d_state s) { p->state = s; p->t = 0; }

static int32_t pace_ms(const v92d_t *p) { return p->cfg.dsn_pace > 1 ? (int32_t)T_PACE_MS * (p->cfg.dsn_pace - 1) : 0; }

static void retrain(v92d_t *p, int ours)
{
    /* the host has asked to hang up: whatever goes wrong now ends the call */
    if (p->clearing) { fail(p, V92D_ERR_HANGUP); return; }
    p->tx_ready = p->rx_ready = 0; p->rx_on = 0; p->trn_on = 0;
    p->retrain_ours = ours; p->up_floor = 0;
    (void)v90tx_select(&p->tx, V90TX_SILENCE);
    enter(p, V92D_RETRAIN);
}

int v92d_init(v92d_t *p, const v92d_cfg *cfg)
{
    int r;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->state = V92D_WAIT_RU;
    r = v90tx_init(&p->tx, cfg->alaw, cfg->uinfo);
    ec_init(&p->ec); p->ec_mu = -1;
    tonedet_init(&p->tone_a, TONEDET_2400);     /* Tone A for more than 50 ms (9.7.1.2) */
    v92_p6_init(&p->p6);
    /* Table 21: Jd as in V.90 with bits 47 and 48 zero */
    p->jd.rates = cfg->down_rates ? cfg->down_rates : ((1u << V90_NUM_RATES) - 1);
    p->jd.cp16_train = 0; p->jd.cp16_reneg = 0; p->jd.max_ld = 3;
    if (r != 0) { fail(p, V92D_ERR_UINFO); return -1; }
    if (cfg->echo_delay > V92D_MAX_ECHO_DELAY) { fail(p, V92D_ERR_UINFO); return -1; }
    ecdly_init(&p->ecd, cfg->echo_delay);
    ecdly_near(&p->ecd, cfg->rtd);
    return 0;
}

/* ---- data mode receiver ---- */

static void put_data(void *user, int bit)
{
    v92d_t *p = (v92d_t *)user;
    if (p->b1_left > 0) {                       /* B1u: 48 data frames of ones (8.7.1) */
        if (!bit) p->b1_bad++;
        if (--p->b1_left == 0) {
            if (p->b1_bad > 8) fail(p, V92D_ERR_B1);
            else {
                /* the carrier watch's reference: the power B1u came with
                 * (which points the moduli use is not ours to guess) */
                p->sig_ms = p->cd_n ? (uint32_t)(p->cd_acc / p->cd_n) : 0;
                p->cd_acc = 0; p->cd_line = 0; p->cd_n = 0; p->cd_quiet = 0; p->cd_idle = 0;
                p->rx_ready = 1;
            }
        }
    } else if (p->rx_ready && p->cfg.putbit && !p->cd_quiet) {
        /* (nothing is delivered from a line that has gone quiet: what the
         * decoder makes of silence is not data) */
        p->cfg.putbit(p->cfg.user, bit);
    }
}

/* CLK_REBASE_AT samples of the training receiver's clock (trn.k) in data
 * mode: it, data_k and the symbol numbers counted on it back by CLK_REBASE
 * (modem/clock.h), before a call of 2^31 samples (74.6 hours) takes them
 * past INT32_MAX */
static void rebase(v92d_t *p)
{
    p->trn.k -= CLK_REBASE; p->data_k -= CLK_REBASE;
    p->trn.dec_m = clk_back(p->trn.dec_m);
    p->m_b1u = clk_back(p->m_b1u); p->m_trn2u = clk_back(p->m_trn2u);
    /* (trn.mref: v92_trn_resume sets it before the receiver decides again) */
}

/* E2u is over: symbol m is the first of B1u */
static void upstream_start(v92d_t *p, int32_t m)
{
    if (p->dsn_state != 2 || v92_dec_init(&p->dec, &p->cpd.map, put_data, p) != 0) { fail(p, V92D_ERR_B1); return; }
    v92_dec_any_start(&p->dec);
    p->base_q4 = 0; p->up_scale = 0;
    p->m_b1u = m;
    p->data_k = m + p->dsn.dly;
    p->b1_left = (int32_t)V92_B1U_FRAMES * p->cpd.map.K; p->b1_bad = 0;
    p->b1_sym = 0; p->b1_leads = 0; p->b1_zero = 0;
    p->up_rate = V92_UP_BPS(p->cpd.drn);
    p->bad_seen = 0; p->q_bad = 0;
    p->rx_on = 1;
    p->trn_on = 2;
    p->sig_ms = 0; p->cd_acc = 0; p->cd_line = 0; p->cd_n = 0; p->cd_quiet = 0; p->cd_idle = 0;
}

/* The first data frame after E2u, sample by sample. A real analogue modem
 * (reading 57) sends one data frame with every Ki = 0 before the 48 data
 * frames of B1u (its encoder is one data frame late with the scrambled
 * ones), and what follows is as if that frame had not been: n = 0 of 8.7.1
 * for the mapping is the first symbol after it. Of twelve symbols of
 * scrambled ones no ten have Ki = 0; a first data frame that has is passed
 * over and the decoder starts again with the next one. */
static void b1u_lead(v92d_t *p, int16_t y)
{
    const v92_map_t *m = &p->cpd.map;
    int i = p->b1_sym, set = m->cidx[i % V92_CFRAME], lc = m->lc[set], j;
    int32_t a = y < 0 ? -(int32_t)y : y;

    for (j = 0; j + 1 < lc && 2 * a > (int32_t)m->pt[set][j] + m->pt[set][j + 1]; j++) ;
    if (v92_eta_to_k(m, i, y < 0 ? -j - 1 : j) == 0) p->b1_zero++;
    if (++p->b1_sym < V92_DFRAME) return;
    if (p->b1_zero < V92_DFRAME - 2 || p->b1_leads >= V92D_B1U_LEAD) { p->b1_sym = -1; return; }
    if (v92_dec_init(&p->dec, m, put_data, p) != 0) { fail(p, V92D_ERR_B1); return; }
    v92_dec_any_start(&p->dec);
    p->m_b1u += V92_DFRAME;
    p->b1_left = (int32_t)V92_B1U_FRAMES * m->K; p->b1_bad = 0;
    p->b1_sym = 0; p->b1_zero = 0; p->b1_leads++;
    p->cd_acc = 0; p->cd_n = 0;
}

/* ---- the caller's gain in data mode ----
 *
 * The design builds the constellation for the power at which the caller
 * sends TRN1u (8.8.3, reading 35), and the decoder slices on those points.
 * Implementations differ on that reference: a caller whose data signal
 * arrives some percent louder or weaker than designed is decided wrong at
 * the larger points, and B1u fails. So the gain is measured on the first
 * V92D_UP_EST symbols of B1u (v92d_up_scale). The points are G.711 levels,
 * nearly the same set at any scale near the right one, so a
 * decision-directed estimate stays where it starts; what tells the right
 * scale is that nearly every symbol then sits on a point (within a quarter
 * of the spacing).
 *
 *   1. At scale 1: if 7/8 of the symbols sit on a point, the scale is 1.
 *      Simulated calls at the designed level score 131 to 144 of 144 and
 *      stop here (one fit, some 14 k ARM instructions). Real ones need not:
 *      of 26 recorded calls that reach B1u, 21 score under 126 (cxe 110,
 *      h4 122, both calls that work at 1) and go on to 2 and 3, some 175 k
 *      instructions in one tick.
 *   2. The power of the symbols against what the design's trial of the
 *      encoder gave (dsn.usum) puts the gain within some 15 %. The search
 *      stays in the window 3/4 .. 4/3 of that start: G.711 is built in
 *      octaves, and at twice or half the right scale many symbols sit on
 *      points too (cxe: 128 at 2 against 110 at 1), so a scale is never
 *      taken that the power does not allow.
 *   3. In that window, 4 % apart on a third of the symbols, then 1 % apart
 *      about the best on all of them, the scale where most symbols sit on
 *      a point.
 *   4. It is taken only if at least 3/4 of the symbols sit on a point there
 *      (a caller off the level: 126 to 144 in simulation) and 1/8 more than
 *      at scale 1 (usr3, a recorded call that fails its B1u at the designed
 *      level: 67 at 1, the best 86). These rules, not step 1, are what keep
 *      a real call at the designed level as it was: no recorded call takes
 *      a scale.
 *
 * A scale of 1 changes nothing at all (up_in, base_step). Otherwise the
 * decoder starts again on B1u from its first symbol, which the training
 * receiver still holds. The fits slice the samples as received, without
 * the baseline (base_q4), which has had only the first symbols to move. */

/* the decoder's input for received sample s */
static int32_t up_in(const v92d_t *p, int32_t s)
{
    if (p->up_scale == 0 || p->up_scale == UP_UNITY) return s;
    return (s * p->up_scale + 8192) >> 14;
}

/* of every step-th of the V92D_UP_EST samples from k (data frame interval
 * 0), the number within a quarter of the spacing of the nearest point once
 * multiplied by c (Q14, at most 65535) */
static int up_fit(const v92_map_t *m, const v92_trn_t *t, int32_t k, int32_t c, int step)
{
    int j, n = 0;
    for (j = 0; j < V92D_UP_EST; j += step) {
        int i = j % V92_DFRAME, lc = v92_lc(m, i), eta;
        int32_t r = ((int32_t)v92_trn_sample(t, k + j) * c + 8192) >> 14, a, e, h;
        eta = v92_slice(m, i, r);
        a = v92_point(m, i, eta); e = r - a;
        /* the spacing on the side of r; past an end point, the one inside */
        if ((e > 0 && eta < lc - 1) || eta == -lc) h = v92_point(m, i, eta + 1) - a;
        else h = a - v92_point(m, i, eta - 1);
        if (4 * (e < 0 ? -e : e) < h) n++;
    }
    return n;
}

/* best of the scales c, c + c * inc / 1024, ... (n of them) that lie in
 * lo .. hi, on every step-th sample; *fit = -1 if none does */
static int32_t up_search(const v92_map_t *m, const v92_trn_t *t, int32_t k, int32_t c, int n,
                         int32_t inc, int step, int32_t lo, int32_t hi, int *fit)
{
    int32_t best = UP_UNITY;
    int q;
    *fit = -1;
    for (q = 0; q < n && c <= hi; q++, c += c * inc / 1024) {
        int f;
        if (c < lo) continue;
        f = up_fit(m, t, k, c, step);
        if (f > *fit) { *fit = f; best = c; }
    }
    return best;
}

int32_t v92d_up_scale(const v92_map_t *m, const v92_trn_t *t, int32_t k0, uint32_t eu2)
{
    uint64_t pw = 0, ex;
    int32_t c, lo, hi, j;
    int f1, fc, ff;

    f1 = up_fit(m, t, k0, UP_UNITY, 1);
    if (f1 >= V92D_UP_EST - V92D_UP_EST / 8) return UP_UNITY;
    for (j = 0; j < V92D_UP_EST; j++) {
        int32_t s = v92_trn_sample(t, k0 + j);
        pw += (uint64_t)((int64_t)s * s);
    }
    ex = (uint64_t)eu2 * V92D_UP_EST;
    while (ex >= ((uint64_t)1 << 34) || pw >= ((uint64_t)1 << 34)) { ex >>= 1; pw >>= 1; }
    if (pw == 0 || ex == 0) return UP_UNITY;
    c = (int32_t)isqrt_u64((ex << 28) / pw);                /* 1 / gain from the power, Q14 */
    /* the window: 3/4 .. 4/3 of it, and a gain of 4 .. 1/4 */
    if (c > 65535 + 65535 / 3 || c < 4096 - 4096 / 4) return UP_UNITY;
    lo = c - c / 4; hi = c + c / 3;
    if (lo < 4096) lo = 4096;
    if (hi > 65535) hi = 65535;
    /* 4 % apart on a third of the symbols; then 1 % apart about the best,
     * on all */
    c = up_search(m, t, k0, c - c / 4, 15, 41, 3, lo, hi, &fc);
    if (fc < 0) return UP_UNITY;
    c = up_search(m, t, k0, c - c * 3 / 100, 7, 10, 1, lo, hi, &ff);
    if (ff < V92D_UP_EST * 3 / 4 || ff <= f1 + V92D_UP_EST / 8) return UP_UNITY;
    return c;
}

/* V92D_UP_EST symbols of B1u are in from sample k0 on: measure, and if the
 * caller is off the designed level start the decoder again from k0 */
static void up_measure(v92d_t *p, int32_t k0)
{
    int32_t c;

    p->up_scale = UP_UNITY;
    if (p->trn.k - k0 > 255 || p->dsn.i <= 0) return;     /* (cannot happen) */
    /* E[u^2] of the design's trial, linear^2 */
    c = v92d_up_scale(&p->cpd.map, &p->trn, k0,
                      (uint32_t)(((uint64_t)p->dsn.usum << 14) / (uint32_t)p->dsn.i));
    if (c == UP_UNITY) return;
    /* the caller is off the designed level: B1u again from its first symbol
     * (and its leading data frames of Ki = 0 looked for again) */
    if (p->trn.k - (k0 - V92_DFRAME * p->b1_leads) > 255) return;
    if (v92_dec_init(&p->dec, &p->cpd.map, put_data, p) != 0) { fail(p, V92D_ERR_B1); return; }
    v92_dec_any_start(&p->dec);
    p->up_scale = c;
    p->m_b1u -= V92_DFRAME * p->b1_leads;
    p->data_k = p->m_b1u + p->dsn.dly;
    p->base_q4 = 0;
    p->b1_left = (int32_t)V92_B1U_FRAMES * p->cpd.map.K; p->b1_bad = 0;
    p->b1_sym = 0; p->b1_zero = 0; p->b1_leads = 0;
}

/* ---- sequences: frame sync hunt, parse ---- */

enum { W_JA, W_CPT, W_PH4 };

static void seq_reset(v92d_t *p, int want)
{
    p->fn = 0; p->sh.ones = 0; p->zeros = 0; p->want = want; p->after_seq = -1;
}

static void on_cpt(v92d_t *p)
{
    v90_cp_t cp;
    if (p->got_cpt) return;
    v92_cpu_to_v90(&p->cpx, &cp);
    if (p->cpx.drn == 0 || v90tx_load_cp(&p->tx, &cp) != 0) { fail(p, V92D_ERR_CPT); return; }
    p->got_cpt = 1;
    /* 9.5.1.1.12: Ri on CPt; 9.5.1.1.13: Ri is on already, Ri-bar on CPt */
    if (p->ja.dil.n > 0) { (void)v90tx_select(&p->tx, V90TX_RI); p->ri_sent = 1; }
    else (void)v90tx_select(&p->tx, V90TX_RI_BAR);
}

static void on_cpu(v92d_t *p)
{
    v90_cp_t cp;
    if (p->cpx.drn == 0) { fail(p, V92D_ERR_CLEARDOWN); return; }          /* 9.11 */
    v92_cpu_to_v90(&p->cpx, &cp);
    if (v90tx_load_cp(&p->tx, &cp) != 0) { fail(p, V92D_ERR_CPU); return; }
    p->got_cpu = 1;
    if (p->cpx.ack) p->got_ack = 1;
    p->down_rate = (p->cpx.drn + 20) * 8000 / 6;
}

/* a complete candidate may be in fbuf: 1 = taken, 0 = need more, -1 = not one */
static int seq_try(v92d_t *p)
{
    int r;
    if (p->want == W_JA) {
        if (p->fn < 276 || p->fn % 12) return 0;
        r = v92_ja_parse(p->fbuf, p->fn, &p->ja);
        if (r > 0) p->got_ja = 1;
    } else if (p->want == W_CPT) {
        if (p->fn < 288 || p->fn % 12) return 0;
        r = v92_up_kind(p->fbuf, p->fn) == V92_UP_CPT ? v92_cpu_parse(p->fbuf, p->fn, 12, &p->cpx) : V92_ERR_RANGE;
        if (r > 0) on_cpt(p);
    } else {
        int kind;
        if (p->fn < V92_SUV_BITS || p->fn % 24) return 0;
        kind = v92_up_kind(p->fbuf, p->fn);
        if (kind == V92_UP_SUVU) {
            v92_suvu_t s;
            r = v92_suvu_parse(p->fbuf, p->fn, 24, &s);
            if (r > 0) { p->got_suvu = 1; if (s.ack) p->got_ack = 1; }
        } else if (kind == V92_UP_CPU) {
            r = v92_cpu_parse(p->fbuf, p->fn, 24, &p->cpx);
            if (r > 0) on_cpu(p);
        } else {
            r = V92_ERR_RANGE;                  /* CPus: fast parameter exchange only */
        }
    }
    return r > 0 ? 1 : r == V92_ERR_SHORT ? 0 : -1;
}

/* one received bit of a sequence; m is the number of the symbol it came in */
static void seq_bit(v92d_t *p, int bit, int32_t m, int last_of_symbol)
{
    /* E1u / E2u: a data frame of zeros right after a complete sequence */
    if (p->after_seq >= 0) {
        int elen = p->want == W_PH4 ? V92_E2U_BITS(2, 0) : V92_E1U_BITS;
        if (bit) p->after_seq = -1;
        else if (++p->after_seq == elen && last_of_symbol) {
            p->after_seq = -1;
            if (p->want == W_CPT) {
                p->got_e1u = 1;
                p->m_trn2u = m + 1;
            } else if (p->want == W_PH4) {
                p->got_e2u = 1;
                if (!p->clearing) upstream_start(p, m + 1);     /* (drn = 0: no data follows) */
            }
            p->fn = 0; p->sh.ones = 0;
            return;
        }
    }
    if (p->fn == 0) {
        if (seqhunt_bit(&p->sh, bit)) { memset(p->fbuf, 1, 17); p->fbuf[17] = 0; p->fn = 18; }
        return;
    }
    p->fbuf[p->fn++] = (uint8_t)bit;
    {
        int r = seq_try(p);
        if (r > 0) { p->fn = 0; p->sh.ones = 0; p->after_seq = 0; }
        else if (r < 0 || p->fn >= V92D_FBUF) { p->fn = 0; p->sh.ones = 0; }
    }
}

/* ---- our Phase 4 sequences ---- */

#ifdef V92D_PROBE
/* Bench only, never in the product: replace the designed filters by trivial
 * ones, so that a recording of B1u from a real modem shows how it reads CPd
 * one layer at a time. The call itself cannot work.
 *   1: z2 = 1, p1 = 0            2: z2 = 1, p1(1) = 1/2
 *   3: z2 = 0 0 0 1 1/2, p1 = 0  4: as 1 with twice the gain */
static void probe_cpd(v92_cpd_t *c)
{
    v92_filt_t *f = &c->filt;
    int k;

    if (V92D_PROBE == 7) {              /* the design, but p1 = 0 */
        for (k = 0; k < f->lp1; k++) f->coef[k] = 0;
        return;
    }
    if (V92D_PROBE == 8) {              /* the design, but z2 = one tap at 64 */
        for (k = 0; k < f->lz2; k++) f->coef[f->lp1 + k] = (int16_t)(k == 64 ? 32767 : 0);
        return;
    }
    if (V92D_PROBE == 10) {             /* the design, z2 three taps later */
        for (k = f->lz2 - 1; k >= 0; k--) f->coef[f->lp1 + k] = k >= 3 ? f->coef[f->lp1 + k - 3] : 0;
        return;
    }
    if (V92D_PROBE == 11 || V92D_PROBE == 12) {     /* the design with LP1 = 48; 12: and p1 = 0 */
        for (k = 0; k < f->lz2; k++) f->coef[48 + k] = f->coef[f->lp1 + k];
        f->lp1 = 48;
        if (V92D_PROBE == 12) for (k = 0; k < 48; k++) f->coef[k] = 0;
        return;
    }
    if (V92D_PROBE == 13 || V92D_PROBE == 14) {     /* the design with LZ2 = 96 (taps 16..111); 13: and p1 = 0 */
        for (k = 0; k < 96; k++) f->coef[f->lp1 + k] = f->coef[f->lp1 + 16 + k];
        f->lz2 = 96;
        if (V92D_PROBE == 13) for (k = 0; k < f->lp1; k++) f->coef[k] = 0;
        return;
    }
    if (V92D_PROBE == 9) {              /* the design at half the gain */
        f->gain4 = (uint16_t)(f->gain4 / 2);
        return;
    }
    f->lz1 = f->lp2 = 0; f->lp1 = 1; f->lz2 = 1;
    for (k = 0; k < 8; k++) f->coef[k] = 0;
    f->coef[1] = 32767;
    if (V92D_PROBE == 2) f->coef[0] = 8192;
    if (V92D_PROBE == 3) { f->lz2 = 5; f->coef[1] = 0; f->coef[4] = 32767; f->coef[5] = 16384; }
    if (V92D_PROBE == 4) f->gain4 = (uint16_t)(f->gain4 * 2);
    if (V92D_PROBE >= 5) {      /* 5: the designed lengths, z2 = one tap at 64, p1 = 0;
                                   6: ... and p1(2) = 1/2 */
        f->lp1 = 64; f->lz2 = 128;
        for (k = 0; k < 192; k++) f->coef[k] = 0;
        f->coef[64 + 64] = 32767;
        if (V92D_PROBE == 6) f->coef[1] = 8192;
    }
}
#endif

/* SUVd says nothing about the design: it is ready as soon as Phase 4 begins */
static void build_suvd(v92d_t *p)
{
    int D = V90_MAP_D(&p->tx.map[V90TX_MAP_TRAIN]), a, n;
    for (a = 0; a < 2; a++) {
        v92_suvd_t s;
        s.silence = 0; s.ack = (uint8_t)a;
        n = v92_suvd_build(&s, D, p->sbits);
        v90_pack_bits(p->sbits, n, p->xsuvd[a]);
        (void)v90tx_set_xseq(&p->tx, a ? V90TX_XSEQ_SUVD_ACK : V90TX_XSEQ_SUVD, p->xsuvd[a], n);
    }
}

static void build_seqs(v92d_t *p)
{
    int D = V90_MAP_D(&p->tx.map[V90TX_MAP_TRAIN]), a, n;
    build_suvd(p);
    for (a = 0; a < 2; a++) {
        v92_cpd_t *c = &p->cpd;
        c->ack = (uint8_t)a;
        n = v92_cpd_build(c, D, p->sbits);
        if (n <= 0 || n > V92D_SBUF) { fail(p, V92D_ERR_DESIGN); return; }
        v90_pack_bits(p->sbits, n, p->xcpd[a]);
        (void)v90tx_set_xseq(&p->tx, a ? V90TX_XSEQ_CPD_ACK : V90TX_XSEQ_CPD, p->xcpd[a], n);
        p->ncpd = n;
    }
}

/* the receive codec's quantising noise at this sample: none to speak of
 * when it hands us 16-bit linear samples (1/12 of a unit squared) */
static uint32_t qvar(const v92d_t *p, int32_t a)
{
    return p->cfg.rx_linear ? 0 : v92_dsn_qvar(p->cfg.alaw, a);
}

/* The baseline follower's step for one decision (Q4; see the data mode
 * loop in v92d_tick): an eighth of the error e = y - tent, less where the
 * error says little about the baseline.
 *   - The codec's quantising error at the point: at a large point its step
 *     is far more than the noise (+-128 at 4220 on mu-law), and an eighth
 *     of it moved the baseline by a third of the smallest spacing at once.
 *     Where the quantiser's variance q is more than twice the noise of the
 *     line the step is scaled by 2 noise / q.
 *   - The decision's reliability: an error beyond a quarter of the spacing
 *     on its side may be a wrong decision, which moves the baseline away
 *     from the truth and makes the next wrong decision likelier (bursts of
 *     upstream errors at the top rates, docs/history/PROGRESS.md answer-fix 1). Its step
 *     falls linearly to nothing at half the spacing.
 * Where the errors are small (the baseline followed, the decisions right)
 * this is the plain eighth. With a linear codec (cfg.rx_linear) it is the
 * plain eighth always: no quantiser, and on the real calls recorded so (106,
 * v90d's HT802) the second rule moved the modulus decoder's flagged frames
 * both ways (8 recordings x configurations one more, 15 one fewer) and
 * changed no decoded bit: it is not taken where it was not shown to help. */
static int32_t base_step(const v92d_t *p, int32_t e, int32_t tent)
{
    const v92_map_t *m = p->dec.map;
    int i, eta, lc;
    int32_t h, a = e < 0 ? -e : e;
    uint32_t q, nz = p->noise > 0 ? p->noise : 1;
    int64_t st = (int64_t)e * 2;
    if (p->cfg.rx_linear) return (int32_t)st;
    i = p->dec.i_in == 0 ? V92_DFRAME - 1 : p->dec.i_in - 1;            /* the interval of tent */
    eta = v92_slice(m, i, tent); lc = v92_lc(m, i);
    /* (the quantiser works on the received amplitude) */
    q = qvar(p, p->up_scale == 0 || p->up_scale == UP_UNITY ? tent : tent * UP_UNITY / p->up_scale);
    if (e > 0) h = eta < lc - 1 ? (v92_point(m, i, eta + 1) - tent) / 2 : INT32_MAX;
    else h = eta > -lc ? (tent - v92_point(m, i, eta - 1)) / 2 : INT32_MAX;
    if (q > 2 * (uint64_t)nz) st = st * 2 * nz / q;
    if (h < 2) h = 2;
    if (a >= h) return 0;
    if (a > h / 2) st = st * 2 * (h - a) / h;
    return (int32_t)st;
}

static void design_start(v92d_t *p)
{
    v92_dsn_cfg c;
    int64_t ge = 0;
    uint32_t pred;
    int j;
    memset(&c, 0, sizeof c);
    c.alaw = p->cfg.alaw;
    for (j = 0; j < V92_CH_NTAP; j++) c.h[j] = v92_trn_h(&p->trn, j);
    c.rx_linear = p->cfg.rx_linear;
    c.noise = p->noise; c.q_quiet = p->q_quiet;
    /* the echo in data mode: what the canceller's taps make of the assumed
     * downstream power, if that is more than TRN1d gave */
    for (j = 0; j < EC_NTAP; j++) { int64_t v = p->ec.h[j] >> 12; ge += v * v; }
    pred = (uint32_t)(((uint64_t)isqrt_u64((uint64_t)ge) * (uint32_t)(p->cfg.down_rms > 0 ? p->cfg.down_rms : DOWN_RMS)) >> 12);
    c.echo_rms = pred > p->echo_rms ? pred : p->echo_rms;
    c.res = p->trn.res; c.q_trn = p->q_trn;
    c.ltot = (uint16_t)p->cfg.ltot; c.lmax = (uint16_t)p->cfg.lmax;
    c.rates = p->ja.rates;
    c.max_rate = p->cfg.max_up_rate;
    if (p->reneg_ours == RENEG_DROP && p->up_rate) {
        /* our rate drop: one step below the rate in use (which carries the
         * earlier drops already); other renegotiations have no such ceiling */
        int lim = p->up_rate - 4000;
        if (!c.max_rate || lim < c.max_rate) c.max_rate = lim;
    }
    c.kappa_q4 = p->cfg.kappa_q4;
    v92_dsn_start(&p->dsn, &c);
    p->dsn_state = 1;
    /* the design has its copy: the estimator's work is not needed until the
     * next rate renegotiation, and the processor's time is (reading 56) */
    v92_trn_hold(&p->trn, 1);
}

static void sel(v92d_t *p, v90tx_sig_t s)
{
    if (v90tx_signal(&p->tx) != s && v90tx_pending(&p->tx) != s) (void)v90tx_select(&p->tx, s);
}

/* ---- rate renegotiation ---- */

/* Ru heard in data mode: how many of the newest received samples (the
 * decoder has them all) are Ru. Ru repeats every 6 samples and is its own
 * inverse 3 later; what the channel makes of its start rises over some 15
 * samples, so only the signs are compared (a sample too small to have one,
 * an eighth of the level, passes): the run of samples whose sign is that 6
 * before and the opposite of that 3 before, plus the 6 the first of them
 * looks back on. Data passes a sample by chance one time in four, so the
 * start comes out early by a sample or so. 0: none found. */
static unsigned ru_run(const v92d_t *p)
{
    int32_t k = p->data_k - 1, a = 0, fl, i;
#define RB(j) ((int32_t)v92_trn_sample(&p->trn, (j)))
    for (i = 0; i < 12; i++) a += RB(k - i) < 0 ? -RB(k - i) : RB(k - i);
    fl = a / 96;                                /* an eighth of the mean level */
    if (fl < RU_FLOOR / 8) return 0;
    for (i = 0; i < 240; i++) {
        int32_t x0 = RB(k - i), x6 = RB(k - i - 6), x3 = RB(k - i - 3);
        if (x0 > -fl && x0 < fl) continue;
        if (!(x6 > -fl && x6 < fl) && (x0 > 0) != (x6 > 0)) break;
        if (!(x3 > -fl && x3 < fl) && (x0 > 0) == (x3 > 0)) break;
    }
#undef RB
    return i < 12 ? 0 : (unsigned)i + 6;
}

/* Ru in data mode: the data frames the decoder still holds that end at
 * least a data frame before where Ru was found to begin go on (the start is
 * found late by the channel's spread of Ru's beginning; no frame that could
 * hold Ru is delivered); then no more data. What the caller's prefilter
 * still held when Ru began never reached the line (V.92 begins Ru on a data
 * frame boundary): that is lost. */
static void ru_heard(v92d_t *p)
{
    if (p->rx_ready && p->rx_on && !p->cd_quiet) {
        unsigned n = ru_run(p), j;
        int32_t y[4 * V92_VIT_DEPTH + V92_TFRAME];
        /* the decoder's inputs again, the newest first, each against the
         * baseline it was taken with (Ru, decided as data before it was
         * heard, has moved the baseline since by up to some 125) */
        for (j = 0; j < sizeof y / sizeof y[0]; j++) {
            int32_t k = p->data_k - 1 - (int32_t)j;
            y[j] = up_in(p, v92_trn_sample(&p->trn, k)) - p->trn.sb[k & 255];
        }
        if (n > 0) v92_dec_flush_before(&p->dec, n + V92_DFRAME, y);
    }
    p->rx_ready = 0;
}

static void reneg_begin(v92d_t *p, int ours)
{
    /* our own: what the caller sends until it has seen Rd is still data */
    p->tx_ready = 0; if (!ours) p->rx_ready = 0;
    p->in_reneg = 1; p->reneg_ours = ours; p->revs = 0;
    p->got_suvu = p->got_cpu = p->got_ack = p->got_e2u = 0;
    p->cpd_sent = 0; p->t_cpd = 0;
    p->dsn_state = 0;
    if (ours) (void)v90tx_select(&p->tx, V90TX_RD);     /* on the next data frame boundary */
    enter(p, V92D_RENEG);
}

/* the caller's Ru has just turned into Ru-bar: TRN2u is 24 symbols away */
static void reneg_resume(v92d_t *p)
{
    int32_t kr = p->trn.k - 1 - V92_P6_REV_LAG;                     /* first sample of Ru-bar */
    int32_t mt = kr - (p->trn.cpk - V92_CH_LEAD) + 24, d;
    /* Ru began on a data frame boundary (9.8.2.1.1) */
    d = (mt - p->m_b1u) % 12; if (d < 0) d += 12;
    mt += d < 6 ? -d : 12 - d;
    p->rx_on = 0;
    v92_trn_resume(&p->trn, mt);
    v92_trn_hold(&p->trn, 0);
    p->trn_on = 1; p->four = 1; p->m_trn2u = mt;
    v92_ml_init(&p->ml, p->jp.pt8_reneg, v92_ru_sbit(23, 1));
    seq_reset(p, W_PH4);
}

/* A dead line (the caller has hung up, the network sends the idle code)
 * leaves the canceller giving out its own estimate of our echo: as loud as
 * the echo was, and as periodic as what we send (Rd has Ru's shape). The
 * caller's signal has to be in the line itself as well as in what the
 * canceller leaves of it.
 *
 * line_idle: one sample of the line; 1 = it has brought nothing but the idle
 * code for LINE_IDLE_RUN samples, and what Ru the detector reads is ours. */
static int line_idle(v92d_t *p, int16_t rxs)
{
    if (rxs < -LINE_IDLE || rxs > LINE_IDLE) p->cd_idle = 0;
    else if (p->cd_idle < 255) p->cd_idle++;
    return p->cd_idle >= LINE_IDLE_RUN;
}

/* Loss of the upstream signal in data mode: far below what B1u brought, in
 * the line or after the canceller, for about half a second. One sample in:
 * the line, what the canceller left; 1 = lost. */
static int carrier_lost(v92d_t *p, int16_t rxs, int16_t x)
{
    uint64_t q = p->sig_ms >> CD_SHIFT, line;
    p->cd_acc += (int32_t)x * x;
    p->cd_line += (uint32_t)((int32_t)rxs * rxs) >> CD_LINE_SHIFT;
    if (++p->cd_n < CD_BLOCK) return 0;
    line = (uint64_t)p->cd_line << CD_LINE_SHIFT;
    if ((uint64_t)p->cd_acc / CD_BLOCK < q || line / CD_BLOCK < q) p->cd_quiet++; else p->cd_quiet = 0;
    p->cd_acc = 0; p->cd_line = 0; p->cd_n = 0;
    return p->cd_quiet >= CD_BLOCKS;
}

/* ---- the tick ---- */

uint8_t v92d_tick(v92d_t *p, int16_t rxs)
{
    uint8_t octet;
    int16_t x;
    v90tx_sig_t sig;
    int r;

    p->now++; p->t++;
    octet = v90tx_tick(&p->tx);
#ifdef P34D_TRACE
    ecdly_put(&p->ecd, v92d_trace_tx(p, (int16_t)v90_octet_linear(p->cfg.alaw, octet)));
#else
    ecdly_put(&p->ecd, (int16_t)v90_octet_linear(p->cfg.alaw, octet));
#endif
    /* The echo: the canceller of ../modem/ec for the first 12 ms, and behind it
     * a model of a slow tail (reading 53): the transmitted signal, 12 ms on,
     * through two first order low-passes (16 and 32 ms), one coefficient
     * each. All of it learns from what is left after both, while the caller
     * is silent. */
    {
        int32_t e, est, u1, u2, tl = ecdly_ref(&p->ecd, EC_NTAP);
        e = ec_tick(&p->ec, ecdly_ref(&p->ecd, 0), rxs, -1);
        p->et_s[0] += tl - (p->et_s[0] >> 7);
        p->et_s[1] += tl - (p->et_s[1] >> 8);
        u1 = p->et_s[0] >> 5; u2 = p->et_s[1] >> 6;
        est = (int32_t)(((int64_t)p->et_g[0] * u1 + (int64_t)p->et_g[1] * u2 + 32768) >> 16);
        e -= est;
        p->et_pw += ((int64_t)u1 * u1 + (int64_t)u2 * u2 - p->et_pw) >> 10;
        if (p->ec_mu >= 0) {
            ec_adapt(&p->ec, e, p->ec_mu);
            if (p->et_pw > 4096) {
                p->et_g[0] += (int32_t)((int64_t)e * u1 * 1024 / p->et_pw);
                p->et_g[1] += (int32_t)((int64_t)e * u2 * 1024 / p->et_pw);
            }
        }
        x = (int16_t)(e > 32767 ? 32767 : e < -32767 ? -32767 : e);
    }
    ecdly_next(&p->ecd);
#ifdef P34D_TRACE
    v92d_trace(p, rxs, x);      /* live/replay.c: what the canceller left */
#endif
    sig = v90tx_signal(&p->tx);

    if (p->state >= V92D_RETRAIN) return octet;

    /* ---- receiver ---- */
    if (p->state != V92D_WAIT_RU && p->state != V92D_MD && tonedet_tick(&p->tone_a, x)) { retrain(p, 0); return octet; }

    if (p->trn_at && p->now == p->trn_at) {
        p->trn_at = 0;
        v92_trn_init(&p->trn); v92_sgn_init(&p->sgn, 0);
        p->trn_on = 1; p->four = 0;
        seq_reset(p, p->state == V92D_TRN1U ? W_JA : W_CPT);
    }
    if (p->trn_on == 1) {
        p->q_trn = (uint32_t)((int64_t)p->q_trn + ((int64_t)qvar(p, rxs) - (int64_t)p->q_trn) / 1024);
        r = v92_trn_tick(&p->trn, x);
        if (r < 0) { fail(p, p->state == V92D_TRN1U ? V92D_ERR_JA : V92D_ERR_CPT); return octet; }
        if (r == 1 && p->trn.dec_m >= 0) {
            int32_t m = p->trn.dec_m;
            if (!p->four) {
                int sbit = p->trn.dec_level < 0;
                int bit = v92_sgn_rx_bit(&p->sgn, sbit);
#ifdef P34D_TRACE
                v92d_trace_sym(p, bit);
#endif
                seq_bit(p, bit, m, 1);
                if (p->got_e1u && !p->four) {       /* TRN2u follows (8.7.6) */
                    p->four = 1;
                    v92_trn_levels(&p->trn, 4);
                    v92_ml_init(&p->ml, p->jp.pt8_train, sbit);
                    seq_reset(p, W_PH4);
                }
            } else if (m >= p->m_trn2u) {
                uint8_t b[3];
                v92_ml_rx(&p->ml, p->trn.dec_level, b);
#ifdef P34D_TRACE
                v92d_trace_sym(p, b[0] | (b[1] << 1));
#endif
                seq_bit(p, b[0], m, 0);
                if (p->state < V92D_RETRAIN) seq_bit(p, b[1], m, 1);
            }
            if (p->state >= V92D_RETRAIN) return octet;
        }
    } else if (p->trn_on == 2) {
        v92_trn_store(&p->trn, x);
    }
    if (p->rx_on) {
        int n = 0;
        if (p->b1_left > 0) { p->cd_acc += (int32_t)x * x; p->cd_n++; }
        while (p->rx_on && p->data_k < p->trn.k) {
            /* the decoder started over on B1u (up_measure): two samples a
             * tick until it is back, so no tick carries the lot. (Some
             * 150 ticks, inside B1u: no Ru can come then, so ru_heard
             * never reads behind a decoder that is catching up) */
            if (p->up_scale && p->up_scale != UP_UNITY && n++ == 2) break;
#ifdef P34D_TRACE
            v92d_trace_data(p, v92_trn_sample(&p->trn, p->data_k));
#endif
            {
                /* The baseline. What the canceller and its tail estimate leave
                 * of our own low frequencies comes back as a slow wander under
                 * the caller's symbols (a U.S. Robotics 5686: 100 units for
                 * tens of symbols, the points 245 apart; whole runs of symbols
                 * were then decided one point off). Each decision's error moves
                 * the baseline by an eighth: slow against the symbols, fast
                 * against the wander; less where the error is the codec's
                 * quantiser or may be a wrong decision (base_step). */
                int32_t y, tent;
                /* the baseline this sample is taken against, kept for ru_heard
                 * (trn.sb is free in data mode: v92_trn_resume fills it
                 * again before the training receiver reads it) */
                p->trn.sb[p->data_k & 255] = (int16_t)(p->base_q4 >> 4);
                y = up_in(p, v92_trn_sample(&p->trn, p->data_k++)) - (p->base_q4 >> 4);
                v92_dec_put(&p->dec, y, &tent);
                p->base_q4 += base_step(p, y - tent, tent);
                if (p->base_q4 > BASE_MAX_Q4) p->base_q4 = BASE_MAX_Q4;
                if (p->base_q4 < -BASE_MAX_Q4) p->base_q4 = -BASE_MAX_Q4;
                if (p->b1_sym >= 0) b1u_lead(p, (int16_t)(y > 32767 ? 32767 : y < -32767 ? -32767 : y));
                if (!p->up_scale && p->b1_left > 0 && p->data_k - (p->m_b1u + p->dsn.dly) == V92D_UP_EST)
                    up_measure(p, p->m_b1u + p->dsn.dly);
            }
        }
        if (p->rx_on && p->trn_on == 2 && p->trn.k >= CLK_REBASE_AT) rebase(p);
        if (p->state >= V92D_RETRAIN) return octet;
    }

    /* ---- procedures ---- */
    switch (p->state) {
    case V92D_WAIT_RU:
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if (r & V92_P6_REV) {
            if (p->cfg.md_len > 0 && !p->revs) { p->revs = 1; enter(p, V92D_MD); break; }     /* 9.5.1.1.1 */
            p->trn_at = p->now - V92_P6_REV_LAG + 24 - TRN_LEAD;
            enter(p, V92D_TRN1U);
        } else if ((int64_t)p->t > MS(T_RU_MS) + p->cfg.md_len * 276) {
            fail(p, V92D_ERR_NO_RU);
        }
        break;

    case V92D_MD:
        /* Ru-bar ends 24 - LAG symbols from the reversal; then MD; listen again a little early */
        if ((int64_t)p->t >= 20 + p->cfg.md_len * 276 - 48) { v92_p6_init(&p->p6); p->state = V92D_WAIT_RU; }
        break;

    case V92D_TRN1U:
        if (p->got_ja) {
            p->peak1 = v92_trn_peak(&p->trn);
            p->trn_on = 0;
            if (v90tx_load_dil(&p->tx, &p->ja.dil) != 0 || v90tx_set_jd(&p->tx, &p->jd) != 0) { fail(p, V92D_ERR_JA); break; }
            (void)v90tx_select(&p->tx, V90TX_SD);       /* 9.5.1.1.3; Sd-bar and TRN1d follow by themselves */
            enter(p, V92D_SD);
        } else if ((int64_t)p->t > MS(T_JA_MS) + p->cfg.rtd) {
            fail(p, V92D_ERR_JA);
        }
        break;

    case V92D_SD:
        if (sig == V90TX_TRN1D) {
            int32_t n = (int32_t)v90tx_symbols(&p->tx), from = p->cfg.rtd + MS(EC_GUARD_MS), len;
            if (n == 1) p->t_trn1d = p->now;
            if (p->cfg.echo_delay < 0) {
                /* first find out how late the echo is (../modem/ecdly.h), on the
                 * change from sample to sample: 9.5.2.1.3 has the caller stop
                 * Ja on our Sd-bar, so it is silent */
                if (ecdly_measure(&p->ecd, n - from, rxs, 1)) {
                    ec_init(&p->ec);
                    p->et_g[0] = p->et_g[1] = 0;
                }
                from += ECDLY_SAMPLES;
            }
            len = TRN1D_SYMS + from;
            /* the caller is silent now: learn the echo of TRN1d */
            p->ec_mu = n < from ? -1 : n < from + EC_FAST ? 0 : 3;
            if (n >= len - NOISE_SYMS) {
                p->nz_acc += (int32_t)x * x;
                p->nq_acc += qvar(p, rxs);
                p->ne_acc += (int32_t)rxs * rxs;
                p->nz_n++;
            }
            if (n >= len) {
                p->ec_mu = -1;
                p->noise = (uint32_t)(p->nz_acc / p->nz_n);
                p->q_quiet = (uint32_t)(p->nq_acc / p->nz_n);
                p->echo_rms = isqrt_u64((uint64_t)(p->ne_acc / p->nz_n));
                (void)v90tx_select(&p->tx, V90TX_JD);                   /* 9.5.1.1.4 */
                v92_p6_init(&p->p6); p->revs = 0;
                enter(p, V92D_JD);
            }
        }
        break;

    case V92D_JD:
        /* 9.5.1.1.6 to .8: Su, its reversal, then Jp with the phase */
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if (r & V92_P6_REV) {
            int n;
            /* The peak of the channel response sat peak1 / 256 samples from a
             * sampling instant during the first TRN1u; the caller has since
             * slipped half a symbol (9.5.2.1.7). Ask for the further slip
             * that puts the peak on a sampling instant. */
            p->jp.frac = (uint16_t)(((-p->peak1 - 128) & 255) << 8);
            p->jp.pt8_train = 0; p->jp.pt8_reneg = 0;
            n = v92_jp_build(&p->jp, p->sbits);
            v90_pack_bits(p->sbits, n, p->xjp);
            (void)v90tx_set_xseq(&p->tx, V90TX_XSEQ_JP, p->xjp, n);
            (void)v90tx_select(&p->tx, V90TX_JP);                       /* after the current Jd */
            enter(p, V92D_JP);
        } else if (clk_diff(p->now, p->t_trn1d) > MS(T_SU_MS) + p->cfg.rtd) {
            retrain(p, 1);                                              /* 9.5.1.2.2 */
        }
        break;

    case V92D_JP:
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if ((r & V92_P6_REV) && sig == V90TX_JP) {
            /* 9.5.1.1.9: Jp' after the current Jp; the DIL or SCR follow by themselves */
            (void)v90tx_select(&p->tx, V90TX_JP_PRIME);
            p->trn_at = p->now - V92_P6_REV_LAG + 24 - TRN_LEAD;
            p->got_cpt = p->got_e1u = p->ri_sent = 0;
            enter(p, V92D_TRAIN);
        } else if ((int64_t)p->t > MS(T_SU_MS) + p->cfg.rtd) {
            retrain(p, 1);
        }
        break;

    case V92D_TRAIN:
        /* 9.5.1.1.13: no DIL, Ri when trained */
        if (p->ja.dil.n == 0 && !p->ri_sent && sig == V90TX_SCR && p->trn_on == 1 && p->trn.neq >= TRAINED_SYMS) {
            (void)v90tx_select(&p->tx, V90TX_RI);
            p->ri_sent = 1;
        }
        if (p->got_e1u) {
            if (p->ja.dil.n > 0) (void)v90tx_select(&p->tx, V90TX_RI_BAR);          /* 9.5.1.1.12 */
            p->got_suvu = p->got_cpu = p->got_ack = p->got_e2u = 0;
            p->cpd_sent = 0; p->t_cpd = 0; p->dsn_state = 0;
            enter(p, V92D_PH4);
        } else if ((int64_t)p->t > MS(T_TRAIN_MS) + p->cfg.rtd) {
            fail(p, V92D_ERR_CPT);
        }
        break;

    case V92D_PH4: {
        int ack = p->got_cpu;
        v90tx_sig_t suvd = ack ? V90TX_SUVD_ACK : V90TX_SUVD, cpd = ack ? V90TX_CPD_ACK : V90TX_CPD;
        int is_suvd = sig == V90TX_SUVD || sig == V90TX_SUVD_ACK, is_cpd = sig == V90TX_CPD || sig == V90TX_CPD_ACK;

        /* the design, on the estimate TRN2u has refined */
        if (p->dsn_state == 0 && p->trn_on == 1 && p->four &&
            p->trn.dec_m - p->m_trn2u >= (p->in_reneg ? RENEG_REFINE : REFINE_SYMS)) design_start(p);
        if (p->dsn_state == 1 && (p->cfg.dsn_pace <= 1 || p->now % p->cfg.dsn_pace == 0)) {
            r = v92_dsn_step(&p->dsn);
            if (r < 0) { fail(p, V92D_ERR_DESIGN); break; }
            if (r == 1) {
                p->cpd = p->dsn.cpd;
                p->cpd.e2u_extend = 0;
#ifdef V92D_PROBE
                probe_cpd(&p->cpd);
#endif
                p->dsn_state = 2;
                build_seqs(p);
                if (p->state != V92D_PH4) break;
            }
        }
        if (p->dsn_state != 2 && !p->in_reneg && sig == V90TX_TRN2D
            && v90tx_units(&p->tx) >= (uint32_t)TRN2D_FRAMES && p->t >= MS(SUVD_HOLD_MS)) {
            /* 9.6.1.1.1: TRN2d for at least 2040 T, then SUVd. The design may
             * take longer than a caller waits for SUVd (a U.S. Robotics 5686
             * went on by itself 3.8 s into TRN2u); CPd follows when it is done. */
            build_suvd(p);
            sel(p, suvd);
        }
        if (p->dsn_state == 2) {
            if (sig == V90TX_TRN2D) {
                /* 9.6.1.1.1: TRN2d for at least 2040 T, then SUVd */
                if (v90tx_units(&p->tx) >= (uint32_t)(p->in_reneg ? RENEG_TRN2D : TRN2D_FRAMES)) sel(p, suvd);
            } else if ((sig == V90TX_SUVD_ACK || sig == V90TX_CPD_ACK) && v90tx_units(&p->tx) >= 1
                       && (p->got_ack || p->got_e2u) && v90tx_pending(&p->tx) == V90TX_NONE) {
                /* 9.6.1.1.4: Ed, then B1d and data by themselves. In our
                 * cleardown this is where it is done: the caller has
                 * acknowledged our CPd with drn = 0 and has our SUVd', and
                 * no data mode follows drn = 0 (9.11): silence, not Ed */
                if (p->clearing) { fail(p, V92D_ERR_HANGUP); break; }
                v90tx_set_getbit(&p->tx, p->cfg.getbit, p->cfg.user);
                (void)v90tx_select(&p->tx, V90TX_ED);
                enter(p, V92D_ED);
            } else if (is_suvd) {
                if (p->cpd_sent == 1) { p->cpd_sent = 2; p->t_cpd = p->now; }       /* the CPd is out */
                /* 9.6.1.1.2. The first CPd without the acknowledge bit, also when
                 * CPu is in already (SUVd went out before the design was done):
                 * a U.S. Robotics 5686 answered a first CPd that had the bit set
                 * with silence, seven calls of seven, and a plain one never. */
                if (p->got_suvu && !p->cpd_sent) sel(p, V90TX_CPD);
                else if (p->cpd_sent == 2 && !p->got_ack
                         && clk_diff(p->now, p->t_cpd) > MS(100) + p->cfg.rtd + 1200) sel(p, cpd);     /* 9.6.1.1.3 */
                else if (sig != suvd) sel(p, suvd);                                 /* the acknowledge bit */
            } else if (is_cpd) {
                /* one CPd at a time, then SUVd: a repeat of 9.6.1.1.3 too,
                 * with SUVd between the copies until it is answered. That is
                 * 9.6.1.1.2's single CPd followed by SUVd, once per copy
                 * (9.6.1.1.3 says only "repeated CPd sequences", and Figures
                 * 12 to 14 draw no repeat); an Agere engine cleared down on
                 * CPds sent back to back */
                p->cpd_sent = 1;
                sel(p, suvd);
            }
        }
        if (p->state == V92D_PH4 && p->clearing && clk_diff(p->now, p->t_clear) > MS(T_CLEAR_MS)) fail(p, V92D_ERR_HANGUP);
        if (p->state == V92D_PH4 && (int64_t)p->t > MS(p->in_reneg ? T_RENEG_MS + pace_ms(p) : T_PHASE4_MS) + 6 * p->cfg.rtd) {
            if (p->in_reneg) retrain(p, 1); else fail(p, V92D_ERR_CPU);
        }
        break;
    }

    case V92D_ED:
        if (sig == V90TX_DATA) p->tx_ready = 1;
        if (p->tx_ready && p->rx_ready) {
            if (p->in_reneg) { p->renegs++; p->in_reneg = 0; }
            v92_p6_init(&p->p6); p->cd_idle = 0;
            enter(p, V92D_DATA);
        } else if ((int64_t)p->t > MS(p->in_reneg ? T_RENEG_MS + pace_ms(p) : T_PHASE4_MS) + 6 * p->cfg.rtd) {
            if (p->in_reneg) retrain(p, 1); else fail(p, V92D_ERR_B1);
        }
        break;

    case V92D_DATA:
        /* 9.8.1.2.1: Ru in data mode */
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if (line_idle(p, rxs)) r = 0;           /* our own echo on a dead line */
        if (r & V92_P6_ON) { ru_heard(p); reneg_begin(p, 0); break; }
        if (carrier_lost(p, rxs, x)) { fail(p, V92D_ERR_CARRIER); break; }
        /* is the line still good enough for the rate? */
        if ((p->t & 8191) == 0) {
            uint32_t nb = p->dec.bad_frames - p->bad_seen;
            p->bad_seen = p->dec.bad_frames;
            p->q_bad = nb >= 4 ? p->q_bad + 1 : 0;
            if (p->q_bad >= 2) {
                p->rate_drops++;
                if (p->up_rate - 4000 < 24000) { retrain(p, 1); p->up_floor = 1; }
                else reneg_begin(p, RENEG_DROP);
            }
        }
        break;

    case V92D_RENEG:
        /* until Ru-bar the caller sends data or Ru: a signal that dies away
         * can look like the start of Ru to the detector (in our cleardown
         * T_CLEAR_MS ends it in time, the caller there or not) */
        if (!p->revs && !p->clearing && carrier_lost(p, rxs, x)) { fail(p, V92D_ERR_CARRIER); break; }
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if (line_idle(p, rxs)) r = 0;           /* our own Rd, Rd-bar on a dead line */
        if (r && p->rx_ready) ru_heard(p);     /* Ru: no more data (what came before it, whole) */
        if ((r & V92_P6_REV) && !p->revs) {
            p->revs = 1;
            reneg_resume(p);
            if (!p->reneg_ours) (void)v90tx_select(&p->tx, V90TX_RD);   /* 9.8.1.2.2 */
        }
        /* Rd for 384 T, then Rd-bar; TRN2d follows by itself */
        if (sig == V90TX_RD && v90tx_units(&p->tx) >= RD_UNITS && v90tx_pending(&p->tx) == V90TX_NONE)
            (void)v90tx_select(&p->tx, V90TX_RD_BAR);
        if (p->clearing) {
            /* 9.11: a rate renegotiation to send drn = 0. 9.8.1.1.2: TRN2d,
             * then SUVd, whether the caller's Ru-bar came or not (a caller
             * that no longer hears us sends none). With Ru-bar the receiver
             * is on TRN2u and Phase 4 does the rest: on SUVu the CPd in use
             * with drn = 0 (9.6.1.1.2), no design. Without, SUVd until
             * T_CLEAR_MS. */
            if (sig == V90TX_TRN2D && v90tx_units(&p->tx) >= RENEG_TRN2D) sel(p, V90TX_SUVD);
            if (p->revs && (sig == V90TX_RD_BAR || sig == V90TX_TRN2D || sig == V90TX_SUVD)) enter(p, V92D_PH4);
            else if (clk_diff(p->now, p->t_clear) > MS(T_CLEAR_MS)) fail(p, V92D_ERR_HANGUP);
        } else if (p->revs && (sig == V90TX_RD_BAR || sig == V90TX_TRN2D)) enter(p, V92D_PH4);
        else if (p->t > MS(T_RENEG_MS)) retrain(p, 1);
        break;

    default:
        break;
    }
    return octet;
}

int v92d_renegotiate(v92d_t *p)
{
    if (p->state != V92D_DATA) return -1;
    reneg_begin(p, 1);
    return 0;
}

int v92d_cleardown(v92d_t *p)
{
    if (p->state != V92D_DATA) return -1;
    reneg_begin(p, 1);
    /* the CPd in use, drn = 0 (9.11): CPd and CPd' with the bit, SUVd; no
     * design (dsn_state 2: the CPd is ready) */
    p->clearing = 1; p->t_clear = p->now;
    p->cpd.drn = 0;
    p->dsn_state = 2;
    build_seqs(p);
    return 0;
}

int v92d_retrain(v92d_t *p)
{
    if ((p->state != V92D_DATA && !p->in_reneg) || p->clearing) return -1;
    retrain(p, 1);
    return 0;
}
