/* V.92 digital modem: design of the CPd parameters. See v92_design.h. */
#include <string.h>
#include "v92_design.h"
#include "v92_mod.h"
#include "v90_pcm.h"
#include "fixmath.h"

uint32_t v92_dsn_qvar(int alaw, int32_t a)
{
    int u = 1, hi = 127;
    int32_t st;
    if (a < 0) a = -a;
    while (u < hi) {                        /* the smallest Ucode not below a */
        int mid = (u + hi) / 2;
        if (v90_ucode_linear(alaw, (unsigned)mid) < a) u = mid + 1; else hi = mid;
    }
    st = v90_ucode_linear(alaw, (unsigned)u) - v90_ucode_linear(alaw, (unsigned)u - 1);
    return (uint32_t)(st * st / 12);
}

enum { ST_LOAD, ST_CONV, ST_CORR, ST_UPD, ST_OUT, ST_G, ST_SCALE, ST_POINTS, ST_TRIAL, ST_MC, ST_VERIFY, ST_DONE, ST_FAIL };

void v92_dsn_start(v92_dsn_t *d, const v92_dsn_cfg *cfg)
{
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    if (d->cfg.kappa_q4 <= 0) d->cfg.kappa_q4 = V92_DSN_KAPPA;
    d->st = ST_LOAD;
    d->rng = 0x1234567u;
}

static int mc_bit(void *user)
{
    v92_dsn_t *d = (v92_dsn_t *)user;
    d->rng = d->rng * 1664525u + 1013904223u;
    return (int)(d->rng >> 31);
}

/* Moduli and K for a base modulus of lc; every candidate point is in the
 * constellation. Returns K (0 if not even 24000 bit/s fits). */
static int set_rate(v92_dsn_t *d, int lc)
{
    v92_map_t *m = &d->cpd.map;
    v92_mod_t chk;
    int i, drn, again;
    for (i = 0; i < V92_DFRAME; i++) m->M[i] = (uint8_t)((i & 3) == 3 ? lc / 2 : lc);
    if (lc < 2) return 0;
    for (drn = V92_UP_NUM_RATES; drn >= 1; drn--) {
        if (!(d->cfg.rates & (1u << (drn - 1)))) continue;
        if (d->cfg.max_rate && V92_UP_BPS(drn) > d->cfg.max_rate) continue;
        if (v92_mod_init(&chk, m->M, V92_UP_K(drn)) == 0) break;
    }
    if (drn < 1) return 0;
    m->K = (uint8_t)V92_UP_K(drn);
    d->cpd.drn = (uint8_t)drn;
    /* lower the moduli in turn while 2^K <= M0 .. M11 (6.4.1) */
    do {
        again = 0;
        for (i = 0; i < V92_DFRAME; i++) {
            if (m->M[i] < 2) continue;
            m->M[i]--;
            if (v92_mod_init(&chk, m->M, m->K) == 0) again = 1;
            else m->M[i]++;
        }
    } while (again);
    m->lc[0] = (uint8_t)d->ncand;
    return m->K;
}

/* num / den * 2^q for 0 <= num, 0 < den, without losing the top bits */
static int64_t ratio(int64_t num, int64_t den, int q)
{
    int sh = 0;
    if (den <= 0 || num <= 0) return 0;
    while (sh < q && num < ((int64_t)1 << 61)) { num *= 2; sh++; }
    den >>= q - sh;
    if (den == 0) den = 1;
    return num / den;
}

/* v fits the int32_t it is stored in. On an ill-conditioned channel (a long
 * low-pass tail, which the caller's own training decides) A'A b can already
 * leave 32 bits, and the conjugate gradient iteration can go astray and
 * grow its vectors without bound: such a channel is refused
 * (V92_DSN_ERR_CHANNEL) rather than let them overflow. */
static int fits32(int64_t v) { return v >= -(int64_t)2147483647 - 1 && v <= 2147483647; }
#define DIVERGED(d) do { (d)->st = ST_FAIL; return V92_DSN_ERR_CHANNEL; } while (0)

/* sum a b, Q24 * Q24 -> Q40 */
static int64_t dot(const int32_t *a, const int32_t *b, int n)
{
    int64_t acc = 0;
    int j;
    for (j = 0; j < n; j++) acc += ((int64_t)a[j] * b[j]) >> 8;
    return acc;
}

