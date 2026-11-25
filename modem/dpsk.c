/* Phase 2 tone / INFO DPSK modem. See dpsk.h. */
#include "dpsk.h"
#include "sintab.h"
#include "fixmath.h"
#include "v22_tabs.h"

#define UNITS_SYM 40        /* time units per symbol; one sample = 3 units */
#define STEP_N 240

static uint32_t f2inc(int f)
{
    return (uint32_t)(((uint64_t)(uint32_t)f << 32) / 8000u);
}

/* ---------------- transmitter ---------------- */

void dpsktx_init(dpsktx_t *t, int fc, int level, int guard)
{
    int p, u;
    /* step response of the pulse driven by a unit impulse train, normalised
     * per polyphase branch so that a steady level is exactly constant */
    for (p = 0; p < UNITS_SYM; p++) {
        int32_t g = 0, acc = 0;
        for (u = p; u < STEP_N; u += UNITS_SYM) g += v22_txh[u];
        for (u = p; u < STEP_N; u += UNITS_SYM) {
            acc += v22_txh[u];
            /* acc 16384 / g; in 32 bits where the product fits (the same
             * quotient: ARMv5 divides 64-bit numbers slowly) */
            t->step[u] = (int16_t)(acc < 131072 && acc > -131072 ? acc * 16384 / g : (int64_t)acc * 16384 / g);
        }
    }
    t->base = 0; t->nev = 0; t->cur = 0;
    t->nbits = t->bitpos = 0; t->symclk = 0;
    t->cph = 0; t->dph = f2inc(fc);
    t->gph = 0; t->dgph = f2inc(1800);
    /* amplitude = RMS * sqrt(2); -1 dB = 0.8913, -7 dB = 0.4467 */
    t->amp = (int32_t)(((int64_t)level * 23170) >> 14);
    t->gamp = 0; t->genv = 0;
    if (guard) {
        t->gamp = (int32_t)(((int64_t)t->amp * 14637) >> 15);
        t->amp = (int32_t)(((int64_t)t->amp * 29205) >> 15);
    }
}

static void set_level(dpsktx_t *t, int v, int32_t age)
{
    int32_t delta = (int32_t)(v - t->cur) * 16384;
    if (v == t->cur) return;
    if (t->nev == DPSK_NEV) {           /* cannot happen at 600 baud; settle the oldest */
        int k;
        t->base += t->ev[0].delta;
        for (k = 1; k < t->nev; k++) t->ev[k - 1] = t->ev[k];
        t->nev--;
    }
    t->ev[t->nev].delta = delta;
    t->ev[t->nev].age = age;
    t->nev++;
    t->cur = v;
}

void dpsktx_on(dpsktx_t *t)      { if (t->cur == 0) set_level(t, 1, 0); }
void dpsktx_off(dpsktx_t *t)     { set_level(t, 0, 0); t->nbits = t->bitpos = 0; }
void dpsktx_reverse(dpsktx_t *t) { if (t->cur) set_level(t, -t->cur, 0); }

void dpsktx_send(dpsktx_t *t, const uint8_t *bits, int n)
{
    int k;
    if (n > (int)sizeof t->bits) n = (int)sizeof t->bits;
    for (k = 0; k < n; k++) t->bits[k] = bits[k];
    t->nbits = n; t->bitpos = 0;
    if (t->cur == 0) {
        set_level(t, 1, 0);             /* the reference point */
        t->symclk = UNITS_SYM;
    } else {
        t->symclk = 0;                  /* the tone itself is the reference */
    }
}

int dpsktx_busy(const dpsktx_t *t) { return t->nbits - t->bitpos; }

