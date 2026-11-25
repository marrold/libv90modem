/* Tests for the QAM receiver core against the test modulator. */
#include <string.h>
#include "test.h"
#include "../modem/qamrx.h"
#include "../sim/qammod.h"
#include "../sim/noise.h"

#define PRE 120             /* symbols before training (AGC settles on them) */
#define TRAIN 800
#define DD4 2000            /* decision-directed 4-point symbols after training (like Ja) */
#define NMAX 60000

static double txr[NMAX], txi[NMAX];
static int    ntx;
static int    L;            /* data constellation is L x L */
static rng_t  srng;

static double qam_scale(void) { return sqrt(3.0 / ((double)L * L - 1)); }

static void next_sym(void *u, double *re, double *im)
{
    (void)u;
    if (ntx < PRE + TRAIN + DD4) {
        *re = rng_uniform(&srng) < 0.5 ? -1 : 1;
        *im = rng_uniform(&srng) < 0.5 ? -1 : 1;
    } else {
        double a = qam_scale();
        *re = a * (2 * (int)(rng_uniform(&srng) * L) - (L - 1));
        *im = a * (2 * (int)(rng_uniform(&srng) * L) - (L - 1));
    }
    if (ntx < NMAX) { txr[ntx] = *re; txi[ntx] = *im; }
    ntx++;
}

static int32_t slice1(int32_t z, int n)
{
    double a = qam_scale() * QRX_REF;
    int k;
    if (n < TRAIN + DD4) return z < 0 ? -QRX_REF : QRX_REF;
    k = (int)floor((z / a + (L - 1)) / 2.0 + 0.5);
    if (k < 0) k = 0;
    if (k > L - 1) k = L - 1;
    return (int32_t)floor(a * (2 * k - (L - 1)) + 0.5);
}

typedef struct {
    int sym_rate, cnum, cden;
    double ppm, foff, snr_db, tilt;
    int nsym, l;
    double sync_off;        /* extra delay told to qamrx_sync, samples */
} scen_t;

typedef struct {
    double train_mse_db;    /* at the end of training */
    double snr_db;          /* slicer SNR over the last quarter */
    int errors;
    int cen0, cen1;         /* tap centroid after training / at the end */
    double freq_hz;
} res_t;

static void run(const scen_t *s, res_t *o)
{
    static qamrx_t rx; qammod_t mod; rng_t nrng;
    double fc = (double)s->sym_rate * s->cnum / s->cden, sig2 = 0, nrms = 0, z1 = 0;
    double t0, e2 = 0, d2 = 0;
    long i, isync;
    int n, synced = 0;

    L = s->l; ntx = 0;
    rng_seed(&srng, 77); rng_seed(&nrng, 5);
    qammod_init(&mod, s->sym_rate, s->ppm, fc + s->foff, 1500.0, next_sym, 0);
    qamrx_init(&rx, s->sym_rate, s->cnum, s->cden);
    t0 = qammod_delay(&mod) + PRE * mod.sps + (s->tilt != 0 ? 0.3 : 0.0);
    isync = (long)t0 - 150;
    memset(o, 0, sizeof *o);
    o->errors = 0;
    for (i = 0; ; i++) {
        double v = qammod_tick(&mod), y;
        y = v + s->tilt * z1; z1 = v;
        if (i < isync) sig2 += y * y;
        if (i == isync) {
            nrms = s->snr_db > 0 ? sqrt(sig2 / isync) / pow(10.0, s->snr_db / 20.0) : 0.0;
            qamrx_sync(&rx, (int32_t)((t0 - i + s->sync_off) * 256));
            synced = 1;
        }
        y += nrms * rng_gauss(&nrng);
        if (!qamrx_tick(&rx, (int16_t)floor(y + 0.5))) continue;
        if (!synced) continue;
        n = rx.nsym - 1;
        if (n >= s->nsym) break;
        if (n < TRAIN) {
            int32_t dr = (int32_t)(txr[PRE + n] * QRX_REF), di = (int32_t)(txi[PRE + n] * QRX_REF);
            qamrx_update(&rx, dr, di, QRX_EQ | QRX_CARRIER | (n > 300 ? QRX_TIMING : 0), n < 600 ? 10 : 11);
            if (n == TRAIN - 1) {
                o->train_mse_db = 10 * log10(rx.mse / 65536.0);
                o->cen0 = qamrx_centroid(&rx);
            }
        } else {
            int32_t dr = slice1(rx.zr, n), di = slice1(rx.zi, n);
            double wr = txr[PRE + n] * QRX_REF, wi = txi[PRE + n] * QRX_REF;
            if (fabs(dr - wr) > 2 || fabs(di - wi) > 2) o->errors++;
            if (n >= s->nsym - s->nsym / 4) {
                e2 += (rx.zr - wr) * (rx.zr - wr) + (rx.zi - wi) * (rx.zi - wi);
                d2 += wr * wr + wi * wi;
            }
            qamrx_update(&rx, dr, di, QRX_EQ | QRX_CARRIER | QRX_TIMING, n < TRAIN + DD4 ? 11 : 13);
        }
    }
    o->snr_db = 10 * log10(d2 / (e2 + 1e-9));
    o->cen1 = qamrx_centroid(&rx);
    o->freq_hz = (double)rx.freq / 4294967296.0 * s->sym_rate;
}

