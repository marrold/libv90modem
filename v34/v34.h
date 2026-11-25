/* v90d/v34 - everything between "bits" and "2D constellation points at the
 * symbol rate" for V.34 primary-channel modulation, as used by the V.90
 * analogue modem in the upstream direction (V.90 clauses 6, 8.3, 8.5).
 *
 * Written from ITU-T V.34 (02/98) clauses 7, 8, 9 and 10.1.3. See README.md
 * for the spec ambiguities and the readings chosen.
 *
 * Integer only, no allocation, no globals: every object is a caller-owned
 * struct, any number of instances may coexist.
 *
 * POINT SCALING CONVENTION
 *   A v34_pt_t is a complex number in Q7 "constellation units": the grid of
 *   Figure 5/V.34 has odd-integer coordinates, so point 0 = (1, 1) is
 *   {128, 128}. Everything the encoder emits and everything the decoder
 *   accepts uses this scale (the precoder works in multiples of 2^-7, so Q7
 *   is exact). The only exception is PP, which is a unit-magnitude phasor
 *   and is returned in Q14 (see v34_pp_point).
 *   Mean energies in units^2: S, 4-point TRN / J / CP / E / SCR: 2;
 *   16-point TRN / MP / CP / E / SCR: 10; PP: 1; data mode: v34_params_t
 *   .energy_q8 / 256. v34_gain_q14() gives the factor that brings any of
 *   them to the power of the 4-point signals.
 */
#ifndef V90D_V34_H
#define V90D_V34_H

#include <stdint.h>
#include <stddef.h>

#define V34_Q    7
#define V34_ONE  (1 << V34_Q)

typedef struct { int16_t re, im; } v34_pt_t;     /* Q7 constellation units */

typedef int  (*v34_getbit_fn)(void *user);           /* returns 0 or 1 */
typedef void (*v34_putbit_fn)(void *user, int bit);

/* ---- scrambler, clause 7 ------------------------------------------- */

enum { V34_GPC = 0,      /* call modem:   1 + x^-18 + x^-23 (7-1) */
       V34_GPA = 1 };    /* answer modem: 1 + x^-5  + x^-23 (7-2); the V.90
                            analogue modem uses this one (V.90 6.5) */

/* sr bit k-1 holds the sequence value k bits ago (scrambler: its own
 * output, descrambler: its input); tap is the polynomial's middle term
 * (5 or 18). The one copy of this scrambler: modem/v34train.h (v34scr_*),
 * v90/v90_scr.c and v32/v32_code.c wrap these. */
typedef struct { uint32_t sr; uint8_t tap; } v34_scr_t;

static inline void v34_scr_init(v34_scr_t *s, int poly)    /* state all zero */
{
    s->sr = 0;
    s->tap = (poly == V34_GPA) ? 5 : 18;
}

static inline int v34_scramble(v34_scr_t *s, int bit)
{
    unsigned o = ((unsigned)bit ^ (s->sr >> (s->tap - 1)) ^ (s->sr >> 22)) & 1;
    s->sr = ((s->sr << 1) | o) & 0x7FFFFF;
    return (int)o;
}

static inline int v34_descramble(v34_scr_t *s, int bit)
{
    unsigned i = (unsigned)bit & 1;
    unsigned o = (i ^ (s->sr >> (s->tap - 1)) ^ (s->sr >> 22)) & 1;
    s->sr = ((s->sr << 1) | i) & 0x7FFFFF;
    return (int)o;
}

/* n bits at once, 1 <= n <= 8: the first in time in bit n - 1 of bits, the
 * last in bit 0; the n output bits are returned the same way. The same as
 * n calls of v34_scramble / v34_descramble, state included.
 *
 * Laid below sr (bit 0 the newest), bit p of the n new bits sees the
 * sequence j bits before it at bit p + j of (sr << n) | bits, so all n
 * come from a few shifts of one word. The descrambler's taps are inputs,
 * all known. The scrambler's are its outputs: those before these n come
 * from sr, and where the middle tap (5) falls inside them, o[p] = c[p] ^
 * o[p + tap] is solved by o ^= o >> tap, o ^= o >> 2 tap, ... (the x^-23
 * tap is always older than the n bits). */
