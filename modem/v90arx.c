/* V.90 analogue modem: PCM downstream receiver. See v90arx.h. */
#include <string.h>
#include <limits.h>
#include "v90arx.h"
#include "clock.h"
#include "divrcp.h"
#include "v34_tabs.h"           /* the fractional delay bank, shared with qamrx */
#include "interp2.h"
#include "v90_pcm.h"
#include "sintab.h"           /* the band edge's image (edge_image) */
#include "fixmath.h"

#define ONE32    ((int64_t)1 << 32)
#define AGC_RMS  4096           /* Sd at the equaliser input */
#define SD_FLOOR 16             /* A/D units: weaker Sd is not looked at */
#define SD_SKIP  12             /* periods of Sd before its clock is measured */
#define TBLK     64             /* symbols per timing measurement */
#define ECLIP    (1 << 15)      /* error used for adaptation, Q YF */
#define FASTN    8000           /* equaliser symbols with the larger steps (1 s) */
#define LS_FROM  700            /* estimator steps over which each interval's UINFO level is measured (ls_set) */
#define LS_TO    1300
/* The far end is not trusted: whatever the line carries, the clock ratio
 * stays within this of one (Q32; 1000 ppm, ten times what V.90's clocks
 * may be off), the phase slew within half of that, and the
 * interpolator makes at most this many outputs per input sample. */
#define STEP_MAX ((int64_t)4294967)
#define CLK_TRUST ((int64_t)21475)  /* 5 ppm: Sd overrules a clock set with v90arx_clock() */
#define SLEW_MAX (STEP_MAX / 2)
#define PRODUCE_MAX 3

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static int32_t asr64(int64_t v, int s) { return (int32_t)((v + ((int64_t)1 << (s - 1))) >> s); }

static int16_t sat16(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }

void v90arx_cons_mask(v90arx_cons_t *c, int alaw, int slot, const uint8_t mask[16])
{
    int u, n = 0;
    for (u = 0; u < 128; u++)
        if ((mask[u >> 3] >> (u & 7)) & 1) {
            c->u[slot][n] = (uint8_t)u;
            c->lv[slot][n] = v90_ucode_linear(alaw, (unsigned)u) << V90ARX_YF;
            n++;
        }
    c->n[slot] = (uint8_t)n;
}

void v90arx_init(v90arx_t *r, const v90arx_cfg_t *cfg)
{
    int i;
    memset(r, 0, sizeof *r);
    r->cfg = *cfg;
    if (r->cfg.ntrain <= 0) r->cfg.ntrain = V90ARX_NTRAIN;
    ec_init(&r->ec);
    r->ec_mu = -1;
    r->step = ONE32;
    r->tnext = ONE32;
    r->gain = 4096;
    v92_p6_init(&r->sd);
    r->L = (int16_t)v90_ucode_linear(cfg->alaw, (unsigned)cfg->uinfo);
    if (r->L < 1) r->L = 1;             /* mu-law Ucode 0: it divides */
    r->l_rcp = divrcp_rcp((uint64_t)r->L);
    r->ll_rcp = divrcp_rcp((uint64_t)((int64_t)r->L * r->L));
    r->hnext_m0 = -1; r->eqnext_m = -1;
    for (i = 0; i < 6; i++) r->ls[i] = r->L;
    /* until told otherwise: the two points of UINFO (TRN1d, Jd, Jd') */
    for (i = 0; i < 6; i++) {
        r->cons.n[i] = 1; r->cons.u[i][0] = (uint8_t)cfg->uinfo;
        r->cons.lv[i][0] = (int32_t)r->L << V90ARX_YF;
    }
    r->state = V90ARX_WAIT;
}

void v90arx_ec(v90arx_t *r, int mu_shift) { r->ec_mu = mu_shift; }
void v90arx_timing_hold(v90arx_t *r, int on) { r->thold = on != 0; if (r->thold) r->slew = 0; }

void v90arx_edge(v90arx_t *r, int on)
{
    if ((on != 0) != r->edge) r->redo = 1;      /* at once, not at the next computation due */
    r->edge = on != 0;
}
void v90arx_hold(v90arx_t *r, int on)
{
    r->hold = on != 0;
    if (r->hold) r->slew = 0;   /* the timing loop's phase correction is per block: none while it waits */
}

void v90arx_set_cons(v90arx_t *r, const v90arx_cons_t *c, int32_t m)
{
    r->next = *c; r->next_m = m; r->have_next = 1;
}

void v90arx_rebase(v90arx_t *r)
{
    r->n0 += CLK_REBASE;
    r->eqnext_m = -1; r->hnext_m0 = -1;     /* m moves */
    if (r->have_next) r->next_m = clk_back(r->next_m);
}

int32_t v90arx_ppm_q8(const v90arx_t *r)
{
    return (int32_t)(((r->step - ONE32) * 256000000LL) >> 32);
}

/* ---- training ------------------------------------------------------------ */

/* The estimator's two loops over the symbol ring sb[] (256, indexed by
 * symbol & 255) taken newest first from m0: in two runs that do not wrap,
 * four taps a pass (ARMv5: no index arithmetic per tap). The same integer
 * sums as one loop over (m0 - j) & 255. */
static int64_t dot_sb(const int32_t *h, const int16_t *sb, int32_t m0)
{
    int64_t acc = 0;
    int i0 = (int)(m0 & 255), j = 0, n1 = i0 + 1 < V90ARX_NCH ? i0 + 1 : V90ARX_NCH;
    const int16_t *s = sb + i0;
    for (; j + 4 <= n1; j += 4, s -= 4)
        { acc += (int64_t)h[j] * s[0]; acc += (int64_t)h[j + 1] * s[-1]; acc += (int64_t)h[j + 2] * s[-2]; acc += (int64_t)h[j + 3] * s[-3]; }
    for (; j < n1; j++, s--) acc += (int64_t)h[j] * s[0];
    s = sb + i0 - j + 256;
    for (; j + 4 <= V90ARX_NCH; j += 4, s -= 4)
        { acc += (int64_t)h[j] * s[0]; acc += (int64_t)h[j + 1] * s[-1]; acc += (int64_t)h[j + 2] * s[-2]; acc += (int64_t)h[j + 3] * s[-3]; }
    for (; j < V90ARX_NCH; j++, s--) acc += (int64_t)h[j] * s[0];
    return acc;
}

/* h[j] += (int32_t)((es * sb[(m0 - j) & 255]) >> 8), without a 64-bit
 * product per tap: es = 256 q + rr (q = es >> 8, 0 <= rr < 256), so
 * (es s) >> 8 = q s + ((rr s) >> 8) exactly, and its low 32 bits are those
 * of (uint32)q * s plus the small term (wrapping as the 64-bit sum's low
 * word does). Returned: the next step's dot_sb (on m0 + 1) but its newest
 * term, sum over j >= 1 of the new h[j] sb[(m0 + 1 - j) & 255], each tap
 * times the symbol of the tap before (p_; 0 for tap 0). The terms are at
 * most 2^31 2^15 = 2^46, 128 of them: the int64 sum is exact in any
 * order, so the same as dot_sb's. */
