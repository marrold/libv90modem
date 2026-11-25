/* Test peer: an x2 client (U.S. Robotics, 1997) calling our answerer, written
 * from docs/history/x2-facts.md as the Courier does it (and D-Modem's client,
 * docs/dsp/x2-details.md "The client"): it checks that our pieces fit
 * together; it does not prove x2.
 *
 *   V.8 as a V.34 caller (sim/v8call: CM with V.34, no PCM, the PSTN access
 *   octet), V.34 Phase 2 (sim/p2c) up to the server's L1; marker_ms after
 *   that L1 began, the marker (4d: high carrier, index 6, 3200 baud) in
 *   place of our L1, L2 and INFO1c; 70 ms later V.34's S, S-bar, PP, TRN
 *   and J (16 points) with GPA, at 3200 baud on the high carrier; J until
 *   the server's preamble has been there 150 ms, then quiet; at its J words
 *   (the 20367th octet of its script) S, S-bar, TRN (512 symbols on 4
 *   points, then 16, 5120 in all), our MP until its record has been
 *   decoded, one MP', E, then V.34 data (B1 first; GPA, expanded shaping,
 *   64 states, non-linear encoding) at the highest rate of up_mask.
 *   reneg_ms into data mode, its rate change (docs/history/x2-facts.md
 *   "Rate change"; D-Modem's client, x2-details.md "Our renegotiation"):
 *   S, S-bar, TRN (512 symbols on 4 points), its MP (reneg_index,
 *   reneg_mask) on 4 points (the Courier's) until the server's record has
 *   been decoded, one MP', E (4 points), then V.34 data again (B1 first) at the highest rate of
 *   reneg_mask; the data bits run on from where they stopped. With
 *   reneg_index2, a second one as the first, reneg_ms after its data.
 *   With reneg_up, reneg_ms is counted from its upstream B1's first
 *   symbol, whatever has come down (its S before our start segment).
 *
 * Downstream it takes the server's octets as they leave it (ideal: no
 * analogue line, no equaliser), passes them through the network's digital
 * pad of pad_mode (docs/history/x2-facts.md "Pads"), and decodes the
 * script: the preamble, TRN and J words, the ones, the alignment, the sync
 * frames (levels A), the records, the start segment and the data (the data
 * levels of the MP's index and pad, x2map_unframe; each octet taken as the
 * level whose octet sent arrives as it, x2call_level), each checked against
 * the facts. In a rate change, from its MP on: the server's marker (8
 * frames +++--- with each position at its largest level, one ---+++), its records in levels B
 * from a fresh descrambler and sign, a frame of ones in levels B, 681 at the
 * new rate, then data again; a marker frame before its MP is counted.
 * Test side. */
#ifndef V90D_SIM_X2CALL_H
#define V90D_SIM_X2CALL_H
#include <stdint.h>
#include "v8call.h"
#include "p2c.h"
#include "../modem/dpsk.h"
#include "../modem/v34mod.h"
#include "../modem/x2map.h"
#include "../v34/v34.h"
#include "../v90/v90_scr.h"

/* where the client falls silent for good (cfg.silent; the tests of the
 * server's time limits) */
enum { X2C_SIL_NONE, X2C_SIL_MARKER, X2C_SIL_J, X2C_SIL_MP, X2C_SIL_DATA };

typedef struct {
    int level;              /* nominal transmit RMS, sample units */
    int pad_mode;           /* the network's digital pad on the downstream: 0 none, 1 3 dB,
                               2 6 dB; reported in the MP as measured. 3 and above: reported
                               (no such pad is applied) */
    int max_index;          /* MP word 0 bits 2..5: the downstream rate index asked for */
    uint16_t up_mask;       /* MP word 1: the upstream rates (bit n - 1 for n x 2400 bit/s) */
    int marker_ms;          /* after the server's L1 began; 0 = 170 */
    int silent;             /* X2C_SIL_*: go quiet after the marker, after J, after 4 MPs,
                               or 1 s into data mode */
    int md;                 /* MP word 3's high byte, the downstream signs MD; 0 = 5 */
    int reneg_ms;           /* a rate change this long into data mode; 0 = none */
    int reneg_index;        /* ... its MP's rate index */
    uint16_t reneg_mask;    /* ... its upstream rates (the Courier's 1FFE) */
    int reneg_md;           /* ... its MD; 0 = 5 */
    int reneg_index2;       /* a second rate change, reneg_ms after the first's data, asking
                               for this index (same mask and MD); 0 = none */
    int reneg_whole;        /* its S once the mapping frame under way is out (0: at once,
                               cutting it) */
    int reneg_up;           /* reneg_ms from its upstream data's first symbol (B1), not
                               from data mode both ways: its S before our start is in */
    int  (*getbit)(void *user);
    void (*putbit)(void *user, int bit);
    void *user;
} x2call_cfg;

