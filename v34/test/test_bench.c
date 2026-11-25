/* Cost of the encoder and the decoder, per 2D symbol.
 *
 * Two measurements:
 *  - wall time per symbol on whatever runs this. On the host that is a
 *    regression guard. Under qemu-arm (`make test-arm`) it says nothing
 *    about the target and is only printed.
 *  - `make cost-arm`: executed ARM instructions per symbol, counted by qemu
 *    (the `count` mode below). The same count is taken for the real FIR
 *    kernel of stage0/bench.c, which the project timed on the actual box
 *    (rfir = 25.4 MMAC/s on the 400 MHz ARM926). Decoder instructions per
 *    symbol / FIR instructions per MAC gives the decoder cost in MACs, and
 *    MACs * symbol rate / 25.4e6 an estimate of the share of that CPU. The
 *    estimate assumes the decoder runs as many cycles per instruction as
 *    the FIR loop does; the decoder has more branches and table lookups, so
 *    read it as a lower bound of the right order. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "test.h"
#include "testutil.h"

#ifdef TEST_SHORT
#define NSYM 24000UL
#else
#define NSYM 240000UL
#endif

static v34_params_t p;
static v34_tx_t tx;
static v34_rx_t rx;
static v34_pt_t buf[NSYM];
static unsigned long sunk, nsunk;
static void sink_put(void *u, int bit) { (void)u; sunk += (unsigned)bit; nsunk++; }

/* same kernel as stage0/bench.c */
#define NTAPS 128
static int16_t rd[NTAPS], rc[NTAPS];
static volatile int32_t vsink;
static int32_t rfir(const int16_t *d, const int16_t *c, int n)
{
    int32_t acc = 0;
    int i;
    for (i = 0; i < n; i++)
        acc += ((int32_t)d[i] * c[i]) >> 6;
    return acc;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void bench(unsigned S, unsigned R, int trellis, const int16_t h[3][2], double esn0_db,
                  const char *note)
{
    v34_cfg_t c;
    tu_src_t src = { 0xFACEB00Cu, 0 };
    double sigma, t_tx, t_rx, t_tent, t0, t1;
    unsigned long i;
    volatile int acc = 0;

    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R; c.trellis = (uint8_t)trellis;
    c.scrambler = V34_GPA;
    if (h) memcpy(c.h, h, sizeof c.h);
    CHECK_EQ_I(v34_params_init(&p, &c), 0);
    sigma = sqrt(p.energy_q8 / 256.0 / (2 * pow(10, esn0_db / 10)));

    v34_tx_init(&tx, &p, tu_getbit, &src);
    t0 = now();
    for (i = 0; i < NSYM; i++) v34_tx_symbol(&tx, &buf[i], NULL);
    t1 = now();
    t_tx = (t1 - t0) * 1e9 / NSYM;
    for (i = 0; i < NSYM; i++) buf[i] = tu_add_noise(buf[i], sigma);

    nsunk = 0;
    v34_rx_init(&rx, &p, sink_put, NULL);
    t0 = now();
    for (i = 0; i < NSYM; i++) v34_rx_put(&rx, buf[i], NULL);
    t1 = now();
    t_rx = (t1 - t0) * 1e9 / NSYM;

    /* the tentative decision alone (what a decision-directed loop adds) */
    t0 = now();
    for (i = 0; i < NSYM; i++) { v34_pt_t s = v34_slice(&p, buf[i]); acc += s.re; }
    t1 = now();
    t_tent = (t1 - t0) * 1e9 / NSYM;

    printf("    %u/%-5u %2d st %-9s %6.0f ns  %6.0f ns  %5.0f ns\n",
           S, R, 16 << trellis, note, t_tx, t_rx, t_tent);
    /* the decoder did decode: all but the B1 frame and the decoding delay */
    CHECK(nsunk >= (NSYM / 8 - p.map.P - V34_VIT_DEPTH / 4 - 2) * (p.map.b - 1u));
    /* the decoder's wall time (t_rx, printed above) is reported, not checked:
     * a loaded machine or an emulator is no measure of it */
}

static const int16_t H[3][2] = { { -9830, 3277 }, { 4096, -1638 }, { -819, 410 } };

/* Instruction counting mode for `make cost-arm`: run silently so that qemu
 * can count executed instructions.
 *   test_bench count <config> <mode> <nsym>
 * mode 0: encoder + noise only; mode 1: the same plus the decoder;
 * mode 2: the same plus the decoder with tentative decisions requested;
 * mode 3: nsym calls of the 128-tap reference FIR instead (the unit).
 * The decoder's cost per symbol is the difference, taken between two
 * values of nsym so that start-up cancels. Noise is integer (sum of four
 * uniform bytes) so that no soft-float runs in the measured loop. */
static int count_mode(int cfg, int mode, unsigned long nsym)
{
    static const struct { unsigned S, R; int tr, pre; unsigned sigma_q7; } cs[] = {
        { 3200, 4800,  V34_TRELLIS_16, 0, 29 },    /* about 13 dB */
        { 3200, 19200, V34_TRELLIS_16, 0, 47 },    /* about 23 dB */
        { 3200, 31200, V34_TRELLIS_16, 0, 44 },    /* about 34 dB */
        { 3200, 19200, V34_TRELLIS_16, 1, 47 },
        { 3200, 19200, V34_TRELLIS_32, 0, 47 },
        { 3200, 19200, V34_TRELLIS_64, 0, 47 } };
    v34_cfg_t c;
    tu_src_t src = { 0xFACEB00Cu, 0 };
    unsigned long i;

    if (mode == 3) {
        int32_t a = 0;
        for (i = 0; i < NTAPS; i++) { rd[i] = (int16_t)(tu_rand() >> 17); rc[i] = (int16_t)(tu_rand() >> 17); }
        for (i = 0; i < nsym; i++) { rd[i & (NTAPS - 1)] ^= 1; a += rfir(rd, rc, NTAPS); }
        vsink = a;
        return 0;
    }
    if (cfg < 0 || cfg >= (int)(sizeof cs / sizeof cs[0])) return 2;
    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)cs[cfg].S; c.data_rate = (uint16_t)cs[cfg].R;
    c.trellis = (uint8_t)cs[cfg].tr; c.scrambler = V34_GPA;
    if (cs[cfg].pre) memcpy(c.h, H, sizeof c.h);
    if (v34_params_init(&p, &c)) return 2;
    v34_tx_init(&tx, &p, tu_getbit, &src);
    v34_rx_init(&rx, &p, sink_put, NULL);
    for (i = 0; i < nsym; i++) {
        v34_pt_t x, t;
        uint32_t a = tu_rand(), b = tu_rand();
        int na = (int)((a & 255) + ((a >> 8) & 255) + ((a >> 16) & 255) + (a >> 24)) - 510;
        int nb = (int)((b & 255) + ((b >> 8) & 255) + ((b >> 16) & 255) + (b >> 24)) - 510;
        v34_tx_symbol(&tx, &x, NULL);
        x.re = (int16_t)(x.re + ((na * (int)cs[cfg].sigma_q7) >> 7));   /* std of na is 148 */
        x.im = (int16_t)(x.im + ((nb * (int)cs[cfg].sigma_q7) >> 7));
        if (mode == 1) v34_rx_put(&rx, x, NULL);
        else if (mode == 2) v34_rx_put(&rx, x, &t);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "count"))
        return count_mode(atoi(argv[2]), atoi(argv[3]), strtoul(argv[4], NULL, 10));

    printf("  decoder state: sizeof(v34_rx_t) = %u bytes, v34_params_t = %u, v34_tx_t = %u (depth %d)\n",
           (unsigned)sizeof(v34_rx_t), (unsigned)sizeof(v34_params_t), (unsigned)sizeof(v34_tx_t),
           V34_VIT_DEPTH);
#ifdef TEST_SHORT
    printf("  wall time under the emulator, not target time: see `make cost-arm`\n");
#endif
    printf("    config                       encoder    decoder    slicer\n");
    printf("                                 per sym    per sym    per sym\n");
    /* the server's normal configuration: 16 states, no precoding */
    bench(3200, 4800,  V34_TRELLIS_16, NULL, 13.0, "");
    bench(3200, 9600,  V34_TRELLIS_16, NULL, 16.0, "");
    bench(3200, 19200, V34_TRELLIS_16, NULL, 23.0, "");
    bench(3000, 28800, V34_TRELLIS_16, NULL, 33.0, "");
    bench(3200, 31200, V34_TRELLIS_16, NULL, 34.0, "");
    bench(3200, 19200, V34_TRELLIS_16, H,    23.0, "precoded");
    /* optional codes */
    bench(3200, 19200, V34_TRELLIS_32, NULL, 23.0, "");
    bench(3200, 19200, V34_TRELLIS_64, NULL, 23.0, "");
    T_REPORT("test_bench");
}
