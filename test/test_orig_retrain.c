/* V.90 and V.92 on the calling side: retrains and rate renegotiations, the
 * library's caller (modem/orig, modem/v90a) against the library's answerer
 * (modem/answer) over test/v90link.h's line (2 km, the echo at -16 dB, the
 * upstream as test_orig_v90 / test_orig_v92 have it).
 *
 * Each case: the call to data mode, 2 s of data, then the event; the time
 * from the event until data moves both ways again, the rates before and
 * after, then 5 s of data both ways that must pass the check (2^23 - 1,
 * self-synchronising: a wrong bit fails it three times) without a failure.
 *   the answerer retrains (answer_retrain: 70 ms of silence, Tone B, 9.5.1.1
 *     V.90, 9.7.1.1 V.92): we follow (9.5.2.2 / 9.7.2.2)
 *   we retrain (orig_retrain, 9.5.2.1 / 9.7.2.1)
 *   the line changes under the call: noise 15 dB up (a rate renegotiation
 *     down is enough; when the noise goes again, back up), the level 10 dB
 *     down, a 6 dB pad put in (the DIL's levels no longer hold: a retrain)
 *   our rate renegotiation (9.6.2.1 / 9.8.2.1) down with a ceiling, and back
 *     up without it; the answerer's (9.6.1.1 / 9.8.1.1) as before
 *   the receiver collapses (its channel estimate and equaliser wiped; and a
 *     burst of loud noise): a retrain brings it back
 *   the answerer's engine (p34d / v92d) retrains of its own accord, not
 *     through answer_retrain(): V.92 must come back with PCM upstream
 *   the answerer retrains with INFO0d in its Phase 2 (a digital modem that
 *     reads 9.7/V.92's "full Phase 2" so): we answer with INFO0a
 * Then the limits: three retrains in a minute are allowed, a fourth refused,
 * and the receiver lost after them clears the call down (ORIG_END_RETRAIN,
 * the answerer told); a retrain that the answerer never answers ends the call
 * (ORIG_END_RETRAIN) with our transmitter silent. Host: everything; TEST_SHORT (32-bit ARM under
 * qemu): V.90's cases and the limits. */
#include <stdio.h>
#include <inttypes.h>
#include "test.h"
#include "v90link.h"
/* the old cleared flag, from the end reason: 1 the caller's cleardown, 2 ours */
static int cleared_of(const answer_t *a)
{
    int e = answer_end(a);
    return e == ANS_END_CLEARDOWN_FAR ? 1 : e == ANS_END_CLEARDOWN_OURS ? 2 : 0;
}

#define MS(x) ((int64_t)(x) * 8)

static vl_t L;

enum { K_ANS_RETRAIN, K_OUR_RETRAIN, K_NOISE, K_LEVEL, K_PAD, K_RENEG, K_ANS_RENEG, K_WIPE, K_BURST, K_ANS_OWN, K_ANS_INFO0, K_N };
static const char *const kname[K_N] = {
    "the answerer retrains", "we retrain", "noise +15 dB", "level -10 dB", "a 6 dB pad put in",
    "we renegotiate, ceiling 40000", "the answerer renegotiates", "receiver wiped", "noise burst 300 ms",
    "the answerer's own retrain", "answerer retrains with INFO0d"
};

typedef struct {
    int ok;                     /* data both ways again, and still at the end */
    int down0, up0, down1, up1; /* rates before and after */
    int32_t back_ms;            /* from the event to data both ways again; -1 never */
    int64_t bd, fd, bu, fu;     /* 5 s after it: bits and check failures */
    int rt_o, rt_a, rn_o;       /* retrains (as we count them, as the answerer does), renegotiations */
    int rt_ours, mod0, mod1;    /* retrains we began; the modulation before and after */
    int info0a;                 /* K_ANS_INFO0: the answerer's Phase 2 got our INFO0a */
    int down2, fd2, fu2;        /* K_NOISE, K_RENEG: after the second step */
    int32_t back2_ms;
} res_t;

static void line(vl_cfg *c, int v92, uint32_t seed)
{
    memset(c, 0, sizeof *c);
    c->loop_km = 2; c->noise_dbm0 = -75; c->ppm = 20; c->phase = 0.17 * (seed % 5);
    c->echo_db = -16; c->up_loss_db = 6; c->up_noise_dbm0 = -70; c->full = 1; c->caller_orig = 1; c->seed = seed;
    if (v92) { c->v92 = 1; c->caller_v92 = 1; c->up_line = 1; }
}