#define UPD1(jj, ss) do { int32_t s_ = (ss), v_ = (int32_t)((uint32_t)h[jj] + q * (uint32_t)s_ + (uint32_t)((rr * s_) >> 8)); \
        h[jj] = v_; an += (int64_t)v_ * p_; p_ = s_; } while (0)
static int64_t upd_sb(int32_t *h, const int16_t *sb, int32_t m0, int64_t es)
{
    uint32_t q = (uint32_t)(uint64_t)(es >> 8);
    int32_t rr = (int32_t)(es & 255), p_ = 0;
    int64_t an = 0;
    int i0 = (int)(m0 & 255), j = 0, n1 = i0 + 1 < V90ARX_NCH ? i0 + 1 : V90ARX_NCH;
    const int16_t *s = sb + i0;
    for (; j + 4 <= n1; j += 4, s -= 4) { UPD1(j, s[0]); UPD1(j + 1, s[-1]); UPD1(j + 2, s[-2]); UPD1(j + 3, s[-3]); }
    for (; j < n1; j++, s--) UPD1(j, s[0]);
    s = sb + i0 - j + 256;
    for (; j + 4 <= V90ARX_NCH; j += 4, s -= 4) { UPD1(j, s[0]); UPD1(j + 1, s[-1]); UPD1(j + 2, s[-2]); UPD1(j + 3, s[-3]); }
    for (; j < V90ARX_NCH; j++, s--) UPD1(j, s[0]);
    return an;
}
#undef UPD1

/* n / L (truncated, as C divides) by L's reciprocal (divrcp.h): L is fixed
 * for the call (UINFO's level) */
static int64_t div_l(const v90arx_t *r, int64_t n) { return divrcp_sdiv(n, (uint64_t)r->L, r->l_rcp); }

/* NLMS step of the channel estimate on sample k: q(k) = sum h[j] s(k + LEAD - j) / L,
 * s the symbols' levels in Table 1 units (known, or decided), pd their mean
 * square; mu = 2^-g. The sum comes from the last step when that step was
 * on m0 - 1 (hnext_m0): its terms but the newest, with the same taps on
 * the same symbols. It is good for the very next symbol only: a symbol
 * that makes no step (a hold, a gate), ls_set (sb[] rewritten), the start
 * of TRAIN (m0 goes back) and v90arx_rebase take it back. */
static void estimate(v90arx_t *r, int32_t k, int64_t pd, int g)
{
    int64_t acc, es, n;
    int32_t e, m0 = k + V90ARX_LEAD;
    int j;
    if (m0 == r->hnext_m0) acc = r->hnext + (int64_t)r->h[0] * r->sb[m0 & 255];
    else acc = dot_sb(r->h, r->sb, m0);
    r->hnext_m0 = -1;
    e = (int32_t)r->qb[k & 255] - asr64(div_l(r, acc), 16);
    if (e > 8191) e = 8191;
    if (e < -8191) e = -8191;
    /* once converged, an error beyond 4 sigma is an impairment of one symbol
     * (a robbed bit in TRN1d): not learnt from */
    if (r->nest > 600 && (int64_t)e * e > 16 * r->res) { r->res += r->res >> 10; r->nest = clk_sat_inc(r->nest); return; }
    /* h += mu e s L / (NCH pd), h in Q16, applied as (es s) >> 8. The
     * decided levels' power can decay to nothing (a far end sending Ucode
     * 0 alone): not below (L / 8)^2 */
    /* TRN1d: how far each interval's UINFO stands from L (v90arx.h, ls) */
    if (r->state == V90ARX_EST && r->nest >= LS_FROM && r->nest < LS_TO) {
        int64_t c[6] = { 0, 0, 0, 0, 0, 0 };
        int s;
        for (j = 0; j < V90ARX_NCH; j++) c[(m0 - j) % 6] += (int64_t)r->h[j] * r->sb[(m0 - j) & 255];
        for (s = 0; s < 6; s++) {
            int32_t u = asr64(div_l(r, c[s]), 16);
            r->ls_num[s] += (int64_t)e * u; r->ls_den[s] += (int64_t)u * u;
        }
    }
    if (pd < ((int64_t)r->L * r->L >> 6)) pd = (int64_t)r->L * r->L >> 6;
    if (pd < 1) pd = 1;
    /* e L 2^24 / (NCH pd), truncated: NCH = 128 divides e L 2^24 exactly,
     * so it is (e L 2^17) / pd, the same quotient. With the symbols known
     * pd is L^2, a reciprocal's division; else in 32 bits when both fit
     * (|e L| < 2^14: C's int32 division truncates as int64's does) */
    n = (int64_t)e * r->L * ((int64_t)1 << 17);
    if (pd == (int64_t)r->L * r->L) es = divrcp_sdiv(n, (uint64_t)pd, r->ll_rcp);
    else if (n > INT32_MIN && n <= INT32_MAX && pd <= INT32_MAX) es = (int32_t)n / (int32_t)pd;
    else es = n / pd;
    es >>= g;
    r->hnext = upd_sb(r->h, r->sb, m0, es);
    r->hnext_m0 = m0 + 1;
    r->res += (((int64_t)e * e) - r->res) / 64;
    r->nest = clk_sat_inc(r->nest);
}

/* How much later (Q16 samples) the response v is than u: the phase step
 * of their DFTs at w = pi/4 and pi/2 (1 and 2 kHz, where the line is
 * good), arg(V conj U) = -w d for v(j) = u(j - d), weighted by |U|^2.
 * The values are taken >> sh. */
static int32_t later_q16(const int32_t *v, const int32_t *u, int n, int sh)
{
    static const int32_t C8[8] = { 32767, 23170, 0, -23170, -32767, -23170, 0, 23170 };
    int64_t num = 0, den = 0;
    int w, j;
    for (w = 1; w <= 2; w++) {
        int64_t xr = 0, xi = 0, ur = 0, ui = 0, pr, pi, m, t, d, wt;
        int ss = 0;
        for (j = 0; j < n; j++) {
            int64_t a = (int64_t)v[j] >> sh, b = (int64_t)u[j] >> sh;
            int32_t c = C8[(j * w) & 7], si = C8[(j * w - 2) & 7];
            xr += a * c; xi -= a * si;
            ur += b * c; ui -= b * si;
        }
        m = 0;
        if ((xr < 0 ? -xr : xr) > m) m = xr < 0 ? -xr : xr;
        if ((xi < 0 ? -xi : xi) > m) m = xi < 0 ? -xi : xi;
        if ((ur < 0 ? -ur : ur) > m) m = ur < 0 ? -ur : ur;
        if ((ui < 0 ? -ui : ui) > m) m = ui < 0 ? -ui : ui;
        while ((m >> ss) >= ((int64_t)1 << 30)) ss++;
        xr >>= ss; xi >>= ss; ur >>= ss; ui >>= ss;
        pr = (xr * ur + xi * ui) >> 30;
        pi = (xi * ur - xr * ui) >> 30;
        if (pr <= 0) continue;
        t = pi * 65536 / pr;                       /* tan, Q16 */
        if (t > 32768) t = 32768;
        if (t < -32768) t = -32768;
        t -= (((t * t) >> 16) * t >> 16) / 3;      /* atan, to third order */
        /* d = -t / w with w = pi/4 or pi/2: * 4/pi or * 2/pi */
        d = -(t * (w == 1 ? 83443 : 41722)) >> 16;  /* 4/pi, 2/pi in Q16 */
        wt = (ur * ur + ui * ui) >> 30;
        num += d * wt; den += wt;
    }
    if (den <= 0) return 0;
    num /= den;
    return (int32_t)(num > 65536 ? 65536 : num < -65536 ? -65536 : num);
}

