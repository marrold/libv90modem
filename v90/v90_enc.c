/* V.90 5.4 encoder building blocks. See v90_enc.h for the interface and the
 * spec readings. */
#include <string.h>
#include "v90_enc.h"

/* ---- mapping parameters --------------------------------------------------- */

void v90_map_set_points(v90_map_t *m, int i, const uint8_t mask[16])
{
    int u, n = 0;
    for (u = 127; u >= 0; u--)
        if ((mask[u >> 3] >> (u & 7)) & 1) m->C[i][n++] = (uint8_t)u;
    m->M[i] = (uint8_t)n;
}

int v90_map_check(const v90_map_t *m)
{
    uint64_t prod = 1;
    int i, k;
    if (m->Sr > 3 || m->ld > 3) return V90_ERR_RANGE;
    for (i = 0; i < 6; i++) {
        if (m->M[i] < 1 || m->M[i] > 128) return V90_ERR_RANGE;
        if (m->C[i][0] > 127) return V90_ERR_RANGE;
        for (k = 1; k < m->M[i]; k++)
            if (m->C[i][k] >= m->C[i][k - 1]) return V90_ERR_RANGE;
        prod *= m->M[i];                                 /* at most 128^6 = 2^42 */
    }
    if (m->K > 42 || ((uint64_t)1 << m->K) > prod) return V90_ERR_RANGE;
    return 0;
}

int v90_map_from_cp(v90_map_t *m, const v90_cp_t *cp)
{
    int i, D, S, K;
    if (cp->drn == 0) return V90_MAP_CLEARDOWN;
    if (cp->drn > 22 || cp->sr > 3 || cp->ld > 3) return V90_ERR_RANGE;
    for (i = 0; i < 6; i++) if (cp->idx[i] > 5) return V90_ERR_RANGE;
    D = cp->drn + (cp->is_cp ? 20 : 8);
    S = 6 - cp->sr;
    K = D - S;
    if (cp->is_cp) { if (K < 15 || K > 39) return V90_ERR_RANGE; }     /* Table 2 */
    else           { if (K < 6  || K > 24) return V90_ERR_RANGE; }     /* Table 17 */
    m->K = (uint8_t)K;
    m->Sr = cp->sr;
    m->ld = cp->ld;
    m->a1 = cp->a1; m->a2 = cp->a2; m->b1 = cp->b1; m->b2 = cp->b2;
    for (i = 0; i < 6; i++) v90_map_set_points(m, i, cp->mask[cp->idx[i]]);
    return v90_map_check(m);
}

/* ---- modulus encoder ------------------------------------------------------ */

/* rcp[M] = ceil(2^32 / M) = floor((2^32 - 1) / M) + 1, for M = 2..128 (M = 1
 * is not divided). For cur < M 2^16, (cur rcp[M]) >> 32 is cur / M exactly:
 * with rcp[M] = (2^32 + e) / M, 0 <= e < M, the product over 2^32 is
 * cur / M + cur e / (M 2^32), and cur e / (M 2^32) < M / 2^16 < 1 / M
 * (M < 256), which a fraction of at most (M - 1) / M cannot carry past the
 * next integer. test/test_small_exact.c checks every cur in `full` mode. */
#define RCP(d)   ((uint32_t)(0xFFFFFFFFu / (d) + 1u))
#define RCP4(d)  RCP(d), RCP((d) + 1), RCP((d) + 2), RCP((d) + 3)
#define RCP16(d) RCP4(d), RCP4((d) + 4), RCP4((d) + 8), RCP4((d) + 12)
static const uint32_t rcp[129] = {
    0, 0, RCP(2), RCP(3), RCP4(4), RCP4(8), RCP4(12), RCP16(16), RCP16(32), RCP16(48),
    RCP16(64), RCP16(80), RCP16(96), RCP16(112), RCP(128)
};

void v90_modulus_encode(const v90_map_t *m, const uint8_t *b, uint8_t k[6])
{
    /* R0 as three 16-bit limbs (K <= 42), divided by each Mi in turn with
     * schoolbook short division: the running remainder is below Mi <= 128, so
     * every step's dividend is below Mi 2^16 <= 2^23, divided through rcp. */
    uint16_t r[3] = { 0, 0, 0 };
    int i, j;
    for (i = 0; i < m->K; i++)
        if (b[i] & 1) r[i >> 4] |= (uint16_t)(1u << (i & 15));
    for (i = 0; i < 6; i++) {
        /* (v90_map_check allows 1 <= Mi <= 128; above, C's division) */
        uint32_t M = m->M[i], mr = M <= 128 ? rcp[M] : 0, rem = 0;
        if (M <= 1) { k[i] = 0; continue; }     /* R0 mod 1, R0 kept */
        for (j = 2; j >= 0; j--) {
            uint32_t cur = (rem << 16) | r[j], q = mr ? (uint32_t)(((uint64_t)cur * mr) >> 32) : cur / M;
            r[j] = (uint16_t)q;
            rem = cur - q * M;
        }
        k[i] = (uint8_t)rem;
    }
}

/* ---- sign precoding ------------------------------------------------------- */

void v90_diff_init(v90_diff_t *d) { memset(d, 0, sizeof *d); }