/* the receiver's channel estimate and equaliser gone: decisions on nothing
 * (and the next symbol's sums its last steps formed from them) */
static void wipe(vl_t *l)
{
    v90arx_t *r = &l->org.e.v90.rx;
    memset(r->h, 0, sizeof r->h);
    memset(r->f, 0, sizeof r->f);
    memset(r->b, 0, sizeof r->b);
    r->hnext_m0 = -1; r->eqnext_m = -1;
}

static int both(vl_t *l) { return orig_moving(&l->org) && answer_moving(&l->ans); }
static void run_ms(vl_t *l, int ms) { int64_t e = l->k + MS(ms); while (l->k < e) vl_tick(l); }

/* the level of everything the far codec sends, at our A/D, by db */
static void line_gain(vl_t *l, double db)
{
    double g = pow(10, db / 20);
    int i;
    for (i = 0; i < (int)(sizeof l->ch.pulse / sizeof l->ch.pulse[0]); i++) l->ch.pulse[i] *= g;
}

/* after an event at t0: wait until data has stopped (a retrain or a
 * renegotiation; or not at all) and moves both ways again; ms, or -1 */
static int32_t back(vl_t *l, int64_t t0, int limit_ms)
{
    int stopped = 0;
    while (l->k < t0 + MS(limit_ms) && l->org.phase != ORIG_FAILED) {
        vl_tick(l);
        if (!both(l)) stopped = 1;
        else if (stopped) return (int32_t)((l->k - t0) / 8);
    }
    return -1;
}

/* still connected at the end, data moving (or about to: the receiver's watch
 * may take the rate back up after a retrain for the line) */
static int still(vl_t *l)
{
    int64_t e = l->k + MS(10000);
    while (l->k < e && !both(l) && l->org.phase != ORIG_FAILED) vl_tick(l);
    return both(l);
}

/* 300 ms to settle, then 5 s of data counted */
static void count(vl_t *l, int64_t *bd, int64_t *fd, int64_t *bu, int64_t *fu)
{
    int64_t d0, u0, f0d, f0u;
    run_ms(l, 300);
    d0 = l->down.got; u0 = l->up.got; f0d = l->down.fails; f0u = l->up.fails;
    { int64_t e = l->k + MS(5000); while (l->k < e && l->org.phase != ORIG_FAILED) vl_tick(l); }
    *bd = l->down.got - d0; *bu = l->up.got - u0; *fd = l->down.fails - f0d; *fu = l->up.fails - f0u;
}

static int connect_w(vl_t *l, int v92, uint32_t seed, int words)
{
    vl_cfg c;
    line(&c, v92, seed);
    vl_init(l, &c);
    if (words) { l->words = 1; l->wdown.gen = (uint16_t)(0x5a00 + 77 * seed); l->wup.gen = (uint16_t)(0x2c00 + 59 * seed); }
    while (l->k < MS(40000) && !both(l) && l->org.phase != ORIG_FAILED) vl_tick(l);
    if (!both(l)) return 0;
    run_ms(l, 2000);
    return both(l);
}
static int connect(vl_t *l, int v92, uint32_t seed) { return connect_w(l, v92, seed, 0); }

