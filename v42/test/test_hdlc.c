/* HDLC framing tests: FCS vectors from V.42 8.1.1.6, bit-exact transmit
 * patterns, zero insertion and deletion, abort, flag fill, length limits. */
#include <limits.h>
#include <string.h>
#include "test.h"
#include "v42_hdlc.h"

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 16) & 0x7FFF;
}

/* reverse the low n bits of v */
static uint32_t bitrev(uint32_t v, int n)
{
    uint32_t r = 0; int i;
    for (i = 0; i < n; i++) if (v & (1u << i)) r |= 1u << (n - 1 - i);
    return r;
}

/* parse a "0001 1101 ..." bit string, first character is the high-order bit */
static uint32_t from_bits(const char *s)
{
    uint32_t v = 0;
    for (; *s; s++) if (*s == '0' || *s == '1') v = (v << 1) | (uint32_t)(*s - '0');
    return v;
}

static void test_fcs(void)
{
    static const uint8_t msg[] = "123456789";
    uint8_t f[16];
    uint16_t c16; uint32_t c32;

    /* check values of the two polynomials as used by ISO 3309 */
    CHECK_EQ_I((uint16_t)~v42_fcs16(V42_FCS16_INIT, msg, 9), 0x906E);
    CHECK((uint32_t)~v42_fcs32(V42_FCS32_INIT, msg, 9) == 0xCBF43926u);

    /* residues quoted in V.42 8.1.1.6.1 and 8.1.1.6.2 (x^15 / x^31 first);
     * our registers hold the low-order power in the top bit, so reverse */
    CHECK_EQ_I(bitrev(V42_FCS16_GOOD, 16), from_bits("0001 1101 0000 1111"));
    CHECK(bitrev(V42_FCS32_GOOD, 32) ==
          from_bits("1100 0111 0000 0100 1101 1101 0111 1011"));

    /* frame plus transmitted FCS leaves the residue */
    memcpy(f, msg, 9);
    c16 = (uint16_t)~v42_fcs16(V42_FCS16_INIT, f, 9);
    f[9] = (uint8_t)c16; f[10] = (uint8_t)(c16 >> 8);
    CHECK_EQ_I(v42_fcs16(V42_FCS16_INIT, f, 11), V42_FCS16_GOOD);
    c32 = ~v42_fcs32(V42_FCS32_INIT, f, 9);
    f[9] = (uint8_t)c32; f[10] = (uint8_t)(c32 >> 8);
    f[11] = (uint8_t)(c32 >> 16); f[12] = (uint8_t)(c32 >> 24);
    CHECK(v42_fcs32(V42_FCS32_INIT, f, 13) == V42_FCS32_GOOD);
    /* incremental computation equals one shot */
    CHECK_EQ_I(v42_fcs16(v42_fcs16(V42_FCS16_INIT, msg, 4), msg + 4, 5),
               v42_fcs16(V42_FCS16_INIT, msg, 9));
}

/* collect n line bits into a '0'/'1' string */
static void pull(v42hdlc_tx_t *t, char *out, int n)
{
    int i;
    for (i = 0; i < n; i++) out[i] = (char)('0' + v42hdlc_tx_bit(t));
    out[n] = 0;
}

