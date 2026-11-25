/* V.34 QAM modulator. See v34mod.h. */
#include <string.h>
#include "v34mod.h"
#include "clock.h"
#include "sintab.h"

/* 10^(-n/20), Q15, n = 0..7 dB */
static const uint16_t RED[8] = { 32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637 };

int v34mod_init(v34mod_t *m, int sym_rate, int car_num, int car_den, int preemph,
                int level, int pwr_red_db)
{
    int hi, g = 4096;
    memset(m, 0, sizeof *m);
    if (preemph < 0 || preemph > 10 || car_den <= 0) return -1;
    /* which of the two carriers of Table 2 this is: 2/3 or 3/4 at 2400,
     * 3/5 or 2/3 at 3000, 4/7 or 3/5 at 3200 */
    hi = sym_rate == 2400 ? (car_num * 10 > car_den * 7)
       : sym_rate == 3000 ? (car_num * 100 > car_den * 63) : (car_num * 100 > car_den * 58);
    switch (sym_rate) {
    case 2400: m->rrc = &v34mod_rrc_2400[0][0]; m->a = 3; m->b = 10;
               if (preemph) { m->pre = v34mod_pre_2400[preemph - 1]; g = v34mod_pre_gain_2400[preemph - 1][hi]; } break;
    case 3000: m->rrc = &v34mod_rrc_3000[0][0]; m->a = 3; m->b = 8;
               if (preemph) { m->pre = v34mod_pre_3000[preemph - 1]; g = v34mod_pre_gain_3000[preemph - 1][hi]; } break;
    case 3200: m->rrc = &v34mod_rrc_3200[0][0]; m->a = 2; m->b = 5;
               if (preemph) { m->pre = v34mod_pre_3200[preemph - 1]; g = v34mod_pre_gain_3200[preemph - 1][hi]; } break;
    default: return -1;
    }
    m->sym_rate = sym_rate;
    m->acc = m->b;                  /* the first tick takes a symbol */
    m->dph = (uint32_t)((((uint64_t)sym_rate * (uint32_t)car_num) << 32) / ((uint64_t)8000 * (uint32_t)car_den));
    if (pwr_red_db < 0) pwr_red_db = 0;
    if (pwr_red_db > 7) pwr_red_db = 7;
    /* a 4-point signal leaves the pulse filter with RMS V34MOD_REF */
    m->gain = (int32_t)((((int64_t)level * g) / V34MOD_REF * RED[pwr_red_db]) >> 15);
    return 0;
}

int v34mod_due(const v34mod_t *m) { return m->acc >= m->b; }

void v34mod_put(v34mod_t *m, int32_t re, int32_t im)
{
    if (re > 16383) re = 16383;
    if (re < -16383) re = -16383;
    if (im > 16383) im = 16383;
    if (im < -16383) im = -16383;
    m->acc -= m->b;
    m->hpos = m->hpos ? m->hpos - 1 : V34MOD_NT - 1;
    m->hr[m->hpos] = m->hr[m->hpos + V34MOD_NT] = (int16_t)re;
    m->hi[m->hpos] = m->hi[m->hpos + V34MOD_NT] = (int16_t)im;
    m->nsym = clk_sat_inc(m->nsym);
}

int16_t v34mod_tick(v34mod_t *m)
{
    const int16_t *g, *hr, *hi;
    int32_t br = 0, bi = 0, c, s, y;
    int k, fr;
    if (m->acc >= m->b) v34mod_put(m, 0, 0);
    g = m->rrc + m->acc * V34MOD_NT; hr = m->hr + m->hpos; hi = m->hi + m->hpos;
    for (k = 0; k < V34MOD_NT; k++) { br += hr[k] * g[k]; bi += hi[k] * g[k]; }
    br >>= 13; bi >>= 13;
    m->acc += m->a;
    /* carrier, sine interpolated between table entries */
    k = (int)(m->ph >> 22); fr = (int)((m->ph >> 12) & 1023);
    s = sintab[k] + (((sintab[(k + 1) & SIN_MASK] - sintab[k]) * fr) >> 10);
    k = (k + SIN_N / 4) & SIN_MASK;
    c = sintab[k] + (((sintab[(k + 1) & SIN_MASK] - sintab[k]) * fr) >> 10);
    m->ph += m->dph;
    y = (int32_t)(((int64_t)br * c - (int64_t)bi * s) >> 15);
    if (m->pre) {
        const int16_t *p;
        int32_t acc;
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        m->ppos = m->ppos ? m->ppos - 1 : V34MOD_NPRE - 1;
        m->pb[m->ppos] = m->pb[m->ppos + V34MOD_NPRE] = (int16_t)y;
        p = m->pb + m->ppos;
        acc = p[V34MOD_NPRE / 2] * m->pre[0];
        for (k = 1; k <= V34MOD_NPRE / 2; k++)
            acc += (p[V34MOD_NPRE / 2 - k] + p[V34MOD_NPRE / 2 + k]) * m->pre[k];
        y = acc >> 13;
    }
    y = (int32_t)(((int64_t)y * m->gain) >> 12);
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    return (int16_t)y;
}

int32_t v34mod_delay_q8(const v34mod_t *m)
{
    /* V34MOD_SPAN symbols of pulse, and half of the pre-emphasis filter */
    return (int32_t)(((int64_t)V34MOD_SPAN * 8000 * 256) / m->sym_rate) + (m->pre ? (V34MOD_NPRE / 2) * 256 : 0);
}