/* A hostile caller (security pass, I2): after S, audio that is loud or
 * jumps (full-scale noise, a run of -32768, full-scale alternation,
 * silence, 256 ms each) while the receiver trains on known points, as in
 * training (mu 10) and in data mode (QRX_DENSE, mu 13). Before the fix the
 * error, the tap products and the taps overflowed int32 and the mse went
 * negative. Returns the number of symbols with a negative mse or an mse
 * above its clamp (4, 6 dB); *cmax: the largest |tap| at the end. */
static int hostile(int dense, int32_t *cmax)
{
    static qamrx_t rx;
    uint32_t r = 1;
    long i;
    int k, bad = 0;
    qamrx_init(&rx, 3200, 3, 5);
    for (i = 0; i < 400; i++) (void)qamrx_tick(&rx, (int16_t)((i * 7919) % 4000 - 2000));
    qamrx_sync(&rx, 0);
    for (i = 0; i < 8000L * 20; i++) {
        int16_t x;
        r = r * 1664525u + 1013904223u;
        switch ((i >> 11) & 3) {
        case 0: x = (int16_t)(int32_t)(r >> 16) ; break;
        case 1: x = -32768; break;
        case 2: x = (int16_t)((i & 1) ? 32767 : -32768); break;
        default: x = 0; break;
        }
        if (!qamrx_tick(&rx, x)) continue;
        qamrx_update(&rx, (r & 1) ? QRX_REF : -QRX_REF, (r & 2) ? QRX_REF : -QRX_REF,
                     QRX_EQ | QRX_CARRIER | QRX_TIMING | (dense ? QRX_DENSE : 0), dense ? 13 : 10);
        if (rx.mse < 0 || rx.mse > 4 * 65536) bad++;
    }
    *cmax = 0;
    for (k = 0; k < QRX_NTAP; k++) {
        int32_t a = rx.cr[k] < 0 ? -rx.cr[k] : rx.cr[k], b = rx.ci[k] < 0 ? -rx.ci[k] : rx.ci[k];
        if (a > *cmax) *cmax = a;
        if (b > *cmax) *cmax = b;
    }
    return bad;
}

