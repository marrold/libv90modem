/* ITU-T V.42 error-correcting endpoint with V.42 bis or V.44 compression.
 *
 * This is the whole layer between the modem's data mode bit stream and an
 * octet stream user (PPP): detection phase (V.42 7.2.1), LAPM over HDLC
 * framing (clause 8), V.44 or V.42 bis (when negotiated), and the buffered
 * start-stop fallback used when the far end has no error correction.
 *
 * It is a pump with no OS calls, no allocation and no floating point. All
 * state lives in the caller's v42ec_t (roughly 50 kB with the default
 * compile-time limits). The caller:
 *
 *   - calls v42ec_tx_bit() once per transmitted line bit and v42ec_rx_bit()
 *     once per received line bit (or the octet-wise forms, 8 bits per
 *     octet, least significant bit first on the line);
 *   - calls v42ec_tick(ms) as time passes (timers T400, T401, T403);
 *   - moves user data with v42ec_write() / v42ec_read(), both of which may
 *     take less than offered (flow control);
 *   - watches v42ec_state().
 *
 * Function names carry the v42ec_ / v42lapm_ / v42hdlc_ / v42comp_ /
 * v42det_ prefixes rather than a bare v42_ so the library can be linked
 * next to other V.42 implementations (the interop tests do that).
 *
 * Behaviour worth knowing:
 *   - Answerer: mark until the ODP, then the ADP "EC" at least ten times and
 *     on until the originator's flags or first frame arrive. Flags or a
 *     frame without any ODP also start the protocol phase. If T400 passes
 *     with neither, the endpoint goes to V42EC_ASYNC (or disconnects with
 *     V42EC_R_NO_EC when cfg.fallback is 0). Characters that arrived during
 *     the detection phase are then delivered to v42ec_read().
 *   - Originator: ODP until two ADPs or the answerer's flags (an answerer
 *     without the detection phase) are seen, or T400 passes; after "EC" it
 *     sends flags until the answerer's flags arrive (at most 600 line bits)
 *     and then XID (if cfg.lapm.use_xid) and SABME.
 *   - Flow control: v42ec_write() takes nothing once the transmit buffer is
 *     full; the far end is held off with RNR when v42ec_read() is not called
 *     and the receive buffer fills. Nothing is ever dropped in error-corrected
 *     mode. In V42EC_ASYNC there is no flow control: receive overruns are
 *     counted in v42ec_status_t.async_overruns.
 *   - Where both ends offer V.44 and V.42 bis, V.44 is used (V.44 7.3);
 *     the two share their storage here, as only one runs on a connection.
 *   - A V.42 bis or V.44 decoder error releases the connection
 *     (V42EC_R_COMPRESSION).
 *   - After V42EC_DISCONNECTED keep calling v42ec_tx_bit() for a few dozen
 *     bits if the line is still up, so a final UA or DM gets out; data
 *     already received stays readable.
 *
 * Typical answer side use:
 *
 *     v42ec_cfg_t cfg; v42ec_t ec;
 *     v42ec_cfg_default(&cfg, 0);          // 0 = answerer
 *     v42ec_init(&ec, &cfg);               // carrier is up: start
 *     for each line bit:  v42ec_rx_bit(&ec, rxbit); txbit = v42ec_tx_bit(&ec);
 *     every millisecond:  v42ec_tick(&ec, 1);
 *     when v42ec_state(&ec) is V42EC_CONNECTED or V42EC_ASYNC:
 *         n = v42ec_write(&ec, data, len);  n = v42ec_read(&ec, buf, max);
 */
#ifndef V90D_V42_H
#define V90D_V42_H
#include <stddef.h>
#include <stdint.h>
#include "v42_hdlc.h"
#include "v42_detect.h"
#include "v42_lapm.h"
#include "v42_comp.h"
#include "v44_comp.h"

/* user data buffers, octets (compile-time) */
#ifndef V42EC_TXBUF
#define V42EC_TXBUF 2048
#endif
#ifndef V42EC_RXBUF
#define V42EC_RXBUF 4096
#endif

enum {
    V42EC_DETECT = 0,       /* detection phase: ODP / ADP exchange */
    V42EC_ESTABLISH,        /* protocol establishment: XID, SABME, UA */
    V42EC_CONNECTED,        /* error-corrected connection up */
    V42EC_ASYNC,            /* fallback: buffered start-stop, no correction */
    V42EC_RELEASING,        /* DISC sent, waiting for the answer */
    V42EC_DISCONNECTED      /* finished; see v42ec_status_t.reason */
};

