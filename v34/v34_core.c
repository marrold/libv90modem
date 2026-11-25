/* v90d/v34 - small shared pieces: the tables of 9.6.3 as printed in the
 * Recommendation, the three convolutional encoders, the precoder arithmetic
 * and the mapper's point <-> label conversion. The scrambler (clause 7) is
 * in v34.h. */
#include "v34_priv.h"

/* ---- Figure 9 and Table 13 ------------------------------------------ */

/* Figure 9/V.34, the 16 labelled points, rows im = -3, -1, 1, 3 and
 * columns re = -3, -1, 1, 3. The 8 subsets are the cosets of the lattice
 * {(4i, 4j), i + j even}, so the labelling repeats with period 8 in each
 * coordinate and the figure is one full period. */
const uint8_t v34_fig9[4][4] = {
    { 0, 7, 4, 3 },     /* im = -3: 000 111 100 011 */
    { 5, 2, 1, 6 },     /* im = -1: 101 010 001 110 */
    { 4, 3, 0, 7 },     /* im =  1: 100 011 000 111 */
    { 1, 6, 5, 2 },     /* im =  3: 001 110 101 010 */
};

const int8_t v34_subset_rep[8][2] = {
    {  1,  1 }, {  1, -1 }, { -1, -1 }, { -1,  1 },
    { -3,  1 }, {  1,  3 }, {  3, -1 }, { -1, -3 },
};

unsigned v34_subset(int re, int im)
{
    /* +1024 keeps the operand positive; 1024 / 2 is a multiple of 4 */
    unsigned ix = ((unsigned)(re + 3 + 1024) >> 1) & 3;
    unsigned iy = ((unsigned)(im + 3 + 1024) >> 1) & 3;
    return v34_fig9[iy][ix];
}

/* Table 13/V.34 exactly as printed: row s(2m), column s(2m+1). */
const uint8_t v34_tab13[8][8] = {
    { 0x0, 0x0, 0x1, 0x1, 0x8, 0x8, 0x9, 0x9 },
    { 0x3, 0x2, 0x2, 0x3, 0xB, 0xA, 0xA, 0xB },
    { 0x5, 0x5, 0x4, 0x4, 0xD, 0xD, 0xC, 0xC },
    { 0x6, 0x7, 0x7, 0x6, 0xE, 0xF, 0xF, 0xE },
    { 0x8, 0x8, 0x9, 0x9, 0x0, 0x0, 0x1, 0x1 },
    { 0xB, 0xA, 0xA, 0xB, 0x3, 0x2, 0x2, 0x3 },
    { 0xD, 0xD, 0xC, 0xC, 0x5, 0x5, 0x4, 0x4 },
    { 0xE, 0xF, 0xF, 0xE, 0x6, 0x7, 0x7, 0x6 },
};

/* ---- convolutional encoders, Figures 10 to 12 ----------------------- */

unsigned v34_conv_y0(int trellis, unsigned state)
{
    switch (trellis) {
    case V34_TRELLIS_32: return (state >> 4) & 1;
    case V34_TRELLIS_64: return (state >> 5) & 1;
    default:             return (state >> 3) & 1;
    }
}

unsigned v34_conv_next(int trellis, unsigned state, unsigned y4321)
{
    unsigned y1 = y4321 & 1, y2 = (y4321 >> 1) & 1;
    unsigned y3 = (y4321 >> 2) & 1, y4 = (y4321 >> 3) & 1;
    unsigned d1 = state & 1, d2 = (state >> 1) & 1, d3 = (state >> 2) & 1;
    unsigned d4 = (state >> 3) & 1, d5 = (state >> 4) & 1, d6 = (state >> 5) & 1;
    unsigned n1, n2, n3, n4, n5 = 0, n6 = 0;

    switch (trellis) {
    case V34_TRELLIS_32:
        /* Figure 11: five delays in a ring, Y2, Y1, Y4, Y2 added between */
        n1 = d5;
        n2 = d1 ^ y2;
        n3 = d2 ^ y1;
        n4 = d3 ^ y4;
        n5 = d4 ^ y2;
        break;
    case V34_TRELLIS_64:
        /* Figure 12. a1 is the node after the first adder (d1 + d2), a3 the
         * node after the Y1 adder (d2 + Y1). The two AND gates are
         * Y2 & d3 (upper) and d3 & a3 (lower). */
        {
            unsigned a1 = d1 ^ d2;
            unsigned a3 = d2 ^ y1;
            n1 = y4 ^ (d3 & a3) ^ a1;
            n2 = a1 ^ y3 ^ (y2 & d3) ^ d4;
            n3 = a3 ^ d3;
            n4 = d3;
            n5 = d6;
            n6 = d5 ^ y2 ^ d3;
        }
        break;
    default:
        /* Figure 10: Y0 fed back to the first delay and to the first adder;
         * Y2, Y2, Y1 added after delays 1, 2, 3 */
        n1 = d4;
        n2 = d1 ^ y2 ^ d4;
        n3 = d2 ^ y2;
        n4 = d3 ^ y1;
        break;
    }
    return n1 | n2 << 1 | n3 << 2 | n4 << 3 | n5 << 4 | n6 << 5;
}

