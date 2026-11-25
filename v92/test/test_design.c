/* v92_design: design the CPd parameters for a known channel, then run the
 * analogue modem's transmitter (v92_enc) through that channel, a G.711
 * quantiser with echo and noise, into the decoder (v92_dec). */
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "v92_design.h"
#include "v92_dec.h"
#include "v90_pcm.h"
#include "pcmenc.h"

static uint32_t seed = 777;
static double gauss(void)
{
    double s = 0; int i;
    for (i = 0; i < 12; i++) s += (double)(tu_lcg(&seed) & 0xffff) / 65536.0;
    return s - 6.0;
}

static const double C1[] = { 0.02, -0.08, 1.0, 0.45, -0.22, 0.12, -0.07, 0.04, -0.02, 0.01 };
static const double C2[] = { -0.03, 0.15, 0.9, -0.5, 0.1, -0.2, 0.08, 0.05, -0.03, -0.02, 0.01 };
static const double C3[] = { 0.05, 1.0, -0.55, -0.25, -0.12, -0.07, -0.04, -0.02 };     /* no DC */

/* the pulses of sim/v92chan.c, shapes 0 and 1, sampled 0.4 T late: the
 * codec's high-pass filter leaves a long, weak tail */
static const double C4[72] = {
    0.01744, 0.90915, 0.13298, -0.32655, -0.04728, -0.17637, -0.10082, -0.10969,
    -0.09381, -0.08098, -0.07277, -0.06127, -0.05308, -0.04429, -0.03676, -0.02976,
    -0.02344, -0.01779, -0.01274, -0.00829, -0.00437, -0.00098, 0.00194, 0.00442,
    0.00649, 0.00819, 0.00955, 0.01061, 0.01140, 0.01195, 0.01228, 0.01243,
    0.01242, 0.01227, 0.01201, 0.01165, 0.01121, 0.01071, 0.01016, 0.00957,
    0.00897, 0.00835, 0.00773, 0.00710, 0.00649, 0.00590, 0.00532, 0.00477,
    0.00425, 0.00375, 0.00328, 0.00284, 0.00244, 0.00206, 0.00172, 0.00140,
    0.00112, 0.00086, 0.00064, 0.00043, 0.00025, 0.00010, -0.00004, -0.00015,
    -0.00025, -0.00033, -0.00039, -0.00044, -0.00048, -0.00051, -0.00053, -0.00054,
};
static const double C5[72] = {
    0.01129, 0.70627, 0.15127, -0.50670, -0.25151, -0.27155, -0.15012, -0.08975,
    -0.03757, 0.00451, 0.02900, 0.04781, 0.05702, 0.06147, 0.06142, 0.05846,
    0.05354, 0.04737, 0.04061, 0.03368, 0.02692, 0.02056, 0.01474, 0.00956,
    0.00506, 0.00123, -0.00194, -0.00448, -0.00647, -0.00794, -0.00896, -0.00960,
    -0.00992, -0.00996, -0.00978, -0.00943, -0.00895, -0.00837, -0.00773, -0.00704,
    -0.00634, -0.00563, -0.00495, -0.00428, -0.00365, -0.00306, -0.00251, -0.00200,
    -0.00155, -0.00113, -0.00077, -0.00045, -0.00017, 0.00007, 0.00028, 0.00045,
    0.00059, 0.00070, 0.00079, 0.00085, 0.00089, 0.00092, 0.00093, 0.00093,
    0.00092, 0.00090, 0.00087, 0.00083, 0.00080, 0.00075, 0.00071, 0.00066,
};

#ifdef TEST_SHORT
#define NSYM 6000
#else
#define NSYM 36000
#endif
static uint8_t txb[NSYM * 6 + 100];
static int ntx, nrx, nbad;
static int get(void *u) { (void)u; txb[ntx] = (uint8_t)(tu_lcg(&seed) & 1); return txb[ntx++]; }
static void put(void *u, int b) { (void)u; if (nrx < ntx && txb[nrx] != b) nbad++; nrx++; }