static inline unsigned v34_scramble_n(v34_scr_t *s, unsigned bits, int n)
{
    uint32_t m = ((uint32_t)1 << n) - 1, y = s->sr << n;
    uint32_t o = ((uint32_t)bits ^ (y >> s->tap) ^ (y >> 23)) & m;
    int sh;
    for (sh = s->tap; sh < n; sh *= 2) o ^= o >> sh;
    s->sr = (y | o) & 0x7FFFFF;
    return (unsigned)o;
}

static inline unsigned v34_descramble_n(v34_scr_t *s, unsigned bits, int n)
{
    uint32_t m = ((uint32_t)1 << n) - 1, x = (s->sr << n) | ((uint32_t)bits & m);
    s->sr = x & 0x7FFFFF;
    return (unsigned)((x ^ (x >> s->tap) ^ (x >> 23)) & m);
}

/* ---- training signals, 10.1.3 -------------------------------------- */

/* S (10.1.3.7): symbol i of a segment of len symbols. S alternates point 0
 * and point 0 rotated 90 degrees counterclockwise and ENDS on the rotated
 * point. S-bar alternates point 0 rotated 180 and 270 degrees
 * counterclockwise and BEGINS on the 180 degree point. */
v34_pt_t v34_s_point(unsigned i, unsigned len);
v34_pt_t v34_sbar_point(unsigned i);

/* PP (10.1.3.6): PP(i), i = 0..287 (six periods of 48). Unit magnitude,
 * Q14 (1.0 = 16384). */
#define V34_PP_LEN 288
void v34_pp_point(unsigned i, int16_t *re_q14, int16_t *im_q14);
/* PP(i)'s angle in units of 30 degrees (0..11): the one generator of the
 * sequence, which modem/v34train.c's v34_pp() scales its own way */
unsigned v34_pp_phase(unsigned i);

/* TRN (10.1.3.8) and the J-type modulation (10.1.3.3 for 4 points,
 * 10.1.3.9 for 16 points), which V.90 uses for Ja, CP, E and SCR.
 * points = 4: 2 bits per symbol (I1, I2); points = 16: 4 bits per symbol
 * (I1, I2, Q1, Q2). In the packed "bits" arguments and return values bit 0
 * is the first bit in time. */
typedef struct { v34_scr_t scr; uint8_t z; uint8_t nbits; } v34_tmod_t;

void     v34_tmod_init(v34_tmod_t *t, int poly, int points); /* scrambler and
                                         differential encoder zeroed */
void     v34_tmod_set_points(v34_tmod_t *t, int points);     /* keeps state */
v34_pt_t v34_tmod_trn(v34_tmod_t *t);    /* one TRN symbol: scrambled ones,
                                            not differentially encoded. Leaves
                                            the differential encoder at this
                                            symbol's rotation, which is how a
                                            following J-type sequence must be
                                            initialised. */
v34_pt_t v34_tmod_put(v34_tmod_t *t, unsigned bits); /* one J-type symbol:
                                            nbits data bits, scrambled, then
                                            differentially encoded */

typedef struct { v34_scr_t scr; uint8_t z; uint8_t nbits; } v34_tdem_t;

void     v34_tdem_init(v34_tdem_t *t, int poly, int points);
void     v34_tdem_set_points(v34_tdem_t *t, int points);
unsigned v34_tdem_trn(v34_tdem_t *t, v34_pt_t r);  /* TRN symbol -> nbits
                                            descrambled bits (all ones once
                                            the descrambler has 23 bits) */
unsigned v34_tdem_put(v34_tdem_t *t, v34_pt_t r);  /* J-type symbol -> nbits
                                            descrambled data bits */

/* ---- data mode parameters ------------------------------------------ */

enum { V34_TRELLIS_16 = 0, V34_TRELLIS_32 = 1, V34_TRELLIS_64 = 2 };

