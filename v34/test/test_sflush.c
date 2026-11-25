/* S in data mode (a rate renegotiation, a cleardown): v34_rx_s_run finds
 * the run of S at the end of the inputs, v34_rx_flush_before delivers the
 * whole mapping frames that end at least one mapping frame before it, and
 * nothing that S cut or that is S. The far end starts S on any symbol of a
 * mapping frame (0: on its boundary), its first symbols clean or turned
 * about 30 degrees and shrunk (a channel's onset, as a real modem's S came
 * in a recording: 17 to 37 degrees off for the first four); every bit
 * delivered must be right, and no more than the frame S cut and one more
 * may be missing. With no S at the end (data, two points that are not 90
 * degrees apart, or not of one size) nothing is found, so nothing is
 * flushed: what the decoder held is dropped, as before. S just reaching
 * the decoder (2 or 3 symbols of it, too few for the run) is seen coming
 * (v34_rx_s_begins: its owner waits a little); data, or those two kinds of
 * points, are not. */
#include "test.h"
#include "testutil.h"

static v34_params_t p;
static v34_tx_t tx;
static v34_rx_t rx;
static int begins;          /* v34_rx_s_begins() at the end of the last run / no_s */

/* point 0 of S and its turn, near the data's mean power */
static void s_point(int n, int distort, int32_t a, v34_pt_t *o)
{
    double re = a, im = a, c, s, t;
    if (n & 1) { t = re; re = -im; im = t; }        /* turned 90 degrees anticlockwise */
    if (distort) {
        c = 0.85 * cos(0.52); s = 0.85 * sin(0.52);  /* about 30 degrees, 15 % smaller */
        t = re * c - im * s; im = re * s + im * c; re = t;
    }
    o->re = (int16_t)lrint(re); o->im = (int16_t)lrint(im);
}

/* data for `mf` mapping frames and `o` symbols of the next, then `ns` symbols
 * of S (the first `nd` distorted); returns the run found, *lost the bits
 * taken from the source that were not delivered, *errs the wrong ones */
static unsigned run(unsigned S, unsigned R, int trellis, unsigned mf, unsigned o, unsigned ns, unsigned nd,
                    unsigned long *lost, unsigned long *errs)
{
    v34_cfg_t c;
    tu_src_t src;
    tu_sink_t sink;
    unsigned long n;
    unsigned run;
    int32_t a;

    *lost = *errs = 0;
    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R;
    c.trellis = (uint8_t)trellis; c.scrambler = V34_GPA;
    if (v34_params_init(&p, &c)) { CHECK(0); return 0; }
    tu_pair_init(&src, &sink, 0x5EEDu + mf * 8 + o);
    tu_seed = 0x2468ACE1u + o;
    v34_tx_init(&tx, &p, tu_getbit, &src);
    v34_rx_init(&rx, &p, tu_putbit, &sink);
    a = (int32_t)lrint(sqrt(p.energy_q8 / 512.0) * V34_ONE);
    for (n = 0; n < 8ul * mf + o; n++) {
        v34_pt_t x;
        v34_tx_symbol(&tx, &x, NULL);
        v34_rx_put(&rx, tu_add_noise(x, 0.05), NULL);
    }
    for (n = 0; n < ns; n++) {
        v34_pt_t x;
        s_point((int)n, n < nd, a, &x);
        v34_rx_put(&rx, tu_add_noise(x, 0.05), NULL);
    }
    run = v34_rx_s_run(&rx);
    begins = v34_rx_s_begins(&rx);
    if (run > 0) v34_rx_flush_before(&rx, run);
    *lost = src.n - sink.n;
    *errs = sink.errs;
    return run;
}