static void test_tx_bits(void)
{
    v42hdlc_tx_t t;
    char s[400];
    static const uint8_t sabme[2] = { 0x03, 0x7F };
    static const uint8_t ones[3] = { 0xFF, 0xFF, 0xFF };
    uint16_t fcs;
    char want[400]; int n = 0, i, run;
    uint8_t all[5];

    /* idle line is flag fill: 01111110 repeated (8.1.5) */
    v42hdlc_tx_init(&t);
    pull(&t, s, 24);
    CHECK(strcmp(s, "011111100111111001111110") == 0);
    CHECK(v42hdlc_tx_need_frame(&t));

    /* one frame: flag, address 0x03 lsb first, control 0x7F (six ones in the
     * octet, so a zero is inserted after the fifth), FCS, flag */
    CHECK_EQ_I(v42hdlc_tx_frame(&t, sabme, 2, 0), 0);
    CHECK(!v42hdlc_tx_need_frame(&t));
    CHECK_EQ_I(v42hdlc_tx_frame(&t, sabme, 2, 0), -1);   /* still loaded */
    fcs = (uint16_t)~v42_fcs16(V42_FCS16_INIT, sabme, 2);
    all[0] = 0x03; all[1] = 0x7F; all[2] = (uint8_t)fcs; all[3] = (uint8_t)(fcs >> 8);
    /* the previous flag doubles as opening flag: data starts at once */
    run = 0;
    for (i = 0; i < 32; i++) {
        int b = (all[i / 8] >> (i % 8)) & 1;
        want[n++] = (char)('0' + b);
        if (b) { if (++run == 5) { want[n++] = '0'; run = 0; } } else run = 0;
    }
    memcpy(want + n, "01111110", 8); n += 8; want[n] = 0;
    pull(&t, s, n);
    CHECK(strcmp(s, want) == 0);
    /* the data really did start 1,1,0,0,0,0,0,0 then 1,1,1,1,1,(0),1,1,0 */
    CHECK(strncmp(s, "11000000" "111110110", 17) == 0);
    CHECK(v42hdlc_tx_need_frame(&t));
    CHECK_EQ_I(t.frames, 1);

    /* all-ones payload never shows six ones in a row inside the frame */
    v42hdlc_tx_init(&t);
    v42hdlc_tx_frame(&t, ones, 3, 1);
    pull(&t, s, 8);                                   /* opening flag */
    CHECK(strcmp(s, "01111110") == 0);
    pull(&t, s, 24 + 4);                              /* 24 ones + 4 stuffed */
    CHECK(strcmp(s, "1111101111101111101111101111") == 0);

    /* a demanded flag preamble is honoured before the frame goes out */
    v42hdlc_tx_init(&t);
    v42hdlc_tx_flags(&t, 16);
    v42hdlc_tx_frame(&t, sabme, 2, 0);
    pull(&t, s, 16 * 8);
    for (i = 0; i < 16; i++) CHECK(strncmp(s + 8 * i, "01111110", 8) == 0);
    pull(&t, s, 8);
    CHECK(strcmp(s, "11000000") == 0);
}

/* run tx bits into rx, returning the first non-zero rx event */
static int pump(v42hdlc_tx_t *t, v42hdlc_rx_t *r, int maxbits)
{
    int i, ev;
    for (i = 0; i < maxbits; i++)
        if ((ev = v42hdlc_rx_bit(r, v42hdlc_tx_bit(t))) != 0) return ev;
    return 0;
}

static void test_loopback(void)
{
    v42hdlc_tx_t t; v42hdlc_rx_t r;
    uint8_t f[V42HDLC_MAX_PAYLOAD];
    int iter, i, len, ev, fcs32;

    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    v42hdlc_rx_set_fcs(&r, V42HDLC_FCS_EITHER);
    for (iter = 0; iter < 400; iter++) {
        len = 2 + (int)(rnd() % (V42HDLC_MAX_PAYLOAD - 1));
        for (i = 0; i < len; i++) {
            unsigned k = rnd() % 8;
            f[i] = k < 3 ? 0xFF : k == 3 ? 0x7E : k == 4 ? 0x00 : (uint8_t)rnd();
        }
        fcs32 = (int)(rnd() & 1);
        while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
        CHECK_EQ_I(v42hdlc_tx_frame(&t, f, len, fcs32), 0);
        ev = pump(&t, &r, 20 * V42HDLC_MAX_FRAME);
        if (ev != len || memcmp(r.buf, f, (size_t)len) != 0 || r.last_fcs32 != fcs32) {
            CHECK(ev == len);
            CHECK(r.last_fcs32 == fcs32);
            break;
        }
    }
    CHECK_EQ_I(iter, 400);
    CHECK_EQ_I(r.good, 400);
    CHECK_EQ_I(r.bad_fcs + r.runts + r.aborts + r.too_long + r.misaligned, 0);

    /* strict modes refuse the other FCS */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    for (i = 0; i < 20; i++) f[i] = (uint8_t)(i * 7);
    v42hdlc_tx_frame(&t, f, 20, 1);
    CHECK_EQ_I(pump(&t, &r, 1000), V42HDLC_RX_BAD_FCS);   /* default is FCS-16 */
    v42hdlc_rx_set_fcs(&r, V42HDLC_FCS32);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 20, 0);
    CHECK_EQ_I(pump(&t, &r, 1000), V42HDLC_RX_BAD_FCS);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 20, 1);
    CHECK_EQ_I(pump(&t, &r, 1000), 20);
}

