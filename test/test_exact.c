/* The S detector (modem/v34train.c) and the QAM receiver core (modem/qamrx.c)
 * against their code before audit Task 30, which wrote their divisions by
 * 8, 16 and 2^25 as divisions by constants (shifts) and the equaliser's tap
 * update two taps a pass. Each ref_* function is that code, kept here
 * verbatim (only renamed); the library must agree with it, every field of
 * the state and every return value, on:
 *   - sdet: a stream of noise, S and S-bar (events fire), for div 8 and 16
 *     (the values the modem sets) and 5 and 12 (any other divisor);
 *   - qamrx_update: random states, with and without QRX_DENSE, and states
 *     at the edges of its divisions (num * 65536 and the error power about
 *     0, +-1, +-(2^25 - 1), +-2^25, and the largest the code forms);
 *   - qamrx_tick on the symbol third that runs the timing loop
 *     (g * 4096 / dnorm), dnorm the dense 2^25, near it, 1, 2^31 - 1, or
 *     |d|^2 as a non-dense update leaves it.
 * The states are bounded as the receiver's own are, so that no expression
 * of either version overflows. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "test.h"
#include "../modem/v34train.h"
#include "../modem/qamrx.h"
#include "../modem/clock.h"
#include "../modem/sintab.h"
#include "../modem/v34_tabs.h"

#ifdef TEST_SHORT
#define N_SDET   200000
#define N_UPDATE 100000
#define N_TICK   100000
#else
#define N_SDET   2000000
#define N_UPDATE 1000000
#define N_TICK   1000000
#endif

static uint64_t rs = 0x9e3779b97f4a7c15u;
static uint32_t rnd32(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 32);
}
/* lo .. hi, inclusive */
static int32_t rnd_in(int32_t lo, int32_t hi)
{
    return (int32_t)((int64_t)lo + (int64_t)(rnd32() % (uint32_t)((int64_t)hi - lo + 1)));
}

/* ---------------- reference copy of v34train.c's sdet_tick (04af29d) ---------------- */
static int ref_sdet_tick(sdet_t *d, int16_t x)
{
    int32_t c = sintab[((d->ph >> 22) + SIN_N / 4) & SIN_MASK], s = sintab[(d->ph >> 22) & SIN_MASK];
    int16_t pr = (int16_t)((x * c) >> 15), pi = (int16_t)(-(x * s) >> 15);
    int32_t or_, oi;
    int64_t z2, o2, p, thr;
    int ev = 0, old;

    d->ph += d->dph;
    old = (d->mpos - d->len) & 31;
    d->sr += pr - d->mr[old]; d->si += pi - d->mi[old];
    d->mr[d->mpos] = pr; d->mi[d->mpos] = pi;
    d->mpos = (d->mpos + 1) & 31;

    or_ = d->zr[(d->zpos - d->len) & 31]; oi = d->zi[(d->zpos - d->len) & 31];
    d->zr[d->zpos] = d->sr; d->zi[d->zpos] = d->si;
    d->zpos = (d->zpos + 1) & 31;

    d->pw += ((int64_t)x * x - d->pw) / 64;
    z2 = (int64_t)d->sr * d->sr + (int64_t)d->si * d->si;
    o2 = (int64_t)or_ * or_ + (int64_t)oi * oi;
    p = (int64_t)d->sr * or_ + (int64_t)d->si * oi;
    /* S carries half its power in the carrier line: |z|^2 = len^2 P / 4 */
    thr = (d->pw * d->len * d->len) / d->div;

    if (d->holdoff > 0) d->holdoff--;
    if (d->pw > 400 && z2 > thr) {
        d->present++;
    } else {
        if (d->present >= SDET_PRESENT) d->holdoff = -(d->len + 4);   /* armed */
        d->present = 0;
    }
    if (d->holdoff < 0) {
        d->holdoff++;
        /* reversed, and already half-way to full amplitude the other way */
        if (p < -(o2 / 2)) { ev = 1; d->holdoff = 400; }
    }
    return ev;
}

/* ---------------- reference copy of qamrx.c (04af29d) ---------------- */
#define ONE24 (1 << 24)

static int ref_qamrx_centroid(const qamrx_t *r)
{
    int64_t num = 0, den = 0;
    int k;
    for (k = 0; k < QRX_NTAP; k++) {
        int64_t e = ((int64_t)r->cr[k] * r->cr[k] + (int64_t)r->ci[k] * r->ci[k]) >> 16;
        num += e * k; den += e;
    }
    return den ? (int)((num * 256) / den) : QRX_CENTER * 256;
}