/* steps normalised by the input and decision powers: k and s such that
 * ((e k) >> 16) x >> s is e x 2^P / np, k < 2^15 */
static void norm_step(int P, int64_t np, int32_t *k, int *sh)
{
    int s = 24;
    int64_t v;
    if (np < 1) np = 1;
    v = ((int64_t)1 << (P + 16 + s)) / np;
    while (v >= 32768 && s > 1) { v >>= 1; s--; }
    *k = (int32_t)v; *sh = s;
}

static void renorm(v90arx_t *r)
{
    /* forward: taps Q20 to an output in Q YF with e in Q YF, so a step of
     * mu e x 2^20 / (NF Pq); feedback: taps Q16 on Q0 levels to Q YF, so
     * mu e d 2^(16 - YF) / (NB Pd); mu = 2^-g is applied by the caller */
    norm_step(20, (int64_t)V90ARX_NF * (r->pq >> 8), &r->kf, &r->shf);
    norm_step(16 - V90ARX_YF, (int64_t)V90ARX_NB * (r->pd >> 8), &r->kb, &r->shb);
}

static int find_cursor(v90arx_t *r)
{
    int j, c = 8;
    int64_t best = -1, hp;
    for (j = 8; j <= V90ARX_NCH - 40; j++) {
        int64_t a = r->h[j] < 0 ? -(int64_t)r->h[j] : r->h[j];
        if (a > best) { best = a; c = j; }
    }
    hp = r->h[c] / (1 << 16);
    if (hp > -16 && hp < 16) return -1;
    r->cpk = (int16_t)c;
    return 0;
}

/* The MMSE decision feedback equaliser for the estimated channel, with the
 * feedback taken as cancelling every earlier symbol. Forward line x[i] =
 * q(k - i) holds symbol m + l through h[a - i - l], a = DFF + cursor; so
 *   R f = p,  R_ij = sum_{l >= 0} h[a-i-l] h[a-j-l] + s2 delta_ij,  p_i = h[a-i]
 *   b_j = sum_i f_i h[a - i + 1 + j]
 * solved by Cholesky in fixed point: R scaled to (2^26, 2^28], its factor
 * G in Q14, z in Q30, f in Q32 (see below). s2 is the estimator's residual,
 * but not less than 2^-18 of the largest diagonal (it bounds the condition).
 * About 18000 multiplies (32 by 32 bits while the factor fits, as it does)
 * and 50 divisions (the rest by reciprocals). */
#define NT ((V90ARX_NF * (V90ARX_NF + 1)) / 2)
#define TRI(i, j) ((i) * ((i) + 1) / 2 + (j))

/* The band edge (v90arx_edge). Our A/D samples the line on its own clock:
 * what the far codec leaves above 4 kHz (the image of the band below it)
 * folds on to 3.8 .. 4 kHz with a phase that turns once per sample of clock
 * drift, so the channel there is not what the estimate says for long (on a
 * 0 km line at 100 ppm its response at 4 kHz goes from -20 to -70 dB and
 * back every 1.25 s, at 3.9 kHz from -17 to -22 dB; measured). An MMSE
 * equaliser computed from the estimate amplifies that band as if it stood
 * still, and the moment it turns, every symbol carries the error (bursts of
 * decision errors, then the decision directed estimate and the timing loop
 * follow them away). So the image is taken into the design as interference
 * of unknown phase: at four frequencies of 3805 .. 3992 Hz, a quarter of the
 * power the channel estimate has there (the image is weaker than the band
 * it mirrors: G.712's codec filters fall from 14 dB at 4 kHz to 32 dB at
 * 4.6 kHz), each over its 46.875 Hz, added to R as
 *   R_ij += sum_k w P_k cos(2 pi f_k (i - j)),  w = 2 46.875 / 8000 / 4,
 * the same on each diagonal of R: v[i - j]. hh and R in the units of the
 * design (dsn_start). */
static void edge_image(const int32_t *hh, int64_t *v)
{
    int k, j, d;
    int64_t P[4];
    for (k = 0; k < 4; k++) {
        int64_t xr = 0, xi = 0;
        int n = 490 + 6 * k;                    /* f = n 8000 / 1024: 3828 .. 3969 Hz */
        for (j = 0; j < V90ARX_NCH; j++) {
            int ph = (n * j) & SIN_MASK;
            xr += (int64_t)hh[j] * sintab[(ph + SIN_N / 4) & SIN_MASK];
            xi -= (int64_t)hh[j] * sintab[ph];
        }
        xr /= 32768; xi /= 32768;               /* |X| <= 2^27 */
        P[k] = (xr * xr + xi * xi) * 3 / 1024;  /* w = 3 / 1024 */
    }
    /* the same on every entry of diagonal d of R (i - j = d) */
    for (d = 0; d < V90ARX_NF; d++) {
        v[d] = 0;
        for (k = 0; k < 4; k++) v[d] += (P[k] / 32768) * sintab[((490 + 6 * k) * d + SIN_N / 4) & SIN_MASK];
    }
}

#define sdiv_rcp divrcp_sdiv

static int fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* sum of a[k] b[k], k < n, four a pass */
static int64_t dot32(const int32_t *a, const int32_t *b, int n)
{
    int64_t q = 0;
    int k = 0;
    for (; k + 4 <= n; k += 4) {
        q += (int64_t)a[k] * b[k]; q += (int64_t)a[k + 1] * b[k + 1];
        q += (int64_t)a[k + 2] * b[k + 2]; q += (int64_t)a[k + 3] * b[k + 3];
    }
    for (; k < n; k++) q += (int64_t)a[k] * b[k];
    return q;
}

/* The design runs as a job of a few steps, one a symbol: all of it in one
 * tick was 0.9 M ARM instructions at first and still 0.4 M made faster,
 * against the 1.2 M a 10 ms block of the box's CPU has for everything. The
 * estimate is taken when the job starts; the taps it computes replace the
 * equaliser's when it ends, some 8 symbols later (LMS goes on meanwhile).
 *
 *   start   the estimate taken (hh); R's first column, its diagonal, the
 *           band edge, the load s2, the scale; R built along its diagonals
 *           (R(i+1, j+1) = R(i, j) - h[a-i] h[a-j]), scaled, into G (32
 *           bits: the largest diagonal is under 2^28, and |R_ij| under it):
 *           the first 14 diagonals
 *   fill    the other 34
 *   chol    Cholesky in place, columns by the step's budget: G in Q14
 *           (|G_ij| <= sqrt(2^28 R_ii) <= 2^28)
 *   solve   z, f, the bias, the forward taps
 *   fb      the feedback taps; both sets go in
 *
 * A value that does not fit 32 bits (none can, by the bounds above) ends
 * the job as a failed design. */
