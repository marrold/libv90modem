/* V.92 digital modem: Ru / Su detector and training receiver. See v92_rx.h. */
#include <string.h>
#include "v92_rx.h"

/* ---- Ru / Su detector ----------------------------------------------------
 * Ru (8.5.5) is + + + - - -, Su (8.5.6) is +a 0 +a -a 0 -a: whatever the
 * channel does to them, x(n) = x(n - 6) while one of them lasts, and for the
 * six samples after a change to the inverse x(n) = -x(n - 6). Both are also
 * their own inverse half a period on, x(n) = -x(n - 3), which a slow wander
 * (the rest of an echo with a long tail, reading 53) is not, although it
 * repeats after 6 samples as well as after any other few. */

void v92_p6_init(v92_p6_t *d) { memset(d, 0, sizeof *d); }

int v92_p6_tick(v92_p6_t *d, int16_t x, int32_t floor_rms)
{
#define X(k) ((int64_t)d->x[(d->pos - (k)) & 63])     /* int64: 2 x(n) x(n-6) at full scale passes int32 */
    int64_t fl = (int64_t)floor_rms * floor_rms * 2;
    int r = 0;
    d->pos = (uint8_t)((d->pos + 1) & 63);
    d->x[d->pos] = x;
    d->c48 += 2 * (X(0) * X(6) - X(48) * X(54));
    d->e48 += X(0) * X(0) - X(48) * X(48) + X(6) * X(6) - X(54) * X(54);
    d->a48 += 2 * (X(0) * X(3) - X(48) * X(51));
    d->e3  += X(0) * X(0) - X(48) * X(48) + X(3) * X(3) - X(51) * X(51);
    d->c6  += 2 * (X(0) * X(6) - X(6) * X(12));
    d->e6  += X(0) * X(0) - X(12) * X(12);
#undef X
    if (d->c48 * 4 > d->e48 * 3 && d->e48 > 48 * fl && -d->a48 * 4 > d->e3 * 3) { if (d->run < 255) d->run++; }
    else d->run = 0;
    if (d->run >= 12) d->armed = 1;
    if (d->armed) {
        r = V92_P6_ON;
        if (d->c6 * 2 < -d->e6 && d->e6 > 6 * fl) { r = V92_P6_REV; d->armed = 0; d->run = 0; }
        else if (d->run == 0 && d->c48 * 4 < d->e48) d->armed = 0;     /* it went away */
    }
    return r;
}

/* ---- training receiver ---------------------------------------------------- */

static int32_t asr64(int64_t v, int s) { return (int32_t)((v + ((int64_t)1 << (s - 1))) >> s); }

void v92_trn_init(v92_trn_t *t)
{
    memset(t, 0, sizeof *t);
    v92_sgn_init(&t->ref, 0);
    t->levels = 2;
}

void v92_trn_levels(v92_trn_t *t, int levels) { t->levels = (uint8_t)(levels == 4 ? 4 : 2); }
void v92_trn_hold(v92_trn_t *t, int on) { t->est_off = (uint8_t)(on != 0); }

int32_t v92_trn_peak(const v92_trn_t *t)
{
    int c = t->cpk;
    int64_t a, b, d, den;
    if (c < 1 || c >= V92_CH_NTAP - 1) return 0;
    a = t->h[c - 1] >> 16; b = t->h[c] >> 16; d = t->h[c + 1] >> 16;
    if (b < 0) { a = -a; b = -b; d = -d; }
    den = a - 2 * b + d;                       /* < 0 at a maximum */
    if (den >= 0) return 256 * (c - V92_CH_LEAD);
    return 256 * (c - V92_CH_LEAD) + (int32_t)(128 * (a - d) / den);
}

/* the cursor: the largest tap where a channel a few ms long can put it */
static int find_cursor(v92_trn_t *t)
{
    int j, c = 8;
    int64_t best = -1, hp;
    for (j = 8; j <= 56; j++) {
        int64_t a = t->h[j] < 0 ? -t->h[j] : t->h[j];
        if (a > best) { best = a; c = j; }
    }
    hp = t->h[c] / (1 << 24);
    if (hp > -16 && hp < 16) return -1;
    t->cpk = (int16_t)c; t->hpk = (int32_t)hp;
    t->gq = (int32_t)(((int64_t)V92_SYM_UNIT << 15) / (hp < 0 ? -hp : hp));
    memset(t->f, 0, sizeof t->f); memset(t->b, 0, sizeof t->b);
    t->f[V92_EQ_DFF] = hp < 0 ? -(1 << 20) : (1 << 20);
    t->eq_on = 1; t->neq = 0; t->mse = 0;
    return 0;
}