/* Everything the encoder / decoder pair must agree on. In V.90 the digital
 * modem dictates trellis, nonlinear, expanded and h[] in its MP sequence
 * (V.90 Table 16) and the pair settles data_rate; symbol_rate comes from
 * INFO1a. */
typedef struct {
    uint16_t symbol_rate;    /* 2400, 2743, 2800, 3000, 3200 or 3429 */
    uint16_t data_rate;      /* bit/s, a multiple of 2400 present in Table 8 */
    uint8_t  trellis;        /* V34_TRELLIS_x, same coding as MP bits 29:30 */
    uint8_t  expanded;       /* MP bit 32: 0 minimum, 1 expanded shaping */
    uint8_t  nonlinear;      /* MP bit 31: 0 Theta = 0, 1 Theta = 0.3125 */
    uint8_t  scrambler;      /* V34_GPC / V34_GPA of the TRANSMITTING modem */
    int16_t  h[3][2];        /* precoder h(1..3), {re, im}, 2.14 format as
                                carried in MP Type 1; all zero = no precoding */
} v34_cfg_t;

#define V34_MAX_M      18
#define V34_QPOINTS    416

/* Framing and mapping parameters for one (symbol rate, total rate) pair:
 * Tables 7, 8 and 10 computed from the formulas of 8.2 and 9.2. */
typedef struct {
    uint8_t  J, P;           /* data frames / superframe, mapping frames /
                                data frame */
    uint16_t N;              /* bits per data frame (8-1) */
    uint8_t  b, r;           /* bits in a high mapping frame; high frames per
                                data frame (8-2) */
    uint16_t swp;            /* switching pattern as printed in Table 8 */
    uint8_t  K, q, M;        /* shell mapping bits, uncoded bits per 2D
                                symbol, rings */
    uint16_t L;              /* points in the 2D constellation (9-2) */
    uint8_t  w;              /* precoder quantiser scale (9-29) */
} v34_map_t;

/* total_rate may be any multiple of 200 so that all of Tables 8 and 10 can
 * be checked; returns 0, or -1 if the pair is not in Table 8. */
int v34_map_params(unsigned symbol_rate, unsigned total_rate, int expanded,
                   v34_map_t *out);

typedef struct {
    v34_cfg_t cfg;
    v34_map_t map;
    uint8_t   precode;       /* 1 if any h is non-zero */
    uint8_t   nstates;       /* 16, 32 or 64 */
    uint8_t   ymax;          /* largest |coordinate| the decoder accepts for
                                y(n) = u(n) + c(n) */
    /* shell mapper tables, 9.4 */
    uint8_t   g2[2 * V34_MAX_M - 1];
    uint16_t  g4[4 * V34_MAX_M - 3];
    uint32_t  z8[8 * V34_MAX_M - 6];   /* saturates at 0xFFFFFFFF */
    /* constellation */
    uint16_t  npts;          /* L / 4 */
    uint32_t  energy_q8;     /* nominal mean energy of u(n), units^2 * 256 */
    uint32_t  energy_tx_q8;  /* mean energy of what is sent, x'(n): the same, or with the
                                non-linear encoder about 18 % more. This is the one for
                                v34_gain_q14: the encoder must not raise the line level. */
    uint8_t   inside[(46 * 46 + 7) / 8]; /* bitmap of the L-point
                                constellation over the odd grid -45..45 */
} v34_params_t;

/* Returns 0 on success; -1 bad symbol rate / data rate pair, -2 bad
 * trellis. (-3 used to mean "nonlinear encoding requested": it is
 * implemented now, see v34_nl_encode.) */
int v34_params_init(v34_params_t *p, const v34_cfg_t *cfg);

/* Factor (Q14) to multiply points of mean energy energy_q8 by so that they
 * have the mean energy of the 4-point training signals (2 units^2). */
uint32_t v34_gain_q14(uint32_t energy_q8);

#define V34_ENERGY_4PT_Q8   (2 * 256)
#define V34_ENERGY_16PT_Q8  (10 * 256)