#define DSN_IDLE  0
#define DSN_FILL  1
#define DSN_CHOL  2
#define DSN_SOLVE 3
#define DSN_FB    4
#define DSN_SPLIT 14                            /* diagonals 0 .. 13: 581 of R's 1176 values */
#define DSN_BUDGET 9000                         /* Cholesky: multiplies (and 12 a division) per step */

static int32_t hh_at(const v90arx_dsn_t *q, int u) { return u >= 0 && u < V90ARX_NCH ? q->hh[u] : 0; }

/* R along its diagonals d0 .. d1 - 1, from R(d, 0) (c0), plus the band
 * edge and, on the main one, the load (ev), scaled; hp: hh with 8 zeros
 * before it. 0, or -1 when a value does not fit 32 bits. */
static int dsn_fill(v90arx_dsn_t *q, const int32_t *hp, int d0, int d1)
{
    int a = q->a, rs = q->rs, d, j;
    for (d = d0; d < d1; d++) {
        int64_t v = q->c0[d], add = q->ev[d];
        const int32_t *u0 = hp + 8 + a - d, *u1 = hp + 8 + a;  /* h[a - (j - 1 + d)] = u0[1 - j], h[a - (j - 1)] = u1[1 - j] */
        for (j = 0; j + d < V90ARX_NF; j++) {
            int64_t e;
            if (j > 0) v -= (int64_t)u0[1 - j] * u1[1 - j];
            e = v + add;
            e = rs > 0 ? (e + ((int64_t)1 << (rs - 1))) >> rs : e * ((int64_t)1 << -rs);
            if (!fits32(e)) return -1;
            q->G[TRI(j + d, j)] = (int32_t)e;
        }
    }
    return 0;
}

/* the job's first step: 0, or -1 (no design) */
static int dsn_start(v90arx_t *r)
{
    v90arx_dsn_t *q = &r->dsn;
    int64_t c0[V90ARX_NF], dg[V90ARX_NF], ev[V90ARX_NF], mx = 0, dmax = 0, s2;
    int a = V90ARX_DFF + r->cpk, i, j, d, hs = 0, rs;
    const int32_t *hh = q->hh;
    int32_t hp[8 + V90ARX_NCH];                  /* hh with zeros before: hp[8 + u] = hh_at(u), u >= -8 */

    q->step = DSN_IDLE;
    for (j = 0; j < V90ARX_NCH; j++) { int64_t v = r->h[j] < 0 ? -(int64_t)r->h[j] : r->h[j]; if (v > mx) mx = v; }
    while ((mx >> hs) >= (1 << 20)) hs++;
    /* rounded, not truncated: the channel has a null at DC, and the many
     * small taps of its tail, each a half step low, add up to a false DC
     * response the solution is very sensitive to (seen at 6 km) */
    for (j = 0; j < V90ARX_NCH; j++) q->hh[j] = hs ? (int32_t)(((int64_t)r->h[j] + (1 << (hs - 1))) >> hs) : r->h[j];
    for (j = 0; j < 8; j++) hp[j] = 0;
    for (j = 0; j < V90ARX_NCH; j++) hp[8 + j] = hh[j];
    /* find_cursor keeps the cursor in 8 .. NCH - 40: a - (NF - 1) >= -8 */
    if (a - (V90ARX_NF - 1) < -8 || a >= V90ARX_NCH) return -1;
    /* first column */
    for (i = 0; i < V90ARX_NF; i++) {
        int64_t acc = 0;
        int u;
        for (u = 0; u <= a - i && u + i < V90ARX_NCH; u++) acc += (int64_t)hh[u] * hh[u + i];
        c0[i] = acc;
    }
    /* the diagonal: R(i, i) = R(0, 0) - sum_{k < i} h[a-k]^2 */
    dg[0] = c0[0];
    for (i = 1; i < V90ARX_NF; i++) { int32_t x = hp[8 + a - (i - 1)]; dg[i] = dg[i - 1] - (int64_t)x * x; }
    /* the residual, in hh^2: h is Q16, hh = h >> hs (up to 2^20, so that
     * the small taps keep their precision: R up to 2^47) */
    s2 = r->res;
    s2 = 16 - hs >= 0 ? s2 << (2 * (16 - hs)) : s2 >> (2 * (hs - 16));
    for (i = 0; i < V90ARX_NF; i++) if (dg[i] > dmax) dmax = dg[i];
    if (r->edge) edge_image(hh, ev); else for (d = 0; d < V90ARX_NF; d++) ev[d] = 0;
    if (s2 < (dmax >> 18)) s2 = dmax >> 18;
    if (s2 < 1) s2 = 1;
    dmax += s2;
    /* scale: the largest diagonal to (2^26, 2^28], by an even shift */
    rs = 0;
    while ((dmax >> rs) > ((int64_t)1 << 28)) rs += 2;
    while (rs > -28 && (rs >= 0 ? dmax >> rs : dmax << -rs) <= ((int64_t)1 << 26)) rs -= 2;
    for (d = 0; d < V90ARX_NF; d++) { q->c0[d] = c0[d]; q->ev[d] = ev[d] + (d == 0 ? s2 : 0); }
    q->a = a; q->hs = hs; q->rs = rs;
    /* the first DSN_SPLIT diagonals now (about half of R), the rest in the next step */
    if (dsn_fill(q, hp, 0, DSN_SPLIT) != 0) return -1;
    q->step = DSN_FILL;
    return 0;
}