/* the tail is not S: nothing found */
static unsigned no_s(int kind)
{
    v34_cfg_t c;
    tu_src_t src;
    tu_sink_t sink;
    unsigned long n;
    int32_t a;
    memset(&c, 0, sizeof c);
    c.symbol_rate = 3200; c.data_rate = 28800; c.trellis = V34_TRELLIS_16; c.scrambler = V34_GPA;
    if (v34_params_init(&p, &c)) { CHECK(0); return 1; }
    tu_pair_init(&src, &sink, 0xD47Au + (uint32_t)kind);
    v34_tx_init(&tx, &p, tu_getbit, &src);
    v34_rx_init(&rx, &p, tu_putbit, &sink);
    a = (int32_t)lrint(sqrt(p.energy_q8 / 512.0) * V34_ONE);
    for (n = 0; n < 8ul * 40 + 3; n++) {
        v34_pt_t x;
        v34_tx_symbol(&tx, &x, NULL);
        v34_rx_put(&rx, x, NULL);
    }
    for (n = 0; kind > 0 && n < 12; n++) {
        v34_pt_t x;
        if (kind == 1) {                            /* two points 45 degrees apart */
            x.re = (int16_t)a; x.im = (int16_t)((n & 1) ? 0 : a);
            if (n & 1) x.re = (int16_t)lrint(a * 1.41421356);
        } else {                                    /* 90 degrees, one 1.5 times the other */
            x.re = (int16_t)((n & 1) ? -a * 3 / 2 : a); x.im = (int16_t)((n & 1) ? a * 3 / 2 : a);
        }
        v34_rx_put(&rx, x, NULL);
    }
    begins = v34_rx_s_begins(&rx);
    return v34_rx_s_run(&rx);
}

int main(void)
{
    static const struct { unsigned S, R; int tr; } cf[] = {
        { 3200, 28800, V34_TRELLIS_16 }, { 3000, 26400, V34_TRELLIS_64 }, { 3429, 33600, V34_TRELLIS_32 }
    };
    unsigned k, o, d;

    for (k = 0; k < sizeof cf / sizeof cf[0]; k++) {
        for (d = 0; d < 2; d++) {
            unsigned long worst = 0, bad = 0;
            unsigned rmin = 99, rmax = 0;
            for (o = 0; o < 8; o++) {
                unsigned long lost, errs;
                unsigned r = run(cf[k].S, cf[k].R, cf[k].tr, 40, o, 14, d ? 5 : 0, &lost, &errs);
                /* a mapping frame carries b or b - 1 bits */
                unsigned long frame = p.map.b;
                CHECK(r > 0);
                CHECK_EQ_I(errs, 0);                        /* nothing but data delivered */
                CHECK(lost <= (o ? 3 : 2) * frame);         /* the cut frame, the margin's, one more */
                if (!d && !o) CHECK(lost >= frame - 1 && lost <= frame);   /* aligned, clean: the margin's frame */
                if (lost > worst) worst = lost;
                bad += errs;
                if (r < rmin) rmin = r;
                if (r > rmax) rmax = r;
            }
            printf("  %u sym/s %u bit/s, %d states, S %s: S found %u..%u of 14, at most %lu bits lost (%u a frame), %lu wrong\n",
                   cf[k].S, cf[k].R, 16 << cf[k].tr, d ? "turned 30 deg for 5" : "clean", rmin, rmax, worst,
                   (unsigned)p.map.b, bad);
        }
    }
    {
        unsigned r0, r1, r2; int b0, b1, b2;
        r0 = no_s(0); b0 = begins; r1 = no_s(1); b1 = begins; r2 = no_s(2); b2 = begins;
        printf("  no S at the end: data %u, 45 degrees %u, unequal sizes %u (0: nothing flushed);"
               " S coming %d %d %d\n", r0, r1, r2, b0, b1, b2);
        CHECK_EQ_I(r0, 0); CHECK_EQ_I(r1, 0); CHECK_EQ_I(r2, 0);
        CHECK_EQ_I(b0, 0); CHECK_EQ_I(b1, 0); CHECK_EQ_I(b2, 0);
    }
    for (k = 2; k <= 3; k++) {
        /* S's first k symbols in: no run yet, but S is seen coming */
        unsigned long lost, errs;
        unsigned r = run(3200, 28800, V34_TRELLIS_16, 40, 3, k, 0, &lost, &errs);
        printf("  S %u symbols in: run %u, S coming %d\n", k, r, begins);
        CHECK_EQ_I(r, 0); CHECK_EQ_I(begins, 1);
    }
    T_REPORT("test_sflush");
}
