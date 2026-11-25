/* v92_rx: Ru / Su detector and the training receiver, against a synthetic
 * channel (FIR, white noise, G.711 quantiser). */
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "v92_rx.h"
#include "v90_pcm.h"
#include "pcmenc.h"

static uint32_t seed = 12345;
static double gauss(void)
{
    double s = 0; int i;
    for (i = 0; i < 12; i++) s += (double)(tu_lcg(&seed) & 0xffff) / 65536.0;
    return s - 6.0;
}

/* channel: taps c[0..nc-1] (linear per unit symbol), noise, codec */
typedef struct { const double *c; int nc; double hist[64]; double sigma; int alaw; } chan_t;
static const double C1[] = { 0.02, -0.08, 1.0, 0.45, -0.22, 0.12, -0.07, 0.04, -0.02, 0.01 };
static const double C2[] = { -0.03, 0.15, 0.9, -0.5, 0.1, -0.2, 0.08, 0.05, -0.03, -0.02, 0.01 };

static int16_t chan(chan_t *ch, double sym)
{
    double y = 0; int i;
    memmove(ch->hist + 1, ch->hist, sizeof ch->hist - sizeof ch->hist[0]);
    ch->hist[0] = sym;
    for (i = 0; i < ch->nc; i++) y += ch->c[i] * ch->hist[i];
    y += ch->sigma * gauss();
    if (y > 32000) y = 32000;
    if (y < -32000) y = -32000;
    return (int16_t)v90_octet_linear(ch->alaw, pcm_encode(ch->alaw, (int16_t)floor(y + 0.5)));
}

/* ---- detector ---- */
static void test_p6(int su, double amp, int alaw)
{
    v92_p6_t d;
    chan_t ch;
    int n, first_on = -1, nrev = 0, rev_at[4] = { 0, 0, 0, 0 }, on_at_end = 0;
    /* Ru: 384, Ru-bar 24, noise.  Su: 144, Su-bar 25, Su 240, Su-bar 24, noise */
    int t1 = su ? 144 : 384, t2 = su ? t1 + 25 : t1 + 24, t3 = su ? t2 + 240 : t2, t4 = su ? t3 + 24 : t2;
    memset(&ch, 0, sizeof ch); ch.c = C1; ch.nc = 10; ch.sigma = 6; ch.alaw = alaw;
    v92_p6_init(&d);
    for (n = -200; n < t4 + 300; n++) {
        double s = 0;
        int bar = (n >= t1 && n < t2) || (n >= t3 && n < t4), r;
        if (n >= 0 && n < t4) {
            unsigned ph = (unsigned)(n >= t2 && su ? n - t2 : n);     /* the pattern restarts after the slip */
            s = su ? (double)v92_su_level(ph, bar, 4096) / 4096.0 : (double)V92_UP_SIGN(v92_ru_sbit(ph, bar));
        } else if (n >= t4) {
            s = (tu_lcg(&seed) & 1) ? 1.0 : -1.0;                     /* TRN1u-like */
        }
        r = v92_p6_tick(&d, chan(&ch, s * amp), 20);
        if ((r & V92_P6_ON) && first_on < 0) first_on = n;
        if (r & V92_P6_REV) { if (nrev < 4) rev_at[nrev] = n; nrev++; }
        if (n >= t4 + 100 && (r & V92_P6_ON)) on_at_end++;
    }
    if (!(first_on >= 20 && first_on < 90)) printf("  first_on %d\n", first_on);
    CHECK(first_on >= 20 && first_on < 90);
    CHECK_EQ_I(nrev, su ? 2 : 1);
    /* the main tap of C1 is 2 samples late */
    CHECK_NEAR(rev_at[0], t1 + 2 + V92_P6_REV_LAG, 2);
    if (su) CHECK_NEAR(rev_at[1], t3 + 2 + V92_P6_REV_LAG, 2);
    CHECK_EQ_I(on_at_end, 0);
}

/* Ru at full scale, straight into the detector: + at hi, - at lo (32767 and
 * -32767, or -32768: the products of the correlations must not overflow) */
