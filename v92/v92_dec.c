/* V.92 upstream data mode decoder. See v92_dec.h.
 *
 * Metrics. A symbol's metric is the squared distance in linear units with
 * the distance clipped to 8191 (so at most 2^26); a residue class with no
 * point in the constellation gets M_NONE = 2^27. A branch is four symbols:
 * at most 2^29. Path metrics are renormalised every step (the smallest is
 * subtracted) and saturate at 2^31 - 1, so 32 bits never overflow. */
#include <string.h>
#include "v92_dec.h"
#include "v92_enc.h"        /* v92_subset */
#include "v34_priv.h"       /* Table 13 and the convolutional encoders */

#define D_CLIP    8191
#define M_NONE    ((uint32_t)1 << 27)
#define PM_START  ((uint32_t)1 << 29)    /* start state other than zero */
#define PM_MAX    0x7FFFFFFFu

/* ---- slicing -------------------------------------------------------------- */

/* Largest index eta with a(eta) <= r, from -lc - 1 (r below every point) to
 * lc - 1. */
static int floor_index(const uint16_t *p, int lc, int32_t r)
{
    int lo = 0, hi = lc;                /* count of points on one side */
    if (r >= 0) {
        while (lo < hi) {               /* number of p[e] <= r */
            int mid = (lo + hi) >> 1;
            if ((int32_t)p[mid] <= r) lo = mid + 1; else hi = mid;
        }
        return lo - 1;
    }
    r = -r;
    while (lo < hi) {                   /* number of p[e] < -r */
        int mid = (lo + hi) >> 1;
        if ((int32_t)p[mid] < r) lo = mid + 1; else hi = mid;
    }
    return -lo - 1;
}

static inline int32_t pt_val(const uint16_t *p, int eta)
{
    return eta >= 0 ? (int32_t)p[eta] : -(int32_t)p[-eta - 1];
}

static inline uint32_t dist2(int32_t d)
{
    if (d < 0) d = -d;
    if (d > D_CLIP) d = D_CLIP;
    return (uint32_t)(d * d);
}

int v92_slice(const v92_map_t *m, int i, int32_t r)
{
    const uint16_t *p = m->pt[m->cidx[i % V92_CFRAME]];
    int lc = v92_lc(m, i);
    int j = floor_index(p, lc, r);
    if (j < -lc) return -lc;
    if (j >= lc - 1) return lc - 1;
    return (pt_val(p, j + 1) - r < r - pt_val(p, j)) ? j + 1 : j;
}

/* Nearest point of each residue class of the index modulo 4: the four
 * indices at or below r and the four above cover every class on both
 * sides. Returns the index of the nearest point of all. */
static int slice_classes(const v92_map_t *m, int i, int32_t r,
                         uint32_t mt[4], int16_t ix[4])
{
    const uint16_t *p = m->pt[m->cidx[i % V92_CFRAME]];
    int lc = v92_lc(m, i);
    int j = floor_index(p, lc, r);
    int n, eta, best = 0;
    uint32_t bm = 0xFFFFFFFFu;

    mt[0] = mt[1] = mt[2] = mt[3] = M_NONE;
    ix[0] = ix[1] = ix[2] = ix[3] = 0;
    for (n = 0; n < 8; n++) {
        unsigned c;
        uint32_t d;
        eta = j - 3 + n;                /* ascending: a tie keeps the lower */
        if (eta < -lc || eta >= lc) continue;
        c = (unsigned)(eta + 256) & 3;
        d = dist2(r - pt_val(p, eta));
        if (d < mt[c]) { mt[c] = d; ix[c] = (int16_t)eta; }
        if (d < bm) { bm = d; best = eta; }
    }
    return best;
}

/* ---- tables --------------------------------------------------------------- */

/* the trellis inputs the selected code uses, packed: 16 states Y2 Y1,
 * 32 states Y4 Y2 Y1, 64 states Y4 Y3 Y2 Y1 */
static unsigned pack_y(int trellis, unsigned y4321)
{
    if (trellis == V92_TRELLIS_16) return y4321 & 3;
    if (trellis == V92_TRELLIS_32) return (y4321 & 3) | ((y4321 >> 3) << 2);
    return y4321;
}