/* ---- bit inversion pattern, Table 12 -------------------------------- */

unsigned v34_v0(unsigned J, unsigned half)
{
    /* left-most bit = first half data frame of the superframe */
    static const uint16_t pat7 = 0x1DFE;   /* 01 11 01 11 11 11 10       */
    static const uint16_t pat8 = 0x77FA;   /* 01 11 01 11 11 11 10 10    */
    if (J == 8)
        return (pat8 >> (15 - (half & 15))) & 1;
    return (pat7 >> (13 - (half % 14))) & 1;
}

/* ---- precoder arithmetic, 9.6.2 items 1) to 3) ---------------------- */

/* round v / 2^sh to the nearest integer, a tie going to the smaller
 * magnitude */
static int32_t round_tz(int64_t v, int sh)
{
    int64_t bias = ((int64_t)1 << (sh - 1)) - 1;
    if (v >= 0)
        return (int32_t)((v + bias) >> sh);
    return -(int32_t)((-v + bias) >> sh);
}

void v34_precode_pc(const v34_params_t *p, int32_t (*xh)[2],
                    int32_t *pn, int32_t *cn)
{
    int64_t qr = 0, qi = 0;
    int k, w = p->map.w;

    if (!p->precode) {
        pn[0] = pn[1] = cn[0] = cn[1] = 0;
        return;
    }
    for (k = 0; k < 3; k++) {       /* (9-28), Q7 * Q14 = Q21 */
        int32_t hr = p->cfg.h[k][0], hi = p->cfg.h[k][1];
        qr += (int64_t)xh[k][0] * hr - (int64_t)xh[k][1] * hi;
        qi += (int64_t)xh[k][0] * hi + (int64_t)xh[k][1] * hr;
    }
    pn[0] = round_tz(qr, 14);       /* multiples of 2^-7 */
    pn[1] = round_tz(qi, 14);
    /* multiples of 2^w, returned in integer units */
    cn[0] = round_tz(pn[0], V34_Q + w) * (1 << w);
    cn[1] = round_tz(pn[1], V34_Q + w) * (1 << w);
}

/* ---- mapper, 9.6.1 -------------------------------------------------- */

void v34_map_point(unsigned label, unsigned rot, int *re, int *im)
{
    int x = v34_constel[label][0], y = v34_constel[label][1], t;
    for (rot &= 3; rot; rot--) {    /* 90 degrees clockwise: (x, y) -> (y, -x) */
        t = x; x = y; y = -t;
    }
    *re = x; *im = y;
}

int v34_unmap_point(int re, int im, unsigned *label, unsigned *rot)
{
    /* The quarter constellation lies on 4Z^2 + (1,1); a clockwise quarter
     * turn moves it to (1,3), (3,3), (3,1) modulo 4. */
    unsigned mx, my, k;
    int t;
    uint16_t l;

    if (!(re & 1) || !(im & 1) || re < -45 || re > 45 || im < -45 || im > 45)
        return -1;
    mx = (unsigned)(re + 1024) & 3;
    my = (unsigned)(im + 1024) & 3;
    k = (mx == 1) ? (my == 1 ? 0 : 1) : (my == 3 ? 2 : 3);
    *rot = k;
    for (; k; k--) {                /* undo: 90 degrees counterclockwise */
        t = re; re = -im; im = t;
    }
    if (re < -43 || im < -43)
        return -1;
    l = v34_label[(im + 43) >> 2][(re + 43) >> 2];
    if (l == 0xFFFF)
        return -1;
    *label = l;
    return 0;
}
