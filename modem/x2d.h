/* x2 (U.S. Robotics, 1997) as its server, from the client's marker to data
 * mode (docs/history/x2-facts.md, "Phase 3 script", "Records", "Mapping",
 * "Upstream"). It plays the part for x2 that p34d plays for V.90: one
 * received linear sample in, one mu-law octet out, at 8 kHz.
 *
 *   silent: the client's S, S-bar, PP, TRN, J (V.34's, its symbol rate and
 *   carrier from the marker, scrambled with GPA): the equaliser trains ->
 *   our training script (x2tx) from the J on. The client is quiet a while
 *   after its J, and our echo canceller trains then, on our TRN -> the
 *   client's S, S-bar and TRN (4 points, then 16) at our J words; its MP, an
 *   x2 record of 4 words, on 16 points -> our record after the sync frames
 *   -> its MP' and E -> the start segment down, B1 up -> data both ways:
 *   x2map frames down, V.34 up (GPA, expanded shaping, 64 states, non-linear
 *   encoding), through v34up as in p34d.
 *
 * The client's MP decides: the downstream rate index it asks for (only an
 * index we have a bank for, X2D_RATE_MASK, which our record names, with
 * MD = 5 signs), the digital pad it measured (0, 3 or 6 dB), and the
 * upstream rates it can send (the highest, at most 28800, is taken).
 * Anything else ends the call: once the marker has come there is no way
 * back to V.34. So does a client that goes silent, at any point, within the
 * time limits below (all well under 10 s), and one that asks for a retrain
 * in data mode or in its rate change: its Tone A (2400 Hz, with its 1800 Hz
 * guard tone) for more than 50 ms ends the call (X2D_ERR_RETRAIN). What a
 * client's retrain leads to in x2 is not known (x2-facts.md "Unsettled");
 * the U.S. Robotics 56K, left unanswered, sends it for some 17 s and hangs
 * up ("Unable to Retrain").
 *
 * A client's rate change in data mode (x2-facts.md "Rate change"; its S
 * is watched for from our start segment on, its B1's): its S
 * (X2D_RENEG; the data stops up, goes on down), S-bar, a short TRN and its
 * MP, on 4 points (not 16 as at the start-up), checked as at the start-up;
 * after the data frame being sent, our marker, our records in levels B, the start segment at the new rate
 * (x2tx); its E, B1 and data at its new upstream rate -> X2D_DATA. Never a
 * marker of ours before its MP.
 *
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_X2D_H
#define V90D_MODEM_X2D_H
#include <stdint.h>
#include "seqhunt.h"
#include "ec.h"
#include "ecdly.h"
#include "v34up.h"
#include "x2tx.h"
#include "tonedet.h"

typedef int  (*x2d_getbit_fn)(void *user);
typedef void (*x2d_putbit_fn)(void *user, int bit);

/* the downstream rate indices we have level banks for, bit n - 1 for index
 * n: what our record says (word 1), and what the client's MP may ask for */
#define X2D_RATE_MASK X2TX_REC_W1
#define X2D_MAX_ECHO_DELAY 1919     /* the longest echo delay that can be given (as p34d) */
#define X2D_FBUF 128                /* an MP (X2REC_BITS(4)) and then some */

typedef struct {
    int marker;             /* the client's 7 bits (p2ans x2_marker): bit 0 its carrier
                               (1 high), bits 4..6 its symbol rate index (3000 or 3200) */
    int rtd;                /* round-trip delay from Phase 2, samples */
    int echo_delay;         /* as p34d_cfg: samples until our echo starts to come back,
                               0..X2D_MAX_ECHO_DELAY; -1 = not known: measured while the
                               client is quiet during our TRN */
    int max_up_rate;        /* upstream ceiling in bit/s; 0 = what the client's MP allows */
    x2d_getbit_fn getbit;   /* data to send */
    x2d_putbit_fn putbit;   /* data received */
    void *user;
} x2d_cfg;

typedef enum {
    X2D_WAIT_J,     /* silent: the client's Phase 3, to its J */
    X2D_TRAIN,      /* our script up to the sync frames; the client quiet, then its Phase 4 */
    X2D_MP,         /* sync frames until the client's MP has come */
    X2D_REC,        /* our record until the client's E */
    X2D_START,      /* the start segment down, B1 up */
    X2D_DATA,       /* data both ways */
    X2D_RENEG,      /* the client's S in data mode: its rate change, to data again */
    X2D_ENDED       /* silent; error says why */
} x2d_state;

enum {
    X2D_OK = 0,
    X2D_ERR_CFG,        /* a marker with a symbol rate we do not receive */
    X2D_ERR_NO_J,       /* the client's Phase 3 (S ... J) did not come */
    X2D_ERR_NO_S4,      /* nor its Phase 4 */
    X2D_ERR_NO_MP,      /* nor its MP */
    X2D_ERR_RATE,       /* its MP asks for a rate index (or MD) we have no bank for */
    X2D_ERR_PAD,        /* ... or reports a pad we have no levels for */
    X2D_ERR_UP,         /* ... or no upstream rate we can receive */
    X2D_ERR_NO_E,       /* no E after our record */
    X2D_ERR_B1,         /* B1 not received in time, or wrong */
    X2D_ERR_CARRIER,    /* the upstream signal lost in data mode */
    X2D_ERR_RENEG,      /* a rate change of the client's not back in data mode in time */
    X2D_ERR_HANGUP,     /* we cleared down (x2d_cleardown) */
    X2D_ERR_RETRAIN     /* the client asked for a retrain (its Tone A): x2 has none */
};

typedef struct {
    x2d_cfg    cfg;
    x2d_state  state;
    int        error;
    uint32_t   now, t;              /* sample clock; samples in this state (modem/clock.h) */
    uint32_t   t_mark;              /* a deadline's reference: when the current wait began */
    int        sym_rate, car_num, car_den;
    /* transmit */
    x2tx_t     tx;
    /* receive */
    ec_t       ec;
    int        ec_mu;
    ecdly_t    ecd;                 /* what we sent; ecd.delay the echo delay in use */
    sdet_t     sdet;
    v34up_t    up;                  /* the client's V.34: its Phase 3 and 4, its data */
    int        rxmode;
    sres_t     sres;
    /* J and records */
    uint16_t   jsr; int32_t jbits, j_at; int j_type;
    uint8_t    fbuf[X2D_FBUF]; int fn; seqhunt_t sh;
    uint16_t   mp[4], cand[4]; int cand_ok;
    int        got_mp, got_e, e_sent;
    int        index, pad_mode;     /* from the MP */
    int        tx_ready;
    int        down_rate, up_rate;  /* bit/s */
    tonedet_t  tone_a;              /* the client's Tone A (a retrain) in data mode and its rate change */
} x2d_t;

/* Returns 0, or -1 (state X2D_ENDED, X2D_ERR_CFG) */
int      x2d_init(x2d_t *d, const x2d_cfg *cfg);
uint8_t  x2d_tick(x2d_t *d, int16_t rx);
/* bit/s in data mode (and in a rate change), else 0: down is ours */
int      x2d_down_rate(const x2d_t *d);
int      x2d_up_rate(const x2d_t *d);
/* From data mode or a rate change: x2 has no cleardown signal of its own;
 * we go silent and the call ends (X2D_ENDED, X2D_ERR_HANGUP). Returns 0,
 * or -1 in any other state. */
int      x2d_cleardown(x2d_t *d);

#endif
