/* V.92 Phase 3 / Phase 4 training signals at the symbol level: which sign
 * or level each symbol has and how the bits of the sequences of v92_seq.h
 * ride on them. Both directions of each mapping are here: the digital modem
 * needs the receiving half of the upstream signals and the sending half of
 * the downstream ones; tests and line simulators need the rest.
 *
 * No filtering, timing or detection: one call per symbol, integer only.
 *
 * TWO-POINT SIGNALS (v92_sgn_t). One bit per symbol, carried by the sign.
 *   upstream, scrambler GPA:   TRN1u (8.5.7), Ja (8.5.4), CPt (8.5.1),
 *                              E1u (8.5.2); amplitude LU;
 *                              line bit 0 = positive, 1 = negative
 *   downstream, scrambler GPC: Jd, Jp, Jp' (8.6.2 to 8.6.4), SCR (8.6.6);
 *                              amplitude: the PCM codeword of Ucode UINFO;
 *                              line bit 1 = positive, 0 = negative
 * TRN1u and SCR are scrambled ones sent as they leave the scrambler. The
 * sequences are scrambled and then differentially encoded: line bit =
 * scrambled bit XOR previous line bit, the previous line bit of the first
 * one being the last symbol of the signal before (TRN1u, TRN1d, the
 * previous sequence).
 *
 * MULTI-LEVEL SIGNALS (v92_ml_t), upstream only: TRN2u and the sequences
 * sent "using the TRN2u modulation" during training: SUVu, CPu, E2u (8.7).
 * 2 bits (4-point) or 3 bits (8-point) per symbol, Tables 28 and 29.
 */
#ifndef V90D_V92_SIG_H
#define V90D_V92_SIG_H
#include "v92.h"
#include "../v90/v90_scr.h"

/* +1 / -1 for a line bit */
#define V92_UP_SIGN(sbit)  ((sbit) ? -1 : 1)     /* 8.5.7 */
#define V92_DN_SIGN(sbit)  ((sbit) ? 1 : -1)     /* 8.6.2 */

/* ---- two-point signals --------------------------------------------------- */

typedef struct {
    v90_scr_t scr;
    uint8_t   gpc;          /* 0: GPA (analogue modem), 1: GPC (digital modem) */
    uint8_t   last;         /* previous line bit */
} v92_sgn_t;

/* Scrambler (or descrambler) zeroed, previous line bit 0. */
void v92_sgn_init(v92_sgn_t *s, int gpc);
/* Zero the scrambler only ("The scrambler shall be initialized to zero prior
 * to the transmission of TRN1u", every TRN1u segment). */
void v92_sgn_scr_reset(v92_sgn_t *s);

/* Transmit side. Each returns the line bit of one symbol. */
int  v92_sgn_tx_trn(v92_sgn_t *s);               /* TRN1u, SCR: a scrambled one */
int  v92_sgn_tx_bit(v92_sgn_t *s, int bit);      /* one bit of a sequence */

/* Receive side, one line bit (the sign decision) in, one bit out.
 * rx_trn: the descrambled bit. It is 1 once 23 symbols of TRN1u / SCR have
 * gone by, and steadily 0 if the polarity of the line is inverted.
 * rx_bit: differential decoding, then descrambling: the bit of the sequence.
 * An inverted line does not matter here, except that a descrambler that
 * went through an inverted TRN1u gets up to 23 bits wrong after the change
 * from rx_trn to rx_bit: the 24 ones that open Ja and the first CPt cover
 * that. */
int  v92_sgn_rx_trn(v92_sgn_t *s, int sbit);
int  v92_sgn_rx_bit(v92_sgn_t *s, int sbit);

/* ---- Ru and Su ----------------------------------------------------------- */

/* Line bit (upstream sense) of symbol n = 0, 1, .. of Ru (8.5.5:
 * + + + - - -), or of its inverse when bar != 0. */
int  v92_ru_sbit(unsigned n, int bar);

/* Level of symbol n of Su (8.5.6: +a, 0, +a, -a, 0, -a with
 * a = sqrt(3/2) * LU), or of its inverse when bar != 0, for LU = lu. The
 * mean square over the 6 symbols is LU^2. */
int32_t v92_su_level(unsigned n, int bar, int32_t lu);

/* ---- TRN2u modulation ---------------------------------------------------- */

typedef struct {
    v90_scr_t scr;
    uint8_t   nbits;        /* bits per symbol: 2 (4-point) or 3 (8-point) */
    uint8_t   msb_first;    /* see v92_ml_set_msb_first() */
    uint8_t   last;         /* previous transmitted (received) sign bit */
} v92_ml_t;

/* pt8: Jp bit 48 (bit 49 in a rate renegotiation). The scrambler is zeroed
 * ("shall be reset at the beginning of TRN2u"); last_sign is the sign bit
 * (1 = negative) of the symbol before: the last one of E1u (8.7.6). A
 * receiver uses the same call. */
void v92_ml_init(v92_ml_t *s, int pt8, int last_sign);

/* Tables 28 and 29 give each symbol as a pattern "MSB:LSB" whose MSB is the
 * sign, without saying which of the 2 or 3 scrambler output bits of a
 * symbol is the MSB (clause 8 states the order of bits for other tables
 * only). Default (on = 0): the first bit in time is the LSB, as for every
 * integer field of the Recommendation, so the last bit of the group is the
 * sign. on = 1: the first bit in time is the MSB (the sign). */
void v92_ml_set_msb_first(v92_ml_t *s, int on);

/* One symbol. bits: nbits data bits, first in time first; NULL = binary
 * ones (TRN2u itself). They are scrambled (GPA), grouped, and the sign bit
 * is differentially encoded. Returns the level as a signed odd integer:
 * +-1, +-3 (4-point) or +-1 .. +-7 (8-point), in units of LU / sqrt(5) or
 * LU / sqrt(21). */
int  v92_ml_tx(v92_ml_t *s, const uint8_t *bits);

/* The inverse: a decided level (signed odd integer as above) in, nbits
 * descrambled bits out. An inverted line does not matter. */
void v92_ml_rx(v92_ml_t *s, int level, uint8_t *bits);

/* Linear value of a TRN2u level for LU = lu: level * lu / sqrt(5) (pt8 = 0)
 * or level * lu / sqrt(21) (pt8 != 0), rounded to nearest. */
int32_t v92_trn2u_level(int level, int pt8, int32_t lu);

/* Nearest TRN2u level (signed odd integer) to the received value r. */
int  v92_trn2u_slice(int32_t r, int pt8, int32_t lu);

#endif