/* the codec's A/D: G.711, or 16-bit linear (rx_lin) */
static int rx_lin;
static int16_t codec(int alaw, double y)
{
    int16_t v = (int16_t)floor(y + 0.5);
    return rx_lin ? v : (int16_t)v90_octet_linear(alaw, pcm_encode(alaw, v));
}
static uint32_t qv(int alaw, int32_t a) { return rx_lin ? 0 : v92_dsn_qvar(alaw, a); }

static int run(const char *name, const double *c, int nc, int del, double amp, double sigma, double echo, int alaw,
               uint32_t rates, int min_bps, int max_bps)
{
    static v92_dsn_t d;
    static v92_enc_t enc;
    static v92_dec_t dec;
    static double s[NSYM + 512];
    v92_dsn_cfg cfg;
    int j, r, m, k, bps;
    double pw = 0, sum2 = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.alaw = alaw; cfg.rx_linear = rx_lin;
    for (j = 0; j < V92_CH_NTAP; j++) {
        int i = j - V92_CH_LEAD - del;
        cfg.h[j] = (i >= 0 && i < nc) ? (int32_t)floor(c[i] * amp * 256.0 + 0.5) : 0;
        sum2 += (double)cfg.h[j] * cfg.h[j] / 65536.0;
    }
    /* what the modem measures: with the far end silent, and with a
     * two-level training signal */
    {
        double hist[96], a0 = 0, a1 = 0, q0 = 0, q1 = 0;
        int t, n = 4000;
        memset(hist, 0, sizeof hist);
        for (t = 0; t < 2 * n; t++) {
            double e = echo * gauss(), y = 0, w = sigma * gauss();
            int16_t q;
            memmove(hist + 1, hist, sizeof hist - sizeof hist[0]);
            hist[0] = t < n ? 0.0 : (tu_lcg(&seed) & 1) ? amp : -amp;
            for (j = 0; j < nc; j++) y += c[j] * hist[j];
            q = codec(alaw, y + e + w);
            if (t < n) { a0 += (q - e) * (q - e); q0 += qv(alaw, q); }
            else { a1 += (q - e - y) * (q - e - y); q1 += qv(alaw, q); }
        }
        cfg.noise = (uint32_t)(a0 / n); cfg.q_quiet = (uint32_t)(q0 / n);
        cfg.res = (uint32_t)(a1 / n); cfg.q_trn = (uint32_t)(q1 / n);
    }
    cfg.echo_rms = (uint32_t)echo;
    (void)sum2;
    cfg.ltot = 192; cfg.lmax = 128;
    cfg.rates = rates;
    v92_dsn_start(&d, &cfg);
    while ((r = v92_dsn_step(&d)) == 0) ;
    if (r != 1) { printf("  %s: design failed %d\n", name, r); CHECK(min_bps == 0); return r; }
    CHECK_EQ_I(v92_map_check(&d.cpd.map), 0);
    CHECK_EQ_I(v92_filt_check(&d.cpd.filt), 0);
    CHECK(d.cpd.filt.lz2 + d.cpd.filt.lp1 <= 192 && d.cpd.filt.lz2 <= 128);
    CHECK(d.cpd.drn >= 1 && d.cpd.drn <= 19 && (rates & (1u << (d.cpd.drn - 1))));
    CHECK_EQ_I(d.cpd.map.K, V92_UP_K(d.cpd.drn));
    bps = V92_UP_BPS(d.cpd.drn);

    /* the call: transmitter, channel, codec, decoder */
    ntx = nrx = nbad = 0;
    CHECK_EQ_I(v92_enc_init(&enc, &d.cpd.map, &d.cpd.filt, get, 0), 0);
    CHECK_EQ_I(v92_dec_init(&dec, &d.cpd.map, put, 0), 0);
    memset(s, 0, sizeof s);
    for (m = 0; m < NSYM; m++) {
        s[m + 256] = (double)v92_enc_symbol(&enc, 0) / 16384.0;
        pw += s[m + 256] * s[m + 256];
    }
    for (m = 0; m < NSYM - 300; m++) {
        double y = 0, e = echo * gauss();
        int16_t q;
        k = m + d.dly;                      /* the sample that carries u(m) */
        for (j = 0; j < nc; j++) y += c[j] * amp * s[256 + k - del - j];
        y += e + sigma * gauss();
        if (y > 32000) y = 32000;
        if (y < -32000) y = -32000;
        q = codec(alaw, y);
        v92_dec_put(&dec, (int32_t)floor((double)q - e + 0.5), 0);
    }
    printf("  %s %s amp %.0f noise %.0f echo %.0f: %d bit/s, M %d of %d, 4G %u, delay %d, power %.2f (limit 1), "
           "ISI %.1f dB, rho %.2f, x rms max %u, peak %.2f, %d cuts, %d steps, %d bits, %d errors\n",
           name, alaw ? "A-law" : "mu-law", amp, sigma, echo, bps, d.cpd.map.M[0], d.ncand, d.cpd.filt.gain4, d.dly,
           pw / NSYM, 10 * log10((double)d.isi_q28 / 268435456.0 + 1e-12), (double)d.rho_q8 / 256.0, d.xrms_max, d.xpk_q4 / 16.0, d.cut, d.steps, nrx, nbad);
    CHECK(pw / NSYM <= 1.08);
    CHECK(nrx > (NSYM - 600) * d.cpd.map.K / 12);
    CHECK_EQ_I(nbad, 0);
    CHECK(bps >= min_bps && bps <= max_bps);
    CHECK(dec.bad_frames == 0);
    return bps;
}

