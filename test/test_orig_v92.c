/* V.92 on the calling side, end to end: the library's caller (modem/orig
 * with V.90 and V.92 offered: V.8, then modem/v90a with PCM upstream)
 * against the library's answerer (modem/answer with V.90 and V.92 on) over
 * test/v90link.h's line: the downstream lines of C5 (sim/dschan: loop, clock
 * offset, pads, robbed bits, law, noise, our hybrid echo at -16 dB, as in
 * test_orig_v90); upstream our clock resampled on to the network's, the
 * line and codec response the answerer's own V.92 tests use (sim/v92chan:
 * the short line up to 2 km, the long one beyond), 6 dB loss, noise, the
 * network codec. Per line: the connect rates down / up, the time from the
 * start of the call to data mode, and 10 s of data both ways (2^23 - 1,
 * self-synchronising check: a wrong bit fails it three times). Then the
 * V.90 fallback (the answerer without V.92; our V.92 off), cleardown from
 * either end, and, reported only, the upstream through the downstream's own
 * loop response (whose low frequency tail outlasts the answerer's channel
 * estimate, see docs/history/PROGRESS.md). */
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

typedef struct {
    const char *name;
    double km, ppm, noise, pad; int rbs, alaw;
    int min_down;               /* checked: at least this downstream (a regression floor two steps
                                   under what the line gives today) */
} row_t;

typedef struct { int ok, mod, down, up, snr, rtd; int32_t t_data; int64_t bd, bu, fd, fu; } res_t;

static void run(const row_t *r, uint32_t seed, int ans_v92, int our_v92, int up_line, res_t *o)
{
    vl_cfg c;
    vl_t *l = &L;
    int64_t d0, u0, f0d, f0u;
    memset(o, 0, sizeof *o);
    memset(&c, 0, sizeof c);
    c.loop_km = r->km; c.noise_dbm0 = r->noise; c.ppm = r->ppm; c.phase = 0.17 * (seed % 5); c.pad_db = r->pad;
    c.rbs = r->rbs; c.rbs_phase = (int)(seed % 6); c.alaw = r->alaw; c.echo_db = -16;
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.up_line = up_line; c.full = 1; c.caller_orig = 1; c.seed = seed;
    c.v92 = ans_v92; c.caller_v92 = our_v92;
    vl_init(l, &c);
    while (l->k < MS(40000) && !orig_moving(&l->org) && l->org.phase != ORIG_FAILED) vl_tick(l);
    if (!orig_moving(&l->org)) return;
    o->t_data = (int32_t)(l->k / 8);
    o->mod = l->org.modulation;
    o->down = l->org.rx_bitrate; o->up = l->org.bitrate;
    o->rtd = orig_rtd(&l->org);
    vl_tick(l);
    while (l->k < MS(40000) + MS(1000) && !answer_moving(&l->ans)) vl_tick(l);
    { int64_t e = l->k + MS(300); while (l->k < e) vl_tick(l); }
    d0 = l->down.got; u0 = l->up.got; f0d = l->down.fails; f0u = l->up.fails;
    { int64_t e = l->k + MS(10000); while (l->k < e && l->org.phase != ORIG_FAILED) vl_tick(l); }
    o->snr = orig_snr_db10(&l->org);
    if (o->snr < 0) o->snr = 0;
    o->bd = l->down.got - d0; o->bu = l->up.got - u0;
    o->fd = l->down.fails - f0d; o->fu = l->up.fails - f0u;
    o->ok = orig_moving(&l->org) && answer_moving(&l->ans);
}

static void print(const char *name, const res_t *o)
{
    printf("  %-38s %4s %7d %6d %6d %3d.%d %5d  %9" PRId64 " %5" PRId64 "  %9" PRId64 " %5" PRId64 "\n", name,
           o->mod == ORIG_MOD_V92 ? "V.92" : o->mod == ORIG_MOD_V90 ? "V.90" : "-", (int)o->t_data, o->down, o->up,
           o->snr / 10, o->snr % 10, o->rtd, o->bd, o->fd, o->bu, o->fu);
}

/* the cases (test/test.h, t_case_arg): 1 .. NR each line, then the two V.90
 * fallbacks, the line reported only, and the two cleardowns */
