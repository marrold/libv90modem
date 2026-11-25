/* HDLC bit-level framing for V.42 LAPM (ITU-T V.42 clause 8.1).
 *
 * Flags 01111110, zero-bit insertion and deletion, abort (seven or more
 * contiguous ones), FCS-16 and FCS-32, interframe flag fill, and maximum
 * frame length enforcement. Bit pump only: no callbacks, no allocation, no
 * OS calls. Octets go on the line least significant bit first (8.1.2.2). */
#ifndef V90D_V42_HDLC_H
#define V90D_V42_HDLC_H
#include <stddef.h>
#include <stdint.h>

/* Largest information field any frame may carry (compile-time ceiling for
 * the negotiable N401, V.42 9.2.3; the default N401 is 128). */
#ifndef V42_MAX_N401
#define V42_MAX_N401 256
#endif
/* address + two control octets + information field */
#define V42HDLC_MAX_PAYLOAD (3 + V42_MAX_N401)
/* plus a 32-bit FCS */
#define V42HDLC_MAX_FRAME   (V42HDLC_MAX_PAYLOAD + 4)

/* FCS computation (8.1.1.6). The running value starts at V42_FCSxx_INIT;
 * the transmitted FCS is the ones complement, low octet first. Running the
 * same computation over frame plus FCS leaves the fixed residue. */
#define V42_FCS16_INIT 0xFFFFu
#define V42_FCS16_GOOD 0xF0B8u       /* 0x1D0F of 8.1.1.6.1, x^15 first */
#define V42_FCS32_INIT 0xFFFFFFFFu
#define V42_FCS32_GOOD 0xDEBB20E3u   /* 0xC704DD7B of 8.1.1.6.2, x^31 first */
uint16_t v42_fcs16(uint16_t fcs, const uint8_t *data, size_t len);
uint32_t v42_fcs32(uint32_t fcs, const uint8_t *data, size_t len);

/* ---------------------------------------------------------------- transmit */
typedef struct {
    uint8_t  buf[V42HDLC_MAX_FRAME];
    int      len;            /* octets in buf, FCS included; 0 = no frame */
    int      pos;            /* next octet of buf to send */
    int      in_frame;       /* currently between opening and closing flag */
    uint8_t  cur;            /* octet being shifted out */
    int      nbits;          /* bits of cur still to send */
    int      raw;            /* cur is a flag or abort: no zero insertion */
    int      ones;           /* run of ones sent inside the frame */
    int      flag_done;      /* a flag has gone out since the last frame data */
    int      min_flags;      /* flags still owed before the next frame */
    int      abort_req;
    unsigned long frames, aborts;
} v42hdlc_tx_t;

void v42hdlc_tx_init(v42hdlc_tx_t *t);
/* True when no frame is loaded and the next bit would start a new flag; this
 * is the moment to load the next frame so it follows the flag directly. */
static inline int v42hdlc_tx_need_frame(const v42hdlc_tx_t *t)
{
    return t->len == 0 && !t->in_frame && t->nbits == 0;
}
/* True while a loaded frame has not been completely sent. */
int  v42hdlc_tx_busy(const v42hdlc_tx_t *t);
/* Load one frame (address, control, information); the FCS is appended here.
 * Returns 0, or -1 if a frame is still loaded or len is out of range. */
int  v42hdlc_tx_frame(v42hdlc_tx_t *t, const uint8_t *data, int len, int fcs32);
/* Send at least n more flags before the next frame (8.3.2.1 NOTE 2). */
void v42hdlc_tx_flags(v42hdlc_tx_t *t, int n);
/* Abort the frame in progress (8.1.4); a no-op between frames. */
void v42hdlc_tx_abort(v42hdlc_tx_t *t);
/* Next line bit (0 or 1). Flags fill the time between frames (8.1.5).
 * Inline: the next bit of the octet being sent; every other case (a new
 * octet, a flag, a zero inserted, an abort) is v42hdlc_tx_bit_full's, which
 * is the whole of it. */
int  v42hdlc_tx_bit_full(v42hdlc_tx_t *t);
static inline int v42hdlc_tx_bit(v42hdlc_tx_t *t)
{
    int bit;
    if (t->abort_req || t->nbits == 0 || t->ones == 5) return v42hdlc_tx_bit_full(t);
    bit = t->cur & 1;
    t->cur >>= 1;
    t->nbits--;
    if (!t->raw) {
        if (bit) t->ones++; else t->ones = 0;
    }
    return bit;
}

