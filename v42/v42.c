/* V.42 endpoint: detection phase, LAPM over HDLC, V.42 bis, async fallback.
 * See v42.h. Clause numbers refer to ITU-T V.42 (03/2002) unless they say
 * V.42 bis. */
#include <string.h>
#include "v42.h"

#define PREAMBLE_FLAGS 16    /* flags sent ahead of the first frame (8.3.2.1 NOTE 2) */
#define FLAGS_SEEN     3     /* "continuous flags" that start the protocol phase */
#define ADP_MIN        10    /* 7.2.1.3: ADP at least ten times */
/* Originator, after the ADP: line bits of flags to send while the answerer
 * may still be finishing its ten ADPs and not yet listening for frames. Cut
 * short as soon as its flags arrive. Ten ADPs are at most 520 bits. */
#define ORIG_WAIT_BITS 600

/* ------------------------------------------------------------------ rings */
static void ring_reset(v42ec_ring_t *r) { r->head = r->len = 0; }
static size_t ring_free(const v42ec_ring_t *r) { return r->size - r->len; }

static size_t ring_put(v42ec_ring_t *r, const uint8_t *p, size_t n)
{
    size_t i, w;
    if (n > ring_free(r)) n = ring_free(r);
    w = (r->head + r->len) % r->size;
    for (i = 0; i < n; i++) {
        r->buf[w] = p[i];
        if (++w == r->size) w = 0;
    }
    r->len += n;
    return n;
}

/* longest contiguous run at the head */
static const uint8_t *ring_peek(const v42ec_ring_t *r, size_t *n)
{
    size_t c = r->size - r->head;
    *n = r->len < c ? r->len : c;
    return r->buf + r->head;
}

static void ring_drop(v42ec_ring_t *r, size_t n)
{
    r->head = (r->head + n) % r->size;
    r->len -= n;
    if (r->len == 0) r->head = 0;
}

static size_t ring_get(v42ec_ring_t *r, uint8_t *p, size_t n)
{
    size_t done = 0;
    if (n > r->len) n = r->len;
    while (done < n) {
        size_t c; const uint8_t *s = ring_peek(r, &c);
        if (c > n - done) c = n - done;
        memcpy(p + done, s, c);
        ring_drop(r, c);
        done += c;
    }
    return n;
}

/* ------------------------------------------------------------------ setup */
void v42ec_cfg_default(v42ec_cfg_t *cfg, int originator)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->originator = originator ? 1 : 0;
    cfg->detect = 1;
    cfg->fallback = 1;
    cfg->t400_ms = 750;
    cfg->establish_ms = 30000;
    cfg->comp_policy = V42COMP_AUTO;
    v42lapm_cfg_default(&cfg->lapm);
    cfg->lapm.comp_tx = cfg->lapm.comp_rx = 1;
    cfg->lapm.comp_p1 = V42COMP_MAX_CODEWORDS < 2048 ? V42COMP_MAX_CODEWORDS : 2048;
    cfg->lapm.comp_p2 = 32;
    cfg->lapm.v44_tx = cfg->lapm.v44_rx = 1;
    cfg->lapm.v44_p1_tx = cfg->lapm.v44_p1_rx = V44COMP_MAX_CODEWORDS < 2048 ? V44COMP_MAX_CODEWORDS : 2048;
    cfg->lapm.v44_p2_tx = cfg->lapm.v44_p2_rx = V44COMP_MAX_STRING;
    cfg->lapm.v44_p3_tx = cfg->lapm.v44_p3_rx = V44COMP_MAX_HISTORY < 4096 ? V44COMP_MAX_HISTORY : 4096;
}

/* --------------------------------------------- the compression procedures */
/* What the endpoint asks of the procedure in use (comp_alg): one entry for
 * V.42 bis and one for V.44, each on its member of the enc and dec unions */
typedef struct {
    int    (*enc_init)(v42ec_t *e);     /* C-INIT, our encoder, and its policy */
    int    (*dec_init)(v42ec_t *e);     /* C-INIT, our decoder */
    size_t (*encode)(v42ec_t *e, const uint8_t *in, size_t inlen,
                     uint8_t *out, size_t outmax, size_t *outlen);
    int    (*flush)(v42ec_t *e, uint8_t *out, size_t outmax, size_t *outlen);
    size_t (*enc_pending)(const v42ec_t *e);
    /* receive room a decode needs: V.42 bis one string (P2), V.44 any */
    size_t (*dec_room)(const v42ec_t *e);
    int    (*decode)(v42ec_t *e, const uint8_t *in, size_t inlen, size_t *used,
                     uint8_t *out, size_t outmax, size_t *outlen);
    int    (*dec_pending)(const v42ec_t *e);
    void   (*enc_counts)(const v42ec_t *e, unsigned long *in, unsigned long *out);
    void   (*dec_counts)(const v42ec_t *e, unsigned long *in, unsigned long *out);
} comp_ops_t;

