/* Tests for V.23 FSK: modulator output frequencies and loopback
 * (answering side transmits 1300/2100 Hz at 1200 baud, receives 390/450 Hz
 * at 75 baud; calling side is the reverse). */
#include "test.h"
#include "../modem/v21.h"
#include "../modem/async.h"
#include "../sim/g711.h"

#define FS 8000
#define MAXS 20000

static int16_t samp[MAXS];
static uint8_t bits[1200];

static double goertzel(const int16_t *x, int n, double f, int fs)
{
    double w = 2.0 * 3.14159265358979323846 * f / fs;
    double c = 2.0 * cos(w), s1 = 0, s2 = 0, s0;
    int i;
    for (i = 0; i < n; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* Frame bytes as start/stop characters, modulate at the given baud, then
 * demodulate per sample into the async receiver, exactly as tick_v23 does.
 * Returns the number of characters recovered into out. */
static int arx_roundtrip(int mark, int space, int baud, const uint8_t *msg, int nmsg, uint8_t *out)
{
    static uint8_t frame[2048];
    static int16_t wav[MAXS];
    v21mod_t mod; v21demod_t dem; arx_t rx;
    int nb = 0, ns, j, k, got = 0;

    for (j = 0; j < 16; j++) frame[nb++] = 1;            /* idle mark: let the demod settle */
    for (k = 0; k < nmsg; k++) {
        frame[nb++] = 0;                                 /* start bit (space) */
        for (j = 0; j < 8; j++) frame[nb++] = (msg[k] >> j) & 1;  /* 8 data, LSB first */
        frame[nb++] = 1;                                 /* stop bit (mark) */
    }
    for (j = 0; j < 16; j++) frame[nb++] = 1;            /* trailing mark */

    v21mod_init(&mod, mark, space, FS, baud, 8192);
    ns = v21mod_run(&mod, frame, nb, wav, MAXS);
    v21demod_init(&dem, mark, space, FS, baud);
    arx_init(&rx, FS, baud);
    for (j = 0; j < ns; j++) {
        int c = arx_tick(&rx, v21demod_tick(&dem, wav[j]));
        if (c >= 0 && got < nmsg) out[got++] = (uint8_t)c;
    }
    return got;
}

int main(void)
{
    v21mod_t m;
    int i, ns;

    /* ---- V.23 forward channel (1200 baud, answering side transmits) ---- */

    /* all-ones -> mark freq 1300 Hz dominates */
    v21mod_init(&m, V23H_MARK, V23H_SPACE, FS, 1200, 8192);
    for (i = 0; i < 1200; i++) bits[i] = 1;
    ns = v21mod_run(&m, bits, 1200, samp, MAXS);
    CHECK_NEAR((double)ns, 1200.0 * FS / 1200.0, 3.0);  /* ~8000 samples */
    {
        double pm = goertzel(samp, ns, V23H_MARK, FS);
        double ps = goertzel(samp, ns, V23H_SPACE, FS);
        CHECK(pm > ps * 50.0);
    }

    /* all-zeros -> space freq 2100 Hz dominates */
    v21mod_init(&m, V23H_MARK, V23H_SPACE, FS, 1200, 8192);
    for (i = 0; i < 1200; i++) bits[i] = 0;
    ns = v21mod_run(&m, bits, 1200, samp, MAXS);
    {
        double pm = goertzel(samp, ns, V23H_MARK, FS);
        double ps = goertzel(samp, ns, V23H_SPACE, FS);
        CHECK(ps > pm * 50.0);
    }

    /* ---- V.23 backward channel (75 baud, calling side transmits) ---- */

    /* all-ones -> mark freq 390 Hz dominates */
    v21mod_init(&m, V23L_MARK, V23L_SPACE, FS, 75, 8192);
    for (i = 0; i < 75; i++) bits[i] = 1;
    ns = v21mod_run(&m, bits, 75, samp, MAXS);
    CHECK_NEAR((double)ns, 75.0 * FS / 75.0, 3.0);      /* ~8000 samples */
    {
        double pm = goertzel(samp, ns, V23L_MARK, FS);
        double ps = goertzel(samp, ns, V23L_SPACE, FS);
        CHECK(pm > ps * 2.0);   /* 2.3:1 expected; channels close at 75 baud window */
    }

    /* all-zeros -> space freq 450 Hz dominates */
    v21mod_init(&m, V23L_MARK, V23L_SPACE, FS, 75, 8192);
    for (i = 0; i < 75; i++) bits[i] = 0;
    ns = v21mod_run(&m, bits, 75, samp, MAXS);
    {
        double pm = goertzel(samp, ns, V23L_MARK, FS);
        double ps = goertzel(samp, ns, V23L_SPACE, FS);
        CHECK(ps > pm * 2.0);
    }

    /* ---- forward channel loopback: mod -> demod, 1200 baud ---- */
    {
        static uint8_t tx[160], rx[400];
        static int16_t wav[MAXS];
        v21mod_t mod; v21demod_t dem;
        uint32_t s = 7;
        int n, nbits, best_err, off;

        for (i = 0; i < 16; i++) tx[i] = 1;
        for (i = 16; i < 144; i++) { s = s * 1103515245u + 12345u; tx[i] = (s >> 20) & 1u; }
        nbits = 144;

        v21mod_init(&mod, V23H_MARK, V23H_SPACE, FS, 1200, 8192);
        ns = v21mod_run(&mod, tx, nbits, wav, MAXS);
        v21demod_init(&dem, V23H_MARK, V23H_SPACE, FS, 1200);
        n = v21demod_run(&dem, wav, ns, rx, 400);
        CHECK_NEAR((double)n, (double)nbits, 3.0);

        best_err = 1 << 30;
        for (off = 0; off <= 4; off++) {
            int err = 0, j;
            for (j = 20; j < nbits - 2; j++)
                if (off + j < n && rx[off + j] != tx[j]) err++;
            if (err < best_err) best_err = err;
        }
        CHECK_EQ_I(best_err, 0);
    }

    /* ---- backward channel loopback: mod -> demod, 75 baud ---- */
    {
        static uint8_t tx[80], rx[200];
        static int16_t wav[MAXS];
        v21mod_t mod; v21demod_t dem;
        uint32_t s = 42;
        int n, nbits, best_err, off;

        for (i = 0; i < 8; i++) tx[i] = 1;
        for (i = 8; i < 72; i++) { s = s * 1103515245u + 12345u; tx[i] = (s >> 20) & 1u; }
        nbits = 72;

        v21mod_init(&mod, V23L_MARK, V23L_SPACE, FS, 75, 8192);
        ns = v21mod_run(&mod, tx, nbits, wav, MAXS);
        v21demod_init(&dem, V23L_MARK, V23L_SPACE, FS, 75);
        n = v21demod_run(&dem, wav, ns, rx, 200);
        CHECK_NEAR((double)n, (double)nbits, 3.0);

        best_err = 1 << 30;
        for (off = 0; off <= 4; off++) {
            int err = 0, j;
            for (j = 10; j < nbits - 2; j++)
                if (off + j < n && rx[off + j] != tx[j]) err++;
            if (err < best_err) best_err = err;
        }
        CHECK_EQ_I(best_err, 0);
    }

    /* ---- forward channel loopback through mu-law PCM ---- */
    {
        static uint8_t tx[160], rx[400];
        static int16_t wav[MAXS];
        v21mod_t mod; v21demod_t dem;
        uint32_t s = 99;
        int n, nbits, best_err, off, j;

        for (i = 0; i < 16; i++) tx[i] = 1;
        for (i = 16; i < 144; i++) { s = s * 1103515245u + 12345u; tx[i] = (s >> 20) & 1u; }
        nbits = 144;

        v21mod_init(&mod, V23H_MARK, V23H_SPACE, FS, 1200, 8192);
        ns = v21mod_run(&mod, tx, nbits, wav, MAXS);
        for (j = 0; j < ns; j++) wav[j] = (int16_t)ulaw_to_linear(linear_to_ulaw(wav[j]));

        v21demod_init(&dem, V23H_MARK, V23H_SPACE, FS, 1200);
        n = v21demod_run(&dem, wav, ns, rx, 400);
        best_err = 1 << 30;
        for (off = 0; off <= 4; off++) {
            int err = 0;
            for (j = 20; j < nbits - 2; j++)
                if (off + j < n && rx[off + j] != tx[j]) err++;
            if (err < best_err) best_err = err;
        }
        CHECK_EQ_I(best_err, 0);
    }

    /* ---- async character framing round-trip (the UART path in tick_v23) ---- */
    {
        static const uint8_t msg[] = { 0x55, 0xAA, 0x00, 0xFF, 'A', 'z', 0x7E, 0x81 };
        static uint8_t out[32];
        int n, k;

        /* forward channel, 1200 baud (as the answering side receives) */
        n = arx_roundtrip(V23H_MARK, V23H_SPACE, 1200, msg, (int)sizeof msg, out);
        CHECK_EQ_I(n, (int)sizeof msg);
        for (k = 0; k < n; k++) CHECK_EQ_I(out[k], msg[k]);

        /* backward channel, 75 baud (as the calling side receives) */
        n = arx_roundtrip(V23L_MARK, V23L_SPACE, 75, msg, (int)sizeof msg, out);
        CHECK_EQ_I(n, (int)sizeof msg);
        for (k = 0; k < n; k++) CHECK_EQ_I(out[k], msg[k]);
    }

    T_REPORT("v23");
}
