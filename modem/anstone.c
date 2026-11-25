/* See anstone.h. */
#include <string.h>
#include "anstone.h"
#include "sintab.h"
#include "fixmath.h"

#define FS 8000
/* a block whose 2100 Hz amplitude fell below this share of the block before
 * it, while its power stayed at least half of what a tone of that block's
 * amplitude has (2 amp^2 / N), is a phase reversal (or a phase jump of the
 * network): bridged with the block before */
#define REV_NUM  7          /* 0.7 */
#define REV_DEN  10
/* the window's verdict from r = (15 Hz amplitude) / (mean) of the envelope:
 * ANSam has 0.2 (0.19 at the least), ANS 0; above AM_HI it is ANSam, below
 * AM_LO it is ANS, in between nothing is decided */
#define AM_HI_NUM 12        /* 0.12 */
#define AM_LO_NUM 6         /* 0.06 */
#define AM_DEN    100

static uint32_t f2inc(int hz) { return (uint32_t)(((uint64_t)hz << 32) / FS); }
static int32_t  q15cos(uint32_t ph) { return sintab[((ph + 0x40000000u) >> 22) & SIN_MASK]; }
static int32_t  q15sin(uint32_t ph) { return sintab[(ph >> 22) & SIN_MASK]; }

static void window_reset(anst_t *d) { d->wsum = d->wc = d->ws = 0; d->wn = 0; d->amph = 0; }

void anst_init(anst_t *d, int floor_rms)
{
    memset(d, 0, sizeof *d);
    if (floor_rms <= 0) floor_rms = 112;          /* -43 dBm0 */
    d->dph = f2inc(2100);
    /* the block amplitude of a tone of that RMS is ANST_BLOCK * peak / 2;
     * three quarters of it, so that the troughs of an ANSam at the floor
     * (0.8 of its average) still count */
    d->min_amp = (int32_t)((int64_t)floor_rms * 1414 * ANST_BLOCK * 3 / (2 * 4 * 1000));
    d->prev = -1;
    d->result = ANST_NONE;
}

/* one window of the envelope: ANST_ANSAM, ANST_ANS, or -1 for neither */
static int verdict(const anst_t *d)
{
    /* r^2 = 4 (wc^2 + ws^2) / wsum^2, the 15 Hz amplitude over the mean */
    uint64_t c2 = (uint64_t)(d->wc * d->wc + d->ws * d->ws) * 4 * AM_DEN * AM_DEN;
    uint64_t m2 = (uint64_t)(d->wsum * d->wsum);
    if (c2 > m2 * AM_HI_NUM * AM_HI_NUM) return ANST_ANSAM;
    if (c2 < m2 * AM_LO_NUM * AM_LO_NUM) return ANST_ANS;
    return -1;
}

static void block(anst_t *d)
{
    int64_t a2 = (int64_t)d->bi * d->bi + (int64_t)d->bq * d->bq;
    int32_t amp = (int32_t)isqrt_u64((uint64_t)a2), env;
    /* the 2100 Hz component holds at least half of the block's power: for a
     * pure tone 2 amp^2 = N * power */
    int tone = amp >= d->min_amp && 4 * a2 >= (int64_t)ANST_BLOCK * d->bpow;
    int bridged = 0;

    if (!tone && d->last_amp > 0 && (int64_t)amp * REV_DEN < (int64_t)d->last_amp * REV_NUM
        && (int64_t)ANST_BLOCK * d->bpow >= (int64_t)d->last_amp * d->last_amp) {
        /* the tone was there in the block before and the power still is: a
         * reversal inside this block */
        bridged = 1;
        d->reversals++;
    }
    if (!tone && !bridged) {
        d->last_amp = 0;
        d->prev = -1;
        window_reset(d);
        return;
    }
    env = bridged ? d->last_amp : amp;
    if (!bridged) d->last_amp = amp;
    env >>= 4;                       /* keeps the window's sums within 64 bits */
    d->wsum += env;
    d->wc += ((int64_t)env * q15cos(d->amph)) >> 15;
    d->ws += ((int64_t)env * q15sin(d->amph)) >> 15;
    d->amph += (uint32_t)(((uint64_t)15 << 32) * ANST_BLOCK / FS);
    if (++d->wn == ANST_WINDOW) {
        int v = verdict(d);
        if (v >= 0 && v == d->prev && d->result == ANST_NONE) {
            d->result = (anst_result)v;
            d->at = d->samples;
        }
        d->prev = v;
        window_reset(d);
    }
}

anst_result anst_tick(anst_t *d, int16_t x)
{
    d->samples++;
    if (d->result != ANST_NONE) return d->result;
    d->bi += ((int32_t)x * q15cos(d->ph)) >> 15;
    d->bq += ((int32_t)x * q15sin(d->ph)) >> 15;
    d->bpow += (int32_t)x * x;
    d->ph += d->dph;
    if (++d->bn == ANST_BLOCK) {
        block(d);
        d->bi = d->bq = 0; d->bpow = 0; d->bn = 0;
    }
    return d->result;
}