/* V.42 bis */
static int bis_enc_init(v42ec_t *e)
{
    if (v42comp_enc_init(&e->enc.bis, e->lapm.comp_n2, e->lapm.comp_n7) != 0) return -1;
    v42comp_enc_policy(&e->enc.bis, e->cfg.comp_policy);
    return 0;
}
static int bis_dec_init(v42ec_t *e)
{
    return v42comp_dec_init(&e->dec.bis, e->lapm.comp_n2, e->lapm.comp_n7);
}
static size_t bis_encode(v42ec_t *e, const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, size_t *outlen)
{
    return v42comp_encode(&e->enc.bis, in, inlen, out, outmax, outlen);
}
static int bis_flush(v42ec_t *e, uint8_t *out, size_t outmax, size_t *outlen)
{
    return v42comp_flush(&e->enc.bis, out, outmax, outlen);
}
static size_t bis_enc_pending(const v42ec_t *e) { return v42comp_enc_pending(&e->enc.bis); }
static size_t bis_dec_room(const v42ec_t *e) { return e->dec.bis.d.n7; }
static int bis_decode(v42ec_t *e, const uint8_t *in, size_t inlen, size_t *used,
                      uint8_t *out, size_t outmax, size_t *outlen)
{
    return v42comp_decode(&e->dec.bis, in, inlen, used, out, outmax, outlen);
}
static int bis_dec_pending(const v42ec_t *e) { return v42comp_dec_pending(&e->dec.bis); }
static void bis_enc_counts(const v42ec_t *e, unsigned long *in, unsigned long *out)
{
    *in = e->enc.bis.in_chars; *out = e->enc.bis.out_octets;
}
static void bis_dec_counts(const v42ec_t *e, unsigned long *in, unsigned long *out)
{
    *in = e->dec.bis.in_octets; *out = e->dec.bis.out_chars;
}

/* V.44 */
static int v44_enc_init(v42ec_t *e)
{
    const v42lapm_t *l = &e->lapm;
    if (v44comp_enc_init(&e->enc.v44, l->v44_n2_tx, l->v44_n7_tx, l->v44_n8_tx) != 0) return -1;
    v44comp_enc_policy(&e->enc.v44, e->cfg.comp_policy);
    return 0;
}
static int v44_dec_init(v42ec_t *e)
{
    const v42lapm_t *l = &e->lapm;
    return v44comp_dec_init(&e->dec.v44, l->v44_n2_rx, l->v44_n7_rx, l->v44_n8_rx);
}
static size_t v44_encode(v42ec_t *e, const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, size_t *outlen)
{
    return v44comp_encode(&e->enc.v44, in, inlen, out, outmax, outlen);
}
static int v44_flush(v42ec_t *e, uint8_t *out, size_t outmax, size_t *outlen)
{
    return v44comp_flush(&e->enc.v44, out, outmax, outlen);
}
static size_t v44_enc_pending(const v42ec_t *e) { return v44comp_enc_pending(&e->enc.v44); }
static size_t v44_dec_room(const v42ec_t *e) { (void)e; return 1; }
static int v44_decode(v42ec_t *e, const uint8_t *in, size_t inlen, size_t *used,
                      uint8_t *out, size_t outmax, size_t *outlen)
{
    return v44comp_decode(&e->dec.v44, in, inlen, used, out, outmax, outlen);
}
static int v44_dec_pending(const v42ec_t *e) { return v44comp_dec_pending(&e->dec.v44); }
static void v44_enc_counts(const v42ec_t *e, unsigned long *in, unsigned long *out)
{
    *in = e->enc.v44.in_chars; *out = e->enc.v44.out_octets;
}
static void v44_dec_counts(const v42ec_t *e, unsigned long *in, unsigned long *out)
{
    *in = e->dec.v44.in_octets; *out = e->dec.v44.out_chars;
}

