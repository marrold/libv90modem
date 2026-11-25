/* Tests for the V.22bis receiver: transmitter -> impairments -> receiver. */
#include <string.h>
#include "test.h"
#include "../modem/v22bis.h"
#include "../sim/g711.h"
#include "../sim/noise.h"
#include "../sim/resamp.h"
#include "../sim/filt.h"

#define FS 8000
#define NBITS 12000

static uint8_t txbits[NBITS + 64], rxbits[NBITS * 2];
static int ntx, nrx;
static uint32_t lfsr;
static int getbit(void *u)
{
    int b;
    (void)u;
    lfsr = lfsr * 1664525u + 1013904223u;
    b = (lfsr >> 24) & 1;
    if (ntx < NBITS) txbits[ntx++] = (uint8_t)b;
    return b;
}
static void putbit(void *u, int b) { (void)u; if (nrx < NBITS * 2) rxbits[nrx++] = (uint8_t)b; }

typedef struct {
    int fc, tx_fc;        /* receiver carrier, transmitter carrier (offset test) */
    int level;            /* tx RMS */
    int mode16;
    double noise_dbfs;    /* <= -200: none */
    double ppm;
    int ulaw;
    int isi;
} cond;

/* returns bit errors over the compared span (after settling); *compared = span */
static int run(const cond *c, int *compared, v22rx_t *rxout)
{
    static const double taps[5] = {0.08, 1.0, 0.25, -0.12, 0.05};
    double fz[5];
    v22tx_t tx; v22rx_t rx; rng_t rng; sresamp_t rs; fir_t fir;
    int n, k, best = 1 << 30, off, total = FS * 8, tdata;
    double in[1], out[4];

    lfsr = 777; ntx = nrx = 0;
    v22tx_init(&tx, c->tx_fc, c->level, getbit, NULL);
    v22rx_init(&rx, c->fc, putbit, NULL);
    rng_seed(&rng, 31337);
    sresamp_init(&rs, ppm_ratio(c->ppm));
    fir_init(&fir, taps, 5, fz);
    v22tx_set_mode(&tx, V22_TX_SB1_1200);
    tdata = FS;                                  /* 1 s of scrambled ones, then data */
    for (n = 0; n < total; n++) {
        int no;
        if (n == tdata) {
            v22tx_set_mode(&tx, c->mode16 ? V22_TX_DATA_2400 : V22_TX_DATA_1200);
            rx.mode16 = c->mode16; rx.data_on = 1;
        }
        in[0] = v22tx_tick(&tx);
        if (c->isi) in[0] = fir_push(&fir, in[0]);
        no = sresamp_push(&rs, in[0], out, 4);
        for (k = 0; k < no; k++) {
            double v = out[k];
            int16_t s;
            if (c->noise_dbfs > -200.0) awgn_add(&rng, &v, 1, c->noise_dbfs, 32124.0);
            s = (int16_t)lround(v);
            if (c->ulaw) s = (int16_t)ulaw_to_linear(linear_to_ulaw(s));
            v22rx_tick(&rx, s);
        }
    }
    if (rxout) *rxout = rx;
    /* the rx stream lags the tx stream by the modem delay; skip the mode-switch
     * transient (first 600 received bits), find the lag, count errors */
    *compared = 0;
    for (off = 0; off < 400; off++) {
        int err = 0, cnt = 0, j;
        for (j = 600; j + off < nrx && j < ntx; j++, cnt++)
            if (rxbits[j + off] != txbits[j]) err++;
        /* tx bits start at the mode switch; rx bits started being delivered then
         * too, so the lag in bits is small and positive */
        if (cnt > 2000 && err < best) { best = err; *compared = cnt; }
    }
    return best;
}

/* a full-scale click into a receiver whose gain was set high on a weak
 * carrier, early in the data (48 < ndata <= 600): the carrier loop's and the
 * equaliser's products overflow 32 bits (an undefined behaviour that
 * check-ubsan traps; 64-bit products give the same bits on every target) */
