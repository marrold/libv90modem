/* QAM receiver core. See qamrx.h. */
#include "qamrx.h"
#include "clock.h"
#include "sintab.h"
#include "v34_tabs.h"
#include "interp2.h"
#include "fixmath.h"

#define ONE24 (1 << 24)
/* |d|^2 under QRX_DENSE, the mean power 2 QRX_REF^2 = 2^25: a division by
 * this constant is a shift (with C's truncation toward zero), not a 64-bit
 * division */
#define DENSE_DN ((int64_t)1 << 25)
typedef char qrx_dense_dn_check[(int64_t)2 * QRX_REF * QRX_REF == DENSE_DN ? 1 : -1];
/* Against hostile audio (security pass, I2: loud or abrupt audio after S,
 * which real calls' hang-up clicks also give), with the result unchanged
 * wherever the arithmetic fitted before:
 * - Products and sums that could leave their type are formed as unsigned
 *   (defined: modulo 2^N) and converted back, which wraps (DESIGN.md: an
 *   assumed implementation-defined behaviour). Where they fitted, the
 *   value is the same; where they did not (signed overflow before), the
 *   value is what the code computed in practice, so that recordings whose
 *   clicks overflowed the tap update replay as they did.
 * - The squared error, whose shift wrapped to a negative mse, is taken as
 *   the clamp's 4 (6 dB) when it would not fit.
 * - The taps: cgrow adds up the most an update can move a tap (2^31 >>
 *   mu_shift), and before it passes QRX_CHEAD every tap is held to
 *   +-QRX_CMAX and cgrow starts again. A tap then stays within QRX_CMAX +
 *   QRX_CHEAD; k56d's one restore of taps saved under that bound adds at
 *   most one QRX_CHEAD more (1.5 * 2^30 in all; this needs k56d to restore
 *   a save at most once, with the receiver re-synced before each save), so
 *   the taps never wrap
 *   and the output's and the centroid's sums stay in int64. A real line's
 *   taps are under 2^24 (Q20): the hold is a pass over the taps every 256
 *   (mu_shift 10) to 4096 (14) symbols that changes nothing there, not a
 *   clamp on every tap update. */
#define QRX_CMAX ((int32_t)1 << 29)
#define QRX_CHEAD ((int32_t)1 << 29)
/* the tap update takes two taps a pass */
typedef char qrx_ntap_even_check[QRX_NTAP % 2 == 0 ? 1 : -1];
/* the equaliser output takes four taps a pass */
typedef char qrx_ntap_four_check[QRX_NTAP % 4 == 0 ? 1 : -1];

void qamrx_init(qamrx_t *r, int sym_rate, int car_num, int car_den)
{
    int i;
    r->step = (int32_t)(((int64_t)8000 << 24) / (3 * sym_rate));
    /* (uint32_t) before the widening: the same value (only the low 32 bits
     * survive the shift), but GCC 12.3 for m68k miscompiles the sign-extended
     * form (docs/history/PROGRESS.md, "m68k") */
    r->dcar = (uint32_t)(((uint64_t)(uint32_t)car_num << 32) / (uint32_t)car_den);
    for (i = 0; i < 64; i++) r->ring[i] = 0;
    r->rpos = 0; r->pw = 0; r->gain = 256; r->running = 0;
    r->tnext = 0; r->t_integ = 0; r->m = 0;
    for (i = 0; i < 2 * QRX_NTAP; i++) r->x[i] = 0;
    r->xpos = 0;
    for (i = 0; i < QRX_NTAP; i++) r->cr[i] = r->ci[i] = 0;
    r->yr = r->yi = r->ymr = r->ymi = r->er = r->ei = 0;
    r->dnorm = 1; r->timing_on = 0;
    r->theta = 0; r->freq = 0;
    r->zr = r->zi = 0; r->nsym = 0; r->mse = 65536; r->mse_acc = (int64_t)65536 << 8;
    r->cgrow = 0;
}

