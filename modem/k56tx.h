/* K56flex's training transmitter, the server's side (docs/history/
 * k56-facts.md "Training down", "Phase 4", "Rate renegotiation"): from
 * silence 1 through R, TRN1A, TRN1B, TRN1C, Js, silence 2, R2, TRN2A,
 * MARK, TRN2B, MP, MP' and E to the data, one mu-law octet a tick.
 *
 * One GPA scrambler, its history and one running sum run from R1 through
 * E; a renegotiation carries them on, a new k56tx_init clears them. The
 * data's own scrambler and encoder (modem/k56enc) start afresh at each
 * data start (Ruling C4), B1 (6 frames of ones) first.
 *
 * TRN2B scrambles the 2-bit value 1 a symbol (a one, then a zero). The
 * even symbols of TRN1C, Js, TRN2A and MARK scramble as many bits as the
 * last TRN2B / MP / E symbol (1 before the first TRN2B), the sign the
 * first: so a first renegotiation's TRN2A and MARK take 2 (k56-facts.md
 * "Training down", corrections of k56 8; D-Modem's training does so).
 *
 * k56tx_segment() is the segment of the octet the next k56tx_tick() sends:
 * the open ends (silence 2 until the client's J, TRN2B until our MP and the
 * grid of 24) are settled before the tick that would send past them, and
 * again in k56tx_got_j / k56tx_set_mp, so a J or MP given before a tick
 * takes effect at that tick. */
#ifndef V90D_MODEM_K56TX_H
#define V90D_MODEM_K56TX_H
#include <stdint.h>
#include "k56enc.h"
#include "v34mp.h"
#include "../v90/v90_scr.h"

typedef enum {
    K56TX_SIL1, K56TX_R1, K56TX_TRN1A, K56TX_TRN1B, K56TX_TRN1C, K56TX_JS,
    K56TX_SIL2, K56TX_R2, K56TX_TRN2A, K56TX_MARK, K56TX_TRN2B, K56TX_MP,
    K56TX_MPA, K56TX_E, K56TX_DATA
} k56tx_seg;

/* segment lengths in symbols */
#define K56TX_SIL1_N      512
#define K56TX_R_N         288
#define K56TX_TRN1A_N     16368
#define K56TX_TRN1B_N     8448
#define K56TX_TRN1C_N     8448
#define K56TX_JS_N        128
#define K56TX_SIL2_MIN    12288   /* then until the J, rounded up to 8 */
#define K56TX_TRN2A_N     16336
#define K56TX_RN_TRN2A_N  2048    /* in a renegotiation */
#define K56TX_MARK_N      32
#define K56TX_TRN2B_MIN   2048    /* then until our MP and the grid */
#define K56TX_MP_N        704     /* MP and MP' each */
#define K56TX_E_N         12
#define K56TX_RN_E_N      24
/* symbols from TRN2B's end to the first data symbol (MP, MP', E) */
#define K56TX_TO_DATA     (2 * K56TX_MP_N + K56TX_E_N)
#define K56TX_RN_TO_DATA  (2 * K56TX_MP_N + K56TX_RN_E_N)

typedef struct {
    k56enc_t   enc;          /* the data encoder, new at each data start */
    v90_scr_t  scr;          /* the training's GPA scrambler */
    v90_scr_t  dscr;         /* the data's GPA scrambler */
    int64_t    sum;          /* the training's running sum */
    uint32_t   hist;         /* the training scrambler's output, newest in bit 0 */
    int32_t    sym;          /* symbols since TRN1B's first (negative before) */
    int32_t    n;            /* symbols sent of the current segment */
    int32_t    len;          /* its length; -1 while open */
    int32_t    alt;          /* the alternating count (TRN1B.., TRN2A..) */
    int32_t    nbit;         /* bits taken of the MP / MP' / E input */
    int      (*getbit)(void *);
    void      *user;
    uint8_t    mpbits[2][V34_MP0_BITS];   /* our MP and MP' */
    uint8_t    frame[K56ENC_N];           /* the data frame being sent */
    uint8_t    nb;           /* bits a symbol of the scrambled segments
                                (TRN1C .. E): 1 from k56tx_init; TRN2B
                                sets 2, MP 2 (1 in a renegotiation), and
                                the next TRN2A and MARK keep it */
    uint8_t    seg;          /* k56tx_seg */
    uint8_t    reneg;        /* this Phase 4 is a renegotiation's */
    uint8_t    reneg_due;    /* k56tx_reneg in mid-frame, or in MP .. E: R2 at the (first) frame's end */
    uint8_t    j_in, mp_in;  /* the client's J, our MP given */
    uint8_t    pad;          /* from the J: TRN2B .. E's pad */
    uint8_t    d_rate, d_pad, d_rbs;      /* k56tx_data's */
    uint8_t    fpos;         /* next octet of frame, K56ENC_N: none */
    uint8_t    b1;           /* B1 frames still to send */
} k56tx_t;

/* from silence 1: the scrambler, its history and the running sum cleared */
void k56tx_init(k56tx_t *t);

/* the client's renegotiation: R2 now, or at the end of the data frame
 * being sent (with our MP, MP' or E going out: of our first data frame,
 * B1's); TRN2A 2048, MP / MP' 1 bit a symbol, E 24; the scrambler,
 * history and running sum go on; a new MP is needed (k56tx_set_mp) */
void k56tx_reneg(k56tx_t *t);

/* the client's J24 octet: pad bits 6-7 (0 none, 1 3 dB, 2 6 dB), robbed
 * symbols bits 0-5. Silence 2 then ends at max(12288, the symbols of it
 * sent, rounded up to 8). Pad code 3 is refused (silence 2 goes on), as is
 * a J after silence 2 or a second one. */
void k56tx_got_j(k56tx_t *t, uint8_t j_octet);

/* our MP (type 0; its ack ignored, MP' is the same with ack 1; one
 * v34_mp_build refuses is ignored). TRN2B then ends
 * once it is at least 2048 long and the data will start on the grid of
 * 24: (k56tx_sym + 1420) % 24 == 0, in a renegotiation + 1432. */
void k56tx_set_mp(k56tx_t *t, const v34_mp_t *mp);

/* the data: rate code 1..13 (30000 + 2000 rate bit/s), pad 0..2, robbed
 * bits (bits 0-5, counted from TRN1B's first symbol), and the source of
 * the data bits (NULL: ones). The encoder is made from these at each data
 * start. */
void k56tx_data(k56tx_t *t, int rate, int pad, uint8_t rbs,
                int (*getbit)(void *), void *user);

/* the next mu-law octet */
uint8_t k56tx_tick(k56tx_t *t);

k56tx_seg k56tx_segment(const k56tx_t *t);
/* symbols sent since TRN1B's first (negative before it; in data, kept
 * modulo a large multiple of 24) */
int32_t k56tx_sym(const k56tx_t *t);
/* the data rate in bit/s in data, else 0 */
int k56tx_rate(const k56tx_t *t);

#endif
