/* LAPM, the error control function of ITU-T V.42. Clause numbers in the
 * comments refer to V.42 (03/2002) unless they say V.42 bis. */
#include <limits.h>
#include <string.h>
#include "v42_lapm.h"

#if V42_MAX_K < V42_DEFAULT_K || V42_MAX_K > 127
#error "V42_MAX_K must be 15..127"
#endif
#if V42_MAX_N401 < V42_DEFAULT_N401 || V42_MAX_N401 > 8191
#error "V42_MAX_N401 must be 128..8191"
#endif

#define MOD(x) ((uint8_t)((x) & 0x7F))

/* frames owed to the peer */
#define P_UA       0x0001u
#define P_DM       0x0002u
#define P_XID_RESP 0x0004u
#define P_XID_CMD  0x0008u
#define P_SABME    0x0010u
#define P_DISC     0x0020u
#define P_RESP_F1  0x0040u   /* supervisory response with F=1 */
#define P_POLL     0x0080u   /* supervisory command with P=1 */
#define P_REJ      0x0100u
#define P_BRKACK   0x0200u
#define P_BRK      0x0400u
#define P_ACK      0x0800u   /* N(R) to convey; any I or S frame does it */
#define P_STATUS   0x1000u   /* busy state changed; needs an S frame */

/* control field encodings (Table 8), P/F bit masked out for U frames */
#define C_RR    0x01
#define C_RNR   0x05
#define C_REJ   0x09
#define C_SREJ  0x0D
#define U_SABME 0x6F
#define U_DM    0x0F
#define U_UI    0x03
#define U_DISC  0x43
#define U_UA    0x63
#define U_FRMR  0x87
#define U_XID   0xAF
#define U_PF    0x10

#define OWN_BUSY_USER 1
#define OWN_BUSY_HOLD 2

static uint8_t cmd_addr(const v42lapm_t *l) { return l->originator ? 0x03 : 0x01; }   /* Table 6 */
static uint8_t rsp_addr(const v42lapm_t *l) { return l->originator ? 0x01 : 0x03; }

/* V.44 Table 10: ranges, and the values a proposal that leaves a parameter
 * out stands for */
#define V44_MIN_P1   256
#define V44_MIN_P2   32
#define V44_MAX_P2   255
#define V44_MIN_P3   512
#define V44_DEF_P1   1024
#define V44_DEF_P2   255

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void v42lapm_cfg_default(v42lapm_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->use_xid = 1;
    c->k_tx = c->k_rx = V42_DEFAULT_K;
    c->n401_tx = c->n401_rx = V42_DEFAULT_N401;
    c->comp_p1 = 512;
    c->comp_p2 = 6;
    c->v44_p1_tx = c->v44_p1_rx = 2048;
    c->v44_p2_tx = c->v44_p2_rx = 255;
    c->v44_p3_tx = c->v44_p3_rx = 4096;
    c->t401_ms = 1000;
    c->t403_ms = 0;
    c->n400 = 10;
}

void v42lapm_init(v42lapm_t *l, int originator, const v42lapm_cfg_t *cfg)
{
    memset(l, 0, sizeof *l);
    l->cfg = *cfg;
    l->cfg.k_tx = clampi(cfg->k_tx, 1, V42_MAX_K);
    l->cfg.k_rx = clampi(cfg->k_rx, 1, 127);
    l->cfg.n401_tx = clampi(cfg->n401_tx, 1, V42_MAX_N401);
    l->cfg.n401_rx = clampi(cfg->n401_rx, 1, V42_MAX_N401);
    l->cfg.comp_p1 = clampi(cfg->comp_p1, 512, 65535);
    l->cfg.comp_p2 = clampi(cfg->comp_p2, 6, 250);
    l->cfg.v44_p1_tx = clampi(cfg->v44_p1_tx, V44_MIN_P1, 65535);      /* V.44 Table 10 */
    l->cfg.v44_p1_rx = clampi(cfg->v44_p1_rx, V44_MIN_P1, 65535);
    l->cfg.v44_p2_tx = clampi(cfg->v44_p2_tx, V44_MIN_P2, V44_MAX_P2);
    l->cfg.v44_p2_rx = clampi(cfg->v44_p2_rx, V44_MIN_P2, V44_MAX_P2);
    l->cfg.v44_p3_tx = clampi(cfg->v44_p3_tx, V44_MIN_P3, 65535);
    l->cfg.v44_p3_rx = clampi(cfg->v44_p3_rx, V44_MIN_P3, 65535);
    l->cfg.n400 = cfg->n400 < 1 ? 1 : cfg->n400;            /* 9.2.2 */
    l->cfg.t401_ms = cfg->t401_ms < 1 ? 1 : cfg->t401_ms;
    l->originator = originator ? 1 : 0;
    l->state = V42LAPM_IDLE;
    l->k_tx = l->k_rx = V42_DEFAULT_K;
    l->n401_tx = l->n401_rx = V42_DEFAULT_N401;
    l->rx_fcs_mode = V42HDLC_FCS16;
    l->t401 = l->t403 = l->brk_timer = -1;
}

/* ----------------------------------------------------------------- timers */
static void t401_start(v42lapm_t *l)
{
    l->t401 = l->cfg.t401_ms;
    l->t403 = -1;                                           /* 8.12.3.2 */
}

static void t401_stop(v42lapm_t *l)
{
    l->t401 = -1;
    if (l->state == V42LAPM_CONNECTED && l->cfg.t403_ms > 0)
        l->t403 = l->cfg.t403_ms;                           /* 8.12.3.1 */
}

/* ------------------------------------------------------- state transitions */
static void terminate(v42lapm_t *l, int reason)
{
    l->state = V42LAPM_DISCONNECTED;
    l->reason = reason;
    l->pend &= (P_UA | P_DM);         /* replies already owed still go out */
    l->t401 = l->t403 = l->brk_timer = -1;
    l->vq = l->vs = l->vh = l->va;
    l->timer_rec = 0; l->brk_tx = 0;
    l->hold_len = l->hold_pos = 0;
}

void v42lapm_abort(v42lapm_t *l, int reason)
{
    if (l->state != V42LAPM_DISCONNECTED) terminate(l, reason);
}

/* frame-rejection condition (8.5.5): terminate when connected, else drop */
static void reject(v42lapm_t *l)
{
    if (l->state == V42LAPM_CONNECTED) terminate(l, V42LAPM_R_PROTOCOL);
}

static void data_reset(v42lapm_t *l)
{
    l->vs = l->va = l->vr = l->vq = l->vh = 0;
    l->slot_head = 0;
    l->hold_len = l->hold_pos = 0;
    l->peer_busy = 0; l->rej_exc = 0; l->timer_rec = 0; l->busy_discard = 0;
    l->data_seen = 0;
    l->rc = 0;                /* no_progress kept: no data moved (D9) */
    l->own_busy &= OWN_BUSY_USER;
    l->pend &= ~(P_RESP_F1 | P_POLL | P_REJ | P_ACK | P_STATUS);
    if (l->own_busy) l->pend |= P_STATUS;
}

