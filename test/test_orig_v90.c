/* V.90 on the calling side, end to end: the library's caller (modem/orig with
 * V.90 offered: V.8, then modem/v90a) against the library's answerer
 * (modem/answer with V.90 on) over test/v90link.h's line (the downstream
 * through sim/dschan: loop, clock offset, pads, robbed bits, law, noise, our
 * hybrid echo at -16 dB (at -10 dB V.8's JM is lost on some lines: see
 * docs/history/PROGRESS.md, C5-2); the upstream resampled to the network clock, 6 dB
 * loss, noise, the network codec). Per line: the connect rates down / up,
 * the time from the start of the call to data mode, and 10 s of data both
 * ways (2^23 - 1, self-synchronising check: a wrong bit fails it three
 * times). The rows marked "Table 1" take Table 1's levels instead of what
 * the DIL measured (a test setting): what the DIL analysis is for. Then
 * cleardown from either end (retrains and our renegotiation:
 * test_orig_retrain). */
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
    double km, ppm, noise, pad; int rbs, alaw, table1;
    int min_down;               /* checked: at least this downstream (0: report only); a regression
                                   floor a step or two under what the line gives today */
} row_t;

typedef struct { int ok, down, up, snr, rtd; int32_t t_data; int64_t bd, bu, fd, fu; int robbed; } res_t;   /* t_data in ms */

static void run(const row_t *r, uint32_t seed, res_t *o)
{
    vl_cfg c;
    vl_t *l = &L;
    int64_t d0, u0, f0d, f0u;
    memset(o, 0, sizeof *o);
    memset(&c, 0, sizeof c);
    c.loop_km = r->km; c.noise_dbm0 = r->noise; c.ppm = r->ppm; c.phase = 0.17 * (seed % 5); c.pad_db = r->pad;
    c.rbs = r->rbs; c.rbs_phase = (int)(seed % 6); c.alaw = r->alaw; c.echo_db = -16;
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.full = 1; c.caller_orig = 1; c.seed = seed;
    vl_init(l, &c);
    l->org.cfg.v90_table1 = r->table1;
    while (l->k < MS(40000) && !orig_moving(&l->org) && l->org.phase != ORIG_FAILED) vl_tick(l);
    if (!orig_moving(&l->org)) return;
    o->t_data = (int32_t)(l->k / 8);
    o->down = l->org.rx_bitrate; o->up = l->org.bitrate;
    o->rtd = orig_rtd(&l->org);
    o->robbed = l->org.e.v90.res.robbed[0] | l->org.e.v90.res.robbed[1] << 1 | l->org.e.v90.res.robbed[2] << 2
              | l->org.e.v90.res.robbed[3] << 3 | l->org.e.v90.res.robbed[4] << 4 | l->org.e.v90.res.robbed[5] << 5;
    vl_tick(l);
    while (l->k < MS(40000) + MS(1000) && !answer_moving(&l->ans)) vl_tick(l);
    { int64_t e = l->k + MS(300); while (l->k < e) vl_tick(l); }     /* the first bits of each direction settle in */
    d0 = l->down.got; u0 = l->up.got; f0d = l->down.fails; f0u = l->up.fails;
    { int64_t e = l->k + MS(10000); while (l->k < e && l->org.phase != ORIG_FAILED) vl_tick(l); }
    o->snr = orig_snr_db10(&l->org);
    if (o->snr < 0) o->snr = 0;
    o->bd = l->down.got - d0; o->bu = l->up.got - u0;
    o->fd = l->down.fails - f0d; o->fu = l->up.fails - f0u;
    o->ok = orig_moving(&l->org) && answer_moving(&l->ans);
}