static void test_p6_full(int16_t hi, int16_t lo)
{
    v92_p6_t d;
    int n, first_on = -1, nrev = 0, rev_at = 0;
    v92_p6_init(&d);
    for (n = -200; n < 384 + 24 + 300; n++) {
        int16_t x = 0;
        int r;
        if (n >= 0 && n < 384 + 24)
            x = V92_UP_SIGN(v92_ru_sbit((unsigned)n, n >= 384)) > 0 ? hi : lo;
        r = v92_p6_tick(&d, x, 20);
        if ((r & V92_P6_ON) && first_on < 0) first_on = n;
        if (r & V92_P6_REV) { if (nrev == 0) rev_at = n; nrev++; }
    }
    CHECK(first_on >= 20 && first_on < 90);
    CHECK_EQ_I(nrev, 1);
    CHECK_NEAR(rev_at, 384 + V92_P6_REV_LAG, 2);
}

static void test_p6_quiet(void)
{
    v92_p6_t d;
    int n, hits = 0;
    v92_p6_init(&d);
    for (n = 0; n < 40000; n++) {       /* noise, then random two-level data */
        int16_t x = (int16_t)(n < 20000 ? 300.0 * gauss() : ((tu_lcg(&seed) & 1) ? 3000 : -3000) + 50.0 * gauss());
        if (v92_p6_tick(&d, x, 20)) hits++;
    }
    CHECK_EQ_I(hits, 0);
    v92_p6_init(&d);
    for (n = 0; n < 2000; n++)          /* a period-6 signal below the floor */
        if (v92_p6_tick(&d, (int16_t)(10 * V92_UP_SIGN(v92_ru_sbit((unsigned)n, 0))), 20)) hits++;
    CHECK_EQ_I(hits, 0);
    /* What an echo canceller leaves of an echo with a slow tail (seen on the
     * FXS port in front of a real modem, live/v92a.raw): a wander of some
     * hundred units, nearly all of it below 60 Hz. It repeats after 6 samples
     * as well as anything, and is neither Ru nor Su. */
    {
        double lp = 0, rms = 0;
        v92_p6_init(&d);
        for (n = 0; n < 40000; n++) {
            int16_t x;
            lp += ((tu_lcg(&seed) & 1 ? 3900.0 : -3900.0) - lp) / 64.0;
            x = (int16_t)(lp + 20.0 * gauss());
            rms += (double)x * x;
            if (v92_p6_tick(&d, x, 20)) hits++;
        }
        CHECK(sqrt(rms / 40000) > 200);
        CHECK_EQ_I(hits, 0);
    }
}

