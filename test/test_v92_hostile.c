/* The V.92 calling modem's upstream encoder against a digital modem's CPd
 * that a sane one would not send. The upstream starts B1u and data as soon
 * as a CPd has acknowledged ours, while the downstream stays in TRN2d until
 * Ed and goes on decoding CPds (modem/v90a_up92.c, modem/v92aseq.c). A CPd
 * with a valid CRC may still carry a modulus Mi of 0, a constellation index
 * past the six sets, or trellis 3.
 *
 * A V.92 call over test/v90link.h is run to data mode, the upstream
 * encoder running (P_DATA). Then, in each case:
 *   - the stored CPd's map is written as such a CPd would leave it
 *     (Mi = 0, index 15, trellis 3), under the running encoder;
 *   - such a CPd is sent through the downstream's sequence path
 *     (v90a__train92, as a TRN2d frame), and so is a valid one.
 * The upstream must be what it is without either: the transmit samples of
 * the next second hashed against an undisturbed run of the same call. A
 * CPd that fails the checks must be refused (V92AS_BAD, the stored CPd
 * unchanged); a valid one is taken. */
#include <stdio.h>
#include <inttypes.h>
#include "test.h"
#include "v90link.h"
#include "../modem/v90a_priv.h"
#include "v90_seq.h"

#define T_MS(x) ((int64_t)(x) * 8)

static vl_t L;
static uint8_t cpd_bits[24000];

static vl_cfg base(void)
{
    vl_cfg c;
    memset(&c, 0, sizeof c);
    c.loop_km = 2; c.noise_dbm0 = -75; c.ppm = 30; c.phase = 0.3; c.echo_db = -10;
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.up_line = 1; c.seed = 1;
    c.v92 = 1; c.caller_v92 = 1;
    c.d_retrain = 1;
    return c;
}

static void run_to(vl_t *l, int64_t t_max, v90a_state want_state)
{
    while (l->k < t_max && l->va.state != want_state && l->va.state != V90A_ENDED) vl_tick(l);
}

static void run_for(vl_t *l, int64_t n) { int64_t e = l->k + n; while (l->k < e && l->va.state != V90A_ENDED) vl_tick(l); }

enum { C_NONE, C_MAP_M0, C_MAP_IDX, C_MAP_TRELLIS, C_CPD_M0, C_CPD_IDX, C_CPD_TRELLIS, C_CPD_VALID, C_N };
static const char *const names[C_N] = {
    "no change", "stored map: M0 = 0", "stored map: index 15", "stored map: trellis 3",
    "CPd: M0 = 0", "CPd: index 15", "CPd: trellis 3", "CPd: valid (the one in use again)"
};

/* the info bit b of word w of a sequence built at unit 1 (17 bits of frame
 * sync, then a start bit and 16 bits a word) */
static uint8_t *info_bit(int w, int b) { return &cpd_bits[17 + 17 * w + 1 + b]; }

/* v92_cpd_build of c, then the fields it would refuse written into the
 * bits, and the CRC again; n bits, padded with zeros to a multiple of d */
static int hostile_cpd(const v92_cpd_t *c, int how, int d)
{
    int n = v92_cpd_build(c, 1, cpd_bits), words, w, b;
    int ncoef = c->filt.lz1 + c->filt.lp1 + c->filt.lz2 + c->filt.lp2;
    uint16_t crc = 0xFFFF;
    if (n < 0) return n;
    words = (n - 18) / 17 - 1;                  /* the info words; the CRC word follows */
    switch (how) {
    case C_CPD_IDX:     /* cidx[0] = 15: the first index word, after M (6) and the lengths (4) */
        for (b = 0; b < 4; b++) *info_bit(2 + 6 + 4 + ncoef, b) = 1;
        break;
    case C_CPD_TRELLIS: /* header bits 27:28 */
        *info_bit(0, 9) = 1; *info_bit(0, 10) = 1;
        break;
    default: break;
    }
    for (w = 0; w < words; w++)
        for (b = 0; b < 16; b++) crc = v90_crc_step(crc, *info_bit(w, b));
    for (b = 0; b < 16; b++) *info_bit(words, b) = (uint8_t)((crc >> b) & 1);
    while (n % d) cpd_bits[n++] = 0;
    return n;
}

