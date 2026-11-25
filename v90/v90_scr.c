/* V.34 clause 7 scrambler. See v90_scr.h: the scrambler of v34.h, its
 * register kept here (bit 0 the previous bit, x^-k is bit k-1). */
#include "v90_scr.h"
#include "v34.h"

void v90_scr_init(v90_scr_t *s) { s->reg = 0; }

static int scr(v90_scr_t *s, int bit, int poly, int descramble)
{
    v34_scr_t t;
    int out;
    v34_scr_init(&t, poly);
    t.sr = s->reg;
    out = descramble ? v34_descramble(&t, bit) : v34_scramble(&t, bit);
    s->reg = t.sr;
    return out;
}

int v90_scramble_gpc(v90_scr_t *s, int bit)   { return scr(s, bit, V34_GPC, 0); }
int v90_descramble_gpc(v90_scr_t *s, int bit) { return scr(s, bit, V34_GPC, 1); }
int v90_scramble_gpa(v90_scr_t *s, int bit)   { return scr(s, bit, V34_GPA, 0); }
int v90_descramble_gpa(v90_scr_t *s, int bit) { return scr(s, bit, V34_GPA, 1); }

/* eight bits at a time through v34_scramble_n / v34_descramble_n */
static void scr_bits(v90_scr_t *s, uint8_t *bits, int n, int poly, int descramble)
{
    v34_scr_t t;
    v34_scr_init(&t, poly);
    t.sr = s->reg;
    while (n > 0) {
        int c = n < 8 ? n : 8, k;
        unsigned w = 0;
        for (k = 0; k < c; k++) w = (w << 1) | (bits[k] & 1u);
        w = descramble ? v34_descramble_n(&t, w, c) : v34_scramble_n(&t, w, c);
        for (k = c - 1; k >= 0; k--, w >>= 1) bits[k] = (uint8_t)(w & 1);
        bits += c; n -= c;
    }
    s->reg = t.sr;
}

void v90_scramble_gpc_bits(v90_scr_t *s, uint8_t *bits, int n)   { scr_bits(s, bits, n, V34_GPC, 0); }
void v90_descramble_gpc_bits(v90_scr_t *s, uint8_t *bits, int n) { scr_bits(s, bits, n, V34_GPC, 1); }
void v90_scramble_gpa_bits(v90_scr_t *s, uint8_t *bits, int n)   { scr_bits(s, bits, n, V34_GPA, 0); }
void v90_descramble_gpa_bits(v90_scr_t *s, uint8_t *bits, int n) { scr_bits(s, bits, n, V34_GPA, 1); }
