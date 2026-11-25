/* v90d/v34 - framing (clause 8) and mapping (clause 9.2) parameters, shell
 * mapper tables (9.4), constellation membership and nominal energy. All of
 * this runs once at start-up; it uses 64-bit multiplies, and one 64-bit
 * division: the nonlinear encoder's Phi (v34_nl_phi_q16, int64 by
 * energy_q8) when cfg->nonlinear is set. */
#include <string.h>
#include "v34_priv.h"

int v34_map_params(unsigned S, unsigned R, int expanded, v34_map_t *out)
{
    unsigned J, P, maxr, minr = 4800, N, b, r, K, q, M, Mx, cnt, i, swp;
    uint64_t pw, lim;

    switch (S) {                         /* Table 7; rate limits of Table 8 */
    case 2400: J = 7; P = 12; maxr = 21600; minr = 2400; break;
    case 2743: J = 8; P = 12; maxr = 26400; break;
    case 2800: J = 7; P = 14; maxr = 26400; break;
    case 3000: J = 7; P = 15; maxr = 28800; break;
    case 3200: J = 7; P = 16; maxr = 31200; break;
    case 3429: J = 8; P = 15; maxr = 33600; break;
    default: return -1;
    }
    /* primary rate n * 2400, optionally plus the 200 bit/s auxiliary channel */
    if (R % 2400 > 200 || R % 200 || R < minr || R > maxr + 200)
        return -1;

    N = (J == 7) ? R / 25 : R / 200 * 7;         /* (8-1): R * 0.28 / J */
    b = (N + P - 1) / P;                         /* smallest integer >= N/P */
    r = N - (b - 1) * P;                         /* (8-2) */

    /* SWP by the counter algorithm of 8.2; left-most bit = first frame */
    swp = 0; cnt = 0;
    for (i = 0; i < P; i++) {
        cnt += r;
        swp <<= 1;
        if (cnt >= P) { swp |= 1; cnt -= P; }
    }

    /* (9-1) */
    q = 0;
    if (b <= 12) {
        K = 0;
    } else {
        K = b - 12;
        while (K >= 32) { K -= 8; q++; }
    }
    /* minimum M: smallest integer >= 2^(K/8), i.e. smallest M with
     * M^8 >= 2^K */
    for (M = 1; ; M++) {
        pw = (uint64_t)M * M; pw *= pw; pw *= pw;
        if (pw >= ((uint64_t)1 << K)) break;
    }
    /* expanded M: nearest integer to 1.25 * 2^(K/8), not less than the
     * minimum. M - 1/2 <= 1.25 * 2^(K/8)  <=>  (4M - 2)^8 <= 5^8 * 2^K */
    lim = (uint64_t)390625 << K;
    for (Mx = M; ; Mx++) {
        uint64_t t = 4 * (Mx + 1) - 2;
        pw = t * t; pw *= pw; pw *= pw;
        if (pw > lim) break;
    }

    out->J = (uint8_t)J; out->P = (uint8_t)P; out->N = (uint16_t)N;
    out->b = (uint8_t)b; out->r = (uint8_t)r; out->swp = (uint16_t)swp;
    out->K = (uint8_t)K; out->q = (uint8_t)q;
    out->M = (uint8_t)(expanded ? Mx : M);
    out->L = (uint16_t)((4u * out->M) << q);     /* (9-2) */
    out->w = (b < 56) ? 1 : 2;                   /* (9-29) */
    return 0;
}

/* Mean ring energy weighted by how often the shell mapper uses each ring
 * when its K-bit input is uniform over 0 .. nwords-1 (nwords a power of
 * two, 2^lg). Complete shells are counted exactly; the words of the last,
 * partly used shell are taken as a uniform sample of that shell. Result in
 * units^2 * 256. */
static uint32_t shell_energy_q8(const v34_params_t *p, unsigned lg,
                                const uint32_t *esum)
{
    unsigned M = p->map.M, q = p->map.q, A, m, i, k, s;
    uint32_t g3[3 * V34_MAX_M - 2], g7[7 * V34_MAX_M - 6];
    uint32_t c7[7 * V34_MAX_M - 5];
    uint32_t nwords = (uint32_t)1 << lg, npart, g8, frac;
    uint64_t acc = 0;

    for (i = 0; i <= 3 * (M - 1); i++) {         /* g3 = g2 * g1 */
        uint32_t t = 0;
        /* the terms with k <= i, k <= 2 (M - 1), i - k < M (the same sum
         * as over all k with those tests: ARMv5 runs it at every start) */
        unsigned k0 = i >= M ? i - (M - 1) : 0, k1 = i < 2 * (M - 1) ? i : 2 * (M - 1);
        for (k = k0; k <= k1; k++) t += p->g2[k];
        g3[i] = t;
    }
    c7[0] = 0;
    for (i = 0; i <= 7 * (M - 1); i++) {         /* g7 = g4 * g3 */
        uint32_t t = 0;
        unsigned k0 = i > 3 * (M - 1) ? i - 3 * (M - 1) : 0, k1 = i < 4 * (M - 1) ? i : 4 * (M - 1);
        for (k = k0; k <= k1; k++) t += p->g4[k] * g3[i - k];
        g7[i] = t;
        c7[i + 1] = c7[i] + t;                   /* may wrap; see below */
    }
    /* last shell in use */
    for (A = 0; A < 8 * (M - 1) && p->z8[A + 1] <= nwords - 1; A++)
        ;
    npart = nwords - p->z8[A];
    g8 = p->z8[A + 1] - p->z8[A];
    for (s = 0; (g8 >> s) >= 65536u; s++)
        ;
    frac = ((npart >> s) << 16) / (g8 >> s);     /* Q16, <= 1.0 */

    for (m = 0; m < M && m <= A; m++) {
        /* words (per position) that carry ring m: all of shells < A, plus
         * the used fraction of shell A. The total over m is nwords < 2^32,
         * so 32-bit prefix sums are exact here. */
        uint32_t full = (A - m <= 7 * (M - 1)) ? c7[A - m] : c7[7 * (M - 1) + 1];
        uint32_t part = (A - m <= 7 * (M - 1)) ? g7[A - m] : 0;
        uint64_t wgt = ((uint64_t)full << 16) + (uint64_t)part * frac;   /* Q16 */
        acc += (wgt >> 8) * (((uint64_t)esum[m] << 8) >> q);             /* Q16 */
    }
    return (uint32_t)((acc >> lg) >> 8);
}