/* ----------------------------------------------------------------- receive */
enum {
    V42HDLC_FCS16 = 0,       /* accept frames with a good 16-bit FCS */
    V42HDLC_FCS32 = 1,       /* accept frames with a good 32-bit FCS */
    V42HDLC_FCS_EITHER = 2   /* accept either (8.10.2, called DCE before SABME) */
};

/* v42hdlc_rx_bit() results below zero: a frame was discarded (8.1.3, 8.1.4) */
#define V42HDLC_RX_BAD_FCS (-1)
#define V42HDLC_RX_SHORT   (-2)
#define V42HDLC_RX_ABORT   (-3)
#define V42HDLC_RX_LONG    (-4)
#define V42HDLC_RX_ALIGN   (-5)

/* v42hdlc_rx_t.flag_run saturates here: only small counts are ever asked */
#define V42HDLC_FLAG_RUN_MAX 65535

typedef struct {
    uint8_t  buf[V42HDLC_MAX_FRAME];
    int      len;            /* complete octets collected */
    int      max_len;        /* frame length limit, FCS included */
    int      synced;         /* a flag has been seen; collecting */
    int      overflow;       /* frame exceeded max_len */
    uint8_t  cur;
    int      nbits;
    int      ones;           /* run of ones on the line */
    int      fcs_mode;
    int      last_fcs32;     /* FCS type of the frame last returned */
    int      flag_run;       /* consecutive flags with nothing between, up
                                to V42HDLC_FLAG_RUN_MAX (an idle link) */
    int      tent;           /* collecting what follows a run of ones, no flag
                                before it: a frame only if its FCS holds */
    unsigned long good, bad_fcs, runts, aborts, too_long, misaligned;
    unsigned long resynced;  /* of good: frames taken with no opening flag */
} v42hdlc_rx_t;

void v42hdlc_rx_init(v42hdlc_rx_t *r);
void v42hdlc_rx_set_fcs(v42hdlc_rx_t *r, int fcs_mode);
/* Limit the accepted frame length (address + control + information + FCS). */
void v42hdlc_rx_set_max(v42hdlc_rx_t *r, int max_len);
/* Feed one line bit. Returns the length (FCS stripped) of a valid frame now
 * held in r->buf, 0 when nothing completed, or a negative V42HDLC_RX_* code
 * when a frame was discarded. r->buf is only valid until the next call.
 *
 * A frame need not have an opening flag after a run of seven or more ones
 * (an abort, or the line at mark): it is taken when its FCS holds with up to
 * five of those ones as its first bits. A modem's line that stops and starts
 * again (a rate change) can leave the frame that was under way with its
 * flag lost and fill ones before it: a U.S. Robotics x2 client's does. What
 * follows the ones and is not such a frame is dropped and not counted, as it
 * would be while hunting for a flag.
 *
 * Inline: a bit with fewer than five ones before it on the line; every
 * other case (a deleted zero, a flag, an abort, the end of a run of ones)
 * is v42hdlc_rx_bit_full's, which is the whole of it. */
int  v42hdlc_rx_bit_full(v42hdlc_rx_t *r, int bit);

/* for v42hdlc_rx_bit and v42hdlc_rx_bit_full: one more bit in the
 * accumulator, an octet to buf (or overflow) when it is full */
static inline void v42hdlc_rx_collect(v42hdlc_rx_t *r, int bit)
{
    r->cur = (uint8_t)(r->cur | ((bit & 1) << r->nbits));
    if (++r->nbits == 8) {
        if (r->len < r->max_len) r->buf[r->len++] = r->cur;
        else r->overflow = 1;
        r->nbits = 0; r->cur = 0;
    }
}

static inline int v42hdlc_rx_bit(v42hdlc_rx_t *r, int bit)
{
    int ones = r->ones;
    if (ones >= 5) return v42hdlc_rx_bit_full(r, bit);
    r->ones = (bit & 1) ? ones + 1 : 0;
    if (r->synced) v42hdlc_rx_collect(r, bit);
    return 0;
}

#endif
