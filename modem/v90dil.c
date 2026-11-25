/* V.90 analogue modem: DIL design and analysis, CPt / CP design. See v90dil.h. */
#include <string.h>
#include "v90dil.h"
#include "v90_pcm.h"
#include "v90_enc.h"
#include "fixmath.h"

#define YF 4                        /* V90ARX_YF */
#define ACC_MAX 1024                /* symbols summed per interval and Ucode, at most */

/* ---- the DIL ---- */

void v90dil_design(v90_dil_t *d, int uinfo)
{
    /* SP, first in time in bit 0: + + + + - - + - + + - - - + - + - +  */
    static const uint8_t SP[18] = { 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0, 1 };
    int c, i;
    memset(d, 0, sizeof *d);
    d->n = V90DIL_NU;
    d->lsp = 18; d->ltp = 18;
    for (i = 0; i < 18; i++) {
        if (SP[i]) d->sp[i >> 3] |= (uint8_t)(1u << (i & 7));
        if (i >= 3) d->tp[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    for (c = 0; c < 8; c++) { d->h[c] = 2; d->ref[c] = (uint8_t)(uinfo & 127); }
    /* up by the even Ucodes, down by the odd ones (0 2 ... 100 99 97 ...
     * 1, then 0 again): no segment follows one much larger than itself. In
     * the order 0 .. 119 (the DIL of 0.5) the segment of Ucode 0 followed that of 119, and
     * what the equaliser leaves of fifteen symbols at the largest level (its
     * residual interference, deterministic in a pattern that repeats every
     * pass, so not averaged out) stood in the means of Ucodes 0 to 6 at up
     * to 160 units on a 0 km line, where their true levels are 0 to 50:
     * CP then took such a Ucode as its smallest point (docs/history/PROGRESS.md, robust) */
    for (i = 0; i < V90DIL_NU; i++) d->train[i] = (uint8_t)(i < (V90DIL_NU + 1) / 2 ? 2 * i : 2 * (V90DIL_NU - 1 - i) + 1);
}

void v90dil_cur_init(v90dil_cur_t *c) { c->seg = c->pos = 0; }

int v90dil_next(const v90_dil_t *d, v90dil_cur_t *c, int *ucode, int *pos)
{
    int u, ch, L, ps, pt, first = c->pos == 0;
    if (d->n == 0 || d->lsp == 0 || d->ltp == 0) { *ucode = 0; *pos = 1; return 0; }
    u = d->train[c->seg] & 127;
    ch = u >> 4;                    /* Hc, REFc: the Uchord of the training symbol */
    L = (d->h[ch] + 1) * 6;
    ps = c->pos % d->lsp; pt = c->pos % d->ltp;
    *pos = (d->sp[ps >> 3] >> (ps & 7)) & 1;
    *ucode = ((d->tp[pt >> 3] >> (pt & 7)) & 1) ? u : (d->ref[ch] & 127);
    if (++c->pos >= L) { c->pos = 0; if (++c->seg >= d->n) c->seg = 0; }
    return first;
}

/* ---- analysis ---- */

void v90dil_acc_init(v90dil_acc_t *a) { memset(a, 0, sizeof *a); }

void v90dil_acc_add(v90dil_acc_t *a, int slot, int ucode, int pos, int32_t y)
{
    if (slot < 0 || slot > 5 || ucode < 0 || ucode > 127) return;
    if (y > (1 << 21)) y = 1 << 21;
    if (y < -(1 << 21)) y = -(1 << 21);
    if (a->cnt[slot][ucode] >= ACC_MAX) return;
    a->sum[slot][ucode] += pos ? y : -y;
    a->cnt[slot][ucode]++;
}

int v90dil_acc_min(const v90dil_acc_t *a)
{
    int s, u, m = ACC_MAX;
    for (s = 0; s < 6; s++) for (u = 0; u < V90DIL_NU; u++) if (a->cnt[s][u] < m) m = a->cnt[s][u];
    return m;
}

static int32_t mean(int32_t sum, int cnt)
{
    if (cnt <= 0) return 0;
    return sum >= 0 ? (sum + cnt / 2) / cnt : -((-sum + cnt / 2) / cnt);
}

int32_t v90dil_acc_mean(const v90dil_acc_t *a, int slot, int ucode)
{
    return mean(a->sum[slot][ucode], a->cnt[slot][ucode]);
}

/* Table 1 of one law (v90_ucode_linear computes it: kept here while it is
 * looked up thousands of times) */
static void table1(int alaw, int32_t *t)
{
    int u;
    for (u = 0; u < 128; u++) t[u] = v90_ucode_linear(alaw, (unsigned)u);
}

/* the Ucode whose Table 1 level (Q YF) is nearest x */
static int nearest(const int32_t *t1, int64_t x)
{
    int lo = 0, hi = 127;
    if (x <= 0) return 0;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if ((int64_t)t1[mid] * (1 << YF) <= x) lo = mid; else hi = mid - 1;
    }
    if (lo < 127 && (int64_t)t1[lo + 1] * (1 << YF) - x < x - (int64_t)t1[lo] * (1 << YF)) lo++;
    return lo;
}

/* nearest(t1, (l << 16) / g) for a level l >= 0, without the division
 * (ARMv5 divides 64-bit numbers in software, a few hundred instructions;
 * the analysis does it some 25000 times): with x = floor(L / g), L = l 2^16,
 * g > 0, t1[i] 2^YF <= x is t1[i] 2^YF g <= L, x <= 0 is L < g, and x
 * nearer t1[lo + 1] than t1[lo] (2 x > (t1[lo] + t1[lo + 1]) 2^YF) is L >=
 * ((t1[lo] + t1[lo + 1]) 2^(YF - 1) + 1) g. Table 1 ascends, so the largest
 * i whose level is not above x is found by walking from *hint (the last
 * answer: the levels of one interval mostly ascend with the Ucode) as well
 * as by halving. The same Ucode as nearest(). */
static int nearest_g(const int32_t *t1, int64_t l, int64_t g, int *hint)
{
    int64_t L = l * 65536;
    int i = *hint;
    if (g <= 0 || g > INT32_MAX) return nearest(t1, (l << 16) / g);    /* (never: g is near 2^16) */
    if (L < g) return 0;
    {
        int32_t g32 = (int32_t)g;
        while (i < 127 && (int64_t)(t1[i + 1] * (1 << YF)) * g32 <= L) i++;
        while (i > 0 && (int64_t)(t1[i] * (1 << YF)) * g32 > L) i--;
        *hint = i;
        if (i < 127 && (int64_t)((t1[i] + t1[i + 1]) * (1 << (YF - 1)) + 1) * g32 <= L) i++;
    }
    return i;
}

/* sum of the squared distances of the measured levels (Ucodes from `from`)
 * to g times their nearest Table 1 level; g in Q16. The terms are not
 * negative: once the sum reaches `stop` (>= 0) it is returned as it stands,
 * which is enough for a caller that only asks whether it stays below. */
static int64_t snap_cost(const v90dil_res_t *r, const int32_t *t1, int64_t g, int from, int64_t stop)
{
    int64_t c = 0;
    int s, u;
    for (s = 0; s < 6; s++) {
        int hint = 0;
        for (u = from; u < 128; u++) {
            int64_t l = r->lv[s][u], d;
            if (l < 0) continue;
            d = l - (((int64_t)t1[nearest_g(t1, l, g, &hint)] * (1 << YF) * g) >> 16);
            c += d * d >> 8;
        }
        if (stop >= 0 && c >= stop) return c;
    }
    return c;
}

/* The analysis runs in steps (v90dil_an_t), each some tens of thousands of
 * ARM instructions at most: begin, one gain candidate a step, the common
 * gain, one interval's own gain a step, one interval's snapping a step, the
 * robbed bits. v90dil_analyse() runs them all. */
enum { AN_CAND = 1, AN_GAIN, AN_SLOT, AN_SNAP, AN_END, AN_DONE };

void v90dil_analyse_begin(v90dil_an_t *an, const v90dil_acc_t *a, int alaw, int uinfo, v90dil_res_t *r)
{
    int s, u;
    memset(an, 0, sizeof *an);
    an->alaw = alaw; an->uinfo = uinfo; an->bv = uinfo; an->best = -1;
    an->tu = (int64_t)v90_ucode_linear(alaw, (unsigned)(uinfo & 127));
    memset(r, 0, sizeof *r);
    table1(alaw, an->t1);
    for (s = 0; s < 6; s++)
        for (u = 0; u < 128; u++) r->lv[s][u] = a->cnt[s][u] ? mean(a->sum[s][u], a->cnt[s][u]) : -1;
    an->v = uinfo;
    an->phase = AN_CAND;
}

int v90dil_analyse_step(v90dil_an_t *an, v90dil_res_t *r)
{
    const int32_t *t1 = an->t1;
    int s, u;
    switch (an->phase) {
    case AN_CAND:
        /* Whatever a pad or a robbed bit does, the far codec puts out the Table 1
         * level of some codeword; the receiver has scaled everything so that
         * UINFO, arriving as codeword v, is at UINFO's own level: the levels are
         * Table 1's times g = T(UINFO) / T(v). v is found as the one whose g
         * leaves the measurements of Ucodes 64 and up closest to such levels
         * (a pad of up to 15 dB), g is then refined by least squares (the
         * receiver's own gain is not exact), and every level is replaced by g
         * times the Table 1 level nearest to it. What the DIL measures beyond
         * that, the residual interference of its repetitive patterns, is not
         * there in data mode. One candidate a step. */
        for (; an->v >= 1 && an->v >= an->uinfo - 40; an->v--) {
            int64_t c, tv = t1[an->v], best = an->best;
            if (tv <= 0) continue;
            /* from Ucode 32: A-law's upper segments are exact doublings of each
             * other, and only its lower ones tell g from 2 g; a near tie goes
             * to the smaller pad */
            c = snap_cost(r, t1, (an->tu << 16) / tv, 32, best < 0 ? -1 : best - best / 8);
            if (best < 0 || c < best - best / 8) { an->best = c; an->bv = an->v; }
            an->v--;
            return 0;
        }
        an->phase = AN_GAIN;
        return 0;
    case AN_GAIN: {
        int64_t g, num = 0, den = 0;
        int32_t tb = v90_ucode_linear(an->alaw, (unsigned)an->bv);
        g = (an->tu << 16) / (tb > 0 ? tb : 1);
        for (s = 0; s < 6; s++) {
            int hint = 0;
            for (u = 64; u < 128; u++) {
                int64_t l = r->lv[s][u], t;
                if (l < 0) continue;
                t = (int64_t)t1[nearest_g(t1, l, g, &hint)] * (1 << YF);
                num += (l * t) >> 8; den += (t * t) >> 8;
            }
        }
        if (den > 0) g = (num << 16) / den;
        r->gain_q16 = (int32_t)g;
        r->uinfo_as = an->bv;
        an->g = g;
        an->s = 0;
        an->phase = AN_SLOT;
        return 0;
    }
    case AN_SLOT: {
        /* ... and by least squares again in each interval alone: what the DIL's
         * repetitive patterns leave of the equaliser's residual interference
         * differs from interval to interval (the positions in a segment, next to
         * the REF symbols or not), a gain of its own of up to +-0.9 % (measured
         * on 2 km with a 3 dB pad: intervals 0..2 low, 3..5 high), which at a
         * pad's codeword spacing was enough to snap a level to its neighbour
         * (docs/history/PROGRESS.md, robust 2b). The codeword is found with the interval's
         * gain, held within 3 % of the common one; its level is the common
         * gain's. One interval a step. */
        int it;
        int64_t g = an->g, *gs = an->gs;
        s = an->s;
        gs[s] = g;
        for (it = 0; it < 2; it++) {
            int64_t ns = 0, ds = 0, gn;
            int hint = 0;
            for (u = 64; u < 128; u++) {
                int64_t l = r->lv[s][u], t;
                if (l < 0) continue;
                t = (int64_t)t1[nearest_g(t1, l, gs[s], &hint)] * (1 << YF);
                ns += (l * t) >> 8; ds += (t * t) >> 8;
            }
            if (ds <= 0) break;
            gn = (ns << 16) / ds;
            if (gn > g + g * 3 / 100) gn = g + g * 3 / 100;
            if (gn < g - g * 3 / 100) gn = g - g * 3 / 100;
            gs[s] = gn;
        }
        if (++an->s == 6) { an->s = 0; an->phase = AN_SNAP; }
        return 0;
    }
    case AN_SNAP: {
        int64_t g = an->g;
        int hint = 0;
        s = an->s;
        for (u = 0; u < 128; u++) {
            int64_t l = r->lv[s][u], w, st;
            if (l < 0) continue;
            /* the codeword by the interval's gain; its level by the common
             * one (data mode has no such bias) */
            w = nearest_g(t1, l, an->gs[s], &hint);
            /* where the codec's step is under 32 units the measurement's own
             * noise would pick a neighbour as often: the mean stands */
            st = w < 127 ? t1[w + 1] - t1[w] : 1024;
            if (st * g < ((int64_t)32 << 16)) continue;
            r->lv[s][u] = (int32_t)(((int64_t)t1[w] * (1 << YF) * g + 32768) >> 16);
        }
        if (++an->s == 6) an->phase = AN_END;
        return 0;
    }
    case AN_END: {
        int looked = 0;                 /* Ucodes from 64 up measured in all six */
        /* A robbed bit takes one interval in six away from the others: where the
         * others agree (their median), it stands off by more than a quarter of
         * the Ucode's step at many Ucodes (mu-law; with a pad in front the
         * codewords that lose to it are no longer just the odd Ucodes) */
        for (u = 64; !an->alaw && u < 128; u++) {
            int32_t vv[6], t, step = (t1[u] - t1[u - 1]) * (1 << YF), med;
            int i, j, ok = 1;
            for (i = 0; i < 6; i++) { vv[i] = r->lv[i][u]; if (vv[i] < 0) ok = 0; }
            if (!ok) continue;
            looked++;
            for (i = 1; i < 6; i++) for (j = i; j > 0 && vv[j - 1] > vv[j]; j--) { t = vv[j]; vv[j] = vv[j - 1]; vv[j - 1] = t; }
            med = (vv[2] + vv[3]) / 2;
            for (s = 0; s < 6; s++) {
                int32_t d = r->lv[s][u] - med;
                if (d < 0) d = -d;
                if (d > step / 4 && r->robbed[s] < 255) r->robbed[s]++;
            }
        }
        /* at a quarter of the Ucodes looked at (the DIL has 37 from 64 up) */
        for (s = 0; s < 6; s++) r->robbed[s] = (uint8_t)(looked > 0 && 4 * r->robbed[s] >= looked);
        for (s = 0; s < 6; s++)
            for (u = 64; u < 128; u++) {
                int32_t t = t1[u] * (1 << YF), e;
                if (r->lv[s][u] < 0) continue;
                e = r->lv[s][u] - t;
                if (e < 0) e = -e;
                /* in 32 bits where it fits (ARMv5 divides 64-bit numbers slowly) */
                e = e < (1 << 23) ? e * 256 / t : (int32_t)(((int64_t)e * 256) / t);
                if (e > r->dev_q8) r->dev_q8 = e;
            }
        an->phase = AN_DONE;
        return 1;
    }
    default:
        return 1;
    }
}

void v90dil_analyse(const v90dil_acc_t *a, int alaw, int uinfo, v90dil_res_t *r)
{
    v90dil_an_t an;
    v90dil_analyse_begin(&an, a, alaw, uinfo, r);
    while (!v90dil_analyse_step(&an, r)) ;
}

/* ---- power (8.5.2) and its limit (Table 15) ---- */

int32_t v90dil_limit(int code)
{
    static const int16_t T15[32] = {
        15124, 14276, 13480, 12724, 12012, 11340, 10708, 10108, 9544, 9008, 8504, 8028, 7580, 7156, 6756, 6380,
        6020, 5684, 5368, 5068, 4784, 4516, 4264, 4024, 3800, 3588, 3388, 3196, 3020, 2852, 2692, 2540 };
    if (code < 0) code = 0;
    if (code > 31) code = 31;
    return T15[code];
}

/* n / 2^K in Q24, rounded up */
static uint64_t frac24(uint64_t n, int K)
{
    if (K >= 24) return (n + (((uint64_t)1 << (K - 24)) - 1)) >> (K - 24);
    return n << (24 - K);
}

static uint32_t power_t1(const int32_t *t1, int K, const uint8_t M[6], const uint8_t C[6][128])
{
    uint64_t R[7], A[7], two = (uint64_t)1 << K, sum = 0, den;
    int Ki[6], i, j;
    R[0] = two - 1; A[0] = 1;
    for (i = 0; i < 6; i++) {
        if (M[i] == 0) return 0xffffffffu;
        Ki[i] = (int)(R[i] % M[i]);
        R[i + 1] = (R[i] - (uint64_t)Ki[i]) / M[i];
        A[i + 1] = A[i] * M[i];
    }
    for (i = 0; i < 6; i++)
        for (j = 0; j < M[i]; j++) {
            uint64_t p = (uint64_t)t1[C[i][j] & 127], n;
            p *= p;
            if (j < Ki[i]) n = A[i] * (R[i + 1] + 1);
            else if (j == Ki[i]) n = two - A[i] * (R[i] - R[i + 1]);
            else n = A[i] * R[i + 1];
            sum += p * frac24(n, K);
        }
    den = (uint64_t)6 << 24;
    sum = (sum + den - 1) / den;
    return sum > 0xffffffffu ? 0xffffffffu : (uint32_t)sum;
}

uint32_t v90dil_power(int alaw, int K, const uint8_t M[6], const uint8_t C[6][128])
{
    int32_t t1[128];
    table1(alaw, t1);
    return power_t1(t1, K, M, C);
}

/* ---- constellations ---- */

typedef v90dil_dsn_t dsn_t;

/* the points of interval s, from the bottom up, d apart */
static int pick(const dsn_t *g, int s, int32_t d, uint8_t *pts)
{
    int u, n = 0;
    int32_t last = 0;
    for (u = 0; u < 128; u++) {
        int32_t l = g->res->lv[s][u];
        if (l < 0) continue;
        if (n == 0 ? 2 * l < d : l - last < d) continue;
        pts[n++] = (uint8_t)u; last = l;
    }
    return n;
}

/* mean power of the lowest m points, Table 1 levels */
static uint64_t pmean(const dsn_t *g, int s, int m)
{
    uint64_t a = 0;
    int j;
    for (j = 0; j < m; j++) { uint64_t l = (uint64_t)g->t1[g->pts[s][j]]; a += l * l; }
    return m ? a / (uint64_t)m : 0;
}

/* Can K bits per frame be carried with points z (Q4) sigma apart, within the
 * power limit? Leaves the design in g. */
static int feasible(dsn_t *g, int K, int z_q4)
{
    uint64_t prod = 1, need = (uint64_t)1 << K;
    int s, j, m0;
    for (s = 0; s < 6; s++) {
        int32_t d = (int32_t)(((int64_t)z_q4 * g->sig[s] + 8) >> 4);
        g->n[s] = (uint8_t)pick(g, s, d, g->pts[s]);
        if (g->n[s] == 0) return 0;
    }
    /* the least moduli with prod >= 2^K: start even, then where it costs least */
    for (m0 = 1; m0 < 128; m0++) {
        uint64_t p = 1; int t;
        for (t = 0; t < 6; t++) p *= (uint64_t)(m0 + 1);
        if (p > need) break;
    }
    for (s = 0; s < 6; s++) { g->M[s] = (uint8_t)(m0 < g->n[s] ? m0 : g->n[s]); prod *= g->M[s]; }
    while (prod < need) {
        int best = -1;
        uint64_t bc = 0;
        for (s = 0; s < 6; s++) {
            uint64_t c;
            if (g->M[s] >= g->n[s]) continue;
            /* the power added, against the bits gained: (P(M+1) - P(M)) M */
            c = (pmean(g, s, g->M[s] + 1) - pmean(g, s, g->M[s])) * g->M[s];
            if (best < 0 || c < bc) { best = s; bc = c; }
        }
        if (best < 0) return 0;
        prod = prod / g->M[best] * (uint64_t)(g->M[best] + 1);
        g->M[best]++;
    }
    /* none more than needed: take back from the dearest */
    for (;;) {
        int best = -1;
        uint64_t bc = 0;
        for (s = 0; s < 6; s++) {
            uint64_t c;
            if (g->M[s] <= 1 || prod / g->M[s] * (uint64_t)(g->M[s] - 1) < need) continue;
            c = pmean(g, s, g->M[s]) - pmean(g, s, g->M[s] - 1);
            if (best < 0 || c > bc) { best = s; bc = c; }
        }
        if (best < 0) break;
        prod = prod / g->M[best] * (uint64_t)(g->M[best] - 1);
        g->M[best]--;
    }
    /* labels: 0 the largest */
    for (s = 0; s < 6; s++) for (j = 0; j < g->M[s]; j++) g->C[s][j] = g->pts[s][g->M[s] - 1 - j];
    g->power = power_t1(g->t1, K, g->M, (const uint8_t (*)[128])g->C);
    return (uint64_t)g->power <= g->lim2;
}

enum { CP_K = 1, CP_Z, CP_FIN, CP_DONE };

void v90dil_design_begin(v90dil_cpjob_t *j, const v90dil_res_t *res, const v90dil_req_t *req, v90_cp_t *cp, v90dil_choice_t *ch)
{
    dsn_t *g = &j->g;
    int s;
    int64_t lim = v90dil_limit(req->max_tx_code);
    memset(j, 0, sizeof *j);
    j->req = *req;
    memset(cp, 0, sizeof *cp);
    memset(ch, 0, sizeof *ch);
    g->res = res; g->alaw = req->alaw;
    table1(req->alaw, g->t1);
    /* Table 15 less 0.1 dB (a factor of 0.977 in power) */
    g->lim2 = (uint64_t)(lim * lim) * 977 / 1000;
    for (s = 0; s < 6; s++) {
        g->sig[s] = (int32_t)isqrt_u64(req->sigma2_q8[s]);     /* units Q4 = Q YF */
        if (g->sig[s] < 16) g->sig[s] = 16;         /* not below one unit */
    }
    cp->is_cp = (uint8_t)(req->is_cp != 0);
    cp->alaw = (uint8_t)(req->alaw != 0);
    cp->rates = req->up_rates;
    cp->rms_ratio = 0x2000;
    for (s = 0; s < 6; s++) cp->idx[s] = (uint8_t)s;
    /* Sr = 0: D = K + 6. CP: drn = D - 20, 1..22 (Table 2: K 15..36 here);
     * CPt: drn = D - 8, K 6..24 (Table 17) */
    if (req->is_cp) { j->kmin = 15; j->kmax = 36; }
    else { j->kmin = 6; j->kmax = req->max_k > 0 && req->max_k < 24 ? req->max_k : 24; }
    j->K = j->kmax;
    j->phase = CP_K;
}

/* one feasible() a step */
int v90dil_design_step(v90dil_cpjob_t *j, const v90dil_res_t *res, v90_cp_t *cp, v90dil_choice_t *ch)
{
    const v90dil_req_t *req = &j->req;
    dsn_t *g = &j->g;
    int s;
    g->res = res;
    switch (j->phase) {
    case CP_K:
        for (; j->K >= j->kmin; j->K--) {
            int D = j->K + 6, bps = D * 4000 / 3;
            if (req->is_cp) {
                int r = D - 21;
                if (req->rates && !((req->rates >> r) & 1)) continue;
            }
            if (req->max_bps > 0 && bps > req->max_bps) continue;
            if (feasible(g, j->K, req->z_q4)) {
                /* as much margin as the power allows */
                j->lo = req->z_q4; j->hi = 64 * 16;
                j->phase = CP_Z;
            } else j->K--;
            return 0;
        }
        j->k = 0;                                   /* drn = 0: cleardown */
        j->phase = CP_DONE;
        return 1;
    case CP_Z:
        if (j->lo < j->hi) {
            int z = (j->lo + j->hi + 1) / 2;
            if (feasible(g, j->K, z)) j->lo = z; else j->hi = z - 1;
            return 0;
        }
        j->phase = CP_FIN;
        return 0;
    case CP_FIN: {
        int K = j->K;
        (void)feasible(g, K, j->lo);
        ch->K = K; ch->D = K + 6; ch->bps = ch->D * 4000 / 3; ch->z_q4 = j->lo; ch->power = g->power;
        cp->drn = (uint8_t)(req->is_cp ? ch->D - 20 : ch->D - 8);
        for (s = 0; s < 6; s++) {
            int i;
            ch->M[s] = g->M[s];
            for (i = 0; i < g->M[s]; i++) cp->mask[s][g->C[s][i] >> 3] |= (uint8_t)(1u << (g->C[s][i] & 7));
        }
        j->k = K;
        j->phase = CP_DONE;
        return 1;
    }
    default:
        return 1;
    }
}

int v90dil_design_cp(const v90dil_res_t *res, const v90dil_req_t *req, v90_cp_t *cp, v90dil_choice_t *ch)
{
    v90dil_cpjob_t j;
    v90dil_design_begin(&j, res, req, cp, ch);
    while (!v90dil_design_step(&j, res, cp, ch)) ;
    return j.k;
}
