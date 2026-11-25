/* V.92 upstream data mode decoder: the digital modem's inverse of 6.4.
 *
 * Input: one number per upstream symbol, the received value of u(n) on the
 * linear scale of the constellation points (v92.h). With the precoder and
 * prefilter the digital modem asked for in CPd, the channel delivers u(n)
 * itself plus noise at the network's sampling instants, so in the simplest
 * receiver this is the linear value of the received PCM octet; a receiver
 * with an echo canceller or a short equaliser in front hands over its output
 * instead.
 *
 * What it does, per symbol: finds the nearest constellation point in each of
 * the four residue classes of the index modulo 4 (the 8-way partition of
 * Figure 9/V.34 depends on the two indices of a 2D point modulo 4 only);
 * per trellis frame (4 symbols): Viterbi step over the V.34 code selected in
 * CPd, with the parity of the index sum as the coded bit Y0 (6.4.2);
 * V92_VIT_DEPTH trellis frames later: decided indices -> Ki (v92_eta_to_k)
 * -> modulus decoder -> descrambler (GPA) -> put().
 *
 * The first symbol given to the decoder must be data frame interval 0 of a
 * data frame sent with zero encoder memory (the first symbol of B1u).
 *
 * Integer only. Cost on the ARM926: see README.md.
 */
#ifndef V90D_V92_DEC_H
#define V90D_V92_DEC_H
#include "v92.h"
#include "v92_mod.h"
#include "../v90/v90_scr.h"

#ifndef V92_VIT_DEPTH
#define V92_VIT_DEPTH 24        /* decision delay in trellis frames; a multiple
                                   of 3 keeps the delay a whole number of
                                   data frames plus a constant */
#endif
#define V92_MAX_STATES 64

/* Symbols that must go in before the K bits of data frame f (0-based) come
 * out of put(): the frame's bits are delivered during the call that takes
 * symbol number V92_DEC_DELAY + 12 * f (counting from 1). */
#define V92_DEC_DELAY (4 * V92_VIT_DEPTH + 8)

typedef struct {                /* one trellis frame of survivor history */
    int16_t idx[V92_TFRAME][4]; /* nearest index per symbol and residue class */
    uint8_t prev[V92_MAX_STATES];
    uint8_t res[V92_MAX_STATES];/* residue class of each of the 4 symbols on
                                   the surviving branch, 2 bits each */
} v92_vstep_t;

typedef struct {
    const v92_map_t *map;
    v92_putbit_fn put;
    void     *user;
    v90_scr_t scr;
    v92_mod_t mod;
    uint8_t   descramble;       /* 1: bits are descrambled before put() */
    uint8_t   hard;             /* 1: no trellis decoding (diagnostic) */
    /* code tables, built by v92_dec_init */
    uint8_t   nstates;          /* 16, 32 or 64 */
    uint8_t   nin;              /* branches into each state: 4, 8 or 16 */
    uint8_t   nlab;             /* branch labels: 2 * nin */
    uint8_t   pred[V92_MAX_STATES][16][2];  /* { predecessor, label } */
    uint8_t   pairlab[64];      /* s1 << 3 | s2 -> label (inputs << 1 | Y0) */
    uint8_t   subres[8][2];     /* 2D subset -> its two (ca | cb << 2) */
    /* front end */
    uint8_t   i_in;             /* data frame interval of the next input */
    uint32_t  m[V92_TFRAME][4]; /* class metrics of the current trellis frame */
    int16_t   ix[V92_TFRAME][4];
    /* Viterbi */
    uint32_t  pm[V92_MAX_STATES];
    v92_vstep_t hist[V92_VIT_DEPTH];
    uint16_t  head;             /* next slot of hist to write */
    uint32_t  nsteps;           /* trellis frames pushed */
    /* back end */
    uint8_t   i_out;            /* data frame interval of the next decision */
    uint8_t   ki[V92_DFRAME];
    /* statistics */
    uint32_t  frames;           /* data frames delivered */
    uint32_t  bad_frames;       /* of those, frames the modulus decoder flagged
                                   (v92_mod_decode() returned 1) */
    uint32_t  err_avg;          /* see v92_dec_quality() */
} v92_dec_t;

/* map must outlive the decoder. Returns 0, or V92_ERR_RANGE if
 * v92_map_check() fails. */
int  v92_dec_init(v92_dec_t *d, const v92_map_t *map, v92_putbit_fn put,
                  void *user);

/* One received symbol, linear scale. If tent is not NULL it receives at
 * once the nearest constellation point of this interval's set (a plain
 * slicer decision, for the caller's adaptive loops). */
void v92_dec_put(v92_dec_t *d, int32_t r, int32_t *tent);

/* Right after v92_dec_init: the state of the convolutional encoder at the
 * first symbol is not taken to be zero (8.7.1 says it is; a real analogue
 * modem after a rate renegotiation was seen to start from another one,
 * reading 59). The first decisions then have the protection of an
 * unterminated code only. */
void v92_dec_any_start(v92_dec_t *d);

/* on = 0: put() receives the scrambled bits as the modulus decoder produced
 * them (b0 first) instead of descrambled data. Default is 1. */
void v92_dec_set_descramble(v92_dec_t *d, int on);

/* on != 0: every symbol is decided on its own, nearest point of the whole
 * constellation, no trellis decoding and no delay. The reference the BER
 * test compares against. Call right after v92_dec_init(). */
void v92_dec_set_hard(v92_dec_t *d, int on);

/* Force out the decisions still inside the Viterbi decoder (end of a test,
 * before a cleardown). Only whole data frames are delivered. */
void v92_dec_flush(v92_dec_t *d);

/* The same for the decisions on the inputs before the n newest only (a rate
 * renegotiation's Ru: the data the caller sent before it); whole data frames
 * only. y (may be NULL): the inputs given, the newest first, at least the
 * ones not decided yet (n + 4 * V92_VIT_DEPTH is enough): the path through
 * those delivered is then the survivor that costs least over them, not the
 * one through what followed. The decoder is to be initialised again before
 * it decodes on. */
void v92_dec_flush_before(v92_dec_t *d, unsigned n, const int32_t *y);

/* Smoothed squared distance between the received values and the decoded
 * path, per symbol, in linear units squared (an estimate of the noise
 * variance). It rises to the scale of the squared point spacing when frame
 * alignment is lost. Not maintained in hard mode. */
uint32_t v92_dec_quality(const v92_dec_t *d);

/* The slicer on its own: index of the constellation point of data frame
 * interval i nearest to r (a tie goes to the lower index). */
int v92_slice(const v92_map_t *m, int i, int32_t r);

#endif