/* the next step: 0 more to come, 1 done (the taps are in), -1 failed */
static int dsn_step(v90arx_t *r)
{
    v90arx_dsn_t *q = &r->dsn;
    int i, j, k, a = q->a;
    if (q->step == DSN_FILL) {
        int32_t hp[8 + V90ARX_NCH];
        for (j = 0; j < 8; j++) hp[j] = 0;
        for (j = 0; j < V90ARX_NCH; j++) hp[8 + j] = q->hh[j];
        if (dsn_fill(q, hp, DSN_SPLIT, V90ARX_NF) != 0) { q->step = DSN_IDLE; return -1; }
        q->col = 0;
        q->step = DSN_CHOL;
        return 0;
    }
    if (q->step == DSN_CHOL) {
        int32_t *G = q->G;
        int32_t budget = DSN_BUDGET;
        for (j = q->col; j < V90ARX_NF && budget > 0; j++) {
            int64_t d = (int64_t)G[TRI(j, j)] * ((int64_t)1 << 28) - dot32(&G[TRI(j, 0)], &G[TRI(j, 0)], j), g;
            if (d <= 0) { q->step = DSN_IDLE; return -1; }
            g = isqrt_s64(d);                          /* d < 2^57: g < 2^29 */
            G[TRI(j, j)] = (int32_t)g;
            q->rc[j] = divrcp_rcp((uint64_t)g);
            for (i = j + 1; i < V90ARX_NF; i++) {
                int64_t t = (int64_t)G[TRI(i, j)] * ((int64_t)1 << 28) - dot32(&G[TRI(i, 0)], &G[TRI(j, 0)], j);
                t = sdiv_rcp(t, (uint64_t)g, q->rc[j]);
                if (!fits32(t)) { q->step = DSN_IDLE; return -1; }
                G[TRI(i, j)] = (int32_t)t;
            }
            budget -= (V90ARX_NF - j) * (j + 12);
        }
        q->col = j;
        if (j == V90ARX_NF) q->step = DSN_SOLVE;
        return 0;
    }
    if (q->step == DSN_SOLVE) {
        const int32_t *G = q->G;
        int rs = q->rs, e1;
        int64_t bias = 0;
        /* G z = p, G' f = z; p in hh >> rs/2, with 8 more fraction bits (the
         * window's oldest positions see nothing of the current symbol but the
         * precursor taps, which are tiny, and their p, divided by the load
         * alone, would be all rounding). z'z = f'p, the MMSE gain, is at
         * most 1, so z in Q30 cannot overflow. |f| <= |z| / sqrt(least
         * eigenvalue) <= 2^-4 (the load is at least 2^8), f in Q32 */
        for (i = 0; i < V90ARX_NF; i++) {
            int64_t p = hh_at(q, a - i), t;
            p = rs > 16 ? (p + (1 << (rs / 2 - 9))) >> (rs / 2 - 8) : p * ((int64_t)1 << (8 - rs / 2));
            t = p * ((int64_t)1 << 36) - dot32(&G[TRI(i, 0)], q->z, i);
            t = sdiv_rcp(t, (uint64_t)G[TRI(i, i)], q->rc[i]);
            if (!fits32(t)) { q->step = DSN_IDLE; return -1; }
            q->z[i] = (int32_t)t;
        }
        for (i = V90ARX_NF - 1; i >= 0; i--) {
            int64_t t = (int64_t)q->z[i] * 65536;
            for (k = i + 1; k < V90ARX_NF; k++) t -= (int64_t)G[TRI(k, i)] * q->fs[k];
            t = sdiv_rcp(t, (uint64_t)G[TRI(i, i)], q->rc[i]);
            if (!fits32(t)) { q->step = DSN_IDLE; return -1; }
            q->fs[i] = (int32_t)t;
        }
        /* f p, the gain the MMSE solution leaves on the symbol (just under 1),
         * Q32: divided out, so the output is unbiased */
        for (i = 0; i < V90ARX_NF; i++) {
            int64_t p = hh_at(q, a - i);
            p = rs > 16 ? (p + (1 << (rs / 2 - 9))) >> (rs / 2 - 8) : p * ((int64_t)1 << (8 - rs / 2));
            bias += ((int64_t)q->fs[i] * p) >> 8;
        }
        bias >>= 12;                                         /* Q20 */
        if (bias <= 0) { q->step = DSN_IDLE; return -1; }
        q->bias = bias;
        q->rb = divrcp_rcp((uint64_t)bias);
        /* f_true = fs 2^e1, e1 = 16 - hs - rs/2: taps Q20 for an output of L in Q YF */
        e1 = 16 - q->hs - rs / 2;
        for (i = 0; i < V90ARX_NF; i++) {
            int64_t v = sdiv_rcp((int64_t)q->fs[i] * r->L, (uint64_t)bias, q->rb);     /* fs L / bias, Q12 */
            int sh = e1 + 20 + V90ARX_YF - 12;
            v = sh >= 0 ? v * ((int64_t)1 << sh) : v >> -sh;
            if (v > 0x7fffffff) v = 0x7fffffff;
            if (v < -0x7fffffff) v = -0x7fffffff;
            q->fn[i] = (int32_t)v;
        }
        q->step = DSN_FB;
        return 0;
    }
    if (q->step == DSN_FB) {
        int rs = q->rs;
        /* b_j = sum f_true h_true / bias = sum fs hh 2^(-rs/2) / bias, Q16 */
        for (j = 0; j < V90ARX_NB; j++) {
            int64_t acc = 0;
            int c = a + 1 + j, lo = c - (V90ARX_NCH - 1), hi = c;   /* hh[c - i] for i in lo .. hi */
            if (lo < 0) lo = 0;
            if (hi > V90ARX_NF - 1) hi = V90ARX_NF - 1;
            for (i = lo; i <= hi; i++) acc += (int64_t)q->fs[i] * q->hh[c - i];
            acc = rs > 0 ? (acc + (1 << (rs / 2 - 1))) >> (rs / 2) : acc * ((int64_t)1 << (-rs / 2));
            acc = sdiv_rcp(acc * 16, (uint64_t)q->bias, q->rb);   /* Q32 / Q20 -> Q16 */
            r->b[j] = (int32_t)(acc > 0x7fffffff ? 0x7fffffff : acc < -0x7fffffff ? -0x7fffffff : acc);
        }
        memcpy(r->f, q->fn, sizeof r->f);
        r->eqnext_m = -1;               /* new taps: the next sums from them */
        q->step = DSN_IDLE;
        return 1;
    }
    return -1;
}

/* ---- the slicer ------------------------------------------------------------ */

static int32_t slice(const v90arx_cons_t *c, int slot, int32_t y, int *ucode, int *pos)
{
    const int32_t *lv = c->lv[slot];
    int n = c->n[slot], lo = 0, hi = n - 1;
    int32_t a = y < 0 ? -y : y;
    *pos = y >= 0;
    if (n == 0) { *ucode = 0; return 0; }
    while (lo < hi) {                       /* the largest level <= a, or the first */
        int mid = (lo + hi + 1) >> 1;
        if (lv[mid] <= a) lo = mid; else hi = mid - 1;
    }
    if (lo + 1 < n && lv[lo + 1] - a < a - lv[lo]) lo++;
    *ucode = c->u[slot][lo];
    return *pos ? lv[lo] : -lv[lo];
}

/* ---- timing ------------------------------------------------------------------ */

/* How far the channel estimate has moved later since the equaliser was
 * first computed. A second order loop per block of TBLK symbols: the phase
 * correction is spread over the next block (slew), the frequency
 * integrates. The estimate follows the channel within about 1000 symbols
 * (NCH / mu); the loop is some four times slower. */
static void timing(v90arx_t *r)
{
    int64_t d = later_q16(r->h, r->href, V90ARX_NCH, 0);
    r->delta = (int32_t)d;
    r->step = clamp64(r->step + d * 65536 / (4096 * TBLK), ONE32 - STEP_MAX, ONE32 + STEP_MAX);
    r->slew = clamp64(d * 65536 / (32 * TBLK), -SLEW_MAX, SLEW_MAX);
}

/* ---- the equaliser ---------------------------------------------------------- */