/* indexed by comp_alg - V42EC_COMP_V42BIS */
static const comp_ops_t comp_ops[2] = {
    { bis_enc_init, bis_dec_init, bis_encode, bis_flush, bis_enc_pending,
      bis_dec_room, bis_decode, bis_dec_pending, bis_enc_counts, bis_dec_counts },
    { v44_enc_init, v44_dec_init, v44_encode, v44_flush, v44_enc_pending,
      v44_dec_room, v44_decode, v44_dec_pending, v44_enc_counts, v44_dec_counts }
};
typedef char comp_ops_rows[(V42EC_COMP_V44 - V42EC_COMP_V42BIS + 1
                            == (int)(sizeof comp_ops / sizeof comp_ops[0])) ? 1 : -1];

/* the procedure in use; only while comp_alg is not V42EC_COMP_NONE */
static const comp_ops_t *ops(const v42ec_t *e)
{
    return &comp_ops[e->comp_alg - V42EC_COMP_V42BIS];
}

/* C-INIT of whichever procedure the XID exchange settled on (V.42 bis 5.2,
 * 5.3; V.44 7.5). LAPM never leaves both agreed. A direction agreed but
 * refused here (values past this build's arrays: v42ec_init keeps our
 * proposals within them, so only a bug gets here) releases the link: the
 * far end would compress, or expect compressed data, while we did not. */
static void comp_init(v42ec_t *e)
{
    const v42lapm_t *l = &e->lapm;
    int tx = 0, rx = 0, bad = 0;

    e->comp_tx = e->comp_rx = 0;
    e->comp_alg = V42EC_COMP_NONE;
    e->enc_dirty = 0;
    if (l->v44_tx || l->v44_rx) {
        e->comp_alg = V42EC_COMP_V44;
        tx = l->v44_tx; rx = l->v44_rx;
    } else if (l->comp_tx || l->comp_rx) {
        e->comp_alg = V42EC_COMP_V42BIS;
        tx = l->comp_tx; rx = l->comp_rx;
    }
    if (tx) {
        if (ops(e)->enc_init(e) == 0) e->comp_tx = 1;
        else bad = 1;
    }
    if (rx) {
        if (ops(e)->dec_init(e) == 0) e->comp_rx = 1;
        else bad = 1;
    }
    if (bad) {
        e->comp_tx = e->comp_rx = 0;
        e->reason_override = V42EC_R_COMPRESSION;
        v42lapm_release(&e->lapm);
    }
}

/* the encoder and decoder in use */
static int enc_pending(const v42ec_t *e)
{
    return e->comp_tx && ops(e)->enc_pending(e) != 0;
}

static int dec_pending(const v42ec_t *e)
{
    return e->comp_rx && ops(e)->dec_pending(e) != 0;
}

static void finish(v42ec_t *e, int reason)
{
    e->state = V42EC_DISCONNECTED;
    e->reason = reason;
    e->t400 = e->t_est = -1;
}

/* protocol establishment phase begins (7.2.2) */
static void start_protocol(v42ec_t *e)
{
    e->state = V42EC_ESTABLISH;
    e->t400 = -1;
    if (e->cfg.originator) {
        e->hdlc_tx_on = 1;
        v42hdlc_tx_flags(&e->htx, PREAMBLE_FLAGS);
        if (e->cfg.detect) e->orig_wait = ORIG_WAIT_BITS;
        else v42lapm_start(&e->lapm);
    } else if (e->t_est < 0 && e->cfg.establish_ms > 0) {
        e->t_est = e->cfg.establish_ms;
    }
}

static void enter_async(v42ec_t *e)
{
    e->t400 = e->t_est = -1;
    if (!e->cfg.fallback) { finish(e, V42EC_R_NO_EC); return; }
    e->state = V42EC_ASYNC;
    e->hdlc_tx_on = 0;
    v42async_tx_init(&e->atx);
}