static void ref_eq_out(const qamrx_t *r, int32_t *yr, int32_t *yi)
{
    const int16_t *x = &r->x[r->xpos];
    int64_t ar = 0, ai = 0;
    int k;
    for (k = 0; k < QRX_NTAP; k++) {
        ar += (int64_t)r->cr[k] * x[k];
        ai += (int64_t)r->ci[k] * x[k];
    }
    *yr = (int32_t)(ar >> 20); *yi = (int32_t)(ai >> 20);
}

static int ref_produce(qamrx_t *r)
{
    int got = 0;
    {
        int32_t f = -r->tnext;                      /* 0 .. ONE24-1 behind the evaluable instant */
        int p = (f >> 19) & (V34_IP - 1);
        int32_t a = (f >> 4) & 0x7fff;              /* position between branches p and p+1, Q15 */
        const int16_t *h0 = v34_interp[p], *h1 = v34_interp[p + 1];
        int32_t acc0 = 0, acc1 = 0, v, ph;
        int i, idx = r->rpos - 1;
        /* 32 branches alone leave a timing granularity of 1/32 sample, which
         * costs several dB at the band edge; interpolate between neighbours */
        for (i = 0; i < V34_IN; i++) {
            int32_t s = r->ring[(idx - i) & 63];
            acc0 += s * h0[i];
            acc1 += s * h1[i];
        }
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
                ref_eq_out(r, &r->ymr, &r->ymi);    /* a third of a symbol early */
            } else if (ph == 0) {
                int32_t c, s;
                ref_eq_out(r, &r->yr, &r->yi);
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
                ref_eq_out(r, &ypr, &ypi);
                th = r->theta - 2 * third;
                s = sintab[(th >> 22) & SIN_MASK]; c = sintab[((th >> 22) + SIN_N / 4) & SIN_MASK];
                zpr = (int32_t)(((int64_t)ypr * c + (int64_t)ypi * s) >> 15);
                zpi = (int32_t)(((int64_t)ypi * c - (int64_t)ypr * s) >> 15);
                th = r->theta - 4 * third;
                s = sintab[(th >> 22) & SIN_MASK]; c = sintab[((th >> 22) + SIN_N / 4) & SIN_MASK];
                zmr = (int32_t)(((int64_t)r->ymr * c + (int64_t)r->ymi * s) >> 15);
                zmi = (int32_t)(((int64_t)r->ymi * c - (int64_t)r->ymr * s) >> 15);
                g = (int64_t)r->er * (zpr - zmr) + (int64_t)r->ei * (zpi - zmi);
                gn = (int32_t)(g * 4096 / r->dnorm);       /* Q12 */
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

static int ref_qamrx_tick(qamrx_t *r, int16_t xin)
{
    if (r->running) while (r->tnext <= 0) (void)ref_produce(r);

    r->ring[r->rpos] = xin;
    r->rpos = (r->rpos + 1) & 63;
    r->pw += (((int64_t)xin * xin) - r->pw) / 256;
    if (!r->running) return 0;

    r->tnext -= ONE24;
    while (r->tnext <= 0) {
        if (ref_produce(r)) return 1;
    }
    return 0;
}

static void ref_qamrx_update(qamrx_t *r, int32_t dr, int32_t di, int flags, int mu_shift)
{
    int32_t er = dr - r->zr, ei = di - r->zi, c, s, pr, pi;
    int64_t dn = (int64_t)dr * dr + (int64_t)di * di, e2;
    int k;
    if (flags & QRX_DENSE) dn = (int64_t)2 * QRX_REF * QRX_REF;
    if (dn < 1) dn = 1;
    e2 = (((int64_t)er * er + (int64_t)ei * ei) << 16) / dn;
    if (e2 > (4 << 16)) e2 = 4 << 16;
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
        for (k = 0; k < QRX_NTAP; k++) {
            r->cr[k] += (pr * x[k] + half) >> mu_shift;
            r->ci[k] += (pi * x[k] + half) >> mu_shift;
        }
    }
    if (flags & QRX_CARRIER) {
        /* phase error Im{z conj(d)} / |d|^2 in rad, Q16 */
        int64_t num = (int64_t)r->zi * dr - (int64_t)r->zr * di;
        int32_t phi = (int32_t)(num * 65536 / dn);
        if (phi > 32768) phi = 32768;
        if (phi < -32768) phi = -32768;
        r->freq += phi * 16;
        r->theta += (uint32_t)(phi * 1043);
    }
    r->theta += r->dcar + (uint32_t)r->freq;

    if ((flags & QRX_TIMING) && (r->nsym & 1023) == 0) {
        int32_t c = ref_qamrx_centroid(r) - QRX_CENTER * 256;
        if (c > 512) c = 512;
        if (c < -512) c = -512;
        r->tnext -= c * 655;
    }
}

/* ---------------- state comparison ---------------- */
#define SAME(f) (a->f == b->f)
#define SAME_ARR(f) (memcmp(a->f, b->f, sizeof a->f) == 0)

static int sdet_same(const sdet_t *a, const sdet_t *b)
{
    return SAME(len) && SAME(div) && SAME(ph) && SAME(dph) && SAME_ARR(mr) && SAME_ARR(mi)
        && SAME(mpos) && SAME(sr) && SAME(si) && SAME_ARR(zr) && SAME_ARR(zi) && SAME(zpos)
        && SAME(pw) && SAME(present) && SAME(holdoff);
}

static int qamrx_same(const qamrx_t *a, const qamrx_t *b)
{
    return SAME(step) && SAME(dcar) && SAME_ARR(ring) && SAME(rpos) && SAME(pw) && SAME(gain)
        && SAME(running) && SAME(tnext) && SAME(t_integ) && SAME(m) && SAME_ARR(x) && SAME(xpos)
        && SAME_ARR(cr) && SAME_ARR(ci) && SAME(yr) && SAME(yi) && SAME(ymr) && SAME(ymi)
        && SAME(er) && SAME(ei) && SAME(dnorm) && SAME(timing_on) && SAME(timing_fast)
        && SAME(theta) && SAME(freq) && SAME(zr) && SAME(zi) && SAME(nsym) && SAME(mse)
        && SAME(mse_acc);
}

/* ---------------- sdet: noise, S and S-bar ---------------- */
static void test_sdet(int div, int sym_rate, int car_num, int car_den)
{
    sdet_t a, b;
    uint32_t ph = 0;
    long n, differ = 0, events = 0;
    int seg = 0, kind = 0, amp = 0;
    memset(&a, 0, sizeof a);
    sdet_init(&a, sym_rate, car_num, car_den);
    a.div = div;
    b = a;
    for (n = 0; n < N_SDET; n++) {
        int16_t x;
        int ea, eb;
        if (seg == 0) {
            /* next segment: noise, S (the carrier line), its inversion
             * S-bar right after S, or silence */
            int was = kind;
            kind = was == 1 ? (rnd32() % 4 ? 2 : 0) : (int)(rnd32() % 4);
            if (kind == 2 && was != 1) kind = 1;
            seg = rnd_in(8, 600);
            amp = rnd_in(0, 30000);
        }
        seg--;
        if (kind == 0) {
            x = (int16_t)rnd_in(-amp, amp);
        } else if (kind == 3) {
            x = (int16_t)rnd_in(-3, 3);
        } else {
            int32_t c = sintab[((ph >> 22) + SIN_N / 4) & SIN_MASK];
            int32_t v = (int32_t)(((int64_t)c * amp) >> 15) + rnd_in(-amp / 8, amp / 8);
            if (kind == 2) v = -v;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            x = (int16_t)v;
        }
        ph += a.dph;
        ea = sdet_tick(&a, x);
        eb = ref_sdet_tick(&b, x);
        events += eb;
        if (ea != eb || !sdet_same(&a, &b)) {
            if (differ < 3) printf("  sdet div %d: differs at sample %ld\n", div, n);
            differ++;
            a = b;
        }
    }
    printf("  sdet, div %2d, %d Bd: %d samples, %ld S / S-bar events: %ld differ\n",
           div, sym_rate, N_SDET, events, differ);
    CHECK(differ == 0);
    CHECK(events > 0);
}

/* ---------------- qamrx: random states ---------------- */
/* a receiver state as the core forms them, bounded so that no expression
 * overflows: the equaliser output under 2^24, the point under 2^22 (|e|^2
 * << 16 in 64 bits) */
static void rand_state(qamrx_t *r)
{
    int k;
    memset(r, 0, sizeof *r);
    qamrx_init(r, rnd32() & 1 ? 3200 : 3000, rnd_in(1, 7), rnd_in(8, 10));
    for (k = 0; k < 64; k++) r->ring[k] = (int16_t)rnd_in(-8192, 8192);
    r->rpos = rnd_in(0, 63);
    r->pw = rnd_in(0, 0x7fffffff);
    r->gain = rnd_in(16, 4096);
    for (k = 0; k < 2 * QRX_NTAP; k++) r->x[k] = (int16_t)rnd_in(-8192, 8192);
    r->xpos = rnd_in(0, QRX_NTAP - 1);
    for (k = 0; k < QRX_NTAP; k++) {
        r->cr[k] = rnd_in(-(1 << 22), 1 << 22);
        r->ci[k] = rnd_in(-(1 << 22), 1 << 22);
    }
    r->yr = rnd_in(-(1 << 24), 1 << 24); r->yi = rnd_in(-(1 << 24), 1 << 24);
    r->ymr = rnd_in(-(1 << 24), 1 << 24); r->ymi = rnd_in(-(1 << 24), 1 << 24);
    r->er = rnd_in(-(1 << 20), 1 << 20); r->ei = rnd_in(-(1 << 20), 1 << 20);
    r->dnorm = rnd_in(1, 0x7fffffff);
    r->t_integ = rnd_in(-ONE24 / 256, ONE24 / 256);
    r->theta = rnd32(); r->freq = rnd_in(-(1 << 24), 1 << 24);
    r->zr = rnd_in(-(1 << 22), 1 << 22); r->zi = rnd_in(-(1 << 22), 1 << 22);
    r->nsym = rnd_in(0, 0x7fffffff);
    if (rnd32() % 8 == 0) r->nsym &= ~1023;     /* the centroid nudge */
    r->mse_acc = (int64_t)rnd_in(0, 0x7fffffff) << (rnd32() % 8);
    r->mse = (int32_t)(r->mse_acc >> 8);
}

static int rand_flags(void)
{
    return (int)(rnd32() & 31);
}

static long update_one(const qamrx_t *s, int32_t dr, int32_t di, int flags, int mu)
{
    qamrx_t a = *s, b = *s;
    qamrx_update(&a, dr, di, flags, mu);
    ref_qamrx_update(&b, dr, di, flags, mu);
    return qamrx_same(&a, &b) ? 0 : 1;
}

static void test_update(void)
{
    static const int32_t edge[] = {
        0, 1, -1, 16, -16, 22, -22, 23, -23, 511, -511, 512, -512, 513, -513,
        1 << 22, -(1 << 22), (1 << 23) - 1, -((1 << 23) - 1)
    };
    qamrx_t s;
    long n, differ = 0, dense = 0, nedge = 0;
    size_t i, j;
    for (n = 0; n < N_UPDATE; n++) {
        int flags = rand_flags(), mu = rnd_in(10, 14);
        int32_t dr, di;
        rand_state(&s);
        /* the wanted point: 4-point (training), a dense data point, or
         * anything up to 2^15 */
        switch (rnd32() % 3) {
        case 0: dr = rnd32() & 1 ? QRX_REF : -QRX_REF; di = rnd32() & 1 ? QRX_REF : -QRX_REF; break;
        case 1: dr = rnd_in(-3 * QRX_REF, 3 * QRX_REF); di = rnd_in(-3 * QRX_REF, 3 * QRX_REF); break;
        default: dr = rnd_in(-32768, 32768); di = rnd_in(-32768, 32768); break;
        }
        if (rnd32() % 16 == 0) dr = di = 0;
        /* with the equaliser on, the error as small as the core's own
         * (pr * x[k] in 32 bits) */
        if (flags & QRX_EQ) {
            s.zr = dr + rnd_in(-(1 << 14), 1 << 14);
            s.zi = di + rnd_in(-(1 << 14), 1 << 14);
        }
        dense += (flags & QRX_DENSE) != 0;
        differ += update_one(&s, dr, di, flags, mu);
    }
    /* edges: d = (1, 0), so that num = zi: num * 65536 and |e|^2 << 16
     * about +-2^25 (QRX_DENSE's divisor) and 0, +-1; and the largest the
     * core can form in 64 bits, |z| up to 2^23 against |d| 2^15 (the line
     * empty, so that the equaliser's products stay 0) */
    for (i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        for (j = 0; j < 4; j++) {
            int fl;
            for (fl = 0; fl < 32; fl++) {
                int32_t dr = 1, di = 0;
                rand_state(&s);
                memset(s.x, 0, sizeof s.x);
                s.zi = edge[i];
                s.zr = j == 0 ? 0 : j == 1 ? 1 : j == 2 ? -(edge[i] / 2) : 1 - edge[i] / 64;
                differ += update_one(&s, dr, di, fl, 12);
                dr = edge[i] & 1 ? 32768 : -32768; di = j & 1 ? 32768 : -32768;
                s.zr = j & 2 ? -(1 << 22) : 1 << 22;
                differ += update_one(&s, dr, di, fl, 12);
                nedge += 2;
            }
        }
    }
    printf("  qamrx_update: %d random states (%ld QRX_DENSE), %ld at the edges: %ld differ\n",
           N_UPDATE, dense, nedge, differ);
    CHECK(differ == 0);
}

/* the third of a symbol that runs the timing loop: m one past an on-time
 * instant, the next produce() is due at the coming sample */
static long tick_one(qamrx_t *s, int32_t dnorm)
{
    qamrx_t a, b;
    int16_t x = (int16_t)rnd_in(-8192, 8192);
    int ra, rb;
    s->running = 1;
    s->timing_on = 1;
    s->timing_fast = (int)(rnd32() & 1);
    s->m = QRX_CENTER + 1 + 3 * rnd_in(0, 1000);
    s->tnext = rnd_in(1, ONE24);
    s->dnorm = dnorm;
    a = *s; b = *s;
    ra = qamrx_tick(&a, x);
    rb = ref_qamrx_tick(&b, x);
    return ra == rb && qamrx_same(&a, &b) ? 0 : 1;
}

static void test_tick(void)
{
    static const int32_t dn_edge[] = {
        1, 2, (1 << 25) - 1, 1 << 25, (1 << 25) + 1, 1 << 24, 0x7fffffff
    };
    static const int32_t e_edge[] = {
        0, 1, -1, (1 << 20), -(1 << 20), (1 << 25) - 1, -((1 << 25) - 1), 1 << 25, -(1 << 25)
    };
    qamrx_t s;
    long n, differ = 0, dense = 0, nedge = 0;
    size_t i, j, k;
    for (n = 0; n < N_TICK; n++) {
        int32_t dn;
        rand_state(&s);
        switch (rnd32() % 4) {
        case 0: case 1: dn = 1 << 25; dense++; break;           /* after a dense update */
        case 2: {                                               /* after a non-dense one */
            int64_t dr = rnd_in(-32768, 32768), di = rnd_in(-32768, 32768);
            int64_t d2 = dr * dr + di * di;
            dn = (int32_t)(d2 < 1 ? 1 : d2 > 0x7fffffff ? 0x7fffffff : d2);
            break;
        }
        default: dn = rnd_in(1, 0x7fffffff); break;
        }
        differ += tick_one(&s, dn);
    }
    /* edges: the error e against the equaliser line, the taps small (|z+ -
     * z-| under 2^17), so that g * 4096 stays in 64 bits for |e| 2^25 */
    for (i = 0; i < sizeof dn_edge / sizeof dn_edge[0]; i++)
        for (j = 0; j < sizeof e_edge / sizeof e_edge[0]; j++)
            for (k = 0; k < sizeof e_edge / sizeof e_edge[0]; k++) {
                int t;
                rand_state(&s);
                for (t = 0; t < QRX_NTAP; t++) {
                    s.cr[t] = rnd_in(-(1 << 16), 1 << 16);
                    s.ci[t] = rnd_in(-(1 << 16), 1 << 16);
                }
                s.ymr = rnd_in(-(1 << 16), 1 << 16); s.ymi = rnd_in(-(1 << 16), 1 << 16);
                s.er = e_edge[j]; s.ei = e_edge[k];
                differ += tick_one(&s, dn_edge[i]);
                nedge++;
            }
    printf("  qamrx_tick, timing third: %d random states (%ld with dnorm 2^25), %ld at the edges: %ld differ\n",
           N_TICK, dense, nedge, differ);
    CHECK(differ == 0);
}

int main(void)
{
    printf("test_exact: the S detector and qamrx against their code before Task 30\n");
    test_sdet(8, 3200, 3, 5);
    test_sdet(16, 3200, 3, 5);
    test_sdet(16, 3000, 3, 5);
    test_sdet(8, 3000, 4, 5);
    test_sdet(5, 3200, 3, 5);
    test_sdet(12, 3200, 7, 8);
    test_update();
    test_tick();
    T_REPORT("test_exact");
}
