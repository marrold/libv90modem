/* QAM receiver core for the V.34 upstream channel (symbol rates 3000 / 3200):
 *
 *   8 kHz real samples -> timing-controlled interpolation to three samples per
 *   symbol -> AGC -> fractionally spaced (T/3) equaliser with complex taps on
 *   the real pass-band signal -> carrier derotation -> one point per symbol.
 *
 * The caller supplies the wanted point for every symbol (a known training
 * symbol or its own decision); the core adapts equaliser, carrier loop and
 * timing loop from the error. Fixed-point, no allocation. */
#ifndef V90D_MODEM_QAMRX_H
#define V90D_MODEM_QAMRX_H
#include <stdint.h>

#define QRX_NTAP 72             /* 24 symbols at T/3 */
#define QRX_CENTER 36
#define QRX_REF 4096            /* a 4-point training symbol is (+-QRX_REF, +-QRX_REF) */

#define QRX_EQ      1           /* adapt the equaliser */
#define QRX_CARRIER 2           /* adapt the carrier loop */
#define QRX_TIMING  4           /* adapt the timing loop */
#define QRX_TFAST   8           /* with QRX_TIMING: acquisition gain, phase only */
#define QRX_DENSE   16          /* the wanted points differ in size (data mode): scale the
                                   carrier and timing errors by the mean power, not by
                                   this point's own, which makes small points shout */

typedef struct {
    /* configuration */
    int32_t  step;              /* input samples per T/3 output, Q24 */
    uint32_t dcar;              /* carrier advance per symbol */
    /* input side */
    int16_t  ring[64]; int rpos;
    int64_t  pw;                /* smoothed input power */
    int32_t  gain;              /* Q8 */
    int      running;
    int32_t  tnext, t_integ;    /* Q24 samples */
    int32_t  m;                 /* T/3 output index; symbol n is on time at m = QRX_CENTER + 3n */
    /* equaliser */
    int16_t  x[2 * QRX_NTAP]; int xpos;
    int32_t  cr[QRX_NTAP], ci[QRX_NTAP];    /* Q20 */
    int32_t  yr, yi;            /* on-time output before derotation */
    int32_t  ymr, ymi;          /* output one T/3 early */
    int32_t  er, ei;            /* base-band error of the last symbol, for timing */
    int32_t  dnorm;             /* |d|^2 of the last update */
    int      timing_on, timing_fast;
    /* carrier */
    uint32_t theta; int32_t freq;
    /* results */
    int32_t  zr, zi;            /* derotated point of the newest symbol */
    int32_t  nsym;              /* symbols out, modulo 2^31 */
    int32_t  mse;               /* smoothed |e|^2 / |d|^2, Q16 (65536 = 0 dB) */
    int32_t  cgrow;             /* the most any tap can have moved since the
                                   taps were last held (qamrx_update) */
    int64_t  mse_acc;           /* the same with 8 more fractional bits */
} qamrx_t;

/* sym_rate 3000 or 3200; carrier as the ratio num/den of the symbol rate
 * (V.34 Table 2: e.g. 3/5 for 1920 Hz at 3200) */
void qamrx_init(qamrx_t *r, int sym_rate, int car_num, int car_den);
/* Start symbol timing: the centre of symbol 0 reaches the receiver input
 * `delay_q8` / 256 samples after the next sample fed to qamrx_tick(). Also
 * fixes the AGC from the power seen so far, clears the equaliser to a centre
 * spike and resets the loops. */
void qamrx_sync(qamrx_t *r, int32_t delay_q8);
/* Feed one sample. Returns 1 when a new point is in zr/zi; the caller must
 * then call qamrx_update() before the next tick. */
int  qamrx_tick(qamrx_t *r, int16_t x);
/* dr/di: the point that should have been received; mu_shift: equaliser step
 * (10 = fast training ... 14 = slow tracking) */
void qamrx_update(qamrx_t *r, int32_t dr, int32_t di, int flags, int mu_shift);
/* centre of gravity of the equaliser taps, in T/3 taps x 256 */
int  qamrx_centroid(const qamrx_t *r);
/* For data mode: x, or 0 when it is an impulse, more than 18 dB above the
 * input RMS measured at S (8 times it: gain is set once in qamrx_sync and
 * is not changed in data mode, so this threshold does not follow later
 * level changes, and a full-scale click needs an RMS at S under about
 * 4015, -12 dBm0, to be caught). A click let in
 * passes along the whole equaliser line, and each update on the way moves
 * every tap by the error times the click; where the signal has little
 * energy (the band edges) the tracking step never takes that back, and
 * the receiver stays deaf for the rest of the call (Task 20,
 * docs/history/audit-0.6.md). Data's own peaks stay under this level;
 * those of some training sequences do not, hence data mode only. */
#define QRX_BLANK 16384
static inline int16_t qamrx_blank(const qamrx_t *r, int16_t x)
{
    return (int32_t)(x < 0 ? -x : x) * r->gain > (int32_t)QRX_BLANK * 256 ? 0 : x;
}

#endif
