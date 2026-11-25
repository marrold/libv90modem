/* V.34 Phase 2, answer modem (modem/p2ans) against the call-side test peer
 * (sim/p2c) over a delayed, attenuated, noisy, echoing, band-limited path:
 * INFO contents, the timings 11.2.1.2 gives, the symbol rate decision in
 * INFO1a, the recovery procedures of 11.2.2.2 and the retrain entry; x2's
 * marker; K56flex's INFO1a flag and offset nudge (k56 design, decision 6). */
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/p2ans.h"
#include "../sim/p2c.h"
#include "../sim/noise.h"

#define MAXD 2048
#define NLP 41
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    int d_ca, d_ac;         /* one-way delays, samples: caller -> answer, answer -> caller */
    double g_ca, g_ac;      /* path gains */
    double echo_a, echo_c;  /* each side's own transmit leaking into its receiver */
    double noise;           /* RMS at each receiver */
    double lp_hz;           /* low-pass on the caller -> answer path, 0 = none */
    int mute_ca_from, mute_ca_to;   /* caller -> answer path dead in this window (samples) */
    int mute_ac_from, mute_ac_to;
    int io_tx, io_rx;       /* our own sample path: delay to the line terminals and back */
    int retrain;            /* both ends start in the retrain entry */
    int seconds;
    /* x2 (docs/history/x2-facts.md "Phase 2 marker"): from the start of our
     * L1 the caller is silent and, mk_at ms after that start, an x2 client's
     * marker frame with body mk_body goes out on its carrier (1200 Hz); 0 =
     * no marker. mk_bad: one CRC bit inverted. */
    int mk_at, mk_body, mk_bad, mk_nofill;
    double foff;            /* Hz: the caller -> answer path shifts every frequency by this */
} scen_t;

/* our transmit from the start of our first L1 on, per run slot */
#define TXLOG (8000 * 8)
static int16_t txlog[2][TXLOG];
static int g_slot;

typedef struct {
    p2ans_t a; p2c_t c;
    int n, a_done_at, c_done_at;
    int l1_samples, l2_samples;
    double l1_pw, l2_pw;
    int c_restarts;         /* times the harness played the caller's side of a retrain */
    int l1_at, x2_at;       /* sample of our first L1, of P2N_X2; -1 = none */
    int ntx;                /* samples in txlog[g_slot] */
    int x2_quiet;           /* nothing sent from 10 ms after P2N_X2 on */
    int k56_at;             /* sample of P2N_K56, -1 = none */
    int k56_quiet;          /* nothing sent from P2N_K56 on */
} run_t;

static double lp[NLP];
static void lp_design(double fc)
{
    int k; double s = 0;
    for (k = 0; k < NLP; k++) {
        double t = k - (NLP - 1) / 2.0, w = 0.54 - 0.46 * cos(2 * M_PI * k / (NLP - 1));
        double h = fabs(t) < 1e-9 ? 2 * fc / 8000.0 : sin(2 * M_PI * fc / 8000.0 * t) / (M_PI * t);
        lp[k] = h * w; s += lp[k];
    }
    for (k = 0; k < NLP; k++) lp[k] /= s;
}

/* a frequency shift: the analytic signal (a windowed Hilbert transformer
 * of NH taps, and the input delayed to its centre) turned by foff */
#define NH 127
static double hb[NH];
static void hb_design(void)
{
    int k;
    for (k = 0; k < NH; k++) {
        int t = k - (NH - 1) / 2;
        double w = 0.54 - 0.46 * cos(2 * M_PI * k / (NH - 1));
        hb[k] = (t & 1) ? 2.0 / (M_PI * t) * w : 0.0;
    }
}

