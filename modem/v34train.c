/* V.34 Phase 3 training references and S detector. See v34train.h. */
#include "v34train.h"
#include "qamrx.h"
#include "sintab.h"

/* QRX_REF * sqrt(2) * cos(n * 30 deg), rounded: not v34_pp_point()'s Q14
 * cosines scaled (8192 * 5793 / 16384 is 2896.5 at 60 deg, here 2896) */
static const int16_t c12[12] = { 5793, 5017, 2896, 0, -2896, -5017, -5793, -5017, -2896, 0, 2896, 5017 };

void v34_pp(int i, int32_t *re, int32_t *im)
{
    unsigned a = v34_pp_phase((unsigned)i);
    *re = c12[a];
    *im = c12[(a + 9) % 12];                /* sin(x) = cos(x - 90 deg) */
}

void v34_rot4(int q, int32_t *re, int32_t *im)
{
    static const int8_t xr[4] = { 1, 1, -1, -1 }, xi[4] = { 1, -1, -1, 1 };
    *re = xr[q & 3] * QRX_REF; *im = xi[q & 3] * QRX_REF;
}

/* v34.h's S and S-bar, from its Q7 units (point 0 = (V34_ONE, V34_ONE)) to
 * QRX_REF ones: exact */
typedef char v34train_qrx_scale[(QRX_REF % V34_ONE == 0) ? 1 : -1];
static void from_q7(v34_pt_t p, int32_t *re, int32_t *im)
{
    *re = (int32_t)p.re * (QRX_REF / V34_ONE);
    *im = (int32_t)p.im * (QRX_REF / V34_ONE);
}

void v34_s(int n, int len, int32_t *re, int32_t *im)
{
    from_q7(v34_s_point((unsigned)n, (unsigned)len), re, im);
}

void v34_sbar(int n, int32_t *re, int32_t *im)
{
    from_q7(v34_sbar_point((unsigned)n), re, im);
}

void v34trn_init(v34trn_t *t, int poly) { v34scr_init(&t->scr, poly); }

void v34trn_next(v34trn_t *t, int32_t *re, int32_t *im)
{
    int i1 = v34scr_put(&t->scr, 1), i2 = v34scr_put(&t->scr, 1);
    v34_rot4(2 * i2 + i1, re, im);
}

/* ---------------- S / S-bar detector ---------------- */

void sdet_init(sdet_t *d, int sym_rate, int car_num, int car_den)
{
    int i;
    /* the side lines sit at +-sym_rate/2 from the carrier: average over a
     * whole number of their periods (20 samples: nulls every 400 Hz) */
    d->len = (sym_rate == 3000) ? 16 : 20;
    d->div = 8;
    d->ph = 0;
    d->dph = (uint32_t)((((uint64_t)sym_rate * (uint32_t)car_num) << 32) / ((uint64_t)8000 * (uint32_t)car_den));
    for (i = 0; i < 32; i++) d->mr[i] = d->mi[i] = 0, d->zr[i] = d->zi[i] = 0;
    d->mpos = d->zpos = 0;
    d->sr = d->si = 0; d->pw = 0; d->present = 0; d->holdoff = 0;
}

int sdet_tick(sdet_t *d, int16_t x)
{
    int32_t c = sintab[((d->ph >> 22) + SIN_N / 4) & SIN_MASK], s = sintab[(d->ph >> 22) & SIN_MASK];
    int16_t pr = (int16_t)((x * c) >> 15), pi = (int16_t)(-(x * s) >> 15);
    int32_t or_, oi;
    int ev = 0, old, on = 0;

    d->ph += d->dph;
    old = (d->mpos - d->len) & 31;
    d->sr += pr - d->mr[old]; d->si += pi - d->mi[old];
    d->mr[d->mpos] = pr; d->mi[d->mpos] = pi;
    d->mpos = (d->mpos + 1) & 31;

    or_ = d->zr[(d->zpos - d->len) & 31]; oi = d->zi[(d->zpos - d->len) & 31];
    d->zr[d->zpos] = d->sr; d->zi[d->zpos] = d->si;
    d->zpos = (d->zpos + 1) & 31;

    d->pw += ((int64_t)x * x - d->pw) / 64;
    if (d->pw > 400) {
        /* S carries half its power in the carrier line: |z|^2 = len^2 P / 4.
         * div is 8 or 16 wherever the modem sets it: divisions by those
         * constants are shifts, not a 64-bit division every sample */
        int64_t e = d->pw * d->len * d->len;
        int64_t thr = d->div == 16 ? e / 16 : d->div == 8 ? e / 8 : e / d->div;
        on = (int64_t)d->sr * d->sr + (int64_t)d->si * d->si > thr;
    }

    if (d->holdoff > 0) d->holdoff--;
    if (on) {
        d->present++;
    } else {
        if (d->present >= SDET_PRESENT) d->holdoff = -(d->len + 4);   /* armed */
        d->present = 0;
    }
    if (d->holdoff < 0) {
        int64_t o2 = (int64_t)or_ * or_ + (int64_t)oi * oi;
        int64_t p = (int64_t)d->sr * or_ + (int64_t)d->si * oi;
        d->holdoff++;
        /* reversed, and already half-way to full amplitude the other way */
        if (p < -(o2 / 2)) { ev = 1; d->holdoff = 400; }
    }
    return ev;
}