/* ---- constellation helpers ----------------------------------------- */

/* Mapper 9.6.1: quarter-constellation label 0..415 rotated clockwise by
 * rot * 90 degrees. Integer units (not Q7). */
void v34_map_point(unsigned label, unsigned rot, int *re, int *im);
/* Inverse: odd-integer grid point -> label and rotation; returns -1 if the
 * point is not in the 1664-point superconstellation. */
int  v34_unmap_point(int re, int im, unsigned *label, unsigned *rot);
/* 3-bit subset label of Figure 9 for an odd-integer grid point. */
unsigned v34_subset(int re, int im);
/* Nearest point of the L-point data constellation (no trellis, no memory):
 * the plain slicer. */
v34_pt_t v34_slice(const v34_params_t *p, v34_pt_t r);

/* ---- non-linear encoder, 9.7 --------------------------------------- */

/* x'(n) = Phi(n) x(n) of (9-33) when p->cfg.nonlinear is set, else x. Q7 in
 * and out. v34_tx_symbol() applies it by itself; a receiver must apply
 * v34_nl_decode() to its equalised point before v34_rx_put(). */
v34_pt_t v34_nl_encode(const v34_params_t *p, v34_pt_t x);
/* (9-34): Phi in Q16 for a point of energy e (units^2 * 2^14) in a signal of
 * mean energy energy_q8 (units^2 * 256). e >= 0 (every caller passes a sum
 * of squares): a large negative e overflows a signed product inside */
int64_t  v34_nl_phi_q16(uint32_t energy_q8, int64_t e);
v34_pt_t v34_nl_decode(const v34_params_t *p, v34_pt_t xp);

/* ---- shell mapper, 9.4 --------------------------------------------- */

/* the mapper's tables for M rings (1..V34_MAX_M): sets map.M, g2, g4 and
 * z8 alone (v34_params_init calls it; K56flex sets M from its own table) */
void     v34_shell_tables(v34_params_t *p, unsigned M);
void     v34_shell_map(const v34_params_t *p, uint32_t r0, uint8_t ring[8]);
uint32_t v34_shell_unmap(const v34_params_t *p, const uint8_t ring[8]);

/* ---- data mode encoder, clause 9 ----------------------------------- */

typedef struct {
    const v34_params_t *p;
    v34_getbit_fn get;
    void     *user;
    v34_scr_t scr;
    uint8_t   conv;          /* convolutional encoder state */
    uint8_t   z;             /* differential encoder Z(m-1) */
    int32_t   xh[3][2];      /* x(n-1..n-3), Q7 */
    int32_t   pn[2];         /* p(n) for the next symbol, Q7 */
    int32_t   cn[2];         /* c(n) for the next symbol, integer units */
    uint8_t   b1;            /* 1 while generating B1 */
    uint8_t   b1_buf;        /* 1 if the buffered mapping frame is B1 */
    uint8_t   mf;            /* mapping frame within the data frame */
    uint8_t   half;          /* half data frame index within the superframe */
    uint8_t   hm;            /* 4D symbol within the half data frame */
    uint8_t   swc;           /* SWP counter (8.2) */
    uint8_t   pos;           /* next symbol of the buffered mapping frame */
    v34_pt_t  x[8], y[8];
} v34_tx_t;

/* The encoder starts with B1 (10.1.3.1: one data frame of scrambled ones,
 * everything initialised to zero), then pulls data bits from get(). p must
 * outlive tx. */
void v34_tx_init(v34_tx_t *tx, const v34_params_t *p, v34_getbit_fn get,
                 void *user);
/* Next 2D symbol: x(n), the precoded point to modulate (x'(n), non-linear
 * encoded, when the parameters say so). If y is not NULL it
 * receives the channel-output point y(n) = u(n) + c(n) the trellis code is
 * defined on (equal to x when there is no precoding). */
void v34_tx_symbol(v34_tx_t *tx, v34_pt_t *x, v34_pt_t *y);
/* 1 if the next symbol v34_tx_symbol() returns belongs to B1. */
int  v34_tx_in_b1(const v34_tx_t *tx);