/* ---- training receiver ---- */
static void test_trn(const double *c, int nc, int main_tap, double amp, double sigma, int alaw, int offset)
{
    static v92_trn_t t;
    static int16_t lev[6000];
    v92_sgn_t tx; v92_ml_t ml;
    chan_t ch;
    int n, m = 0, ndec = 0, nerr = 0, first = -1, r, j, last_m = -1, order_bad = 0;
    const int n_trn = 2040, n_seq = 600, n_ml = 2400, total = n_trn + n_seq + n_ml;
    double emax = 0, hp = 0, sq = 0;
    memset(&ch, 0, sizeof ch); ch.c = c; ch.nc = nc; ch.sigma = sigma; ch.alaw = alaw;
    v92_sgn_init(&tx, 0);
    /* the transmitter starts `offset` samples after the receiver was told */
    v92_trn_init(&t);
    for (n = 0; n < offset; n++) (void)v92_trn_tick(&t, chan(&ch, V92_UP_SIGN(v92_ru_sbit((unsigned)n, 1)) * amp));
    for (n = 0; n < -offset; n++, m++) {
        lev[m] = (int16_t)V92_UP_SIGN(v92_sgn_tx_trn(&tx));
        (void)chan(&ch, lev[m] * amp);
    }
    for (; m < total + 40; m++) {
        double s;
        if (m < n_trn) { lev[m] = (int16_t)V92_UP_SIGN(v92_sgn_tx_trn(&tx)); s = lev[m]; }
        else if (m < n_trn + n_seq) { lev[m] = (int16_t)V92_UP_SIGN(v92_sgn_tx_bit(&tx, (int)(tu_lcg(&seed) & 1))); s = lev[m]; }
        else {
            uint8_t b[2];
            if (m == n_trn + n_seq) v92_ml_init(&ml, 0, lev[m - 1] < 0);
            b[0] = (uint8_t)(tu_lcg(&seed) & 1); b[1] = (uint8_t)(tu_lcg(&seed) & 1);
            lev[m] = (int16_t)v92_ml_tx(&ml, b);
            s = (double)v92_trn2u_level(lev[m], 0, 4096) / 4096.0;
        }
        r = v92_trn_tick(&t, chan(&ch, s * amp));
        CHECK(r >= 0);
        if (r == 1) {
            if (first < 0) first = t.dec_m;
            if (t.dec_m != last_m + 1 && last_m >= 0) order_bad++;
            last_m = t.dec_m;
            if (t.dec_m >= 0 && t.dec_m < total) { ndec++; if (t.dec_level != lev[t.dec_m]) nerr++; }
            /* the owner switches at the symbol boundary it knows from the sequences */
            if (t.dec_m == n_trn + n_seq - 1) v92_trn_levels(&t, 4);
        }
    }
    CHECK(first >= 0 && first < 800);
    CHECK_EQ_I(order_bad, 0);
    CHECK(ndec > total - 800);
    CHECK_EQ_I(nerr, 0);
    CHECK_EQ_I(t.cpk, V92_CH_LEAD + main_tap + offset);
    /* the estimate against the truth, relative to the main tap */
    for (j = 0; j < V92_CH_NTAP; j++) {
        int i = j - V92_CH_LEAD - offset;
        double want = (i >= 0 && i < nc) ? c[i] * amp : 0.0, got = (double)v92_trn_h(&t, j) / 256.0;
        if (fabs(got - want) > emax) emax = fabs(got - want);
        if (fabs(want) > hp) hp = fabs(want);
        sq += (got - want) * (got - want);
    }
    printf("  trn amp %.0f sigma %.0f %s offset %d: cursor %d, estimate error max %.1f rms %.1f (peak %.0f), residual rms %.1f, eq mse %.4f\n",
           amp, sigma, alaw ? "A-law" : "mu-law", offset, t.cpk, emax, sqrt(sq), hp, sqrt((double)t.res),
           (double)t.mse / (4096.0 * 4096.0));
    CHECK(emax < 0.01 * hp);
    CHECK_NEAR(t.hpk, c[main_tap] * amp, 0.05 * hp);       /* as it was when the cursor was chosen */
    /* the residual is the noise: line noise and the quantiser, never less */
    CHECK(sqrt((double)t.res) > sigma * 0.9 && sqrt((double)t.res) < sigma * 1.3 + 0.03 * fabs(amp));
    CHECK(abs(v92_trn_peak(&t) - 256 * (main_tap + offset)) < 128);
}

static void test_dead(void)
{
    static v92_trn_t t;
    int n, r = 0;
    v92_trn_init(&t);
    for (n = 0; n < 1000 && r == 0; n++) r = v92_trn_tick(&t, (int16_t)(3.0 * gauss()));
    CHECK_EQ_I(r, -1);
}

int main(void)
{
    test_p6(0, 4000, 0); test_p6(1, 4000, 0); test_p6(0, -900, 1); test_p6(1, 12000, 1);
    test_p6_full(32767, -32767); test_p6_full(32767, -32768);
    test_p6_quiet();
    test_trn(C1, 10, 2, 4000, 5, 0, 0);
    test_trn(C1, 10, 2, 4000, 40, 0, 3);
    test_trn(C1, 10, 2, -1500, 5, 1, -3);
    test_trn(C2, 11, 2, 7000, 20, 1, 2);
    test_trn(C2, 11, 2, 2500, 10, 0, -2);
    test_dead();
    T_REPORT("rx");
}
