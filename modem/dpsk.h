/* Phase 2 signalling modem (V.34 10.1.2 / V.90 8.2): tones A and B with
 * 180-degree phase reversals, and the INFO sequences as 600 bit/s binary DPSK
 * on the same carriers (1200 Hz from the digital/call side, 2400 Hz plus an
 * 1800 Hz guard tone from the analogue/answer side).
 *
 * Transmitter model: the baseband is a level in {-1, 0, +1} whose every change
 * (carrier on/off, a DPSK "1", a tone phase reversal) is smoothed by the step
 * response of the 75 % root-raised-cosine pulse. For bits on the 600 Hz grid
 * that is exactly RRC-shaped DPSK; a tone is the constant level; a reversal
 * can be placed on any sample. Fixed-point, no allocation. */
#ifndef V90D_MODEM_DPSK_H
#define V90D_MODEM_DPSK_H
#include <stdint.h>
#include "info.h"

#define DPSK_FC_B 1200      /* tone B / INFO from the digital modem */
#define DPSK_FC_A 2400      /* tone A / INFO from the analogue modem */

/* samples from commanding a change to the middle of its transition on the line */
#define DPSK_TX_DELAY 33
/* samples from the middle of a reversal at the receiver input to DPSK_EV_REVERSAL */
#define DPSK_RX_REV_DELAY 84

#define DPSK_NEV 12

typedef struct {
    int16_t  step[240];             /* step response, Q14, 3 entries per sample */
    int32_t  base;                  /* settled level, Q14 */
    struct { int32_t delta, age; } ev[DPSK_NEV];
    int      nev;
    int      cur;                   /* commanded level: -1, 0, +1 */
    uint8_t  bits[INFO_MAX_BITS + 8];
    int      nbits, bitpos;
    int32_t  symclk;                /* 1/3-sample units until the next symbol instant */
    uint32_t cph, dph, gph, dgph;
    int32_t  amp, gamp, genv;
} dpsktx_t;

/* level = RMS of the nominal transmit power in 16-bit sample units. With
 * guard != 0 the carrier is sent 1 dB below it and an 1800 Hz guard tone 7 dB
 * below it accompanies the carrier (the analogue modem's format). */
void    dpsktx_init(dpsktx_t *t, int fc, int level, int guard);
void    dpsktx_on(dpsktx_t *t);         /* carrier on (no-op when already on) */
void    dpsktx_off(dpsktx_t *t);
void    dpsktx_reverse(dpsktx_t *t);    /* 180-degree phase reversal, now */
/* queue bits (copied); 1 = reversal, at 600 bit/s. Turns the carrier on first
 * when it is off (the point "at an arbitrary carrier phase"). */
void    dpsktx_send(dpsktx_t *t, const uint8_t *bits, int n);
int     dpsktx_busy(const dpsktx_t *t); /* bits not yet started */
int16_t dpsktx_tick(dpsktx_t *t);

#define DPSK_EV_FRAME    1      /* a CRC-clean INFO payload is in r->frame */
#define DPSK_EV_REVERSAL 2      /* a phase reversal in a steady tone */

typedef struct {
    const int16_t (*h)[2];
    int16_t  ring[128]; int rpos;
    uint32_t cph, dph;
    int32_t  zr[32], zi[32]; int zpos;
    int64_t  pw;                    /* smoothed |z|^2 */
    int      carrier;
    int32_t  steady;                /* samples of continuous unmodulated carrier */
    int      hold, cand;
    int32_t  clk;
    struct {
        int32_t pr, pi;
        uint8_t hist[128]; int pos;
    } hunt[3];
    int      expect;                /* payload length hunted for, 0 = none */
    uint8_t  frame[INFO_MAX_BITS];
    uint8_t  fill;                  /* the 4 bits before the sync of that frame, bit 0 the last */
    uint8_t  expect2;               /* a second payload length hunted for as well, 0 = none */
    uint8_t  flen;                  /* payload length of the frame in r->frame */
} dpskrx_t;

void dpskrx_init(dpskrx_t *r, int fc);
void dpskrx_expect(dpskrx_t *r, int payload_bits);
/* after dpskrx_expect(): hunt for frames of this payload length as well
 * (r->flen says which one came in); cleared by the next dpskrx_expect().
 * When it is the shorter, such a frame is reported only one bit after a
 * frame of the expected length with the same sync would have ended and did
 * not, in any hunter, so (expect - payload_bits + 1) bits late: an expected
 * frame that any hunter receives clean always wins. */
void dpskrx_expect_also(dpskrx_t *r, int payload_bits);
int  dpskrx_tick(dpskrx_t *r, int16_t x);
/* received carrier level, RMS in 16-bit sample units */
int  dpskrx_level(const dpskrx_t *r);

#endif
