/* V.22bis / V.22 modem: 600 baud QAM, 1200 bit/s (4 points) or 2400 bit/s
 * (16 points), FDM duplex: caller transmits on 1200 Hz, answerer on 2400 Hz.
 * Fixed-point, sample-driven, no allocation. */
#ifndef V90D_MODEM_V22BIS_H
#define V90D_MODEM_V22BIS_H
#include <stdint.h>
#include "scramble.h"

#define V22_FC_LOW  1200
#define V22_FC_HIGH 2400

/* ---- line coding (V.22bis 2.5.2) ---- */
/* Advance *quadrant (0..3 = phase quadrants 1..4) by the dibit `d` (first bit
 * in bit 1) and return the point selected by `e` (the last two bits of a
 * quadbit; 1200 bit/s always uses e = 01). Coordinates are odd integers. */
void v22_encode(int *quadrant, int d, int e, int *x, int *y);

/* ---- transmitter ---- */
typedef enum {
    V22_TX_SILENCE,
    V22_TX_USB1,        /* unscrambled binary 1 at 1200 bit/s */
    V22_TX_S1,          /* unscrambled double dibit 00, 11 at 1200 bit/s */
    V22_TX_SB1_1200,    /* scrambled binary 1 at 1200 bit/s */
    V22_TX_SB1_2400,
    V22_TX_DATA_1200,   /* scrambled user data */
    V22_TX_DATA_2400
} v22_txmode;

typedef int  (*v22_getbit_fn)(void *user);
typedef void (*v22_putbit_fn)(void *user, int bit);

typedef struct {
    v22_txmode mode;
    int32_t  si[6], sq[6];     /* symbol history, newest first, pre-scaled */
    int      acc;              /* pulse phase 0..39 within the symbol */
    uint32_t cph, dph;         /* carrier */
    int32_t  gain;             /* symbol unit gain */
    int      quadrant, s1_flip;
    scr_t    scr;
    v22_getbit_fn getbit; void *user;
    uint32_t nsym;             /* symbols sent in the current mode */
    int      fc, level;
    uint32_t gph, gdph;        /* guard tone oscillator */
    int32_t  gamp;             /* guard tone peak amplitude (0: no guard tone) */
} v22tx_t;

/* level = RMS amplitude of the line signal (16-bit sample units) */
void    v22tx_init(v22tx_t *t, int fc, int level, v22_getbit_fn getbit, void *user);
void    v22tx_set_mode(v22tx_t *t, v22_txmode mode);   /* takes effect at the next symbol */
int16_t v22tx_tick(v22tx_t *t);
/* Guard tone (2.1, 2.2): hz = 1800 (6 dB below the data signal) or 550 (3 dB
 * below), 0 = none. Only a high-channel transmitter sends it; for the low
 * channel the call does nothing. The total line level stays at `level`, so
 * the data signal drops by about 1 dB (1800 Hz) or 1.8 dB (550 Hz). */
void    v22tx_set_guard(v22tx_t *t, int hz);

/* ---- receiver ---- */
typedef enum {
    V22_SIG_NONE,       /* no carrier */
    V22_SIG_USB1,       /* unscrambled binary 1 (steady -90 deg per symbol) */
    V22_SIG_S1,         /* unscrambled double dibit 00,11 (period two symbols) */
    V22_SIG_OTHER       /* carrier with scrambled / random data */
} v22_sig;

#define V22_EQN 15      /* T/2-spaced equaliser taps */
#define V22_UNIT 1024   /* constellation unit after AGC */
#define V22_TIMING_SIGN (-1)

typedef struct {
    const int16_t (*bank)[67][2];
    int16_t  ring[128]; int rpos;
    int32_t  tnext, t_integ;       /* next output instant, 1/(24*256) sample */
    uint32_t cph, dph;
    int      half;                 /* 1 = the next output is an on-time sample */
    int32_t  gain;                 /* AGC, Q12 */
    int64_t  pw_raw, pw_on;        /* smoothed powers (raw, and on-time after AGC) */
    int      carrier, agc_count; int64_t agc_acc;
    int32_t  hr[4], hi[4]; int hpos;   /* raw half-symbol history for the 2T lag */
    int64_t  c2r, e2;
    v22_sig  sig, sig_cand; int sig_count;
    int32_t  xr[V22_EQN], xi[V22_EQN];   /* equaliser delay line, newest first */
    int32_t  wr[V22_EQN], wi[V22_EQN];   /* taps, Q24 */
    uint32_t theta; int32_t freq;        /* carrier loop */
    int32_t  mse_slow; int gated;        /* burst gate: long-term error, symbols held */
    int32_t  on_r, on_i, mid_r, mid_i;   /* timing detector memory */
    int      mode16, quadrant;
    scr_t    dscr;
    int      ones_run, zeros_run;  /* consecutive descrambled ONEs / ZEROs */
    uint32_t nsym;                 /* symbols since carrier came up */
    uint32_t ndata;                /* symbols since scrambled data was first seen */
    int32_t  mse;                  /* smoothed |error|^2, 4096 = one V22_UNIT^2 */
    int32_t  zr, zi;               /* last equalised, derotated point (diagnostic) */
    v22_putbit_fn putbit; void *user;
    int      data_on;              /* deliver descrambled bits to putbit */
    int      clamp;                /* circuit 104 clamped: deliver binary 1, at the 2400 bit/s clock */
    uint8_t  pend[64]; int p_head, p_n;   /* 2400 bit/s data on its way up, see V22_RX_HOLD */
    int      dx[2], dy[2];         /* the last two decisions */
    int      rep, brk, susp;       /* symbols repeating with period two: count, breaks, "this may be S1" */
    int      s1_strong;            /* half symbols in a row with the S1 correlation above 7/8 */
    int      sig_age;              /* half symbols since sig last changed */
    int32_t  mse_long;             /* |error|^2 smoothed over about 500 symbols, same unit as mse */
    /* line quality measured on a received S1 (see v22rx_s1_snr) */
    int64_t  q_cur[3], q_prev[3], q_tot[3];   /* lag product (re, im) and power */
    int      q_n, q_have_prev, q_chunks;
} v22rx_t;

