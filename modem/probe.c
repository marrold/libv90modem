/* L1 / L2 line probing: generator and block-DFT analyser. See probe.h. */
#include "probe.h"
#include "fixmath.h"

int probe_has_tone(int m)
{
    return m >= 1 && m <= PROBE_NBIN && m != 6 && m != 8 && m != 12 && m != 16;
}

/* Table 17/V.34: tones whose initial phase is 180 degrees */
static int phase180(int m)
{
    return m == 2 || m == 11 || m == 15 || m == 18 || m == 20 || m == 21 || m == 22 || m == 23;
}

/* cos(2 pi k / 160), Q15, by linear interpolation in the sine table: ph =
 * k 2^32 / 160 + 2^30, i = ph >> 22, f = (ph >> 7) & 0x7fff, sintab[i] +
 * ((sintab[i + 1] - sintab[i]) f >> 15), computed once (ARMv5 took some 50000
 * instructions for its 64-bit divisions, in one tick, at every start) */
static const int16_t cos160_tab[PROBE_BLK] = {
    32767, 32741, 32666, 32540, 32363, 32137, 31861, 31536, 31163, 30741, 30273, 29757,
    29195, 28589, 27938, 27245, 26508, 25732, 24915, 24061, 23170, 22242, 21280, 20285,
    19259, 18204, 17120, 16011, 14876, 13718, 12539, 11341, 10125, 8894, 7649, 6393,
    5125, 3851, 2570, 1286, 0, -1287, -2571, -3851, -5126, -6393, -7650, -8895,
    -10126, -11342, -12539, -13719, -14876, -16011, -17121, -18204, -19260, -20286, -21281, -22243,
    -23170, -24062, -24916, -25733, -26509, -27245, -27938, -28590, -29196, -29757, -30273, -30742,
    -31164, -31537, -31862, -32137, -32364, -32541, -32666, -32742, -32767, -32742, -32667, -32541,
    -32364, -32137, -31862, -31537, -31164, -30742, -30273, -29758, -29196, -28590, -27939, -27245,
    -26509, -25733, -24916, -24062, -23170, -22243, -21281, -20286, -19260, -18204, -17121, -16012,
    -14877, -13719, -12539, -11342, -10126, -8895, -7650, -6393, -5126, -3852, -2571, -1287,
    0, 1286, 2570, 3850, 5125, 6393, 7649, 8894, 10125, 11341, 12539, 13718,
    14875, 16010, 17120, 18204, 19259, 20285, 21280, 22242, 23170, 24061, 24915, 25732,
    26508, 27245, 27937, 28589, 29195, 29756, 30273, 30741, 31163, 31536, 31861, 32137,
    32363, 32540, 32665, 32741,
};

void probetx_init(probetx_t *p, int level)
{
    const int16_t *cs = cos160_tab;
    uint8_t tm[PROBE_NBIN], tn[PROBE_NBIN];             /* the tones at 0 and at 180 degrees */
    int k, m, i, n0 = 0, n1 = 0;
    for (m = 1; m <= PROBE_NBIN; m++) {
        if (!probe_has_tone(m)) continue;
        if (phase180(m)) tn[n1++] = (uint8_t)m; else tm[n0++] = (uint8_t)m;
    }
    for (k = 0; k < PROBE_BLK; k++) {
        int32_t acc = 0;
        /* the same sum, tone by tone: cs[(3 m k) mod 160] */
        for (i = 0; i < n0; i++) acc += cs[(3 * tm[i] * k) % PROBE_BLK];
        for (i = 0; i < n1; i++) acc -= cs[(3 * tn[i] * k) % PROBE_BLK];
        /* each tone at amplitude level * sqrt(2/21) (10113 in Q15) */
        acc = (int32_t)(((int64_t)acc * 10113) >> 15);
        p->tab[k] = (int16_t)(((int64_t)acc * level) >> 15);
    }
    p->pos = 0;
}

