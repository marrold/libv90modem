/* Record what the calling modem's A/D receives in a simulated Bell call:
 * the library's caller (orig, Bell only) against the library's answerer
 * (V.22, V.21, Bell 103 and Bell 212A on) over sim/v32line (10 dB loss,
 * echoes, -45 dBm0 noise, 1 Hz, 50 ppm), until SECONDS seconds after data
 * mode. The answerer's terminal sends a sentence over and over, so that the
 * caller receives characters (Bell 103 hands on only characters, not the
 * idle marks). rig/bench_bell replays it into the caller alone. Host only.
 *
 *   rec_bell 103|212 SECONDS OUT
 *
 * OUT: "BELL0001", the caller's modes and the sample at which it reached
 * data mode (32-bit little-endian each), then every A/D sample as a 16-bit
 * little-endian value. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "orig.h"
#include "answer.h"
#include "v14.h"
#include "v32line.h"

static answer_t A;
static orig_t O;
static v32line_t L;
static v14_t a_dte;
static uint32_t gen = 0x1234567u;
static const char text[] = "the quick brown fox jumps over the lazy dog. ";

/* the caller's data, as rig/bench_bell sends it: the first 300 data bits
 * are marks (an idle start-stop line: a Bell 103 answerer connects on
 * 200 ms of the caller's mark), then pseudo-random */
static int nget;
static int get(void *u) { (void)u; if (nget < 300) { nget++; return 1; } gen = gen * 1103515245u + 12345u; return (int)((gen >> 16) & 1); }
static void put(void *u, int bit) { (void)u; (void)bit; }
static double peer(void *u, double rx)
{
    long q = lrint(rx);
    (void)u;
    return answer_tick(&A, (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q));
}
static void put32(FILE *f, uint32_t v) { int i; for (i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 0xff), f); }

int main(int argc, char **argv)
{
    v32line_cfg lc; orig_cfg oc; answer_cfg ac;
    FILE *f;
    int16_t rx = 0;
    long n, t_data = -1, end = 60L * 8000;
    if (argc < 4) { fprintf(stderr, "usage: rec_bell 103|212 SECONDS OUT\n"); return 2; }
    memset(&lc, 0, sizeof lc);
    lc.mulaw = 1; lc.seed = 3; lc.delay = 160; lc.loss_db = 10;
    lc.near_db = 12; lc.far_db = 15; lc.c_near_db = 12; lc.c_far_db = 18;
    lc.noise_a_dbm0 = lc.noise_c_dbm0 = -45; lc.freq_offset = 1; lc.ppm = 50;
    v32line_init(&L, &lc);
    v14_init(&a_dte);
    memset(&oc, 0, sizeof oc);
    oc.level = 3000; oc.modes = strcmp(argv[1], "212") == 0 ? ORIG_MODE_BELL212A | ORIG_MODE_BELL103 : ORIG_MODE_BELL103;
    oc.getbit = get; oc.putbit = put;
    orig_init(&O, &oc);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_BELL103 | ANS_MODE_BELL212A;
    ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &a_dte;
    answer_init(&A, &ac);
    f = fopen(argv[3], "wb");
    if (!f) return 1;
    fwrite("BELL0001", 1, 8, f); put32(f, oc.modes); put32(f, 0);
    for (n = 0; n < end; n++) {
        fputc(rx & 0xff, f); fputc(((uint16_t)rx >> 8) & 0xff, f);
        if (v14_tx_pending(&a_dte) < 64) v14_write(&a_dte, (const uint8_t *)text, (int)sizeof text - 1);
        rx = v32line_tick(&L, orig_tick(&O, rx), peer, NULL);
        if (t_data < 0 && orig_moving(&O)) { t_data = n + 1; end = t_data + atol(argv[2]) * 8000L; }
        if (O.phase == ORIG_FAILED) break;
    }
    fseek(f, 12, SEEK_SET); put32(f, (uint32_t)t_data);
    fclose(f);
    printf("rec_bell: modulation %d, %d bit/s, data mode at sample %ld, %ld samples\n", O.modulation, O.bitrate, t_data, n);
    return t_data < 0;
}
