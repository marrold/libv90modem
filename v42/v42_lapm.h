/* LAPM, the error control function of ITU-T V.42 (clauses 8, 9, 10, 12).
 *
 * Frame level only: frames come in as octet strings that already passed the
 * HDLC checks of 8.1.3 (address, control, information; no FCS) and go out
 * the same way. The caller moves them through v42_hdlc. No callbacks, no
 * allocation, no OS calls; time arrives through v42lapm_tick().
 *
 * Implemented: XID negotiation (k, N401, optional functions, V.42 bis
 * P0/P1/P2, V.44 C0/P0/P1/P2/P3 in the user data subfield), SABME/UA establishment, I frames modulo 128, RR/RNR/REJ, P/F
 * checkpointing, T401/N400, optional T403, DISC/DM, BRK/BRKACK in UI
 * frames, 32-bit FCS negotiation. Not implemented and never agreed to in
 * XID: SREJ (both forms) and the TEST loop-back. FRMR is acted upon when
 * received and never sent (as in the 2002 text). T402 is not used: replies
 * are queued at once. */
#ifndef V90D_V42_LAPM_H
#define V90D_V42_LAPM_H
#include <stdint.h>
#include "v42_hdlc.h"

/* Compile-time ceiling for the window size k (9.2.4; default k is 15). */
#ifndef V42_MAX_K
#define V42_MAX_K 32
#endif

#define V42_DEFAULT_K     15
#define V42_DEFAULT_N401  128

/* connection states */
enum {
    V42LAPM_IDLE = 0,        /* disconnected, connection not yet attempted
                                (the answerer waits here for XID / SABME) */
    V42LAPM_NEGOTIATING,     /* XID command sent, awaiting XID response */
    V42LAPM_ESTABLISHING,    /* SABME sent, awaiting UA */
    V42LAPM_CONNECTED,
    V42LAPM_RELEASING,       /* DISC sent, awaiting UA or DM */
    V42LAPM_DISCONNECTED     /* over; see reason */
};

/* why the connection ended (L-RELEASE indication) */
enum {
    V42LAPM_R_NONE = 0,
    V42LAPM_R_LOCAL,         /* v42lapm_release() completed */
    V42LAPM_R_PEER_DISC,     /* peer sent DISC */
    V42LAPM_R_PEER_DM,       /* peer answered DM, or sent DM unsolicited */
    V42LAPM_R_N400,          /* retransmission limit reached, no response */
    V42LAPM_R_PROTOCOL,      /* frame rejection, bad N(R), FRMR, late SABME */
    V42LAPM_R_NEGOTIATION    /* XID exchange failed or gave illegal values */
};

/* break handling options, the "DS" bits of 12.3.1 */
#define V42_BRK_NONDESTRUCTIVE_INSEQ      0x00   /* D=0 S=0 */
#define V42_BRK_NONDESTRUCTIVE_EXPEDITED  0x40   /* D=0 S=1 */
#define V42_BRK_DESTRUCTIVE_EXPEDITED     0xC0   /* D=1 S=1 */

typedef struct {
    int use_xid;             /* originator: negotiate with XID before SABME */
    int k_tx, k_rx;          /* window wanted, our send / receive direction */
    int n401_tx, n401_rx;    /* information field limit wanted, octets */
    int fcs32;               /* request (originator) / accept (answerer) */
    int comp_tx, comp_rx;    /* V.42 bis wanted, our send / receive direction */
    int comp_p1;             /* V.42 bis codewords offered (512..) */
    int comp_p2;             /* V.42 bis maximum string length (6..250) */
    /* V.44 (V.44 7.3, 7.4, Table 10). Where both ends can do V.44 and
     * V.42 bis, V.44 is the one used. */
    int v44_tx, v44_rx;      /* V.44 wanted, our send / receive direction */
    int v44_p1_tx, v44_p1_rx;    /* codewords (256..) */
    int v44_p2_tx, v44_p2_rx;    /* maximum string length (32..255) */
    int v44_p3_tx, v44_p3_rx;    /* history length (512..) */
    int t401_ms;             /* acknowledgement timer (9.2.1) */
    int t403_ms;             /* inactivity timer (9.2.6), 0 = not used */
    int n400;                /* maximum retransmissions (9.2.2) */
} v42lapm_cfg_t;

typedef struct {
    uint16_t len;
    uint8_t  data[V42_MAX_N401];
} v42lapm_slot_t;