int16_t dpsktx_tick(dpsktx_t *t)
{
    int32_t b = t->base, out;
    int k, j;

    /* symbol clock: start the next bit */
    if (t->bitpos < t->nbits) {
        if (t->symclk <= 0) {
            if (t->bits[t->bitpos]) set_level(t, -t->cur, -t->symclk);
            t->bitpos++;
            t->symclk += UNITS_SYM;
        }
        t->symclk -= 3;
    }

    for (k = 0, j = 0; k < t->nev; k++) {
        if (t->ev[k].age >= STEP_N) { t->base += t->ev[k].delta; b += t->ev[k].delta; continue; }
        b += (int32_t)(((int64_t)t->ev[k].delta * t->step[t->ev[k].age]) >> 14);
        t->ev[k].age += 3;
        t->ev[j++] = t->ev[k];
    }
    t->nev = j;

    out = (int32_t)(((int64_t)b * t->amp) >> 14);
    out = (out * sintab[((t->cph >> 22) + SIN_N / 4) & SIN_MASK]) >> 15;
    t->cph += t->dph;
    if (t->gamp) {
        /* guard tone follows the carrier on/off with a 5 ms ramp */
        int32_t target = t->cur ? 16384 : 0;
        if (t->genv < target) { t->genv += 410; if (t->genv > target) t->genv = target; }
        else if (t->genv > target) { t->genv -= 410; if (t->genv < 0) t->genv = 0; }
        out += (int32_t)((((int64_t)t->gamp * t->genv) >> 14)
                         * sintab[((t->gph >> 22) + SIN_N / 4) & SIN_MASK]) >> 15;
        t->gph += t->dgph;
    }
    return (int16_t)out;
}

/* ---------------- receiver ---------------- */

#define REV_LAG 20              /* samples (1.5 symbols) for the reversal detector */
#define CARRIER_ON  (60 * 60)   /* |z|^2 thresholds: amplitude 60 ~ -51 dBm0 */
#define CARRIER_OFF (40 * 40)
#define MIN_STEADY 240          /* 30 ms of tone needed before a reversal counts */
#define CONFIRM 48              /* a reversal is reported this long after it is first seen, */
#define CONFIRM_STEADY 24       /* if the tone has been steady again for this long: a DPSK
                                   "1" inside an INFO sequence is followed by more reversals */

static const int hunt_off[3] = { 0, 13, 27 };
static const uint8_t SYNC[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };

void dpskrx_init(dpskrx_t *r, int fc)
{
    int i, h;
    r->h = (fc == DPSK_FC_B) ? v22_rx_lo[0] : v22_rx_hi[0];
    for (i = 0; i < 128; i++) r->ring[i] = 0;
    r->rpos = 0;
    r->cph = 0; r->dph = f2inc(fc);
    for (i = 0; i < 32; i++) r->zr[i] = r->zi[i] = 0;
    r->zpos = 0;
    r->pw = 0; r->carrier = 0; r->steady = 0; r->hold = 0; r->cand = 0;
    r->clk = 0;
    for (h = 0; h < 3; h++) {
        r->hunt[h].pr = r->hunt[h].pi = 0; r->hunt[h].pos = 0;
        for (i = 0; i < 128; i++) r->hunt[h].hist[i] = 0;
    }
    r->expect = 0; r->expect2 = 0; r->flen = 0;
}

void dpskrx_expect(dpskrx_t *r, int payload_bits)
{
    int h, i;
    if (payload_bits > INFO_MAX_BITS - 16) payload_bits = INFO_MAX_BITS - 16;
    r->expect = payload_bits;
    r->expect2 = 0;
    for (h = 0; h < 3; h++) for (i = 0; i < 128; i++) r->hunt[h].hist[i] = 0;
}

void dpskrx_expect_also(dpskrx_t *r, int payload_bits)
{
    if (payload_bits > INFO_MAX_BITS - 16) payload_bits = INFO_MAX_BITS - 16;
    r->expect2 = (uint8_t)payload_bits;
}

int dpskrx_level(const dpskrx_t *r)
{
    /* pw is an average of e = zr^2 + zi^2, never above the largest e and never
     * below 0; zr and zi are (br*c + bi*s) >> 15 of a sum that does not
     * overflow int32, so |zr|, |zi| <= 2^16, e <= 2^33 and pw / 2 <= 2^32. */
    return r->carrier ? (int)isqrt_u64((uint64_t)(r->pw / 2)) : 0;
}

/* a clean frame of payload length len that ended lag bits before hunter
 * h's last bit? */
static int frame_ends(dpskrx_t *r, int h, int len, int lag)
{
    const uint8_t *hs = r->hunt[h].hist;
    int start = r->hunt[h].pos - (8 + len) - lag, k;
    uint8_t pl[INFO_MAX_BITS];
    for (k = 0; k < 8; k++) if (hs[(start + k) & 127] != SYNC[k]) return 0;
    if (len < 16) return 0;     /* the CRC alone is 16 (and keeps gcc sure pl is set) */
    for (k = 0; k < len; k++) pl[k] = hs[(start + 8 + k) & 127];
    if (!info_check(pl, len)) return 0;
    for (k = 0; k < len; k++) r->frame[k] = pl[k];
    r->flen = (uint8_t)len;
    r->fill = 0;
    for (k = 1; k <= 4; k++) r->fill = (uint8_t)(r->fill | (hs[(start - k) & 127] << (k - 1)));
    return 1;
}