static void equalise(v90arx_t *r, int32_t k, int16_t q)
{
    const int16_t *x, *dl;
    int64_t acc = 0, acc2 = 0;
    int32_t m, y, lev, e, ec, es;
    int i, uc = 0, pos = 1, known = 0, slot, g, gate;
    v90arx_dec_t *o;

    if (--r->xpos < 0) r->xpos = V90ARX_NF - 1;
    r->x[r->xpos] = r->x[r->xpos + V90ARX_NF] = q;
    r->pq += (((int64_t)q * q << 8) - r->pq) >> 8;
    m = k - V90ARX_DFF - (r->cpk - V90ARX_LEAD);
    if (m < 0) return;
    x = &r->x[r->xpos]; dl = &r->d[r->dpos];

    /* the sums: from the last symbol's LMS step when it was the one before
     * (eqnext_m) with the newest terms added, both lines having moved on
     * by one since (a hold, a gate, a design's taps leave none: the sums
     * are formed here, four taps a pass; NF and NB are multiples of 4) */
    if (m == r->eqnext_m) {
        acc = r->fnext + (int64_t)r->f[0] * x[0];
        acc2 = r->bnext + (int64_t)r->b[0] * dl[0];
    } else {
        for (i = 0; i < V90ARX_NF; i += 4) {
            acc += (int64_t)r->f[i] * x[i]; acc += (int64_t)r->f[i + 1] * x[i + 1];
            acc += (int64_t)r->f[i + 2] * x[i + 2]; acc += (int64_t)r->f[i + 3] * x[i + 3];
        }
        for (i = 0; i < V90ARX_NB; i += 4) {
            acc2 += (int64_t)r->b[i] * dl[i]; acc2 += (int64_t)r->b[i + 1] * dl[i + 1];
            acc2 += (int64_t)r->b[i + 2] * dl[i + 2]; acc2 += (int64_t)r->b[i + 3] * dl[i + 3];
        }
    }
    r->eqnext_m = -1;
    y = asr64(acc, 20) - asr64(acc2, 16 - V90ARX_YF);

    if (r->have_next && m >= r->next_m) { r->cons = r->next; r->have_next = 0; }
    slot = (int)(m % 6);
    if (m < r->cfg.ntrain && m < r->mref) {
        lev = (int32_t)r->sb[m & 255] * (1 << V90ARX_YF);
        uc = r->cfg.uinfo; pos = lev >= 0; known = 1;
    } else if (r->cfg.known_lv && r->cfg.known_lv(r->cfg.known_ctx, m, &uc, &pos, &lev)) {
        known = 1;
    } else if (r->cfg.known && r->cfg.known(r->cfg.known_ctx, m, &uc, &pos)) {
        lev = v90_ucode_linear(r->cfg.alaw, (unsigned)uc) << V90ARX_YF;
        if (!pos) lev = -lev;
        known = 1;
    } else {
        lev = slice(&r->cons, slot, y, &uc, &pos);
    }
    if (r->state == V90ARX_TRAIN && m >= r->cfg.ntrain) r->state = V90ARX_DD;
    e = y - lev;

    /* Symbols far off their point (more than 4 sigma of what has been
     * accepted) teach nothing but the impairment: a robbed bit or a pad
     * moving a point by a step, or a wrong decision. They are left out of
     * the adaptation; the gate opens slowly if they persist. */
    {
        int64_t e2 = (int64_t)e * e;
        gate = !known && e2 > 16 * r->gmse;
        if (gate) r->gmse += r->gmse >> 10;
        else r->gmse += (e2 - r->gmse) >> 8;
    }
    /* A slot whose error stands 9 dB above the best slot's is impaired as
     * a whole (robbed bits): its symbols are not adapted on either. */
    if (!known && r->state == V90ARX_DD) {
        uint64_t mn = r->mse_slot[0];
        int s;
        for (s = 1; s < 6; s++) if (r->mse_slot[s] < mn) mn = r->mse_slot[s];
        if (r->mse_slot[slot] > 8 * mn) gate = 1;
    }
    if (!r->hold && !gate) {
        ec = e > ECLIP ? ECLIP : e < -ECLIP ? -ECLIP : e;
        /* mu 1/8 for the first second, then 1/32 */
        g = r->neq < FASTN ? 3 : 5;
        es = (int32_t)(((int64_t)ec * r->kf) >> 16);
        /* each tap's step also forms the next symbol's sum, the new tap
         * times the sample of the tap before (p; none before tap 0): the
         * line moves on by one, and its newest sample is added then. The
         * terms are at most 2^31 2^15 = 2^46, at most 128 of them: the
         * int64 sum is exact in any order, the same as the loops' above */
        {
            int sh = r->shf + g;
            int32_t half = (int32_t)1 << (sh - 1), p = 0;
            int64_t an = 0;
            /* 32 bits, as always; with a wild error (es far out) the
             * product overflows and wraps, as it always did, now defined:
             * unsigned arithmetic, two's complement back */
            /* (a lost receiver's taps run to the end of the range: the
             * step wraps there as well, defined; the retrain follows) */
#define FUPD(ii) do { int32_t v_ = (int32_t)((uint32_t)r->f[ii] - (uint32_t)((int32_t)((uint32_t)es * (uint32_t)x[ii] + (uint32_t)half) >> sh)); \
        r->f[ii] = v_; an += (int64_t)v_ * p; p = x[ii]; } while (0)
            for (i = 0; i < V90ARX_NF; i += 4) { FUPD(i); FUPD(i + 1); FUPD(i + 2); FUPD(i + 3); }
#undef FUPD
            r->fnext = an;
        }
        es = (int32_t)(((int64_t)ec * r->kb) >> 16);
        {
            int sh = r->shb + g;
            int32_t half = (int32_t)1 << (sh - 1), p = 0;
            int64_t an = 0;
#define BUPD(ii) do { int32_t v_ = (int32_t)((uint32_t)r->b[ii] + (uint32_t)((int32_t)((uint32_t)es * (uint32_t)dl[ii] + (uint32_t)half) >> sh)); \
        r->b[ii] = v_; an += (int64_t)v_ * p; p = dl[ii]; } while (0)
            for (i = 0; i < V90ARX_NB; i += 4) { BUPD(i); BUPD(i + 1); BUPD(i + 2); BUPD(i + 3); }
#undef BUPD
            r->bnext = an;
        }
        r->eqnext_m = m + 1;
    }
    {
        int16_t dv = (int16_t)(lev >> V90ARX_YF);
        int64_t e2 = (int64_t)e * e;           /* Q YF squared = Q8 */
        int64_t ms = (int64_t)r->mse, mss = (int64_t)r->mse_slot[slot];
        if (--r->dpos < 0) r->dpos = V90ARX_NB - 1;
        r->d[r->dpos] = r->d[r->dpos + V90ARX_NB] = dv;
        r->pd += (((int64_t)dv * dv << 8) - r->pd) >> 8;
        r->mse = (uint64_t)(ms + ((e2 - ms) >> 8));
        r->mse_slot[slot] = (uint64_t)(mss + ((e2 - mss) >> 6));
    }
    r->neq++;
    if (r->neq == 0) r->neq = 0x80000000u;      /* (past FASTN again; the same modulo rc and 64) */
    if ((r->neq & 63) == 0) renorm(r);

    /* the channel estimate, one symbol behind (every symbol of its span is
     * known or decided), the timing loop, and the equaliser again from time
     * to time */
    /* the symbol as known or decided, held or not: the estimate's
     * regressor (when it was kept only while adapting, the estimate went on
     * after a hold with up to NCH stale symbols in its span, and its error
     * moved it and the timing loop with it: docs/history/PROGRESS.md, robust) */
    r->sb[m & 255] = (int16_t)(lev >> V90ARX_YF);
    if (!r->hold) {
        /* what aliases at the band edge turns with our sampling phase, one
         * turn per sample of drift: compute the equaliser more often the
         * faster the clocks slip (every 0.26, 0.13, 0.06 s up to 30 ppm,
         * 61 ppm, above) */
        int64_t off = r->step - ONE32;
        int rc = V90ARX_RECALC;                     /* a power of two */
        if (off < 0) off = -off;
        if (off > (ONE32 >> 15)) rc >>= 1;          /* 30.5 ppm */
        if (off > (ONE32 >> 14)) rc >>= 1;          /* 61 ppm */
        if (!gate) estimate(r, m - V90ARX_LEAD, r->state == V90ARX_TRAIN ? (int64_t)r->L * r->L : r->pd >> 8, 2);
        else r->hnext_m0 = -1;                      /* (no step: no sum for the next) */
        if (++r->tblk >= TBLK) { r->tblk = 0; if (!r->thold) timing(r); else r->slew = 0; }
        /* a design under way takes its next step, one a symbol (a failed
         * one leaves the taps as they are) */
        if (r->dsn.step != DSN_IDLE) { (void)dsn_step(r); r->dsn_ran = 1; }
        else if (r->state == V90ARX_DD && ((r->neq & (uint32_t)(rc - 1)) == 0 || r->redo)) { r->redo = 0; (void)dsn_start(r); r->dsn_ran = 1; }
    } else r->hnext_m0 = -1;

    if (r->nout >= 2) return;
    o = &r->out[r->nout++];
    o->m = m; o->slot = (uint8_t)slot; o->ucode = (uint8_t)uc; o->sign = (uint8_t)pos;
    o->known = (uint8_t)known; o->y = y; o->e = e;
}