static void run(int v92, int kind, uint32_t seed, res_t *o)
{
    vl_t *l = &L;
    int64_t t0;
    memset(o, 0, sizeof *o);
    o->back_ms = o->back2_ms = -1;
    if (!connect(l, v92, seed)) return;
    o->down0 = l->org.rx_bitrate; o->up0 = l->org.bitrate; o->mod0 = l->org.modulation;
    t0 = l->k;
    switch (kind) {
    case K_ANS_RETRAIN: CHECK_EQ_I(answer_retrain(&l->ans), 0); break;
    case K_OUR_RETRAIN: CHECK_EQ_I(orig_retrain(&l->org), 0); break;
    case K_NOISE: l->ch.nrms *= pow(10, 15 / 20.0); break;
    case K_LEVEL: line_gain(l, -10); break;
    case K_PAD: l->ch.cfg.pad_db = 6; break;
    case K_RENEG: CHECK_EQ_I(orig_renegotiate(&l->org, 40000), 0); break;
    case K_ANS_RENEG:
        if (v92) CHECK_EQ_I(v92d_renegotiate(&l->ans.u.v92), 0);
        else CHECK_EQ_I(p34d_renegotiate(&l->ans.p34), 0);
        break;
    case K_WIPE:
        wipe(l);
        break;
    case K_ANS_OWN:
        /* the answerer's engine retrains of its own accord (its checks of
         * the upstream, a renegotiation that stalls), not through
         * answer_retrain() */
        if (v92) CHECK_EQ_I(v92d_retrain(&l->ans.u.v92), 0);
        else CHECK_EQ_I(p34d_retrain(&l->ans.p34), 0);
        break;
    case K_ANS_INFO0:
        /* a digital modem whose retrain is the whole of Phase 2 from INFO0d
         * on (ours sends no INFO0, 9.7.1.1 read as 9.5.1.1/V.90; a server
         * may read "the full Phase 2" otherwise): its Phase 2 restarted as
         * at the start of the call, with INFO0d */
        CHECK_EQ_I(answer_retrain(&l->ans), 0);
        while (l->ans.phase != ANS_P2 && l->k < t0 + MS(1000)) vl_tick(l);
        CHECK_EQ_I(l->ans.phase, ANS_P2);
        p2d_init(&l->ans.p2, &l->ans.p2cfg);
        if (v92) p2d_offer_v92(&l->ans.p2, l->ans.pcm_up);
        break;
    case K_BURST: {
        double n0 = l->ch.nrms;
        l->ch.nrms = l->ch.tone_rms * pow(10, -10 / 20.0);   /* -10 dBm0 */
        run_ms(l, 300);
        l->ch.nrms = n0;
        break;
    }
    default: break;
    }
    o->back_ms = back(l, t0, 30000);
    o->down1 = l->org.rx_bitrate; o->up1 = l->org.bitrate; o->mod1 = l->org.modulation;
    o->info0a = l->ans.p2.got0a;
    count(l, &o->bd, &o->fd, &o->bu, &o->fu);
    o->ok = still(l);
    if (kind == K_NOISE || kind == K_RENEG) {
        /* the second step: the noise goes again / no ceiling */
        t0 = l->k;
        if (kind == K_NOISE) l->ch.nrms /= pow(10, 15 / 20.0);
        else CHECK_EQ_I(orig_renegotiate(&l->org, 56000), 0);
        o->back2_ms = back(l, t0, 40000);
        o->down2 = l->org.rx_bitrate;
        { int64_t a, b; count(l, &a, &b, &a, &a); o->fd2 = (int)b; }
        o->ok = o->ok && still(l);
    }
    o->rt_o = orig_retrains(&l->org); o->rt_a = l->ans.retrains; o->rn_o = l->org.e.v90.renegs;
    o->rt_ours = l->org.e.v90.retrains_ours;
}

static void print(const char *name, const res_t *o)
{
    printf("  %-30s %5d %6d/%5d %6d/%5d%s %d %d %d %d %9" PRId64 " %4" PRId64 " %9" PRId64 " %4" PRId64, name, (int)o->back_ms,
           o->down0, o->up0, o->down1, o->up1, o->mod1 != o->mod0 ? (o->mod1 == ORIG_MOD_V90 ? " V.90" : " V.92") : "     ",
           o->rt_o, o->rt_ours, o->rt_a, o->rn_o, o->bd, o->fd, o->bu, o->fu);
    if (o->down2) printf("  then %d ms %d, fail %d", (int)o->back2_ms, o->down2, o->fd2);
    printf("\n");
}