static void enter_connected(v42lapm_t *l)
{
    l->state = V42LAPM_CONNECTED;
    l->pend &= P_UA;
    data_reset(l);
    l->no_progress = 0;
    l->brk_tx = 0; l->brk_timer = -1;
    t401_stop(l);
    l->established++;
}

static void send_sabme(v42lapm_t *l)
{
    l->state = V42LAPM_ESTABLISHING;
    l->pend = (l->pend & (P_UA | P_DM)) | P_SABME;
    l->rc = 0;
    /* 8.10.2: once the 32-bit FCS is agreed the originator uses it from
     * the SABME on; replies may still come either way until UA */
    l->tx_fcs32 = l->fcs32;
    l->rx_fcs_mode = l->fcs32 ? V42HDLC_FCS_EITHER : V42HDLC_FCS16;
    t401_start(l);
}

void v42lapm_start(v42lapm_t *l)
{
    if (!l->originator || l->state != V42LAPM_IDLE) return;
    if (l->cfg.use_xid) {
        l->state = V42LAPM_NEGOTIATING;
        l->pend |= P_XID_CMD;
        l->rc = 0;
        t401_start(l);
    } else {
        send_sabme(l);
    }
}

void v42lapm_release(v42lapm_t *l)
{
    switch (l->state) {
    case V42LAPM_IDLE:
        terminate(l, V42LAPM_R_LOCAL);
        break;
    case V42LAPM_NEGOTIATING:
    case V42LAPM_ESTABLISHING:
    case V42LAPM_CONNECTED:
        /* 8.7: DISC with P=1; queued data is discarded */
        l->state = V42LAPM_RELEASING;
        l->pend = (l->pend & (P_UA | P_DM)) | P_DISC;
        l->vq = l->vs = l->vh = l->va;
        l->timer_rec = 0; l->brk_tx = 0; l->brk_timer = -1;
        l->rc = 0;
        t401_start(l);
        l->t403 = -1;
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------- XID */
#define FI_GENERAL   0x82
#define GI_PARAM     0x80
#define GI_PRIVATE   0xF0
#define GI_USER      0xFF

typedef struct {
    int has_opt; uint8_t opt[4]; int opt_len;
    int n401_tx, n401_rx, k_tx, k_rx;       /* -1 when absent; octets, frames */
    int has_set_id, p0, p1, p2;             /* -1 when absent */
    /* V.44 parameter set of the user data subfield; T and R are the
     * sender's transmit and receive directions */
    int v44, v44_c0, v44_p0;
    int v44_p1t, v44_p1r, v44_p2t, v44_p2r, v44_p3t, v44_p3r;
} xid_t;

static uint32_t be_value(const uint8_t *p, int n)
{
    uint32_t v = 0;
    while (n-- > 0) v = (v << 8) | *p++;
    return v;
}

/* A PV of n octets read as a number of at most max octets (security pass
 * M3): leading zero octets past max are padding, so the value is the last
 * max octets; any other octet there makes it more than max octets hold,
 * and it is read as the largest they do (out of range, above every
 * limit), not as its leading octets. */
static uint32_t pv_value(const uint8_t *p, int n, int max)
{
    if (n > max) {
        int k;
        for (k = 0; k < n - max; k++)
            if (p[k]) return max >= 4 ? UINT32_MAX : ((uint32_t)1 << (8 * max)) - 1;
        p += n - max; n = max;
    }
    return be_value(p, n);
}

/* a value of up to 4 octets as an int: one past INT_MAX is out of range
 * anyway, and held there (security pass M4: the conversion of 2^31 and
 * above is implementation-defined, and gave a negative, absent value) */
static int pv_int(uint32_t v)
{
    return v > (uint32_t)INT_MAX ? INT_MAX : (int)v;
}

/* The user data subfield (12.2.1.3) as V.44 Annex A lays it out:
 * parameters in the PI, PL, PV form of the other subfields, the V.44 set
 * led by its set identifier. Parameters of other sets (the manufacturer ID
 * of Table 11c among them) are passed over. */
static void parse_user(const uint8_t *p, int len, xid_t *x)
{
    int pos = 0;

    while (pos + 2 <= len) {
        int pi = p[pos], pl = p[pos + 1];
        const uint8_t *pv = p + pos + 2;
        int v;
        if (pos + 2 + pl > len) break;      /* cut short: use what came before */
        v = (int)pv_value(pv, pl, 3);
        if (pi == 0x40) {
            /* "V44" in V.44 Table A.1; V.42 Table 11c NOTE 2 has 0x2A for
             * the first octet */
            x->v44 = pl == 3 && (pv[0] == 'V' || pv[0] == 0x2A) && pv[1] == '4' && pv[2] == '4';
        } else if (x->v44 && pl >= 1) {
            switch (pi) {
            case 0x41: x->v44_c0 = v; break;
            case 0x42: x->v44_p0 = v & 3; break;
            case 0x43: x->v44_p1t = v; break;
            case 0x44: x->v44_p1r = v; break;
            case 0x45: x->v44_p2t = v; break;
            case 0x46: x->v44_p2r = v; break;
            case 0x47: x->v44_p3t = v; break;
            case 0x48: x->v44_p3r = v; break;
            default: break;
            }
        }
        pos += 2 + pl;
    }
}

static int parse_xid(const uint8_t *info, int len, xid_t *x)
{
    int pos = 1;

    memset(x, 0, sizeof *x);
    x->n401_tx = x->n401_rx = x->k_tx = x->k_rx = -1;
    x->p0 = x->p1 = x->p2 = -1;
    x->v44_c0 = x->v44_p0 = -1;
    x->v44_p1t = x->v44_p1r = x->v44_p2t = x->v44_p2r = x->v44_p3t = x->v44_p3r = -1;
    if (len < 1 || info[0] != FI_GENERAL) return -1;
    while (pos < len) {
        int gi = info[pos], gl, end;
        if (gi == GI_USER) {                /* no length: runs to the FCS (12.2.1.3) */
            parse_user(info + pos + 1, len - pos - 1, x);
            break;
        }
        if (pos + 3 > len) break;
        gl = (info[pos + 1] << 8) | info[pos + 2];
        pos += 3;
        end = pos + gl;
        if (end > len) break;               /* truncated group: use what came before */
        while (pos + 2 <= end) {
            int pi = info[pos], pl = info[pos + 1];
            const uint8_t *pv = info + pos + 2;
            uint32_t v;
            if (pos + 2 + pl > end) break;
            v = pv_value(pv, pl, 4);
            if (gi == GI_PARAM) {
                switch (pi) {
                case 3: if (pl >= 1) { x->has_opt = 1; x->opt_len = pl; memcpy(x->opt, pv, (size_t)(pl > 4 ? 4 : pl)); } break;
                case 5: x->n401_tx = (int)(v >> 3); break;      /* units are bits */
                case 6: x->n401_rx = (int)(v >> 3); break;
                case 7: x->k_tx = pv_int(v); break;
                case 8: x->k_rx = pv_int(v); break;
                default: break;             /* not recognised: ignored (12.2.2) */
                }
            } else if (gi == GI_PRIVATE) {
                /* Table 11b: the set identifier comes first; without it
                 * the other parameters belong to someone else's set */
                if (pi == 0) {
                    x->has_set_id = pl == 3 && (pv[0] == 'V' || pv[0] == 0x2A) && pv[1] == '4' && pv[2] == '2';
                } else if (x->has_set_id) {
                    if (pi == 1 && pl >= 1) x->p0 = (int)(v & 3);
                    else if (pi == 2 && pl >= 1) x->p1 = pv_int(v);
                    else if (pi == 3 && pl >= 1) x->p2 = pv_int(v);
                }
            }
            pos += 2 + pl;
        }
        pos = end;
    }
    return 0;
}

/* Build the information field of an XID frame. Direction-sensitive values
 * are relative to the sender of the frame (Table 11a NOTE 2). */
static int build_xid(uint8_t *p, int response, int fcs32, int n401_tx, int n401_rx,
                     int k_tx, int k_rx, int with_comp, int p0, int p1, int p2, int opt3)
{
    int n = 0;
    p[n++] = FI_GENERAL;
    p[n++] = GI_PARAM; p[n++] = 0; p[n++] = (uint8_t)(opt3 ? 19 : 20);
    /* optional functions: bits 2, 4, 8, 9, 12, 16 always (Table 11a NOTE 1);
     * bit 17 = 32-bit FCS, and then bit 16 is cleared in a response.
     * opt3: in three octets. Our commands always have them so, as real
     * modems send them (an Agere SV92PP drops what follows a mask of any
     * other length, our compression offer with it); a response has the
     * form of the command it answers (a Conexant CX93001 does not take
     * four octets after three). */
    p[n++] = 3; p[n++] = (uint8_t)(opt3 ? 3 : 4);
    p[n++] = 0x8A;
    p[n++] = (uint8_t)((response && fcs32) ? 0x09 : 0x89);
    p[n++] = (uint8_t)(fcs32 ? 0x01 : 0x00);
    if (!opt3) p[n++] = 0x00;
    p[n++] = 5; p[n++] = 2; p[n++] = (uint8_t)((n401_tx * 8) >> 8); p[n++] = (uint8_t)(n401_tx * 8);
    p[n++] = 6; p[n++] = 2; p[n++] = (uint8_t)((n401_rx * 8) >> 8); p[n++] = (uint8_t)(n401_rx * 8);
    p[n++] = 7; p[n++] = 1; p[n++] = (uint8_t)k_tx;
    p[n++] = 8; p[n++] = 1; p[n++] = (uint8_t)k_rx;
    if (with_comp) {
        /* V.42 bis Annex A; the set identifier is "V42" */
        p[n++] = GI_PRIVATE; p[n++] = 0; p[n++] = 15;
        p[n++] = 0; p[n++] = 3; p[n++] = 'V'; p[n++] = '4'; p[n++] = '2';
        p[n++] = 1; p[n++] = 1; p[n++] = (uint8_t)p0;
        p[n++] = 2; p[n++] = 2; p[n++] = (uint8_t)(p1 >> 8); p[n++] = (uint8_t)p1;
        p[n++] = 3; p[n++] = 1; p[n++] = (uint8_t)p2;
    }
    return n;
}
/* the longest information fields build_xid and build_v44 write */
#define XID_BASE_MAX  24        /* four octets of optional functions */
#define XID_V42BIS    18
#define XID_V44       34
/* the largest XID response: the base with one compression set (V.44 7.3
 * NOTE: one procedure only), 58 octets */
typedef char v42lapm_xid_resp_fits[sizeof ((v42lapm_t *)0)->xid_resp >= XID_BASE_MAX + XID_V44
                                   && XID_V44 >= XID_V42BIS ? 1 : -1];

/* 9.2.3, 9.2.4: the responder's value lies between the initiator's value
 * and the default, inclusive; `mine` is what this end would like. */
static int choose(int theirs, int dflt, int mine)
{
    if (theirs <= 0) theirs = dflt;         /* absent: default */
    if (theirs >= dflt) {
        int cap = mine > dflt ? mine : dflt;
        return theirs < cap ? theirs : cap;
    }
    return theirs;
}

static int comp_values_bad(const xid_t *x)
{
    /* V.42 bis 5.1: P1 below 512 or P2 outside 6..250 is a procedural error */
    return (x->p1 >= 0 && x->p1 < 512) || (x->p2 >= 0 && (x->p2 < 6 || x->p2 > 250));
}

static int mini(int a, int b) { return a < b ? a : b; }

/* The V.44 parameter set as a user data subfield (V.44 Table A.1). C0 is
 * 0: negotiation is by XID only (7.4.1). */
static int build_v44(uint8_t *p, int p0, int p1t, int p1r, int p2t, int p2r, int p3t, int p3r)
{
    int n = 0;
    p[n++] = GI_USER;
    p[n++] = 0x40; p[n++] = 3; p[n++] = 'V'; p[n++] = '4'; p[n++] = '4';
    p[n++] = 0x41; p[n++] = 1; p[n++] = 0;
    p[n++] = 0x42; p[n++] = 1; p[n++] = (uint8_t)p0;
    p[n++] = 0x43; p[n++] = 2; p[n++] = (uint8_t)(p1t >> 8); p[n++] = (uint8_t)p1t;
    p[n++] = 0x44; p[n++] = 2; p[n++] = (uint8_t)(p1r >> 8); p[n++] = (uint8_t)p1r;
    p[n++] = 0x45; p[n++] = 1; p[n++] = (uint8_t)p2t;
    p[n++] = 0x46; p[n++] = 1; p[n++] = (uint8_t)p2r;
    p[n++] = 0x47; p[n++] = 2; p[n++] = (uint8_t)(p3t >> 8); p[n++] = (uint8_t)p3t;
    p[n++] = 0x48; p[n++] = 2; p[n++] = (uint8_t)(p3r >> 8); p[n++] = (uint8_t)p3r;
    return n;
}

/* V.44 7.4: a value below the minimum of Table 10 is a procedural error */
static int v44_values_bad(const xid_t *x)
{
    if (!x->v44) return 0;
    return (x->v44_p1t >= 0 && x->v44_p1t < V44_MIN_P1) || (x->v44_p1r >= 0 && x->v44_p1r < V44_MIN_P1) ||
           (x->v44_p2t >= 0 && (x->v44_p2t < V44_MIN_P2 || x->v44_p2t > V44_MAX_P2)) ||
           (x->v44_p2r >= 0 && (x->v44_p2r < V44_MIN_P2 || x->v44_p2r > V44_MAX_P2)) ||
           (x->v44_p3t >= 0 && x->v44_p3t < V44_MIN_P3) || (x->v44_p3r >= 0 && x->v44_p3r < V44_MIN_P3);
}

/* 7.4: each value in force is the lesser of the two ends' values. A
 * parameter left out stands for its default (Table 10): 1024 codewords,
 * strings of 255, a history of three times the codewords. The peer's
 * transmit direction is our receive direction. */
static void v44_take(v42lapm_t *l, const xid_t *x)
{
    int p1t = x->v44_p1t >= 0 ? x->v44_p1t : V44_DEF_P1;
    int p1r = x->v44_p1r >= 0 ? x->v44_p1r : V44_DEF_P1;
    int p3t = x->v44_p3t >= 0 ? x->v44_p3t : mini(3 * p1t, 65535);
    int p3r = x->v44_p3r >= 0 ? x->v44_p3r : mini(3 * p1r, 65535);

    l->v44_n2_rx = mini(p1t, l->cfg.v44_p1_rx);
    l->v44_n2_tx = mini(p1r, l->cfg.v44_p1_tx);
    l->v44_n7_rx = mini(x->v44_p2t >= 0 ? x->v44_p2t : V44_DEF_P2, l->cfg.v44_p2_rx);
    l->v44_n7_tx = mini(x->v44_p2r >= 0 ? x->v44_p2r : V44_DEF_P2, l->cfg.v44_p2_tx);
    l->v44_n8_rx = mini(p3t, l->cfg.v44_p3_rx);
    l->v44_n8_tx = mini(p3r, l->cfg.v44_p3_tx);
}

static void rx_xid_cmd(v42lapm_t *l, const uint8_t *info, int len)
{
    xid_t x;
    int with_comp = 0, p0 = 0;

    if (l->state != V42LAPM_IDLE && l->state != V42LAPM_CONNECTED) return;
    if (parse_xid(info, len, &x) != 0) return;
    if (l->state == V42LAPM_IDLE) {
        if (comp_values_bad(&x) || v44_values_bad(&x)) { terminate(l, V42LAPM_R_NEGOTIATION); return; }
        /* the initiator's transmit direction is our receive direction */
        l->k_rx = choose(x.k_tx, V42_DEFAULT_K, l->cfg.k_rx);
        l->k_tx = choose(x.k_rx, V42_DEFAULT_K, l->cfg.k_tx);
        l->n401_rx = choose(x.n401_tx, V42_DEFAULT_N401, l->cfg.n401_rx);
        l->n401_tx = choose(x.n401_rx, V42_DEFAULT_N401, l->cfg.n401_tx);
        l->fcs32 = l->cfg.fcs32 && x.has_opt && (x.opt[2] & 0x01);
        l->comp_tx = l->comp_rx = 0;
        if (x.p0 >= 0) {
            /* P0 bit 1: initiator to responder, bit 2: responder to initiator */
            with_comp = 1;
            l->comp_rx = (x.p0 & 1) && l->cfg.comp_rx;
            l->comp_tx = (x.p0 & 2) && l->cfg.comp_tx;
            l->comp_n2 = mini(x.p1 >= 0 ? x.p1 : 512, l->cfg.comp_p1);
            l->comp_n7 = mini(x.p2 >= 0 ? x.p2 : 6, l->cfg.comp_p2);
        }
        l->v44_tx = l->v44_rx = 0;
        if (x.v44) {
            /* V.44 Table 10: P0 bit 1 is the sender's transmit direction,
             * bit 2 its receive direction; left out, both. C0 asking for
             * negotiation after establishment (7.4.2) is not taken up:
             * the values of this XID are used and the answer has N = 0. */
            int p0 = x.v44_p0 >= 0 ? x.v44_p0 : 3;
            l->v44_rx = (p0 & 1) && l->cfg.v44_rx;
            l->v44_tx = (p0 & 2) && l->cfg.v44_tx;
            if (l->v44_tx || l->v44_rx) {
                /* V.44 is used in preference to V.42 bis, and the answer
                 * names one procedure only (V.44 7.3 NOTE): the V.42 bis
                 * set is left out of it */
                v44_take(l, &x);
                l->comp_tx = l->comp_rx = 0;
                with_comp = 0;
            }
        }
        /* 8.10.2: until the SABME shows which FCS is in use, check both */
        l->rx_fcs_mode = l->fcs32 ? V42HDLC_FCS_EITHER : V42HDLC_FCS16;
        l->xid_seen = 1;
    } else {
        /* Renegotiation on an established connection is not supported:
         * the reply restates the values in force and changes nothing. */
        with_comp = x.p0 >= 0 && !(l->v44_tx || l->v44_rx);
    }
    p0 = (l->comp_rx ? 1 : 0) | (l->comp_tx ? 2 : 0);
    l->xid_resp_len = build_xid(l->xid_resp, 1, l->fcs32, l->n401_tx, l->n401_rx,
                                l->k_tx, l->k_rx, with_comp, p0,
                                with_comp ? l->comp_n2 : 512, with_comp ? l->comp_n7 : 6,
                                x.has_opt && x.opt_len == 3);
    if (l->v44_tx || l->v44_rx)
        l->xid_resp_len += build_v44(l->xid_resp + l->xid_resp_len,
                                     (l->v44_tx ? 1 : 0) | (l->v44_rx ? 2 : 0),
                                     l->v44_n2_tx, l->v44_n2_rx, l->v44_n7_tx, l->v44_n7_rx,
                                     l->v44_n8_tx, l->v44_n8_rx);
    l->pend |= P_XID_RESP;
}

static void rx_xid_resp(v42lapm_t *l, const uint8_t *info, int len)
{
    xid_t x;
    int hi, v, want_p0;

    if (l->state != V42LAPM_NEGOTIATING) return;
    if (parse_xid(info, len, &x) != 0) return;
    /* the responder's receive direction is our transmit direction. Its
     * value is taken when it is below ours (9.2.4 places it between our
     * value and the default, but fewer or shorter frames than it can take
     * are always safe), and capped at the larger of the two */
    hi = l->cfg.k_tx > V42_DEFAULT_K ? l->cfg.k_tx : V42_DEFAULT_K;
    l->k_tx = clampi(x.k_rx > 0 ? x.k_rx : V42_DEFAULT_K, 1, hi);
    l->k_rx = clampi(x.k_tx > 0 ? x.k_tx : V42_DEFAULT_K, 1, 127);
    hi = l->cfg.n401_tx > V42_DEFAULT_N401 ? l->cfg.n401_tx : V42_DEFAULT_N401;
    l->n401_tx = clampi(x.n401_rx > 0 ? x.n401_rx : V42_DEFAULT_N401, 1, hi);
    v = x.n401_tx > 0 ? x.n401_tx : V42_DEFAULT_N401;
    if (v > V42_MAX_N401 || comp_values_bad(&x)) { terminate(l, V42LAPM_R_NEGOTIATION); return; }
    l->n401_rx = v;
    l->fcs32 = l->cfg.fcs32 && x.has_opt && (x.opt[2] & 0x01);
    want_p0 = (l->cfg.comp_tx ? 1 : 0) | (l->cfg.comp_rx ? 2 : 0);
    l->comp_tx = l->comp_rx = 0;
    if (x.p0 > 0) {
        /* V.42 bis 5.1: only directions that were proposed may be granted */
        if (x.p0 & ~want_p0) { terminate(l, V42LAPM_R_NEGOTIATION); return; }
        l->comp_tx = (x.p0 & 1) != 0;
        l->comp_rx = (x.p0 & 2) != 0;
        l->comp_n2 = mini(x.p1 >= 0 ? x.p1 : 512, l->cfg.comp_p1);
        l->comp_n7 = mini(x.p2 >= 0 ? x.p2 : 6, l->cfg.comp_p2);
    }
    l->v44_tx = l->v44_rx = 0;
    if (x.v44 && (x.v44_p0 < 0 || x.v44_p0 > 0)) {
        /* the responder's transmit direction (bit 1) is our receive
         * direction. Only what was proposed may be granted, in legal
         * values (V.44 7.4), and one procedure only (7.3 NOTE). */
        int p0 = x.v44_p0 >= 0 ? x.v44_p0 : 3;
        if (((p0 & 1) && !l->cfg.v44_rx) || ((p0 & 2) && !l->cfg.v44_tx) ||
            v44_values_bad(&x) || l->comp_tx || l->comp_rx) {
            terminate(l, V42LAPM_R_NEGOTIATION);
            return;
        }
        l->v44_rx = (p0 & 1) != 0;
        l->v44_tx = (p0 & 2) != 0;
        v44_take(l, &x);
    }
    l->pend &= ~P_XID_CMD;
    send_sabme(l);
}

/* ------------------------------------------------------ receiving frames */
/* 8.4.3.2: N(R) acknowledges; returns the number of frames newly
 * acknowledged, or -1 after terminating on an invalid N(R) (8.5.2). */
static int process_nr(v42lapm_t *l, uint8_t nr, int from_i)
{
    int span = MOD(l->vq - l->va), d = MOD(nr - l->va);

    /* The valid range is taken up to the newest frame built rather than
     * V(S): after a go-back V(S) is behind frames the peer may yet
     * acknowledge. */
    if (d > span) { terminate(l, V42LAPM_R_PROTOCOL); return -1; }
    if (d > 0) {
        if (MOD(l->vs - l->va) < d) l->vs = nr;
        if (MOD(l->vh - l->va) < d) l->vh = nr;
        l->slot_head = (l->slot_head + d) % V42_MAX_K;
        l->va = nr;
        l->data_seen = 1;
        l->no_progress = 0;
        /* NOTE 1: not while a poll is outstanding; NOTE 2: an I frame does
         * not stop the timer while the peer is busy */
        if (!l->timer_rec && !(from_i && l->peer_busy)) {
            if (l->va != l->vs) t401_start(l); else t401_stop(l);
        }
    }
    return d;
}

/* D9 (see enter_recovery): one more recovery with I frames outstanding and
 * V(A) not moved since the last; 0 when that is one past N400 and the
 * link was released */
static int no_progress(v42lapm_t *l)
{
    if (!l->peer_busy && MOD(l->vh - l->va) != 0 && l->no_progress++ >= l->cfg.n400) {
        terminate(l, V42LAPM_R_N400);
        return 0;
    }
    return 1;
}

static void busy_update(v42lapm_t *l, int mask)
{
    int old = l->own_busy;
    l->own_busy = mask;
    if (l->state != V42LAPM_CONNECTED) return;
    if (!old && mask) {
        l->pend |= P_STATUS;                /* RNR at the earliest opportunity */
    } else if (old && !mask) {
        /* 8.4.7 last paragraph: RR, or REJ when frames went missing. I
         * frames discarded while busy are treated as such a sequence
         * error so the peer goes back at once instead of on T401. */
        if (l->busy_discard) { l->busy_discard = 0; l->rej_exc = 1; l->pend |= P_REJ; }
        else l->pend |= P_STATUS;
    }
}

void v42lapm_set_busy(v42lapm_t *l, int busy)
{
    busy_update(l, busy ? (l->own_busy | OWN_BUSY_USER) : (l->own_busy & ~OWN_BUSY_USER));
}

int v42lapm_rx_avail(const v42lapm_t *l, const uint8_t **p)
{
    *p = l->hold + l->hold_pos;
    return l->hold_len - l->hold_pos;
}

void v42lapm_rx_consume(v42lapm_t *l, int n)
{
    l->hold_pos += n;
    if (l->hold_pos >= l->hold_len) {
        l->hold_len = l->hold_pos = 0;
        if (l->own_busy & OWN_BUSY_HOLD) busy_update(l, l->own_busy & ~OWN_BUSY_HOLD);
    }
}

/* common entry for I and S frames; returns 0 if the frame is to be ignored */
static int data_state(v42lapm_t *l)
{
    if (l->state == V42LAPM_ESTABLISHING && l->originator) {
        /* 8.3.2.1 last paragraph: the UA must have been lost */
        enter_connected(l);
        l->rx_fcs_mode = l->tx_fcs32 ? V42HDLC_FCS32 : V42HDLC_FCS16;
    }
    if (l->state != V42LAPM_CONNECTED) return 0;
    /* Table 4, destructive break: discard until it is acknowledged */
    if (l->brk_tx == 2 && (l->brk_opt & 0x80)) return 0;
    if (l->t401 < 0 && l->cfg.t403_ms > 0) l->t403 = l->cfg.t403_ms;
    return 1;
}

static void rx_i(v42lapm_t *l, const uint8_t *f, int len, int is_cmd)
{
    uint8_t ns = f[1] >> 1, nr = f[2] >> 1;
    int p = f[2] & 1, ilen = len - 3;

    if (!is_cmd) { l->violations++; return; }       /* I is a command only */
    if (!data_state(l)) return;
    if (ilen > l->n401_rx) { reject(l); return; }   /* 8.2.4.12 d) */
    if (process_nr(l, nr, 1) < 0) return;
    if (l->own_busy) {
        /* 8.4.7: discard after updating V(A); P=1 is answered RNR F=1 */
        l->rx_i_dropped++; l->busy_discard = 1;
        if (p) l->pend |= P_RESP_F1;
        return;
    }
    if (ns != l->vr) {
        /* 8.5.1: N(S) sequence error; one REJ per exception condition */
        l->rx_i_dropped++;
        if (!l->rej_exc) { l->rej_exc = 1; l->pend |= P_REJ; }
        if (p) l->pend |= P_RESP_F1;
        return;
    }
    if (l->hold_len > l->hold_pos) {
        /* the user has not taken the previous frame: busy from here on */
        l->rx_i_dropped++; l->busy_discard = 1;
        busy_update(l, l->own_busy | OWN_BUSY_HOLD);
        if (p) l->pend |= P_RESP_F1;
        return;
    }
    memcpy(l->hold, f + 3, (size_t)ilen);           /* 8.4.2 */
    l->hold_len = ilen; l->hold_pos = 0;
    l->vr = MOD(l->vr + 1);
    l->rej_exc = 0;
    l->data_seen = 1;
    l->rx_i++;
    l->pend |= p ? P_RESP_F1 : P_ACK;
}

static void rx_s(v42lapm_t *l, const uint8_t *f, int len, int is_cmd)
{
    uint8_t type = f[1], nr = f[2] >> 1;
    int pf = f[2] & 1, final = !is_cmd && pf, was_rec, d;

    if (!data_state(l)) return;
    /* SREJ is never agreed (8.4.5.1); anything else is undefined; an
     * information field makes the length incorrect (8.2.4.12 b) */
    if ((type != C_RR && type != C_RNR && type != C_REJ) || len != 3) { reject(l); return; }
    was_rec = l->timer_rec;
    switch (type) {
    case C_RR:
        if (process_nr(l, nr, 0) < 0) return;
        l->peer_busy = 0;
        if (final && was_rec) {
            /* 8.4.8: checkpoint reply; resume from N(R) */
            l->timer_rec = 0;
            l->vs = nr;
            t401_stop(l);
        } else if (!was_rec && l->va == l->vs) {
            t401_stop(l);
        }
        break;
    case C_RNR:
        if (process_nr(l, nr, 0) < 0) return;       /* 8.4.6 */
        l->peer_busy = 1;
        if (final && was_rec) { l->timer_rec = 0; l->vs = nr; }
        if (!l->timer_rec) t401_start(l);
        break;
    default:                                        /* REJ, 8.4.4 */
        l->rx_rej++;
        if ((d = process_nr(l, nr, 0)) < 0) return;
        l->peer_busy = 0;
        /* a go-back that acknowledges nothing is a recovery without
         * progress too (D9; a poll's answer was counted at the poll) */
        if (d == 0 && !was_rec && !no_progress(l)) return;
        if (!was_rec || final) {
            l->timer_rec = 0;
            l->vs = nr;
            t401_stop(l);
        }
        break;
    }
    /* Table 9: a response with F=1 that no poll asked for. Its N(R) has
     * been used as an acknowledgement all the same (8.4.3.2). */
    if (final && !was_rec) l->violations++;
    if (is_cmd && pf) l->pend |= P_RESP_F1;
}

static void rx_ui(v42lapm_t *l, const uint8_t *f, int len, int is_cmd)
{
    int msg, x;

    if (l->state != V42LAPM_CONNECTED) return;
    if (len < 3) { l->violations++; return; }
    msg = f[2] & 0x7F; x = f[2] >> 7;
    if (msg == 0x40 && is_cmd) {
        /* BRK (12.3.1, 8.13.3.2) */
        if (x == l->vrb) {
            l->vrb ^= 1;
            l->brk_rx = 1;
            l->brk_rx_opt = len >= 4 ? (f[3] & 0xC0) : 0;
            l->brk_rx_len = len >= 5 ? f[4] : 0;
            if (l->brk_rx_opt & 0x80) {             /* Table 5: destructive */
                data_reset(l);
                t401_stop(l);
                l->resets++;
            }
        }
        l->brkack_f = (f[1] & U_PF) ? 1 : 0;
        l->pend |= P_BRKACK;
    } else if (msg == 0x60 && !is_cmd) {
        /* BRKACK (8.13.3.3) */
        if (l->brk_tx == 2 && x == (l->vsb ^ 1)) {
            l->vsb ^= 1;
            l->brk_tx = 0; l->brk_timer = -1;
            l->pend &= ~P_BRK;
            l->brk_confirmed++;
        }
    } else {
        l->violations++;                            /* Table 9 NOTE 1 */
    }
}

static void rx_u(v42lapm_t *l, const uint8_t *f, int len, int is_cmd, int fcs32)
{
    int m = f[1] & ~U_PF, pf = (f[1] & U_PF) ? 1 : 0;

    switch (m) {
    case U_SABME:
        if (!is_cmd) { l->violations++; return; }
        if (len != 2) { reject(l); return; }
        switch (l->state) {
        case V42LAPM_IDLE:
            /* 8.3.2.1; 8.10.2: the SABME's own FCS settles the FCS in use */
            l->tx_fcs32 = l->fcs32 && fcs32;
            l->rx_fcs_mode = l->tx_fcs32 ? V42HDLC_FCS32 : V42HDLC_FCS16;
            enter_connected(l);
            l->ua_f = pf; l->pend |= P_UA;
            break;
        case V42LAPM_NEGOTIATING:
        case V42LAPM_ESTABLISHING:
            l->ua_f = pf; l->pend |= P_UA;          /* 8.9.1 */
            break;
        case V42LAPM_CONNECTED:
            if (!l->originator && !l->data_seen) {
                /* 8.4.9.1: our UA was lost; answer again and start our
                 * own I frames over (8.3.2.1 NOTE 3) */
                l->ua_f = pf; l->pend |= P_UA;
                l->vs = l->va;
                l->peer_busy = 0; l->rej_exc = 0; l->timer_rec = 0;
                l->pend &= ~(P_RESP_F1 | P_POLL | P_REJ);
                t401_stop(l);
            } else {
                terminate(l, V42LAPM_R_PROTOCOL);
            }
            break;
        default:                                    /* 8.9.2; unwilling */
            l->dm_f = pf; l->pend |= P_DM;
            break;
        }
        break;
    case U_DISC:
        if (!is_cmd) { l->violations++; return; }
        if (len != 2) { reject(l); return; }
        if (l->state == V42LAPM_CONNECTED) {
            terminate(l, V42LAPM_R_PEER_DISC);      /* 8.7.2 */
            l->ua_f = pf; l->pend |= P_UA;
        } else if (l->state == V42LAPM_RELEASING) {
            l->ua_f = pf; l->pend |= P_UA;          /* 8.9.1 */
        } else {
            l->dm_f = pf; l->pend |= P_DM;          /* 8.8, 8.9.2 */
        }
        break;
    case U_UA:
        if (is_cmd) { l->violations++; return; }
        if (len != 2) { reject(l); return; }
        if (l->state == V42LAPM_ESTABLISHING && pf) {
            enter_connected(l);
            l->rx_fcs_mode = l->tx_fcs32 ? V42HDLC_FCS32 : V42HDLC_FCS16;
        } else if (l->state == V42LAPM_RELEASING && pf) {
            terminate(l, V42LAPM_R_LOCAL);
        } else {
            l->violations++;                        /* Table 9 */
        }
        break;
    case U_DM:
        if (is_cmd) { l->violations++; return; }
        if (len != 2) { reject(l); return; }
        switch (l->state) {
        case V42LAPM_ESTABLISHING:
            if (pf) terminate(l, V42LAPM_R_PEER_DM);
            break;
        case V42LAPM_RELEASING:
            if (pf) terminate(l, V42LAPM_R_LOCAL);
            break;
        case V42LAPM_CONNECTED:
            if (!pf || l->timer_rec) terminate(l, V42LAPM_R_PEER_DM);
            else l->violations++;
            break;
        default:
            break;
        }
        break;
    case U_FRMR:
        if (is_cmd) { l->violations++; return; }
        if (l->state == V42LAPM_CONNECTED) terminate(l, V42LAPM_R_PROTOCOL);    /* 8.5.6 */
        break;
    case U_XID:
        if (is_cmd) rx_xid_cmd(l, f + 2, len - 2);
        else rx_xid_resp(l, f + 2, len - 2);
        break;
    case U_UI:
        rx_ui(l, f, len, is_cmd);
        break;
    default:
        reject(l);                                  /* includes TEST: not agreed */
        break;
    }
}

void v42lapm_rx_frame(v42lapm_t *l, const uint8_t *f, int len, int fcs32)
{
    int cr, is_cmd;

    if (len < 2) return;
    /* 8.1.3 e): single octet address, DLCI 0 only */
    if (!(f[0] & 1) || (f[0] >> 2) != 0) return;
    cr = (f[0] >> 1) & 1;
    is_cmd = l->originator ? (cr == 0) : (cr == 1);         /* Table 6 */
    if ((f[1] & 1) == 0) {
        if (len >= 3) rx_i(l, f, len, is_cmd);
    } else if ((f[1] & 3) == 1) {
        if (len >= 3) rx_s(l, f, len, is_cmd);
    } else {
        rx_u(l, f, len, is_cmd, fcs32);
    }
}

/* --------------------------------------------------- transmitting frames */
/* RR, RNR or REJ as 8.4.6 NOTE 2 chooses */
static int sup(v42lapm_t *l, uint8_t *f, uint8_t addr, int pf)
{
    uint8_t type = C_RR;
    if (l->own_busy) {
        type = C_RNR;
    } else if (l->pend & P_REJ) {
        type = C_REJ; l->pend &= ~P_REJ; l->tx_rej++;
    }
    f[0] = addr; f[1] = type; f[2] = (uint8_t)((l->vr << 1) | pf);
    l->pend &= ~(P_ACK | P_STATUS);
    return 3;
}

static int i_allowed(const v42lapm_t *l)
{
    return l->state == V42LAPM_CONNECTED && !l->timer_rec && !l->peer_busy &&
           MOD(l->vs - l->va) < l->k_tx;
}

int v42lapm_tx_frame(v42lapm_t *l, uint8_t *f, int *fcs32)
{
    *fcs32 = l->tx_fcs32;
    if (l->pend & P_UA) {
        l->pend &= ~P_UA;
        f[0] = rsp_addr(l); f[1] = (uint8_t)(U_UA | (l->ua_f ? U_PF : 0));
        return 2;
    }
    if (l->pend & P_DM) {
        l->pend &= ~P_DM;
        f[0] = rsp_addr(l); f[1] = (uint8_t)(U_DM | (l->dm_f ? U_PF : 0));
        return 2;
    }
    if (l->pend & P_XID_RESP) {
        l->pend &= ~P_XID_RESP;
        f[0] = rsp_addr(l); f[1] = U_XID;
        memcpy(f + 2, l->xid_resp, (size_t)l->xid_resp_len);
        if (l->state != V42LAPM_CONNECTED) *fcs32 = 0;      /* 8.10.2 */
        return 2 + l->xid_resp_len;
    }
    if (l->state == V42LAPM_NEGOTIATING && (l->pend & P_XID_CMD)) {
        int want = l->cfg.comp_tx || l->cfg.comp_rx, n;
        l->pend &= ~P_XID_CMD;
        f[0] = cmd_addr(l); f[1] = U_XID;
        *fcs32 = 0;
        n = 2 + build_xid(f + 2, 0, l->cfg.fcs32, l->cfg.n401_tx, l->cfg.n401_rx,
                          l->cfg.k_tx, l->cfg.k_rx, want,
                          (l->cfg.comp_tx ? 1 : 0) | (l->cfg.comp_rx ? 2 : 0),
                          l->cfg.comp_p1, l->cfg.comp_p2, 1);
        /* both procedures are offered and the responder picks one (V.44
         * 7.3 NOTE); the user data subfield comes last (V.42 12.2.1.3) */
        if (l->cfg.v44_tx || l->cfg.v44_rx)
            n += build_v44(f + n, (l->cfg.v44_tx ? 1 : 0) | (l->cfg.v44_rx ? 2 : 0),
                           l->cfg.v44_p1_tx, l->cfg.v44_p1_rx, l->cfg.v44_p2_tx, l->cfg.v44_p2_rx,
                           l->cfg.v44_p3_tx, l->cfg.v44_p3_rx);
        return n;
    }
    if (l->state == V42LAPM_ESTABLISHING && (l->pend & P_SABME)) {
        l->pend &= ~P_SABME;
        f[0] = cmd_addr(l); f[1] = U_SABME | U_PF;          /* 8.3.2.1 NOTE 1 */
        return 2;
    }
    if (l->state == V42LAPM_RELEASING && (l->pend & P_DISC)) {
        l->pend &= ~P_DISC;
        f[0] = cmd_addr(l); f[1] = U_DISC | U_PF;           /* 8.7.2 NOTE */
        return 2;
    }
    if (l->state != V42LAPM_CONNECTED) return 0;

    /* in-sequence break: goes once everything before it is acknowledged */
    if (l->brk_tx == 1 && l->va == l->vq) {
        l->brk_tx = 2; l->pend |= P_BRK;
        l->brk_timer = l->cfg.t401_ms; l->brk_rc = 0;
    }
    if (l->pend & P_RESP_F1) {
        l->pend &= ~P_RESP_F1;
        return sup(l, f, rsp_addr(l), 1);
    }
    if (l->pend & P_POLL) {
        l->pend &= ~P_POLL;
        l->tx_polls++;
        return sup(l, f, cmd_addr(l), 1);
    }
    if ((l->pend & P_REJ) && !l->own_busy)              /* REJ would clear busy */
        return sup(l, f, rsp_addr(l), 0);
    if (l->pend & P_BRKACK) {
        l->pend &= ~P_BRKACK;
        f[0] = rsp_addr(l); f[1] = (uint8_t)(U_UI | (l->brkack_f ? U_PF : 0));
        f[2] = (uint8_t)(0x60 | (l->vrb << 7));
        return 3;
    }
    if (l->pend & P_BRK) {
        int n = 4;
        l->pend &= ~P_BRK;
        f[0] = cmd_addr(l); f[1] = U_UI;                    /* P = 0 */
        f[2] = (uint8_t)(0x40 | (l->vsb << 7));
        f[3] = (uint8_t)l->brk_opt;
        if (l->brk_len > 0) f[n++] = (uint8_t)l->brk_len;
        return n;
    }
    if (l->vs != l->vq && i_allowed(l)) {
        /* 8.4.1 */
        int idx = MOD(l->vs - l->va);
        const v42lapm_slot_t *s = &l->win[(l->slot_head + idx) % V42_MAX_K];
        f[0] = cmd_addr(l);
        f[1] = (uint8_t)(l->vs << 1);
        f[2] = (uint8_t)(l->vr << 1);
        memcpy(f + 3, s->data, s->len);
        if (idx < MOD(l->vh - l->va)) l->tx_i_retx++;
        else l->vh = MOD(l->vs + 1);
        l->vs = MOD(l->vs + 1);
        l->tx_i++;
        l->pend &= ~P_ACK;
        if (l->t401 < 0) t401_start(l);
        return 3 + s->len;
    }
    if (l->pend & (P_ACK | P_STATUS))
        return sup(l, f, rsp_addr(l), 0);
    return 0;
}

int v42lapm_can_send(const v42lapm_t *l)
{
    if (!i_allowed(l) || l->vs != l->vq || l->brk_tx != 0) return 0;
    return l->n401_tx;
}

int v42lapm_send(v42lapm_t *l, const uint8_t *info, int len)
{
    v42lapm_slot_t *s;
    if (len < 1 || len > v42lapm_can_send(l)) return -1;
    s = &l->win[(l->slot_head + MOD(l->vq - l->va)) % V42_MAX_K];
    memcpy(s->data, info, (size_t)len);
    s->len = (uint16_t)len;
    l->vq = MOD(l->vq + 1);
    return 0;
}

int v42lapm_unacked(const v42lapm_t *l)
{
    return MOD(l->vq - l->va);
}

/* ------------------------------------------------------------------ break */
int v42lapm_send_break(v42lapm_t *l, int opt, int len10ms)
{
    if (l->state != V42LAPM_CONNECTED || l->brk_tx != 0) return -1;
    l->brk_opt = opt & 0xC0;
    l->brk_len = clampi(len10ms, 0, 255);
    if (l->brk_opt & 0x80) {
        /* Table 4, destructive: data not yet transmitted is discarded and
         * the state variables return to their initial values */
        data_reset(l);
        t401_stop(l);
        l->resets++;
    }
    if (l->brk_opt & 0xC0) {
        l->brk_tx = 2; l->pend |= P_BRK;
        l->brk_timer = l->cfg.t401_ms; l->brk_rc = 0;       /* 8.13.3.1 */
    } else {
        l->brk_tx = 1;                  /* after outstanding data (Table 4) */
    }
    return 0;
}

int v42lapm_get_break(v42lapm_t *l, int *opt, int *len10ms)
{
    if (!l->brk_rx) return 0;
    l->brk_rx = 0;
    *opt = l->brk_rx_opt; *len10ms = l->brk_rx_len;
    return 1;
}

/* ------------------------------------------------------------------- time */
/* Timer recovery begins (8.4.8, 8.12.3.3). V.42 starts the retransmission
 * count again at each recovery, so a peer that answers every poll and
 * never acknowledges an I frame keeps the link up for ever. Ruled
 * otherwise here (every wait bounded): after N400 whole recoveries with I
 * frames sent and unacknowledged and V(A) never moving, the next one
 * releases the link (V42LAPM_R_N400) instead. Not while the peer says it
 * is busy (RNR): 8.4.7 lets that last. Returns 0 when it released. */
static int enter_recovery(v42lapm_t *l)
{
    if (!no_progress(l)) return 0;
    l->timer_rec = 1; l->rc = 0;
    return 1;
}

static void t401_expired(v42lapm_t *l)
{
    l->t401 = -1;
    l->t401_expiries++;
    switch (l->state) {
    case V42LAPM_NEGOTIATING:                               /* 8.10.3 */
    case V42LAPM_ESTABLISHING:                              /* 8.3.2.2 */
    case V42LAPM_RELEASING:                                 /* 8.7.3 */
        if (l->rc >= l->cfg.n400) {
            terminate(l, l->state == V42LAPM_RELEASING ? V42LAPM_R_LOCAL : V42LAPM_R_N400);
            return;
        }
        l->rc++;
        l->pend |= l->state == V42LAPM_NEGOTIATING ? P_XID_CMD :
                   l->state == V42LAPM_ESTABLISHING ? P_SABME : P_DISC;
        l->t401 = l->cfg.t401_ms;
        break;
    case V42LAPM_CONNECTED:                                 /* 8.4.8 */
        if (!l->timer_rec) { if (!enter_recovery(l)) return; }
        else l->rc++;
        if (l->rc < l->cfg.n400) {
            l->pend |= P_POLL;
            t401_start(l);
        } else {
            terminate(l, V42LAPM_R_N400);
        }
        break;
    default:
        break;
    }
}

void v42lapm_line_restored(v42lapm_t *l)
{
    if (l->state != V42LAPM_CONNECTED) return;
    if (!l->timer_rec && l->va == l->vs) return;
    if (!l->timer_rec && !enter_recovery(l)) return;
    l->pend |= P_POLL;
    t401_start(l);
}

void v42lapm_tick(v42lapm_t *l, unsigned ms)
{
    int d = ms > 1000000u ? 1000000 : (int)ms;

    if (l->t401 >= 0 && (l->t401 -= d) <= 0) t401_expired(l);
    if (l->state == V42LAPM_CONNECTED && l->t403 >= 0 && (l->t403 -= d) <= 0) {
        /* 8.12.3.3 */
        l->t403 = -1;
        if (enter_recovery(l)) {
            l->pend |= P_POLL;
            t401_start(l);
        }
    }
    if (l->state == V42LAPM_CONNECTED && l->brk_tx == 2 && l->brk_timer >= 0 &&
        (l->brk_timer -= d) <= 0) {
        /* 8.13.3.4 */
        if (l->brk_rc >= l->cfg.n400) {
            l->brk_tx = 0; l->brk_timer = -1; l->brk_failed++;
        } else {
            l->brk_rc++;
            l->pend |= P_BRK;
            l->brk_timer = l->cfg.t401_ms;
        }
    }
}
