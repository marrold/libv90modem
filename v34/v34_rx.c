/* v90d/v34 - data mode decoder for the server: slicers, Viterbi decoder for
 * the 4D trellis codes of V.34 9.6.3 working on the channel-output points
 * y(n) = u(n) + c(n), inverse precoder, inverse mapper, differential
 * decoder, inverse shell mapper, inverse parser, descrambler.
 *
 * Fixed point, no allocation, no globals. See README.md for the structure
 * and the cost.
 *
 * Code structure used by the decoder (derived from Figure 9 and Table 13,
 * checked in test_constel.c): a 2D subset label is s = (a, l) with a the
 * top bit and l the two low bits; l counts clockwise quarter turns. For a
 * 4D symbol (s1, s2), Table 13 gives Y4..Y1 and the two labels also carry
 * one more bit, par = lsb(s1) ^ lsb(s2). The encoder arranges
 * par = Y0(m) ^ V0(m) (Y0 from the convolutional encoder state, V0 the
 * superframe bit inversion); that holds for y whatever the precoder adds,
 * which is exactly what the modulo encoder C0 is for. So a trellis branch
 * leaving state S with input bits Y is labelled by (Y restricted to the
 * bits the code uses, par = Y0(S) ^ V0), and its metric is the smallest
 * m1[s1] + m2[s2] over all pairs (s1, s2) carrying that label. */
#include <string.h>
#include "v34_priv.h"
#include "../modem/compiler.h"

#define PM_INF      (1u << 30)
/* Sanity windows for the two inverse-precoder recursions (rebuilt x(n)
 * against received x(n)). The received x carries the unwhitened noise,
 * which for strong coefficients is a good deal larger than the noise the
 * trellis decoder sees, and a false alarm on the decision path costs a
 * burst of data: so that path only restarts on a gross mismatch (runaway of
 * 1/H(D), which happens when the coefficients are not minimum phase). The
 * tentative path can afford to be nervous. */
#define GUARD_DEC_Q7   (6 * V34_ONE)
#define GUARD_TENT_Q7  (3 * V34_ONE / 2)
/* class slicer: search the points directly up to this many per quadrant */
#define BRUTE_PTS      8

/* ---- nearest-point searches ----------------------------------------- */

static inline uint32_t dist2(int re, int im, int x, int y)
{
    int32_t dx = re - x * V34_ONE, dy = im - y * V34_ONE;
    return (uint32_t)(dx * dx) + (uint32_t)(dy * dy);
}

/* neighbour offsets, in lattice steps: the 2Z^2 grid of all points, and the
 * checkerboard lattice {(4i, 4j), i + j even} of one subset */
