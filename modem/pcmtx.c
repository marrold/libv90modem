/* PCM upstream transmit clock and interpolator. See pcmtx.h. */
#include <string.h>
#include "pcmtx.h"
#include "pcmtx_tab.h"
#include "v34_tabs.h"
#include "interp2.h"

/* the wide bank (pcmtx_wide) is the receiver's, of the same shape */
typedef char pcmtx_same_banks[(V34_IP == PCMTX_IP && V34_IN == PCMTX_IN && V34_ID == PCMTX_ID) ? 1 : -1];

#define ONE32 ((int64_t)1 << 32)

void pcmtx_init(pcmtx_t *t)
{
    memset(t, 0, sizeof *t);
    t->tt = ONE32;              /* the first symbol instant: the first sample */
    t->since = 4 * ONE32;
}

void pcmtx_clock(pcmtx_t *t)
{
    t->tt -= ONE32;
    if (t->since < 4 * ONE32) t->since += ONE32;
}

int pcmtx_due(const pcmtx_t *t) { return t->tt <= 0; }

void pcmtx_put(pcmtx_t *t, int16_t sym, int64_t step)
{
    t->ring[t->pos] = sym;
    t->pos = (t->pos + 1) & (PCMTX_RING - 1);
    t->since = -t->tt;          /* the instant was tt before this sample */
    t->tt += step > 0 ? step : ONE32;
    t->nsym++;
}

/* The bank's branch p yields the signal PCMTX_ID + p / 32 samples before the
 * newest input it is laid on. The output is the symbols' signal at
 * PCMTX_DELAY = PCMTX_ID + 2 samples before now, and the newest symbol's
 * instant was `since` ago: laid on the newest symbol (j = 0) that is
 * PCMTX_ID + 2 - since samples before it, laid on the one before (j = 1)
 * PCMTX_ID + 1 - since. Of the two, the one that gives a branch in 0..32. */
int16_t pcmtx_out(const pcmtx_t *t)
{
    int64_t a, acc0, acc1, v;
    int j, p;
    int32_t f;
    const int16_t *h0, *h1;
    if (t->since < ONE32) { j = 1; a = ONE32 - t->since; }
    else { j = 0; a = 2 * ONE32 - t->since; if (a < 0) a = 0; }
    p = (int)(a >> 27);
    f = (int32_t)((a >> 12) & 0x7fff);
    if (p >= PCMTX_IP) { p = PCMTX_IP - 1; f = 0x8000; }
    if (t->wide) { h0 = v34_interp[p]; h1 = v34_interp[p + 1]; }
    else { h0 = pcmtx_interp[p]; h1 = pcmtx_interp[p + 1]; }
    /* ring[(pos - 1 - j - i) & (RING - 1)], newest first */
    interp2_64(t->ring, t->pos - 1 - j, PCMTX_RING - 1, h0, h1, PCMTX_IN, &acc0, &acc1);
    v = acc0 + (((acc1 - acc0) * f) >> 15);     /* Q14 */
    v = (v + 8192) >> 14;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void pcmtx_wide(pcmtx_t *t, int on) { t->wide = on != 0; }

void pcmtx_slip(pcmtx_t *t, uint32_t frac_q16)
{
    t->tt += (int64_t)frac_q16 << 16;
}
