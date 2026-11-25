/* CPU cost of the V.90 answering modem on the target, and a check that the
 * target computes exactly what the host does.
 *
 * tools/rec_v90 runs a simulated call on the host and records the codewords
 * the answering modem receives. This program replays them through the same
 * modem code (integer only, so the result must be identical: the transmit
 * hash is compared with the host's) and times it per phase of the call.
 * It ends with "tx hash" (what it sent) and "rx hash" (the data bits it
 * received, FNV-1a over one bit at a time) for tools/bench-hashes.sh.
 *
 *   bench_v90 [-x | -k] [-e none|v42bis|v44] RECORDING [HOST-HASH-HEX]
 *
 * -x: the recording is of an x2 call (tools/rec_x2): x2 is offered, and
 * its phases are counted as V.90's (Phase 2 is V.34's, then the x2
 * start-up, then data mode).
 * -k: of a K56flex call (tools/rec_k56): K56flex is offered, and its
 * phases are counted the same way (V.8bis with V.8, Phase 2 V.34's, then
 * the K56flex start-up, then data mode).
 * -e: the data bits go to and from our V.42 endpoint (rig/ec_side.h), with
 * no compression, V.42 bis or V.44, as tools/rec_x2's EC argument records;
 * its cost is in the figures (`make cost-x2-v42`). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../modem/answer.h"
#include "ec_side.h"

typedef struct { uint32_t s; long n, bad; uint32_t h; } prbs;
static int nx(prbs *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
static int get(void *u) { prbs *p = ((prbs **)u)[0]; p->n++; return nx(p); }
static void put(void *u, int b) { prbs *p = ((prbs **)u)[1]; p->n++; p->h = (p->h ^ (uint32_t)(b & 1)) * 16777619u; if (nx(p) != (b & 1)) p->bad++; }
/* -e: V.42 on the bits; the bits received are still counted and hashed */
static ec_side ec;
static int ec_get(void *u) { ((prbs **)u)[0]->n++; return ec_side_get(&ec); }
static void ec_put(void *u, int b) { prbs *p = ((prbs **)u)[1]; p->n++; p->h = (p->h ^ (uint32_t)(b & 1)) * 16777619u; ec_side_put(&ec, b); }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define BLK 80
enum { PH_V8, PH_P2, PH_P3, PH_DATA, NPH };
static const char *const ph_name[NPH] = { "V.8 (ANSam, CM/JM)", "Phase 2 (probing)", "Phases 3 and 4 (training)", "data mode" };

int main(int argc, char **argv)
{
    static answer_t a; static prbs src, sink; static prbs *io[2];
    static uint8_t in[BLK];
    answer_cfg ac;
    double t_ph[NPH] = { 0 }, worst[NPH] = { 0 };
    long n_ph[NPH] = { 0 }, total = 0;
    uint32_t hash = 2166136261u;
    FILE *f;
    size_t got;
    int x2 = 0, k56 = 0, ecm = EC_OFF;

    if (argc > 1 && !strcmp(argv[1], "-x")) { x2 = 1; argv++; argc--; }
    else if (argc > 1 && !strcmp(argv[1], "-k")) { k56 = 1; argv++; argc--; }
    if (argc > 2 && !strcmp(argv[1], "-e")) {
        if ((ecm = ec_mode(argv[2])) == EC_OFF) { fprintf(stderr, "bench_v90: -e none, v42bis or v44\n"); return 2; }
        argv += 2; argc -= 2;
    }
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: bench_v90 [-x | -k] [-e none|v42bis|v44] RECORDING [HASH]\n"); return 2; }
    ec_side_init(&ec, 0, ecm);
    src.s = 5; sink.s = 9; sink.h = 2166136261u; io[0] = &src; io[1] = &sink;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90; ac.getbit = ecm == EC_OFF ? get : ec_get; ac.putbit = ecm == EC_OFF ? put : ec_put; ac.user = io;
    if (x2) ac.modes |= V8_MOD_V34 | ANS_MODE_X2;
    if (k56) ac.modes |= V8_MOD_V34 | ANS_MODE_K56;
    answer_init(&a, &ac);
    if (x2 || k56) answer_v90_line(&a, -1, 0, 0);     /* as tools/rec_x2, tools/rec_k56 */

    while ((got = fread(in, 1, BLK, f)) == BLK) {
        int ph = (a.phase == ANS_V90 || a.phase == ANS_X2 || a.phase == ANS_K56) && a.connected ? PH_DATA
                 : a.phase == ANS_P3 || a.phase == ANS_V90 || a.phase == ANS_X2 || a.phase == ANS_K56 ? PH_P3
                 : a.phase == ANS_P2 || a.phase == ANS_V34 ? PH_P2 : PH_V8;
        double t0 = now(), dt;
        int i;
        if (ecm == EC_OFF)
            for (i = 0; i < BLK; i++) hash = (hash ^ answer_tick_pcm(&a, in[i])) * 16777619u;
        else
            for (i = 0; i < BLK; i++) {
                ec_side_sample(&ec, total + i, a.connected);
                hash = (hash ^ answer_tick_pcm(&a, in[i])) * 16777619u;
            }
        dt = now() - t0;
        t_ph[ph] += dt; n_ph[ph] += BLK; total += BLK;
        if (dt > worst[ph]) worst[ph] = dt;
    }
    fclose(f);

    printf("replayed %.1f s; transmit hash %08x%s; connected %d, %d bit/s down, %d bit/s up; bits from line %ld (%ld bad)\n",
           total / 8000.0, (unsigned)hash,
           argc > 2 ? (strtoul(argv[2], NULL, 16) == hash ? " = host" : " DIFFERS FROM HOST") : "",
           a.connected, a.bitrate, a.rx_bitrate, sink.n, sink.bad);
    printf("tx hash %08x\nrx hash %08x\n", (unsigned)hash, (unsigned)sink.h);
    if (ecm != EC_OFF)
        printf("V.42 answerer: state %d, got %lu octets, hash %08x\n",
               ec.on ? (int)v42ec_state(&ec.ec) : -1, ec.got, (unsigned)ec.h);
    {
        int p;
        for (p = 0; p < NPH; p++) {
            if (!n_ph[p]) continue;
            printf("  %-28s %6.1f s of line: %5.1f %% of the CPU, worst 10 ms block took %5.2f ms\n",
                   ph_name[p], n_ph[p] / 8000.0, 100.0 * t_ph[p] / (n_ph[p] / 8000.0), worst[p] * 1000.0);
        }
    }
    return 0;
}