typedef enum {
    X2C_V8, X2C_P2, X2C_MARKER, X2C_P3, X2C_QUIET, X2C_P4, X2C_MP, X2C_E, X2C_DATA,
    X2C_RENEG,              /* its rate change: from its S to its E (then X2C_E, X2C_DATA) */
    X2C_ENDED
} x2c_state;

/* what the client found in the server's script */
typedef struct {
    int  not_x2;            /* the server's INFO0a had neither bit 23 nor 24 */
    long pre_bad;           /* preamble octets other than the facts' (behind the pad) */
    long trn_bad;           /* TRN octets off level, or descrambled zeros */
    int  jw_good;           /* J words 09B0 (of 99) */
    int  jw_last;           /* the 100th was 099F */
    long ones_bad, align_bad;
    long sync_frames, sync_bad;     /* frames of ones before the first record; others */
    int  rec_good, rec_bad; /* records F33C 7FFF 0000, others (CRC or words) */
    long start_ones, start_bad;     /* the start segment's frames in the data levels */
    long data_frames, data_lost;    /* data frames; ones that did not decode */
    int  data_up;           /* our upstream in data (after B1) */
    /* the rate change */
    long mark_early;        /* marker frames (+++---) before its MP */
    long mark_frames;       /* +++--- frames after its MP */
    int  mark_last;         /* the ---+++ frame came */
    long mark_bad;          /* other frames in the marker */
    int  rr_nak, rr_ack;    /* records 733C / F33C 7FFF 0000 in levels B */
    int  rr_bad, rr_order;  /* others; a 733C after an F33C */
    int  rr_ones;           /* the frames of ones in levels B after them (one each) */
    long rstart_ones, rstart_bad;   /* the 681 frames at the new rate */
} x2call_seen;

typedef struct {
    x2call_cfg   cfg;
    x2c_state    state;
    int32_t      now, t;
    v8call_t     v8;
    p2c_t        p2;
    int32_t      mark_at;           /* when the marker goes out */
    dpsktx_t     mk; uint8_t mkbits[64]; int nmk; int32_t mk_off;
    /* upstream */
    v34mod_t     mod;
    int          sig; int32_t sn;   /* signal and symbol count in it */
    v34_tmod_t   tmod; int32_t g16;
    uint8_t      mpbits[112]; int mpn, mp_count, mp_ack;
    v34_params_t tpar; v34_tx_t vtx; int32_t tx_dg;
    int          up_rate;
    int          quiet;             /* cfg.silent has struck */
    int32_t      data_at;           /* when data mode began (-1 not yet) */
    int32_t      up_at;             /* when its upstream data (B1) began (-1 not yet) */
    int          renegs;            /* rate changes begun */
    int          reneg_due, reneg_mp;   /* time for one; its first MP has gone out */
    int          rr_base;           /* the server's records counted before this rate change */
    int          rr_acked;          /* an F33C has come in this rate change */
    /* downstream */
    int16_t      ra[256];           /* octet received -> levels A octet sent, -1 none */
    int32_t      k;                 /* octets since the preamble began, -1 before */
    int32_t      j_from;            /* our J began (the preamble is looked for from here) */
    v90_scr_t    dscr;              /* GPC descrambler */
    unsigned     jw;                /* the J word being collected */
    x2map_t      map;
    uint8_t      fr[X2MAP_N]; int fpos;
    uint8_t      rb[96]; int rn, rones;     /* a record being collected */
    int          e_sent, in_start, in_data; int32_t start_n, start_len;
    int          rr;                /* the rate change downstream: 1 the marker, 2 the records */
    x2call_seen  seen;
} x2call_t;

void         x2call_init(x2call_t *c, const x2call_cfg *cfg);
/* one octet from the server in (as it left it), one line sample out */
int16_t      x2call_tick(x2call_t *c, uint8_t octet_from_server);
x2c_state    x2call_state(const x2call_t *c);
/* the network's pad on one octet (the facts' model) */
uint8_t      x2call_pad(uint8_t octet, int pad_mode);
/* the level (as received, its sign o's) that the client takes octet o
 * arriving at position pos for, in the data levels of m: the one whose
 * octet sent arrives as o behind m's pad. Two levels of the Quad's mode 1
 * lists (e2, f1) are sent as octets that the pad model delivers one code
 * off them (x2-facts.md "Banks"); a client knows what arrives for each
 * level. o itself when none does (and in levels A). */
uint8_t      x2call_level(const x2map_t *m, int pos, uint8_t o);

#endif
