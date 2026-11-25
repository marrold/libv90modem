/* V.34 Phase 3 training signals as seen by the receiver (V.34 10.1.3.6 to
 * 10.1.3.8): reference generators for PP and the 4-point TRN, the points of S
 * and S-bar, and the S / S-bar detector. Points are in QRX_REF units: point 0
 * of the quarter-superconstellation is (+QRX_REF, +QRX_REF). */
#ifndef V90D_MODEM_V34TRAIN_H
#define V90D_MODEM_V34TRAIN_H
#include <stdint.h>
#include "qamrx.h"
#include "../v34/v34.h"

#define V34_S_LEN 128
#define V34_SBAR_LEN 16

/* PP(i), i = 0..V34_PP_LEN - 1 (same power as the 4-point signals): the
 * sequence of v34_pp_phase(), in QRX_REF units (not a multiple of
 * v34_pp_point()'s Q14 values) */
void v34_pp(int i, int32_t *re, int32_t *im);
/* S: n-th symbol of a run of `len` symbols ending on point 0 rotated +90 deg;
 * S-bar: n-th symbol, starting on point 0 rotated 180 deg (v34_s_point() and
 * v34_sbar_point() in QRX_REF units) */
void v34_s(int n, int len, int32_t *re, int32_t *im);
void v34_sbar(int n, int32_t *re, int32_t *im);

/* self-synchronising scrambler, clause 7/V.34: v34.h's */
typedef v34_scr_t v34scr_t;
#define V34T_GPC V34_GPC         /* 1 + x^-18 + x^-23, call modem / digital modem */
#define V34T_GPA V34_GPA         /* 1 + x^-5 + x^-23, answer modem / analogue modem */
static inline void v34scr_init(v34scr_t *s, int poly) { v34_scr_init(s, poly); }
static inline int  v34scr_put(v34scr_t *s, int bit) { return v34_scramble(s, bit); }
static inline int  v34scr_get(v34scr_t *s, int bit) { return v34_descramble(s, bit); }

/* 4-point TRN: scrambled ones, two bits per symbol */
typedef struct { v34scr_t scr; } v34trn_t;
void v34trn_init(v34trn_t *t, int poly);
void v34trn_next(v34trn_t *t, int32_t *re, int32_t *im);
/* point 0 rotated clockwise by q * 90 degrees */
void v34_rot4(int q, int32_t *re, int32_t *im);

/* ---- S / S-bar detector ----
 * S is two points 90 degrees apart sent alternately: half of its power is a
 * steady tone at the carrier. S-bar is the same signal inverted. The detector
 * watches that tone through a moving average that nulls the two side lines. */
typedef struct {
    int      len;                   /* averaging length */
    int      div;                   /* S is believed when its carrier line holds more than
                                       2 / div of the power: 8 (a quarter) by default; 16
                                       where nothing but S can be on the line, for channels
                                       that tilt the three lines of S against each other */
    uint32_t ph, dph;
    int16_t  mr[32], mi[32]; int mpos;
    int32_t  sr, si;                /* moving sums */
    int32_t  zr[32], zi[32]; int zpos;
    int64_t  pw;                    /* smoothed input power */
    int32_t  present;               /* samples for which S (or S-bar) has been there */
    int32_t  holdoff;
} sdet_t;

void sdet_init(sdet_t *d, int sym_rate, int car_num, int car_den);
/* returns 1 once per S -> S-bar (or S-bar -> S) transition, SDET_DELAY samples
 * after the boundary between the two reached the input */
int  sdet_tick(sdet_t *d, int16_t x);
#define SDET_DELAY(d) (((d)->len * 3) / 4)
#define SDET_PRESENT 80             /* samples of tone = "S detected" */
/* S heard in data mode before the V.34 decoder holds four symbols of it (its
 * carrier line can build up early; the newest two inputs look like S's
 * points, v34_rx_s_begins): data decoding goes on for up to this many
 * samples more (9 symbols or more), until v34_rx_s_run() finds S. The
 * decoder delivers what it decided 48 symbols back, data still. Anything
 * else heard as S (noise) is taken at once, as before */
#define SDET_RUN_WAIT 32

/* ---- re-acquisition on a later S, equaliser kept ----
 * After a silence the far transmitter's symbol clock has drifted and its
 * carrier phase is arbitrary, but the channel, and so the equaliser, is the
 * same. S is the carrier (the mean of its two points, (0, +REF)) plus an
 * alternation at half the symbol rate: the mean gives the carrier phase
 * whatever the timing phase, the alternation then gives known symbols for
 * the timing loop, and the S / S-bar boundary gives the symbol count.
 *
 * Feed every symbol of the receiver core once the silence has begun:
 *     if (qamrx_tick(rx, x)) ev = sres_symbol(&s, rx);
 * sres_symbol() does the qamrx_update() itself. It returns 1 once, on the last
 * symbol of S-bar: the next symbol is the first of what follows. */
typedef struct {
    int     state, cnt, neg;
    int64_t sr, si;
} sres_t;

void sres_init(sres_t *s);
/* the same when S arrives without a silence before it (a rate renegotiation
 * out of data mode): the carrier is already locked, S is believed at once */
void sres_resume(sres_t *s);
int  sres_symbol(sres_t *s, qamrx_t *r);
/* 1 while S (or its S-bar) is being received and believed */
int  sres_in_s(const sres_t *s);
/* angle of (re, im) as a 32-bit phase (2^32 = one turn) */
uint32_t v34_angle(int32_t re, int32_t im);

#endif