static unsigned unpack_y(int trellis, unsigned in)
{
    if (trellis == V92_TRELLIS_32) return (in & 3) | ((in >> 2) << 3);
    return in;
}

int v92_dec_init(v92_dec_t *d, const v92_map_t *map, v92_putbit_fn put, void *user)
{
    unsigned s, in, s1, s2, a, b;
    uint8_t cnt[V92_MAX_STATES];
    int tr;

    memset(d, 0, sizeof *d);
    if (v92_map_check(map) != 0) return V92_ERR_RANGE;
    d->map = map;
    d->put = put;
    d->user = user;
    d->descramble = 1;
    v90_scr_init(&d->scr);
    (void)v92_mod_init(&d->mod, map->M, map->K);

    tr = map->trellis;
    d->nstates = (uint8_t)(16 << tr);
    d->nin = (uint8_t)(4 << tr);
    d->nlab = (uint8_t)(2 * d->nin);

    /* branches: state s, inputs in -> next state, label = in << 1 | Y0(s) */
    memset(cnt, 0, sizeof cnt);
    for (s = 0; s < d->nstates; s++)
        for (in = 0; in < d->nin; in++) {
            unsigned ns = v34_conv_next(tr, s, unpack_y(tr, in));
            if (cnt[ns] >= d->nin) return V92_ERR_RANGE;   /* cannot happen */
            d->pred[ns][cnt[ns]][0] = (uint8_t)s;
            d->pred[ns][cnt[ns]][1] = (uint8_t)(in << 1 | v34_conv_y0(tr, s));
            cnt[ns]++;
        }

    /* 2D subsets: the two (a mod 4, b mod 4) pairs of each label, and the
     * parity of a + b, which is the same for both */
    memset(cnt, 0, 8);
    for (a = 0; a < 4; a++)
        for (b = 0; b < 4; b++) {
            unsigned l = v92_subset((int)a, (int)b);
            d->subres[l][cnt[l]++] = (uint8_t)(a | b << 2);
        }
    /* pairs of 2D subsets -> branch label; Y0 is the parity of the sum of
     * the four indices (6.4.2) */
    for (s1 = 0; s1 < 8; s1++)
        for (s2 = 0; s2 < 8; s2++) {
            unsigned r1 = d->subres[s1][0], r2 = d->subres[s2][0];
            unsigned par = ((r1 & 3) + (r1 >> 2) + (r2 & 3) + (r2 >> 2)) & 1;
            d->pairlab[s1 << 3 | s2] =
                (uint8_t)(pack_y(tr, v34_tab13[s1][s2]) << 1 | par);
        }

    d->pm[0] = 0;
    for (s = 1; s < d->nstates; s++) d->pm[s] = PM_START;
    return 0;
}

void v92_dec_any_start(v92_dec_t *d)
{
    unsigned s;
    for (s = 0; s < d->nstates; s++) d->pm[s] = 0;
}

void v92_dec_set_descramble(v92_dec_t *d, int on) { d->descramble = (uint8_t)(on != 0); }
void v92_dec_set_hard(v92_dec_t *d, int on) { d->hard = (uint8_t)(on != 0); }

uint32_t v92_dec_quality(const v92_dec_t *d)
{
    return d->err_avg >> 6;
}

/* ---- back end: decided indices -> bits ------------------------------------ */

static void decided(v92_dec_t *d, int eta)
{
    const v92_map_t *m = d->map;
    int i = d->i_out, n;

    d->ki[i] = (uint8_t)v92_eta_to_k(m, i, eta);
    if (++i < V92_DFRAME) { d->i_out = (uint8_t)i; return; }
    d->i_out = 0;
    {
        uint8_t bits[V92_MAX_K];
        d->frames++;
        if (v92_mod_decode(&d->mod, d->ki, bits)) d->bad_frames++;
        for (n = 0; n < m->K; n++) {
            int b = bits[n];
            if (d->descramble) b = v90_descramble_gpa(&d->scr, b);
            d->put(d->user, b);
        }
    }
}

/* deliver the trellis frame in hist slot `slot` as decided through state st */
static void emit_step(v92_dec_t *d, unsigned slot, unsigned st)
{
    const v92_vstep_t *h = &d->hist[slot];
    unsigned res = h->res[st];
    int k;
    for (k = 0; k < V92_TFRAME; k++)
        decided(d, h->idx[k][(res >> (2 * k)) & 3]);
}