int main(int argc, char **argv)
{
    static const row_t R[] = {
        { "2 km,    0 ppm, -75 dBm0",            2,    0, -75, 0, 0, 0, 49333 },
        { "0 km, +100 ppm, -75 dBm0",            0,  100, -75, 0, 0, 0, 36000 },
        { "6 km, -100 ppm, -75 dBm0",            6, -100, -75, 0, 0, 0, 41333 },
        { "4 km,  +50 ppm, -65 dBm0",            4,   50, -65, 0, 0, 0, 37333 },
        { "2 km,  +30 ppm, 3 dB pad",            2,   30, -75, 3, 0, 0, 42666 },   /* 44000 .. 48000 with Te
                                   alone (docs/history/PROGRESS.md, robust 2a, 2b) */
        { "2 km,  -30 ppm, 6 dB pad",            2,  -30, -75, 6, 0, 0, 40000 },
        { "2 km,  +30 ppm, robbed bit",          2,   30, -75, 0, 1, 0, 41333 },
        { "2 km,  +30 ppm, 6 dB pad, robbed bit", 2,  30, -75, 6, 1, 0, 37333 },
        { "2 km,  -50 ppm, A-law",               2,  -50, -75, 0, 0, 1, 44000 },
    };
    enum { NR = sizeof R / sizeof R[0] };
    enum { NCASES = NR + 5 };
    static res_t O[NR];
    res_t o;
    int i;
    const int only = t_case_arg(argc, argv, NCASES);

    for (i = 0; i < NR; i++) {
        res_t *p = &O[i];
        if (!T_CASE(only, i + 1)) continue;
        if (i == 0) {
            printf("the library's caller (V.8, V.92 analogue) against the library's answerer, 10 s of data per line\n");
            printf("  %-38s %4s %7s %6s %6s %6s %5s  %9s %5s  %9s %5s\n", "line", "mod", "data ms", "down", "up", "SNR dB", "rtd",
                   "bits down", "fail", "bits up", "fail");
        }
        run(&R[i], (uint32_t)(20 + i), 1, 1, 1, p);
        print(R[i].name, p);
        CHECK(p->ok);
        CHECK_EQ_I(p->mod, ORIG_MOD_V92);
        CHECK(p->down >= R[i].min_down);
        CHECK(p->up >= 24000);
        CHECK(p->bd > (int64_t)p->down * 9);
        CHECK(p->bu > (int64_t)p->up * 9);
        CHECK_EQ_I(p->fd, 0);
        CHECK_EQ_I(p->fu, 0);
        CHECK(p->t_data < 20000);
        CHECK(p->rtd > 0);
    }

    if (T_CASE(only, NR + 1)) {
        printf("V.90 when V.92 is not on both sides\n");
        run(&R[0], 40, 0, 1, 1, &o);
        print("the answerer without V.92", &o);
        CHECK(o.ok);
        CHECK_EQ_I(o.mod, ORIG_MOD_V90);
        CHECK(o.up <= 33600);
        CHECK_EQ_I(o.fd, 0);
        CHECK_EQ_I(o.fu, 0);
    }
    if (T_CASE(only, NR + 2)) {
        run(&R[0], 41, 1, 0, 1, &o);
        print("ours with V.92 off", &o);
        CHECK(o.ok);
        CHECK_EQ_I(o.mod, ORIG_MOD_V90);
        CHECK_EQ_I(o.fd, 0);
        CHECK_EQ_I(o.fu, 0);
    }

    if (T_CASE(only, NR + 3)) {
        printf("reported only: the upstream through the downstream's loop and codec response\n");
        run(&R[0], 42, 1, 1, 2, &o);
        print("2 km,    0 ppm, dschan's response up", &o);
    }

    if (T_CASE(only, NR + 4) || T_CASE(only, NR + 5)) {
        vl_cfg c;
        vl_t *l = &L;
        int64_t t0;
        memset(&c, 0, sizeof c);
        c.loop_km = 2; c.noise_dbm0 = -75; c.ppm = 20; c.phase = 0.4; c.echo_db = -16;
        c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.up_line = 1; c.full = 1; c.caller_orig = 1; c.seed = 7;
        c.v92 = 1; c.caller_v92 = 1;
        if (T_CASE(only, NR + 4)) {
            printf("ending a call\n");
            vl_init(l, &c);
            while (l->k < MS(30000) && !(orig_moving(&l->org) && answer_moving(&l->ans))) vl_tick(l);
            CHECK_EQ_I(l->org.modulation, ORIG_MOD_V92);
            CHECK_EQ_I(l->ans.modulation, ANS_MOD_V92);
            CHECK_EQ_I(orig_retrains(&l->org), 0);      /* (retrains: test_orig_retrain) */
            CHECK(orig_snr_db10(&l->org) > 400);
            t0 = l->k;
            CHECK_EQ_I(orig_cleardown(&l->org), 0);
            while (l->k < t0 + MS(4000) && l->org.phase != ORIG_FAILED) vl_tick(l);
            { int64_t e = l->k + MS(500); while (l->k < e) vl_tick(l); }
            printf("  ours: caller ended %d after %d ms (end %d), answerer phase %d, cleared %d\n", l->org.phase,
                   (int)((l->k - t0) / 8), l->org.end, l->ans.phase, cleared_of(&l->ans));
            CHECK_EQ_I(l->org.end, ORIG_END_HANGUP);
            CHECK_EQ_I(answer_end(&l->ans), ANS_END_CLEARDOWN_FAR);
        }

        if (T_CASE(only, NR + 5)) {
            c.seed = 8;
            vl_init(l, &c);
            while (l->k < MS(30000) && !(orig_moving(&l->org) && answer_moving(&l->ans))) vl_tick(l);
            t0 = l->k;
            CHECK_EQ_I(answer_cleardown(&l->ans), 0);
            while (l->k < t0 + MS(12000) && l->org.phase != ORIG_FAILED) vl_tick(l);
            printf("  the answerer's: caller ended after %d ms, end %d\n", (int)((l->k - t0) / 8), l->org.end);
            CHECK_EQ_I(l->org.phase, ORIG_FAILED);
            CHECK_EQ_I(l->org.end, ORIG_END_CLEARDOWN);
        }
    }
    T_REPORT("orig_v92");
}
