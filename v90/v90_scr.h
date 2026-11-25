/* V.34 clause 7 self-synchronising scrambler, both generating polynomials.
 *
 *   GPC = 1 + x^-18 + x^-23   (7-1/V.34): used by the V.90 digital modem (5.3)
 *   GPA = 1 + x^-5  + x^-23   (7-2/V.34): used by the V.90 analogue modem (6.5)
 *
 * The scrambler divides the data by the polynomial:
 *   out[n] = in[n] ^ out[n-a] ^ out[n-23],  a = 18 (GPC) or 5 (GPA).
 * The descrambler multiplies: out[n] = in[n] ^ in[n-a] ^ in[n-23]; it
 * self-synchronises after 23 received bits.
 *
 * The state is one 32-bit word holding the last 23 scrambled bits (bit 0 is
 * the most recent). Zero is the initialised state the Recommendations ask for.
 */
#ifndef V90D_V90_SCR_H
#define V90D_V90_SCR_H
#include <stdint.h>

typedef struct { uint32_t reg; } v90_scr_t;

void v90_scr_init(v90_scr_t *s);

int v90_scramble_gpc(v90_scr_t *s, int bit);     /* returns the scrambled bit */
int v90_descramble_gpc(v90_scr_t *s, int bit);   /* returns the data bit */
int v90_scramble_gpa(v90_scr_t *s, int bit);
int v90_descramble_gpa(v90_scr_t *s, int bit);

/* n bits in place, one a byte (0/1; bit 0 of each is taken), element 0
 * first: the same as n calls of the forms above */
void v90_scramble_gpc_bits(v90_scr_t *s, uint8_t *bits, int n);
void v90_descramble_gpc_bits(v90_scr_t *s, uint8_t *bits, int n);
void v90_scramble_gpa_bits(v90_scr_t *s, uint8_t *bits, int n);
void v90_descramble_gpa_bits(v90_scr_t *s, uint8_t *bits, int n);

#endif