/* trace back from state st at the end of the newest step over `back` steps;
 * returns the state at the end of the step `back` steps before the newest */
static unsigned trace(const v92_dec_t *d, unsigned st, unsigned back)
{
    unsigned slot = d->head, n;
    for (n = 0; n < back; n++) {
        slot = slot ? slot - 1 : V92_VIT_DEPTH - 1;
        st = d->hist[slot].prev[st];
    }
    return st;
}

static unsigned best_state(const v92_dec_t *d)
{
    unsigned s, b = 0;
    for (s = 1; s < d->nstates; s++) if (d->pm[s] < d->pm[b]) b = s;
    return b;
}

/* ---- one trellis frame ---------------------------------------------------- */

static void viterbi_step(v92_dec_t *d)
{
    v92_vstep_t *h;
    uint32_t m2[2][8], lm[32], npm[V92_MAX_STATES], minpm;
    uint8_t r2[2][8], lr[32];
    unsigned hf, s, s1, s2, l, j;

    /* 2D subsets of the two halves: the better of the two residue pairs */
    for (hf = 0; hf < 2; hf++)
        for (s = 0; s < 8; s++) {
            unsigned ra = d->subres[s][0], rb = d->subres[s][1];
            uint32_t va = d->m[2 * hf][ra & 3] + d->m[2 * hf + 1][ra >> 2];
            uint32_t vb = d->m[2 * hf][rb & 3] + d->m[2 * hf + 1][rb >> 2];
            if (vb < va) { va = vb; ra = rb; }
            m2[hf][s] = va;
            r2[hf][s] = (uint8_t)ra;
        }
    /* 4D subsets (branch labels): the best pair of 2D subsets */
    for (l = 0; l < d->nlab; l++) { lm[l] = 0xFFFFFFFFu; lr[l] = 0; }
    for (s1 = 0; s1 < 8; s1++)
        for (s2 = 0; s2 < 8; s2++) {
            uint32_t v = m2[0][s1] + m2[1][s2];
            l = d->pairlab[s1 << 3 | s2];
            if (v < lm[l]) { lm[l] = v; lr[l] = (uint8_t)(r2[0][s1] | r2[1][s2] << 4); }
        }

    /* add, compare, select */
    h = &d->hist[d->head];
    memcpy(h->idx, d->ix, sizeof h->idx);
    minpm = 0xFFFFFFFFu;
    for (s = 0; s < d->nstates; s++) {
        uint32_t bv = 0xFFFFFFFFu;
        unsigned bj = 0;
        for (j = 0; j < d->nin; j++) {
            uint32_t v = d->pm[d->pred[s][j][0]] + lm[d->pred[s][j][1]];
            if (v < bv) { bv = v; bj = j; }
        }
        npm[s] = bv;
        h->prev[s] = d->pred[s][bj][0];
        h->res[s] = lr[d->pred[s][bj][1]];
        if (bv < minpm) minpm = bv;
    }
    for (s = 0; s < d->nstates; s++) {
        uint32_t v = npm[s] - minpm;
        d->pm[s] = v > PM_MAX ? PM_MAX : v;
    }
    /* minpm is what the best path gained in this frame of four symbols */
    minpm >>= 2;
    if (minpm > ((uint32_t)1 << 25)) minpm = (uint32_t)1 << 25;
    d->err_avg = d->err_avg - (d->err_avg >> 6) + minpm;

    d->head = (uint16_t)(d->head + 1 == V92_VIT_DEPTH ? 0 : d->head + 1);
    d->nsteps++;
    /* once the history is full its oldest step (now under head) is decided */
    if (d->nsteps >= V92_VIT_DEPTH)
        emit_step(d, d->head, trace(d, best_state(d), V92_VIT_DEPTH - 1));
}