/* NLMS step of the channel estimate on sample ke */
static void estimate(v92_trn_t *t, int32_t ke)
{
    int64_t acc = 0;
    int32_t e, es, m0 = ke + V92_CH_LEAD;
    int j, g;
    for (j = 0; j < V92_CH_NTAP; j++)
        acc += (int64_t)(int32_t)(t->h[j] >> 8) * t->sb[(m0 - j) & 255];
    e = (int32_t)t->rb[ke & 255] - asr64(acc, 28);
    g = t->nest < 400 ? 0 : t->nest < 800 ? 1 : t->nest < 1600 ? 2 : t->nest < 3000 ? 3 : 5;
    /* h += mu e s / (96 LU^2), mu = 2^-g: in Q24 that is e s 4096 / 96 */
    es = (e * 43) >> g;
    for (j = 0; j < V92_CH_NTAP; j++)
        t->h[j] += (int64_t)es * t->sb[(m0 - j) & 255];
    if (t->nest >= 400) {
        int64_t d = (int64_t)e * e - (int64_t)t->res;
        t->res = (uint32_t)((int64_t)t->res + (t->nest < 800 ? d / 16 : d / 256));
    }
    t->nest++;
}

int v92_trn_tick(v92_trn_t *t, int16_t r)
{
    int32_t k = t->k++, ke = k - V92_TRN_DL, m, y, d, e;
    int64_t acc;
    int i, level, msh;

    t->rb[k & 255] = r;
    while (t->mref <= k + V92_CH_LEAD && t->mref < V92_TRN_REF) {
        t->sb[t->mref & 255] = (int16_t)(V92_UP_SIGN(v92_sgn_tx_trn(&t->ref)) * V92_SYM_UNIT);
        t->mref++;
    }
    if (t->eq_on == 2) return -1;
    /* every symbol in the estimator's span must be one of TRN1u: wait 80 */
    if (ke >= 80 && !t->est_off) {
        estimate(t, ke);
        if (t->nest == V92_TRN_EST0 && !t->eq_on && find_cursor(t) != 0) { t->eq_on = 2; return -1; }
    }
    if (!t->eq_on) return 0;

    /* equaliser */
    acc = ((int64_t)r * t->gq) >> 15;
    t->qb[k & 31] = (int16_t)(acc > 32767 ? 32767 : acc < -32767 ? -32767 : acc);
    m = k - V92_EQ_DFF - (t->cpk - V92_CH_LEAD);
    if (m < 0) return 0;
    acc = 0;
    for (i = 0; i < V92_EQ_NF; i++) acc += (int64_t)t->f[i] * t->qb[(k - i) & 31];
    for (i = 0; i < V92_EQ_NB; i++) acc -= (int64_t)t->b[i] * t->sb[(m - 1 - i) & 255];
    y = asr64(acc, 20);
    if (m < t->mref) {                          /* known */
        d = t->sb[m & 255];
        level = d < 0 ? -1 : 1;
    } else if (t->levels == 4) {
        level = v92_trn2u_slice(y, 0, V92_SYM_UNIT);
        d = v92_trn2u_level(level, 0, V92_SYM_UNIT);
        t->sb[m & 255] = (int16_t)d;
    } else {
        level = y < 0 ? -1 : 1;
        d = level * V92_SYM_UNIT;
        t->sb[m & 255] = (int16_t)d;
    }
    e = y - d;
    if (e > 16383) e = 16383;
    if (e < -16383) e = -16383;
    msh = t->neq < 300 ? 2 : m < t->mref ? 3 : 5;
    for (i = 0; i < V92_EQ_NF; i++) t->f[i] -= (e * t->qb[(k - i) & 31]) >> (10 + msh);
    for (i = 0; i < V92_EQ_NB; i++) t->b[i] += (e * t->sb[(m - 1 - i) & 255]) >> (10 + msh);
    t->mse = (uint32_t)((int64_t)t->mse + ((int64_t)e * e - (int64_t)t->mse) / 64);
    t->neq++;
    t->dec_m = m; t->dec_level = (int16_t)level; t->dec_y = y;
    return 1;
}

void v92_trn_resume(v92_trn_t *t, int32_t m_t)
{
    int i;
    /* Ru-bar is the 24 symbols before m_t, Ru the 384 before that; both
     * patterns have period 6 and 384 is a multiple of 6 */
    for (i = 1; i <= 250; i++) {
        int32_t m = m_t - i;
        int bar = i <= 24;
        unsigned ph = (unsigned)(((m - m_t) % 6 + 6) % 6);
        t->sb[m & 255] = (int16_t)(V92_UP_SIGN(v92_ru_sbit(ph, bar)) * V92_SYM_UNIT);
    }
    for (i = 1; i <= 32; i++) {
        int64_t acc = ((int64_t)t->rb[(t->k - i) & 255] * t->gq) >> 15;
        t->qb[(t->k - i) & 31] = (int16_t)(acc > 32767 ? 32767 : acc < -32767 ? -32767 : acc);
    }
    t->mref = m_t;
    t->levels = 4;
}