void qamrx_sync(qamrx_t *r, int32_t delay_q8)
{
    int i;
    int32_t rms = isqrt_s64(r->pw);
    int64_t t;
    /* AGC: bring the input to an RMS of 2048 */
    if (rms < 8) rms = 8;
    r->gain = (2048 * 256) / rms;
    if (r->gain > 16384) r->gain = 16384;
    if (r->gain < 16) r->gain = 16;
    for (i = 0; i < QRX_NTAP; i++) r->cr[i] = r->ci[i] = 0;
    for (i = 0; i < 2 * QRX_NTAP; i++) r->x[i] = 0;
    r->t_integ = 0; r->theta = 0; r->freq = 0; r->nsym = 0; r->mse = 65536;
    r->mse_acc = (int64_t)65536 << 8;
    r->cgrow = 0;
    r->timing_on = 0; r->er = r->ei = 0;
    /* output grid: tau = t_sym0 + m * step. After the next push the evaluable
     * instant is (that sample - V34_ID); pick the first grid point not before it.
     * tnext counts from the evaluable instant, and is decremented on the push. */
    t = ((int64_t)delay_q8 << 16) + ((int64_t)V34_ID << 24) + ONE24;
    r->m = 0;
    while (t < ONE24) { t += r->step; r->m++; }
    while (t - r->step >= ONE24) { t -= r->step; r->m--; }
    r->tnext = (int32_t)t;
    r->running = 1;
}

int qamrx_centroid(const qamrx_t *r)
{
    int64_t num = 0, den = 0;
    int k;
    for (k = 0; k < QRX_NTAP; k++) {
        int64_t e = ((int64_t)r->cr[k] * r->cr[k] + (int64_t)r->ci[k] * r->ci[k]) >> 16;
        num += e * k; den += e;
    }
    if (!den) return QRX_CENTER * 256;
    /* num * 256 leaves int64 only for taps far beyond a real line's (see
     * QRX_CHEAD); then the same quotient in two steps */
    if (num < ((int64_t)1 << 54)) return (int)((num * 256) / den);
    return (int)(num / den * 256 + (num % den) * 256 / den);
}

static void eq_out(const qamrx_t *r, int32_t *yr, int32_t *yi)
{
    const int16_t *x = &r->x[r->xpos];
    int64_t ar = 0, ai = 0;
    int k;
    /* four taps a pass (QRX_NTAP is a multiple of 4), in order */
    for (k = 0; k < QRX_NTAP; k += 4) {
        ar += (int64_t)r->cr[k] * x[k];         ai += (int64_t)r->ci[k] * x[k];
        ar += (int64_t)r->cr[k + 1] * x[k + 1]; ai += (int64_t)r->ci[k + 1] * x[k + 1];
        ar += (int64_t)r->cr[k + 2] * x[k + 2]; ai += (int64_t)r->ci[k + 2] * x[k + 2];
        ar += (int64_t)r->cr[k + 3] * x[k + 3]; ai += (int64_t)r->ci[k + 3] * x[k + 3];
    }
    *yr = (int32_t)(ar >> 20); *yi = (int32_t)(ai >> 20);
}

/* every tap to +-QRX_CMAX (see QRX_CHEAD) */
static void hold_taps(qamrx_t *r)
{
    int k;
    for (k = 0; k < QRX_NTAP; k++) {
        if (r->cr[k] > QRX_CMAX) r->cr[k] = QRX_CMAX;
        if (r->cr[k] < -QRX_CMAX) r->cr[k] = -QRX_CMAX;
        if (r->ci[k] > QRX_CMAX) r->ci[k] = QRX_CMAX;
        if (r->ci[k] < -QRX_CMAX) r->ci[k] = -QRX_CMAX;
    }
    r->cgrow = 0;
}

/* a - b, a + b and a * b on int32 and int64 modulo 2^N (see QRX_CMAX) */
static int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static int64_t add64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
static int64_t sub64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
static int64_t mul64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }

/* one T/3 sample out of the interpolator and through the equaliser; returns 1
 * when it was the on-time sample of a symbol */