static void test_errors(void)
{
    v42hdlc_tx_t t; v42hdlc_rx_t r;
    uint8_t f[V42HDLC_MAX_PAYLOAD];
    uint8_t line[4000];
    int nb, i, ev, got, bad;
    static const char runt[] = "01111110" "10000000" "01000000" "11000000" "01111110";
    static const char odd[]  = "01111110" "10000000" "01000000" "11000000" "00100000" "101" "01111110";

    for (i = 0; i < 60; i++) f[i] = (uint8_t)(i * 13 + 1);

    /* a single flipped bit anywhere in the frame is caught by the FCS */
    for (bad = 8; bad < 8 + 62 * 8; bad += 37) {
        v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
        v42hdlc_tx_frame(&t, f, 60, 0);
        nb = 0;
        while (nb < 8 || v42hdlc_tx_busy(&t)) line[nb++] = (uint8_t)v42hdlc_tx_bit(&t);
        for (i = 0; i < 16; i++) line[nb++] = (uint8_t)v42hdlc_tx_bit(&t);
        line[bad] ^= 1;
        got = 0;
        for (i = 0; i < nb; i++) { ev = v42hdlc_rx_bit(&r, line[i]); if (ev > 0) got++; }
        CHECK_EQ_I(got, 0);
        CHECK_EQ_I(r.good, 0);
    }

    /* transmitter abort: receiver drops the frame, then recovers */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    v42hdlc_tx_frame(&t, f, 60, 0);
    for (i = 0; i < 100; i++) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_abort(&t);
    CHECK_EQ_I(pump(&t, &r, 100), V42HDLC_RX_ABORT);
    CHECK_EQ_I(t.aborts, 1);
    CHECK_EQ_I(r.aborts, 1);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 60, 0);
    CHECK_EQ_I(pump(&t, &r, 2000), 60);
    CHECK(memcmp(r.buf, f, 60) == 0);

    /* line stuck at mark is not a frame and not repeated aborts */
    v42hdlc_rx_init(&r);
    for (i = 0; i < 16; i++) v42hdlc_rx_bit(&r, "01111110"[i % 8] - '0');
    for (i = 0; i < 500; i++) CHECK(v42hdlc_rx_bit(&r, 1) == 0);
    CHECK_EQ_I(r.aborts, 0);                 /* nothing was in progress */

    /* fewer than four octets between flags (8.1.3 b) */
    v42hdlc_rx_init(&r);
    ev = 0;
    for (i = 0; runt[i]; i++) { int e = v42hdlc_rx_bit(&r, runt[i] - '0'); if (e) ev = e; }
    CHECK_EQ_I(ev, V42HDLC_RX_SHORT);
    CHECK_EQ_I(r.runts, 1);

    /* not an integral number of octets (8.1.3 c) */
    v42hdlc_rx_init(&r);
    ev = 0;
    for (i = 0; odd[i]; i++) { int e = v42hdlc_rx_bit(&r, odd[i] - '0'); if (e) ev = e; }
    CHECK_EQ_I(ev, V42HDLC_RX_ALIGN);
    CHECK_EQ_I(r.misaligned, 1);

    /* maximum frame length enforcement: over the limit is discarded, at the
     * limit is accepted */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    v42hdlc_rx_set_max(&r, 3 + 20 + 2);
    v42hdlc_tx_frame(&t, f, 3 + 21, 0);
    CHECK_EQ_I(pump(&t, &r, 2000), V42HDLC_RX_LONG);
    CHECK_EQ_I(r.too_long, 1);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 3 + 20, 0);
    CHECK_EQ_I(pump(&t, &r, 2000), 3 + 20);
    /* the transmitter refuses frames that cannot fit */
    v42hdlc_tx_init(&t);
    CHECK_EQ_I(v42hdlc_tx_frame(&t, f, V42HDLC_MAX_PAYLOAD + 1, 0), -1);
    CHECK_EQ_I(v42hdlc_tx_frame(&t, f, 0, 0), -1);

    /* consecutive flags are counted (used to spot the protocol phase) */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    for (i = 0; i < 80; i++) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    CHECK_EQ_I(r.flag_run, 10);
    for (i = 0; i < 20; i++) v42hdlc_rx_bit(&r, 1);
    CHECK_EQ_I(r.flag_run, 0);
    /* an idle link sends flags for ever (T403 off): the count saturates,
     * it does not overflow (at 56 kbit/s INT_MAX flags are ~85 hours) */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    for (i = 0; i < 80; i++) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    r.flag_run = INT_MAX - 5;
    for (i = 0; i < 80; i++) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    CHECK(r.flag_run >= 3 && r.flag_run <= INT_MAX - 5);
    v42hdlc_rx_init(&r);
    for (i = 0; i < 8 * 70000; i++) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    CHECK_EQ_I(r.flag_run, 65535);
}

