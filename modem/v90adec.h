/* V.90 analogue modem: the data mode decoder of the PCM downstream, the
 * inverse of the digital modem's encoder (V.90 5.4):
 *
 *   six decided Ucodes and signs of a data frame
 *     -> labels K0..K5 (5.4.4: label 0 the largest Ucode of the interval's
 *        constellation) -> R0 = K0 + M0 (K1 + M1 (K2 + ...)) -> b0..bK-1
 *     -> sign bits $0..$5 -> s0..sS-1, undoing the differential coding and
 *        whatever sign inversion rules the spectral shaper chose
 *   -> d0..dD-1 = s0..sS-1, b0..bK-1 (5.4.2), still scrambled (GPC).
 *
 * Signs, Sr = 0 (5.4.5.1): s_i = $i XOR $(i-1), $(-1) being $5 of the frame
 * before. Sr > 0: with g_j(k) the received sign of position k of shaping
 * frame j and q_j(k) = g_j(k) XOR g_(j-1)(k), every sequence of rules the
 * trellis of Figure 2 allows cancels out of
 *     x_j    = q_j(0)
 *     p_j(k) = q_j(k) XOR x_j                        k even, k >= 2
 *     p_j(1) = q_j(1) XOR q_(j-1)(last odd k) XOR x_j
 *     p_j(k) = q_j(k) XOR q_j(k-2)                   k odd, k >= 3
 * (rule A..D inverts the even positions by e_j and the odd ones by o_j,
 * with e_j = o_j XOR o_(j-1) by the trellis; Table 4's two differential
 * codes then leave only x_j = e_j XOR e_(j-1)), and p_j(1..) are the sign
 * bits in the order of Table 3. So the decoder needs neither the shaping
 * filter nor ld. All state starts at zero, as the encoder's does before
 * TRN2d and B1d (8.6.1, 8.6.5).
 *
 * Integer only, no allocation, no static state. */
#ifndef V90D_MODEM_V90ADEC_H
#define V90D_MODEM_V90ADEC_H
#include <stdint.h>
#include "../v90/v90_enc.h"

typedef struct {
    uint8_t K, Sr, D;
    uint8_t M[6];
    uint8_t lab[6][128];        /* label of Ucode u in interval i, 255 = not a member */
    uint8_t gprev[6];           /* signs of the previous shaping frame, by position */
    uint8_t qodd;               /* q at the last odd position of the previous shaping frame */
    uint8_t sprev;              /* Sr = 0: $5 of the previous data frame */
} v90adec_t;

/* The mapping parameters (from CPt or CP: v90_map_from_cp), state to zero.
 * Returns 0, or V90_ERR_RANGE when the map is not usable (v90_map_check). */
int v90adec_init(v90adec_t *d, const v90_map_t *m);
/* State to zero, the map kept (TRN2d, B1d). */
void v90adec_reset(v90adec_t *d);

/* One data frame: u[i], pos[i] (1 = positive) of intervals 0..5 -> bits[0..D-1].
 * Returns D, or -D when the frame was not one the encoder could have sent:
 * a Ucode outside its interval's constellation (taken as the nearest
 * member by Ucode) or labels that make R0 >= 2^K (R0 mod 2^K taken). The
 * bits are written either way. */
int v90adec_frame(v90adec_t *d, const uint8_t u[6], const uint8_t pos[6], uint8_t *bits);

#endif