void v92_dec_put(v92_dec_t *d, int32_t r, int32_t *tent)
{
    int i = d->i_in, k = i % V92_TFRAME;
    int near;

    if (r > 0x100000) r = 0x100000;         /* keeps the differences in range */
    if (r < -0x100000) r = -0x100000;
    if (d->hard) {
        near = v92_slice(d->map, i, r);
        decided(d, near);
    } else {
        near = slice_classes(d->map, i, r, d->m[k], d->ix[k]);
        if (k == V92_TFRAME - 1) viterbi_step(d);
    }
    if (tent) *tent = v92_point(d->map, i, near);
    d->i_in = (uint8_t)(i + 1 == V92_DFRAME ? 0 : i + 1);
}

void v92_dec_flush(v92_dec_t *d)
{
    unsigned held, n, slot, st;
    uint8_t path[V92_VIT_DEPTH];

    if (d->hard) return;
    /* a full history has already delivered its oldest step */
    held = d->nsteps < V92_VIT_DEPTH ? (unsigned)d->nsteps : V92_VIT_DEPTH - 1;
    /* states along the best path, newest first */
    st = best_state(d);
    slot = d->head;
    for (n = 0; n < held; n++) {
        path[n] = (uint8_t)st;
        slot = slot ? slot - 1 : V92_VIT_DEPTH - 1;
        st = d->hist[slot].prev[st];
    }
    /* deliver oldest first; a trailing partial data frame stays in ki[] */
    for (n = held; n-- > 0; ) {
        slot = (d->head + V92_VIT_DEPTH - 1 - n) % V92_VIT_DEPTH;
        emit_step(d, slot, path[n]);
    }
    d->nsteps = 0;
    d->head = 0;
}

void v92_dec_flush_before(v92_dec_t *d, unsigned n, const int32_t *y)
{
    unsigned held, part, c, k, q, slot, st, s, bs = 0, kE, slotE;
    uint8_t path[V92_VIT_DEPTH];
    uint64_t best = UINT64_MAX;

    if (d->hard) return;
    held = d->nsteps < V92_VIT_DEPTH ? (unsigned)d->nsteps : V92_VIT_DEPTH - 1;
    part = (unsigned)d->i_in % V92_TFRAME;     /* inputs of the trellis frame under way */
    if (held * V92_TFRAME + part <= n) return;
    c = (held * V92_TFRAME + part - n) / V92_TFRAME;   /* trellis frames wholly before them */
    if (c == 0) return;
    if (c > held) c = held;
    /* the newest of them, k back from the newest held (0) */
    kE = held - c;
    slotE = (d->head + 2u * V92_VIT_DEPTH - 1u - kE) % V92_VIT_DEPTH;
    if (y) {
        /* The best state now is the best through what came after them: the
         * path through them is the survivor, of the states at the newest
         * of them, that costs least over them, from the inputs given */
        for (s = 0; s < d->nstates; s++) {
            uint64_t cost = 0;
            st = s; slot = slotE;
            for (k = kE; k < held; k++) {
                const v92_vstep_t *h = &d->hist[slot];
                unsigned res = h->res[st];
                for (q = 0; q < V92_TFRAME; q++) {
                    unsigned j = part + V92_TFRAME * k + (V92_TFRAME - 1 - q);      /* inputs back */
                    int i = ((int)d->i_in - 1 - (int)j) % V92_DFRAME;
                    if (i < 0) i += V92_DFRAME;
                    cost += dist2(y[j] - v92_point(d->map, i, h->idx[q][(res >> (2 * q)) & 3]));
                }
                st = h->prev[st];
                slot = slot ? slot - 1 : V92_VIT_DEPTH - 1;
            }
            if (cost < best) { best = cost; bs = s; }
        }
        st = bs;
    } else {
        st = best_state(d);
        slot = d->head;
        for (k = 0; k < kE; k++) {
            slot = slot ? slot - 1 : V92_VIT_DEPTH - 1;
            st = d->hist[slot].prev[st];
        }
    }
    /* states along that path, from the newest of them back */
    slot = slotE;
    for (k = 0; k < c; k++) {
        path[k] = (uint8_t)st;
        st = d->hist[slot].prev[st];
        slot = slot ? slot - 1 : V92_VIT_DEPTH - 1;
    }
    /* oldest first; a data frame they leave partial stays in ki[] */
    for (k = c; k-- > 0; )
        emit_step(d, (slotE + V92_VIT_DEPTH - k) % V92_VIT_DEPTH, path[k]);
    d->nsteps = 0;
    d->head = 0;
}