int main(void)
{
    scen_t s; res_t o;

    /* clean line, 3200 symbols/s on the 1920 Hz carrier, 64-QAM data */
    memset(&s, 0, sizeof s);
    s.sym_rate = 3200; s.cnum = 3; s.cden = 5; s.nsym = 8000; s.l = 8;
    run(&s, &o);
    printf("  3200 clean: training mse %.1f dB, slicer SNR %.1f dB, errors %d, centroid %.2f -> %.2f\n",
           o.train_mse_db, o.snr_db, o.errors, o.cen0 / 256.0, o.cen1 / 256.0);
    CHECK(o.train_mse_db < -32.0);
    CHECK(o.snr_db > 44.0);
    CHECK_EQ_I(o.errors, 0);
    CHECK_NEAR(o.cen0 / 256.0, QRX_CENTER, 2.0);        /* taps stay centred */

    /* training converges the same for every sub-sample alignment (regression:
     * two interpolator outputs in one input sample used to shift the equaliser
     * line under the update, for a third of all alignments) */
    {
        int k; double worst = -99;
        s.nsym = 1000;
        for (k = 0; k < 12; k++) {
            s.sync_off = k * 0.07;
            run(&s, &o);
            if (o.train_mse_db > worst) worst = o.train_mse_db;
        }
        printf("  3200 clean: worst training mse over 12 alignments %.1f dB\n", worst);
        CHECK(worst < -38.0);
        s.sync_off = 0; s.nsym = 8000;
    }

    /* 3000 symbols/s on the 2000 Hz carrier */
    s.sym_rate = 3000; s.cnum = 2; s.cden = 3;
    run(&s, &o);
    printf("  3000 clean: training mse %.1f dB, slicer SNR %.1f dB, errors %d\n", o.train_mse_db, o.snr_db, o.errors);
    CHECK(o.train_mse_db < -38.0);
    CHECK(o.snr_db > 44.0);
    CHECK_EQ_I(o.errors, 0);

    /* tilted, noisy line (32 dB): the slicer sees nearly the channel SNR */
    s.sym_rate = 3200; s.cnum = 3; s.cden = 5; s.snr_db = 32.0; s.tilt = -0.4; s.l = 4; s.nsym = 12000;
    run(&s, &o);
    printf("  3200 tilt, 32 dB: slicer SNR %.1f dB, errors %d\n", o.snr_db, o.errors);
    CHECK(o.snr_db > 29.5);
    CHECK_EQ_I(o.errors, 0);

    /* clock and carrier offsets, long run: timing loop holds the taps in place */
    s.snr_db = 36.0; s.tilt = 0; s.l = 8; s.nsym = 50000;
    s.ppm = 100; s.foff = 5.0;
    run(&s, &o);
    printf("  +100 ppm, +5 Hz: slicer SNR %.1f dB, errors %d, centroid %.2f -> %.2f, loop %.2f Hz\n",
           o.snr_db, o.errors, o.cen0 / 256.0, o.cen1 / 256.0, o.freq_hz);
    CHECK(o.snr_db > 35.0);          /* offsets cost nothing measurable */
    CHECK_EQ_I(o.errors, 0);
    CHECK_NEAR(o.cen1 / 256.0, o.cen0 / 256.0, 0.5);
    CHECK_NEAR(o.freq_hz, 5.0, 0.5);
    s.ppm = -100; s.foff = -5.0;
    run(&s, &o);
    printf("  -100 ppm, -5 Hz: slicer SNR %.1f dB, errors %d, centroid %.2f -> %.2f, loop %.2f Hz\n",
           o.snr_db, o.errors, o.cen0 / 256.0, o.cen1 / 256.0, o.freq_hz);
    CHECK(o.snr_db > 35.0);
    CHECK_EQ_I(o.errors, 0);
    CHECK_NEAR(o.cen1 / 256.0, o.cen0 / 256.0, 0.5);
    CHECK_NEAR(o.freq_hz, -5.0, 0.5);

    /* hostile audio after S: no overflow, the mse stays in range, the
     * taps stay within their bound (2^30 and one stretch of updates) */
    {
        int32_t cmax;
        CHECK_EQ_I(hostile(0, &cmax), 0);
        CHECK(cmax <= (1 << 30) + (1 << 28));
        CHECK_EQ_I(hostile(1, &cmax), 0);
        CHECK(cmax <= (1 << 30) + (1 << 28));
    }

    T_REPORT("qamrx");
}
