/* What the Phase 2 exchanges share (p2d, p2ans, p2call). See p2common.h. */
#include "p2common.h"

const int16_t p2_sr[6] = { 2400, 2743, 2800, 3000, 3200, 3429 };
const int8_t p2_car[6][2][2] = {
    { {2, 3}, {3, 4} }, { {3, 5}, {2, 3} }, { {3, 5}, {2, 3} },
    { {3, 5}, {2, 3} }, { {4, 7}, {3, 5} }, { {4, 7}, {4, 7} } };
const int8_t p2_top[6] = { 9, 10, 10, 12, 13, 14 };

/* 1600 1800, 1646 1829, 1680 1867, 1800 2000, 1829 1920, 1959 1959 */
int p2_carrier_hz(int k, int hi)
{
    int d = p2_car[k][hi][0], e = p2_car[k][hi][1];
    return (p2_sr[k] * d + e / 2) / e;
}

int tone_under_probe(proberx_t *prx, int *tone_blocks, int16_t x, int m)
{
    proberx_tick(prx, x);
    if (prx->n != 0) return *tone_blocks >= 2;
    {
        int lvl = proberx_last(prx, m), ref = proberx_last(prx, 6);
        if (lvl > 30 && lvl > 4 * ref) (*tone_blocks)++; else *tone_blocks = 0;
    }
    return *tone_blocks >= 2;
}

uint32_t db10_lin(int d)
{
    static const uint16_t frac[9] = { 32768, 35734, 38968, 42495, 46341, 50535, 55109, 60097, 65535 };
    int32_t e = d * 2177;                   /* log2 in Q16: 0.0332193 * 65536 */
    int n = e >> 16, i = (e >> 13) & 7, f = e & 0x1fff;
    uint32_t m;
    if (d <= 0) return 1;
    m = (uint32_t)frac[i] + (uint32_t)(((int32_t)(frac[i + 1] - frac[i]) * f) >> 13);    /* 2^frac, Q15 */
    return n >= 15 ? m << (n - 15) : m >> (15 - n);
}

uint32_t lin_q16(int d)
{
    static const uint16_t frac[9] = { 32768, 35734, 38968, 42495, 46341, 50535, 55109, 60097, 65535 };
    int32_t e = (int32_t)d * 2177 + ((int32_t)32 << 16);    /* log2 in Q16, offset by 32 */
    int n = (int)(e >> 16), i = (int)((e >> 13) & 7), f = (int)(e & 0x1fff), sh;
    uint32_t m = (uint32_t)frac[i] + (uint32_t)(((int32_t)(frac[i + 1] - frac[i]) * f) >> 13);
    sh = n - 32 + 16 - 15;
    return sh >= 0 ? m << sh : m >> -sh;
}

/* indices 1..5 rise in a straight line to 2, 4 ... 10 dB at f = s; 6..10
 * are flat to 0.4 s, rise by beta to 0.8 s and by gamma more to 1.2 s */
int template_db10(int idx, int f, int s)
{
    int x = (int)((int32_t)f * 1000 / s);   /* f / s, thousandths */
    int beta = 5 * (idx - 5), gamma = 10 * (idx - 5);
    if (idx <= 0) return 0;
    if (idx <= 5) return 20 * idx * x / 1000;
    if (x <= 400) return 0;
    if (x <= 800) return beta * (x - 400) / 400;
    return beta + gamma * (x - 800) / 400;
}

/* 1 / mean(1 / (1 + SNR)) over the tones of the band, as p2ans's band_snr.
 * Each tone's SNR is its noise-limited one (r dB less) or the floor of the
 * noise that follows the signal (f_db10 below the whole signal, spread over
 * 4 kHz), the lower; the filter tilts both alike, the transmit power
 * staying the same. The carrier is Table 2's d / e of the symbol rate,
 * truncated (as v34_carrier gives it). */
int band(const int *snr, const int *rx_level, int k, int c, int idx, int r, int f_db10, const int *share)
{
    int s = p2_sr[k], fc = s * p2_car[k][c ? 1 : 0][0] / p2_car[k][c ? 1 : 0][1];
    int m, n = 0, tbar = 0, t[PROBE_NBIN + 1];
    int64_t sum = 0, tsum = 0;
    for (m = 1; m <= PROBE_NBIN; m++) {
        t[m] = 0;
        if (!probe_has_tone(m) || 150 * m < fc - s / 2 - 75 || 150 * m > fc + s / 2 + 75) continue;
        t[m] = template_db10(idx, 150 * m, s);
        tsum += lin_q16(t[m]);
        n++;
    }
    if (!n) return 0;
    tbar = probe_db10(tsum, (int64_t)n << 16);
    for (m = 1; m <= PROBE_NBIN; m++) {
        int v, lim;
        if (!probe_has_tone(m) || 150 * m < fc - s / 2 - 75 || 150 * m > fc + s / 2 + 75) continue;
        v = snr[m] - 10 * r;
        lim = rx_level[m] > 0 ? share[m] + f_db10 : 0;
        if (v > lim) v = lim;
        v += t[m] - tbar;
        if (v < 0) v = 0;
        if (v > 600) v = 600;
        sum += ((int64_t)1 << 40) / (1 + db10_lin(v));
    }
    if (sum < 1) return 0;
    /* the probing tones stand 150 Hz apart, the analyser's noise band is
     * 50 Hz: a signal that fills s Hz with the same power has 21 * 50 / s
     * of the per-tone SNR */
    return probe_db10((int64_t)n << 40, sum) + probe_db10(1050, s);
}