/* What the taps of g outside the cursor and the feedback make of the x(n)
 * so far (x: linear * 2^V92_XQ; n: its number). Linear. */
static int64_t isi_sample(v92_dsn_t *d, int32_t x, int n)
{
    int D = -d->s0, ng = V92_CH_NTAP + d->lz2 - 1, j;
    int64_t acc = 0;
    d->xh[n & 255] = x;
    for (j = 0; j < ng; j++) {
        if (j == D && (j += d->lp1 + 1) >= ng) break;    /* the feedback taps reach the end of g */
        acc += (int64_t)d->g[j] * d->xh[(n - j) & 255];
    }
    return acc / ((int64_t)d->g0 << V92_XQ);
}

int v92_dsn_step(v92_dsn_t *d)
{
    const v92_dsn_cfg *c = &d->cfg;
    v92_filt_t *f = &d->cpd.filt;
    int j, n;

    d->steps++;
    switch (d->st) {
    case ST_LOAD: {
        /* the channel with its peak scaled to 1 (Q14); b = A' e(D), the
         * start of the conjugate gradient iteration with w = 0 */
        int32_t pk = 0, a;
        int64_t lam;
        d->pk = 0;
        for (j = 0; j < V92_CH_NTAP; j++) {
            a = c->h[j] < 0 ? -c->h[j] : c->h[j];
            if (a > pk) { pk = a; d->pk = j; }
        }
        if (pk < 4096) { d->st = ST_FAIL; return V92_DSN_ERR_CHANNEL; }
        d->hpk = pk;
        for (j = 0; j < V92_CH_NTAP; j++) d->hn[j] = (int32_t)((int64_t)c->h[j] * 16384 / pk);
        d->lz2 = c->lmax && c->lmax < V92_DSN_LZ2 ? c->lmax : V92_DSN_LZ2;
        d->lp1 = V92_DSN_LP1;
        if (c->ltot && d->lz2 + d->lp1 > c->ltot) d->lp1 = c->ltot - d->lz2;
        if (d->lp1 < 2) { d->st = ST_FAIL; return V92_DSN_ERR_CHANNEL; }
        d->skip = d->lz2 > V92_DSN_Z2_SKIP ? V92_DSN_Z2_SKIP : 0;
        {
            /* the cursor: late in z2, which needs its taps before the cursor
             * (those after it are the precoder's work), as far as the LP1
             * samples behind it still lie inside h * z2 */
            int co = d->skip ? d->lz2 - V92_DSN_POST : d->lz2 / 2;
            int room = V92_CH_NTAP + d->lz2 - 2 - d->lp1 - d->pk;
            if (co > room) co = room;
            if (co < d->lz2 / 2) co = d->lz2 / 2;
            d->s0 = -(d->pk + co);
        }
        /* lambda, Q30, in units of the squared peak: twice the noise with
         * the analogue modem silent, within 2^-15 .. 2^-6 */
        lam = (((int64_t)2 * (c->noise > 1048576 ? 1048576 : c->noise)) << 38) / (((int64_t)pk * pk) >> 8);
        if (lam < 32768) lam = 32768;
        if (lam > 16777216) lam = 16777216;
        d->lambda = (uint32_t)lam;
        for (j = 0; j < d->lz2; j++) {
            int i = -d->s0 - j;
            d->cw[j] = 0;
            d->cr[j] = d->cp[j] = (i >= 0 && i < V92_CH_NTAP) ? d->hn[i] * 1024 : 0;
        }
        /* the coefficient held at zero: the iteration runs in the subspace
         * without it (residual and directions have no component there) */
        if (d->skip) d->cr[d->skip] = d->cp[d->skip] = 0;
        d->rr = dot(d->cr, d->cr, d->lz2);
        d->iter = 0;
        d->st = ST_CONV; d->i = 0;
        return 0;
    }
    case ST_CONV: {
        /* t = A p, without the rows the precoder feedback takes care of */
        int D = -d->s0, ng = V92_CH_NTAP + d->lz2 - 1;
        for (n = 0; n < 16 && d->i < ng; n++, d->i++) {
            int64_t acc = 0;
            int j0 = d->i - d->lz2 + 1, j1 = d->i;
            if (d->i > D && d->i <= D + d->lp1) { d->g[d->i] = 0; n--; continue; }
            if (j0 < 0) j0 = 0;
            if (j1 > V92_CH_NTAP - 1) j1 = V92_CH_NTAP - 1;
            for (j = j0; j <= j1; j++) acc += (int64_t)d->hn[j] * d->cp[d->i - j];
            if (!fits32(acc / 16384)) DIVERGED(d);
            d->g[d->i] = (int32_t)(acc / 16384);
        }
        if (d->i >= ng) { d->st = ST_CORR; d->i = 0; }
        return 0;
    }
    case ST_CORR:
        /* q = A' t + lambda p */
        for (n = 0; n < 16 && d->i < d->lz2; n++, d->i++) {
            int64_t acc = 0, q;
            for (j = 0; j < V92_CH_NTAP; j++) acc += (int64_t)d->hn[j] * d->g[d->i + j];
            q = acc / 16384 + (((int64_t)d->lambda * d->cp[d->i]) >> 30);
            if (!fits32(acc / 16384) || !fits32(q)) DIVERGED(d);
            d->cq[d->i] = (int32_t)q;
        }
        if (d->i >= d->lz2) { if (d->skip) d->cq[d->skip] = 0; d->st = ST_UPD; }
        return 0;

    case ST_UPD: {
        int64_t pq, al, be, rn;
        if (d->resid) {
            /* q = M w: the residual afresh, r = b - q, and a new start
             * from it. The recurrence for r drifts in this arithmetic. */
            for (j = 0; j < d->lz2; j++) {
                int i = -d->s0 - j;
                int64_t r = ((i >= 0 && i < V92_CH_NTAP) ? d->hn[i] * 1024 : 0) - (int64_t)d->cq[j];
                if (!fits32(r)) DIVERGED(d);
                d->cr[j] = (int32_t)r;
                d->cp[j] = d->cr[j];
            }
            if (d->skip) d->cr[d->skip] = d->cp[d->skip] = 0;
            d->rr = dot(d->cr, d->cr, d->lz2);
            d->resid = 0;
            d->st = (d->rr < 64 || d->iter >= V92_DSN_ITER) ? ST_OUT : ST_CONV; d->i = 0;
            return 0;
        }
        pq = dot(d->cp, d->cq, d->lz2);
        al = ratio(d->rr, pq, 18);
        if (al > ((int64_t)1 << 34)) al = (int64_t)1 << 34;
        {
            /* al <= 2^34: al p overflows int64 only past INT64_MAX / al */
            int64_t lim = al > 0 ? INT64_MAX / al : INT64_MAX;
            for (j = 0; j < d->lz2; j++) {
                int64_t dw, dr;
                if (d->cp[j] > lim || d->cp[j] < -lim || d->cq[j] > lim || d->cq[j] < -lim) DIVERGED(d);
                dw = (al * d->cp[j]) >> 18;
                dr = (al * d->cq[j]) >> 18;
                if (!fits32(dw) || !fits32(dr) || !fits32(d->cw[j] + dw) || !fits32(d->cr[j] - dr)) DIVERGED(d);
                d->cw[j] = (int32_t)(d->cw[j] + dw);
                d->cr[j] = (int32_t)(d->cr[j] - dr);
            }
        }
        rn = dot(d->cr, d->cr, d->lz2);
        be = ratio(rn, d->rr, 18);
        if (be > ((int64_t)1 << 22)) be = (int64_t)1 << 22;
        for (j = 0; j < d->lz2; j++) {
            int64_t dp = (be * d->cp[j]) >> 18;         /* be <= 2^22: fits int64 */
            if (!fits32(dp) || !fits32(d->cr[j] + dp)) DIVERGED(d);
            d->cp[j] = (int32_t)(d->cr[j] + dp);
        }
        d->rr = rn;
        d->iter++;
        if (rn < 64 || pq <= 0) d->st = ST_OUT;
        else {
            if (d->iter % V92_DSN_RESTART == 0) { memcpy(d->cp, d->cw, sizeof d->cp); d->resid = 1; }
            else if (d->iter >= V92_DSN_ITER) { d->st = ST_OUT; d->i = 0; return 0; }
            d->st = ST_CONV;
        }
        d->i = 0;
        return 0;
    }
    case ST_OUT:
        /* w for the channel as given (Q8 per LU), Q30 */
        for (j = 0; j < d->lz2; j++) {
            int64_t w = (int64_t)d->cw[j] * 16384 / d->hpk;
            if (!fits32(w) || w == -(int64_t)2147483647 - 1) DIVERGED(d);    /* |w| is taken below */
            d->w[j] = (int32_t)w;
        }
        d->st = ST_G; d->i = 0;
        return 0;

    case ST_G:
        /* g = h * z2, Q8 * Q30 -> Q14 */
        for (n = 0; n < 4 && d->i < V92_CH_NTAP + d->lz2 - 1; n++, d->i++) {
            int64_t acc = 0;
            int j0 = d->i - d->lz2 + 1, j1 = d->i;
            if (j0 < 0) j0 = 0;
            if (j1 > V92_CH_NTAP - 1) j1 = V92_CH_NTAP - 1;
            for (j = j0; j <= j1; j++) acc += (int64_t)c->h[j] * d->w[d->i - j];
            d->g[d->i] = (int32_t)(acc / (1 << 24));
        }
        if (d->i >= V92_CH_NTAP + d->lz2 - 1) d->st = ST_SCALE;
        return 0;

    case ST_SCALE: {
        int D = -d->s0, ng = V92_CH_NTAP + d->lz2 - 1, k;
        int32_t wmax = 0;
        int64_t isi = 0, wn2 = 0, g4;
        d->g0 = d->g[D];
        if (d->g0 < 4096) { d->st = ST_FAIL; return V92_DSN_ERR_CHANNEL; }
        for (j = 0; j < ng; j++)
            if (j < D || j > D + d->lp1) isi += (int64_t)d->g[j] * d->g[j];
        {
            /* isi 2^16 / den without forming isi 2^16 (it need not fit:
             * |g| < 2^28 as |w| <= |cw| 2^14 / hpk); past 2^32, saturated */
            int64_t den = ((int64_t)d->g0 * d->g0) >> 12, qi = isi / den;
            d->isi_q28 = qi >= 65536 ? UINT32_MAX : (uint32_t)(qi * 65536 + (isi % den) * 65536 / den);
        }
        for (j = 0; j < d->lz2; j++) { int32_t a = d->w[j] < 0 ? -d->w[j] : d->w[j]; if (a > wmax) wmax = a; }
        /* G z2 = w / g0; the largest z2 at about 0.92 */
        g4 = (int64_t)wmax * 4 * 1118 / ((int64_t)1024 * d->g0) + 1;
        if (g4 > 65535) { d->st = ST_FAIL; return V92_DSN_ERR_CHANNEL; }
        f->lz1 = 0; f->lp1 = (uint16_t)d->lp1; f->lz2 = (uint16_t)d->lz2; f->lp2 = 0;
        f->gain4 = (uint16_t)g4;
        for (k = 1; k <= d->lp1; k++) {
            int64_t p = -(int64_t)d->g[D + k] * 16384 / d->g0;
            f->coef[k - 1] = (int16_t)(p > 32767 ? 32767 : p < -32767 ? -32767 : p);
        }
        for (j = 0; j < d->lz2; j++) {
            int64_t den = (int64_t)d->g0 * g4, num = (int64_t)d->w[j] * 131072;
            int64_t z = (num + (num < 0 ? -den / 2 : den / 2)) / den;
            f->coef[d->lp1 + j] = (int16_t)(z > 32767 ? 32767 : z < -32767 ? -32767 : z);
            wn2 += z * z;
        }
        /* mean square of G v = Px sum (G z2)^2 = 1 at Px = 2^66 / (g4^2 sum z2^2) */
        d->xrms_max = (uint32_t)(((uint64_t)1 << 33) / ((uint64_t)g4 * isqrt_u64((uint64_t)wn2)));
        d->dly = D - V92_CH_LEAD;
        d->st = ST_POINTS;
        return 0;
    }
    case ST_POINTS: {
        /* candidate points, smallest first */
        uint32_t n0, isi, kap = (uint32_t)c->kappa_q4;
        int32_t a = 0;
        uint16_t *pt = d->cpd.map.pt[0];
        n0 = c->noise > c->q_quiet ? c->noise - c->q_quiet : 0;
        if (n0 < 1) n0 = 1;
        d->rho_q8 = (c->res > n0 && c->q_trn) ? (uint32_t)(((uint64_t)(c->res - n0) << 8) / c->q_trn) : 256;
        if (d->rho_q8 < 256) d->rho_q8 = 256;
        if (d->rho_q8 > 4096) d->rho_q8 = 4096;
        /* An allowance for what is not measured. The error of the channel
         * estimate: an NLMS estimate with step 1/32 is off by res / 64 at
         * the training level, and the data signal may run at that power.
         * And the echo that the frozen
         * canceller leaves under a data signal: 34 dB below the echo, a
         * figure from the simulated calls, where the decoder's error at
         * the smallest points was larger than noise floor, quantiser and
         * estimate explain. */
        {
#ifndef V92_DSN_ECHO_DIV_LIN
#define V92_DSN_ECHO_DIV_LIN 96
#endif
            /* in 64 bits and saturated: an echo estimate past about 3.1 M
             * (canceller taps at their limit, security pass M1) would wrap
             * e * e, and the noise read as small gave too dense a
             * constellation */
            uint64_t e = c->echo_rms / (c->rx_linear ? V92_DSN_ECHO_DIV_LIN : 48);
            uint64_t s = (uint64_t)n0 + e * e + c->res / 64;
            n0 = s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
        }
        /* first with x(n) taken as white, then with what the first trial
         * measured: x(n) is not white, and the residual of g is strongest
         * where x(n) is */
        isi = d->pass ? d->isi_var : (uint32_t)(((uint64_t)d->isi_q28 * d->xrms_max * d->xrms_max) >> 28);
        for (n = 0; n < V92_MAX_LC; n++) {
            int32_t dd = 0, pass;
            for (pass = 0; pass < 2; pass++) {
                uint64_t q = c->rx_linear ? 0 : v92_dsn_qvar(c->alaw, a + dd + (int32_t)(c->echo_rms + c->echo_rms / 2));
                uint64_t var = n0 + (q * d->rho_q8 >> 8) + isi;
                dd = (int32_t)(isqrt_u64(var * kap * kap) >> 4) + 1;
            }
            a += n == 0 ? (dd + 1) / 2 : dd;
            if (a > 32000) break;
            pt[n] = (uint16_t)a;
        }
        d->ncand = n;
        d->cpd.has_mod = d->cpd.has_filt = d->cpd.has_const = 1;
        d->cpd.map.trellis = V92_TRELLIS_16;
        /* the smallest modulus that carries the lowest rate allowed */
        if (set_rate(d, d->ncand) == 0) { d->st = ST_FAIL; return V92_DSN_ERR_RATE; }
        {
            int lo = 1, hi = d->ncand;      /* lo: too small, hi: enough */
            while (hi - lo > 1) {
                int mid = (lo + hi) / 2;
                if (set_rate(d, mid) == 0) lo = mid; else hi = mid;
            }
            d->base = lo;
        }
        d->lo = d->base; d->hi = d->ncand;  /* lo: known to fit (base: nothing yet), hi: largest not yet ruled out */
        d->lc = d->hi;
        d->st = ST_TRIAL;
        return 0;
    }
    case ST_TRIAL:
        if (set_rate(d, d->lc) == 0) { d->st = ST_FAIL; return V92_DSN_ERR_RATE; }
        if (v92_enc_init(&d->enc, &d->cpd.map, f, mc_bit, d) != 0) { d->st = ST_FAIL; return V92_DSN_ERR_CHANNEL; }
        d->pacc = 0; d->iacc = 0; d->xacc = 0; d->imax = 0; d->i = 0; d->usum = 0;
        memset(d->xh, 0, sizeof d->xh);
        d->st = d->lo == d->hi ? ST_VERIFY : ST_MC;
        return 0;

    case ST_MC:
        for (n = 0; n < 4; n++, d->i++) {
            v92_sym_t sy;
            int32_t o = v92_enc_symbol(&d->enc, &sy);
            d->pacc += (int64_t)o * o >> 14;
            if (o > 8 * 16384 || o < -8 * 16384) d->pacc += (int64_t)1 << 50;    /* the precoder has run away */
            if (!d->pass) {
                int64_t acc = isi_sample(d, sy.x, d->i), xl = sy.x / (1 << V92_XQ);
                if (d->i >= 256) { d->iacc += acc * acc; d->xacc += xl * xl; }
            }
        }
        if (d->i >= V92_DSN_MC) {
            d->power_q14 = (uint32_t)(d->pacc / d->i);
            if (!d->pass) {
                /* the constellation again, with the interference as it is:
                 * its ratio to the power of x(n), times the power of x(n)
                 * plus a half, or what the power limit allows if less */
                int64_t x2 = d->xacc / (d->i - 256), lim = (int64_t)d->xrms_max * d->xrms_max;
                x2 += x2 / 2;
                if (x2 > lim) x2 = lim;
                d->isi_var = d->xacc > 0 ? (uint32_t)(ratio(d->iacc, d->xacc, 30) * x2 >> 30) : 0;
                d->pass = 1;
                d->st = ST_POINTS;
                return 0;
            }
            if (d->power_q14 <= 16384) d->lo = d->lc; else d->hi = d->lc - 1;
            if (d->lo == d->hi) {
                /* not even the lowest rate within the power limit */
                if (d->lo == d->base) { d->st = ST_FAIL; return V92_DSN_ERR_RATE; }
                d->lc = d->lo;
            } else {
                d->lc = (d->lo + d->hi + 1) / 2;
            }
            d->st = ST_TRIAL;
        }
        return 0;

    case ST_VERIFY:
        /* The choice is run for longer. The points of an equivalence class
         * are not evenly spaced and end somewhere below the top of the
         * constellation, so x(n) is not bounded the way it is with a true
         * modulo: when the feedback asks for a u(n) beyond the last member
         * of the class, x(n) comes out large and one-sided, and a feedback
         * filter that has the channel's zero at 0 Hz keeps that for the
         * symbols to come. Such excursions (|x| above V92_DSN_XPEAK times
         * its RMS; 1.73 for an even distribution) must not be seen at all:
         * if they are, the modulus is lowered, which gives every class more
         * members. */
        for (n = 0; n < 4; n++, d->i++) {
            v92_sym_t sy;
            int32_t o = v92_enc_symbol(&d->enc, &sy);
            int64_t xl = sy.x / (1 << V92_XQ), is = isi_sample(d, sy.x, d->i);
            d->pacc += (int64_t)o * o >> 14;
            d->usum += (uint32_t)(((int64_t)sy.u * sy.u) >> 14);
            d->xacc += xl * xl;
            if (xl * xl > d->iacc) d->iacc = xl * xl;
            if (d->i >= 256 && is * is > d->imax) d->imax = is * is;
        }
        if (d->i >= V92_DSN_VERIFY || d->iacc > ((int64_t)1 << 40)) {
            int ok = d->i >= V92_DSN_VERIFY;
            d->power_q14 = (uint32_t)(d->pacc / d->i);
            d->xpk_q4 = (uint32_t)(isqrt_u64((uint64_t)ratio(d->iacc, d->xacc / d->i + 1, 16)) >> 4);
            if (ok && d->imax > (int64_t)20 * d->isi_var + 16 && d->pass < 3) {
                /* The interference is not evenly spread in time: a rare
                 * large x(n) under one of the larger taps of g stands out.
                 * The largest sample seen is to be 3.5 deviations. */
                d->isi_var = (uint32_t)(d->imax / 12 + 1);
                d->pass++;
                d->st = ST_POINTS;
                return 0;
            }
            if (ok && d->xpk_q4 <= V92_DSN_XPEAK && d->power_q14 <= 16384 + 16384 / 32) { d->st = ST_DONE; return 1; }
            d->lc -= d->lc / 12 + 1;
            d->lo = d->hi = d->lc;
            d->cut++;
            if (d->lc <= d->base) { d->st = ST_FAIL; return V92_DSN_ERR_RATE; }
            d->st = ST_TRIAL;
        }
        return 0;

    case ST_DONE: return 1;
    default:      return V92_DSN_ERR_CHANNEL;
    }
}