static int produce(qamrx_t *r)
{
    int got = 0;
    {
        int32_t f = -r->tnext;                      /* 0 .. ONE24-1 behind the evaluable instant */
        int p = (f >> 19) & (V34_IP - 1);
        int32_t a = (f >> 4) & 0x7fff;              /* position between branches p and p+1, Q15 */
        const int16_t *h0 = v34_interp[p], *h1 = v34_interp[p + 1];
        int32_t acc0, acc1, v, ph;
        /* 32 branches alone leave a timing granularity of 1/32 sample, which
         * costs several dB at the band edge; interpolate between neighbours.
         * The newest sample is ring[(rpos - 1) & 63]; int32 sums cannot
         * overflow (test/test_interp_bound.c) */
        interp2_32(r->ring, r->rpos - 1, 63, h0, h1, V34_IN, &acc0, &acc1);
        acc0 >>= 14; acc1 >>= 14;
        v = (acc0 + (((acc1 - acc0) * a) >> 15)) * r->gain >> 8;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        /* push into the equaliser line (newest first, doubled buffer) */
        if (--r->xpos < 0) r->xpos = QRX_NTAP - 1;
        r->x[r->xpos] = r->x[r->xpos + QRX_NTAP] = (int16_t)v;

        ph = (r->m - QRX_CENTER) % 3;
        if (ph < 0) ph += 3;
        if (r->m >= QRX_CENTER - 1) {
            if (ph == 2) {
                eq_out(r, &r->ymr, &r->ymi);        /* a third of a symbol early */
            } else if (ph == 0) {
                int32_t c, s;
                eq_out(r, &r->yr, &r->yi);
                s = sintab[(r->theta >> 22) & SIN_MASK];
                c = sintab[((r->theta >> 22) + SIN_N / 4) & SIN_MASK];
                r->zr = (int32_t)(((int64_t)r->yr * c + (int64_t)r->yi * s) >> 15);
                r->zi = (int32_t)(((int64_t)r->yi * c - (int64_t)r->yr * s) >> 15);
                /* on past 2^31 - 1 from 0 (defined: unsigned), so that
                 * the low bits keep cycling for qamrx_update's nudge; the
                 * training readers (nsym - 1) are long done by then */
                r->nsym = (int32_t)(((uint32_t)r->nsym + 1u) & 0x7fffffffu);
                got = 1;
            } else if (r->timing_on) {
                /* a third late: timing gradient Re{e conj(z+ - z-)} / |d|^2, with
                 * z+ and z- the outputs a third of a symbol late and early, each
                 * derotated by the carrier phase of its own instant (theta has
                 * already moved on one symbol since the on-time sample) */
                int32_t ypr, ypi, zpr, zpi, zmr, zmi, c, s;
                uint32_t third = r->dcar / 3, th;
                int64_t g;
                int32_t gn;
                eq_out(r, &ypr, &ypi);
                th = r->theta - 2 * third;
                s = sintab[(th >> 22) & SIN_MASK]; c = sintab[((th >> 22) + SIN_N / 4) & SIN_MASK];
                zpr = (int32_t)(((int64_t)ypr * c + (int64_t)ypi * s) >> 15);
                zpi = (int32_t)(((int64_t)ypi * c - (int64_t)ypr * s) >> 15);
                th = r->theta - 4 * third;
                s = sintab[(th >> 22) & SIN_MASK]; c = sintab[((th >> 22) + SIN_N / 4) & SIN_MASK];
                zmr = (int32_t)(((int64_t)r->ymr * c + (int64_t)r->ymi * s) >> 15);
                zmi = (int32_t)(((int64_t)r->ymi * c - (int64_t)r->ymr * s) >> 15);
                g = add64(mul64(r->er, sub32(zpr, zmr)), mul64(r->ei, sub32(zpi, zmi)));
                g = mul64(g, 4096);
                gn = (int32_t)(r->dnorm == DENSE_DN ? g / DENSE_DN  /* Q12 */
                                                    : g / r->dnorm);
                if (gn > 4096) gn = 4096;
                if (gn < -4096) gn = -4096;
                /* second-order loop, time constant about 130 symbols */
                if (r->timing_fast) {
                    r->tnext += gn * 256 + r->t_integ;
                } else {
                    r->t_integ += gn / 4;
                    if (r->t_integ > ONE24 / 256) r->t_integ = ONE24 / 256;
                    if (r->t_integ < -ONE24 / 256) r->t_integ = -ONE24 / 256;
                    r->tnext += gn * 32 + r->t_integ;
                }
                r->timing_on = 0;
            }
        }
        r->m++;
        if (r->m >= CLK_REBASE_AT) r->m -= CLK_REBASE;     /* (read modulo 3 only, and past QRX_CENTER) */
        r->tnext += r->step;
    }
    return got;
}

int qamrx_tick(qamrx_t *r, int16_t xin)
{
    /* Two T/3 instants can fall within one input sample. When the first of
     * them completed a symbol, the second was left for now, so that the caller's
     * qamrx_update() saw the equaliser line as it was for that symbol. */
    if (r->running) while (r->tnext <= 0) (void)produce(r);

    r->ring[r->rpos] = xin;
    r->rpos = (r->rpos + 1) & 63;
    r->pw += (((int64_t)xin * xin) - r->pw) / 256;
    if (!r->running) return 0;

    r->tnext -= ONE24;
    while (r->tnext <= 0) {
        if (produce(r)) return 1;
    }
    return 0;
}

