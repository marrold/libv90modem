/* V.92 6.4 analogue modem transmitter model. See v92_enc.h. */
#include <string.h>
#include "v92_enc.h"
#include "v34_priv.h"       /* Figure 9, Table 13 and the convolutional
                               encoders of V.34, which 6.4.3 and 6.4.4 use */

/* ---- 6.4.3 --------------------------------------------------------------- */

unsigned v92_subset(int a, int b)
{
    return v34_subset(2 * a + 1, 2 * b + 1);
}

unsigned v92_inverse_map(const int16_t y[V92_TFRAME])
{
    return v34_tab13[v92_subset(y[0], y[1])][v92_subset(y[2], y[3])];
}

/* ---- 8.7.4 --------------------------------------------------------------- */

void v92_rm_k(const uint8_t M[V92_DFRAME], int prime, uint8_t k[V92_DFRAME])
{
    int i;
    for (i = 0; i < V92_DFRAME; i++) {
        /* RM: intervals 0, 1, 4, 5, 8, 9 carry Mi - 1; RM': the others */
        int high = ((i & 2) == 0) ? !prime : !!prime;
        k[i] = (uint8_t)(high ? M[i] - 1 : 0);
    }
}

/* ---- encoder ------------------------------------------------------------- */

int v92_enc_init(v92_enc_t *e, const v92_map_t *map, const v92_filt_t *filt,
                 v92_getbit_fn get, void *user)
{
    memset(e, 0, sizeof *e);
    if (v92_map_check(map) != 0) return V92_ERR_RANGE;
    if (filt && v92_filt_check(filt) != 0) return V92_ERR_RANGE;
    e->map = map;
    e->filt = filt;
    e->get = get;
    e->user = user;
    v90_scr_init(&e->scr);
    (void)v92_mod_init(&e->mod, map->M, map->K);    /* checked above */
    return 0;
}

void v92_enc_set_pick(v92_enc_t *e, v92_pick_fn fn, void *user)
{
    e->pick = fn;
    e->pick_user = user;
}

void v92_enc_force_k(v92_enc_t *e, const uint8_t k[V92_DFRAME])
{
    memcpy(e->kf, k, V92_DFRAME);
    e->forced = 1;
}

/* v / 2^sh rounded to nearest, half away from zero, saturated to int32 */
static int32_t rnd_shift(int64_t v, int sh)
{
    int64_t h = (int64_t)1 << (sh - 1);
    int64_t r = v >= 0 ? (v + h) >> sh : -((-v + h) >> sh);
    if (r > INT32_MAX) return INT32_MAX;
    if (r < -INT32_MAX) return -INT32_MAX;
    return (int32_t)r;
}

/* history sample `back` symbols ago, back = 1 .. V92_MAX_LSEC */
#define HIDX(hp, back) ((hp) >= (back) ? (hp) - (back) : (hp) + V92_MAX_LSEC - (back))

/* sum over k = 1 .. n of hist[HIDX(hp, k)] c[k - 1]: the history newest
 * first, in the two runs that do not wrap (n <= V92_MAX_LSEC) */
static int64_t hdot16(const int16_t *hist, int hp, const int16_t *c, unsigned n)
{
    int64_t acc = 0;
    unsigned k = 0, n1 = (unsigned)hp < n ? (unsigned)hp : n;
    const int16_t *h = hist + hp - 1;
    for (; k < n1; k++, h--) acc += (int32_t)*h * c[k];
    for (h = hist + V92_MAX_LSEC - 1; k < n; k++, h--) acc += (int32_t)*h * c[k];
    return acc;
}
static int64_t hdot32(const int32_t *hist, int hp, const int16_t *c, unsigned n)
{
    int64_t acc = 0;
    unsigned k = 0, n1 = (unsigned)hp < n ? (unsigned)hp : n;
    const int32_t *h = hist + hp - 1;
    for (; k + 4 <= n1; k += 4, h -= 4) {
        acc += (int64_t)h[0] * c[k]; acc += (int64_t)h[-1] * c[k + 1];
        acc += (int64_t)h[-2] * c[k + 2]; acc += (int64_t)h[-3] * c[k + 3];
    }
    for (; k < n1; k++, h--) acc += (int64_t)*h * c[k];
    for (h = hist + V92_MAX_LSEC - 1; k + 4 <= n; k += 4, h -= 4) {
        acc += (int64_t)h[0] * c[k]; acc += (int64_t)h[-1] * c[k + 1];
        acc += (int64_t)h[-2] * c[k + 2]; acc += (int64_t)h[-3] * c[k + 3];
    }
    for (; k < n; k++, h--) acc += (int64_t)*h * c[k];
    return acc;
}