/* one differential bit for hunter h; returns 1 when a clean frame completed */
static int hunt_bit(dpskrx_t *r, int h, int bit)
{
    r->hunt[h].hist[r->hunt[h].pos] = (uint8_t)bit;
    r->hunt[h].pos = (r->hunt[h].pos + 1) & 127;
    if (!r->expect) return 0;
    if (frame_ends(r, h, r->expect, 0)) return 1;
    if (!r->expect2) return 0;
    /* The second length is taken only one bit after a frame of the first
     * with the same sync would have ended, and did not: the start of a
     * longer frame whose CRC happens to hold as a shorter one (1 in 2^16 of
     * them) is never taken for the shorter. The one bit more is for the
     * other hunters: each samples a symbol within one symbol of this one,
     * so one that has the longer frame clean has reported it (and cleared
     * every history) by then. The shorter frame arrives (expect - expect2
     * + 1) bits late; the history holds the 8 + expect + 1 + 4 it needs. */
    return frame_ends(r, h, r->expect2, r->expect > r->expect2 ? r->expect - r->expect2 + 1 : 0);
}

int dpskrx_tick(dpskrx_t *r, int16_t x)
{
    int64_t ar = 0, ai = 0, p, e;
    int32_t br, bi, c, s, zr, zi, or_, oi;
    int i, idx, h, ev = 0;

    r->ring[r->rpos] = x;
    idx = r->rpos;
    r->rpos = (r->rpos + 1) & 127;
    r->cph += r->dph;
    for (i = 0; i < V22_RXN; i++) {
        int32_t v = r->ring[(idx - i) & 127];
        ar += v * r->h[i][0];
        ai += v * r->h[i][1];
    }
    br = (int32_t)(ar >> 15); bi = (int32_t)(ai >> 15);
    s = sintab[(r->cph >> 22) & SIN_MASK];
    c = sintab[((r->cph >> 22) + SIN_N / 4) & SIN_MASK];
    zr = (br * c + bi * s) >> 15;
    zi = (bi * c - br * s) >> 15;

    r->zpos = (r->zpos + 1) & 31;
    r->zr[r->zpos] = zr; r->zi[r->zpos] = zi;
    or_ = r->zr[(r->zpos - REV_LAG) & 31]; oi = r->zi[(r->zpos - REV_LAG) & 31];

    e = (int64_t)zr * zr + (int64_t)zi * zi;
    r->pw += (e - r->pw) / 64;
    if (!r->carrier && r->pw > CARRIER_ON) r->carrier = 1;
    else if (r->carrier && r->pw < CARRIER_OFF) { r->carrier = 0; r->steady = 0; }

    /* tone / reversal detector */
    p = (int64_t)zr * or_ + (int64_t)zi * oi;
    if (r->hold) r->hold--;
    if (r->cand && --r->cand == 0 && r->steady >= CONFIRM_STEADY) ev |= DPSK_EV_REVERSAL;
    if (r->carrier && p > r->pw / 2) {
        r->steady++;
    } else {
        /* leaving a long enough tone arms the detector for one lag: a
         * reversal drives p from +|z|^2 through zero to -|z|^2 */
        if (r->steady >= MIN_STEADY) r->hold = REV_LAG;
        if (r->carrier && r->hold && p < -(r->pw / 2)) {
            r->cand = CONFIRM;
            r->hold = 0;
        }
        r->steady = 0;
    }

    /* INFO frame hunters at three symbol-clock phases */
    for (h = 0; h < 3; h++) {
        int32_t u = r->clk - hunt_off[h];
        if (u < 0) u += UNITS_SYM;
        if (u < 3) {
            int64_t d = (int64_t)zr * r->hunt[h].pr + (int64_t)zi * r->hunt[h].pi;
            r->hunt[h].pr = zr; r->hunt[h].pi = zi;
            if (r->carrier && hunt_bit(r, h, d < 0)) {
                int k, j;
                ev |= DPSK_EV_FRAME;
                for (k = 0; k < 3; k++) for (j = 0; j < 128; j++) r->hunt[k].hist[j] = 0;
                break;
            }
        }
    }
    r->clk += 3;
    if (r->clk >= UNITS_SYM) r->clk -= UNITS_SYM;
    return ev;
}
