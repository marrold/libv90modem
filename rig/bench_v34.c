/* CPU cost of the V.34 answering modem on the target, and a check that the
 * target computes exactly what the host does: replays what tools/rec_v34
 * recorded through modem/v8ans + modem/v34ans (integer only, so the transmit
 * hash must equal the host's) and times it per phase of the call. It ends
 * with "tx hash" (what it sent) and "rx hash" (the data bits it received,
 * FNV-1a over one bit at a time) for tools/bench-hashes.sh.
 *
 *   bench_v34 RECORDING
 *
 * For exact instruction counts run it under qemu on two truncated copies of
 * the recording (see `make cost-v34`). */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "v34glue.h"

static v34prbs src = { 5, 0, 0 }, sink = { 9, 0, 0 };
static uint32_t rxh = 2166136261u;     /* FNV-1a of the data bits received */
static int  get(void *u) { (void)u; src.n++; return v34prbs_next(&src); }
static void put(void *u, int b) { (void)u; sink.n++; rxh = (rxh ^ (uint32_t)(b & 1)) * 16777619u; if (v34prbs_next(&sink) != (b & 1)) sink.bad++; }

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
    static v34glue_t a;
    static int16_t in[BLK];
    v34ans_cfg ac;
    double t_ph[NPH] = { 0 }, worst[NPH] = { 0 };
    long n_ph[NPH] = { 0 }, total = 0;
    uint32_t hash = 2166136261u;
    FILE *f;
    int k;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: bench_v34 RECORDING\n"); return 2; }
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.law = 1; ac.getbit = get; ac.putbit = put;
    v34glue_init(&a, &ac);
    while (fread(in, sizeof in[0], BLK, f) == BLK) {
        int ph = a.phase == 0 ? PH_V8 : a.m.state == V34A_PHASE2 || a.m.state == V34A_RETRAIN ? PH_P2
               : a.m.state == V34A_DATA ? PH_DATA : PH_P3;
        double t0 = now(), dt;
        int i;
        for (i = 0; i < BLK; i++) hash = (hash ^ (uint16_t)v34glue_tick(&a, in[i])) * 16777619u;
        dt = now() - t0;
        t_ph[ph] += dt; n_ph[ph] += BLK; total += BLK;
        if (dt > worst[ph]) worst[ph] = dt;
    }
    fclose(f);
    printf("replayed %.1f s; transmit hash %08x; state %d, %d bit/s out, %d bit/s in; bits from line %ld (%ld bad)\n",
           total / 8000.0, (unsigned)hash, (int)a.m.state, a.m.dx.tx_rate, a.m.dx.rx_rate, sink.n, sink.bad);
    printf("tx hash %08x\nrx hash %08x\n", (unsigned)hash, (unsigned)rxh);
    for (k = 0; k < NPH; k++)
        if (n_ph[k])
            printf("  %-28s %5.1f s of line: %5.1f %% of this CPU, worst 10 ms block took %5.2f ms\n",
                   ph_name[k], n_ph[k] / 8000.0, 100.0 * t_ph[k] / (n_ph[k] / 8000.0), worst[k] * 1e3);
    return 0;
}