static void cases(int v92)
{
    int k;
    printf("%s: case, ms back, down/up before, after (the modulation if it changed), retrains (ours, of them ours, the\n"
           "  answerer's), renegotiations; 5 s: bits down, fail, bits up, fail\n",
           v92 ? "V.92" : "V.90");
    for (k = 0; k < K_N; k++) {
        res_t o;
        run(v92, k, (uint32_t)(60 + 10 * v92 + k), &o);
        print(kname[k], &o);
        CHECK(o.ok);
        CHECK(o.back_ms > 0);
        CHECK(o.bd > (int64_t)o.down1 * 4);
        CHECK(o.bu > (int64_t)o.up1 * 4);
        CHECK_EQ_I(o.fd, 0);
        CHECK_EQ_I(o.fu, 0);
        CHECK_EQ_I(o.mod1, o.mod0);                     /* V.92 stays V.92 */
        switch (k) {
        case K_ANS_INFO0:
            /* the answerer heard our INFO0a in the retrain's Phase 2 */
            CHECK_EQ_I(o.info0a, 1);
            CHECK_EQ_I(o.rt_o, 1); CHECK_EQ_I(o.rt_a, 1); CHECK_EQ_I(o.rt_ours, 0);
            CHECK(o.back_ms < 12000);
            break;
        case K_ANS_RETRAIN: case K_OUR_RETRAIN: case K_ANS_OWN:
            CHECK_EQ_I(o.rt_o, 1); CHECK_EQ_I(o.rt_a, 1);
            CHECK_EQ_I(o.rt_ours, k == K_OUR_RETRAIN);
            CHECK(o.down1 >= o.down0 - 4000);
            CHECK(o.back_ms < 12000);
            break;
        case K_NOISE:
            CHECK(o.down1 < o.down0);                   /* a lower rate */
            CHECK(o.down2 > o.down1);                   /* and back up */
            CHECK(o.back2_ms > 0);
            CHECK_EQ_I(o.fd2, 0);
            break;
        case K_LEVEL: case K_PAD: case K_WIPE: case K_BURST:
            CHECK(o.rt_ours >= 1);                      /* ours: our receiver saw it */
            break;
        case K_RENEG:
            CHECK(o.down1 <= 40000 && o.down1 < o.down0);
            CHECK(o.down2 > 40000);
            CHECK_EQ_I(o.fd2, 0);
            CHECK_EQ_I(o.rt_o, 0);
            break;
        case K_ANS_RENEG:
            CHECK_EQ_I(o.rt_o, 0); CHECK_EQ_I(o.rt_a, 0);
            break;
        default: break;
        }
    }
}

/* the answerer's rate renegotiation, no V.42: the data both ways is counter
 * words. Downstream none is lost or repeated, and no bit, at its Rd
 * (9.6.1.1 / 9.8.1.1: the frames before Rd are data, every one of them).
 * Upstream, across V.90's S: we send the mapping frame under way whole, and
 * the answerer delivers the frames its decoder still held but one, the
 * margin against an S found late (v34_rx_flush_before): exactly one mapping
 * frame is lost, nothing repeated or wrong. V.92's Ru (on a data frame
 * boundary, 9.8.2.1.1) cuts off what the caller's prefilter still held
 * (about dly samples: those frames never reach the far end), and the
 * answerer keeps a data frame of margin too: whole data frames are lost,
 * no more than dly / 12 + 3 of them, nothing wrong */
static void words_reneg(int v92)
{
    vl_t *l = &L;
    int64_t t0, w0, lost0, brk0, u0, ulost0, ubrk0, ugap0, gap, K = 0, dly = 0, frame = 0;
    int32_t ms;
    CHECK(connect_w(l, v92, 80 + v92, 1));
    /* the upstream's frame as it was when the data stopped */
    if (v92) { K = l->ans.u.v92.cpd.map.K; dly = l->ans.u.v92.dsn.dly; }
    else frame = (int64_t)l->ans.p34.up_rate * 8 / l->ans.p34.cfg.sym_rate;
    w0 = l->wdown.words; lost0 = l->wdown.lost; brk0 = l->wdown.breaks;
    u0 = l->wup.words; ulost0 = l->wup.lost; ubrk0 = l->wup.breaks; ugap0 = l->wup.gap_bits;
    t0 = l->k;
    if (v92) CHECK_EQ_I(v92d_renegotiate(&l->ans.u.v92), 0);
    else CHECK_EQ_I(p34d_renegotiate(&l->ans.p34), 0);
    ms = back(l, t0, 30000);
    run_ms(l, 2000);
    gap = l->wup.gap_bits - ugap0;
    printf("%s: the answerer renegotiates, counter words: back %d ms, words before %" PRId64 ", after %" PRId64 ", lost %" PRId64 "\n",
           v92 ? "V.92" : "V.90", (int)ms, w0, l->wdown.words - w0, l->wdown.lost - lost0);
    printf("  upstream: words before %" PRId64 ", after %" PRId64 ", lost %" PRId64 ", again %" PRId64 "; breaks down %" PRId64 ", up %" PRId64 " (%" PRId64 " bits missing)\n",
           u0, l->wup.words - u0, l->wup.lost - ulost0, l->wup.again, l->wdown.breaks - brk0, l->wup.breaks - ubrk0, gap);
    CHECK(ms > 0);
    CHECK(w0 > 1000);
    CHECK(l->wdown.words - w0 > 1000);
    CHECK_EQ_I(l->wdown.lost - lost0, 0);              /* (one frame, 21..42 bits, was) */
    CHECK_EQ_I(l->wdown.breaks - brk0, 0);
    CHECK_EQ_I(l->wdown.locked, 1);
    CHECK(u0 > 1000);
    CHECK(l->wup.words - u0 > 1000);
    CHECK_EQ_I(l->wup.again, 0);
    CHECK_EQ_I(l->wup.breaks - ubrk0, 1);
    if (!v92) {
        /* one mapping frame (432 bits, 6 frames, were) */
        CHECK(gap >= frame - 1 && gap <= frame);
        CHECK(16 * (l->wup.lost - ulost0) >= gap && 16 * (l->wup.lost - ulost0) < gap + 32);
    } else {
        /* whole data frames, within the bound (672 bits, 14 frames, were) */
        printf("  V.92: %" PRId64 " data frames of %d bits lost, the bound %d (dly %d)\n", gap / K, (int)K,
               (int)(dly / V92_DFRAME + 3), (int)dly);
        CHECK(gap > 0 && gap % K == 0);
        CHECK(gap / K <= dly / V92_DFRAME + 3);
    }
    CHECK_EQ_I(l->wup.locked, 1);
}