void v42ec_init(v42ec_t *e, const v42ec_cfg_t *cfg)
{
    memset(e, 0, sizeof *e);
    e->cfg = *cfg;
    e->cfg.originator = cfg->originator ? 1 : 0;
    /* never propose more than the arrays of v42_comp.h and v44_comp.h hold */
    if (e->cfg.lapm.comp_p1 > V42COMP_MAX_CODEWORDS) e->cfg.lapm.comp_p1 = V42COMP_MAX_CODEWORDS;
    if (e->cfg.lapm.v44_p1_tx > V44COMP_MAX_CODEWORDS) e->cfg.lapm.v44_p1_tx = V44COMP_MAX_CODEWORDS;
    if (e->cfg.lapm.v44_p1_rx > V44COMP_MAX_CODEWORDS) e->cfg.lapm.v44_p1_rx = V44COMP_MAX_CODEWORDS;
    if (e->cfg.lapm.v44_p3_tx > V44COMP_MAX_HISTORY) e->cfg.lapm.v44_p3_tx = V44COMP_MAX_HISTORY;
    if (e->cfg.lapm.v44_p3_rx > V44COMP_MAX_HISTORY) e->cfg.lapm.v44_p3_rx = V44COMP_MAX_HISTORY;
    e->txr.buf = e->txbuf; e->txr.size = sizeof e->txbuf;
    e->rxr.buf = e->rxbuf; e->rxr.size = sizeof e->rxbuf;
    e->t400 = e->t_est = -1;
    v42hdlc_tx_init(&e->htx);
    v42hdlc_rx_init(&e->hrx);
    v42async_tx_init(&e->atx);
    v42det_rx_init(&e->det_rx, e->cfg.originator);
    v42det_tx_init(&e->det_tx, e->cfg.originator ? V42DET_PAT_ODP : V42DET_PAT_ADP_V42, 8);
    v42lapm_init(&e->lapm, e->cfg.originator, &e->cfg.lapm);
    v42hdlc_rx_set_fcs(&e->hrx, e->lapm.rx_fcs_mode);

    if (e->cfg.detect) {
        e->state = V42EC_DETECT;
        e->t400 = e->cfg.t400_ms > 0 ? e->cfg.t400_ms : 750;
    } else {
        e->hdlc_tx_on = 1;
        start_protocol(e);
    }
}

/* ------------------------------------------- follow what LAPM has done */
static void drain_rx(v42ec_t *e)
{
    for (;;) {
        const uint8_t *p;
        int n = v42lapm_rx_avail(&e->lapm, &p);
        size_t room = ring_free(&e->rxr);

        if (e->comp_rx) {
            uint8_t tmp[256];
            size_t used = 0, out = 0, max = room < sizeof tmp ? room : sizeof tmp;
            int rc;
            if (n == 0 && !dec_pending(e)) break;
            if (room < ops(e)->dec_room(e)) break;
            rc = ops(e)->decode(e, p, (size_t)n, &used, tmp, max, &out);
            ring_put(&e->rxr, tmp, out);
            if (used) v42lapm_rx_consume(&e->lapm, (int)used);
            if (rc != 0) {
                /* C-ERROR (V.42 bis 5.8, V.44 7.15). V.42 bis asks for
                 * "appropriate recovery action, including re-establishment";
                 * this implementation does not re-establish, it releases
                 * the connection so no doubtful data reaches the user. */
                if (n > (int)used) v42lapm_rx_consume(&e->lapm, n - (int)used);
                e->reason_override = V42EC_R_COMPRESSION;
                v42lapm_release(&e->lapm);
                break;
            }
            if (used == 0 && out == 0) break;
        } else {
            size_t c = (size_t)n < room ? (size_t)n : room;
            if (c == 0) break;
            ring_put(&e->rxr, p, c);
            v42lapm_rx_consume(&e->lapm, (int)c);
        }
    }
}