int v34_params_init(v34_params_t *p, const v34_cfg_t *cfg)
{
    unsigned M, i, n, rot, maxc = 0;
    uint32_t esum[V34_MAX_M];

    memset(p, 0, sizeof *p);
    if (cfg->data_rate % 2400)
        return -1;
    if (v34_map_params(cfg->symbol_rate, cfg->data_rate, cfg->expanded != 0,
                       &p->map))
        return -1;
    if (cfg->trellis > V34_TRELLIS_64)
        return -2;
    p->cfg = *cfg;
    p->nstates = (uint8_t)(16u << cfg->trellis);
    for (i = 0; i < 3; i++)
        if (cfg->h[i][0] || cfg->h[i][1])
            p->precode = 1;

    M = p->map.M;
    v34_shell_tables(p, M);             /* (9-3) to (9-6) */

    /* constellation: labels 0 .. L/4-1 and their rotations */
    p->npts = (uint16_t)(p->map.L / 4);
    memset(esum, 0, sizeof esum);
    for (n = 0; n < p->npts; n++) {
        int x = v34_constel[n][0], y = v34_constel[n][1], re, im;
        unsigned ax = (unsigned)(x < 0 ? -x : x), ay = (unsigned)(y < 0 ? -y : y);
        if (ax > maxc) maxc = ax;
        if (ay > maxc) maxc = ay;
        esum[n >> p->map.q] += (uint32_t)(x * x + y * y);
        for (rot = 0; rot < 4; rot++) {
            unsigned ix, iy, b;
            v34_map_point(n, rot, &re, &im);
            ix = (unsigned)(re + 45) >> 1;
            iy = (unsigned)(im + 45) >> 1;
            b = iy * 46 + ix;
            p->inside[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
    }

    /* nominal energy: high and low mapping frames weighted by SWP */
    {
        unsigned K = p->map.K, P = p->map.P, r = p->map.r;
        uint32_t eh = shell_energy_q8(p, K, esum);
        uint32_t el = (K > 0 && r < P) ? shell_energy_q8(p, K - 1, esum) : eh;
        p->energy_q8 = (eh * r + el * (P - r) + P / 2) / P;
        p->energy_tx_q8 = p->energy_q8;
        if (cfg->nonlinear) {
            /* the same average over the rings with every point's energy
             * multiplied by its Phi^2 (the precoder's small dither left out) */
            uint64_t acc[V34_MAX_M];
            memset(acc, 0, sizeof acc);
            for (n = 0; n < p->npts; n++) {
                int x = v34_constel[n][0], y = v34_constel[n][1];
                uint64_t e = (uint64_t)(x * x + y * y);
                uint64_t phi = (uint64_t)v34_nl_phi_q16(p->energy_q8, (int64_t)(e << 14));
                acc[n >> p->map.q] += e * ((phi * phi) >> 16);          /* Q16 */
            }
            for (i = 0; i < M; i++) esum[i] = (uint32_t)((acc[i] + 32768) >> 16);
            eh = shell_energy_q8(p, K, esum);
            el = (K > 0 && r < P) ? shell_energy_q8(p, K - 1, esum) : eh;
            p->energy_tx_q8 = (eh * r + el * (P - r) + P / 2) / P;
        }
    }

    /* bound on |y| = |u + c| per coordinate */
    if (p->precode) {
        unsigned half = 1u << (p->map.w - 1), hs = 0, y;
        for (i = 0; i < 3; i++) {
            hs += (unsigned)(cfg->h[i][0] < 0 ? -cfg->h[i][0] : cfg->h[i][0]);
            hs += (unsigned)(cfg->h[i][1] < 0 ? -cfg->h[i][1] : cfg->h[i][1]);
        }
        /* |x| <= maxc + half, |p| <= hs * |x| / 2^14, |c| <= |p| + half */
        y = maxc + half + (((maxc + half) * hs) >> 14) + 1;
        y |= 1;
        p->ymax = (uint8_t)(y > 255 ? 255 : y);
    } else {
        p->ymax = (uint8_t)maxc;
    }
    return 0;
}

uint32_t v34_gain_q14(uint32_t energy_q8)
{
    /* largest g with g^2 * E <= 2^37, i.e. g = 2^14 * sqrt(2 * 256 / E) */
    uint32_t g = 0, bit;
    if (energy_q8 == 0)
        return 0;
    for (bit = 1u << 18; bit; bit >>= 1) {
        uint64_t t = (uint64_t)(g | bit);
        if (t * t * energy_q8 <= ((uint64_t)1 << 37))
            g |= bit;
    }
    return g;
}
