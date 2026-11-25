/* What the three Phase 2 exchanges share: p2d (V.90 9.2.1, digital modem),
 * p2ans (V.34 11.2.1.2, answer modem) and p2call (V.34 11.2.1.1 and V.90
 * 9.2.2, call / analogue modem). Their timing constants, V.34 Tables 1, 2
 * and 8, the tone hunt over our own probing signal, and p2call's projection
 * of the probing result. For their .c files only: the short names (MS ...)
 * would clash with other files' own. */
#ifndef V90D_MODEM_P2COMMON_H
#define V90D_MODEM_P2COMMON_H
#include <stdint.h>
#include "probe.h"

#define MS(x) ((x) * 8)
#define TONE_SEEN 160           /* 20 ms of steady carrier = tone detected */
#define TURN 320                /* the 40 ms turn-round */
#define TAIL 80                 /* 10 ms of tone after a reversal */
/* L2 analysed by the answering sides, p2d and p2ans */
#define PROBE_BLOCKS 23         /* 460 ms of L2 analysed (not to exceed 500 ms) */
/* L2 analysed by the calling side, p2call: read from 10 ms in for 280 ms,
 * our tone 290 ms after L2 began. V.34 11.2.1.1.5 and V.90 9.2.2.1.8 allow
 * up to 500 ms; the far end waits for our tone 600 ms (V.34 11.2.2.2.3;
 * V.90's digital modem 650 ms, 9.2.1.2.5) plus a round trip from the start
 * of its L2, and so has 300 ms or more to hear it over the echo of its own
 * L2, where 500 ms left 100. A real answering modem used nearly all of the
 * 100 ms that 500 ms leave (its tone A came 92 to 103 ms after ours
 * arrived) and ignored the reversal that followed (live calls reported by
 * an independent softmodem, which reads 300 ms). 14 blocks of 20 ms still
 * average one or two jitter-buffer slips. */
#define PROBE_BLOCKS_CALL 14

/* L1 is sent for 160 ms. P2D_L1 (and p2call's P2L_L1, P2L_A_L1) ends at
 * t >= MS(160) - 1, P2N_L1 at t >= MS(160): one sample apart, deliberately
 * left so, since moving either moves the L1 / L2 boundary on the line and
 * every handshake recorded after it. */

/* V.34 Table 1: the symbol rates, Hz (2743 and 3429 rounded) */
extern const int16_t p2_sr[6];
/* V.34 Table 2: the carriers as d / e of the symbol rate, [index][low /
 * high] = { d, e } */
extern const int8_t p2_car[6][2][2];
/* V.34 Table 8: the highest data rate per symbol rate, units of 2400 bit/s */
extern const int8_t p2_top[6];
/* Table 1's carrier in Hz: p2_sr[k] * d / e rounded to the nearest
 * (k 0..5, hi 0 or 1) */
int p2_carrier_hz(int k, int hi);

/* a tone (bin m) standing out of our own probing echo for two blocks
 * running: prx takes x; tone_blocks counts the blocks */
int tone_under_probe(proberx_t *prx, int *tone_blocks, int16_t x, int m);

/* 10^(d/100) for d in 0.1 dB, 0..600: 2^(d * 0.0332193); 1 for d <= 0 */
uint32_t db10_lin(int d);
/* 10^(d/100) in Q16, d in 0.1 dB, -300..300 */
uint32_t lin_q16(int d);
/* the pre-emphasis templates of Figures 1 and 2 (Tables 3, 4) at f Hz for
 * symbol rate s, 0.1 dB */
int template_db10(int idx, int f, int s);
/* What a linear equaliser makes of symbol rate k (0..5) on carrier c, the
 * answerer sending with pre-emphasis filter idx and r dB less power, from
 * the per-tone SNRs and levels of the probing result (snr, rx_level:
 * PROBE_NBIN + 1 entries, 0.1 dB and RMS) and each tone's share of the
 * whole signal (share), 0.1 dB */
int band(const int *snr, const int *rx_level, int k, int c, int idx, int r, int f_db10, const int *share);

#endif