/* reasons beyond the V42LAPM_R_* values */
#define V42EC_R_NO_EC        16   /* peer has no V.42 and fallback is disabled */
#define V42EC_R_COMPRESSION  17   /* V.42 bis or V.44 decoder reported C-ERROR */
#define V42EC_R_NO_SABME     18   /* answerer: nothing after the detection phase */

typedef struct {
    int originator;         /* 1 = originating DCE, 0 = answering DCE */
    int detect;             /* run the detection phase (7.2.1) */
    int fallback;           /* on detection failure: 1 = async mode, 0 = disconnect */
    int t400_ms;            /* detection phase timer (9.1.1), default 750 */
    int establish_ms;       /* answerer: give up if no SABME arrives this long
                               after the detection phase; 0 = wait for ever */
    int comp_policy;        /* V42COMP_AUTO / _ALWAYS / _NEVER for our encoder
                               (V44COMP_* are the same values) */
    v42lapm_cfg_t lapm;     /* k, N401, V.42 bis, V.44, FCS, T401, T403, N400 */
} v42ec_cfg_t;

/* the compression procedure in use on the connection */
enum { V42EC_COMP_NONE = 0, V42EC_COMP_V42BIS, V42EC_COMP_V44 };

typedef struct {
    int state, reason;
    int k_tx, k_rx, n401_tx, n401_rx;        /* in force (LAPM) */
    int comp_alg;            /* V42EC_COMP_* */
    int comp_tx, comp_rx;    /* compression agreed, our send / receive side */
    int comp_n2, comp_n7;    /* V.42 bis: codewords, string length */
    int v44_n2_tx, v44_n7_tx, v44_n8_tx;     /* V.44, our encoder: codewords,
                                                string length, history */
    int v44_n2_rx, v44_n7_rx, v44_n8_rx;     /* V.44, our decoder */
    int fcs32;
    size_t tx_queued;        /* written, not yet handed to the line */
    size_t rx_avail;         /* ready for v42ec_read() */
    int unacked;             /* I frames awaiting acknowledgement */
    unsigned long rx_frames, rx_frames_bad;  /* HDLC receiver */
    unsigned long tx_i, tx_i_retx, rx_i, rx_i_dropped;
    unsigned long tx_rej, rx_rej, t401_expiries, violations;
    unsigned long comp_in, comp_out;         /* our encoder: characters, octets */
    unsigned long decomp_in, decomp_out;     /* our decoder: octets, characters */
    unsigned long async_overruns, async_framing;
} v42ec_status_t;

typedef struct {
    uint8_t *buf; size_t size, head, len;
} v42ec_ring_t;

typedef struct v42ec {
    v42ec_cfg_t cfg;
    int state, reason;
    int sub;                 /* detection: 0 = hunting, 1 = answerer sending ADP */
    int hdlc_tx_on;          /* transmitter is in HDLC mode (else start-stop) */
    int want_hdlc, want_async;   /* switch at the next pattern boundary */
    int orig_wait;           /* originator: line bits left before the first frame */
    int reason_override;     /* endpoint reason to report instead of LAPM's */
    int enc_dirty;           /* characters encoded since the last flush */
    int adp_strip;           /* async: still dropping "E NUL" repetitions */
    int brk_tx_pend, brk_tx_opt, brk_tx_len;
    size_t brk_tx_after;     /* written octets that go out before the break */
    int t400, t_est;         /* ms left, negative = stopped */

    v42det_rx_t  det_rx;     /* also the start-stop receiver in async mode */
    v42det_tx_t  det_tx;
    v42async_tx_t atx;
    v42hdlc_tx_t htx;
    v42hdlc_rx_t hrx;
    v42lapm_t    lapm;
    union { v42comp_enc_t bis; v44comp_enc_t v44; } enc;    /* the one of comp_alg */
    union { v42comp_dec_t bis; v44comp_dec_t v44; } dec;
    int comp_alg;            /* V42EC_COMP_* */
    int comp_tx, comp_rx;    /* our encoder / decoder is in use */
    unsigned long seen_established, seen_resets;

    v42ec_ring_t txr, rxr;
    uint8_t txbuf[V42EC_TXBUF], rxbuf[V42EC_RXBUF];

    int brk_have, brk_opt, brk_len;
    size_t brk_after;        /* received octets to read before the break */
    unsigned long async_overruns, async_framing;
    /* the line stopped (v42ec_line_stopped) and is not yet back: no new
     * frame is begun; stop_cut: a frame was in flight at the stop, number
     * stop_frames of the transmitter's (htx.frames) */
    int stopped, stop_cut;
    unsigned long stop_frames;
} v42ec_t;