int main(void)
{
    static const row_t R[] = {
        { "2 km,    0 ppm, -75 dBm0",            2,    0, -75, 0, 0, 0, 0, 48000 },
        { "0 km, +100 ppm, -75 dBm0",            0,  100, -75, 0, 0, 0, 0, 32000 },     /* the band edge (C5-1) */
        { "6 km, -100 ppm, -75 dBm0",            6, -100, -75, 0, 0, 0, 0, 44000 },
        { "4 km,  +50 ppm, -65 dBm0",            4,   50, -65, 0, 0, 0, 0, 38666 },
        { "2 km,  +30 ppm, 3 dB pad",            2,   30, -75, 3, 0, 0, 0, 41333 },
        { "  ... Table 1 levels",                2,   30, -75, 3, 0, 0, 1, 0 },
        { "2 km,  -30 ppm, 6 dB pad",            2,  -30, -75, 6, 0, 0, 0, 38666 },
        { "  ... Table 1 levels",                2,  -30, -75, 6, 0, 0, 1, 0 },
        { "2 km,  +30 ppm, robbed bit",          2,   30, -75, 0, 1, 0, 0, 44000 },
        { "  ... Table 1 levels",                2,   30, -75, 0, 1, 0, 1, 0 },
        { "2 km,  +30 ppm, 6 dB pad, robbed bit", 2,  30, -75, 6, 1, 0, 0, 37333 },   /* 38666 .. 45333 with Te
                                   alone (29333 .. 44000 before robust 2b; docs/history/PROGRESS.md) */
        { "2 km,  -50 ppm, A-law",               2,  -50, -75, 0, 0, 1, 0, 42666 },
    };
    enum { NR = sizeof R / sizeof R[0] };
    static res_t O[NR];
    int i;

    printf("the library's caller (V.8, V.90 analogue) against the library's answerer, 10 s of data per line\n");
    printf("  %-38s %7s %6s %6s %6s %5s  %9s %5s  %9s %5s  %s\n", "line", "data ms", "down", "up", "SNR dB", "rtd",
           "bits down", "fail", "bits up", "fail", "robbed");
    for (i = 0; i < NR; i++) {
        res_t *o = &O[i];
        run(&R[i], (uint32_t)(20 + i), o);
        printf("  %-38s %7d %6d %6d %3d.%d %5d  %9" PRId64 " %5" PRId64 "  %9" PRId64 " %5" PRId64 "  %d%d%d%d%d%d\n", R[i].name,
               (int)o->t_data, o->down, o->up, o->snr / 10, o->snr % 10, o->rtd, o->bd, o->fd, o->bu, o->fu,
               o->robbed & 1, (o->robbed >> 1) & 1, (o->robbed >> 2) & 1, (o->robbed >> 3) & 1, (o->robbed >> 4) & 1, (o->robbed >> 5) & 1);
        if (R[i].min_down) {
            CHECK(o->ok);
            CHECK(o->down >= R[i].min_down);
            CHECK(o->up >= 21600);
            CHECK(o->bd > (int64_t)o->down * 9);
            CHECK(o->bu > (int64_t)o->up * 9);
            CHECK_EQ_I(o->fd, 0);
            CHECK_EQ_I(o->fu, 0);
            CHECK(o->t_data < 13000);
            CHECK(o->rtd > 0);
            CHECK_EQ_I(o->robbed != 0, R[i].rbs);
        }
    }
    /* what the DIL analysis buys: with Table 1's levels the same lines carry
     * errors (or connect lower) */
    for (i = 0; i < NR; i++)
        if (R[i].table1) CHECK(O[i].fd > 0 || !O[i].ok || O[i].down < O[i - 1].down);

    printf("ending a call\n");
    {
        vl_cfg c;
        vl_t *l = &L;
        int64_t t0;
        memset(&c, 0, sizeof c);
        c.loop_km = 2; c.noise_dbm0 = -75; c.ppm = 20; c.phase = 0.4; c.echo_db = -16;
        c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.full = 1; c.caller_orig = 1; c.seed = 7;
        vl_init(l, &c);
        while (l->k < MS(30000) && !(orig_moving(&l->org) && answer_moving(&l->ans))) vl_tick(l);
        CHECK_EQ_I(l->org.modulation, ORIG_MOD_V90);
        CHECK_EQ_I(l->org.used_v8, 1);
        CHECK_EQ_I(orig_retrains(&l->org), 0);      /* (retrains and renegotiations: test_orig_retrain) */
        CHECK_EQ_I(orig_retraining(&l->org), 0);
        CHECK(orig_snr_db10(&l->org) > 400);
        t0 = l->k;
        CHECK_EQ_I(orig_cleardown(&l->org), 0);
        while (l->k < t0 + MS(3000) && l->org.phase != ORIG_FAILED) vl_tick(l);
        { int64_t e = l->k + MS(500); while (l->k < e) vl_tick(l); }
        printf("  ours: caller ended %d after %d ms (end %d), answerer phase %d, cleared %d\n", l->org.phase,
               (int)((l->k - t0) / 8), l->org.end, l->ans.phase, cleared_of(&l->ans));
        CHECK_EQ_I(l->org.end, ORIG_END_HANGUP);
        CHECK_EQ_I(answer_end(&l->ans), ANS_END_CLEARDOWN_FAR);

        c.seed = 8;
        vl_init(l, &c);
        while (l->k < MS(30000) && !(orig_moving(&l->org) && answer_moving(&l->ans))) vl_tick(l);
        t0 = l->k;
        CHECK_EQ_I(answer_cleardown(&l->ans), 0);
        while (l->k < t0 + MS(3000) && l->org.phase != ORIG_FAILED) vl_tick(l);
        printf("  the answerer's: caller ended after %d ms, end %d\n", (int)((l->k - t0) / 8), l->org.end);
        CHECK_EQ_I(l->org.phase, ORIG_FAILED);
        CHECK_EQ_I(l->org.end, ORIG_END_CLEARDOWN);
    }
    T_REPORT("orig_v90");
}