int16_t probetx_tick(probetx_t *p, int l1)
{
    int32_t v = p->tab[p->pos];
    if (++p->pos == PROBE_BLK) p->pos = 0;
    if (l1) v *= 2;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

void proberx_init(proberx_t *r)
{
    int m;
    r->n = 0; r->nblk = 0;
    for (m = 0; m <= PROBE_NBIN; m++) {
        r->ar[m] = r->ai[m] = 0;
        r->lr[m] = r->li[m] = 0;
        r->pw[m] = r->p0[m] = r->cr[m] = r->ci[m] = 0;
    }
}

/* The block's DFT at 150 m Hz, m = 1 .. 25, summed as the samples come
 * (sample k of the block meets cos and sin of 2 pi 3 m k / 160: the same sums,
 * in the same order, as over the whole block at its end, which took some
 * 80000 ARM instructions in one tick) */
void proberx_tick(proberx_t *r, int16_t x)
{
    const int16_t *cs = cos160_tab;
    int m, k = r->n, step = 3 * k, idx = 0;
    while (step >= PROBE_BLK) step -= PROBE_BLK;     /* idx = (3 m k) mod 160, isn = (idx - 40) mod 160 */
    for (m = 1; m <= PROBE_NBIN; m++) {
        int isn;
        idx += step; if (idx >= PROBE_BLK) idx -= PROBE_BLK;
        isn = idx + PROBE_BLK - 40; if (isn >= PROBE_BLK) isn -= PROBE_BLK;
        r->ar[m] += (int32_t)x * cs[idx];
        r->ai[m] -= (int32_t)x * cs[isn];
    }
    if (++r->n < PROBE_BLK) return;
    r->n = 0;
    for (m = 1; m <= PROBE_NBIN; m++) {
        int32_t xr = (int32_t)(r->ar[m] >> 15), xi = (int32_t)(r->ai[m] >> 15);     /* amplitude * 80 */
        r->ar[m] = r->ai[m] = 0;
        r->pw[m] += (int64_t)xr * xr + (int64_t)xi * xi;
        if (r->nblk == 0) r->p0[m] = (int64_t)xr * xr + (int64_t)xi * xi;
        if (r->nblk > 0) {
            r->cr[m] += (int64_t)xr * r->lr[m] + (int64_t)xi * r->li[m];
            r->ci[m] += (int64_t)xi * r->lr[m] - (int64_t)xr * r->li[m];
        }
        r->lr[m] = xr; r->li[m] = xi;
    }
    r->nblk++;
}

/* repeating (signal) power and noise power per block at bin m, in |X|^2 units.
 * With c = sum X[k] conj(X[k-1]) over K-1 block pairs, the signal is |c|/(K-1)
 * and sum |X[k] - e^(j theta) X[k-1]|^2 = 2 sum|X|^2 - |X first|^2 - |X last|^2
 * - 2|c| is 2(K-1) times the noise, whatever the frequency offset theta. */
static void powers(const proberx_t *r, int m, int64_t *sig, int64_t *noise)
{
    int64_t a = r->cr[m] < 0 ? -r->cr[m] : r->cr[m], b = r->ci[m] < 0 ? -r->ci[m] : r->ci[m];
    /* |c| without overflow: scale down to 31 bits first */
    int sh = 0;
    while ((a >> sh) > 0x3fffffff || (b >> sh) > 0x3fffffff) sh++;
    int64_t c = (int64_t)isqrt_s64((a >> sh) * (a >> sh) + (b >> sh) * (b >> sh)) << sh;
    int64_t last = (int64_t)r->lr[m] * r->lr[m] + (int64_t)r->li[m] * r->li[m];
    *sig = c / (r->nblk - 1);
    *noise = (2 * r->pw[m] - r->p0[m] - last - 2 * c) / (2 * (r->nblk - 1));
}

int proberx_level(const proberx_t *r, int m)
{
    int64_t s, t;
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return 0;
    powers(r, m, &s, &t);
    return (int)isqrt_s64(s / 12800);
}

int64_t proberx_power(const proberx_t *r, int m)
{
    int64_t s, t;
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return 0;
    powers(r, m, &s, &t);
    return s > 0 ? s : 0;
}

int proberx_last(const proberx_t *r, int m)
{
    if (r->nblk < 1 || m < 1 || m > PROBE_NBIN) return 0;
    return (int)isqrt_s64(((int64_t)r->lr[m] * r->lr[m] + (int64_t)r->li[m] * r->li[m]) / 12800);
}

int proberx_noise(const proberx_t *r, int m)
{
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return 0;
    return (int)isqrt_s64(r->pw[m] / r->nblk / 12800);
}

int proberx_snr(const proberx_t *r, int m)
{
    int64_t s, n;
    int db;
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return 0;
    powers(r, m, &s, &n);
    if (s <= 0) return 0;
    if (n < 1) n = 1;
    db = probe_db10(s, n);
    return db < 0 ? 0 : db > 600 ? 600 : db;
}

void proberx_bin(const proberx_t *r, int m, int *snr, int *level, int64_t *power)
{
    int64_t s, n;
    int db;
    *snr = *level = 0; *power = 0;
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return;
    powers(r, m, &s, &n);
    *level = (int)isqrt_s64(s / 12800);
    *power = s > 0 ? s : 0;
    if (s <= 0) return;
    if (n < 1) n = 1;
    db = probe_db10(s, n);
    *snr = db < 0 ? 0 : db > 600 ? 600 : db;
}

int proberx_freq_off(const proberx_t *r)
{
    int64_t x = r->cr[7], y = r->ci[7], ax, ay, mn, mx, z, t;
    if (r->nblk < 3 || proberx_snr(r, 7) < 100) return -512;
    ax = x < 0 ? -x : x; ay = y < 0 ? -y : y;
    while (ax > 0x3fffffff || ay > 0x3fffffff) { ax >>= 1; ay >>= 1; }
    mn = ax < ay ? ax : ay; mx = ax < ay ? ay : ax;
    if (mx == 0) return -512;
    z = (mn << 15) / mx;                                    /* Q15, 0..1 */
    /* atan(z) in turns, Q20: z * (1/8 + 0.04345 * (1 - z)) */
    t = (z * (131072 + ((45560 * (32768 - z)) >> 15))) >> 15;
    if (ay > ax) t = 262144 - t;                            /* 1/4 turn - t */
    if (x < 0) t = 524288 - t;                              /* 1/2 turn - t */
    if (y < 0) t = -t;
    /* one turn per 20 ms block = 50 Hz = 2500 units of 0.02 Hz */
    t = (t * 2500) / 1048576;
    return (int)(t > 511 ? 511 : t < -511 ? -511 : t);
}

/* log2(v) in Q8 */
static int32_t log2q8(int64_t v)
{
    static const uint16_t tab[17] = { 0, 22, 44, 63, 82, 100, 118, 134, 150, 165, 179, 193,
                                      207, 220, 232, 244, 256 };
    int n = 0, i, f;
    int64_t t = v;
    if (v <= 0) return -(64 << 8);
    /* n = floor(log2 v), by halves */
    if (t >= (int64_t)1 << 32) { t >>= 32; n += 32; }
    if (t >= (int64_t)1 << 16) { t >>= 16; n += 16; }
    if (t >= (int64_t)1 << 8) { t >>= 8; n += 8; }
    if (t >= (int64_t)1 << 4) { t >>= 4; n += 4; }
    if (t >= (int64_t)1 << 2) { t >>= 2; n += 2; }
    if (t >= (int64_t)1 << 1) { n += 1; }
    /* 8 mantissa bits below the leading one */
    t = n >= 8 ? (v >> (n - 8)) & 0xff : (v << (8 - n)) & 0xff;
    i = (int)(t >> 4); f = (int)(t & 15);
    return (n << 8) + tab[i] + (((tab[i + 1] - tab[i]) * f) >> 4);
}

int probe_db10(int64_t num, int64_t den)
{
    /* 10 log10(2) = 3.0103 dB per octave -> 30.103 tenths; / 256 for Q8 */
    return (int)(((int64_t)(log2q8(num) - log2q8(den)) * 30103) / 256000);
}
