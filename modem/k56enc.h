/* K56flex's data encoder (docs/history/k56-facts.md, "Data", "Robbed
 * bits"): frames of 8 symbols, the scrambled data bits in, mu-law octets
 * out, and back.
 *
 * A frame's bits, in time order (one per byte, 0/1, already GPA-scrambled):
 * the first K' are the shell index (bit 0 first), shifted left by L - n on
 * a robbed-bit line so that its range is K bits; V.34's shell mapper (9.4,
 * M rings of the rate and pad) gives the rings of symbols 0..7. Then for
 * each symbol: a sign bit (1 positive) unless it is a shaping symbol, then
 * q = u - 1 bits, the low bits of the point within its ring (bit 0 forced
 * to 0 and not sent on a robbed symbol). Ring to group by k56_ring, point
 * g << q | bits to magnitude index m by k56_bios; the octet 0xff - m
 * positive, 0x7f - m negative.
 *
 * Every r-th data symbol over the call (r the rate's shaping period; the
 * first is data symbol r - 1) is a shaping symbol: negative when the
 * running sum of the values sent is above 0, else positive.
 *
 * Robbed bits (rbs bit k: symbol k of the 6-cycle, data symbol 0 at cycle
 * symbol 0; none at 32000 and 34000, where q = 0; 5 robbed as 6): data
 * symbol j of the 3-frame cycle (j = 0..23) is robbed when rbs bit j mod 6
 * is set. With n6 robbed in 6 and n in the frame, L = 0 2 4 4 6 8 8 for
 * n6 = 0..6: the frame takes R/1000 - L bits, K' = K - (L - n); at n6 = 3
 * it takes R/1000 - n, K' = K, no shift.
 *
 * Ruling C4: each data start (the first, and after every renegotiation or
 * retrain) is a new encoder: frame 0 of the cycle, running sum 0, shaping
 * counter 0 (and the caller's GPA scrambler at 0). */
#ifndef V90D_MODEM_K56ENC_H
#define V90D_MODEM_K56ENC_H
#include <stdint.h>
#include "../v34/v34.h"

#define K56ENC_N 8          /* symbols (octets) per frame */

typedef struct {
    v34_params_t   shell;   /* g2, g4, z8 (and map.M) for the rate's M */
    const uint8_t *ring;    /* k56_ring of the rate and pad, or NULL */
    const uint8_t *bios;    /* k56_bios of the rate and pad */
    int64_t        sum;     /* the running sum of the linear values sent */
    uint32_t       rob;     /* bit j: data symbol j of the 3-frame cycle robbed */
    uint8_t        rate;    /* rate code 1..13, 0 when the rate or pad is invalid */
    uint8_t        K, q, r; /* shell bits, bits within a ring, shaping period */
    uint8_t        n6;      /* robbed symbols in 6 (5 counted as 6) */
    uint8_t        frame;   /* place in the 3-frame cycle, 0..2 */
    uint8_t        shape;   /* data symbols since the last shaping one, 0..r-1 */
} k56enc_t;

/* rate code 1..13 (R = 30000 + 2000 rate), pad 0..2 (none, 3 dB, 6 dB),
 * rbs the robbed-bit mask (bits 0-5). A new encoder: frame 0 of the cycle,
 * running sum 0, shaping counter 0. An invalid rate or pad gives an
 * encoder that takes no bits and sends zeros (0xff). */
void k56enc_init(k56enc_t *e, int rate, int pad, uint8_t rbs);

/* the bits the next frame takes: R / 1000, less on a robbed-bit line */
int k56enc_bits(const k56enc_t *e);

/* one frame from k56enc_bits(e) scrambled bits */
void k56enc_frame(k56enc_t *e, const uint8_t *bits, uint8_t out[K56ENC_N]);

/* the inverse (for the tests and the simulated client): the bits of one
 * frame from its octets. Returns the bit count, or -1 when an octet is not
 * a point of the constellation (on a robbed symbol a code and the code with
 * its last bit inverted are the same point) or the rings are not a shell
 * index of the frame. The shaping signs are not checked. Uses the cycle
 * and the shaping counter only; keep a separate k56enc_t. */
int k56enc_unframe(k56enc_t *d, const uint8_t in[K56ENC_N], uint8_t *bits);

#endif