/* A frame with no opening flag after a run of ones (seven or more: an abort,
 * or the line at mark). A U.S. Robotics x2 client (56K FAX EXT, flash 5.1.4)
 * sends this after each of its rate changes: B1, some fifty ones, and then
 * the frame it had begun, the flag that opened it gone, its first bits run
 * into the ones (a live call, 2026-11-21: both of its rate changes, and the
 * caller's idle flags after them in another). The receiver takes it when its
 * FCS holds with k = 0..5 of those ones as its first bits (k = 5: the zero
 * that ends the run is an inserted one). Anything else after the ones is
 * dropped silently, as it always was. */
static int resync_case(const uint8_t *f, int len, int fcs32, int ones)
{
    v42hdlc_tx_t t; v42hdlc_rx_t r;
    int i, ev, got = 0, nb = 0, b;
    static uint8_t line[8 * (V42HDLC_MAX_FRAME + 64)];

    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    v42hdlc_rx_set_fcs(&r, V42HDLC_FCS_EITHER);
    for (i = 0; i < 24; i++) line[nb++] = (uint8_t)v42hdlc_tx_bit(&t);    /* flags */
    for (i = 0; i < ones; i++) line[nb++] = 1;
    v42hdlc_tx_frame(&t, f, len, fcs32);
    v42hdlc_tx_flags(&t, 1);
    for (i = 0; i < 8; i++) (void)v42hdlc_tx_bit(&t);                     /* its opening flag: lost */
    while (v42hdlc_tx_busy(&t)) line[nb++] = (uint8_t)v42hdlc_tx_bit(&t);
    for (i = 0; i < 16; i++) line[nb++] = (uint8_t)v42hdlc_tx_bit(&t);
    for (i = 0; i < nb; i++) {
        b = line[i];
        ev = v42hdlc_rx_bit(&r, b);
        if (ev > 0) {
            got++;
            CHECK_EQ_I(ev, len);
            CHECK(ev == len && memcmp(r.buf, f, (size_t)len) == 0);
            CHECK_EQ_I(r.last_fcs32, fcs32);
        }
    }
    CHECK_EQ_I(r.bad_fcs + r.runts + r.aborts + r.too_long + r.misaligned, 0);
    return got;
}

/* bits from that call around the caller's first rate change, packed least
 * significant first: flags, its I frame N(S) 68, flags; the rate change
 * (its S to its B1, not delivered); 51 ones; N(S) 69 with no opening flag,
 * a flag, N(S) 70, flags */
static const char usr_reneg_hex[] =
    "7e7e7e7e7e7e7e7e7e7e7e7e03881c7d01540431e40cd890f9c83504000083f9"
    "527af5f3f3f3f3f3f3f3f3f3f3ffffffffff7f4091a32fc08ac00df0f1cc400d"
    "70f6ed110df0420560039c752fb401ce799db4063836627e038c1c00c8c77d21"
    "84f8f9fd";
#define USR_RENEG_BITS 794