static const int8_t nb_all[8][2] = {
    { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
static const int8_t nb_sub[8][2] = {
    { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 } };

/* (*px, *py) is the lattice point nearest to (re, im) but lies outside the
 * constellation. Find the nearest lattice point that is inside. The lattice
 * is (*px, *py) + step * (i, j), with i + j even if checker is set.
 * Returns 0 if the lattice has no point inside the constellation.
 *
 * Exact: (1) walk towards the origin (the points of smallest norm are
 * inside if any point is) until a point inside is found; (2) improve it
 * greedily; (3) scan every lattice point that could still be closer. */
static int nearest_inside(const v34_params_t *p, int re, int im, int *px, int *py,
                          int step, int checker)
{
    const int8_t (*nb)[2] = checker ? nb_sub : nb_all;
    int cx = *px, cy = *py, x = cx, y = cy, i, j, n, moved;
    uint32_t best, d;
    int32_t dx, dy, w;

    for (;;) {                          /* (1) */
        int bx = x, by = y;
        int32_t bn = x * x + y * y;
        moved = 0;
        for (i = 0; i < 8; i++) {
            int nx = x + step * nb[i][0], ny = y + step * nb[i][1];
            int32_t nn = nx * nx + ny * ny;
            if (nn < bn) { bn = nn; bx = nx; by = ny; moved = 1; }
        }
        if (!moved)
            break;
        x = bx; y = by;
        if (v34_inside(p, x, y))
            goto found;
    }
    /* at a point of smallest norm that is not inside: the only candidates
     * left are the other points of the same norm, all neighbours */
    for (i = 0; i < 8; i++) {
        int nx = x + step * nb[i][0], ny = y + step * nb[i][1];
        if (v34_inside(p, nx, ny)) { x = nx; y = ny; goto found; }
    }
    return 0;

found:
    best = dist2(re, im, x, y);
    do {                                /* (2) */
        moved = 0;
        for (i = 0; i < 8; i++) {
            int nx = x + step * nb[i][0], ny = y + step * nb[i][1];
            if (v34_inside(p, nx, ny) && (d = dist2(re, im, nx, ny)) < best) {
                best = d; x = nx; y = ny; moved = 1;
            }
        }
    } while (moved);

    /* (3) anything closer lies within the distance found; bound it by
     * 1.5 * max(|dx|, |dy|) >= sqrt(dx^2 + dy^2) */
    dx = re - x * V34_ONE; if (dx < 0) dx = -dx;
    dy = im - y * V34_ONE; if (dy < 0) dy = -dy;
    w = dx > dy ? dx : dy;
    w += (w >> 1) + 1;
    /* w / step, plus how far (cx, cy) can be from r: half a step per
     * coordinate on the square lattices, a whole step on the checkerboard */
    n = (int)(w >> (step == 4 ? V34_Q + 2 : V34_Q + 1)) + (checker ? 2 : 1);
    for (i = -n; i <= n; i++) {
        int nx = cx + step * i;
        dx = re - nx * V34_ONE; if (dx < 0) dx = -dx;
        if (dx > w)
            continue;
        for (j = -n; j <= n; j++) {
            int ny;
            if (checker && ((i + j) & 1))
                continue;
            ny = cy + step * j;
            dy = im - ny * V34_ONE; if (dy < 0) dy = -dy;
            if (dy > w || !v34_inside(p, nx, ny))
                continue;
            d = dist2(re, im, nx, ny);
            if (d < best) { best = d; x = nx; y = ny; }
        }
    }
    *px = x; *py = y;
    return 1;
}

/* keep the input within reach of the valid points so that the searches
 * stay short and the squared distances fit in 32 bits */
static inline int clampq(int v, int lim)
{
    return v > lim ? lim : v < -lim ? -lim : v;
}

static inline int nearest_odd(int vq7)
{
    return 2 * (int)((unsigned)(vq7 + 512 * 2 * V34_ONE) >> (V34_Q + 1)) - 1024 + 1;
}

uint32_t v34_slice_subset(const v34_params_t *p, int re, int im, unsigned s,
                          int16_t pt[2])
{
    /* In the coordinates a = re + im, b = re - im the subset is the square
     * lattice rep + (8i, 8j). */
    int ra = v34_subset_rep[s][0] + v34_subset_rep[s][1];
    int rb = v34_subset_rep[s][0] - v34_subset_rep[s][1];
    int lim = (p->ymax + 4) * V34_ONE, a, b, x, y;
    uint32_t d;

    re = clampq(re, lim); im = clampq(im, lim);
    a = ra + 8 * (((re + im - ra * V34_ONE + 4 * V34_ONE + (1 << 20)) >> 10) - 1024);
    b = rb + 8 * (((re - im - rb * V34_ONE + 4 * V34_ONE + (1 << 20)) >> 10) - 1024);
    x = (int)((unsigned)(a + b + 4096) >> 1) - 2048;    /* (a + b) / 2, exact */
    y = (int)((unsigned)(a - b + 4096) >> 1) - 2048;
    if (!p->precode && !v34_inside(p, x, y) &&
        !nearest_inside(p, re, im, &x, &y, 4, 1)) {
        pt[0] = pt[1] = 0;
        return V34_M_BIG;
    }
    pt[0] = (int16_t)x; pt[1] = (int16_t)y;
    d = dist2(re, im, x, y) >> V34_M_SHIFT;
    return d > V34_M_MAX ? V34_M_MAX : d;
}

/* The decoder's version for the 32- and 64-state codes: what eight
 * v34_slice_subset calls give, in one pass. In a = re + im, b = re - im the
 * eight subsets' lattices (ra, rb) + (8i, 8j) have between them only four
 * residues of ra modulo 8 (0, 2, 4, 6) and four of rb, each shared by two
 * subsets; the nearest a (and b) of a residue does not depend on which
 * representative of it the formula is given. So the four nearest a, the
 * four nearest b and their squared distances are found once, and a
 * subset's point is the pair its residues pick. The squared distance in
 * (a, b) is twice the one in (re, im), exactly: da = dx + dy, db = dx - dy. */
static const uint8_t sub_res[8] = {     /* v34_subset_rep[s]: (ra mod 8) / 2 | (rb mod 8) / 2 << 2 */
    1 | 0 << 2, 0 | 1 << 2, 3 | 0 << 2, 0 | 3 << 2,
    3 | 2 << 2, 2 | 3 << 2, 1 | 2 << 2, 2 | 1 << 2 };

void v34_slice_subsets(const v34_params_t *p, int re, int im, uint32_t m[8],
                       int16_t pt[8][2])
{
    int lim = (p->ymax + 4) * V34_ONE, av[4], bv[4], k, x, y;
    uint32_t da2[4], db2[4], d;
    unsigned s;

    re = clampq(re, lim); im = clampq(im, lim);
    for (k = 0; k < 4; k++) {
        int r = 2 * k;
        int32_t e;
        av[k] = r + 8 * (((re + im - r * V34_ONE + 4 * V34_ONE + (1 << 20)) >> 10) - 1024);
        bv[k] = r + 8 * (((re - im - r * V34_ONE + 4 * V34_ONE + (1 << 20)) >> 10) - 1024);
        e = re + im - av[k] * V34_ONE; da2[k] = (uint32_t)(e * e);
        e = re - im - bv[k] * V34_ONE; db2[k] = (uint32_t)(e * e);
    }
    for (s = 0; s < 8; s++) {
        unsigned ia = sub_res[s] & 3, ib = sub_res[s] >> 2;
        int a = av[ia], b = bv[ib];
        x = (int)((unsigned)(a + b + 4096) >> 1) - 2048;    /* (a + b) / 2, exact */
        y = (int)((unsigned)(a - b + 4096) >> 1) - 2048;
        if (p->precode || v34_inside(p, x, y)) {
            d = (da2[ia] + db2[ib]) >> 1;
        } else if (nearest_inside(p, re, im, &x, &y, 4, 1)) {
            d = dist2(re, im, x, y);
        } else {
            pt[s][0] = pt[s][1] = 0;
            m[s] = V34_M_BIG;
            continue;
        }
        pt[s][0] = (int16_t)x; pt[s][1] = (int16_t)y;
        d >>= V34_M_SHIFT;
        m[s] = d > V34_M_MAX ? V34_M_MAX : d;
    }
}

/* The 16-state code does not use Y4 (nor Y3), so the top bit of the subset
 * label never matters to it: its 2D metrics are those of the four cosets of
 * 4Z^2, "the points that are l clockwise quarter turns from the quarter
 * constellation grid". Half the slicing work of the 8-way partition.
 * v34_slice_classes below calls this for constellations of BRUTE_PTS
 * points or fewer (its brute-force branch); v34_slice_subsets, the 8-way
 * partition's one pass, does not cover the 4-way one, so it stays. */
uint32_t v34_slice_class(const v34_params_t *p, int re, int im, unsigned l,
                         int16_t pt[2])
{
    int rx0 = v34_subset_rep[l][0], ry0 = v34_subset_rep[l][1];
    int lim = (p->ymax + 4) * V34_ONE, x, y;
    uint32_t d;

    re = clampq(re, lim); im = clampq(im, lim);
    if (!p->precode && p->npts <= BRUTE_PTS) {
        /* Few points: most guesses on the infinite lattice would fall
         * outside. The valid points of class l are the quarter
         * constellation turned l quarter turns clockwise: turn the input
         * the other way and compare with the quarter constellation. */
        int tr = re, ti = im, t;
        unsigned i, k, bi = 0;
        uint32_t best = 0xFFFFFFFFu;
        for (k = l; k; k--) { t = tr; tr = -ti; ti = t; }
        for (i = 0; i < p->npts; i++) {
            d = dist2(tr, ti, v34_constel[i][0], v34_constel[i][1]);
            if (d < best) { best = d; bi = i; }
        }
        x = v34_constel[bi][0]; y = v34_constel[bi][1];
        for (k = l; k; k--) { t = x; x = y; y = -t; }
        pt[0] = (int16_t)x; pt[1] = (int16_t)y;
        best >>= V34_M_SHIFT;
        return best > V34_M_MAX ? V34_M_MAX : best;
    }
    x = rx0 + 4 * ((int)((unsigned)(re - rx0 * V34_ONE + 2 * V34_ONE + (1 << 20)) >> (V34_Q + 2)) - (1 << (18 - V34_Q)));
    y = ry0 + 4 * ((int)((unsigned)(im - ry0 * V34_ONE + 2 * V34_ONE + (1 << 20)) >> (V34_Q + 2)) - (1 << (18 - V34_Q)));
    if (!p->precode && !v34_inside(p, x, y))
        (void)nearest_inside(p, re, im, &x, &y, 4, 0);  /* never empty */
    pt[0] = (int16_t)x; pt[1] = (int16_t)y;
    d = dist2(re, im, x, y) >> V34_M_SHIFT;
    return d > V34_M_MAX ? V34_M_MAX : d;
}

/* The decoder's version: the four nearest lattice points share two
 * candidate columns (x = 1 and x = -1 modulo 4) and two candidate rows. */
void v34_slice_classes(const v34_params_t *p, int re, int im, uint32_t m[4],
                       int16_t pt[4][2])
{
    int lim = (p->ymax + 4) * V34_ONE, xc[2], yc[2], k;
    uint32_t dx2[2], dy2[2];
    unsigned l;

    if (!p->precode && p->npts <= BRUTE_PTS) {
        for (l = 0; l < 4; l++)
            m[l] = v34_slice_class(p, re, im, l, pt[l]);
        return;
    }
    re = clampq(re, lim); im = clampq(im, lim);
    for (k = 0; k < 2; k++) {           /* k = 0: 4Z + 1, k = 1: 4Z - 1 */
        int r0 = 1 - 2 * k;
        int32_t d;
        xc[k] = r0 + 4 * ((int)((unsigned)(re - r0 * V34_ONE + 2 * V34_ONE + (1 << 20)) >> (V34_Q + 2)) - (1 << (18 - V34_Q)));
        yc[k] = r0 + 4 * ((int)((unsigned)(im - r0 * V34_ONE + 2 * V34_ONE + (1 << 20)) >> (V34_Q + 2)) - (1 << (18 - V34_Q)));
        d = re - xc[k] * V34_ONE; dx2[k] = (uint32_t)(d * d);
        d = im - yc[k] * V34_ONE; dy2[k] = (uint32_t)(d * d);
    }
    for (l = 0; l < 4; l++) {
        /* class representatives (1,1), (1,-1), (-1,-1), (-1,1) */
        unsigned xi = l >> 1, yi = (l ^ (l >> 1)) & 1;
        int x = xc[xi], y = yc[yi];
        uint32_t d = dx2[xi] + dy2[yi];

        if (!p->precode && !v34_inside(p, x, y)) {
            /* Look at the eight neighbours first. Every other lattice
             * point is at least 6 units away (the input is within 2 units
             * of the centre in each coordinate), so a neighbour no farther
             * than that is the answer. */
            int i, j, bx = 0, by = 0;
            uint32_t best = 0xFFFFFFFFu;
            for (i = -4; i <= 4; i += 4)
                for (j = -4; j <= 4; j += 4)
                    if (v34_inside(p, x + i, y + j)) {
                        uint32_t e = dist2(re, im, x + i, y + j);
                        if (e < best) { best = e; bx = x + i; by = y + j; }
                    }
            if (best <= (uint32_t)(6 * V34_ONE) * (6 * V34_ONE)) {
                x = bx; y = by;
            } else {
                (void)nearest_inside(p, re, im, &x, &y, 4, 0);  /* never empty */
            }
            d = dist2(re, im, x, y);
        }
        pt[l][0] = (int16_t)x; pt[l][1] = (int16_t)y;
        d >>= V34_M_SHIFT;
        m[l] = d > V34_M_MAX ? V34_M_MAX : d;
    }
}

/* nearest valid y point, any subset; integer units */
static void slice_any(const v34_params_t *p, int re, int im, int *px, int *py)
{
    int lim = (p->ymax + 4) * V34_ONE, x, y;

    re = clampq(re, lim); im = clampq(im, lim);
    x = nearest_odd(re);
    y = nearest_odd(im);
    if (p->precode) {
        x = clampq(x, p->ymax);
        y = clampq(y, p->ymax);
    } else if (!v34_inside(p, x, y)) {
        (void)nearest_inside(p, re, im, &x, &y, 2, 0);  /* (1,1) is always inside */
    }
    *px = x; *py = y;
}

v34_pt_t v34_slice(const v34_params_t *p, v34_pt_t r)
{
    v34_pt_t o;
    int x, y, lim = ((p->ymax > 45 ? 45 : p->ymax) + 4) * V34_ONE, re, im;

    /* always the L-point constellation, whatever the precoder setting */
    re = clampq(r.re, lim); im = clampq(r.im, lim);
    x = nearest_odd(re);
    y = nearest_odd(im);
    if (!v34_inside(p, x, y))
        (void)nearest_inside(p, re, im, &x, &y, 2, 0);
    o.re = (int16_t)(x * V34_ONE);
    o.im = (int16_t)(y * V34_ONE);
    return o;
}

/* ---- initialisation -------------------------------------------------- */

void v34_rx_init(v34_rx_t *rx, const v34_params_t *p, v34_putbit_fn put,
                 void *user)
{
    unsigned s, y, t, used, s1, s2, keys[16];
    uint8_t cnt[V34_MAX_STATES];

    memset(rx, 0, sizeof *rx);
    rx->p = p;
    rx->put = put;
    rx->user = user;
    v34_scr_init(&rx->scr, p->cfg.scrambler);

    /* trellis: which of Y4..Y1 the code uses, and for every state its
     * predecessors with the label of the branch. For a given input the
     * three encoders map states one to one, so every state has exactly
     * nkeys predecessors, one per input. They are stored in ascending
     * order, which fixes how ties are broken. */
    used = (p->cfg.trellis == V34_TRELLIS_64) ? 0xF :
           (p->cfg.trellis == V34_TRELLIS_32) ? 0xB : 0x3;
    for (y = 0; y < 16; y++)
        if ((y & used) == y)
            keys[rx->nkeys++] = y;
    rx->nlab = (uint8_t)((keys[rx->nkeys - 1] + 1) << 1);
    memset(cnt, 0, sizeof cnt);
    for (s = 0; s < p->nstates; s++)
        for (y = 0; y < rx->nkeys; y++) {
            t = v34_conv_next(p->cfg.trellis, s, keys[y]);
            if (cnt[t] < 16) {
                rx->pred[t][cnt[t]][0] = (uint8_t)s;
                rx->pred[t][cnt[t]][1] = (uint8_t)(keys[y] << 1 | v34_conv_y0(p->cfg.trellis, s));
                cnt[t]++;
            }
        }
    for (s1 = 0; s1 < 8; s1++)
        for (s2 = 0; s2 < 8; s2++)
            rx->pairidx[s1 << 3 | s2] =
                (uint8_t)((v34_tab13[s1][s2] & used) << 1 | ((s1 ^ s2) & 1));
    /* 2D partition the code needs, and the pairs to combine */
    rx->nsub = (p->cfg.trellis == V34_TRELLIS_16) ? 4 : 8;
    for (s1 = 0; s1 < rx->nsub; s1++)
        for (s2 = 0; s2 < rx->nsub; s2++)
            rx->pairlist[rx->npairs++] = (uint8_t)(s1 << 3 | s2);

    /* 10.1.3.1: the encoder starts B1 from the zero state, and B1 carries
     * the bit inversions of the last data frame of a superframe */
    for (s = 1; s < V34_MAX_STATES; s++)
        rx->pm[s] = PM_INF;
    rx->half_in = (uint8_t)(2 * p->map.J - 2);
    rx->b1 = 1;
}

/* ---- back end: decided points to bits -------------------------------- */

static void rx_bit(v34_rx_t *rx, unsigned bit)
{
    int d = v34_descramble(&rx->scr, (int)(bit & 1));
    if (rx->b1) {
        rx->b1_bits++;
        if (d != 1)
            rx->b1_errors++;
    } else {
        rx->put(rx->user, d);
    }
}

static void rx_bits(v34_rx_t *rx, uint32_t v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        rx_bit(rx, (unsigned)(v >> i) & 1);
}

/* inverse parser, 9.3, for one complete mapping frame */
static void rx_frame(v34_rx_t *rx)
{
    const v34_params_t *p = rx->p;
    unsigned b = p->map.b, K = p->map.K, q = p->map.q, P = p->map.P, high, j;

    rx->swc = (uint8_t)(rx->swc + p->map.r);
    high = rx->swc >= P;
    if (high)
        rx->swc = (uint8_t)(rx->swc - P);

    rx->frames++;
    if (b > 12) {
        uint32_t r0 = v34_shell_unmap(p, rx->ring);
        if (r0 >> (high ? K : K - 1))
            rx->shell_errors++;
        rx_bits(rx, r0, high ? K : K - 1);
        for (j = 0; j < 4; j++) {
            rx_bits(rx, rx->ibits[j], 3);
            rx_bits(rx, rx->qv[2 * j], q);
            rx_bits(rx, rx->qv[2 * j + 1], q);
        }
    } else {
        unsigned n = high ? b : b - 1;
        for (j = 0; j < 4; j++)
            rx_bits(rx, rx->ibits[j], (j + 8 < n) ? 3 : 2);
    }
    if (++rx->mf == P) {
        rx->mf = 0;
        rx->swc = 0;
        rx->b1 = 0;
    }
}

/* One decided 2D point y (integer units), 2D symbol number n: undo the
 * precoder and the mapper. Returns the rotation, stores ring and Q. */
static unsigned rx_point(v34_rx_t *rx, const int16_t y[2], unsigned slot)
{
    const v34_params_t *p = rx->p;
    int ur = y[0], ui = y[1];
    unsigned label = 0, rot = 0;

    if (p->precode) {
        int32_t xr = y[0] * V34_ONE - rx->pn[0];
        int32_t xi = y[1] * V34_ONE - rx->pn[1];
        ur -= rx->cn[0];
        ui -= rx->cn[1];
        if (rx->xmode) {
            /* x(n) rebuilt from decisions must agree with what was
             * received. If it does not, a decision was wrong or the filter
             * memory is: restart the recursion from the received value
             * rather than let 1/H(D) ring on the error. */
            const int16_t *xd = rx->xd[rx->xd_r];
            int32_t er = xr - xd[0], ei = xi - xd[1];
            if (er > GUARD_DEC_Q7 || er < -GUARD_DEC_Q7 || ei > GUARD_DEC_Q7 || ei < -GUARD_DEC_Q7) {
                xr = xd[0]; xi = xd[1];
                rx->reseeds++;
            }
        }
        rx->xh[2][0] = rx->xh[1][0]; rx->xh[2][1] = rx->xh[1][1];
        rx->xh[1][0] = rx->xh[0][0]; rx->xh[1][1] = rx->xh[0][1];
        rx->xh[0][0] = xr;           rx->xh[0][1] = xi;
        v34_precode_pc(p, rx->xh, rx->pn, rx->cn);
    }
    if (++rx->xd_r == V34_XD_LEN)
        rx->xd_r = 0;
    if (v34_unmap_point(ur, ui, &label, &rot) || label >= p->npts) {
        rx->bad_points++;
        label %= p->npts;
    }
    rx->ring[slot] = (uint8_t)(label >> p->map.q);
    rx->qv[slot] = (uint16_t)(label & ((1u << p->map.q) - 1));
    return rot;
}

static void rx_decided(v34_rx_t *rx, const int16_t y1[2], const int16_t y2[2])
{
    unsigned j = rx->j;
    unsigned k1 = rx_point(rx, y1, 2 * j);
    unsigned k2 = rx_point(rx, y2, 2 * j + 1);
    unsigned I = (k1 - rx->z) & 3;              /* differential decoder */
    unsigned i1 = ((k2 - k1) & 3) >> 1;         /* k2 - k1 = 2 * I1 + U0 */

    rx->z = (uint8_t)k1;
    rx->ibits[j] = (uint8_t)(i1 | I << 1);
    rx->nout++;
    if (++rx->j == 4) {
        rx->j = 0;
        rx_frame(rx);
    }
}

/* ---- Viterbi decoder -------------------------------------------------- */

/* decide the oldest undecided 4D symbol by tracing back from state s of
 * the step in hist slot idx, depth steps after it (1: that step itself) */
static void rx_decide(v34_rx_t *rx, unsigned s, unsigned idx, unsigned depth)
{
    unsigned pair;
    const v34_vstep_t *st;

    while (--depth) {
        s = rx->hist[idx].prev[s];
        idx = idx ? idx - 1 : V34_VIT_DEPTH - 1u;
    }
    st = &rx->hist[idx];
    pair = st->pair[s];
    rx_decided(rx, st->pt[0][pair >> 3], st->pt[1][pair & 7]);
}

/* decide the oldest undecided 4D symbol by tracing back from the best
 * state */
static void rx_traceback(v34_rx_t *rx)
{
    rx_decide(rx, rx->best, rx->head ? rx->head - 1u : V34_VIT_DEPTH - 1u,
              (unsigned)(rx->nsteps - rx->nout));    /* 1 .. DEPTH */
}

/* acs() walks rx->pred a state at a time: 16 branches of 2 bytes each */
typedef char v34_pred_row_check[sizeof(((v34_rx_t *)0)->pred[0]) == 32 ? 1 : -1];

/* Add-compare-select for every target state, out of line so that its few
 * pointers stay in registers. pp is the whole of rx->pred, read through as
 * bytes; nk (4, 8 or 16) is a multiple of 4, four predecessors a pass. The
 * predecessors are compared in their stored (ascending) order with a strict
 * <, so the first of equal metrics wins; a state none of whose
 * predecessors is reachable keeps PM_INF and its first predecessor. Only the
 * winner's position is carried through the loop: prev[t] and lab[t] (its
 * branch label) are stored at the end. Returns the smallest new metric, its
 * state (the first of equal ones) in *bsp. */
static V90M_NOINLINE uint32_t acs(const uint32_t *pm, const uint32_t *bm, const uint8_t *pp,
                                  unsigned ns, unsigned nk, uint32_t *npm, uint8_t *prev,
                                  uint8_t *lab, unsigned *bsp)
{
    uint32_t best = PM_INF;
    unsigned t, bs = 0;
    for (t = 0; t < ns; t++, pp += 32) {
        const uint8_t *q = pp, *end = pp + 2 * nk, *pb = pp;
        uint32_t cb = PM_INF, c;
        do {
            c = pm[q[0]] + bm[q[1]]; if (c < cb) { cb = c; pb = q; }
            c = pm[q[2]] + bm[q[3]]; if (c < cb) { cb = c; pb = q + 2; }
            c = pm[q[4]] + bm[q[5]]; if (c < cb) { cb = c; pb = q + 4; }
            c = pm[q[6]] + bm[q[7]]; if (c < cb) { cb = c; pb = q + 6; }
            q += 8;
        } while (q != end);
        npm[t] = cb;
        prev[t] = pb[0];
        lab[t] = pb[1];
        if (cb < best) { best = cb; bs = t; }
    }
    *bsp = bs;
    return best;
}

static void rx_trellis_step(v34_rx_t *rx, const uint32_t *m2)
{
    const v34_params_t *p = rx->p;
    v34_vstep_t *st = &rx->hist[rx->head];
    uint32_t bm[32], npm[V34_MAX_STATES], best;
    uint8_t bpair[32];
    const uint32_t *m1 = rx->m1, *pm = rx->pm;
    const unsigned ns = p->nstates, nk = rx->nkeys, nlab = rx->nlab, npairs = rx->npairs;
    unsigned i, t, v0 = 0, bs = 0;

    /* superframe bit inversion at the start of each half data frame */
    if (rx->m_in == 0)
        v0 = v34_v0(p->map.J, rx->half_in);
    if (++rx->m_in == 2u * p->map.P) {
        rx->m_in = 0;
        if (++rx->half_in == 2u * p->map.J)
            rx->half_in = 0;
    }

    /* branch metrics: best pair of 2D subsets for every 4D label. The bit
     * inversion flips the parity the encoder produced, so it is folded
     * into the index here: bm[] is indexed by Y4..Y1 << 1 | Y0. */
    for (i = 0; i < nlab; i++) {
        bm[i] = 2 * V34_M_BIG;
        bpair[i] = 0;
    }
    for (i = 0; i < npairs; i++) {
        unsigned pr = rx->pairlist[i], l = rx->pairidx[pr] ^ v0;
        uint32_t sum = m1[pr >> 3] + m2[pr & 7];
        if (sum < bm[l]) { bm[l] = sum; bpair[l] = (uint8_t)pr; }
    }

    /* add-compare-select, per target state. A state that cannot be reached
     * yet has metric PM_INF and never wins. st->pair first receives the
     * winning branch label, then the subset pair that gave its metric. */
    best = acs(pm, bm, (const uint8_t *)rx->pred, ns, nk, npm, st->prev, st->pair, &bs);
    for (t = 0; t < ns; t++) {
        st->pair[t] = bpair[st->pair[t]];
        rx->pm[t] = (npm[t] >= PM_INF) ? PM_INF : npm[t] - best;
    }
    rx->best = (uint8_t)bs;

    /* the best metric was 0 before this step: `best` is what the decoded
     * path just cost. Smooth it (Q4, time constant 64 4D symbols). */
    if (best > V34_M_MAX)
        best = V34_M_MAX;
    rx->err_avg = (uint32_t)((int32_t)rx->err_avg +
                             (((int32_t)(best << 4) - (int32_t)rx->err_avg) >> 6));

    if (++rx->head == V34_VIT_DEPTH)
        rx->head = 0;
    rx->nsteps++;
    if (rx->nsteps - rx->nout >= V34_VIT_DEPTH)
        rx_traceback(rx);
}

/* one whitened symbol r ~ y(n), Q7 */
static void rx_symbol(v34_rx_t *rx, int re, int im)
{
    v34_vstep_t *st = &rx->hist[rx->head];
    uint32_t m2[8];

#ifdef V34_TEST_API
    if (rx->hard) {                     /* no trellis decoding */
        int x, y;
        int16_t y2[2];
        slice_any(rx->p, re, im, &x, &y);
        if (rx->k == 0) {
            rx->hard_y[0] = (int16_t)x; rx->hard_y[1] = (int16_t)y;
            rx->k = 1;
            return;
        }
        y2[0] = (int16_t)x; y2[1] = (int16_t)y;
        rx->k = 0;
        rx->nsteps++;
        rx_decided(rx, rx->hard_y, y2);
        return;
    }
#endif
    {
        uint32_t *m = rx->k ? m2 : rx->m1;
        int16_t (*pt)[2] = st->pt[rx->k];
        if (rx->nsub == 4)
            v34_slice_classes(rx->p, re, im, m, pt);
        else
            v34_slice_subsets(rx->p, re, im, m, pt);
    }
    if (rx->k == 0) {
        rx->k = 1;
        return;
    }
    rx->k = 0;
    rx_trellis_step(rx, m2);
}

static int16_t sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void v34_rx_put(v34_rx_t *rx, v34_pt_t xhat, v34_pt_t *tent)
{
    const v34_params_t *p = rx->p;
    int32_t re = xhat.re, im = xhat.im;
    int yx, yy, k;

    rx->xmode = 1;
    rx->xd[rx->xd_w][0] = xhat.re;
    rx->xd[rx->xd_w][1] = xhat.im;
    if (++rx->xd_w == V34_XD_LEN)
        rx->xd_w = 0;

    if (!p->precode) {
        if (tent)
            *tent = v34_slice(p, xhat);
        rx_symbol(rx, re, im);
        return;
    }

    /* noise-whitening filter: r(n) = xhat(n) + sum h(k) xhat(n-k), which is
     * y(n) plus white noise when xhat is x(n) plus the noise the precoder
     * coefficients were computed for */
    {
        int64_t ar = 0, ai = 0;
        for (k = 0; k < 3; k++) {
            int32_t hr = p->cfg.h[k][0], hi = p->cfg.h[k][1];
            ar += (int64_t)rx->xin[k][0] * hr - (int64_t)rx->xin[k][1] * hi;
            ai += (int64_t)rx->xin[k][0] * hi + (int64_t)rx->xin[k][1] * hr;
        }
        re += (int32_t)((ar + (1 << 13)) >> 14);
        im += (int32_t)((ai + (1 << 13)) >> 14);
        rx->xin[2][0] = rx->xin[1][0]; rx->xin[2][1] = rx->xin[1][1];
        rx->xin[1][0] = rx->xin[0][0]; rx->xin[1][1] = rx->xin[0][1];
        rx->xin[0][0] = xhat.re;       rx->xin[0][1] = xhat.im;
    }

    /* tentative decision: slice r to the y grid, then take off p(n) rebuilt
     * from the previous tentative decisions (9.6.2 run as a decision
     * feedback loop). Kept running even if the caller does not look. */
    {
        int32_t pn[2], cn[2], xr, xi, er, ei;
        slice_any(p, re, im, &yx, &yy);
        v34_precode_pc(p, rx->xt, pn, cn);
        xr = yx * V34_ONE - pn[0];
        xi = yy * V34_ONE - pn[1];
        er = xr - xhat.re; ei = xi - xhat.im;
        if (er > GUARD_TENT_Q7 || er < -GUARD_TENT_Q7 || ei > GUARD_TENT_Q7 || ei < -GUARD_TENT_Q7) {
            /* the feedback loop has lost track: fall back to the error seen
             * on the y grid and restart the loop from the received value */
            if (tent) {
                tent->re = sat16(xhat.re + (yx * V34_ONE - re));
                tent->im = sat16(xhat.im + (yy * V34_ONE - im));
            }
            xr = xhat.re; xi = xhat.im;
        } else if (tent) {
            tent->re = sat16(xr); tent->im = sat16(xi);
        }
        rx->xt[2][0] = rx->xt[1][0]; rx->xt[2][1] = rx->xt[1][1];
        rx->xt[1][0] = rx->xt[0][0]; rx->xt[1][1] = rx->xt[0][1];
        rx->xt[0][0] = xr;           rx->xt[0][1] = xi;
    }
    rx_symbol(rx, re, im);
}

void v34_rx_put_y(v34_rx_t *rx, v34_pt_t yhat, v34_pt_t *tent)
{
    rx->xmode = 0;
    if (++rx->xd_w == V34_XD_LEN)
        rx->xd_w = 0;
    if (tent) {
        int x, y;
        slice_any(rx->p, yhat.re, yhat.im, &x, &y);
        tent->re = (int16_t)(x * V34_ONE);
        tent->im = (int16_t)(y * V34_ONE);
    }
    rx_symbol(rx, yhat.re, yhat.im);
}

#ifdef V34_TEST_API
void v34_rx_set_hard(v34_rx_t *rx, int on)
{
    rx->hard = (uint8_t)(on != 0);
}

void v34_rx_flush(v34_rx_t *rx)
{
    while (rx->nout < rx->nsteps)
        rx_traceback(rx);
}
#endif

/* the input i symbols before the newest (i = 0: the newest) */
static const int16_t *xd_back(const v34_rx_t *rx, unsigned i)
{
    return rx->xd[(rx->xd_w + 2u * V34_XD_LEN - 1u - i) % V34_XD_LEN];
}

/* two points of one size (within about 12 %), 90 degrees apart (within
 * about 14 degrees), not nothing: |a|^2, |b|^2 and a.b */
static int s_pair(int64_t aa, int64_t bb, int64_t ab)
{
    return aa >= (int64_t)V34_ONE * V34_ONE && 4 * (aa > bb ? aa - bb : bb - aa) <= aa &&
           4 * (ab < 0 ? -ab : ab) <= aa;
}

int v34_rx_s_begins(const v34_rx_t *rx)
{
    unsigned pend = 2u * (unsigned)(rx->nsteps - rx->nout) + rx->k;
    const int16_t *a, *b;
    if (!rx->xmode || pend < 2)
        return 0;
    a = xd_back(rx, 0); b = xd_back(rx, 1);
    return s_pair((int64_t)a[0] * a[0] + (int64_t)a[1] * a[1], (int64_t)b[0] * b[0] + (int64_t)b[1] * b[1],
                  (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1]);
}

unsigned v34_rx_s_run(const v34_rx_t *rx)
{
    unsigned pend = 2u * (unsigned)(rx->nsteps - rx->nout) + rx->k, i, run = 0;
    int32_t a[2], b[2];
    int64_t aa, bb, ab, t, sum = 0, best = 0;
    int odd;
    if (!rx->xmode || pend < 4)
        return 0;
    for (i = 0; i < 2; i++) {
        a[i] = (xd_back(rx, 0)[i] + xd_back(rx, 2)[i]) / 2;
        b[i] = (xd_back(rx, 1)[i] + xd_back(rx, 3)[i]) / 2;
    }
    aa = (int64_t)a[0] * a[0] + (int64_t)a[1] * a[1];
    bb = (int64_t)b[0] * b[0] + (int64_t)b[1] * b[1];
    ab = (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1];
    if (!s_pair(aa, bb, ab))
        return 0;
    /* S begins on point 0, the next symbol is it turned 90 degrees
     * anticlockwise (v34_s(): 128 symbols, the last turned): the oldest of
     * the run is the one of a pair the other is turned anticlockwise from.
     * b x a > 0: a is b turned anticlockwise, so the run is even. */
    odd = (int64_t)b[0] * a[1] - (int64_t)b[1] * a[0] < 0;
    /* a match: within a quarter of the points' size. The run grows only on
     * a clear gain, so that a data point that happens to lie near the S
     * point of its turn moves the start back only when it is close */
    t = aa / 16;
    for (i = 0; i < pend; i++) {
        const int16_t *x = xd_back(rx, i);
        const int32_t *r = (i & 1) ? b : a;
        int64_t dr = x[0] - r[0], di = x[1] - r[1];
        sum += t - (dr * dr + di * di);
        if ((int)((i + 1) & 1) == odd && sum > best + (i > 1 ? t / 4 : 0)) {
            best = sum;
            run = i + 1;
        }
    }
    return run;
}

/* the 2D metrics of the input i symbols before the newest, as rx_symbol()
 * had them (v34_rx_put()'s whitening done again from the inputs kept) */
static void metrics_back(const v34_rx_t *rx, unsigned i, uint32_t m[8])
{
    const v34_params_t *p = rx->p;
    const int16_t *x = xd_back(rx, i);
    int16_t pt[8][2];
    int32_t re = x[0], im = x[1];
    unsigned k;

    if (p->precode) {
        int64_t ar = 0, ai = 0;
        for (k = 0; k < 3; k++) {
            const int16_t *xp = xd_back(rx, i + 1 + k);
            int32_t hr = p->cfg.h[k][0], hi = p->cfg.h[k][1];
            ar += (int64_t)xp[0] * hr - (int64_t)xp[1] * hi;
            ai += (int64_t)xp[0] * hi + (int64_t)xp[1] * hr;
        }
        re += (int32_t)((ar + (1 << 13)) >> 14);
        im += (int32_t)((ai + (1 << 13)) >> 14);
    }
    if (rx->nsub == 4)
        v34_slice_classes(p, re, im, m, pt);
    else
        v34_slice_subsets(p, re, im, m, pt);
}

void v34_rx_flush_before(v34_rx_t *rx, unsigned n)
{
    unsigned pend = 2u * (unsigned)(rx->nsteps - rx->nout) + rx->k;
    unsigned d = (unsigned)(rx->nsteps - rx->nout), c, t, s, idx, bs = 0;
    uint32_t m[2 * V34_VIT_DEPTH][8];
    uint64_t best = UINT64_MAX;

    /* a mapping frame (8 symbols) of margin: where S begins is found from
     * its points, and a real channel turns the first few of them (up to 37
     * degrees in a recording), so the start found can be late by several
     * symbols; a frame that could hold S is never delivered */
#ifdef V34_TEST_API
    if (rx->hard)
        return;
#endif
    if (!rx->xmode || pend <= n + 8)
        return;
    c = (pend - n - 8) / 2;                 /* 4D symbols wholly before the margin */
    c = ((rx->j + c) / 4) * 4;              /* ... to the end of the last whole frame */
    if (c <= rx->j)
        return;
    c -= rx->j;
    /* The best state now is the best through what came after them (S):
     * the path through them is the survivor, of the states at the last of
     * them, that costs least over the steps still undecided, their metrics
     * taken again from the inputs kept. */
    idx = (rx->head + 2u * V34_VIT_DEPTH - 1u - (d - c)) % V34_VIT_DEPTH;
    for (t = 0; t < 2 * c; t++)
        metrics_back(rx, pend - 1 - t, m[t]);
    for (s = 0; s < rx->p->nstates; s++) {
        uint64_t cost = 0;
        unsigned st = s, ix = idx;
        for (t = c; t-- > 0;) {
            unsigned pr = rx->hist[ix].pair[st];
            cost += (uint64_t)m[2 * t][pr >> 3] + m[2 * t + 1][pr & 7];
            st = rx->hist[ix].prev[st];
            ix = ix ? ix - 1 : V34_VIT_DEPTH - 1u;
        }
        if (cost < best) {
            best = cost;
            bs = s;
        }
    }
    for (; c > 0; c--)
        rx_decide(rx, bs, idx, c);
}

uint32_t v34_rx_quality(const v34_rx_t *rx)
{
    return rx->err_avg >> 5;        /* Q4 per 4D symbol -> per 2D symbol */
}