void v22rx_init(v22rx_t *r, int fc, v22_putbit_fn putbit, void *user);
void v22rx_tick(v22rx_t *r, int16_t sample);
/* Signal-to-noise ratio (a power ratio, not dB; at most 100000) in the
 * receive band, measured on the S1 last received: S1 repeats every two
 * symbols, so what does not repeat is noise. -1 if S1 was too short. */
int  v22rx_s1_snr(const v22rx_t *r);
/* A retrain request (S1) from the far end takes about 40 ms to recognise for
 * certain (v22rx_s1_certain), and until then it decodes as plausible
 * 16-point data. To keep that from the DTE, received data at 2400 bit/s is
 * delivered V22_RX_HOLD bits (13 ms) late, and as soon as four symbols in a
 * row have repeated the one two before (which S1 does from its third symbol,
 * and random data once in 65000 symbols) delivery stops and the bits queue
 * up. They are thrown away if it was S1 (v22rx_clamp), and delivered, merely
 * late, if it was not. The queue is also delivered when the carrier drops. */
#define V22_RX_HOLD 32
/* S1 beyond doubt: classified as S1 and the two-symbol correlation above 7/8
 * for 8 half symbols running. Random data now and then correlates enough
 * (0.6) to be classified S1 for a few milliseconds: measured about once in
 * 90 s at 1200 bit/s, rarer at 2400; it was never seen above 0.8. Start-up
 * can live with that, data mode cannot retrain on it. */
int  v22rx_s1_certain(const v22rx_t *r);
/* Clamp circuit 104 to binary 1 (and drop the held bits), or release it. */
void v22rx_clamp(v22rx_t *r, int on);
/* Prepare for a retrain (V.22bis 6.4): four-point decisions until told
 * otherwise. `lost` != 0 also forgets the carrier and clock frequency
 * estimates, for a receiver that has lost equalisation. */
void v22rx_retrain(v22rx_t *r, int lost);
/* received line level, RMS in 16-bit sample units (0 when no carrier) */
int  v22rx_level(const v22rx_t *r);

/* ---- complete modem: handshake (V.22bis 6.3.1) + data ----
 * (v22m_ prefix: spandsp, linked in as an interop test peer, owns v22bis_*) */
typedef enum { V22_CALLER, V22_ANSWERER } v22_role;

typedef enum {
    V22S_WAIT,        /* caller: silent, waiting for unscrambled ones; answerer: sending them */
    V22S_SILENT,      /* caller: the 456 ms pause */
    V22S_TRAIN,       /* exchanging S1 / scrambled ones */
    V22S_S1_RX,       /* S1 being received, waiting for its end */
    V22S_UP_2400,     /* S1 seen from the far end: timed switch to 2400 */
    V22S_UP_1200,     /* 1200 bit/s agreed: timed entry to data */
    V22S_DATA,
    V22S_FAILED,
    V22S_RT_WAIT      /* retrain signal sent, waiting for S1 from the far end (6.4) */
} v22_state;

typedef struct {
    v22_role  role;
    int       allow2400;
    v22tx_t   tx;
    v22rx_t   rx;
    v22_state state;
    int32_t   t;            /* samples in the current state (saturates) */
    int32_t   t112;         /* samples since circuit 112 turned on (-1: not yet) */
    int32_t   s1_tx;        /* samples of S1 still to send */
    int32_t   seen;         /* how long the awaited signal has been present */
    int       bitrate;      /* 0 until agreed, then 1200 or 2400 */
    int       tx_ready, rx_ready;   /* circuit 106 follows 105 / circuit 104 not clamped */
    /* retrain (6.4) */
    int       retraining;   /* a retrain is in progress (circuits 107 and 109 stay ON) */
    int       rt_sent;      /* our S1 of this retrain has been sent */
    int32_t   t_s1;         /* samples since we last began sending S1 in a retrain */
    int       rt_tries;     /* retrain signals sent since data mode was last reached */
    int       retrains;     /* retrains completed */
    int       rt_lost;      /* of those we started, how many because equalisation was lost */
    int       s1_snr;       /* answerer: v22rx_s1_snr() of the caller's S1 at start-up */
    int       low_snr_1200; /* answerer: chose 1200 bit/s because of it */
} v22bis_t;

void    v22m_init(v22bis_t *m, v22_role role, int allow2400, int level,
                    v22_getbit_fn getbit, v22_putbit_fn putbit, void *user);
int16_t v22m_tick(v22bis_t *m, int16_t rx);
/* Start a retrain (6.4) now; only acts in data mode at 2400 bit/s. The modem
 * also starts one by itself when its receiver has lost equalisation, and
 * answers one started by the far end. While tx_ready is 0 getbit is not
 * called; while rx_ready is 0 putbit is given binary 1. */
void    v22m_retrain(v22bis_t *m);
/* Guard tone (2.1, 2.2) sent with the high channel: hz = 1800, 550, or 0 for
 * none (the default). Only the modem transmitting in the high channel (the
 * answerer) sends it. Call right after v22m_init. */
void    v22m_set_guard(v22bis_t *m, int hz);

#endif
