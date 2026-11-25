/* v90d/v34 - internal declarations shared by the library sources. */
#ifndef V90D_V34_PRIV_H
#define V90D_V34_PRIV_H

#include "v34.h"

/* Figure 5/V.34 (v34_constel.c, generated) */
extern const int8_t   v34_constel[V34_QPOINTS][2];
extern const uint16_t v34_label[23][23];

/* Table 13/V.34: [s(2m)][s(2m+1)] -> Y4 Y3 Y2 Y1 */
extern const uint8_t v34_tab13[8][8];
/* Figure 9/V.34 read as a 4x4 tile: [iy][ix], ix = (re+3)/2, iy = (im+3)/2 */
extern const uint8_t v34_fig9[4][4];
/* one grid point of every subset (inside the tile of Figure 9) */
extern const int8_t  v34_subset_rep[8][2];

/* Convolutional encoders of Figures 10, 11, 12. state bit i-1 holds delay
 * element i counted from the left of the figure; Y0 is the last element.
 * y4321 = Y4 << 3 | Y3 << 2 | Y2 << 1 | Y1. */
unsigned v34_conv_next(int trellis, unsigned state, unsigned y4321);
unsigned v34_conv_y0(int trellis, unsigned state);

/* Bit inversion V0 (Table 12) for half data frame number half (0 is the
 * first of a superframe). */
unsigned v34_v0(unsigned J, unsigned half);

/* Precoder arithmetic 9.6.2, shared by encoder and decoder so that both
 * round identically. xh: x(n-1), x(n-2), x(n-3) in Q7. */
void v34_precode_pc(const v34_params_t *p, int32_t (*xh)[2],
                    int32_t *pn, int32_t *cn);

/* Decoder metric scale: squared Q7 distance >> V34_M_SHIFT = 256 per unit^2 */
#define V34_M_SHIFT 6
#define V34_M_MAX   (1u << 24)
#define V34_M_BIG   (1u << 26)      /* the subset has no valid point */

/* Nearest valid point of 2D subset s (Figure 9 label) to (re, im) in Q7:
 * a point of the L-point constellation when there is no precoding, any
 * grid point otherwise. Returns the metric, V34_M_BIG if the subset has no
 * valid point. pt receives the point in integer units. (The decoder takes
 * all eight at once, v34_slice_subsets.) */
uint32_t v34_slice_subset(const v34_params_t *p, int re, int im, unsigned s,
                          int16_t pt[2]);

/* All eight at once (what the decoder calls for the 32- and 64-state
 * codes): m[s], pt[s] for s = 0..7, the same as v34_slice_subset. */
void v34_slice_subsets(const v34_params_t *p, int re, int im, uint32_t m[8],
                       int16_t pt[8][2]);

/* Same for the 4-way partition the 16-state code needs: l = 0..3 is the
 * number of clockwise quarter turns (the two low bits of the subset label). */
uint32_t v34_slice_class(const v34_params_t *p, int re, int im, unsigned l,
                         int16_t pt[2]);

/* All four at once (what the decoder calls): m[l], pt[l] for l = 0..3. Same
 * metrics as v34_slice_class. */
void v34_slice_classes(const v34_params_t *p, int re, int im, uint32_t m[4],
                       int16_t pt[4][2]);

static inline int v34_inside(const v34_params_t *p, int re, int im)
{
    unsigned ix, iy, i;
    if (re < -45 || re > 45 || im < -45 || im > 45)
        return 0;
    ix = (unsigned)(re + 45) >> 1;
    iy = (unsigned)(im + 45) >> 1;
    i = iy * 46 + ix;
    return (p->inside[i >> 3] >> (i & 7)) & 1;
}

#endif
