/* The calling modem in V.90 (modem/orig with modem/v90a) on the target:
 * replays what tools/rec_v90c recorded and prints a hash of everything it
 * sent (integer only, so it must equal the host's), when it reached data
 * mode, its rates, and the bits it received; then "tx hash" (the same
 * hash) and "rx hash" (the data bits received, FNV-1a over one bit at a
 * time) for tools/bench-hashes.sh. A recording of a V.92 call
 * ("V92C0001") is replayed with V.92 on as well (PCM upstream).
 *
 *   bench_v90c RECORDING [-w N] [-t] [-n N]
 *
 * -w N also lists the N ticks that took the longest here (host time), for
 * an instruction count of each under qemu (`make cost-v90c`). For counts
 * per second of line run it under qemu on truncated copies of the recording
 * (the header says where data mode begins). A recording with a retrain or
 * a renegotiation of ours (rec_v90c ... rt / rn) asks for it at the same
 * tick.
 *
 * -t calls bench_v90c_mark() before every tick and -n N stops after N
 * samples: tools/qprof turns qemu's log of such a run into the instructions
 * of every tick and per function (`make prof-v90c`). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "orig.h"

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* the recorder's upstream data: 2^23 - 1 (test/v90link.h, bert_get, seed 5) */
typedef struct { uint32_t gen; int64_t got; uint32_t h; } src_t;
static int get(void *u)
{
    src_t *s = (src_t *)u;
    int bit = (int)(((s->gen >> 17) ^ (s->gen >> 22)) & 1);
    s->gen = ((s->gen << 1) | (uint32_t)bit) & 0x7fffff;
    return bit;
}
static void put(void *u, int bit) { src_t *s = (src_t *)u; s->got++; s->h = (s->h ^ (uint32_t)(bit & 1)) * 16777619u; }

static orig_t o;
/* not inlined (called through a volatile pointer): its address marks the trace */
volatile int64_t bench_v90c_marked;
void bench_v90c_mark(int64_t k);
void bench_v90c_mark(int64_t k) { bench_v90c_marked = k; }
static void (*volatile mark)(int64_t) = bench_v90c_mark;

#define NW 8

int main(int argc, char **argv)
{
    uint8_t hdr[20], b[2];
    orig_cfg c;
    src_t s;
    uint32_t hash = 2166136261u, t_rec;
    int64_t k = 0, t_data = -1;
    int nw = 0, tm = 0, i;
    int64_t nmax = -1, t_ev = -1;
    int ev = 0;
    double t0, t, wt[NW]; int64_t wk[NW];
    FILE *f;

    if (argc < 2) { fprintf(stderr, "usage: bench_v90c RECORDING [-w N] [-t] [-n N]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f || fread(hdr, 1, 12, f) != 12 || (memcmp(hdr, "V90C000", 7) != 0 && memcmp(hdr, "V92C000", 7) != 0)
        || (hdr[7] != '1' && hdr[7] != '2') || (hdr[7] == '2' && fread(hdr + 12, 1, 8, f) != 8)) {
        fprintf(stderr, "bench_v90c: not a recording of rec_v90c\n");
        return 1;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) nw = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) nmax = atoll(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0) tm = 1;
    }
    t_rec = (uint32_t)hdr[8] | (uint32_t)hdr[9] << 8 | (uint32_t)hdr[10] << 16 | (uint32_t)hdr[11] << 24;
    if (hdr[7] == '2') {
        ev = (int)hdr[12];
        t_ev = (int64_t)((uint32_t)hdr[16] | (uint32_t)hdr[17] << 8 | (uint32_t)hdr[18] << 16 | (uint32_t)hdr[19] << 24);
    }
    if (nw > NW) nw = NW;
    for (i = 0; i < NW; i++) { wt[i] = 0; wk[i] = -1; }
    memset(&s, 0, sizeof s); s.gen = (0x4321u + 5u) | 1u; s.h = 2166136261u;
    memset(&c, 0, sizeof c);
    c.level = 3000; c.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21 | ORIG_MODE_V90;
    if (memcmp(hdr, "V92C", 4) == 0) c.modes |= ORIG_MODE_V92;
    c.echo_delay = 0; c.law = 0;
    c.getbit = get; c.putbit = put; c.user = &s;
    orig_init(&o, &c);
    t0 = now();
    while ((nmax < 0 || k < nmax) && fread(b, 1, 2, f) == 2) {
        int16_t rx = (int16_t)(uint16_t)(b[0] | (unsigned)b[1] << 8), tx;
        double ta = nw ? now() : 0;
        if (tm) mark(k);
        if (ev && k == t_ev) { if (ev == 1) (void)orig_retrain(&o); else (void)orig_renegotiate(&o, 0); }
        tx = orig_tick(&o, rx);
        if (nw) {
            double d = now() - ta;
            int j = nw - 1;
            if (d > wt[j]) {
                while (j > 0 && d > wt[j - 1]) { wt[j] = wt[j - 1]; wk[j] = wk[j - 1]; j--; }
                wt[j] = d; wk[j] = k;
            }
        }
        hash = (hash ^ (uint32_t)((uint16_t)tx & 0xff)) * 16777619u;
        hash = (hash ^ (uint32_t)((uint16_t)tx >> 8)) * 16777619u;
        k++;
        if (t_data < 0 && orig_moving(&o)) t_data = k;
    }
    t = now() - t0;
    fclose(f);
    printf("bench_v90c: %" PRId64 " samples (%.2f s of line), %sdata mode at sample %" PRId64 " (recorded %" PRIu32 "), "
           "%d / %d bit/s, %" PRId64 " bits received, hash %08" PRIx32 "\n",
           k, (double)k / 8000.0, o.modulation == ORIG_MOD_V92 ? "V.92, " : "", t_data, t_rec, o.rx_bitrate, o.bitrate, s.got, hash);
    printf("tx hash %08" PRIx32 "\nrx hash %08" PRIx32 "\n", hash, s.h);
    if (!nw) fprintf(stderr, "bench_v90c: %.3f s of CPU here (%.1f %% of real time)\n", t, 100.0 * t / ((double)k / 8000.0 + 1e-9));
    for (i = 0; i < nw; i++) if (wk[i] >= 0) printf("slow tick %" PRId64 " (%.3f s): %.1f us here\n", wk[i], (double)wk[i] / 8000.0, wt[i] * 1e6);
    return 0;
}