static void test_resync(void)
{
    uint8_t f[V42HDLC_MAX_PAYLOAD];
    static const uint8_t first[] = { 0x03, 0x01, 0x02, 0x07, 0x0F, 0x1F, 0x3F, 0xFF, 0x80 };
    v42hdlc_rx_t r;
    int i, k, fcs32, ev, n = 0;
    uint8_t want[3] = { 0x88, 0x8A, 0x8C };

    for (i = 0; i < 40; i++) f[i] = (uint8_t)(i * 29 + 5);
    /* any first octet; LAPM's are 0x01 and 0x03 (k = 1, 2) */
    for (fcs32 = 0; fcs32 <= 1; fcs32++)
        for (k = 0; k < (int)sizeof first; k++) {
            f[0] = first[k];
            CHECK_EQ_I(resync_case(f, 3, fcs32, 49), 1);
            CHECK_EQ_I(resync_case(f, 40, fcs32, 51), 1);
            CHECK_EQ_I(resync_case(f, 12, fcs32, 7), 1);
        }

    /* what is not a frame after the ones is dropped and not counted */
    v42hdlc_rx_init(&r);
    for (i = 0; i < 16; i++) v42hdlc_rx_bit(&r, "01111110"[i % 8] - '0');
    for (i = 0; i < 49; i++) CHECK(v42hdlc_rx_bit(&r, 1) == 0);
    for (i = 0; i < 300; i++) {
        int b = (int)(rnd() >> 3) & 1;
        if (i % 5 == 4) b = 0;                      /* no flag or abort inside */
        CHECK(v42hdlc_rx_bit(&r, b) <= 0);
    }
    for (i = 0; i < 16; i++) CHECK(v42hdlc_rx_bit(&r, "01111110"[i % 8] - '0') <= 0);
    CHECK_EQ_I(r.good + r.bad_fcs + r.runts + r.aborts + r.too_long + r.misaligned, 0);

    /* the tail of a frame whose head was lost (a retrain): dropped */
    {
        v42hdlc_tx_t t; int nb2 = 0;
        static uint8_t line[2000];
        for (i = 0; i < 40; i++) f[i] = (uint8_t)(i * 29 + 5);
        f[0] = 0x03;
        v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
        v42hdlc_tx_frame(&t, f, 40, 0);
        while (v42hdlc_tx_busy(&t)) line[nb2++] = (uint8_t)v42hdlc_tx_bit(&t);
        for (i = 0; i < 16; i++) line[nb2++] = (uint8_t)v42hdlc_tx_bit(&t);
        for (i = 0; i < 49; i++) CHECK(v42hdlc_rx_bit(&r, 1) == 0);
        for (k = 24; k < 140; k += 13) {             /* the head cut at several points (two octets or more) */
            int e;
            v42hdlc_rx_init(&r);
            for (i = 0; i < 49; i++) (void)v42hdlc_rx_bit(&r, 1);
            for (i = k; i < nb2; i++) { e = v42hdlc_rx_bit(&r, line[i]); CHECK(e <= 0); }
            CHECK_EQ_I(r.good + r.bad_fcs + r.aborts, 0);
        }
    }

    /* random bits after a run of ones, then a flag: taken as a frame about
     * 0.75 x 2^-16 of the time (FCS-16, at most two alignments); bound loosely */
    {
        long trials, taken = 0, lapm = 0;
        for (trials = 0; trials < 200000; trials++) {
            int len = 40 + (int)(rnd() % 400), ones = 0;
            v42hdlc_rx_init(&r);
            for (i = 0; i < 16; i++) (void)v42hdlc_rx_bit(&r, "01111110"[i % 8] - '0');
            for (i = 0; i < 20; i++) (void)v42hdlc_rx_bit(&r, 1);
            (void)v42hdlc_rx_bit(&r, 0);
            for (i = 0; i < len; i++) {
                int b = (int)(rnd() >> 4) & 1;
                if (ones == 5) b = 0;                   /* no flag or abort inside */
                ones = b ? ones + 1 : 0;
                (void)v42hdlc_rx_bit(&r, b);
            }
            for (i = 0; i < 8; i++) ev = v42hdlc_rx_bit(&r, "01111110"[i] - '0');
            if (ev > 0) { taken++; if (r.buf[0] == 0x01 || r.buf[0] == 0x03) lapm++; }
        }
        printf("  random bits after ones: %ld of %ld taken (%ld with a LAPM address)\n", taken, trials, lapm);
        CHECK(taken * 65536 <= 4 * trials);
        CHECK(lapm * 65536 <= trials);
    }

    /* the caller's own bits: N(S) 68, 69 and 70, in order */
    v42hdlc_rx_init(&r);
    for (i = 0; i < USR_RENEG_BITS; i++) {
        unsigned o = 0; int d;
        for (d = 0; d < 2; d++) {
            char c = usr_reneg_hex[2 * (i / 8) + d];
            o = o * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        ev = v42hdlc_rx_bit(&r, (int)(o >> (i % 8)) & 1);
        if (ev > 0) {
            CHECK(n < 3 && r.buf[0] == 0x03);
            if (n < 3) CHECK_EQ_I(r.buf[1], want[n]);
            n++;
        }
    }
    CHECK_EQ_I(n, 3);
    CHECK_EQ_I(r.bad_fcs + r.runts + r.aborts + r.too_long + r.misaligned, 0);
}

/* random bit noise must never produce a crash or an oversized frame */
static void test_noise(void)
{
    v42hdlc_rx_t r;
    long i; int ev, worst = 0;
    v42hdlc_rx_init(&r);
    v42hdlc_rx_set_fcs(&r, V42HDLC_FCS_EITHER);
    for (i = 0; i < 2000000; i++) {
        ev = v42hdlc_rx_bit(&r, (int)(rnd() >> 7) & 1);
        if (ev > worst) worst = ev;
    }
    CHECK(worst <= V42HDLC_MAX_PAYLOAD);
    CHECK(r.bad_fcs > 100);
    /* FCS-16 lets through about one random frame in 65536 */
    CHECK(r.good * 2000 < r.bad_fcs + 2000);
}

int main(void)
{
    test_fcs();
    test_tx_bits();
    test_loopback();
    test_errors();
    test_resync();
    test_noise();
    T_REPORT("test_hdlc");
}