void v90_sign_precode(v90_diff_t *d, int Sr, const uint8_t *s, uint8_t t[6])
{
    int j, k, n = 0, F;
    if (Sr == 0) {
        /* 5.4.5.1: plain differential coding along the data frame */
        uint8_t prev = d->tprev[0];
        for (k = 0; k < 6; k++) { prev ^= (uint8_t)(s[k] & 1); t[k] = prev; }
        d->tprev[0] = prev;
        return;
    }
    /* Sr shaping frames of F bits. Table 3: position 0 is the redundant bit
     * (0), the others take the s bits in order. Table 4: the odd positions
     * form one differential chain that runs on across shaping frames, even
     * positions pass. Then t_j(k) = p'_j(k) XOR t_(j-1)(k). */
    F = 6 / Sr;
    for (j = 0; j < Sr; j++) {
        for (k = 0; k < F; k++) {
            uint8_t p = k == 0 ? 0 : (uint8_t)(s[n++] & 1);
            if (k & 1) { p ^= d->podd; d->podd = p; }
            p ^= d->tprev[k];
            d->tprev[k] = p;
            t[j * F + k] = p;
        }
    }
}

void v90_frame_encode(const v90_map_t *m, v90_diff_t *d, const uint8_t *bits,
                      uint8_t u[6], uint8_t t[6])
{
    uint8_t k[6];
    int i, S = V90_MAP_S(m);
    v90_sign_precode(d, m->Sr, bits, t);                 /* d0..dS-1 = s0..sS-1 */
    v90_modulus_encode(m, bits + S, k);                  /* dS..dD-1 = b0..bK-1 */
    for (i = 0; i < 6; i++) u[i] = m->C[i][k[i]];
}

/* ---- spectral shaper ------------------------------------------------------ */

#define SH_SAT ((int32_t)1 << 29)

static int32_t sh_sat(int64_t v)
{
    if (v > SH_SAT) return SH_SAT;
    if (v < -SH_SAT) return -SH_SAT;
    return (int32_t)v;
}

/* p / 64 rounded to nearest (halves up), without relying on the shift of a
 * negative value */
static int64_t q6_round(int64_t p)
{
    p += 32;
    if (p >= 0) return (int64_t)((uint64_t)p >> 6);
    return -(int64_t)(((uint64_t)(-p) + 63) >> 6);
}

void v90_shaper_init(v90_shaper_t *sh) { memset(sh, 0, sizeof *sh); }

int32_t v90_shaper_step(v90_shaper_t *sh, const v90_map_t *m, int32_t x)
{
    /* 1) y[n] = x[n] - b1 x[n-1] + a1 y[n-1]
     * 2) v[n] = y[n] - b2 y[n-1] + a2 v[n-1]          (y, v in Q6) */
    int32_t y = sh_sat((int64_t)x * 64 - (int64_t)m->b1 * sh->x1
                       + q6_round((int64_t)m->a1 * sh->y1));
    int32_t v = sh_sat((int64_t)y - q6_round((int64_t)m->b2 * sh->y1)
                       + q6_round((int64_t)m->a2 * sh->v1));
    sh->x1 = x; sh->y1 = y; sh->v1 = v;
    return v;
}

typedef struct {
    const v90_map_t *m;
    const int32_t *lin;
    const uint8_t *t;
    int F, n;
} search_t;

/* Run shaping frame f from state st with odd-bit inversion o (the even-bit
 * inversion follows from the trellis state); returns the metric increment,
 * 3) w[n] = v^2[n] + w[n-1], and optionally the signs. */
static uint64_t run_frame(const search_t *c, int f, v90_shaper_t *st, int o, uint8_t *sign)
{
    int e = o ^ st->q, k;
    uint64_t w = 0;
    for (k = 0; k < c->F; k++) {
        int sg = (c->t[f * c->F + k] ^ ((k & 1) ? o : e)) & 1;
        int32_t mag = c->lin[f * c->F + k];
        int64_t v = v90_shaper_step(st, c->m, sg ? mag : -mag);
        w += (uint64_t)(v * v);                          /* |v| <= 2^29: 24 terms fit */
        if (sign) sign[k] = (uint8_t)sg;
    }
    st->q = (uint8_t)o;                                  /* Figure 2: next state */
    return w;
}

/* least metric over all allowed rule sequences for frames f..n-1 */
static uint64_t search(const search_t *c, int f, v90_shaper_t st, int *choice)
{
    uint64_t best = 0;
    int o, besto = 0;
    for (o = 0; o < 2; o++) {
        v90_shaper_t s2 = st;
        uint64_t w = run_frame(c, f, &s2, o, 0);
        if (f + 1 < c->n) w += search(c, f + 1, s2, 0);
        if (o == 0 || w < best) { best = w; besto = o; }
    }
    if (choice) *choice = besto;
    return best;
}

int v90_shape(v90_shaper_t *sh, const v90_map_t *m, const int32_t *lin,
              const uint8_t *t, int nframes, uint8_t *sign)
{
    search_t c;
    int o, rule;
    c.m = m; c.lin = lin; c.t = t;
    c.F = 6 / m->Sr;
    c.n = nframes < m->ld + 1 ? nframes : m->ld + 1;
    if (c.n < 1) c.n = 1;
    (void)search(&c, 0, *sh, &o);
    rule = 2 * sh->q + o;                                /* A, B from state 0; C, D from state 1 */
    (void)run_frame(&c, 0, sh, o, sign);
    return rule;
}
