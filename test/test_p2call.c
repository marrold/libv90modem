/* V.34 Phase 2, call modem (modem/p2call) against the library's answer
 * modem (modem/p2ans) over a delayed, attenuated, noisy, echoing,
 * band-limited path: the INFO exchange, the timings 11.2.1.1 gives, the
 * INFO1c decision (Table 15) on lines of different bandwidth, noise, slope
 * and level, the recovery procedures of 11.2.2.1 and the retrain entry.
 * Also INFO1c's builder against Table 15, and p2call_decide on made-up
 * probe results where INFO0a or our configuration limits what INFO1c may
 * say. There is no independent V.34 answer modem to test against (spandsp
 * has no V.34): the answerer is the library's, itself checked against the
 * test peer sim/p2c (test_p2ans). */
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/p2call.h"
#include "../modem/p2ans.h"
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
    double lp_hz;           /* low-pass on both paths, 0 = none */
    const double *loop; int nloop;  /* a loop filter on both paths (0 = flat) */
    int mute_ca_from, mute_ca_to;   /* caller -> answer path dead in this window (samples) */
    int mute_ac_from, mute_ac_to;
    int io_tx, io_rx;       /* the CALLER's own sample path: delay to the line terminals and back */
    int retrain;            /* both ends start in the retrain entry */
    int ans_back_ms;        /* the answerer, done, comes back with a retrain of its own after this */
    int ans_answers;        /* the answerer, done, answers our Tone B as a retrain (11.5.2.2) */
    int ans_reinit;         /* the answerer starts over each time it leaves P2N_INFO0 */
    int seconds;
} scen_t;

typedef struct {
    p2ans_t a; p2call_t c;
    int n, a_done_at, c_done_at;
    int l1_samples, l2_samples;
    double l1_pw, l2_pw;
    int a_restarts;         /* times the harness played the answerer's side of a retrain */
    int tone_at;            /* when the caller ended its reading of L2 (Tone B: 11.2.1.1.5) */
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

/* one direction of the line: delay d, then the filters, from the history b */
static double path(const scen_t *s, const double *b, int i, int d)
{
    double x = 0;
    int k, j;
    if (s->lp_hz > 0) {
        for (k = 0; k < NLP; k++) {
            double v = 0;
            if (s->nloop > 0) for (j = 0; j < s->nloop; j++) v += s->loop[j] * b[(i - d - k - j + 4 * MAXD) % MAXD];
            else v = b[(i - d - k + 4 * MAXD) % MAXD];
            x += lp[k] * v;
        }
        return x;
    }
    if (s->nloop > 0) { for (j = 0; j < s->nloop; j++) x += s->loop[j] * b[(i - d - j + 4 * MAXD) % MAXD]; return x; }
    return b[(i - d + 4 * MAXD) % MAXD];
}

static int16_t clip16(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : floor(v + 0.5)); }

