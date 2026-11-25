/* The calling side over more seeds: every line of test_orig_v90 /
 * test_orig_v92 with seeds 100 .. 105 (other noise, clock phase, robbed
 * interval), V.90 and V.92, 10 s of data each way per call.
 *
 * Checked on every call: data mode reached on both sides and kept, at least
 * 28000 bit/s down (and 24000 up with V.92), and both directions without one
 * failure of the self-synchronising check. Before robust 2b: V.90 1 of 54
 * calls bad (the 3 dB pad line, seed 103: the receiver lost), V.92 5 of 54
 * (four on the 6 dB pad + robbed bit line: the answerer retrained at B1u, or
 * upstream errors after our timing moved). Before answer-fix 1, 2 of the
 * V.92 calls had bursts of upstream check failures at 37333 bit/s: the
 * digital modem's baseline follower took the codec's quantising error at the
 * large points for a baseline (docs/history/PROGRESS.md, answer-fix 1).
 * Host: 108 calls; TEST_SHORT (32-bit ARM under qemu): seed 100. */
#include <stdio.h>
#include <inttypes.h>
#include "test.h"
#include "v90link.h"

#define MS(x) ((int64_t)(x) * 8)

static vl_t L;

typedef struct { const char *name; double km, ppm, noise, pad; int rbs, alaw; } line_t;

/* a case (test/test.h, t_case_arg) is one line in V.90 or V.92: V.90's
 * lines are cases 1 .. 9, V.92's 10 .. 18. A case run alone checks its own
 * calls; the closing count of a modulation is printed with its last line
 * (the bad calls of all its lines, or of that case's own when it runs
 * alone: its calls are the only ones the program then knows of) */
int main(int argc, char **argv)
{
    static const line_t LN[] = {
        { "2 km,    0 ppm",                2,    0, -75, 0, 0, 0 },
        { "0 km, +100 ppm",                0,  100, -75, 0, 0, 0 },
        { "6 km, -100 ppm",                6, -100, -75, 0, 0, 0 },
        { "4 km,  +50 ppm, -65 dBm0",      4,   50, -65, 0, 0, 0 },
        { "2 km,  +30 ppm, 3 dB pad",      2,   30, -75, 3, 0, 0 },
        { "2 km,  -30 ppm, 6 dB pad",      2,  -30, -75, 6, 0, 0 },
        { "2 km,  +30 ppm, robbed bit",    2,   30, -75, 0, 1, 0 },
        { "2 km,  +30 ppm, 6 dB pad, RBS", 2,   30, -75, 6, 1, 0 },
        { "2 km,  -50 ppm, A-law",         2,  -50, -75, 0, 0, 1 },
    };
#ifdef TEST_SHORT
    enum { NS = 1 };
#else
    enum { NS = 6 };
#endif
    enum { NL = sizeof LN / sizeof LN[0] };
    int v92, i, s, bad[2] = { 0, 0 }, calls = 0;
    const int only = t_case_arg(argc, argv, 2 * NL);
    for (v92 = 0; v92 <= 1; v92++) {
        for (i = 0; i < NL; i++) {
            char row[160];
            int n = 0, lo = 99999, hi = 0;
            if (!T_CASE(only, v92 * NL + i + 1)) continue;
            if (i == 0) printf("%s: 9 lines x %d seeds, 10 s of data each way\n", v92 ? "V.92" : "V.90", NS);
            n += snprintf(row + n, sizeof row - (size_t)n, "  %-30s", LN[i].name);
            for (s = 0; s < NS; s++) {
                vl_cfg c;
                vl_t *l = &L;
                uint32_t seed = 100u + (uint32_t)s;
                int ok, down, up;
                int64_t f0d, f0u, d0, u0, fd, fu;
                memset(&c, 0, sizeof c);
                c.loop_km = LN[i].km; c.noise_dbm0 = LN[i].noise; c.ppm = LN[i].ppm; c.phase = 0.17 * (seed % 5);
                c.pad_db = LN[i].pad; c.rbs = LN[i].rbs; c.rbs_phase = (int)(seed % 6); c.alaw = LN[i].alaw;
                c.echo_db = -16; c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.full = 1; c.caller_orig = 1; c.seed = seed;
                if (v92) { c.v92 = 1; c.caller_v92 = 1; c.up_line = 1; }
                vl_init(l, &c);
                while (l->k < MS(40000) && !(orig_moving(&l->org) && answer_moving(&l->ans)) && l->org.phase != ORIG_FAILED) vl_tick(l);
                ok = orig_moving(&l->org) && answer_moving(&l->ans);
                down = l->org.rx_bitrate; up = l->org.bitrate;
                { int64_t e = l->k + MS(300); while (l->k < e) vl_tick(l); }
                f0d = l->down.fails; f0u = l->up.fails; d0 = l->down.got; u0 = l->up.got;
                { int64_t e = l->k + MS(10000); while (l->k < e && l->org.phase != ORIG_FAILED) vl_tick(l); }
                fd = l->down.fails - f0d; fu = l->up.fails - f0u;
                ok = ok && orig_moving(&l->org) && answer_moving(&l->ans) && fd == 0 && down >= 28000
                     && (!v92 || (l->org.modulation == ORIG_MOD_V92 && up >= 24000))
                     && l->down.got - d0 > (int64_t)down * 9 && l->up.got - u0 > (int64_t)up * 9;
                calls++;
                if (!ok || fu > 0) {
                    bad[v92]++;
                    printf("    %s seed %u: BAD: data %d, %d / %d bit/s, %" PRId64 " / %" PRId64 " check failures down / up\n",
                           LN[i].name, (unsigned)seed, ok, down, up, fd, fu);
                }
                if (down < lo) lo = down;
                if (down > hi) hi = down;
            }
            snprintf(row + n, sizeof row - (size_t)n, " down %5d .. %5d", lo, hi);
            printf("%s\n", row);
            if (only) CHECK_EQ_I(bad[v92], 0);
            if (i == NL - 1) {
                printf("  %d of %d calls bad\n", bad[v92], 9 * NS);
                if (!only) CHECK_EQ_I(bad[v92], 0);
            }
        }
    }
    CHECK(calls == (only ? NS : 18 * NS));
    T_REPORT("orig_seeds");
}
