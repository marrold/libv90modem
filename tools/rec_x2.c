/* Record a simulated x2 call for the bench (rig/bench_v90 -x): the x2
 * client of sim/x2call calls the answering modem with x2 offered over the
 * link of test/test_answer_x2.c (our octets to it through the network's
 * pad, its samples back through a mu-law codec with our own echo and
 * noise), and the codewords the answering modem receives go to a file.
 *
 *   rec_x2 [SECONDS [PAD [FILE [INDEX [EC]]]]]   (defaults 40, 0, /dev/null, 1)
 *
 * INDEX is the downstream rate index the client asks for (1: 32000, 15:
 * 56000). EC (none, v42bis or v44): both ends run V.42 (rig/ec_side.h) on
 * their data bits, with no compression, V.42 bis or V.44, each writing an
 * English sentence; the bench replays it with -e EC. Without EC the data
 * bits are the bench's pseudo-random sequence.
 *
 * Prints the transmit hash the bench compares with, and what the call did. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "answer.h"
#include "x2call.h"
#include "g711.h"
#include "noise.h"
#include "../rig/ec_side.h"

/* the bench's data: an LCG per direction, the same seeds as rec_v90 */
typedef struct { uint32_t s; long n, bad; } prbs;
static int nx(prbs *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
static int get(void *u) { prbs *p = ((void **)u)[0]; p->n++; return nx(p); }
static void put(void *u, int b) { prbs *p = ((void **)u)[1]; p->n++; if (nx(p) != (b & 1)) p->bad++; }
/* with EC: V.42 on the bits (rig/ec_side.h) */
static ec_side aec, xec;
static int aget(void *u) { (void)u; return ec_side_get(&aec); }
static void aput(void *u, int b) { (void)u; ec_side_put(&aec, b); }
static int xget(void *u) { (void)u; return ec_side_get(&xec); }
static void xput(void *u, int b) { (void)u; ec_side_put(&xec, b); }

#define XD 4096
int main(int argc, char **argv)
{
    static answer_t a; static x2call_t x;
    static prbs dsrc, dsink, asrc, asink; static void *aio[2], *dio[2];
    static uint8_t down[XD]; static int16_t up[XD]; static double dl[XD];
    int secs = argc > 1 ? atoi(argv[1]) : 40, pad = argc > 2 ? atoi(argv[2]) : 0;
    int index = argc > 4 ? atoi(argv[4]) : 1, ecm = argc > 5 ? ec_mode(argv[5]) : EC_OFF;
    FILE *dump = fopen(argc > 3 ? argv[3] : "/dev/null", "wb");
    const int d_ac = 37, d_ca = 41, e = 300;
    uint32_t hash = 2166136261u;
    answer_cfg ac; x2call_cfg xc; rng_t rng;
    uint8_t arx = 0xff;
    long n, data_at = -1;
    if (!dump) { perror("rec_x2"); return 2; }
    if (argc > 5 && ecm == EC_OFF) { fprintf(stderr, "rec_x2: EC is none, v42bis or v44\n"); return 2; }
    ec_side_init(&aec, 0, ecm); ec_side_init(&xec, 1, ecm);
    memset(down, 0x7f, sizeof down);
    rng_seed(&rng, 4242);
    dsrc.s = asink.s = 5; asrc.s = dsink.s = 9;
    dio[0] = &dsrc; dio[1] = &dsink; aio[0] = &asrc; aio[1] = &asink;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_X2;
    ac.getbit = ecm == EC_OFF ? get : aget; ac.putbit = ecm == EC_OFF ? put : aput; ac.user = dio;
    answer_init(&a, &ac);
    answer_v90_line(&a, -1, 0, 0);
    memset(&xc, 0, sizeof xc);
    xc.level = 3000; xc.pad_mode = pad == 6 ? 2 : pad == 3 ? 1 : 0; xc.max_index = index; xc.up_mask = 0x03FE;
    xc.getbit = ecm == EC_OFF ? get : xget; xc.putbit = ecm == EC_OFF ? put : xput; xc.user = aio;
    x2call_init(&x, &xc);
    for (n = 0; n < 8000L * secs; n++) {
        int i = (int)(n & (XD - 1));
        double v;
        uint8_t atx;
        fputc(arx, dump);
        if (ecm != EC_OFF) { ec_side_sample(&aec, n, a.connected); ec_side_sample(&xec, n, x.data_at >= 0); }
        atx = answer_tick_pcm(&a, arx);
        hash = (hash ^ atx) * 16777619u;
        down[i] = atx;
        dl[i] = (double)ulaw_to_linear(atx);
        up[i] = x2call_tick(&x, down[(n - d_ac) & (XD - 1)]);
        v = 0.5 * up[(n - d_ca) & (XD - 1)]
            + 0.12 * dl[(n - e - 2) & (XD - 1)] + 0.08 * dl[(n - e - 4) & (XD - 1)] - 0.05 * dl[(n - e - 9) & (XD - 1)]
            + 2.0 * rng_gauss(&rng);
        arx = linear_to_ulaw((int32_t)lround(v));
        if (data_at < 0 && a.connected) data_at = n;
        if (a.phase == ANS_FAILED) { printf("FAILED at %.2f s, x2 error %d\n", n / 8000.0, a.u.x2.error); break; }
    }
    fclose(dump);
    printf("transmit hash %08x, connected %d (data mode from %.2f s), bits to line %ld, bits from line %ld (%ld bad)\n",
           (unsigned)hash, a.connected, data_at / 8000.0, dsrc.n, dsink.n, dsink.bad);
    printf("x2 %d down / %d up; down %ld bits %ld bad\n", a.bitrate, a.rx_bitrate, asink.n, asink.bad);
    if (ecm != EC_OFF)
        printf("V.42 answerer: state %d, got %lu octets, hash %08x; client: state %d, got %lu octets, hash %08x\n",
               aec.on ? (int)v42ec_state(&aec.ec) : -1, aec.got, (unsigned)aec.h,
               xec.on ? (int)v42ec_state(&xec.ec) : -1, xec.got, (unsigned)xec.h);
    return a.connected ? 0 : 1;
}