/* ---- data mode decoder --------------------------------------------- */

#ifndef V34_VIT_DEPTH
#define V34_VIT_DEPTH 24     /* decision delay in 4D symbols */
#endif
#define V34_MAX_STATES 64

typedef struct {             /* one trellis step of survivor history */
    int16_t pt[2][8][2];     /* nearest point per 2D subset, both symbols */
    uint8_t prev[V34_MAX_STATES];
    uint8_t pair[V34_MAX_STATES]; /* s(2m) << 3 | s(2m+1) of the survivor */
} v34_vstep_t;

#define V34_XD_LEN (2 * V34_VIT_DEPTH + 4)

typedef struct {
    const v34_params_t *p;
    v34_putbit_fn put;
    void     *user;
    v34_scr_t scr;
    /* front end */
    int16_t   xin[3][2];     /* last three inputs, for the whitening filter */
    int32_t   xt[3][2];      /* last three tentative x decisions, Q7 */
    int16_t   xd[V34_XD_LEN][2]; /* inputs delayed to the decision point */
    uint8_t   xmode;         /* 1: fed through v34_rx_put (x domain) */
    uint8_t   k;             /* 0/1: position inside the 4D symbol */
#ifdef V34_TEST_API     /* the layout differs under V34_TEST_API: never link
                           v34/test's objects with the library's */
    uint8_t   hard;          /* 1: trellis decoder bypassed (diagnostic) */
    int16_t   hard_y[2];     /* first point of the 4D symbol in that mode */
#endif
    uint16_t  xd_w, xd_r;    /* write / read positions in xd */
    uint32_t  m1[8];         /* subset metrics of the first 2D symbol */
    /* Viterbi */
    uint8_t   pred[V34_MAX_STATES][16][2]; /* the nkeys branches into each
                                state: { predecessor (ascending), label =
                                input Y4..Y1 << 1 | Y0 of the predecessor } */
    uint8_t   pairidx[64];   /* (s1, s2) -> branch label: Y4..Y1 << 1 | parity */
    uint8_t   nkeys;         /* distinct trellis inputs = branches per state */
    uint8_t   nlab;          /* branch labels are 0 .. nlab - 1 */
    uint8_t   nsub;          /* 2D subsets the code distinguishes: 4 or 8 */
    uint8_t   pairlist[64];  /* the (s1, s2) pairs to combine */
    uint8_t   npairs;
    uint32_t  pm[V34_MAX_STATES];
    v34_vstep_t hist[V34_VIT_DEPTH];
    uint16_t  head;          /* next slot of hist to write */
    uint8_t   best;          /* state with the smallest path metric */
    uint32_t  nsteps;        /* 4D symbols pushed so far */
    uint32_t  nout;          /* 4D symbols decided so far */
    uint8_t   half_in;       /* half data frame the trellis input is in */
    uint16_t  m_in;          /* 4D symbol index within that half frame */
    /* back end (decided symbols) */
    int32_t   xh[3][2];      /* reconstructed x(n-1..n-3), Q7 */
    int32_t   pn[2], cn[2];  /* p(n), c(n) of the next decided symbol */
    uint8_t   z;             /* Z(m-1) */
    uint8_t   j;             /* 4D symbol within the mapping frame */
    uint8_t   mf, swc, b1;
    uint8_t   ring[8];
    uint16_t  qv[8];
    uint8_t   ibits[4];      /* I1 | I2 << 1 | I3 << 2 */
    /* statistics */
    uint32_t  b1_bits, b1_errors;  /* B1 bits received / not equal to 1 */
    uint32_t  bad_points;    /* decided points outside the constellation */
    uint32_t  frames;        /* mapping frames decoded (B1 included) */
    uint32_t  shell_errors;  /* mapping frames whose ring indices are not a
                                shell mapper codeword (index >= 2^K, or
                                S(i,K) = 1 in a low frame): uncorrected
                                errors, or lost mapping frame alignment */
    uint32_t  reseeds;       /* times the inverse precoder was re-anchored
                                on the received signal (precoding only) */
    uint32_t  err_avg;       /* smoothed winning-path metric per 4D symbol,
                                see v34_rx_quality() */
} v34_rx_t;