/* ---- re-acquisition on a later S ---- */
uint32_t v34_angle(int32_t re, int32_t im)
{
    /* first octant: atan(z) in turns ~ z (1/8 + 0.04345 (1 - z)), within 0.1 deg */
    int64_t ar = re < 0 ? -(int64_t)re : re, ai = im < 0 ? -(int64_t)im : im, z, t;
    uint32_t a;
    int swap = ai > ar;
    if (swap) { int64_t x = ar; ar = ai; ai = x; }
    if (ar == 0) return 0;
    z = (ai << 16) / ar;                                    /* Q16, 0..1 */
    t = (z * (8192 + ((2848 * (65536 - z)) >> 16))) >> 16;  /* turns, Q16 */
    a = (uint32_t)t << 16;
    if (swap) a = 0x40000000u - a;
    if (re < 0) a = 0x80000000u - a;
    if (im < 0) a = 0u - a;
    return a;
}

enum { SRES_WAIT, SRES_PHASE, SRES_TIME, SRES_SBAR };
#define SRES_ON      6      /* symbols of energy before S is believed */
#define SRES_SKIP    8      /* symbols left to the filters before measuring */
#define SRES_AVG     16     /* symbols averaged for the carrier phase (even) */
#define SRES_MINTIME 24     /* symbols of timing recovery before S-bar is looked for */

int sres_in_s(const sres_t *s)
{
    return (s->state == SRES_TIME && s->cnt >= SRES_MINTIME) || s->state == SRES_SBAR;
}

void sres_init(sres_t *s)
{
    s->state = SRES_WAIT; s->cnt = 0; s->neg = 0; s->sr = s->si = 0;
}

void sres_resume(sres_t *s)
{
    s->state = SRES_TIME; s->cnt = 0; s->neg = 0; s->sr = s->si = 0;
}

int sres_symbol(sres_t *s, qamrx_t *r)
{
    int32_t zr = r->zr, zi = r->zi, dr, di;
    int64_t e = (int64_t)zr * zr + (int64_t)zi * zi;
    const int64_t half = (int64_t)QRX_REF * QRX_REF;        /* half the power of S */

    switch (s->state) {
    case SRES_WAIT:
        s->cnt = e > half / 2 ? s->cnt + 1 : 0;
        if (s->cnt >= SRES_ON + SRES_SKIP) { s->state = SRES_PHASE; s->cnt = 0; s->sr = s->si = 0; }
        qamrx_update(r, zr, zi, 0, 12);
        return 0;

    case SRES_PHASE:
        s->sr += zr; s->si += zi;
        qamrx_update(r, zr, zi, 0, 12);
        if (++s->cnt < SRES_AVG) return 0;
        s->sr /= SRES_AVG; s->si /= SRES_AVG;
        if (s->sr * s->sr + s->si * s->si < half / 2) {     /* no carrier line: not S */
            sres_init(s);
            return 0;
        }
        /* the mean should sit at +90 degrees: z = y exp(-j theta) */
        r->theta += v34_angle((int32_t)s->sr, (int32_t)s->si) - 0x40000000u;
        s->state = SRES_TIME; s->cnt = 0; s->neg = 0;
        return 0;

    case SRES_TIME:
        if (e < half / 4) {                                 /* it went away again */
            qamrx_update(r, zr, zi, 0, 12);
            sres_init(s);
            return 0;
        }
        if (zi < 0 && s->cnt < SRES_MINTIME) {
            /* S stays in the upper half plane whatever the timing; random
             * symbols (SCR) do not for long */
            qamrx_update(r, zr, zi, 0, 12);
            sres_init(s);
            return 0;
        }
        if (zi < 0) {
            if (++s->neg == 2) { s->state = SRES_SBAR; s->cnt = 1; }   /* counted below */
        } else {
            s->neg = 0;
        }
        /* Carrier from the mean of each pair, not from decisions: while the
         * timing is still half a symbol out both points fall on the mean and
         * decisions would turn the pattern by 45 degrees. */
        if (!s->neg && s->cnt > 0)
            r->theta += (uint32_t)((int32_t)(v34_angle(zr + (int32_t)s->sr, zi + (int32_t)s->si) - 0x40000000u) / 4);
        s->sr = zr; s->si = zi;
        dr = zr < 0 ? -QRX_REF : QRX_REF;
        di = s->neg ? -QRX_REF : QRX_REF;
        qamrx_update(r, dr, di, QRX_TIMING | QRX_TFAST, 12);
        s->cnt++;
        return 0;

    default:    /* SRES_SBAR: s->cnt symbols of S-bar seen */
        if (s->cnt > 2)
            r->theta += (uint32_t)((int32_t)(v34_angle(zr + (int32_t)s->sr, zi + (int32_t)s->si) - 0xC0000000u) / 4);
        s->sr = zr; s->si = zi;
        dr = zr < 0 ? -QRX_REF : QRX_REF;
        qamrx_update(r, dr, -QRX_REF, QRX_TIMING, 12);
        return ++s->cnt == V34_SBAR_LEN;
    }
}