int main(void)
{
    const uint32_t all = (1u << V92_UP_NUM_RATES) - 1;
    static v92_dsn_t d;
    v92_dsn_cfg cfg;
    int r, hi, lo;

    hi = run("C1", C1, 10, 2, 4000, 4, 0, 0, all, 36000, 48000);
    lo = run("C1", C1, 10, 2, 4000, 40, 0, 0, all, 26000, 38000);
    CHECK(lo < hi);
    (void)run("C1", C1, 10, 0, 4000, 4, 600, 0, all, 33000, 48000);
    (void)run("C1", C1, 10, 2, -4000, 4, 300, 1, all, 33000, 48000);
    (void)run("C2", C2, 11, 3, 5000, 4, 300, 0, all, 33000, 48000);
    (void)run("C2", C2, 11, 3, 2000, 20, 300, 1, all, 24000, 40000);
    (void)run("C3", C3, 8, 1, 4000, 4, 300, 0, all, 33000, 48000);
    (void)run("C3", C3, 8, 1, 4000, 4, 300, 1, all, 33000, 48000);
    (void)run("line 0", C4, 72, 2, 4000, 3, 400, 0, all, 33000, 48000);
    (void)run("line 0", C4, 72, 2, 4000, 40, 400, 1, all, 26000, 40000);
    (void)run("line 1", C5, 72, 2, 4000, 3, 400, 1, all, 33000, 48000);
    (void)run("line 1", C5, 72, 2, 1500, 20, 400, 0, all, 26000, 40000);
    /* the rate mask of Ja is obeyed */
    r = run("C1 masked", C1, 10, 2, 4000, 4, 0, 0, 0x00000111u, 24000, 34666);
    CHECK(r == 24000 || r == 29333 || r == 34666);

    /* A codec with a linear A/D (cfg.rx_linear). The line of the unit: the
     * caller no louder than our echo at the codec. With a G.711 encoder the
     * points must keep the distance of its steps at the echo's peaks; a
     * linear one leaves the line noise and the echo allowance. */
    lo = run("line 0, echo as loud", C4, 72, 2, 1400, 3, 1400, 0, all, 24000, 32000);
    rx_lin = 1;
    hi = run("the same, linear A/D", C4, 72, 2, 1400, 3, 1400, 0, all, 36000, 48000);
    CHECK(hi >= lo + 8000);
    r = run("the same, A-law tables", C4, 72, 2, 1400, 3, 1400, 1, all, 36000, 48000);
    CHECK_EQ_I(r, hi);                      /* the law no longer matters */
    rx_lin = 0;

    /* failures */
    memset(&cfg, 0, sizeof cfg);
    cfg.rates = all;
    v92_dsn_start(&d, &cfg);
    while ((r = v92_dsn_step(&d)) == 0) ;
    CHECK_EQ_I(r, V92_DSN_ERR_CHANNEL);
    cfg.h[V92_CH_LEAD] = 4000 * 256; cfg.noise = 4000 * 4000; cfg.res = cfg.noise;      /* hopeless */
    v92_dsn_start(&d, &cfg);
    while ((r = v92_dsn_step(&d)) == 0) ;
    CHECK_EQ_I(r, V92_DSN_ERR_RATE);
    /* An echo estimate past 96 * 65535 (linear A/D, whose divisor is 96;
     * canceller taps at their limit, security pass M1): the noise term
     * saturates, so the design is as hopeless as with a little less echo,
     * and does not wrap to a quiet line and a dense constellation */
    {
        uint32_t er[2] = { 96u * 65535u, 96u * 65536u };
        int i;
        for (i = 0; i < 2; i++) {
            memset(&cfg, 0, sizeof cfg);
            cfg.h[V92_CH_LEAD] = 4000 * 256; cfg.noise = 9; cfg.q_quiet = 3; cfg.res = 600; cfg.q_trn = 300;
            cfg.echo_rms = er[i]; cfg.rates = all; cfg.rx_linear = 1;
            v92_dsn_start(&d, &cfg);
            while ((r = v92_dsn_step(&d)) == 0) ;
            CHECK_EQ_I(r, V92_DSN_ERR_RATE);
        }
    }
    /* A channel estimate with a long low-pass tail (a ramp from 10^6 down
     * to 0), the noise and training figures of an ordinary call: the
     * design's vectors grow past 32 bits. It ends with an end code, in a
     * bounded number of steps. */
    {
        static const int ramp[2][2] = { { 30, 40 }, { 40, V92_CH_NTAP - 40 } };
        int i, j;
        for (i = 0; i < 2; i++) {
            memset(&cfg, 0, sizeof cfg);
            for (j = 0; j < ramp[i][1]; j++)
                cfg.h[ramp[i][0] + j] = 1000000 - j * (1000000 / ramp[i][1]);
            cfg.noise = 9; cfg.q_quiet = 3; cfg.echo_rms = 400; cfg.res = 600; cfg.q_trn = 300;
            cfg.ltot = 256; cfg.lmax = 192; cfg.rates = all;
            v92_dsn_start(&d, &cfg);
            while ((r = v92_dsn_step(&d)) == 0 && d.steps < 1000000) ;
            printf("  ramp from tap %d, %d taps: end %d after %d steps\n", ramp[i][0], ramp[i][1], r, d.steps);
            CHECK(r != 0);
        }
    }
    /* The peak in the last tap, random ISI as large as the peak (run 2442
     * of the audit's fuzz): the cursor sits so late that the feedback taps
     * after it reach the end of g. The trials must not read past it. */
    {
        static const int32_t H[V92_CH_NTAP] = {
            -11316, -827, -6630, -6585, 12144, 10072, -10678, 13411, 3390, 15179, -3871, -10771,
            -8338, -9896, -7201, -15314, 1110, 7020, -8938, 8740, 11325, -2952, 11790, -11815,
            12731, 7800, -12626, 7730, 5955, 8669, 16370, -6903, -896, 15523, -9902, -4835,
            -2724, -9478, 5423, -3604, 5408, -13439, -2726, -15835, 15712, 9711, 517, -1343,
            10280, 4272, 4376, -13017, -13208, 4724, 4665, -10847, -12879, 13555, 12147, -8989,
            -865, -5183, -7258, 7522, -9162, -151, -675, 6496, -6431, -11635, -15396, -1920,
            7203, -5311, -2389, 6239, -15240, -14550, 8694, 14355, -15454, 14785, -5315, 12465,
            -16413, 5192, 1219, 8306, 10304, 1956, -13285, -5212, 11714, 9674, -2411, 16560,
        };
        memset(&cfg, 0, sizeof cfg);
        memcpy(cfg.h, H, sizeof cfg.h);
        cfg.alaw = 1; cfg.rx_linear = 1;
        cfg.noise = 3176; cfg.q_quiet = 1067; cfg.echo_rms = 616; cfg.res = 61828; cfg.q_trn = 65775;
        cfg.ltot = 320; cfg.lmax = 128; cfg.rates = all;
        v92_dsn_start(&d, &cfg);
        while ((r = v92_dsn_step(&d)) == 0 && d.steps < 1000000) ;
        printf("  peak in the last tap: end %d after %d steps\n", r, d.steps);
        CHECK(r != 0);
    }
    T_REPORT("design");
}
