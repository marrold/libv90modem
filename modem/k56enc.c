/* K56flex's data encoder. See k56enc.h.
 *
 * The rules are docs/history/k56-facts.md's "Data" and "Robbed bits", as
 * D-Modem (https://github.com/D-Modem/D-Modem at c06a380d, its notes on
 * K56flex) describes SM56's decoder. Facts only; no code of D-Modem. The
 * shell mapper is V.34's (v34/v34_shell.c). */
#include <string.h>
#include "k56enc.h"
#include "k56tab.h"
#include "../v90/v90_pcm.h"

/* the bits lost a frame for n6 robbed symbols in 6 */
static const uint8_t rob_loss[7] = { 0, 2, 4, 4, 6, 8, 8 };

void k56enc_init(k56enc_t *e, int rate, int pad, uint8_t rbs)
{
    const k56_rate_t *t;
    int j, n6 = 0;

    memset(e, 0, sizeof *e);
    if (rate < 1 || rate > K56_NRATES || pad < 0 || pad > 2)
        return;
    t = &k56_rates[rate - 1];
    e->rate = (uint8_t)rate;
    e->K = t->K;
    e->q = (uint8_t)(t->u - 1);
    e->r = t->r;
    e->ring = k56_ring(rate, pad);
    e->bios = k56_bios(rate, pad);
    v34_shell_tables(&e->shell, t->M[pad]);

    rbs &= 0x3f;
    if (e->q == 0)                      /* 32000, 34000: no bit to rob */
        rbs = 0;
    for (j = 0; j < 6; j++)
        n6 += (rbs >> j) & 1;
    if (n6 == 5) {                      /* treated as all six */
        rbs = 0x3f;
        n6 = 6;
    }
    e->n6 = (uint8_t)n6;
    for (j = 0; j < 24; j++)
        if ((rbs >> (j % 6)) & 1)
            e->rob |= (uint32_t)1 << j;
}

/* robbed symbols in frame f of the cycle */
static int robbed(const k56enc_t *e, int f)
{
    uint32_t b = (e->rob >> (8 * f)) & 0xffu;
    int n = 0;
    for (; b; b &= b - 1)
        n++;
    return n;
}

/* the shell mapper's shift (L - n) for this frame; K' = K - shift */
static int shell_shift(const k56enc_t *e)
{
    return e->n6 == 3 ? 0 : rob_loss[e->n6] - robbed(e, e->frame);
}

int k56enc_bits(const k56enc_t *e)
{
    int base;
    if (!e->rate)
        return 0;
    base = 30 + 2 * e->rate;
    return e->n6 == 3 ? base - robbed(e, e->frame) : base - rob_loss[e->n6];
}

/* the next symbol's counters; returns 1 when it is a shaping symbol */
static int shaping(const k56enc_t *e)
{
    return e->shape == e->r - 1;
}

static void next_symbol(k56enc_t *e)
{
    e->shape = (uint8_t)(shaping(e) ? 0 : e->shape + 1);
}

static void next_frame(k56enc_t *e)
{
    e->frame = (uint8_t)(e->frame == 2 ? 0 : e->frame + 1);
}

void k56enc_frame(k56enc_t *e, const uint8_t *bits, uint8_t out[K56ENC_N])
{
    uint8_t ring[K56ENC_N];
    uint32_t s = 0;
    int sh, kp, pos, i, b;

    if (!e->rate) {
        memset(out, 0xff, K56ENC_N);
        return;
    }
    sh = shell_shift(e);
    kp = e->K - sh;
    for (i = 0; i < kp; i++)
        s |= (uint32_t)(bits[i] & 1) << i;
    v34_shell_map(&e->shell, s << sh, ring);

    pos = kp;
    for (i = 0; i < K56ENC_N; i++) {
        int rb = (int)((e->rob >> (8 * e->frame + i)) & 1u);
        int pos_sign, p, m;
        if (shaping(e))
            pos_sign = e->sum <= 0;
        else
            pos_sign = bits[pos++] & 1;
        p = e->ring[ring[i]] << e->q;
        for (b = rb; b < e->q; b++)
            p |= (bits[pos++] & 1) << b;
        m = e->bios[p];
        out[i] = (uint8_t)(pos_sign ? 0xff - m : 0x7f - m);
        e->sum += v90_octet_linear(0, out[i]);
        next_symbol(e);
    }
    next_frame(e);
}

int k56enc_unframe(k56enc_t *d, const uint8_t in[K56ENC_N], uint8_t *bits)
{
    uint8_t ring[K56ENC_N], pt[K56ENC_N], sign[K56ENC_N], shp[K56ENC_N], rbv[K56ENC_N];
    int sh, kp, pos, i, b, ok = 1;
    int np, M = d->shell.map.M;
    uint32_t s;

    if (!d->rate)
        return -1;
    np = M << d->q;
    sh = shell_shift(d);
    kp = d->K - sh;
    for (i = 0; i < K56ENC_N; i++) {
        int rb = (int)((d->rob >> (8 * d->frame + i)) & 1u);
        int m = 0x7f - (in[i] & 0x7f), p, g, k;
        for (p = 0; p < np; p++)
            if (rb ? !(p & 1) && (d->bios[p] | 1) == (m | 1) : d->bios[p] == m)
                break;
        if (p == np) {
            ok = 0;
            p = 0;
        }
        g = p >> d->q;
        for (k = 0; k < M && d->ring[k] != g; k++)
            ;
        ring[i] = (uint8_t)k;
        pt[i] = (uint8_t)(p & ((1 << d->q) - 1));
        sign[i] = (uint8_t)(in[i] >> 7);
        rbv[i] = (uint8_t)rb;
        shp[i] = (uint8_t)shaping(d);
        next_symbol(d);
    }
    next_frame(d);
    if (!ok)
        return -1;

    s = v34_shell_unmap(&d->shell, ring);
    if ((s & (((uint32_t)1 << sh) - 1)) || s >= (uint32_t)1 << d->K)
        return -1;
    s >>= sh;
    for (i = 0; i < kp; i++)
        bits[i] = (uint8_t)((s >> i) & 1);
    pos = kp;
    for (i = 0; i < K56ENC_N; i++) {
        if (!shp[i])
            bits[pos++] = sign[i];
        for (b = rbv[i]; b < d->q; b++)
            bits[pos++] = (uint8_t)((pt[i] >> b) & 1);
    }
    return pos;
}