static void run(const scen_t *s, run_t *r, const p2ans_cfg *ac, const p2c_cfg *cc)
{
    static double hx[MAXD];
    static int16_t lca[MAXD], lac[MAXD], ltx[MAXD]; static double lrx[MAXD];
    rng_t rng;
    int16_t atx = 0, ctx = 0;
    int i, k, c_fail_at = -1, secs = s->seconds ? s->seconds : 8;
    static dpsktx_t mk;
    uint8_t mkbits[64];
    int nmk = 0, mk_off = 0;

    memset(lca, 0, sizeof lca); memset(lac, 0, sizeof lac);
    memset(ltx, 0, sizeof ltx); memset(lrx, 0, sizeof lrx); memset(hx, 0, sizeof hx);
    if (s->lp_hz > 0) lp_design(s->lp_hz);
    if (s->foff != 0) hb_design();
    memset(r, 0, sizeof *r);
    if (s->retrain) {
        info0_t i0 = cc->info0c;
        p2ans_init_retrain(&r->a, ac, &i0);
        p2c_init_retrain(&r->c, cc);
    } else {
        p2ans_init(&r->a, ac);
        p2c_init(&r->c, cc);
    }
    rng_seed(&rng, 2024);
    r->a_done_at = r->c_done_at = -1;
    r->l1_at = r->x2_at = -1; r->x2_quiet = 1;
    r->k56_at = -1; r->k56_quiet = 1;
    if (s->mk_at) {
        dpsktx_init(&mk, DPSK_FC_B, cc->level, 0);
        nmk = p2ans_x2_info_frame((uint8_t)s->mk_body, mkbits);
        if (s->mk_bad) mkbits[2 + 4 + 8 + 7 + 3] ^= 1;
        if (s->mk_nofill) mkbits[2 + 1] = 0;    /* a fill bit: sync and CRC stay good */
    }
    for (i = 0; i < 8000 * secs; i++) {
        double xa, xc, line_a;
        /* our transmit sample reaches the line terminals io_tx samples late */
        ltx[i % MAXD] = atx;
        line_a = ltx[(i - s->io_tx + MAXD) % MAXD];
        lca[i % MAXD] = ctx; lac[i % MAXD] = (int16_t)line_a;
        if (s->lp_hz > 0) {
            xa = 0;
            for (k = 0; k < NLP; k++) xa += lp[k] * lca[(i - s->d_ca - k + 2 * MAXD) % MAXD];
            xa *= s->g_ca;
        } else {
            xa = s->g_ca * lca[(i - s->d_ca + MAXD) % MAXD];
        }
        if (s->foff != 0) {
            double h = 0, ph = 2 * M_PI * s->foff * i / 8000.0;
            hx[i % MAXD] = xa;
            for (k = 0; k < NH; k += 2) h += hb[k] * hx[(i - k + MAXD) % MAXD];    /* (odd k: 0) */
            xa = hx[(i - (NH - 1) / 2 + MAXD) % MAXD] * cos(ph) - h * sin(ph);
        }
        xc = s->g_ac * lac[(i - s->d_ac + MAXD) % MAXD];
        if (i >= s->mute_ca_from && i < s->mute_ca_to) xa = 0;
        if (i >= s->mute_ac_from && i < s->mute_ac_to) xc = 0;
        xa += s->echo_a * line_a + s->noise * rng_gauss(&rng);
        xc += s->echo_c * ctx + s->noise * rng_gauss(&rng);
        /* and what the line terminals receive reaches us io_rx samples late */
        lrx[i % MAXD] = xa;
        if (r->a.state == P2N_L1) { r->l1_samples++; r->l1_pw += (double)atx * atx; }
        if (r->a.state == P2N_L2) { r->l2_samples++; r->l2_pw += (double)atx * atx; }
        atx = p2ans_tick(&r->a, (int16_t)lrx[(i - s->io_rx + MAXD) % MAXD]);
        if (r->a.state == P2N_L1 && r->l1_at < 0) { r->l1_at = i; if (s->mk_at) r->c.mute = 1; }
        if (r->a.state == P2N_X2 && r->x2_at < 0) r->x2_at = i;
        if (r->x2_at >= 0 && i > r->x2_at + 80 && atx != 0) r->x2_quiet = 0;
        if (r->a.state == P2N_K56 && r->k56_at < 0) r->k56_at = i;
        if (r->k56_at >= 0 && atx != 0) r->k56_quiet = 0;
        if (r->l1_at >= 0 && r->ntx < TXLOG) txlog[g_slot][r->ntx++] = atx;
        ctx = p2c_tick(&r->c, (int16_t)xc);
        if (s->mk_at && r->l1_at >= 0) {
            int32_t v;
            if (i == r->l1_at + s->mk_at * 8) { dpsktx_send(&mk, mkbits, nmk); mk_off = -1; }
            if (mk_off < 0 && !dpsktx_busy(&mk)) mk_off = i + 60;      /* the last bit out, then off */
            if (i == mk_off) dpsktx_off(&mk);
            v = ctx + dpsktx_tick(&mk);
            ctx = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        /* the caller's side of a retrain (11.5.1.2), which the peer leaves to
         * the test: it gave up, so 70 ms of silence and Tone B */
        if (r->c.state == P2C_FAILED && c_fail_at < 0 && r->c_restarts < 3) c_fail_at = i;
        if (c_fail_at >= 0 && i == c_fail_at + 560) { p2c_init_retrain(&r->c, cc); r->c_restarts++; c_fail_at = -1; }
        if (r->a.state == P2N_DONE && r->a_done_at < 0) r->a_done_at = i;
        if (r->c.state == P2C_DONE && r->c_done_at < 0) r->c_done_at = i;
        if (r->a.state == P2N_FAILED) break;
        if (r->a.state == P2N_DONE && r->c.state == P2C_DONE) break;
        if (r->x2_at >= 0 && i > r->x2_at + 8000) break;
        if (r->k56_at >= 0 && i > r->k56_at + 8000) break;
    }
    r->n = i;
}

static void caller_cfg(p2c_cfg *cc)
{
    int k;
    memset(cc, 0, sizeof *cc);
    cc->level = 4000;
    cc->info0c.sr3429 = 1; cc->info0c.lo3000 = cc->info0c.hi3000 = 1;
    cc->info0c.lo3200 = cc->info0c.hi3200 = 1; cc->info0c.allow3429 = 1;
    cc->info0c.pwr_reduce = 1; cc->info0c.max_sr_diff = 5; cc->info0c.c1664 = 1;
    cc->hi_carrier_mask = 1 << 4;                   /* high carrier at 3200 only */
    for (k = 0; k < 6; k++) { cc->preemph[k] = k + 1; cc->max_rate[k] = -1; }
    cc->min_pwr_red = 2; cc->add_pwr_red = 3; cc->md_len = 0;
}

/* INFO1a as the caller received it: the same, but for the frequency offset */
static int same_info1a(const info1a_t *x, const info1a_t *y)
{
    return x->v90 == y->v90 && x->md_len == y->md_len && x->up_sr == y->up_sr && x->down_sr == y->down_sr
        && x->min_pwr_red == y->min_pwr_red && x->add_pwr_red == y->add_pwr_red
        && x->hi_carrier == y->hi_carrier && x->preemph == y->preemph && x->max_rate == y->max_rate;
}

static int same_txlog(const run_t *x, const run_t *y)
{
    int k;
    if (x->ntx != y->ntx) return 0;
    for (k = 0; k < x->ntx; k++) if (txlog[0][k] != txlog[1][k]) return 0;
    return 1;
}

int main(void)
{
    static run_t r;
    p2ans_cfg ac; p2c_cfg cc;
    scen_t s;
    int m, num, den, clean_rate;

    /* Tables 1 and 2 */
    CHECK_EQ_I(v34_symbol_rate(0), 2400); CHECK_EQ_I(v34_symbol_rate(3), 3000);
    CHECK_EQ_I(v34_symbol_rate(4), 3200); CHECK_EQ_I(v34_symbol_rate(6), 0);
    v34_carrier(0, 0, &num, &den); CHECK_EQ_I(2400 * num / den, 1600);
    v34_carrier(0, 1, &num, &den); CHECK_EQ_I(2400 * num / den, 1800);
    v34_carrier(3, 0, &num, &den); CHECK_EQ_I(3000 * num / den, 1800);
    v34_carrier(3, 1, &num, &den); CHECK_EQ_I(3000 * num / den, 2000);
    v34_carrier(4, 0, &num, &den); CHECK_EQ_I(3200 * num / den, 1828);
    v34_carrier(4, 1, &num, &den); CHECK_EQ_I(3200 * num / den, 1920);

    memset(&ac, 0, sizeof ac);
    ac.level = 3000;
    caller_cfg(&cc);

    /* ---- clean short line ---- */
    memset(&s, 0, sizeof s);
    s.d_ca = 20; s.d_ac = 24; s.g_ca = 0.5; s.g_ac = 0.5; s.noise = 2.0;
    run(&s, &r, &ac, &cc);
    printf("  p2ans clean: done at %.2f s (peer %.2f s), rtd %d / %d, 3200 SNR %.1f dB, projected %d\n",
           r.a_done_at / 8000.0, r.c_done_at / 8000.0, (int)r.a.rtd, (int)r.c.rtd,
           r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.a.error, 0);
    CHECK(r.n < 8000 * 4);
    CHECK_EQ_I(r.a.n_info0a, 1); CHECK_EQ_I(r.c.n_info0c, 1);
    CHECK_EQ_I(r.a.retrains, 0); CHECK_EQ_I(r.a.restarts, 0);
    /* INFO0a as the caller received it: what this modem can do (Table 14) */
    CHECK_EQ_I(r.c.info0a.sr2743, 0); CHECK_EQ_I(r.c.info0a.sr2800, 0); CHECK_EQ_I(r.c.info0a.sr3429, 0);
    CHECK_EQ_I(r.c.info0a.lo3000, 1); CHECK_EQ_I(r.c.info0a.hi3000, 1);
    CHECK_EQ_I(r.c.info0a.lo3200, 1); CHECK_EQ_I(r.c.info0a.hi3200, 1);
    CHECK_EQ_I(r.c.info0a.allow3429, 0); CHECK_EQ_I(r.c.info0a.pwr_reduce, 1);
    CHECK_EQ_I(r.c.info0a.max_sr_diff, 0); CHECK_EQ_I(r.c.info0a.cme, 0); CHECK_EQ_I(r.c.info0a.c1664, 1);
    /* INFO0c as we received it */
    CHECK_EQ_I(r.a.info0c.sr3429, 1); CHECK_EQ_I(r.a.info0c.max_sr_diff, 5);
    CHECK_EQ_I(r.a.info0c.c1664, 1); CHECK_EQ_I(r.a.info0c.sr2743, 0);
    /* round-trip delay at both ends within 1 ms: ours checks the caller's
     * 40 ms turn-round, the caller's checks OURS (11.2.1.2.5: 40 +- 1 ms) */
    CHECK_NEAR(r.a.rtd, 44, 8);
    CHECK_NEAR(r.c.rtd, 44, 8);
    /* 11.2.1.2.6: Tone A for 50 ms, then the reversal (as the caller saw it) */
    CHECK_NEAR(r.c.t_rev3 - r.c.t_tone_a2, 400, 56);
    /* L1 for 160 ms at 6 dB above L2; L2 no longer than 550 ms + a round trip */
    CHECK_EQ_I(r.l1_samples, 1280);
    CHECK(r.l2_samples > 800 && r.l2_samples <= 4400 + 44);
    CHECK_NEAR(10 * log10((r.l1_pw / r.l1_samples) / (r.l2_pw / r.l2_samples)), 6.02, 0.3);
    CHECK_NEAR(sqrt(r.l2_pw / r.l2_samples), 3000, 150);       /* L2 at the nominal power */
    /* INFO1a as the caller received it (Table 16): V.34, 3200 both ways */
    CHECK_EQ_I(r.c.info1a.v90, 0);
    CHECK_EQ_I(r.c.info1a.up_sr, 4); CHECK_EQ_I(r.c.info1a.down_sr, 4);
    CHECK_EQ_I(r.c.info1a.md_len, 0);
    CHECK_EQ_I(r.c.info1a.min_pwr_red, 0); CHECK_EQ_I(r.c.info1a.add_pwr_red, 0);
    CHECK_EQ_I(r.c.info1a.preemph, 0);
    CHECK(r.c.info1a.max_rate >= 12 && r.c.info1a.max_rate <= 13);  /* 3200 symbols/s: 31200 at most */
    CHECK_NEAR(r.c.info1a.freq_off, 0, 3);
    CHECK_EQ_I(r.c.info1a.hi_carrier, r.a.rx_hi_carrier);
    clean_rate = r.c.info1a.max_rate;
    /* the outcome on our side: what INFO1c asked of our transmitter at 3200 */
    CHECK_EQ_I(r.a.sym_idx, 4); CHECK_EQ_I(r.a.sym_rate, 3200);
    CHECK_EQ_I(r.a.tx_hi_carrier, 1); CHECK_EQ_I(r.a.tx_preemph, 5); CHECK_EQ_I(r.a.tx_pwr_red, 2);
    CHECK_EQ_I(r.a.md_len, 0);
    /* probing saw the line: L2 tones at about peer level / sqrt(21), high SNR */
    for (m = 2; m <= 22; m++) {
        if (!probe_has_tone(m)) continue;
        CHECK_NEAR(r.a.rx_level[m], 4000 * 0.5 / sqrt(21.0), 4000 * 0.5 / sqrt(21.0) * 0.1);
        CHECK(r.a.snr[m] > 300);
    }
    /* signal 2000 RMS, noise 2 RMS over 4 kHz: 60 dB, 61 dB in 3200 Hz */
    CHECK_NEAR(r.a.rx_snr_db10 / 10.0, 37.0, 1.5);     /* the floor assumed for what follows the signal */

    /* ---- the symbol rate decision ---- */
    /* the caller cannot use 3200 from us: 3000, and its parameters for 3000 */
    cc.max_rate[4] = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 3000); CHECK_EQ_I(r.c.info1a.up_sr, 3); CHECK_EQ_I(r.c.info1a.down_sr, 3);
    CHECK_EQ_I(r.a.tx_hi_carrier, 0); CHECK_EQ_I(r.a.tx_preemph, 4);
    CHECK(r.c.info1a.max_rate >= 11 && r.c.info1a.max_rate <= 12);  /* 28800 at most at 3000 */
    /* only 2400 left */
    cc.max_rate[3] = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 2400); CHECK_EQ_I(r.c.info1a.up_sr, 0); CHECK_EQ_I(r.c.info1a.down_sr, 0);
    CHECK_EQ_I(r.a.tx_preemph, 1);
    CHECK_EQ_I(r.c.info1a.max_rate, 9);                             /* 21600 at most at 2400 */
    /* nothing we implement: Phase 2 fails with the reason */
    cc.max_rate[0] = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_FAILED); CHECK_EQ_I(r.a.error, P2N_ERR_NO_RATE);
    caller_cfg(&cc);
    /* our own configuration can exclude rates */
    ac.sr_mask = P2N_SR_3000;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3000);
    ac.sr_mask = 0;
    /* a ceiling on the projected rate */
    ac.max_rate = 19200;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.info1a.max_rate, 8);
    ac.max_rate = 0;
    /* the caller cannot do 1664 points: nothing above 28800 is projected */
    cc.info0c.c1664 = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.c.info1a.max_rate, 12);
    cc.info0c.c1664 = 1;
    /* a caller that cannot transmit the high carrier at 3200 is not asked to */
    cc.info0c.hi3200 = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.a.rx_hi_carrier, 0);
    cc.info0c.hi3200 = 1; cc.info0c.lo3200 = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.a.rx_hi_carrier, 1);
    caller_cfg(&cc);

    /* a line that ends at 2.9 kHz: the probing must send us down to 2400.
     * The caller projects the same throughput at every rate, so it is our
     * measurement that decides. */
    cc.max_rate[0] = cc.max_rate[3] = cc.max_rate[4] = 9;
    s.lp_hz = 2900; s.noise = 4.0;
    run(&s, &r, &ac, &cc);
    printf("  p2ans 2.9 kHz line: %d symbols/s, carrier %s, SNR %.1f dB, projected %d\n", r.a.sym_rate,
           r.a.rx_hi_carrier ? "high" : "low", r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 2400);
    CHECK_EQ_I(r.a.rx_hi_carrier, 0);       /* 400 .. 2800 Hz fits, 600 .. 3000 Hz does not */
    CHECK(r.c.info1a.max_rate >= 7);
    /* and one that ends at 3.2 kHz: 3000 symbols/s on the low carrier (300 .. 3300 Hz) */
    s.lp_hz = 3200;
    run(&s, &r, &ac, &cc);
    printf("  p2ans 3.2 kHz line: %d symbols/s, carrier %s, SNR %.1f dB, projected %d\n", r.a.sym_rate,
           r.a.rx_hi_carrier ? "high" : "low", r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 3000); CHECK_EQ_I(r.a.rx_hi_carrier, 0);
    caller_cfg(&cc);

    /* ---- long, lossy, noisy line with echo both sides ---- */
    memset(&s, 0, sizeof s);
    s.d_ca = 400; s.d_ac = 440; s.g_ca = 0.06; s.g_ac = 0.08; s.noise = 6.0;
    s.echo_a = 0.2; s.echo_c = 0.2;
    run(&s, &r, &ac, &cc);
    printf("  p2ans long line: rtd %d / %d, SNR %.1f dB, projected %d\n", (int)r.a.rtd, (int)r.c.rtd,
           r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_NEAR(r.a.rtd, 840, 8);
    CHECK_NEAR(r.c.rtd, 840, 8);
    CHECK(r.l2_samples <= 4400 + 840);
    /* signal 240 RMS, noise 6 RMS over 4 kHz: 32 dB, 33 dB in 3200 Hz */
    CHECK_NEAR(r.a.rx_snr_db10 / 10.0, 33.0, 3.0);
    CHECK(r.c.info1a.max_rate < clean_rate);
    CHECK(r.c.info1a.max_rate >= 7 && r.c.info1a.max_rate <= 10);
    /* our own sample path is slow (a driver that buffers): told so, the
     * modem still turns round in 40 ms at the line and measures the line */
    s.io_tx = 90; s.io_rx = 70; ac.io_delay = 160;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_NEAR(r.a.rtd, 840, 8);
    CHECK_NEAR(r.c.rtd, 840, 8);
    s.io_tx = s.io_rx = 0; ac.io_delay = 0;

    /* ---- recovery ---- */
    memset(&s, 0, sizeof s);
    s.d_ca = 20; s.d_ac = 24; s.g_ca = 0.5; s.g_ac = 0.5; s.noise = 2.0;
    /* INFO0c lost (11.2.2.2.1): Tone B is there, INFO0c is not */
    s.mute_ca_to = 900;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.a.info0c.sr3429, 1);
    CHECK_NEAR(r.a.rtd, 44, 8);
    CHECK(r.a.n_info0a >= 2); CHECK(r.c.n_info0c >= 2);
    /* INFO0a lost: the caller asks again */
    s.mute_ca_to = 0; s.mute_ac_to = 900;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.c.info0a.c1664, 1);
    CHECK(r.a.n_info0a >= 2); CHECK(r.c.n_info0c >= 2);
    s.mute_ac_to = 0;
    /* our first Tone A reversal lost (11.2.2.2.2): no Tone B reversal comes
     * back within 2000 ms, so again from 11.2.1.2.3 */
    {
        scen_t s2 = s; run_t probe;
        /* find when the first reversal is on the line, then repeat the call with
         * the path dead around it */
        run(&s2, &probe, &ac, &cc);
        s2.mute_ac_from = probe.c.t_rev1 - 200; s2.mute_ac_to = probe.c.t_rev1 + 400;
        run(&s2, &r, &ac, &cc);
        CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
        CHECK(r.a.restarts >= 1);
        CHECK_NEAR(r.a.rtd, 44, 8); CHECK_NEAR(r.c.rtd, 44, 8);
        /* INFO1c lost (11.2.2.2.4): 2000 ms + 2 round trips after Tone B we
         * begin a retrain (11.5.2.1): 70 ms of silence, Tone A, and Phase 2
         * again from the reversals */
        s2 = s;
        s2.mute_ca_from = probe.c_done_at - 3200; s2.mute_ca_to = probe.c_done_at + 800;
        s2.seconds = 12;
        run(&s2, &r, &ac, &cc);
        printf("  p2ans INFO1c lost: done at %.2f s after %d retrain(s)\n", r.a_done_at / 8000.0, r.a.retrains);
        CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
        CHECK_EQ_I(r.a.retrains, 1);
        CHECK_EQ_I(r.a.sym_rate, 3200);
        CHECK_NEAR(r.a.rtd, 44, 8);
    }

    /* ---- the retrain entry (11.5.2): no INFO0, straight to the tones ---- */
    s.retrain = 1;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.a.n_info0a, 0); CHECK_EQ_I(r.c.n_info0c, 0);
    CHECK_NEAR(r.a.rtd, 44, 8); CHECK_NEAR(r.c.rtd, 44, 8);
    CHECK_EQ_I(r.a.sym_rate, 3200);
    CHECK_EQ_I(r.a.info0c.c1664, 1);        /* remembered from the first Phase 2 */
    s.retrain = 0;

    /* ---- x2 (docs/history/x2-facts.md "Phase 2 marker") ---- */
    {
        scen_t keep = s;
        uint8_t b[64], body[7];
        int n, k, same;
        static run_t r0;
        /* the marker frame: lead-in, fill, sync, 7 bits LSB first, CRC */
        n = p2ans_x2_info_frame(0x4d, b);
        CHECK_EQ_I(n, 2 + 4 + 8 + 7 + 16 + 4);
        CHECK_EQ_I(b[0], 1); CHECK_EQ_I(b[1], 1);
        for (k = 0; k < 4; k++) CHECK_EQ_I(b[2 + k], 1);
        { static const uint8_t sync[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };
          for (k = 0; k < 8; k++) CHECK_EQ_I(b[6 + k], sync[k]); }
        for (k = 0; k < 7; k++) { body[k] = (uint8_t)((0x4d >> k) & 1); CHECK_EQ_I(b[14 + k], body[k]); }
        {
            uint16_t crc = 0;
            for (k = 0; k < 16; k++) crc |= (uint16_t)(b[21 + k] << k);
            CHECK_EQ_I(crc, 0x24dd);                    /* the facts' CRC of body 4d */
            CHECK_EQ_I(crc, info_crc(body, 7));
        }
        for (k = 0; k < 4; k++) CHECK_EQ_I(b[37 + k], 1);

        memset(&s, 0, sizeof s);
        s.d_ca = 20; s.d_ac = 24; s.g_ca = 0.5; s.g_ac = 0.5; s.noise = 2.0;
        /* INFO0a with x2: max_sr_diff = 4 (bit 23 of 21..23) and cme (bit 24);
         * the rest, and the call, as without */
        ac.x2 = 1; g_slot = 0;
        run(&s, &r, &ac, &cc);
        CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
        CHECK_EQ_I(r.c.info0a.max_sr_diff, 4); CHECK_EQ_I(r.c.info0a.cme, 1);
        CHECK_EQ_I(r.c.info0a.c1664, 1); CHECK_EQ_I(r.c.info0a.pwr_reduce, 1);
        CHECK_EQ_I(r.c.info0a.lo3200, 1); CHECK_EQ_I(r.c.info0a.hi3200, 1); CHECK_EQ_I(r.c.info0a.allow3429, 0);
        CHECK_EQ_I(r.a.x2_marker, -1);
        CHECK_EQ_I(r.a.sym_rate, 3200);
        {
            info0_t i0; uint8_t f[INFO_MAX_BITS];
            memset(&i0, 0, sizeof i0); i0.max_sr_diff = 4; i0.cme = 1;
            n = info0a_build(&i0, f);
            CHECK_EQ_I(f[21], 0); CHECK_EQ_I(f[22], 0); CHECK_EQ_I(f[23], 1); CHECK_EQ_I(f[24], 1);
        }
        /* ... and the same from our L1 on as without x2 */
        ac.x2 = 0; g_slot = 1;
        run(&s, &r0, &ac, &cc);
        CHECK_EQ_I(r0.a.state, P2N_DONE); CHECK_EQ_I(r0.c.info0a.max_sr_diff, 0); CHECK_EQ_I(r0.c.info0a.cme, 0);
        CHECK_EQ_I(r0.ntx, r.ntx);
        same = r0.ntx == r.ntx;
        for (k = 0; k < r.ntx && k < r0.ntx; k++) if (txlog[0][k] != txlog[1][k]) same = 0;
        CHECK(same);

        /* the marker 4d 170 ms after our L1 began: x2 */
        ac.x2 = 1; g_slot = 0;
        s.mk_at = 170; s.mk_body = 0x4d;
        run(&s, &r, &ac, &cc);
        printf("  p2ans x2 marker: P2N_X2 %.0f ms after L1 began, marker %02x\n",
               (r.x2_at - r.l1_at) / 8.0, (unsigned)r.a.x2_marker);
        CHECK_EQ_I(r.a.state, P2N_X2); CHECK_EQ_I(r.a.x2_marker, 0x4d);
        CHECK(r.x2_at - r.l1_at >= 8 * (170 + 60) && r.x2_at - r.l1_at <= 8 * (170 + 90));
        CHECK(r.x2_quiet);                              /* silent until the client's J */
        CHECK_EQ_I(r.a.rtd, r0.a.rtd);
        /* the latest the facts saw (260 ms) */
        s.mk_at = 260;
        run(&s, &r, &ac, &cc);
        CHECK_EQ_I(r.a.state, P2N_X2); CHECK_EQ_I(r.a.x2_marker, 0x4d);
        /* bits 1..3 = 6 is what counts (0d: 6, and symbol rate index 0) */
        s.mk_at = 170; s.mk_body = 0x0d;
        run(&s, &r, &ac, &cc);
        CHECK_EQ_I(r.a.state, P2N_X2); CHECK_EQ_I(r.a.x2_marker, 0x0d);
        /* without x2 offered the same marker is nothing */
        ac.x2 = 0; s.mk_body = 0x4d;
        run(&s, &r, &ac, &cc);
        CHECK(r.a.state != P2N_X2); CHECK_EQ_I(r.a.x2_marker, -1);

        /* refused: a bad CRC, the marker 900 ms after the end of L1, a body
         * with another index in bits 1..3 (49: 4, plain V.34; 41: 0). Each
         * leaves the V.34 path as it is without x2: the same samples. */
        {
            static const struct { int at, body, bad; const char *what; } no[] = {
                { 170, 0x4d, 1, "bad CRC" }, { 160 + 900, 0x4d, 0, "late" },
                { 170, 0x49, 0, "index 4" }, { 170, 0x41, 0, "index 0" },
                { 170, 0x4d, 2, "fill bit 0" } };
            int j;
            for (j = 0; j < 5; j++) {
                s.mk_at = no[j].at; s.mk_body = no[j].body; s.mk_bad = no[j].bad & 1; s.mk_nofill = no[j].bad >> 1; s.seconds = 4;
                ac.x2 = 1; g_slot = 0; run(&s, &r, &ac, &cc);
                ac.x2 = 0; g_slot = 1; run(&s, &r0, &ac, &cc);
                same = r0.ntx == r.ntx;
                for (k = 0; k < r.ntx && k < r0.ntx; k++) if (txlog[0][k] != txlog[1][k]) same = 0;
                printf("  p2ans x2 marker refused (%s): state %d, %s\n", no[j].what, (int)r.a.state,
                       same ? "the same samples as without x2" : "SAMPLES DIFFER");
                CHECK(r.a.state != P2N_X2); CHECK_EQ_I(r.a.x2_marker, -1);
                CHECK_EQ_I(r.a.state, r0.a.state);
                CHECK(same);
            }
        }
        /* the 800 ms after the end of L1 count to the start of the marker
         * frame (its lead-in), not to its end: one that begins just inside
         * them is taken though it ends some 70 ms after them, one that begins
         * just after them is not (the frame reaches us 7 ms after it is sent) */
        s.mk_bad = 0; s.mk_nofill = 0; s.mk_body = 0x4d; s.seconds = 4; ac.x2 = 1; g_slot = 0;
        s.mk_at = 160 + 800 - 15;
        run(&s, &r, &ac, &cc);
        printf("  p2ans x2 marker sent %d ms after L1 began: state %d\n", s.mk_at, (int)r.a.state);
        CHECK_EQ_I(r.a.state, P2N_X2); CHECK_EQ_I(r.a.x2_marker, 0x4d);
        s.mk_at = 160 + 800 + 5;
        run(&s, &r, &ac, &cc);
        printf("  p2ans x2 marker sent %d ms after L1 began: state %d\n", s.mk_at, (int)r.a.state);
        CHECK(r.a.state != P2N_X2); CHECK_EQ_I(r.a.x2_marker, -1);
        /* only in the first Phase 2 of a call: in a retrain's (11.5.2) the
         * same marker is nothing, and the samples are as without x2 */
        s.mk_at = 170; s.retrain = 1;
        ac.x2 = 1; g_slot = 0; run(&s, &r, &ac, &cc);
        ac.x2 = 0; g_slot = 1; run(&s, &r0, &ac, &cc);
        same = r0.ntx == r.ntx;
        for (k = 0; k < r.ntx && k < r0.ntx; k++) if (txlog[0][k] != txlog[1][k]) same = 0;
        printf("  p2ans x2 marker in a retrain: state %d, %s\n", (int)r.a.state,
               same ? "the same samples as without x2" : "SAMPLES DIFFER");
        CHECK(r.l1_at > 0);
        CHECK(r.a.state != P2N_X2); CHECK_EQ_I(r.a.x2_marker, -1);
        CHECK_EQ_I(r.a.state, r0.a.state);
        CHECK(same);
        s = keep;
        ac.x2 = 0; g_slot = 0;
    }

    /* ---- K56flex (k56 design, decision 6; k56-facts.md "Phase 2") ---- */
    {
        scen_t keep = s;
        static run_t r0;
        uint8_t b[INFO_MAX_BITS];
        int n, k, hit_pos = 0, hit_neg = 0;
        /* the offset nudge: a measured offset ending in binary 01 (which
         * the client reads as K56flex) moves one step toward zero */
        CHECK_EQ_I(p2ans_k56_nudge(5), 4); CHECK_EQ_I(p2ans_k56_nudge(-3), -2);
        CHECK_EQ_I(p2ans_k56_nudge(1), 0); CHECK_EQ_I(p2ans_k56_nudge(-7), -6);
        CHECK_EQ_I(p2ans_k56_nudge(-511), -510); CHECK_EQ_I(p2ans_k56_nudge(509), 508);
        {
            static const int same[] = { 0, 2, 3, 4, 6, 7, -1, -2, -4, -5, -6, 511, -512, 510 };
            for (k = 0; k < (int)(sizeof same / sizeof same[0]); k++) CHECK_EQ_I(p2ans_k56_nudge(same[k]), same[k]);
        }

        memset(&s, 0, sizeof s);
        s.d_ca = 20; s.d_ac = 24; s.g_ca = 0.5; s.g_ac = 0.5; s.noise = 2.0;
        caller_cfg(&cc);

        /* an agreed client that sends INFO1c 0x001, 3200 selected: INFO1a
         * bits 40:49 are 0x001; the rest of INFO1a and its timing are as
         * without K56flex; after its last bit P2N_K56, and silence */
        cc.k56 = P2C_K56_ACCEPT;
        ac.k56 = 0; g_slot = 1;
        run(&s, &r0, &ac, &cc);
        ac.k56 = 1; g_slot = 0;
        run(&s, &r, &ac, &cc);
        printf("  p2ans K56flex flag: state %d, INFO1a offset %d, P2N_K56 at %.4f s (P2N_DONE without: %.4f s), %s\n",
               (int)r.a.state, r.c.info1a.freq_off, r.k56_at / 8000.0, r0.a_done_at / 8000.0,
               r.k56_quiet ? "silent from then" : "NOT SILENT");
        CHECK_EQ_I(r.a.state, P2N_K56); CHECK_EQ_I(r.a.k56, 1); CHECK_EQ_I(r.a.error, 0);
        CHECK_EQ_I(r.c.state, P2C_DONE);
        CHECK_EQ_I(r.a.info1c.freq_off, 1);
        CHECK_EQ_I(r.a.info1a.freq_off, 1); CHECK_EQ_I(r.c.info1a.freq_off, 1);
        n = info1a_build(&r.a.info1a, b);
        CHECK_EQ_I(n, INFO1A_BITS);
        CHECK_EQ_I(b[40], 1);
        for (k = 41; k <= 49; k++) CHECK_EQ_I(b[k], 0);
        CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.c.info1a.up_sr, 4); CHECK_EQ_I(r.c.info1a.down_sr, 4);
        CHECK_EQ_I(r0.a.state, P2N_DONE); CHECK_EQ_I(r0.a.k56, 0);
        CHECK(same_info1a(&r.c.info1a, &r0.c.info1a));
        CHECK_EQ_I(r.k56_at, r0.a_done_at);
        CHECK(r.k56_quiet); CHECK(r.n >= r.k56_at + 8000);
        CHECK_EQ_I(r.a_done_at, -1);
        /* the samples up to INFO1a's offset field are those of the call without */
        {
            int same = 1;
            for (k = 0; k < r.ntx && k < r0.ntx && k < r0.a_done_at - r0.l1_at - 2000; k++)
                if (txlog[0][k] != txlog[1][k]) same = 0;
            CHECK(same);
        }
        /* ... and in the first Phase 2 of a retrain (11.5.2) */
        s.retrain = 1;
        run(&s, &r, &ac, &cc);
        CHECK_EQ_I(r.a.state, P2N_K56); CHECK_EQ_I(r.a.k56, 1); CHECK_EQ_I(r.c.info1a.freq_off, 1);
        s.retrain = 0;

        /* an agreed client whose check failed: INFO1c 0x200. INFO1a as
         * without K56flex, but for a measured offset ending in 01, moved one
         * step toward zero. Over a few frequency offsets of the line. */
        cc.k56 = P2C_K56_DECLINE;
        {
            static const double FO[] = { 0.0, 0.10, -0.06, 0.02, 0.08, -0.04, -0.14, 0.18 };
            int j;
            for (j = 0; j < (int)(sizeof FO / sizeof FO[0]); j++) {
                int meas;
                s.foff = FO[j];
                ac.k56 = 0; g_slot = 1; run(&s, &r0, &ac, &cc);
                ac.k56 = 1; g_slot = 0; run(&s, &r, &ac, &cc);
                meas = r0.c.info1a.freq_off;
                printf("  p2ans K56flex, INFO1c 0x200, line offset %+.2f Hz: measured %d, INFO1a %d (%s)\n",
                       FO[j], meas, r.c.info1a.freq_off, same_txlog(&r, &r0) ? "the same samples" : "samples differ");
                CHECK_EQ_I(r0.a.state, P2N_DONE); CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.k56, 0);
                CHECK_EQ_I(r.a.info1c.freq_off, -512);
                CHECK_EQ_I(meas, proberx_freq_off(&r0.a.prx));
                CHECK(same_info1a(&r.c.info1a, &r0.c.info1a));
                CHECK_EQ_I(r.c.info1a.freq_off, p2ans_k56_nudge(meas));
                if (((unsigned)meas & 3) == 1) {
                    CHECK(r.c.info1a.freq_off != meas);
                    if (meas > 0) hit_pos++; else hit_neg++;
                } else {
                    CHECK_EQ_I(r.c.info1a.freq_off, meas);
                    CHECK(same_txlog(&r, &r0));
                }
            }
            /* the sweep met the nudge both ways */
            CHECK(hit_pos > 0); CHECK(hit_neg > 0);
            s.foff = 0;
        }

        /* INFO1c 0x001, but 3200 is not selected: unflagged, as the case
         * before. The client cannot transmit at 3200 (INFO0c), or we do not
         * allow 3200. On a line 0.02 Hz off, measured as 1: nudged to 0. */
        cc.k56 = P2C_K56_ACCEPT; s.foff = 0.02;
        for (k = 0; k < 2; k++) {
            if (k == 0) { cc.info0c.lo3200 = cc.info0c.hi3200 = 0; }
            else { caller_cfg(&cc); cc.k56 = P2C_K56_ACCEPT; ac.sr_mask = P2N_SR_2400 | P2N_SR_3000; }
            ac.k56 = 0; run(&s, &r0, &ac, &cc);
            ac.k56 = 1; run(&s, &r, &ac, &cc);
            printf("  p2ans K56flex, INFO1c 0x001, %s: %d symbols/s, state %d, INFO1a offset %d (measured %d)\n",
                   k ? "3200 not allowed by us" : "the client cannot send 3200", r.a.sym_rate, (int)r.a.state,
                   r.c.info1a.freq_off, r0.c.info1a.freq_off);
            CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.k56, 0);
            CHECK_EQ_I(r.a.sym_rate, 3000); CHECK_EQ_I(r.c.info1a.up_sr, 3);
            CHECK_EQ_I(r.a.info1c.freq_off, 1);
            CHECK(same_info1a(&r.c.info1a, &r0.c.info1a));
            CHECK_EQ_I(r0.c.info1a.freq_off, 1); CHECK_EQ_I(r.c.info1a.freq_off, 0);
        }
        ac.sr_mask = 0; s.foff = 0;
        caller_cfg(&cc);

        /* without cfg.k56 an INFO1c of 0x001 is nothing: every sample as
         * with the caller's measured offset in it */
        ac.k56 = 0;
        cc.k56 = P2C_K56_ACCEPT; g_slot = 0; run(&s, &r, &ac, &cc);
        cc.k56 = 0;              g_slot = 1; run(&s, &r0, &ac, &cc);
        CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.k56, 0);
        CHECK_EQ_I(r.a.info1c.freq_off, 1);
        CHECK_EQ_I(r.c.info1a.freq_off, proberx_freq_off(&r.a.prx));
        CHECK(same_txlog(&r, &r0));
        CHECK_EQ_I(r.a_done_at, r0.a_done_at);
        g_slot = 0;
        s = keep;
    }

    /* ---- dead line: gives up with a reason ---- */
    s.g_ca = 0.0; s.seconds = 30;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_FAILED);
    CHECK_EQ_I(r.a.error, P2N_ERR_INFO0);
    CHECK(r.n < 8000 * 12);

    /* ---- bounds (spec D5): a caller that repeats INFO0c ---- */
    memset(&s, 0, sizeof s);
    s.d_ca = 20; s.d_ac = 24; s.g_ca = 0.5; s.g_ac = 0.5; s.noise = 2.0;
    caller_cfg(&cc);
    /* for ever, never answering our Tone A reversal (WAIT_BREV1 -> INFO0
     * each time): the exchange clock, 20 s from the start */
    cc.info0_repeat = -1; s.seconds = 60;
    run(&s, &r, &ac, &cc);
    printf("  p2ans INFO0c for ever: state %d error %d at %.2f s, INFO0c %d\n",
           (int)r.a.state, r.a.error, r.n / 8000.0, r.c.n_info0c);
    CHECK_EQ_I(r.a.state, P2N_FAILED); CHECK_EQ_I(r.a.error, P2N_ERR_TIME);
    CHECK(r.n >= 8 * P2N_EXCHANGE_MS); CHECK(r.n <= 8 * (20000 + 100));
    CHECK(r.c.n_info0c > 20);       /* the loop really ran */
    /* a few repeats are no fault: Phase 2 completes */
    cc.info0_repeat = 3; s.seconds = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.c.n_info0c, 4);
    caller_cfg(&cc);

    T_REPORT("p2ans");
}
