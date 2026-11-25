/* V.92 6.4: the analogue modem's data mode transmitter, as a bit-exact model
 * of everything the Recommendation defines: scrambler (6.3), modulus encoder
 * (6.4.1), equivalence classes, precoder filter and prefilter (6.4.2),
 * inverse map (6.4.3), convolutional encoder (6.4.4) and the framing of
 * Figure 1. The digital modem never runs this; it is here for the simulator
 * and because it defines what the decoder (v92_dec.h) must undo.
 *
 * What the Recommendation leaves open, and what this model does:
 *  - Which point u(n) of the equivalence class E(Ki) is sent. The NOTE in
 *    8.8.3 tells the digital modem to assume the analogue modem "minimizes
 *    the power at the precoder output on a symbol-by-symbol basis". Default
 *    here: the member that minimises |x(n)|, a tie going to the lower index.
 *    v92_enc_set_pick() replaces the rule (the decoder accepts any member).
 *  - The precision of x(n) and v(n). Here both are kept with 8 fractional
 *    bits, each sum being rounded to nearest (half away from zero) once.
 *
 * Output scale: the return value of v92_enc_symbol() is G * v(n) in Q14,
 * where 1.0 (16384) is the level at which the signal has the transmit power
 * the digital modem designed for (8.8.3: "when the prefilter output
 * multiplied by G has a mean-square value of 1, the analogue modem will
 * transmit at the desired power").
 */
#ifndef V90D_V92_ENC_H
#define V90D_V92_ENC_H
#include "v92.h"
#include "v92_mod.h"
#include "../v90/v90_scr.h"

#define V92_XQ 8                /* fractional bits of x(n) and v(n) */

/* One transmitted symbol, for tests and simulators. */
typedef struct {
    uint8_t i;                  /* data frame interval, 0..11 */
    uint8_t ki;                 /* modulus encoder output Ki */
    uint8_t y0;                 /* Y0 of this trellis frame */
    int16_t eta;                /* y(n): index of u(n), -LC .. LC - 1 */
    int32_t u;                  /* u(n) = a(eta), linear */
    int32_t x;                  /* x(n), linear * 2^V92_XQ */
    int32_t v;                  /* v(n), linear * 2^V92_XQ */
} v92_sym_t;

/* Chooses u(n): eta[0..n-1] are the indices of the members of the
 * equivalence class in ascending order (n >= 1), i is the data frame
 * interval; returns the position (0..n-1) of the member to send. */
typedef int (*v92_pick_fn)(void *user, int i, const int16_t *eta, int n);

typedef struct {
    const v92_map_t  *map;
    const v92_filt_t *filt;     /* NULL: precoder and prefilter bypassed */
    v92_getbit_fn get;
    void    *user;
    v92_pick_fn pick;
    void    *pick_user;
    v90_scr_t scr;              /* GPA (6.3) */
    v92_mod_t mod;
    uint8_t  conv;              /* convolutional encoder state (v34_conv_next) */
    uint8_t  y0;                /* Y0 of the current trellis frame */
    uint8_t  i;                 /* data frame interval of the next symbol */
    uint8_t  forced;            /* 1: kf holds the Ki of the next data frame */
    uint8_t  ki[V92_DFRAME];
    uint8_t  kf[V92_DFRAME];
    int16_t  eta[V92_TFRAME];   /* y(0..3) of the current trellis frame */
    uint16_t hp;                /* next write position in the histories */
    int16_t  uh[V92_MAX_LSEC];  /* u(n - 1), ... (circular) */
    int32_t  xh[V92_MAX_LSEC];  /* x(n - 1), ... */
    int32_t  vh[V92_MAX_LSEC];  /* v(n - 1), ... */
} v92_enc_t;

/* Everything initialised to zero (8.7.1: "the scrambler, modulus encoder,
 * convolutional encoder, precoder and prefilter memories are initialized to
 * zero prior to transmitting B1u"); the first symbol returned is data frame
 * interval 0 and n = 0 of the filter equations. B1u is then 48 data frames
 * (V92_B1U_FRAMES) of binary ones from get(), and data follows without any
 * re-initialisation.
 *
 * map and filt must outlive the encoder, which reads them at every symbol,
 * unchecked: a map or filter that fails v92_map_check / v92_filt_check
 * makes it divide by zero or read out of bounds. An owner whose map or
 * filter may change (a CPd received later) gives the encoder copies that
 * only it writes, taken when it calls this (v90a: encm, encf).
 * Returns 0, or V92_ERR_RANGE if v92_map_check() or v92_filt_check() fail. */
int  v92_enc_init(v92_enc_t *e, const v92_map_t *map, const v92_filt_t *filt,
                  v92_getbit_fn get, void *user);

#define V92_B1U_FRAMES 48       /* 8.7.1 */

/* Replace the rule that chooses u(n) within E(Ki); NULL restores the
 * minimum |x(n)| rule. */
void v92_enc_set_pick(v92_enc_t *e, v92_pick_fn fn, void *user);

/* Use k[0..11] as the modulus encoder outputs of the next data frame instead
 * of scrambled data (RM and RM' of 8.7.4, which are "trellis encoded" but
 * defined by their Ki). Call before the first symbol of that frame; get() is
 * not called for it and the scrambler and d(f) do not move. */
void v92_enc_force_k(v92_enc_t *e, const uint8_t k[V92_DFRAME]);

/* The next symbol. Returns G * v(n) in Q14 (see above), or u(n) itself when
 * the encoder was initialised without filters. s, if not NULL, receives the
 * intermediate values. */
int32_t v92_enc_symbol(v92_enc_t *e, v92_sym_t *s);

/* 6.4.3 inverse map: Y4 << 3 | Y3 << 2 | Y2 << 1 | Y1 for the trellis frame
 * with indices y[0..3]: Table 13/V.34 of the Figure 9/V.34 labels of the 2D
 * points (2 y(0) + 1, 2 y(1) + 1) and (2 y(2) + 1, 2 y(3) + 1). */
unsigned v92_inverse_map(const int16_t y[V92_TFRAME]);

/* Figure 9/V.34 label (0..7) of the 2D point (2 a + 1, 2 b + 1). */
unsigned v92_subset(int a, int b);

/* Tables 25 and 26: the Ki of one period of RM (prime = 0) or RM' (prime
 * != 0) for moduli M. Table 25 prints "u11 = 0" for interval 11 of RM; it
 * is read as K11 = 0. */
void v92_rm_k(const uint8_t M[V92_DFRAME], int prime, uint8_t k[V92_DFRAME]);

#endif