static void sync(v42ec_t *e)
{
    const uint8_t *p;

    if (e->state == V42EC_DETECT || e->state == V42EC_ASYNC || e->state == V42EC_DISCONNECTED)
        return;
    v42hdlc_rx_set_fcs(&e->hrx, e->lapm.rx_fcs_mode);

    if (e->lapm.established != e->seen_established) {
        /* L-ESTABLISH: C-INIT both compression functions (V.42 bis 5.2, 5.3;
         * V.44 7.5) and
         * drop whatever the detection phase left in the receive buffer */
        e->seen_established = e->lapm.established;
        e->seen_resets = e->lapm.resets;
        comp_init(e);
        ring_reset(&e->rxr);
        e->brk_have = 0; e->brk_tx_pend = 0;
        e->t_est = -1;
    }
    if (e->lapm.resets != e->seen_resets) {
        /* destructive break (8.13, V.42 bis 5.6; for V.44 by the same rule,
         * see v42/README.md): data in both directions is
         * discarded and the compression functions start again */
        e->seen_resets = e->lapm.resets;
        ring_reset(&e->txr); ring_reset(&e->rxr);
        comp_init(e);
        e->brk_tx_pend = 0; e->brk_have = 0;
    }

    switch (e->lapm.state) {
    case V42LAPM_CONNECTED:    e->state = V42EC_CONNECTED; break;
    case V42LAPM_RELEASING:    e->state = V42EC_RELEASING; break;
    case V42LAPM_DISCONNECTED:
        finish(e, e->reason_override ? e->reason_override : e->lapm.reason);
        return;
    default:                   e->state = V42EC_ESTABLISH; break;
    }
    if (e->state != V42EC_CONNECTED) return;

    drain_rx(e);
    if (e->lapm.state != V42LAPM_CONNECTED) { sync(e); return; }

    /* Own-receiver busy (8.4.7) is left to LAPM, which declares it when a
     * frame arrives and the previous one could not be moved out of its
     * hold buffer above. Declaring it earlier from the buffer fill would
     * only make it discard frames that still fit. */

    /* a break is handed up once everything received before it is in the
     * user's buffer */
    if (!e->brk_have && v42lapm_rx_avail(&e->lapm, &p) == 0
        && !dec_pending(e)
        && v42lapm_get_break(&e->lapm, &e->brk_opt, &e->brk_len)) {
        e->brk_have = 1;
        e->brk_after = e->brk_opt == V42_BRK_NONDESTRUCTIVE_INSEQ ? e->rxr.len : 0;
    }
}

/* ------------------------------------------------------------ transmitter */
/* Turn queued user data into at most one new I frame. */
static void feed_lapm(v42ec_t *e)
{
    int room = v42lapm_can_send(&e->lapm);

    if (room > 0) {
        uint8_t info[V42_MAX_N401];
        size_t n = 0, limit = e->txr.len;

        if (e->brk_tx_pend && limit > e->brk_tx_after) limit = e->brk_tx_after;
        if (e->comp_tx) {
            for (;;) {
                size_t avail, c;
                const uint8_t *p = ring_peek(&e->txr, &avail);
                if (avail > limit) avail = limit;
                c = ops(e)->encode(e, p, avail, info, (size_t)room, &n);
                if (c) e->enc_dirty = 1;
                ring_drop(&e->txr, c);
                limit -= c;
                if (e->brk_tx_pend) e->brk_tx_after -= c;
                if (c == 0 || n >= (size_t)room) break;
            }
            /* nothing more to send for now: C-FLUSH so the far end gets
             * every character already accepted (V.42 bis 7.9, V.44 7.13) */
            if (limit == 0 && e->enc_dirty && n < (size_t)room
                && ops(e)->flush(e, info, (size_t)room, &n))
                e->enc_dirty = 0;
        } else {
            if (limit > (size_t)room) limit = (size_t)room;
            n = ring_get(&e->txr, info, limit);
            if (e->brk_tx_pend) e->brk_tx_after -= n;
        }
        if (n > 0) v42lapm_send(&e->lapm, info, (int)n);
    }
    if (e->brk_tx_pend && e->brk_tx_after == 0 && !e->enc_dirty
        && !enc_pending(e)
        && v42lapm_send_break(&e->lapm, e->brk_tx_opt, e->brk_tx_len) == 0)
        e->brk_tx_pend = 0;
}

/* before a line bit, when the originator still waits for its first frame
 * or the transmitter could take the next one */
static void hdlc_next(v42ec_t *e)
{
    if (e->orig_wait > 0 && (--e->orig_wait == 0 || e->hrx.flag_run >= FLAGS_SEEN)) {
        e->orig_wait = 0;
        v42lapm_start(&e->lapm);
    }
    if (!e->stopped && v42hdlc_tx_need_frame(&e->htx)) {
        uint8_t f[V42HDLC_MAX_PAYLOAD];
        int n, fcs32 = 0;
        if (e->state == V42EC_CONNECTED) feed_lapm(e);
        n = v42lapm_tx_frame(&e->lapm, f, &fcs32);
        if (n > 0) v42hdlc_tx_frame(&e->htx, f, n, fcs32);
    }
}

static int hdlc_bit(v42ec_t *e)
{
    if (e->orig_wait > 0 || v42hdlc_tx_need_frame(&e->htx)) hdlc_next(e);
    return v42hdlc_tx_bit(&e->htx);
}