/* The decoder expects its first input to be the first symbol of B1; the N
 * bits of B1 are checked against ones (b1_bits / b1_errors) and not
 * delivered. p must outlive rx. */
void v34_rx_init(v34_rx_t *rx, const v34_params_t *p, v34_putbit_fn put,
                 void *user);

/* One equalised symbol, Q7, as an estimate of the transmitted x(n) (the
 * output of a linear equaliser, with the nonlinear decoder already applied
 * if used). The decoder applies the noise-whitening filter 1 + sum h(p)D^p
 * itself to obtain y(n). If tent is not NULL it immediately receives the
 * tentative decision for this symbol in the same domain as the input: the
 * nearest valid constellation point when there is no precoding, otherwise
 * the nearest y-grid point minus the precoder's p(n) rebuilt from earlier
 * tentative decisions. Decoded, descrambled data bits come out of put()
 * V34_VIT_DEPTH 4D symbols later, one mapping frame at a time. */
void v34_rx_put(v34_rx_t *rx, v34_pt_t xhat, v34_pt_t *tent);

/* Same, for a receiver that does its own whitening and hands over an
 * estimate of y(n) directly. tent receives the nearest valid y point. */
void v34_rx_put_y(v34_rx_t *rx, v34_pt_t yhat, v34_pt_t *tent);

#ifdef V34_TEST_API     /* v34/test only (v34/Makefile defines it) */
/* Diagnostic: on != 0 bypasses the trellis decoder, every symbol is decided
 * on its own (nearest valid point) with no delay. This is the
 * "symbol-by-symbol slicing" reference the BER test compares against. Call
 * right after v34_rx_init(). */
void v34_rx_set_hard(v34_rx_t *rx, int on);

/* Force out the decisions still inside the Viterbi decoder (end of a test).
 * Only whole mapping frames are delivered. */
void v34_rx_flush(v34_rx_t *rx);
#endif

/* S in data mode (a rate renegotiation, a cleardown): how many of the newest
 * inputs to v34_rx_put() are S, two points 90 degrees apart sent
 * alternately (10.1.3.2); 0 when the newest four are not S, or the inputs
 * came through v34_rx_put_y(). At most the inputs not yet decided: a run
 * that reaches back that far leaves no data to flush. The two points are
 * taken from the newest four, the run's start is where the cumulative
 * match against them is best, on a point 0 (S begins there: 128 symbols
 * ending on point 0 turned 90 degrees anticlockwise). */
unsigned v34_rx_s_run(const v34_rx_t *rx);

/* 1 when the newest two inputs to v34_rx_put() could be S's two points (one
 * size within about 12 %, 90 degrees apart within about 14 degrees): S may
 * have just reached the decoder, too new for v34_rx_s_run() */
int v34_rx_s_begins(const v34_rx_t *rx);

/* Force out the whole mapping frames that end at least one mapping frame
 * (8 symbols) before the n newest inputs (n = v34_rx_s_run(): the data the
 * far end sent before S). The margin covers a start of S found late (its
 * first points turned by the channel): no frame that S cut, or that holds S,
 * is delivered; an S that began on a frame boundary costs the frame before
 * it. The path through them is the survivor that
 * costs least over them, not the one through what followed. Inputs through
 * v34_rx_put() only; with v34_rx_put_y(), or hard decisions, nothing. */
void v34_rx_flush_before(v34_rx_t *rx, unsigned n);

/* Smoothed squared distance between the received points and the decoded
 * path, per 2D symbol, in units^2 * 256 (so it estimates the noise variance
 * per 2D symbol; divide energy_q8 by it for an SNR estimate). It rises to
 * roughly the squared minimum distance scale when symbol or frame alignment
 * is lost; see README "Resynchronisation". */
uint32_t v34_rx_quality(const v34_rx_t *rx);

#endif