static void test_click(void)
{
    static const int lvl[3] = {180, 220, 250}, at[4] = {60, 100, 200, 400};
    int li, ai, mode16, runs = 0;
    for (li = 0; li < 3; li++)
    for (ai = 0; ai < 4; ai++)
    for (mode16 = 0; mode16 < 2; mode16++) {
        v22tx_t tx; v22rx_t rx; int n, clicked = 0;
        lfsr = 99; ntx = nrx = 0;
        v22tx_init(&tx, V22_FC_LOW, lvl[li], getbit, NULL);
        v22rx_init(&rx, V22_FC_LOW, putbit, NULL);
        v22tx_set_mode(&tx, V22_TX_SB1_1200);
        for (n = 0; n < 8000; n++) {
            int16_t v = v22tx_tick(&tx);
            if (n == 2400) { v22tx_set_mode(&tx, mode16 ? V22_TX_DATA_2400 : V22_TX_DATA_1200); rx.mode16 = mode16; rx.data_on = 1; }
            if (!clicked && rx.ndata == (uint32_t)at[ai]) { v = 32767; clicked = 1; }
            v22rx_tick(&rx, v);
        }
        runs += clicked;
    }
    CHECK_EQ_I(runs, 24);
}

int main(void)
{
    cond c; int cmp, err; v22rx_t rx;

    test_click();

    /* 1. clean, 1200 bit/s, low channel */
    memset(&c, 0, sizeof c);
    c.fc = c.tx_fc = V22_FC_LOW; c.level = 3000; c.noise_dbfs = -300;
    err = run(&c, &cmp, &rx);
    CHECK(cmp > 6000); CHECK_EQ_I(err, 0);
    CHECK(rx.carrier);
    CHECK_NEAR(v22rx_level(&rx), 3000, 450);

    /* 2. clean, 2400 bit/s, both channels */
    c.mode16 = 1;
    err = run(&c, &cmp, &rx);
    CHECK(cmp > 12000 - 1200); CHECK_EQ_I(err, 0);
    printf("  v22rx clean 2400: mse %d\n", (int)rx.mse);
    /* the timing loop, not the equaliser, must have found the sampling instant:
     * on a clean channel the equaliser stays close to its centre spike. (A
     * timing detector with the wrong sign passes every BER test, because the
     * T/2 equaliser quietly interpolates around it.) */
    {
        double centre = hypot((double)rx.wr[V22_EQN / 2], (double)rx.wi[V22_EQN / 2]), rest = 0;
        int k;
        for (k = 0; k < V22_EQN; k++) if (k != V22_EQN / 2) rest += hypot((double)rx.wr[k], (double)rx.wi[k]);
        printf("  v22rx equaliser: centre %.3f, sum of other taps %.3f\n", centre / 16777216.0, rest / 16777216.0);
        CHECK(centre > 0.7 * 16777216.0);
        CHECK(rest < 0.6 * 16777216.0);       /* wrong sign gives about 1.5 */
    }
    c.fc = c.tx_fc = V22_FC_HIGH;
    err = run(&c, &cmp, NULL);
    CHECK_EQ_I(err, 0);

    /* 3. weak signal (-33 dB), mu-law quantised */
    c.fc = c.tx_fc = V22_FC_LOW; c.level = 250; c.ulaw = 1;
    err = run(&c, &cmp, &rx);
    CHECK(cmp > 10000); CHECK_EQ_I(err, 0);
    c.level = 3000;

    /* 4. +7 Hz and -7 Hz carrier offset (V.22bis 2.6) */
    c.tx_fc = V22_FC_LOW + 7; err = run(&c, &cmp, &rx); CHECK(cmp > 10000); CHECK_EQ_I(err, 0);
    printf("  v22rx +7 Hz: loop freq %d\n", (int)rx.freq);
    c.tx_fc = V22_FC_LOW - 7; err = run(&c, &cmp, NULL); CHECK_EQ_I(err, 0);
    c.tx_fc = V22_FC_LOW;

    /* 5. sample clock offset +/- 200 ppm */
    c.ppm = 200; err = run(&c, &cmp, &rx); CHECK(cmp > 10000); CHECK_EQ_I(err, 0);
    printf("  v22rx +200 ppm: timing integrator %d\n", (int)rx.t_integ);
    c.ppm = -200; err = run(&c, &cmp, NULL); CHECK_EQ_I(err, 0);
    c.ppm = 0;

    /* 6. linear distortion (needs the equaliser) */
    c.isi = 1; err = run(&c, &cmp, &rx); CHECK(cmp > 10000); CHECK_EQ_I(err, 0);
    printf("  v22rx ISI: mse %d\n", (int)rx.mse);

    /* 7. everything at once plus noise 28 dB below the signal */
    c.level = 1500; c.tx_fc = V22_FC_LOW + 5; c.ppm = 150;
    c.noise_dbfs = 20.0 * log10(1500.0 * sqrt(2.0) / 32124.0) - 28.0;
    err = run(&c, &cmp, &rx);
    printf("  v22rx combined 2400: %d errors / %d bits, mse %d\n", err, cmp, (int)rx.mse);
    CHECK(cmp > 10000); CHECK(err <= 2);

    /* 8. 1200 bit/s at 12 dB SNR still clean enough (4 points are robust) */
    c.mode16 = 0; c.isi = 0; c.ppm = 0; c.tx_fc = V22_FC_LOW; c.level = 1500;
    c.noise_dbfs = 20.0 * log10(1500.0 * sqrt(2.0) / 32124.0) - 12.0;
    err = run(&c, &cmp, NULL);
    printf("  v22rx 1200 @ 12 dB SNR: %d errors / %d bits\n", err, cmp);
    CHECK(err * 200 < cmp);                        /* < 0.5 % */

    /* 9. signal classification */
    {
        v22tx_t tx; int i, mode;
        static const v22_txmode modes[3] = {V22_TX_USB1, V22_TX_S1, V22_TX_SB1_1200};
        static const v22_sig want[3] = {V22_SIG_USB1, V22_SIG_S1, V22_SIG_OTHER};
        for (mode = 0; mode < 3; mode++) {
            int agree = 0;
            v22tx_init(&tx, V22_FC_HIGH, 1000, getbit, NULL);
            v22rx_init(&rx, V22_FC_HIGH, putbit, NULL);
            v22tx_set_mode(&tx, modes[mode]);
            for (i = 0; i < 4000; i++) {
                v22rx_tick(&rx, v22tx_tick(&tx));
                if (i >= 800 && rx.sig == want[mode]) agree++;
            }
            CHECK(agree >= 3190);
        }
        /* silence, then low-level noise: no carrier */
        v22rx_init(&rx, V22_FC_HIGH, putbit, NULL);
        { rng_t g; int on = 0; rng_seed(&g, 5);
          for (i = 0; i < 8000; i++) { double v = 0.0; awgn_add(&g, &v, 1, -60.0, 32124.0); v22rx_tick(&rx, (int16_t)lround(v)); on |= rx.carrier; }
          CHECK_EQ_I(on, 0); CHECK_EQ_I(rx.sig, V22_SIG_NONE); }
        /* the other channel at full level is not a carrier for us */
        v22tx_init(&tx, V22_FC_LOW, 5000, getbit, NULL);
        v22tx_set_mode(&tx, V22_TX_SB1_1200);
        v22rx_init(&rx, V22_FC_HIGH, putbit, NULL);
        { int on = 0; for (i = 0; i < 8000; i++) { v22rx_tick(&rx, v22tx_tick(&tx)); on |= rx.carrier; } CHECK_EQ_I(on, 0); }
    }

    T_REPORT("v22rx");
}