int v42ec_tx_bit(v42ec_t *e)
{
    if (e->hdlc_tx_on) return hdlc_bit(e);      /* also after release: the last UA or DM */

    if (e->state == V42EC_ASYNC) {
        uint8_t c;
        if (v42async_tx_idle(&e->atx) && ring_get(&e->txr, &c, 1))
            v42async_tx_char(&e->atx, c);
        return v42async_tx_bit(&e->atx);
    }
    if (e->state == V42EC_DISCONNECTED) return 1;

    if (e->cfg.originator) {
        /* 7.2.1.2: ODP until the ADP is seen or T400 runs out; patterns are
         * always completed */
        if (e->state == V42EC_DETECT && v42det_tx_boundary(&e->det_tx)) {
            if (e->want_hdlc) { start_protocol(e); return hdlc_bit(e); }
            if (e->want_async) {
                enter_async(e);
                return 1;
            }
        }
        return v42det_tx_bit(&e->det_tx);
    }
    /* answerer, 7.2.1.3: mark, then the ADP once the ODP has been seen */
    if (e->sub == 0) {
        if (e->want_hdlc) { e->hdlc_tx_on = 1; return hdlc_bit(e); }
        return 1;
    }
    if (e->want_hdlc && e->det_tx.patterns >= ADP_MIN && v42det_tx_boundary(&e->det_tx)) {
        e->hdlc_tx_on = 1;
        return hdlc_bit(e);
    }
    return v42det_tx_bit(&e->det_tx);
}

void v42ec_tx_octets(v42ec_t *e, uint8_t *buf, size_t n)
{
    size_t i; int b;
    for (i = 0; i < n; i++) {
        unsigned o = 0;
        for (b = 0; b < 8; b++) o |= (unsigned)v42ec_tx_bit(e) << b;
        buf[i] = (uint8_t)o;
    }
}

/* --------------------------------------------------------------- receiver */
static void async_char(v42ec_t *e, int c)
{
    uint8_t ch = (uint8_t)c;
    if (c == V42ASYNC_FRAMING) { e->async_framing++; e->adp_strip = 0; return; }
    if (c < 0) return;
    if (e->adp_strip) {
        /* the answerer is still repeating "E NUL": not user data */
        int gap = e->det_rx.rx.gap;
        int want = e->adp_strip == 1 ? 'E' : 0;
        if (c == want && gap >= 8 && gap <= 16) { e->adp_strip ^= 3; return; }
        e->adp_strip = 0;
    }
    if (ring_put(&e->rxr, &ch, 1) == 0) e->async_overruns++;
}

void v42ec_rx_bit(v42ec_t *e, int bit)
{
    int r;

    bit = bit ? 1 : 0;
    if (e->state != V42EC_DETECT && e->state != V42EC_ASYNC && e->state != V42EC_DISCONNECTED) {
        /* HDLC alone: establishing, connected, releasing */
        r = v42hdlc_rx_bit(&e->hrx, bit);
        if (r > 0) {
            v42lapm_rx_frame(&e->lapm, e->hrx.buf, r, e->hrx.last_fcs32);
            sync(e);
        }
        return;
    }
    switch (e->state) {
    case V42EC_DISCONNECTED:
        return;
    case V42EC_ASYNC:
        async_char(e, v42async_rx_bit(&e->det_rx.rx, bit));
        return;
    case V42EC_DETECT:
        r = v42det_rx_bit(&e->det_rx, bit);
        /* keep what arrives: it is the user's if we end up in start-stop
         * mode (Appendix I.3 b) */
        if (e->det_rx.last_char >= 0) async_char(e, e->det_rx.last_char);
        if (e->cfg.originator) {
            if (e->want_hdlc || e->want_async) return;
            /* an answerer without the detection phase (7.2.1) sends flags
             * instead of the ADP: they too say it does LAPM */
            v42hdlc_rx_bit(&e->hrx, bit);
            if (r == V42DET_ADP_V42 || e->hrx.flag_run >= FLAGS_SEEN) e->want_hdlc = 1;
            else if (r == V42DET_ADP_NOEC) {
                e->want_async = 1;
                ring_reset(&e->rxr);
                e->adp_strip = 1;
            }
            return;
        }
        if (r == V42DET_ODP && e->sub == 0) {
            e->sub = 1;
            e->t400 = -1;
            ring_reset(&e->rxr);
            if (e->cfg.establish_ms > 0) e->t_est = e->cfg.establish_ms;
        }
        /* flags or a frame start the protocol phase, with or without ODP;
         * a frame with no opening flag (after a run of ones, v42_hdlc.h)
         * only with an address LAPM takes (0x01, 0x03): a caller without
         * error control sends characters after idle mark */
        {
            unsigned long rs = e->hrx.resynced;
            r = v42hdlc_rx_bit(&e->hrx, bit);
            if (r > 0 && e->hrx.resynced != rs && e->hrx.buf[0] != 0x01 && e->hrx.buf[0] != 0x03) r = 0;
        }
        if (r > 0 || e->hrx.flag_run >= FLAGS_SEEN) {
            e->want_hdlc = 1;
            start_protocol(e);
            if (r > 0) {
                v42lapm_rx_frame(&e->lapm, e->hrx.buf, r, e->hrx.last_fcs32);
                sync(e);
            }
        }
        return;
    }
}