static void run(const scen_t *s, run_t *r, const p2ans_cfg *ac, const p2call_cfg *cc)
{
    static double lca[MAXD], lac[MAXD]; static int16_t ltx[MAXD], lrx[MAXD];
    rng_t rng;
    int16_t atx = 0, ctx = 0;
    int i, secs = s->seconds ? s->seconds : 8;
    int a_done = -1, tone_b = 0;
    info0_t i0c;

    memset(lca, 0, sizeof lca); memset(lac, 0, sizeof lac);
    memset(ltx, 0, sizeof ltx); memset(lrx, 0, sizeof lrx);
    if (s->lp_hz > 0) lp_design(s->lp_hz);
    memset(r, 0, sizeof *r);
    /* what our INFO0c says (11.5: the answerer keeps it from the first Phase 2) */
    memset(&i0c, 0, sizeof i0c);
    i0c.lo3000 = i0c.hi3000 = i0c.lo3200 = i0c.hi3200 = 1; i0c.pwr_reduce = 1; i0c.c1664 = 1;
    if (s->retrain) {
        info0_t i0a;
        memset(&i0a, 0, sizeof i0a);
        i0a.lo3000 = i0a.hi3000 = i0a.lo3200 = i0a.hi3200 = 1; i0a.pwr_reduce = 1; i0a.c1664 = 1;
        p2ans_init_retrain(&r->a, ac, &i0c);
        p2call_init_retrain(&r->c, cc, &i0a);
    } else {
        p2ans_init(&r->a, ac);
        p2call_init(&r->c, cc);
    }
    rng_seed(&rng, 2024);
    r->a_done_at = r->c_done_at = -1;
    for (i = 0; i < 8000 * secs; i++) {
        double xa, xc, line_c;
        /* the caller's transmit sample reaches its line terminals io_tx samples late */
        ltx[i % MAXD] = ctx;
        line_c = ltx[(i - s->io_tx + MAXD) % MAXD];
        lca[i % MAXD] = line_c; lac[i % MAXD] = atx;
        xa = s->g_ca * path(s, lca, i, s->d_ca);
        xc = s->g_ac * path(s, lac, i, s->d_ac);
        if (i >= s->mute_ca_from && i < s->mute_ca_to) xa = 0;
        if (i >= s->mute_ac_from && i < s->mute_ac_to) xc = 0;
        xa += s->echo_a * atx + s->noise * rng_gauss(&rng);
        xc += s->echo_c * line_c + s->noise * rng_gauss(&rng);
        /* what the caller's line terminals receive reaches it io_rx samples late */
        lrx[i % MAXD] = clip16(xc);
        if (r->c.state == P2L_L1) { r->l1_samples++; r->l1_pw += (double)ctx * ctx; }
        if (r->c.state == P2L_L2) { r->l2_samples++; r->l2_pw += (double)ctx * ctx; }
        atx = p2ans_tick(&r->a, clip16(xa));
        if (s->ans_reinit && r->a.state != P2N_INFO0) { p2ans_init(&r->a, ac); r->a_restarts++; }
        ctx = p2call_tick(&r->c, lrx[(i - s->io_rx + MAXD) % MAXD]);
        if (r->a.state == P2N_DONE && r->a_done_at < 0) r->a_done_at = i;
        if (r->c.state == P2L_DONE && r->c_done_at < 0) r->c_done_at = i;
        if (r->c.state == P2L_WAIT_REV3 && !r->tone_at) r->tone_at = i;
        /* the answerer's side of a retrain after its Phase 2 was over: its
         * Phase 3 gave up by itself (Tone A after 70 ms of silence), or it
         * heard our Tone B for 50 ms (11.5.2.2: 70 ms of silence, Tone A) */
        if (r->a.state == P2N_DONE && r->c.state != P2L_DONE && r->a_restarts < 2) {
            if (a_done < 0) a_done = i;
            if (s->ans_back_ms && i - a_done == 8 * s->ans_back_ms) {
                p2ans_init_retrain(&r->a, ac, &i0c); r->a_restarts++; a_done = -1;
            }
            if (s->ans_answers) {
                tone_b = r->c.state == P2L_WAIT_REV1 ? tone_b + 1 : 0;
                if (tone_b == 8 * (50 + 70)) { p2ans_init_retrain(&r->a, ac, &i0c); r->a_restarts++; a_done = -1; }
            }
        }
        if (r->c.state == P2L_FAILED) break;
        if (r->a.state == P2N_DONE && r->c.state == P2L_DONE) break;
    }
    r->n = i;
}

static void ans_cfg(p2ans_cfg *ac) { memset(ac, 0, sizeof *ac); ac->level = 3000; }
static void call_cfg(p2call_cfg *cc) { memset(cc, 0, sizeof *cc); cc->level = 3000; }
static void clean(scen_t *s)
{
    memset(s, 0, sizeof *s);
    s->d_ca = 20; s->d_ac = 24; s->g_ca = 0.5; s->g_ac = 0.5; s->noise = 2.0;
}

/* made-up probe results: every tone at the same level and SNR */
static void flat(p2call_t *p, int snr)
{
    int m;
    for (m = 1; m <= PROBE_NBIN; m++) {
        p->snr[m] = probe_has_tone(m) ? snr : 0;
        p->rx_level[m] = probe_has_tone(m) ? 300 : 0;
    }
    p->sdr_db10 = 600;
}
static void full_info0a(info0_t *i)
{
    memset(i, 0, sizeof *i);
    i->lo3000 = i->hi3000 = i->lo3200 = i->hi3200 = 1; i->pwr_reduce = 1; i->c1664 = 1;
}

static const double LOOP_LONG[3] = { 0.25, 0.6, 0.25 };     /* -5 dB at 1.9 kHz, -13 dB at 3 kHz */