/* one call to data mode, the case applied, then the transmit samples of the
 * next second hashed; *ev: the sequence events of the CPd sent */
static uint32_t run_case(int how, int *ev, int *same_cpd)
{
    vl_cfg c = base();
    vl_t *l = &L;
    uint32_t h = 2166136261u;
    int64_t k;
    *ev = 0; *same_cpd = 1;
    vl_init(l, &c);
    run_to(l, T_MS(40000), V90A_DATA);
    run_for(l, T_MS(300));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(l->va.us, P_DATA);
    switch (how) {
    case C_MAP_M0:      l->va.cpd.map.M[0] = 0; break;
    case C_MAP_IDX:     l->va.cpd.map.cidx[0] = 15; break;
    case C_MAP_TRELLIS: l->va.cpd.map.trellis = 3; break;
    case C_CPD_M0: case C_CPD_IDX: case C_CPD_TRELLIS: case C_CPD_VALID: {
        static v92_cpd_t want, before;
        int d = l->va.dec.D, n, f;
        want = l->va.cpd;
        before = l->va.cpd;
        want.ack = 1;
        if (how == C_CPD_M0) want.map.M[0] = 0;
        n = hostile_cpd(&want, how, d);
        CHECK(n > 0);
        for (f = 0; f + d <= n; f += d) {
            int got_cpd = l->va.got_cpd;
            l->va.got_cpd = 0;              /* (so that the frame's event is seen) */
            v90a__train92(&l->va, cpd_bits + f, d, 0);
            if (l->va.got_cpd) *ev |= V92AS_CPD;
            *ev |= l->va.aseq.ev & V92AS_BAD;
            l->va.got_cpd = got_cpd | l->va.got_cpd;
        }
        if (how == C_CPD_VALID) before.ack = 1;
        *same_cpd = memcmp(&before, &l->va.cpd, sizeof before) == 0;
        break;
    }
    default: break;
    }
    for (k = 0; k < T_MS(1000) && l->va.state != V90A_ENDED; k++) {
        vl_tick(l);
        h = (h ^ (uint16_t)l->last_tx) * 16777619u;
    }
    CHECK_EQ_I(l->va.state, V90A_DATA);
    return h;
}

int main(int argc, char **argv)
{
    uint32_t ref = 0;
    int i, only = argc > 1 ? atoi(argv[1]) : -1;    /* one case alone (after the undisturbed run) */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_v92_hostile: a CPd a sane digital modem would not send, under a running upstream encoder\n");
    for (i = 0; i < C_N; i++) {
        int ev, same;
        uint32_t h;
        if (only >= 0 && i != 0 && i != only) continue;
        h = run_case(i, &ev, &same);
        if (i == C_NONE) ref = h;
        printf("  %-34s: upstream %s", names[i], h == ref ? "as without it" : "CHANGED");
        if (i >= C_CPD_M0) printf("; CPd %s, stored CPd %s", (ev & V92AS_CPD) ? "taken" : (ev & V92AS_BAD) ? "refused" : "not seen",
                                  same ? "unchanged" : "changed");
        printf("\n");
        CHECK_EQ_I(h, ref);
        if (i >= C_CPD_M0 && i < C_CPD_VALID) { CHECK_EQ_I(ev & V92AS_CPD, 0); CHECK(ev & V92AS_BAD); CHECK(same); }
        if (i == C_CPD_VALID) { CHECK(ev & V92AS_CPD); CHECK(same); }
    }
    T_REPORT("test_v92_hostile");
}