void qamrx_update(qamrx_t *r, int32_t dr, int32_t di, int flags, int mu_shift)
{
    int32_t er = sub32(dr, r->zr), ei = sub32(di, r->zi), c, s, pr, pi;
    /* |e|^2 << 16 is formed when it fits (|e|^2 under 2^47); past that
     * (signed overflow before, and a negative mse), the clamp below at
     * once: 4 << 16, which the quotient is anyway for any |d|^2 up to 2^45 */
    uint64_t e2u = (uint64_t)((int64_t)er * er) + (uint64_t)((int64_t)ei * ei);
    int64_t dn, e2 = e2u < ((uint64_t)1 << 47) ? (int64_t)e2u << 16 : -1;
    int k;
    if (flags & QRX_DENSE) {
        dn = DENSE_DN;
        if (e2 >= 0) e2 /= DENSE_DN;
    } else {
        uint64_t d2 = (uint64_t)((int64_t)dr * dr) + (uint64_t)((int64_t)di * di);
        dn = d2 > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)d2;
        if (dn < 1) dn = 1;
        if (e2 >= 0) e2 /= dn;
    }
    if (e2 > (4 << 16) || e2 < 0) e2 = 4 << 16;
    r->mse_acc += ((e2 << 8) - r->mse_acc) / 64;
    r->mse = (int32_t)(r->mse_acc >> 8);

    /* rotate the error back to the pass-band */
    s = sintab[(r->theta >> 22) & SIN_MASK];
    c = sintab[((r->theta >> 22) + SIN_N / 4) & SIN_MASK];
    pr = (int32_t)(((int64_t)er * c - (int64_t)ei * s) >> 15);
    pi = (int32_t)(((int64_t)ei * c + (int64_t)er * s) >> 15);
    r->er = er; r->ei = ei;             /* base-band error, for the timing loop */
    r->dnorm = (int32_t)(dn > 0x7fffffff ? 0x7fffffff : dn);
    r->timing_on = (flags & QRX_TIMING) != 0;
    r->timing_fast = (flags & QRX_TFAST) != 0;

    if (flags & QRX_EQ) {
        const int16_t *x = &r->x[r->xpos];
        /* rounded, not truncated: a shift alone rounds every update down, and
         * half an LSB per symbol on every tap is a drift of 0.14 in 90 s */
        const int32_t half = (int32_t)1 << (mu_shift - 1);
        /* an update moves a tap by at most 2^31 >> mu_shift */
        const int32_t most = (int32_t)(((uint32_t)1 << 31) >> mu_shift);
        if (r->cgrow > QRX_CHEAD - most) hold_taps(r);
        r->cgrow += most;
        for (k = 0; k < QRX_NTAP; k += 2) {
            r->cr[k] = add32(r->cr[k], add32(mul32(pr, x[k]), half) >> mu_shift);
            r->ci[k] = add32(r->ci[k], add32(mul32(pi, x[k]), half) >> mu_shift);
            r->cr[k + 1] = add32(r->cr[k + 1], add32(mul32(pr, x[k + 1]), half) >> mu_shift);
            r->ci[k + 1] = add32(r->ci[k + 1], add32(mul32(pi, x[k + 1]), half) >> mu_shift);
        }
    }
    if (flags & QRX_CARRIER) {
        /* phase error Im{z conj(d)} / |d|^2 in rad, Q16 */
        int64_t num = mul64(sub64((int64_t)r->zi * dr, (int64_t)r->zr * di), 65536);
        int32_t phi = (int32_t)((flags & QRX_DENSE) ? num / DENSE_DN : num / dn);
        if (phi > 32768) phi = 32768;
        if (phi < -32768) phi = -32768;
        r->freq = add32(r->freq, phi * 16);
        r->theta += (uint32_t)(phi * 1043);
    }
    r->theta += r->dcar + (uint32_t)r->freq;

    /* The equaliser can absorb any timing phase, so the timing loop alone does
     * not pin the taps in place. Every 1024 symbols nudge the sampling instant
     * towards keeping the taps' centre of gravity in the middle; the equaliser
     * follows. One tap of offset moves the clock by 1/100 sample. */
    if ((flags & QRX_TIMING) && (r->nsym & 1023) == 0) {
        int32_t c = qamrx_centroid(r) - QRX_CENTER * 256;
        if (c > 512) c = 512;
        if (c < -512) c = -512;
        r->tnext -= c * 655;
    }
}