int main(void)
{
    static run_t r;
    static p2call_t u;
    p2ans_cfg ac; p2call_cfg cc;
    scen_t s;
    int k, m, clean4, clean0;

    /* ---- INFO1c (Table 15): the bits where the table puts them ---- */
    {
        info1c_t i, o;
        uint8_t bits[INFO_MAX_BITS];
        int n;
        memset(&i, 0, sizeof i);
        i.min_pwr_red = 5; i.add_pwr_red = 2; i.md_len = 77; i.freq_off = -37;
        for (k = 0; k < 6; k++) { i.sr[k].hi_carrier = (uint8_t)(k & 1); i.sr[k].preemph = (uint8_t)(k + 3); i.sr[k].max_rate = (uint8_t)(14 - k); }
        n = info1c_build(&i, bits);
        CHECK_EQ_I(n, INFO1C_BITS);
        CHECK_EQ_I(bits[12] | bits[13] << 1 | bits[14] << 2, 5);           /* 12:14 */
        CHECK_EQ_I(bits[15] | bits[16] << 1 | bits[17] << 2, 2);           /* 15:17 */
        for (m = 0, k = 0; k < 7; k++) m |= bits[18 + k] << k;
        CHECK_EQ_I(m, 77);                                                  /* 18:24 */
        CHECK_EQ_I(bits[25], 0);                                            /* 2400: 25, 26:29, 30:33 */
        CHECK_EQ_I(bits[26] | bits[27] << 1 | bits[28] << 2 | bits[29] << 3, 3);
        CHECK_EQ_I(bits[30] | bits[31] << 1 | bits[32] << 2 | bits[33] << 3, 14);
        CHECK_EQ_I(bits[61], 0);                                            /* 3200: 61:69 */
        CHECK_EQ_I(bits[62] | bits[63] << 1 | bits[64] << 2 | bits[65] << 3, 7);
        CHECK_EQ_I(bits[66] | bits[67] << 1 | bits[68] << 2 | bits[69] << 3, 10);
        CHECK_EQ_I(bits[70], 1);                                            /* 3429: 70:78 */
        for (m = 0, k = 0; k < 10; k++) m |= bits[79 + k] << k;
        CHECK_EQ_I(m, 1024 - 37);                                           /* 79:88, two's complement */
        CHECK(info_check(bits + INFO_HDR_BITS, INFO_PAYLOAD(INFO1C_BITS)));
        info1c_parse(bits + INFO_HDR_BITS, &o);
        CHECK_EQ_I(o.min_pwr_red, 5); CHECK_EQ_I(o.add_pwr_red, 2); CHECK_EQ_I(o.md_len, 77); CHECK_EQ_I(o.freq_off, -37);
        for (k = 0; k < 6; k++) { CHECK_EQ_I(o.sr[k].hi_carrier, k & 1); CHECK_EQ_I(o.sr[k].preemph, k + 3); CHECK_EQ_I(o.sr[k].max_rate, 14 - k); }
        /* the answer modem reads it as before */
        {
            /* both cleared first: memcmp also compares the padding (one
             * octet before freq_off), which the parsers do not write */
            info1d_t d, c;
            memset(&d, 0, sizeof d); memset(&c, 0, sizeof c);
            info1d_parse(bits + INFO_HDR_BITS, &d);
            info1c_parse(bits + INFO_HDR_BITS, &c);
            CHECK(memcmp(&d, &c, sizeof d) == 0);
        }
    }

    /* ---- the INFO1c decision on made-up probe results ---- */
    call_cfg(&cc);
    p2call_init(&u, &cc); full_info0a(&u.info0a); flat(&u, 500);
    p2call_decide(&u);
    /* 50 dB everywhere: the floor of what follows the signal (36 dB) decides;
     * the top of Table 8 at 2400, 26400 at 3000, 28800 at 3200 */
    CHECK_EQ_I(u.info1c.sr[0].max_rate, 9); CHECK_EQ_I(u.info1c.sr[3].max_rate, 11); CHECK_EQ_I(u.info1c.sr[4].max_rate, 12);
    CHECK_EQ_I(u.info1c.sr[1].max_rate, 0); CHECK_EQ_I(u.info1c.sr[2].max_rate, 0); CHECK_EQ_I(u.info1c.sr[5].max_rate, 0);
    for (k = 0; k < 6; k++) CHECK_EQ_I(u.info1c.sr[k].preemph, 0);
    CHECK_EQ_I(u.info1c.min_pwr_red, 0); CHECK_EQ_I(u.info1c.md_len, 0);
    CHECK(u.info1c.add_pwr_red >= 1);       /* SNR to spare at the top rates */
    /* less SNR, lower rates */
    flat(&u, 250); p2call_decide(&u);
    CHECK(u.info1c.sr[4].max_rate >= 3 && u.info1c.sr[4].max_rate <= 5);
    CHECK(u.info1c.sr[0].max_rate < 9);
    CHECK(u.info1c.add_pwr_red <= 2);
    flat(&u, 150); p2call_decide(&u);
    CHECK(u.info1c.sr[4].max_rate >= 1 && u.info1c.sr[4].max_rate <= 3);
    /* the answerer cannot do 1664 points: nothing over 28800 (Table 15 NOTE) */
    flat(&u, 500); u.info0a.c1664 = 0; p2call_decide(&u);
    CHECK(u.info1c.sr[4].max_rate <= 12);
    /* nor transmit at 3200 at all; then on the low carrier only, the high only */
    full_info0a(&u.info0a); u.info0a.lo3200 = u.info0a.hi3200 = 0; p2call_decide(&u);
    CHECK_EQ_I(u.info1c.sr[4].max_rate, 0); CHECK_EQ_I(u.info1c.sr[3].max_rate, 11);
    full_info0a(&u.info0a); u.info0a.hi3200 = 0; p2call_decide(&u);
    CHECK_EQ_I(u.info1c.sr[4].hi_carrier, 0); CHECK(u.info1c.sr[4].max_rate > 0);
    full_info0a(&u.info0a); u.info0a.lo3200 = 0; u.info0a.lo3000 = 0; p2call_decide(&u);
    CHECK_EQ_I(u.info1c.sr[4].hi_carrier, 1); CHECK_EQ_I(u.info1c.sr[3].hi_carrier, 1);
    /* our own configuration: symbol rates, a ceiling */
    full_info0a(&u.info0a);
    u.cfg.sr_mask = P2N_SR_2400; p2call_decide(&u);
    CHECK_EQ_I(u.info1c.sr[0].max_rate, 9); CHECK_EQ_I(u.info1c.sr[3].max_rate, 0); CHECK_EQ_I(u.info1c.sr[4].max_rate, 0);
    u.cfg.sr_mask = 0; u.cfg.max_rate = 19200; p2call_decide(&u);
    for (k = 0; k < 6; k++) CHECK(u.info1c.sr[k].max_rate <= 8);
    u.cfg.max_rate = 0;
    /* a line that falls 12 dB from 300 to 3300 Hz in level (and SNR): pre-emphasis */
    for (m = 1; m <= PROBE_NBIN; m++) if (probe_has_tone(m)) {
        int dl = (150 * m - 300) * 120 / 3000;
        u.snr[m] = 400 - dl; u.rx_level[m] = (int)(300 * pow(10, -dl / 200.0));
    }
    p2call_decide(&u);
    printf("  falling line: pre-emphasis %d / %d / %d at 2400 / 3000 / 3200, rates %d %d %d\n",
           u.info1c.sr[0].preemph, u.info1c.sr[3].preemph, u.info1c.sr[4].preemph,
           u.info1c.sr[0].max_rate, u.info1c.sr[3].max_rate, u.info1c.sr[4].max_rate);
    CHECK(u.info1c.sr[4].preemph >= 2 && u.info1c.sr[4].preemph <= 10);
    CHECK(u.info1c.sr[3].preemph >= 2 && u.info1c.sr[3].preemph <= 10);
    /* strong distortion in the gaps: ask for less power, unless the answerer cannot reduce it */
    flat(&u, 400); u.sdr_db10 = 220; p2call_decide(&u);
    printf("  distortion 22 dB down: power reduction %d (+ %d), rate at 3200 %d\n",
           u.info1c.min_pwr_red, u.info1c.add_pwr_red, u.info1c.sr[4].max_rate);
    CHECK(u.info1c.min_pwr_red >= 2 && u.info1c.min_pwr_red <= 7);
    u.info0a.pwr_reduce = 0; p2call_decide(&u);
    CHECK_EQ_I(u.info1c.min_pwr_red, 0); CHECK_EQ_I(u.info1c.add_pwr_red, 0);
    full_info0a(&u.info0a);
    /* a G.711 codec's share (36 dB) is no reason to reduce power */
    flat(&u, 500); u.sdr_db10 = 370; p2call_decide(&u);
    CHECK_EQ_I(u.info1c.min_pwr_red, 0);

    ans_cfg(&ac); call_cfg(&cc);

    /* ---- clean short line ---- */
    clean(&s);
    run(&s, &r, &ac, &cc);
    printf("  p2call clean: done at %.2f s (answerer %.2f s), rtd %d / %d, INFO1c %d %d %d (3200 SNR %.1f dB), answerer chose %d\n",
           r.c_done_at / 8000.0, r.a_done_at / 8000.0, (int)r.c.rtd, (int)r.a.rtd,
           r.c.info1c.sr[0].max_rate, r.c.info1c.sr[3].max_rate, r.c.info1c.sr[4].max_rate, r.c.band_snr[4] / 10.0, r.a.sym_rate);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.c.error, 0);
    CHECK(r.n < 8000 * 4);
    /* 11.2.1.1.5 lets the call modem read L2 for up to 500 ms; we read it
     * for no more than 300 ms from its start (the answerer's L2 began 10 ms
     * before probe_from): a real modem whose tone came 92 to 103 ms after
     * ours arrived ignored the reversal when we listened 460 ms (reported by
     * an independent softmodem's live calls) */
    printf("  p2call clean: Tone B %d ms after the start of the answerer's L2\n", (r.tone_at - (r.c.probe_from - 80)) / 8);
    CHECK(r.tone_at - (r.c.probe_from - 80) <= 8 * 300);
    CHECK_EQ_I(r.c.n_info0c, 1); CHECK_EQ_I(r.a.n_info0a, 1);
    CHECK_EQ_I(r.c.retrains, 0); CHECK_EQ_I(r.c.restarts, 0); CHECK_EQ_I(r.c.rev3_timeouts, 0);
    CHECK_EQ_I(r.a.retrains, 0); CHECK_EQ_I(r.a.restarts, 0);
    /* INFO0c as the answerer received it: what this modem can do (Table 14) */
    CHECK_EQ_I(r.a.info0c.sr2743, 0); CHECK_EQ_I(r.a.info0c.sr2800, 0); CHECK_EQ_I(r.a.info0c.sr3429, 0);
    CHECK_EQ_I(r.a.info0c.lo3000, 1); CHECK_EQ_I(r.a.info0c.hi3000, 1);
    CHECK_EQ_I(r.a.info0c.lo3200, 1); CHECK_EQ_I(r.a.info0c.hi3200, 1);
    CHECK_EQ_I(r.a.info0c.allow3429, 0); CHECK_EQ_I(r.a.info0c.pwr_reduce, 1);
    CHECK_EQ_I(r.a.info0c.max_sr_diff, 0); CHECK_EQ_I(r.a.info0c.cme, 0); CHECK_EQ_I(r.a.info0c.c1664, 1);
    /* INFO0a as we received it */
    CHECK_EQ_I(r.c.info0a.lo3200, 1); CHECK_EQ_I(r.c.info0a.hi3200, 1); CHECK_EQ_I(r.c.info0a.c1664, 1);
    CHECK_EQ_I(r.c.info0a.pwr_reduce, 1); CHECK_EQ_I(r.c.info0a.sr3429, 0);
    /* round trip at both ends within 1 ms: each checks the other's 40 ms turn-round */
    CHECK_NEAR(r.c.rtd, 44, 8);
    CHECK_NEAR(r.a.rtd, 44, 8);
    /* our L1 for 160 ms at 6 dB above L2; L2 no longer than 550 ms + a round trip */
    CHECK_NEAR(r.l1_samples, 1280, 2);
    CHECK(r.l2_samples > 800 && r.l2_samples <= 4400 + 44);
    CHECK_NEAR(10 * log10((r.l1_pw / r.l1_samples) / (r.l2_pw / r.l2_samples)), 6.02, 0.3);
    CHECK_NEAR(sqrt(r.l2_pw / r.l2_samples), 3000, 150);
    /* INFO1c as the answerer received it (Table 15) */
    CHECK_EQ_I(r.a.info1c.sr[1].max_rate, 0); CHECK_EQ_I(r.a.info1c.sr[2].max_rate, 0); CHECK_EQ_I(r.a.info1c.sr[5].max_rate, 0);
    CHECK(r.a.info1c.sr[4].max_rate >= 11 && r.a.info1c.sr[4].max_rate <= 13);
    CHECK(r.a.info1c.sr[3].max_rate >= 10 && r.a.info1c.sr[3].max_rate <= 12);
    CHECK(r.a.info1c.sr[0].max_rate >= 8 && r.a.info1c.sr[0].max_rate <= 9);
    for (k = 0; k < 6; k++) CHECK_EQ_I(r.a.info1c.sr[k].preemph, 0);
    CHECK_EQ_I(r.a.info1c.min_pwr_red, 0); CHECK_EQ_I(r.a.info1c.md_len, 0);
    CHECK_NEAR(r.a.info1c.freq_off, 0, 3);
    CHECK(memcmp(&r.a.info1c, &r.c.info1c, sizeof r.c.info1c) == 0);
    clean4 = r.c.info1c.sr[4].max_rate; clean0 = r.c.info1c.sr[0].max_rate;
    /* the outcome: one symbol rate, each receiver's carrier as it asked */
    CHECK_EQ_I(r.c.sym_rate, 3200); CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.c.sym_idx, 4);
    CHECK_EQ_I(r.c.tx_hi_carrier, r.a.rx_hi_carrier); CHECK_EQ_I(r.c.rx_hi_carrier, r.a.tx_hi_carrier);
    CHECK_EQ_I(r.c.tx_preemph, 0); CHECK_EQ_I(r.c.tx_pwr_red, 0); CHECK_EQ_I(r.c.md_len, 0);
    CHECK_EQ_I(r.a.tx_preemph, 0); CHECK_EQ_I(r.a.tx_pwr_red, 0);
    CHECK_EQ_I(r.c.info1a.v90, 0); CHECK_NEAR(r.c.info1a.freq_off, 0, 3);
    /* 11.2.1.1.6: Tone A for 50 ms, then the reversal, as we saw it */
    CHECK_NEAR(r.c.t_rev3 - r.c.t_tone_a2, 400, 56);

    /* ---- bandwidth: the probing must send the symbol rate down ---- */
    s.lp_hz = 2900; s.noise = 4.0;
    run(&s, &r, &ac, &cc);
    printf("  p2call 2.9 kHz line: INFO1c %d %d %d, carriers %d %d %d; answerer chose %d\n",
           r.c.info1c.sr[0].max_rate, r.c.info1c.sr[3].max_rate, r.c.info1c.sr[4].max_rate,
           r.c.info1c.sr[0].hi_carrier, r.c.info1c.sr[3].hi_carrier, r.c.info1c.sr[4].hi_carrier, r.a.sym_rate);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK(r.c.info1c.sr[4].max_rate < r.c.info1c.sr[0].max_rate);
    CHECK(r.c.info1c.sr[3].max_rate < r.c.info1c.sr[0].max_rate);
    CHECK_EQ_I(r.c.info1c.sr[0].hi_carrier, 0);
    CHECK_EQ_I(r.a.sym_rate, 2400); CHECK_EQ_I(r.c.sym_rate, 2400);
    s.lp_hz = 3300;
    run(&s, &r, &ac, &cc);
    printf("  p2call 3.3 kHz line: INFO1c %d %d %d, carriers %d %d %d; answerer chose %d\n",
           r.c.info1c.sr[0].max_rate, r.c.info1c.sr[3].max_rate, r.c.info1c.sr[4].max_rate,
           r.c.info1c.sr[0].hi_carrier, r.c.info1c.sr[3].hi_carrier, r.c.info1c.sr[4].hi_carrier, r.a.sym_rate);
    CHECK_EQ_I(r.c.state, P2L_DONE);
    CHECK(r.c.info1c.sr[4].max_rate < clean4);
    CHECK_EQ_I(r.c.info1c.sr[3].hi_carrier, 0); CHECK_EQ_I(r.c.info1c.sr[4].hi_carrier, 0);
    CHECK(r.c.sym_rate < 3200);

    /* ---- noise: lower projections ---- */
    clean(&s);
    s.d_ca = 400; s.d_ac = 440; s.g_ca = 0.06; s.g_ac = 0.08; s.noise = 6.0; s.echo_a = 0.2; s.echo_c = 0.2;
    run(&s, &r, &ac, &cc);
    printf("  p2call long noisy line: rtd %d / %d, INFO1c %d %d %d (3200 SNR %.1f dB)\n", (int)r.c.rtd, (int)r.a.rtd,
           r.c.info1c.sr[0].max_rate, r.c.info1c.sr[3].max_rate, r.c.info1c.sr[4].max_rate, r.c.band_snr[4] / 10.0);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_NEAR(r.c.rtd, 840, 8); CHECK_NEAR(r.a.rtd, 840, 8);
    CHECK(r.c.info1c.sr[4].max_rate < clean4); CHECK(r.c.info1c.sr[0].max_rate <= clean0);
    CHECK(r.c.info1c.sr[4].max_rate >= 7 && r.c.info1c.sr[4].max_rate <= 11);
    CHECK_EQ_I(r.c.info1c.min_pwr_red, 0);
    clean(&s); s.noise = 40.0;
    run(&s, &r, &ac, &cc);
    printf("  p2call noise 32 dB down: INFO1c %d %d %d, answerer chose %d\n",
           r.c.info1c.sr[0].max_rate, r.c.info1c.sr[3].max_rate, r.c.info1c.sr[4].max_rate, r.a.sym_rate);
    CHECK_EQ_I(r.c.state, P2L_DONE);
    CHECK(r.c.info1c.sr[4].max_rate >= 7 && r.c.info1c.sr[4].max_rate < clean4);
    /* our own path is slow (a driver that buffers): told so, we still turn
     * round in 40 ms at the line */
    clean(&s); s.io_tx = 90; s.io_rx = 70; cc.io_delay = 160;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_NEAR(r.c.rtd, 44, 8); CHECK_NEAR(r.a.rtd, 44, 8);
    cc.io_delay = 0;

    /* ---- slope: pre-emphasis asked of the answerer ---- */
    clean(&s); s.loop = LOOP_LONG; s.nloop = 3;
    run(&s, &r, &ac, &cc);
    printf("  p2call sloping loop: pre-emphasis %d / %d / %d, INFO1c %d %d %d; answerer chose %d, sends with filter %d\n",
           r.c.info1c.sr[0].preemph, r.c.info1c.sr[3].preemph, r.c.info1c.sr[4].preemph,
           r.c.info1c.sr[0].max_rate, r.c.info1c.sr[3].max_rate, r.c.info1c.sr[4].max_rate, r.a.sym_rate, r.a.tx_preemph);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK(r.c.info1c.sr[r.c.sym_idx].preemph > 0);
    CHECK_EQ_I(r.a.tx_preemph, r.c.info1c.sr[r.c.sym_idx].preemph);

    /* ---- level: the answerer's signal overloads our receiver (16-bit clip) ---- */
    clean(&s); s.g_ac = 8.0;
    run(&s, &r, &ac, &cc);
    printf("  p2call hot line: distortion %.1f dB down, power reduction %d (+ %d); INFO1c %d %d %d\n", r.c.sdr_db10 / 10.0,
           r.c.info1c.min_pwr_red, r.c.info1c.add_pwr_red, r.c.info1c.sr[0].max_rate, r.c.info1c.sr[3].max_rate, r.c.info1c.sr[4].max_rate);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK(r.c.sdr_db10 < 340);              /* more than a G.711 codec's share */
    CHECK(r.c.info1c.min_pwr_red >= 1);
    CHECK_EQ_I(r.a.tx_pwr_red, r.c.info1c.min_pwr_red);
    clean(&s);
    run(&s, &r, &ac, &cc);
    CHECK(r.c.sdr_db10 > 400);

    /* ---- what we allow ---- */
    cc.sr_mask = P2N_SR_2400;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_DONE);
    CHECK_EQ_I(r.a.info1c.sr[3].max_rate, 0); CHECK_EQ_I(r.a.info1c.sr[4].max_rate, 0);
    CHECK_EQ_I(r.a.sym_rate, 2400); CHECK_EQ_I(r.c.sym_rate, 2400); CHECK_EQ_I(r.c.sym_idx, 0);
    cc.sr_mask = 0; cc.max_rate = 14400;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_DONE);
    for (k = 0; k < 6; k++) CHECK(r.a.info1c.sr[k].max_rate <= 6);
    cc.max_rate = 0;

    /* ---- recovery ---- */
    clean(&s);
    /* INFO0c lost (11.2.2.2.1 at the answerer) */
    s.mute_ca_to = 900;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK(r.c.n_info0c >= 2); CHECK(r.a.n_info0a >= 2);
    CHECK_NEAR(r.c.rtd, 44, 8);
    /* INFO0a lost (11.2.2.1.1): Tone A comes before INFO0a, so INFO0c again */
    s.mute_ca_to = 0; s.mute_ac_to = 900;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK(r.c.n_info0c >= 2); CHECK_EQ_I(r.c.info0a.c1664, 1);
    s.mute_ac_to = 0;
    {
        scen_t s2 = s; run_t ref;
        run(&s2, &ref, &ac, &cc);
        /* the first Tone A reversal lost (11.2.2.1.2): Tone B goes on until
         * the answerer, 2000 ms later, starts over (11.2.2.2.2) */
        s2.mute_ac_from = ref.c.t_rev1 - 200; s2.mute_ac_to = ref.c.t_rev1 + 400;
        run(&s2, &r, &ac, &cc);
        CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
        CHECK(r.a.restarts >= 1);
        CHECK_NEAR(r.c.rtd, 44, 8); CHECK_NEAR(r.a.rtd, 44, 8);
        /* the second one lost (11.2.2.1.3): 2000 ms, silence, Tone A again, Tone B */
        s2 = s; s2.mute_ac_from = ref.c.t_rev2 - 200; s2.mute_ac_to = ref.c.t_rev2 + 400; s2.seconds = 12;
        run(&s2, &r, &ac, &cc);
        printf("  p2call second reversal lost: done at %.2f s, our restarts %d, answerer's %d\n",
               r.c_done_at / 8000.0, r.c.restarts, r.a.restarts);
        CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
        CHECK(r.c.restarts >= 1);
        CHECK_NEAR(r.c.rtd, 44, 8); CHECK_NEAR(r.a.rtd, 44, 8);
        /* the third (11.2.1.1.6) lost (11.2.2.1.4): our reversal all the same */
        s2 = s; s2.mute_ac_from = ref.c.t_rev3 - 40; s2.mute_ac_to = ref.c.t_rev3 + 200;
        run(&s2, &r, &ac, &cc);
        CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
        CHECK_EQ_I(r.c.rev3_timeouts, 1); CHECK_EQ_I(r.c.retrains, 0);
        CHECK_EQ_I(r.c.sym_rate, 3200);
        /* Tone A not heard under our L2 (11.2.2.1.5): we begin a retrain
         * (11.5.1.1); the answerer, without INFO1c, begins one too */
        s2 = s; s2.mute_ac_from = ref.c.t_rev3 + 800; s2.mute_ac_to = ref.c_done_at; s2.seconds = 14;
        run(&s2, &r, &ac, &cc);
        printf("  p2call Tone A lost under L2: done at %.2f s after %d retrain(s) of ours, %d of the answerer's\n",
               r.c_done_at / 8000.0, r.c.retrains, r.a.retrains);
        CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
        CHECK_EQ_I(r.c.retrains, 1);
        CHECK_EQ_I(r.c.sym_rate, 3200); CHECK_NEAR(r.c.rtd, 44, 8);
        /* INFO1c lost: the answerer's Tone A goes on; 700 ms and a round trip
         * after INFO1c we answer it as a retrain (11.2.2.1.6, 11.5.1.2) */
        s2 = s; s2.mute_ca_from = ref.c_done_at - 1600; s2.mute_ca_to = ref.c_done_at + 400; s2.seconds = 14;
        run(&s2, &r, &ac, &cc);
        printf("  p2call INFO1c lost: done at %.2f s, %d retrain(s) of ours, %d of the answerer's\n",
               r.c_done_at / 8000.0, r.c.retrains, r.a.retrains);
        CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
        CHECK_EQ_I(r.c.retrains, 1); CHECK_EQ_I(r.a.retrains, 1);
        CHECK_EQ_I(r.c.sym_rate, 3200);
        /* INFO1a lost: the answerer went on to Phase 3 and comes back with
         * Tone A (its own retrain): we answer it */
        s2 = s; s2.mute_ac_from = ref.c_done_at - 800; s2.mute_ac_to = ref.c_done_at + 200; s2.ans_back_ms = 1500; s2.seconds = 14;
        run(&s2, &r, &ac, &cc);
        CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
        CHECK_EQ_I(r.c.retrains, 1); CHECK_EQ_I(r.a_restarts, 1);
        /* ... or stays away: we begin the retrain, and it answers our Tone B */
        s2.ans_back_ms = 0; s2.ans_answers = 1;
        run(&s2, &r, &ac, &cc);
        printf("  p2call INFO1a lost: done at %.2f s after %d retrain(s) of ours\n", r.c_done_at / 8000.0, r.c.retrains);
        CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
        CHECK_EQ_I(r.c.retrains, 1); CHECK_EQ_I(r.a_restarts, 1);
        CHECK_EQ_I(r.c.sym_rate, 3200); CHECK_NEAR(r.c.rtd, 44, 8);
    }

    /* ---- the retrain entry (11.5.1): no INFO0, Tone B at once ---- */
    clean(&s); s.retrain = 1;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_DONE); CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.c.n_info0c, 0); CHECK_EQ_I(r.a.n_info0a, 0);
    CHECK_NEAR(r.c.rtd, 44, 8); CHECK_NEAR(r.a.rtd, 44, 8);
    CHECK_EQ_I(r.c.sym_rate, 3200);
    CHECK_EQ_I(r.c.info0a.c1664, 1);        /* the first Phase 2's */

    /* ---- a retrain's Phase 2 bounded (spec D5): an answerer that keeps
     * sending INFO0a sends us back to 11.2.1.1.1 each time, counted as a
     * restart ---- */
    clean(&s); s.retrain = 1; s.ans_reinit = 1; s.seconds = 120;
    run(&s, &r, &ac, &cc);
    printf("  p2call INFO0a for ever: state %d, error %d at %.2f s, restarts %d, INFO0c sent %d, answerer started %d times\n",
           r.c.state, r.c.error, r.n / 8000.0, r.c.restarts, r.c.n_info0c, r.a_restarts);
    CHECK_EQ_I(r.c.state, P2L_FAILED);
    CHECK_EQ_I(r.c.error, P2L_ERR_REV);
    CHECK(r.n <= 8000 * (4 * 8 + 1));

    /* ---- dead lines: give up with a reason ---- */
    clean(&s); s.g_ac = 0.0; s.seconds = 30;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_FAILED); CHECK_EQ_I(r.c.error, P2L_ERR_INFO0);
    CHECK(r.n < 8000 * 12);
    clean(&s); s.g_ca = 0.0; s.seconds = 30;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.c.state, P2L_FAILED); CHECK_EQ_I(r.c.error, P2L_ERR_REV);
    CHECK(r.n < 8000 * 25);

    /* ---- the clock from L2 (calling B6): a strong L2 whose tones turn
     * about a quarter turn per 20 ms block (far beyond 1000 ppm), so that
     * ci is huge against cr: the clamp, not a 64-bit overflow ---- */
    {
        static proberx_t pr;
        int m, sgn;
        for (sgn = 1; sgn >= -1; sgn -= 2) {
            memset(&pr, 0, sizeof pr);
            pr.nblk = 50;
            for (m = 1; m <= PROBE_NBIN; m++) {
                if (!probe_has_tone(m)) continue;
                pr.cr[m] = (int64_t)1 << 20;
                pr.ci[m] = sgn * ((int64_t)1 << 41);
                pr.pw[m] = ((int64_t)1 << 41) + ((int64_t)1 << 20);
            }
            CHECK(proberx_snr(&pr, 7) >= 200);
            printf("  clock from a hostile L2 (ci %c2^41, cr 2^20): %d/256 ppm\n", sgn > 0 ? '+' : '-',
                   (int)p2call_clock_q8(&pr));
            CHECK_EQ_I(p2call_clock_q8(&pr), -sgn * 256000);    /* the +-1000 ppm clamp */
        }
        /* within the clamp, as before: one tone (1050 Hz) turning 1/476 rad per block, about 16 ppm */
        memset(&pr, 0, sizeof pr);
        pr.nblk = 50;
        pr.cr[7] = (int64_t)1 << 30; pr.ci[7] = -((int64_t)1 << 30) / 476; pr.pw[7] = (int64_t)1 << 30;
        printf("  clock from a tone at 1050 Hz, ci/cr -1/476: %d/256 ppm\n", (int)p2call_clock_q8(&pr));
        CHECK_EQ_I(p2call_clock_q8(&pr), 4076);         /* 256e6 / (476 6 pi 7) */
        /* right at the limit: num = 20 den with den at its largest (2^31 - 1
         * after the scaling loop), the largest product the multiply takes
         * (20 (2^31 - 1) 13581342, about 5.8e17); one step past it, the
         * early clamp. 1050 Hz alone: num = 7 (ci >> 8), den = 49 (cr >> 8) */
        for (sgn = 0; sgn <= 1; sgn++) {
            const int64_t x = 43826196;                 /* 49 x <= 2^31 - 1 */
            memset(&pr, 0, sizeof pr);
            pr.nblk = 50;
            pr.cr[7] = 256 * x; pr.ci[7] = 256 * 140 * x + 256 * sgn;
            pr.pw[7] = pr.ci[7] + pr.cr[7] / 64;
            CHECK(proberx_snr(&pr, 7) >= 200);
            printf("  clock at the limit%s (ci/cr 140%s): %d/256 ppm\n", sgn ? ", one past" : "", sgn ? " + 1/x" : "",
                   (int)p2call_clock_q8(&pr));
            CHECK_EQ_I(p2call_clock_q8(&pr), -256000);
        }
    }

    T_REPORT("p2call");
}
