/* v90d/v34 - Phase 3 / 4 training signals as point sequences (V.34 10.1.3):
 * S, S-bar, PP, TRN, and the scrambled, differentially encoded 4- and
 * 16-point modulation of 10.1.3.3 / 10.1.3.9 that carries J, MP, E and, in
 * V.90, Ja, CP, E and SCR. */
#include "v34_priv.h"

static v34_pt_t mkpt(int re, int im)
{
    v34_pt_t p;
    p.re = (int16_t)(re * V34_ONE);
    p.im = (int16_t)(im * V34_ONE);
    return p;
}

/* ---- S, 10.1.3.7 ---------------------------------------------------- */

v34_pt_t v34_s_point(unsigned i, unsigned len)
{
    /* the last symbol is point 0 turned 90 degrees counterclockwise */
    return ((len - 1 - i) & 1) ? mkpt(1, 1) : mkpt(-1, 1);
}

v34_pt_t v34_sbar_point(unsigned i)
{
    /* the first symbol is point 0 turned 180 degrees, the other one is
     * point 0 turned 270 degrees counterclockwise */
    return (i & 1) ? mkpt(1, -1) : mkpt(-1, -1);
}

/* ---- PP, 10.1.3.6 --------------------------------------------------- */

unsigned v34_pp_phase(unsigned i)
{
    unsigned k = (i / 4) % 72, I = i % 4;
    return (k * I + ((k % 3 == 1) ? 4 : 0)) % 12;
}

void v34_pp_point(unsigned i, int16_t *re, int16_t *im)
{
    /* cos(n * 30 degrees), Q14 */
    static const int16_t c30[12] = {
        16384, 14189, 8192, 0, -8192, -14189, -16384, -14189, -8192, 0, 8192, 14189 };
    unsigned ph = v34_pp_phase(i);
    *re = c30[ph];
    *im = c30[(ph + 9) % 12];                               /* sin = cos(x - 90) */
}

/* ---- TRN and the J-type modulation ---------------------------------- */

void v34_tmod_set_points(v34_tmod_t *t, int points)
{
    t->nbits = (points == 16) ? 4 : 2;
}

void v34_tmod_init(v34_tmod_t *t, int poly, int points)
{
    v34_scr_init(&t->scr, poly);
    t->z = 0;
    v34_tmod_set_points(t, points);
}

/* scramble nbits (bit 0 first in time) and split into I and Q */
static void tmod_bits(v34_tmod_t *t, unsigned bits, unsigned *I, unsigned *Q)
{
    unsigned s = 0, i;
    for (i = 0; i < t->nbits; i++)
        s |= (unsigned)v34_scramble(&t->scr, (int)((bits >> i) & 1)) << i;
    *I = s & 3;                 /* I = 2 * I2 + I1, I1 first in time */
    *Q = (s >> 2) & 3;          /* Q = 2 * Q2 + Q1 (16 points only) */
}

v34_pt_t v34_tmod_trn(v34_tmod_t *t)
{
    unsigned I, Q;
    int re, im;
    tmod_bits(t, 0xF, &I, &Q);
    t->z = (uint8_t)I;          /* "final symbol of the transmitted TRN" */
    v34_map_point(Q, I, &re, &im);
    return mkpt(re, im);
}

v34_pt_t v34_tmod_put(v34_tmod_t *t, unsigned bits)
{
    unsigned I, Q;
    int re, im;
    tmod_bits(t, bits, &I, &Q);
    t->z = (uint8_t)((t->z + I) & 3);
    v34_map_point(Q, t->z, &re, &im);
    return mkpt(re, im);
}

void v34_tdem_set_points(v34_tdem_t *t, int points)
{
    t->nbits = (points == 16) ? 4 : 2;
}

void v34_tdem_init(v34_tdem_t *t, int poly, int points)
{
    v34_scr_init(&t->scr, poly);
    t->z = 0;
    v34_tdem_set_points(t, points);
}

/* nearest point of the 4- or 16-point training constellation -> rotation
 * and quarter-constellation label */
static void tdem_slice(const v34_tdem_t *t, v34_pt_t r, unsigned *rot, unsigned *Q)
{
    int x, y, lim = (t->nbits == 4) ? 3 : 1;
    unsigned label = 0;

    x = 2 * ((r.re + 1024 * V34_ONE) / (2 * V34_ONE)) - 1024 + 1;   /* nearest odd */
    y = 2 * ((r.im + 1024 * V34_ONE) / (2 * V34_ONE)) - 1024 + 1;
    if (x > lim) x = lim;
    if (x < -lim) x = -lim;
    if (y > lim) y = lim;
    if (y < -lim) y = -lim;
    (void)v34_unmap_point(x, y, &label, rot);
    *Q = label;
}

static unsigned tdem_bits(v34_tdem_t *t, unsigned I, unsigned Q)
{
    unsigned s = (I & 3) | (Q & 3) << 2, out = 0, i;
    for (i = 0; i < t->nbits; i++)
        out |= (unsigned)v34_descramble(&t->scr, (int)((s >> i) & 1)) << i;
    return out;
}

unsigned v34_tdem_trn(v34_tdem_t *t, v34_pt_t r)
{
    unsigned rot, Q;
    tdem_slice(t, r, &rot, &Q);
    t->z = (uint8_t)rot;
    return tdem_bits(t, rot, Q);
}

unsigned v34_tdem_put(v34_tdem_t *t, v34_pt_t r)
{
    unsigned rot, Q, I;
    tdem_slice(t, r, &rot, &Q);
    I = (rot - t->z) & 3;
    t->z = (uint8_t)rot;
    return tdem_bits(t, I, Q);
}