/* the equaliser starts: its lines filled from what is known so far */
static void eq_start(v90arx_t *r, int32_t k)
{
    int i;
    int32_t m = k - V90ARX_DFF - (r->cpk - V90ARX_LEAD);
    for (i = 0; i < V90ARX_NF; i++) r->x[i] = r->x[i + V90ARX_NF] = r->qb[(k - i) & 255];
    r->xpos = 0;
    for (i = 0; i < V90ARX_NB; i++) {
        int32_t mm = m - i;
        int16_t v = mm >= 0 ? r->sb[mm & 255] : 0;
        r->d[i] = r->d[i + V90ARX_NB] = v;
    }
    r->dpos = 0;
    /* the estimate's m0 goes back DFF + cursor symbols from EST's last */
    r->eqnext_m = -1; r->hnext_m0 = -1;
}

/* An interval where UINFO arrives at another level than in the others: a
 * robbed bit behind a pad (UINFO 90 is even, so a robbed bit alone leaves it
 * be, but a 6 dB pad makes it codeword 75, which loses its LSB). The
 * estimate, fitted against +-L in every interval, then carried that
 * interval's error into every tap: TRN1d's error 42 dB (mse), the timing loop
 * following the estimate 0.03 sample away during the DIL and back during
 * TRN2d, and a V.92 transmitter, on that clock, moved under the digital
 * modem's measurements (docs/history/PROGRESS.md, robust 2b). So the estimate's residual
 * is projected on each interval's part of the prediction over TRN1d's
 * symbols LS_FROM .. LS_TO, and each interval's level set from it (within
 * 25 % of L; one closer than 1 % to L stays L); the reference from there,
 * and the symbols in the estimate's span, take it, and the estimate has
 * EST0 - LS_TO steps to settle on it. */
static void ls_set(v90arx_t *r)
{
    int s;
    int32_t m;
    for (s = 0; s < 6; s++) {
        int64_t d;
        if (r->ls_den[s] <= 0) continue;
        d = r->ls_num[s] * 4096 / r->ls_den[s];             /* delta, Q12 */
        if (d > 1024) d = 1024;
        if (d < -1024) d = -1024;
        if (d > -41 && d < 41) continue;
        r->ls[s] = (int16_t)(r->L + (r->L * d) / 4096);
    }
    for (m = r->mref - 256 > 0 ? r->mref - 256 : 0; m < r->mref; m++) {
        int16_t v = r->sb[m & 255];
        r->sb[m & 255] = (int16_t)(v >= 0 ? r->ls[m % 6] : -r->ls[m % 6]);
    }
    for (s = 0; s < 6; s++) r->cons.lv[s][0] = (int32_t)r->ls[s] << V90ARX_YF;
    r->hnext_m0 = -1;                   /* sb[] changed under the estimate's next sum */
}

/* ---- one network-rate sample ----------------------------------------------- */