int32_t v92_enc_symbol(v92_enc_t *e, v92_sym_t *s)
{
    const v92_map_t *m = e->map;
    const v92_filt_t *f = e->filt;
    int i = e->i, k = i % V92_TFRAME;
    int lc = v92_lc(m, i);
    int r, period, first, eta, n;
    int32_t fb = 0, u, x, v, out;

    /* data frame boundary: scramble K bits and run the modulus encoder */
    if (i == 0) {
        if (e->forced) {
            memcpy(e->ki, e->kf, V92_DFRAME);
            e->forced = 0;
        } else {
            uint8_t bits[V92_MAX_K];
            for (n = 0; n < m->K; n++) bits[n] = (uint8_t)(e->get(e->user) & 1);
            v90_scramble_gpa_bits(&e->scr, bits, m->K);
            v92_mod_encode(&e->mod, bits, e->ki);
        }
    }
    /* trellis frame boundary: Y0 depends on earlier trellis frames only */
    if (k == 0) e->y0 = (uint8_t)v34_conv_y0(m->trellis, e->conv);

    /* the equivalence class E(Ki): indices congruent to r modulo period */
    if (k < 3) {
        r = e->ki[i];
        period = m->M[i];
    } else {
        int par = (e->eta[0] + e->eta[1] + e->eta[2] + e->y0) % 2 != 0;
        r = 2 * e->ki[i] + par;
        period = 2 * m->M[i];
    }
    first = -lc + (r + lc) % period;        /* lowest member; r + lc >= 0 */

    /* precoder feedback: sum u(n-k) z1(k) + sum x(n-k) p1(k), in XQ */
    if (f) {
        int64_t acc = 0;                    /* Q(XQ + 14) */
        const int16_t *z1 = f->coef, *p1 = z1 + f->lz1;
        /* Q0 * Q15, one bit down; XQ * Q14 */
        acc += hdot16(e->uh, e->hp, z1, f->lz1) * (1 << (V92_XQ - 1));
        acc += hdot32(e->xh, e->hp, p1, f->lp1);
        fb = rnd_shift(acc, 14);
    }

    /* choose u(n) within the class */
    if (e->pick) {
        int16_t cand[2 * V92_MAX_LC];
        for (n = 0, eta = first; eta < lc; eta += period) cand[n++] = (int16_t)eta;
        n = e->pick(e->pick_user, i, cand, n);
        eta = cand[n];
    } else {
        int64_t best = -1;
        int e1, best_eta = first;
        for (e1 = first; e1 < lc; e1 += period) {
            int64_t xa = (int64_t)v92_point(m, i, e1) * (1 << V92_XQ) + fb;
            if (xa < 0) xa = -xa;
            /* of two equally good members the one with the higher index: what
             * a real analogue modem does (reading 1) */
            if (best < 0 || xa <= best) { best = xa; best_eta = e1; }
        }
        eta = best_eta;
    }
    u = v92_point(m, i, eta);
    e->eta[k] = (int16_t)eta;

    /* precoder filter output and prefilter */
    if (f) {
        int64_t acc = 0;                    /* Q(XQ + 15) */
        const int16_t *z2 = f->coef + f->lz1 + f->lp1, *p2 = z2 + f->lz2;
        int64_t xs = (int64_t)u * (1 << V92_XQ) + fb;
        x = xs > INT32_MAX ? INT32_MAX : xs < -INT32_MAX ? -INT32_MAX : (int32_t)xs;
        acc += (int64_t)x * z2[0];
        if (f->lz2 > 1) acc += hdot32(e->xh, e->hp, z2 + 1, f->lz2 - 1u);
        acc += hdot32(e->vh, e->hp, p2, f->lp2) * 2;    /* XQ * Q14, one bit up */
        v = rnd_shift(acc, 15);
        /* G = gain4 / 2^18; result in Q14 */
        out = rnd_shift((int64_t)v * f->gain4, V92_XQ + 18 - 14);
    } else {
        x = u * (1 << V92_XQ);
        v = x;
        out = u;
    }
    e->uh[e->hp] = (int16_t)u;
    e->xh[e->hp] = x;
    e->vh[e->hp] = v;
    e->hp = (uint16_t)(e->hp + 1 == V92_MAX_LSEC ? 0 : e->hp + 1);

    if (s) {
        s->i = (uint8_t)i;
        s->ki = e->ki[i];
        s->y0 = e->y0;
        s->eta = (int16_t)eta;
        s->u = u;
        s->x = x;
        s->v = v;
    }

    /* end of the trellis frame: clock the convolutional encoder (4T delays) */
    if (k == 3)
        e->conv = (uint8_t)v34_conv_next(m->trellis, e->conv, v92_inverse_map(e->eta));
    e->i = (uint8_t)(i + 1 == V92_DFRAME ? 0 : i + 1);
    return out;
}