typedef struct {
    v42lapm_cfg_t cfg;
    int originator;
    int state, reason;

    /* parameters in force (defaults until XID says otherwise) */
    int k_tx, k_rx, n401_tx, n401_rx;
    int comp_tx, comp_rx;    /* V.42 bis agreed for our send / receive side */
    int comp_n2, comp_n7;
    int v44_tx, v44_rx;      /* V.44 agreed for our send / receive side */
    int v44_n2_tx, v44_n7_tx, v44_n8_tx;     /* our encoder */
    int v44_n2_rx, v44_n7_rx, v44_n8_rx;     /* our decoder */
    int fcs32;               /* 32-bit FCS agreed in XID */
    int tx_fcs32;            /* FCS our frames carry now */
    int rx_fcs_mode;         /* V42HDLC_FCS* our receiver should apply now */

    /* 8.2.3.2 state variables; vq is one past the newest I frame built */
    uint8_t vs, va, vr, vq;
    uint8_t vh;              /* one past the highest N(S) ever sent */
    int data_seen;           /* an I frame was accepted or acknowledged */
    int slot_head;           /* slot of frame V(A) */
    v42lapm_slot_t win[V42_MAX_K];

    int peer_busy, own_busy, rej_exc, timer_rec, busy_discard;
    int rc;                  /* retransmission count */
    int no_progress;         /* recoveries (T401 polls, REJ go-backs) since
                                V(A) last moved, with I frames unacknowledged;
                                the one after N400 of them releases the link
                                (V42LAPM_R_N400; a deviation from V.42, see
                                docs/error-control.md). Not counted while
                                the peer is busy (RNR, 8.4.7); a reset of the
                                data path (destructive break) keeps it */
    int t401, t403;          /* ms left, negative = stopped */
    unsigned pend;           /* frames owed, see v42_lapm.c */
    int ua_f, dm_f, brkack_f;
    uint8_t xid_resp[64]; int xid_resp_len;
    int xid_seen;            /* answerer: an XID command was answered */

    /* received information awaiting the user (one I frame) */
    uint8_t hold[V42_MAX_N401];
    int hold_len, hold_pos;

    /* break transfer (8.13) */
    int vsb, vrb;
    int brk_tx;              /* 0 none, 1 waiting for acks, 2 sent */
    int brk_opt, brk_len, brk_timer, brk_rc;
    int brk_rx, brk_rx_opt, brk_rx_len;

    /* events for the layer above: compare with a remembered copy */
    unsigned long established;   /* entered the connected state */
    unsigned long resets;        /* destructive break reset the data path */
    unsigned long brk_confirmed, brk_failed;

    /* statistics */
    unsigned long tx_i, tx_i_retx, rx_i, rx_i_dropped, tx_rej, rx_rej;
    unsigned long tx_polls, t401_expiries, violations;
} v42lapm_t;

void v42lapm_cfg_default(v42lapm_cfg_t *c);
void v42lapm_init(v42lapm_t *l, int originator, const v42lapm_cfg_t *cfg);
/* L-ESTABLISH request: originator sends XID (if configured) then SABME.
 * Has no effect on the answerer, which waits for the originator. */
void v42lapm_start(v42lapm_t *l);
/* L-RELEASE request: DISC, queued data discarded (8.7). */
void v42lapm_release(v42lapm_t *l);
/* Abandon the connection without telling the peer. */
void v42lapm_abort(v42lapm_t *l, int reason);

/* A valid frame arrived (FCS already checked; fcs32 tells which one). */
void v42lapm_rx_frame(v42lapm_t *l, const uint8_t *f, int len, int fcs32);
/* Next frame to transmit into f (V42HDLC_MAX_PAYLOAD octets of room).
 * Returns its length, or 0 when there is nothing to send. *fcs32 is set to
 * the FCS type the frame must carry. */
int  v42lapm_tx_frame(v42lapm_t *l, uint8_t *f, int *fcs32);

/* Maximum information field of a new I frame that could be handed over
 * right now, or 0 if none may be (window full, peer busy, recovery...). */
int  v42lapm_can_send(const v42lapm_t *l);
/* L-DATA request: queue one I frame. Returns 0, or -1 if not allowed. */
int  v42lapm_send(v42lapm_t *l, const uint8_t *info, int len);
/* I frames sent or queued and not yet acknowledged. */
int  v42lapm_unacked(const v42lapm_t *l);

/* L-DATA indication: information received and not yet consumed. */
int  v42lapm_rx_avail(const v42lapm_t *l, const uint8_t **p);
void v42lapm_rx_consume(v42lapm_t *l, int n);
/* Own-receiver busy condition (8.4.7): set while the user cannot take data. */
void v42lapm_set_busy(v42lapm_t *l, int busy);

/* L-SIGNAL request: send a break. opt is one of V42_BRK_*, len10ms the
 * length in 10 ms units (0 = default length). Returns 0, or -1 if not
 * connected or a previous break is still unconfirmed. */
int  v42lapm_send_break(v42lapm_t *l, int opt, int len10ms);
/* L-SIGNAL indication: returns 1 and fills opt and len10ms once per break. */
int  v42lapm_get_break(v42lapm_t *l, int *opt, int *len10ms);

void v42lapm_tick(v42lapm_t *l, unsigned ms);
/* The line carries data again after a pause of the modem's own (retrain,
 * rate renegotiation): with frames unacknowledged or a recovery under way,
 * the checkpoint of 8.4.8 is sent now instead of when T401 next runs out. */
void v42lapm_line_restored(v42lapm_t *l);

#endif