static void process(v90arx_t *r, int16_t q)
{
    uint32_t n = r->n++;
    int32_t k;

    if (r->state == V90ARX_WAIT) {
        int st;
        if (r->deaf) return;
        st = v92_p6_tick(&r->sd, q, SD_FLOOR);
        if (st == V92_P6_ON) {
            /* the 1333 Hz line of each period of 6 */
            static const int16_t C6[6] = { 32767, 16384, -16384, -32767, -16384, 16384 };
            static const int16_t S6[6] = { 0, 28378, 28378, 0, -28378, -28378 };
            r->sd_pow += (int64_t)q * q; r->sd_cnt = clk_sat_inc(r->sd_cnt);
            r->sd_br += (q * C6[r->sd_i]) >> 8;
            r->sd_bi -= (q * S6[r->sd_i]) >> 8;
            if (++r->sd_i == 6) {
                /* each period against the one SDLAG before it, from the
                 * SD_SKIP-th (the line's tail takes about 10 ms to settle
                 * into Sd's period after silence), and two periods late
                 * (the last ones before the reversal may hold Sd-bar) */
                int w = V90ARX_SDLAG + 3, p = r->sd_np;
                if (r->sd_np < INT_MAX) r->sd_np++;
                r->sd_x[p % w][0] = r->sd_br; r->sd_x[p % w][1] = r->sd_bi;
                if (p - 2 - V90ARX_SDLAG >= SD_SKIP) {
                    const int32_t *a = r->sd_x[(p - 2) % w], *b = r->sd_x[(p - 2 - V90ARX_SDLAG) % w];
                    r->sd_yr += (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1];
                    r->sd_yi += (int64_t)a[1] * b[0] - (int64_t)a[0] * b[1];
                }
                r->sd_br = r->sd_bi = 0; r->sd_i = 0;
            }
        } else if (st == V92_P6_REV && r->sd_cnt >= 48) {
            int32_t rms = isqrt_s64(r->sd_pow / r->sd_cnt);
            int64_t g;
            r->level = rms;
            if (rms < 1) rms = 1;
            g = ((int64_t)AGC_RMS << 12) / rms;
            r->gain = (int32_t)(g > (1 << 22) ? (1 << 22) : g);
            /* the line turned by -2 pi offset per period: from the tangent
             * (Q32) of SDLAG periods' turn, a step of 1 + offset (input
             * samples per network sample) */
            if (r->sd_np >= SD_SKIP + V90ARX_SDLAG + 10 && r->sd_yr > 0) {
                int64_t t = r->sd_yi, y = r->sd_yr;
                while (t > ((int64_t)1 << 30) || t < -((int64_t)1 << 30)) { t >>= 1; y >>= 1; }
                if (y > 0) {
                    /* tan, Q32; beyond 1 (a turn of 45 degrees, 7800 ppm)
                     * it is no codec clock, and the product would overflow */
                    t = clamp64(t * ONE32 / y, -ONE32, ONE32);
                    /* (relative to the step Sd came through: one sample, or
                     * what v90arx_clock() set; that one, from a longer
                     * measurement, stands unless Sd finds it 5 ppm off) */
                    int64_t corr = ((t * 683565276) >> 32) / V90ARX_SDLAG;  /* 1 / (2 pi), Q32 */
                    if (!r->clk_set || corr > CLK_TRUST || corr < -CLK_TRUST) r->step = r->step - corr;
                    r->step = clamp64(r->step, ONE32 - STEP_MAX, ONE32 + STEP_MAX);
                }
            }
            /* Sd-bar is 48 symbols, then TRN1d */
            r->n0 = n - (uint32_t)V92_P6_REV_LAG + 48;
            v90_scr_init(&r->scr);
            r->mref = 0; r->nest = 0;
            memset(r->h, 0, sizeof r->h);
            { int s; for (s = 0; s < 6; s++) { r->ls[s] = r->L; r->ls_num[s] = r->ls_den[s] = 0; } }
            r->state = V90ARX_EST;
        } else if (st == 0) {
            r->sd_pow = 0; r->sd_cnt = 0;
            r->sd_br = r->sd_bi = 0; r->sd_i = 0; r->sd_np = 0; r->sd_yr = r->sd_yi = 0;
        }
        return;
    }
    if (r->state == V90ARX_FAIL) return;

    k = clk_diff(n, r->n0);
    r->qb[k & 255] = q;
    /* TRN1d reference: the scrambler fed with ones, from zero (8.4.5); the
     * estimate needs it up to its end, V90ARX_EST0 steps from sample NCH,
     * whatever is taken as known (TRN1d is at least 2040 T) */
    while (r->mref <= k + V90ARX_LEAD && (r->mref < r->cfg.ntrain || r->state == V90ARX_EST)) {
        r->sb[r->mref & 255] = (int16_t)(v90_scramble_gpc(&r->scr, 1) ? r->ls[r->mref % 6] : -r->ls[r->mref % 6]);
        r->mref++;
    }
    if (r->state == V90ARX_EST) {
        if (k < V90ARX_NCH) return;
        /* mu 1 throughout: TRN1d's signs start far from white (the scrambler
         * leaves its zero state through long runs: lag-1 correlation 0.55
         * over the first 300 symbols, 0.13 by 600), so a smaller step would
         * still be converging when the equaliser is computed */
        estimate(r, k, (int64_t)r->L * r->L, 0);
        if (r->nest == LS_TO) ls_set(r);
        /* the first equaliser: designed from the estimate as it stands after
         * EST0 steps, over the next few symbols (the estimate goes on), and
         * started on the symbol its design ends on */
        if (r->nest == V90ARX_EST0) {
            if (find_cursor(r) != 0 || dsn_start(r) != 0) r->state = V90ARX_FAIL;
            r->dsn_ran = 1;
            return;
        }
        if (r->dsn.step != DSN_IDLE) {
            int i, d = dsn_step(r);
            r->dsn_ran = 1;
            if (d < 0) { r->state = V90ARX_FAIL; return; }
            if (d == 0) return;
            r->pq = 0;
            for (i = 1; i <= 64; i++) { int32_t v = r->qb[(k - i) & 255]; r->pq += ((int64_t)v * v << 8) / 64; }
            r->pd = (int64_t)r->L * r->L << 8;
            renorm(r);
            eq_start(r, k);
            r->neq = 0; r->mse = 0;
            r->gmse = (int64_t)r->L * r->L << 4;        /* open: (L / 4)^2, Q8 */
            memcpy(r->href, r->h, sizeof r->h);
            r->tblk = 0;
            r->state = V90ARX_TRAIN;
        }
        return;
    }
    equalise(r, k, q);
}

/* ---- interpolator ----------------------------------------------------------- */

static void produce(v90arx_t *r)
{
    uint32_t f = (uint32_t)(-r->tnext);         /* behind the evaluable instant */
    int p = (int)(f >> 27);
    int32_t a = (int32_t)((f >> 12) & 0x7fff);  /* between branches p and p + 1, Q15 */
    const int16_t *h0 = v34_interp[p], *h1 = v34_interp[p + 1];
    int64_t acc0, acc1, v;
    /* ring[(rpos - 1 - i) & 63], newest first */
    interp2_64(r->ring, r->rpos - 1, 63, h0, h1, V34_IN, &acc0, &acc1);
    v = acc0 + (((acc1 - acc0) * a) >> 15);     /* Q14 */
    process(r, sat16((v * r->gain) >> 26));
}

void v90arx_clock(v90arx_t *r, int32_t ppm_q8)
{
    /* ppm_q8 * 2^32 / 256000000, as ppm_q8 * 2^21 / 125000: the same quotient
     * (the same fraction, truncated the same way), but GCC 12.3 for m68k
     * miscompiles the shift of a sign-extended argument by 32
     * (docs/history/PROGRESS.md, "m68k") */
    r->step = clamp64(ONE32 + (int64_t)ppm_q8 * 2097152 / 125000, ONE32 - STEP_MAX, ONE32 + STEP_MAX);
    r->clk_set = 1;
}

void v90arx_deaf(v90arx_t *r, int on)
{
    r->deaf = on != 0;
    v92_p6_init(&r->sd);
    r->sd_pow = 0; r->sd_cnt = 0;
    r->sd_br = r->sd_bi = 0; r->sd_i = 0; r->sd_np = 0; r->sd_yr = r->sd_yi = 0;
}

int v90arx_tick(v90arx_t *r, int16_t rx, int16_t tx)
{
    int16_t x = ec_tick(&r->ec, tx, rx, r->ec_mu);
    int i;
    r->dsn_ran = 0;
    r->eout = x;
    r->nout = 0;
    r->ring[r->rpos] = x;
    r->rpos = (r->rpos + 1) & 63;
    r->tnext -= ONE32;
    for (i = 0; r->tnext <= 0 && i < PRODUCE_MAX; i++) {
        produce(r);
        r->tnext += r->step + r->slew;
    }
    return r->nout;
}
