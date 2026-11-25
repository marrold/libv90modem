/* The calling modem in Bell 103 or Bell 212A (modem/orig) on the target:
 * replays what tools/rec_bell recorded into the caller alone and prints a
 * hash of everything it sent (integer only: the host's and the target's
 * must agree), then "tx hash" (the same hash) and "rx hash" (the data
 * bits it received, FNV-1a over one bit at a time) for
 * tools/bench-hashes.sh. For an instruction count under qemu (`make
 * cost-bell`) it calls bench_bell_mark() where data mode begins and one
 * second later: in qemu's instruction trace that function's address
 * splits one run into the start-up and that second of data mode.
 *
 *   bench_bell RECORDING */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "orig.h"

static orig_t o;
static uint32_t gen = 0x1234567u;
/* the caller's data, as tools/rec_bell sends it: the first 300 data bits
 * are marks (an idle start-stop line: a Bell 103 answerer connects on
 * 200 ms of the caller's mark), then pseudo-random */
static int nget;
static int get(void *u) { (void)u; if (nget < 300) { nget++; return 1; } gen = gen * 1103515245u + 12345u; return (int)((gen >> 16) & 1); }
typedef struct { int64_t got; uint32_t h; } sink_t;
static void put(void *u, int bit) { sink_t *s = (sink_t *)u; s->got++; s->h = (s->h ^ (uint32_t)(bit & 1)) * 16777619u; }
/* not inlined (called through a volatile pointer): its address marks the trace */
volatile int bench_bell_marked;
void bench_bell_mark(int i);
void bench_bell_mark(int i) { bench_bell_marked = i; }
static void (*volatile mark)(int) = bench_bell_mark;
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv)
{
    uint8_t hdr[16], b[2];
    orig_cfg c;
    uint32_t hash = 2166136261u, t_rec;
    int64_t k = 0;
    sink_t s = { 0, 2166136261u };
    FILE *f;
    if (argc < 2) { fprintf(stderr, "usage: bench_bell RECORDING\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f || fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "BELL0001", 8) != 0) { fprintf(stderr, "bench_bell: not a recording of rec_bell\n"); return 1; }
    t_rec = get32(hdr + 12);
    memset(&c, 0, sizeof c);
    c.level = 3000; c.modes = get32(hdr + 8);
    c.getbit = get; c.putbit = put; c.user = &s;
    orig_init(&o, &c);
    while (fread(b, 1, 2, f) == 2) {
        int16_t rx = (int16_t)(uint16_t)(b[0] | (unsigned)b[1] << 8), tx;
        if (k == (int64_t)t_rec) mark(1);
        if (k == (int64_t)t_rec + 8000) mark(2);
        tx = orig_tick(&o, rx);
        hash = (hash ^ (uint32_t)((uint16_t)tx & 0xff)) * 16777619u;
        hash = (hash ^ (uint32_t)((uint16_t)tx >> 8)) * 16777619u;
        k++;
    }
    fclose(f);
    printf("bench_bell: %" PRId64 " samples (%.2f s of line), modulation %d, %d bit/s, data mode at sample %" PRIu32 ", %" PRId64 " bits received, hash %08" PRIx32 "\n",
           k, (double)k / 8000.0, o.modulation, o.bitrate, t_rec, s.got, hash);
    printf("tx hash %08" PRIx32 "\nrx hash %08" PRIx32 "\n", hash, s.h);
    return 0;
}