void v42ec_rx_octets(v42ec_t *e, const uint8_t *buf, size_t n)
{
    size_t i; int b;
    for (i = 0; i < n; i++)
        for (b = 0; b < 8; b++) v42ec_rx_bit(e, (buf[i] >> b) & 1);
}

/* ------------------------------------------------------------------- time */
void v42ec_line_stopped(v42ec_t *e)
{
    e->stopped = 1;
    e->stop_cut = e->hdlc_tx_on && e->htx.in_frame;
    e->stop_frames = e->htx.frames;
}

void v42ec_line_restored(v42ec_t *e)
{
    if (e->stopped) {
        e->stopped = 0;
        if (e->hdlc_tx_on) {
            /* the frame the stop cut, if it is still going: abort it (an
             * I frame comes again through LAPM's poll); whole flags first */
            if (e->stop_cut && e->htx.in_frame && e->htx.frames == e->stop_frames)
                v42hdlc_tx_abort(&e->htx);
            v42hdlc_tx_flags(&e->htx, 2);
        }
    }
    v42lapm_line_restored(&e->lapm);
}

void v42ec_tick(v42ec_t *e, unsigned ms)
{
    if (e->state == V42EC_DISCONNECTED || e->state == V42EC_ASYNC) return;
    if (ms > 1000000u) ms = 1000000u;       /* as v42lapm_tick: past every timer, and an int */

    if (e->t400 >= 0) {
        e->t400 -= (int)ms;
        if (e->t400 <= 0) {
            /* 7.2.1.2, 7.2.1.3: no error-correcting peer */
            e->t400 = -1;
            if (e->cfg.originator) e->want_async = 1;   /* at the pattern boundary */
            else enter_async(e);
            return;
        }
    }
    if (e->t_est >= 0) {
        e->t_est -= (int)ms;
        if (e->t_est <= 0) {
            v42lapm_abort(&e->lapm, V42LAPM_R_PROTOCOL);
            finish(e, V42EC_R_NO_SABME);
            return;
        }
    }
    if (e->state != V42EC_DETECT) {
        v42lapm_tick(&e->lapm, ms);
        sync(e);
    }
}

/* -------------------------------------------------------------- user side */
size_t v42ec_write_space(const v42ec_t *e)
{
    if (e->state == V42EC_RELEASING || e->state == V42EC_DISCONNECTED) return 0;
    return ring_free(&e->txr);
}

size_t v42ec_write(v42ec_t *e, const uint8_t *data, size_t len)
{
    if (e->state == V42EC_RELEASING || e->state == V42EC_DISCONNECTED) return 0;
    return ring_put(&e->txr, data, len);
}

size_t v42ec_read_avail(const v42ec_t *e)
{
    if (e->state == V42EC_DETECT || e->state == V42EC_ESTABLISH) return 0;
    if (e->brk_have && e->brk_after < e->rxr.len) return e->brk_after;
    return e->rxr.len;
}

size_t v42ec_read(v42ec_t *e, uint8_t *data, size_t max)
{
    size_t n = v42ec_read_avail(e);
    if (n > max) n = max;
    n = ring_get(&e->rxr, data, n);
    if (e->brk_have) e->brk_after -= n < e->brk_after ? n : e->brk_after;
    if (n) sync(e);                             /* room again: unblock the receiver */
    return n;
}