/* Fill cfg with defaults for the given role: detection on, fallback on,
 * T400 750 ms, k 15, N401 128, XID used, V.42 bis offered both ways with
 * P1 = min(2048, V42COMP_MAX_CODEWORDS) and P2 = 32, V.44 offered both
 * ways with min(2048, V44COMP_MAX_CODEWORDS) codewords, strings of 255 and
 * a history of min(4096, V44COMP_MAX_HISTORY), 16-bit FCS,
 * T401 1000 ms, N400 10, T403 off. */
void   v42ec_cfg_default(v42ec_cfg_t *cfg, int originator);
/* Start. Call when the modem reports its data mode is up (7.2). */
void   v42ec_init(v42ec_t *e, const v42ec_cfg_t *cfg);

/* Line side, one bit at a time. */
int    v42ec_tx_bit(v42ec_t *e);
void   v42ec_rx_bit(v42ec_t *e, int bit);
/* Line side, octets of 8 line bits, least significant bit first. */
void   v42ec_tx_octets(v42ec_t *e, uint8_t *buf, size_t n);
void   v42ec_rx_octets(v42ec_t *e, const uint8_t *buf, size_t n);

/* Time. */
void   v42ec_tick(v42ec_t *e, unsigned ms);
/* The modem has stopped carrying data (a retrain, a rate renegotiation;
 * V90M_EV_DATA_STOPPED): the frame in flight, if any, is noted, and no new
 * frame is begun until the line is back. Optional: without it
 * v42ec_line_restored() only polls. */
void   v42ec_line_stopped(v42ec_t *e);
/* The modem carries data again (V90M_EV_DATA_MOVING). After
 * v42ec_line_stopped(): the frame that was in flight at the stop is
 * aborted if it is still not out (its head went before the stop, its tail
 * would come after: the far end cannot take it), and the next frame is
 * opened by two whole flags (an x2 rate change resumes after fill ones,
 * with our last flag cut); frames begun later are not touched. In any case
 * LAPM polls if frames are unacknowledged (v42lapm_line_restored()). */
void   v42ec_line_restored(v42ec_t *e);

/* User side. v42ec_write() returns how many octets were taken (0..len);
 * data written before the connection is up is held. v42ec_read() returns
 * how many octets were delivered (0..max). */
size_t v42ec_write(v42ec_t *e, const uint8_t *data, size_t len);
size_t v42ec_write_space(const v42ec_t *e);
size_t v42ec_read(v42ec_t *e, uint8_t *data, size_t max);
size_t v42ec_read_avail(const v42ec_t *e);
/* True when everything written has been sent and, in error-corrected
 * mode, acknowledged by the far end. */
int    v42ec_tx_idle(const v42ec_t *e);

/* Orderly release (7.7): DISC is sent; unsent data is discarded, so wait
 * for v42ec_tx_idle() first if it matters. In async mode it just stops. */
void   v42ec_release(v42ec_t *e);
/* The line is gone: stop at once, without a word to the far end, in every
 * state (V42EC_DISCONNECTED with V42LAPM_R_LOCAL on return). A UA or DM
 * already owed may still go out while v42ec_tx_bit() is called; nothing
 * else does, and data already received stays readable. */
void   v42ec_abort(v42ec_t *e);

/* Break (7.4, 7.5): opt is one of V42_BRK_*, len10ms the length in units of
 * 10 ms (0 = default). Send returns 0, or -1 if not possible now. Get
 * returns 1 once per received break, in sequence with v42ec_read() data for
 * the in-sequence kind. */
int    v42ec_send_break(v42ec_t *e, int opt, int len10ms);
int    v42ec_get_break(v42ec_t *e, int *opt, int *len10ms);

int    v42ec_state(const v42ec_t *e);
void   v42ec_get_status(const v42ec_t *e, v42ec_status_t *st);

#endif
