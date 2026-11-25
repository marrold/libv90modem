/* Record a simulated K56flex call for the bench (rig/bench_v90 -k): the
 * K56flex client of sim/k56call calls the answering modem with K56flex
 * offered over the link of test/test_answer_k56.c (our octets to it
 * through the network's pad and robbed bits, its samples back through a
 * mu-law codec with our own echo, after the pad, and noise), and the
 * codewords the answering modem receives go to a file.
 *
 *   rec_k56 [SECONDS [PAD [FILE [RATE [RBS]]]]] [heavy]   (defaults 40, 0, /dev/null, 13, 0)
 *
 * PAD in dB (0, 3, 6); RATE the downstream rate code the client asks for
 * (1: 32000, 13: 56000); RBS the robbed-bit pattern on the line (hex, bit k
 * for the samples whose count mod 6 is k). heavy, anywhere among them (not
 * counted as one of them): the client ignores our MP and sends x2's upstream
 * (64 states, non-linear, expanded shaping; k56call_cfg.heavy_up), e.g.
 * make cost-k56 K56_ARGS=heavy.
 *
 * Prints the transmit hash the bench compares with, and what the call did. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "answer.h"
#include "k56call.h"
#include "x2call.h"
#include "g711.h"
#include "noise.h"

/* the bench's data: an LCG per direction, the same seeds as rec_v90 */
typedef struct { uint32_t s; long n, bad; } prbs;
static int nx(prbs *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
static int get(void *u) { prbs *p = ((void **)u)[0]; p->n++; return nx(p); }
static void put(void *u, int b) { prbs *p = ((void **)u)[1]; p->n++; if (nx(p) != (b & 1)) p->bad++; }

#define XD 4096
int main(int argc0, char **argv0)
{
    static answer_t a; static k56call_t k;
    static prbs dsrc, dsink, asrc, asink; static void *aio[2], *dio[2];
    static uint8_t down[XD]; static int16_t up[XD]; static double dl[XD];
    char *argv[6];
    int argc = 0, heavy = 0, i;
    int secs, pad, rate, rbs, pad_mode;
    FILE *dump;
    const int d_ac = 37, d_ca = 41;
    uint32_t hash = 2166136261u;
    answer_cfg ac; k56call_cfg kc; rng_t rng;
    uint8_t arx = 0xff;
    long n, data_at = -1;
    for (i = 0; i < argc0; i++) {
        if (i > 0 && strcmp(argv0[i], "heavy") == 0) heavy = 1;
        else if (argc < 6) argv[argc++] = argv0[i];
    }
    secs = argc > 1 ? atoi(argv[1]) : 40; pad = argc > 2 ? atoi(argv[2]) : 0;
    rate = argc > 4 ? atoi(argv[4]) : 13; rbs = argc > 5 ? (int)strtol(argv[5], NULL, 16) : 0;
    pad_mode = pad == 6 ? 2 : pad == 3 ? 1 : 0;
    dump = fopen(argc > 3 ? argv[3] : "/dev/null", "wb");
    if (!dump) { perror("rec_k56"); return 2; }
    memset(down, 0xff, sizeof down);
    rng_seed(&rng, 4242);
    dsrc.s = asink.s = 5; asrc.s = dsink.s = 9;
    dio[0] = &dsrc; dio[1] = &dsink; aio[0] = &asrc; aio[1] = &asink;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_K56;
    ac.getbit = get; ac.putbit = put; ac.user = dio;
    answer_init(&a, &ac);
    answer_v90_line(&a, -1, 0, 0);
    k56call_default(&kc, 3000);
    kc.pad_mode = pad_mode; kc.rate = rate; kc.rbs = rbs & 0x3f; kc.heavy_up = heavy ? K56C_HEAVY_ALWAYS : K56C_HEAVY_NONE;
    kc.getbit = get; kc.putbit = put; kc.user = aio;
    k56call_init(&k, &kc);
    for (n = 0; n < 8000L * secs; n++) {
        int i = (int)(n & (XD - 1));
        double v;
        uint8_t atx;
        fputc(arx, dump);
        atx = answer_tick_pcm(&a, arx);
        hash = (hash ^ atx) * 16777619u;
        down[i] = atx;
        dl[i] = (double)ulaw_to_linear(x2call_pad(atx, pad_mode));
        up[i] = k56call_tick(&k, down[(n - d_ac) & (XD - 1)]);
        v = 0.5 * up[(n - d_ca) & (XD - 1)]
            + 0.12 * dl[(n - 2) & (XD - 1)] + 0.08 * dl[(n - 4) & (XD - 1)] - 0.05 * dl[(n - 9) & (XD - 1)]
            + 2.0 * rng_gauss(&rng);
        arx = linear_to_ulaw((int32_t)lround(v));
        if (data_at < 0 && a.connected) data_at = n;
        if (a.phase == ANS_FAILED) { printf("FAILED at %.2f s, K56flex error %d\n", n / 8000.0, a.u.k56.error); break; }
    }
    fclose(dump);
    printf("transmit hash %08x, connected %d (data mode from %.2f s), bits to line %ld, bits from line %ld (%ld bad)\n",
           (unsigned)hash, a.connected, data_at / 8000.0, dsrc.n, dsink.n, dsink.bad);
    printf("K56flex %d down / %d up; down %ld bits %ld bad\n", a.bitrate, a.rx_bitrate, asink.n, asink.bad);
    return a.connected && a.modulation == ANS_MOD_K56 ? 0 : 1;
}