/* three retrains in a minute, the fourth refused; one never answered */
static void limits(int v92)
{
    vl_t *l = &L;
    int i;
    int64_t t0;
    printf("%s: limits\n", v92 ? "V.92" : "V.90");
    CHECK(connect(l, v92, 90 + v92));
    for (i = 0; i < 3; i++) {
        int32_t ms;
        t0 = l->k;
        CHECK_EQ_I(orig_retrain(&l->org), 0);
        ms = back(l, t0, 20000);
        printf("  retrain %d: %d ms, down %d\n", i + 1, (int)ms, l->org.rx_bitrate);
        CHECK(ms > 0);
        run_ms(l, 500);
    }
    CHECK_EQ_I(orig_retrain(&l->org), -1);
    run_ms(l, 1000);
    CHECK(both(l));
    CHECK_EQ_I(orig_retrains(&l->org), 3);
    /* past the limit the receiver lost is a cleardown (9.7 / 9.11), not a retrain */
    t0 = l->k;
    wipe(l);
    while (l->k < t0 + MS(10000) && l->org.phase != ORIG_FAILED) vl_tick(l);
    run_ms(l, 500);
    printf("  lost past the limit: end %d after %d ms, answerer cleared %d, retrains %d\n", l->org.end,
           (int)((l->k - t0) / 8) - 500, cleared_of(&l->ans), orig_retrains(&l->org));
    CHECK_EQ_I(l->org.end, ORIG_END_RETRAIN);
    CHECK_EQ_I(answer_end(&l->ans), ANS_END_CLEARDOWN_FAR);
    CHECK_EQ_I(orig_retrains(&l->org), 3);

    CHECK(connect(l, v92, 95 + v92));
    t0 = l->k;
    CHECK_EQ_I(orig_retrain(&l->org), 0);
    l->dmute = 1;                       /* the answerer is gone */
    while (l->k < t0 + MS(40000) && l->org.phase != ORIG_FAILED) vl_tick(l);
    printf("  never answered: phase %d, end %d after %d ms, retrains %d\n", l->org.phase, l->org.end,
           (int)((l->k - t0) / 8), orig_retrains(&l->org));
    CHECK_EQ_I(l->org.phase, ORIG_FAILED);
    CHECK_EQ_I(l->org.end, ORIG_END_RETRAIN);
    CHECK_EQ_I(l->org.connected, 0);
    run_ms(l, 100);
    CHECK_EQ_I(l->last_tx, 0);
}

int main(void)
{
    cases(0);
#ifndef TEST_SHORT
    cases(1);
#endif
    words_reneg(0);
#ifndef TEST_SHORT
    words_reneg(1);
#endif
    limits(0);
#ifndef TEST_SHORT
    limits(1);
#endif
    T_REPORT("orig_retrain");
}