int v42ec_tx_idle(const v42ec_t *e)
{
    if (e->txr.len != 0) return 0;
    if (e->state == V42EC_ASYNC) return v42async_tx_idle(&e->atx);
    if (e->enc_dirty || enc_pending(e)) return 0;
    return v42lapm_unacked(&e->lapm) == 0 && !e->brk_tx_pend;
}

void v42ec_release(v42ec_t *e)
{
    switch (e->state) {
    case V42EC_DETECT:
    case V42EC_ASYNC:
        finish(e, V42LAPM_R_LOCAL);
        break;
    case V42EC_ESTABLISH:
    case V42EC_CONNECTED:
        v42lapm_release(&e->lapm);
        sync(e);
        break;
    default:
        break;
    }
}

void v42ec_abort(v42ec_t *e)
{
    if (e->state == V42EC_DISCONNECTED) return;
    v42lapm_abort(&e->lapm, V42LAPM_R_LOCAL);   /* only a UA or DM owed still goes */
    e->orig_wait = 0;
    finish(e, V42LAPM_R_LOCAL);
}

int v42ec_send_break(v42ec_t *e, int opt, int len10ms)
{
    if (e->state != V42EC_CONNECTED || e->brk_tx_pend) return -1;
    if (opt == V42_BRK_NONDESTRUCTIVE_INSEQ) {
        /* goes after the data already written; feed_lapm() hands it on */
        if (e->lapm.brk_tx != 0) return -1;
        e->brk_tx_pend = 1; e->brk_tx_opt = opt; e->brk_tx_len = len10ms;
        e->brk_tx_after = e->txr.len;
        return 0;
    }
    if (v42lapm_send_break(&e->lapm, opt, len10ms) != 0) return -1;
    sync(e);
    return 0;
}

int v42ec_get_break(v42ec_t *e, int *opt, int *len10ms)
{
    if (!e->brk_have || e->brk_after != 0) return 0;
    e->brk_have = 0;
    *opt = e->brk_opt; *len10ms = e->brk_len;
    sync(e);
    return 1;
}

int v42ec_state(const v42ec_t *e) { return e->state; }

void v42ec_get_status(const v42ec_t *e, v42ec_status_t *st)
{
    const v42lapm_t *l = &e->lapm;
    memset(st, 0, sizeof *st);
    st->state = e->state; st->reason = e->reason;
    st->k_tx = l->k_tx; st->k_rx = l->k_rx;
    st->n401_tx = l->n401_tx; st->n401_rx = l->n401_rx;
    if (l->v44_tx || l->v44_rx) {
        st->comp_alg = V42EC_COMP_V44;
        st->comp_tx = l->v44_tx; st->comp_rx = l->v44_rx;
        st->v44_n2_tx = l->v44_n2_tx; st->v44_n7_tx = l->v44_n7_tx; st->v44_n8_tx = l->v44_n8_tx;
        st->v44_n2_rx = l->v44_n2_rx; st->v44_n7_rx = l->v44_n7_rx; st->v44_n8_rx = l->v44_n8_rx;
    } else {
        st->comp_alg = (l->comp_tx || l->comp_rx) ? V42EC_COMP_V42BIS : V42EC_COMP_NONE;
        st->comp_tx = l->comp_tx; st->comp_rx = l->comp_rx;
        st->comp_n2 = l->comp_n2; st->comp_n7 = l->comp_n7;
    }
    st->fcs32 = l->fcs32;
    st->tx_queued = e->txr.len;
    st->rx_avail = v42ec_read_avail(e);
    st->unacked = v42lapm_unacked(l);
    st->rx_frames = e->hrx.good;
    st->rx_frames_bad = e->hrx.bad_fcs + e->hrx.runts + e->hrx.aborts
                      + e->hrx.too_long + e->hrx.misaligned;
    st->tx_i = l->tx_i; st->tx_i_retx = l->tx_i_retx;
    st->rx_i = l->rx_i; st->rx_i_dropped = l->rx_i_dropped;
    st->tx_rej = l->tx_rej; st->rx_rej = l->rx_rej;
    st->t401_expiries = l->t401_expiries; st->violations = l->violations;
    if (e->comp_alg != V42EC_COMP_NONE) {
        if (e->comp_tx) ops(e)->enc_counts(e, &st->comp_in, &st->comp_out);
        if (e->comp_rx) ops(e)->dec_counts(e, &st->decomp_in, &st->decomp_out);
    }
    st->async_overruns = e->async_overruns; st->async_framing = e->async_framing;
}
