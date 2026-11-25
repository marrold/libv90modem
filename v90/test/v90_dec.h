/* Reference decoder for the V.90 digital modem's signals over an ideal
 * channel. TEST CODE ONLY: it exists so that every encoder path can be
 * verified by a round trip, is written independently of the encoder (64-bit
 * arithmetic, closed-form inverse of the shaping precoder), and is never
 * linked into the modem.
 *
 * Inverse of 5.4.5 used here. Let g_j(k) be the received sign of position k
 * of shaping frame j and q_j(k) = g_j(k) XOR g_(j-1)(k). Every allowed
 * sequence of sign inversion rules cancels out of:
 *   x_j    = q_j(0)                       (position 0 carries no data)
 *   p_j(k) = q_j(k) XOR x_j               k even, k >= 2
 *   p_j(1) = q_j(1) XOR q_(j-1)(last odd position) XOR x_j
 *   p_j(k) = q_j(k) XOR q_j(k-2)          k odd, k >= 3
 * and p_j(1..) are the sign input bits in order (Table 3). A sequence of
 * rules that breaks the trellis of Figure 2 does not cancel, so a clean
 * round trip also shows the shaper stayed inside the constraint.
 */
#ifndef V90D_V90_DEC_H
#define V90D_V90_DEC_H
#include <stdint.h>
#include "../v90_enc.h"
#include "../v90_scr.h"

typedef struct {
    const v90_map_t *m;
    int alaw;
    uint8_t prev[6];      /* signs of the previous shaping frame, by position */
    uint8_t qodd;         /* q at the last odd position seen */
} v90dec_t;

/* state as after the transmitter's "initialized to zero" */
void v90dec_init(v90dec_t *d, const v90_map_t *m, int alaw);

/* One data frame from Ucodes and sign bits to D bits (still scrambled).
 * Returns D, -1 if a Ucode is not a member of its interval's constellation,
 * -2 if the labels decode to a number >= 2^K. */
int v90dec_symbols(v90dec_t *d, const uint8_t u[6], const uint8_t sign[6], uint8_t *bits);

/* Same, from six PCM octets. */
int v90dec_frame(v90dec_t *d, const uint8_t oct[6], uint8_t *bits);

/* Receive path for the two-point signals of Phase 3 (8.4.2, 8.4.3, 8.4.5):
 * the sign of each UINFO symbol, through the GPC descrambler. TRN1d carries
 * the scrambled bit as the sign; Jd and Jd' carry it differentially
 * (bit = sign XOR previous sign). Both keep the same descrambler and
 * "previous sign", as the transmitter runs one scrambler through all three. */
typedef struct { v90_scr_t scr; uint8_t prev; } v90dec_sign_t;
void v90dec_sign_init(v90dec_sign_t *d);
int  v90dec_trn1d(v90dec_sign_t *d, int sign);    /* returns the data bit */
int  v90dec_jd(v90dec_sign_t *d, int sign);       /* returns the data bit */

#endif
