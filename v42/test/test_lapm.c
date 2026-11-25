/* LAPM tests at frame level: two error control functions joined by a
 * simulated frame pipe (1 frame per direction per millisecond) with frame
 * loss injected by filters. Encodings are checked against V.42 Table 8,
 * 12.2 and 12.3; procedures against clause 8. */
#include <string.h>
#include "test.h"
#include "v42_lapm.h"
#include "v42_comp.h"

static unsigned rnd_state = 4242;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 16) & 0x7FFF;
}

static v42lapm_t A, B;                  /* A originates, B answers */
static v42lapm_cfg_t ca, cb;

/* user data flowing each way */
#define DMAX 60000
typedef struct {
    uint8_t src[DMAX]; size_t len, pos; int chunk;
    uint8_t got[DMAX]; size_t ngot;
    int reading;                        /* receiver takes data */
} flow_t;
static flow_t ab, ba;

/* frame log */
#define LOGN 20000
typedef struct { int from_a, len, fcs32, t; uint8_t f[40]; } logent;
static logent flog[LOGN];
static int nlog, now_ms;

/* filter: return 0 to lose the frame */
typedef int (*filter_fn)(int from_a, const uint8_t *f, int len);
static filter_fn filt;

static void flow_reset(flow_t *w, size_t len, int chunk)
{
    size_t i;
    w->len = len; w->pos = 0; w->ngot = 0; w->chunk = chunk; w->reading = 1;
    for (i = 0; i < len; i++) w->src[i] = (uint8_t)(rnd() >> 2);
}

static void setup(void)
{
    v42lapm_cfg_default(&ca); v42lapm_cfg_default(&cb);
    flow_reset(&ab, 0, 100); flow_reset(&ba, 0, 100);
    nlog = 0; now_ms = 0; filt = NULL;
}

static void init_pair(void)
{
    v42lapm_init(&A, 1, &ca);
    v42lapm_init(&B, 0, &cb);
}

static void sink(v42lapm_t *l, flow_t *w)
{
    const uint8_t *p; int n;
    if (!w->reading) return;
    while ((n = v42lapm_rx_avail(l, &p)) > 0) {
        if (w->ngot + (size_t)n <= DMAX) memcpy(w->got + w->ngot, p, (size_t)n);
        w->ngot += (size_t)n;
        v42lapm_rx_consume(l, n);
    }
}

/* what the layer above does when the line can take a frame */
static int next_frame(v42lapm_t *l, flow_t *w, uint8_t *f, int *fcs32)
{
    int room;
    /* offer data first so an I frame, not a bare RR, carries the next
     * acknowledgement (8.4.2.2) */
    if (w->pos < w->len && (room = v42lapm_can_send(l)) > 0) {
        int c = w->chunk < room ? w->chunk : room;
        if ((size_t)c > w->len - w->pos) c = (int)(w->len - w->pos);
        CHECK_EQ_I(v42lapm_send(l, w->src + w->pos, c), 0);
        w->pos += (size_t)c;
    }
    return v42lapm_tx_frame(l, f, fcs32);
}

static void one_way(int from_a)
{
    v42lapm_t *tx = from_a ? &A : &B, *rx = from_a ? &B : &A;
    uint8_t f[V42HDLC_MAX_PAYLOAD + 8];
    int fcs32 = 0, n = next_frame(tx, from_a ? &ab : &ba, f, &fcs32);
    if (n == 0) return;
    if (nlog < LOGN) {
        logent *e = &flog[nlog++];
        e->from_a = from_a; e->len = n; e->fcs32 = fcs32; e->t = now_ms;
        memcpy(e->f, f, (size_t)(n < 40 ? n : 40));
    }
    if (filt && !filt(from_a, f, n)) return;
    v42lapm_rx_frame(rx, f, n, fcs32);
    sink(rx, from_a ? &ab : &ba);
}

static void run(int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        one_way(1); one_way(0);
        sink(&A, &ba); sink(&B, &ab);
        v42lapm_tick(&A, 1); v42lapm_tick(&B, 1);
        now_ms++;
    }
}

static int connect_pair(void)
{
    init_pair();
    v42lapm_start(&A);
    run(20);
    return A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED;
}

/* count logged frames from one side whose first octets match */
static int count(int from_a, const uint8_t *pat, int plen)
{
    int i, c = 0;
    for (i = 0; i < nlog; i++)
        if (flog[i].from_a == from_a && flog[i].len >= plen && memcmp(flog[i].f, pat, (size_t)plen) == 0) c++;
    return c;
}
#define COUNT(from_a, ...) count(from_a, (const uint8_t[]){__VA_ARGS__}, (int)sizeof((const uint8_t[]){__VA_ARGS__}))
/* count by control octet only */
static int count_ctrl(int from_a, int ctrl)
{
    int i, c = 0;
    for (i = 0; i < nlog; i++) if (flog[i].from_a == from_a && flog[i].f[1] == ctrl) c++;
    return c;
}
static int is_i(const uint8_t *f, int len) { return len >= 3 && (f[1] & 1) == 0; }

static int flows_ok(void)
{
    return ab.ngot == ab.len && ba.ngot == ba.len &&
           memcmp(ab.got, ab.src, ab.len) == 0 && memcmp(ba.got, ba.src, ba.len) == 0;
}

/* ------------------------------------------------------------------ tests */
static void test_establish_encodings(void)
{
    static const uint8_t xid_plain[] = {
        0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8A, 0x89, 0x00,
        0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00, 0x07, 0x01, 0x0F, 0x08, 0x01, 0x0F };
    static const uint8_t xid_comp[] = {
        0xF0, 0x00, 0x0F, 0x00, 0x03, 'V', '4', '2', 0x01, 0x01, 0x03,
        0x02, 0x02, 0x02, 0x00, 0x03, 0x01, 0x20 };

    /* SABME P=1 from the originator is 03 7F; UA F=1 from the answerer is
     * 03 73 (Table 6: answerer responses carry C/R = 1; Table 8) */
    setup(); ca.use_xid = 0;
    CHECK(connect_pair());
    CHECK_EQ_I(nlog, 2);
    CHECK(flog[0].from_a == 1 && flog[0].len == 2 && flog[0].f[0] == 0x03 && flog[0].f[1] == 0x7F);
    CHECK(flog[1].from_a == 0 && flog[1].len == 2 && flog[1].f[0] == 0x03 && flog[1].f[1] == 0x73);
    CHECK_EQ_I(A.established, 1); CHECK_EQ_I(B.established, 1);
    CHECK_EQ_I(A.k_tx, 15); CHECK_EQ_I(B.n401_rx, 128);
    /* the answerer does nothing on its own */
    setup(); init_pair(); run(3000);
    CHECK_EQ_I(nlog, 0); CHECK_EQ_I(B.state, V42LAPM_IDLE);

    /* default XID command and response (12.2, Table 11a) */
    setup();
    CHECK(connect_pair());
    CHECK_EQ_I(nlog, 4);
    CHECK_EQ_I(flog[0].len, 2 + (int)sizeof xid_plain);
    CHECK(flog[0].f[0] == 0x03 && flog[0].f[1] == 0xAF);
    CHECK(memcmp(flog[0].f + 2, xid_plain, sizeof xid_plain) == 0);
    CHECK_EQ_I(flog[1].len, 2 + (int)sizeof xid_plain);
    CHECK(flog[1].f[0] == 0x03 && flog[1].f[1] == 0xAF && flog[1].from_a == 0);
    CHECK(memcmp(flog[1].f + 2, xid_plain, sizeof xid_plain) == 0);
    CHECK(flog[2].f[1] == 0x7F && flog[3].f[1] == 0x73);

    /* with V.42 bis requested both ways (Table 11b / V.42 bis Annex A) */
    setup();
    ca.comp_tx = ca.comp_rx = 1; ca.comp_p1 = 512; ca.comp_p2 = 32;
    cb.comp_tx = cb.comp_rx = 1; cb.comp_p1 = 512; cb.comp_p2 = 32;
    CHECK(connect_pair());
    CHECK_EQ_I(flog[0].len, 2 + (int)sizeof xid_plain + (int)sizeof xid_comp);
    CHECK(memcmp(flog[0].f + 2 + sizeof xid_plain, xid_comp, 14) == 0);
    CHECK_EQ_I(flog[1].len, flog[0].len);
    CHECK(memcmp(flog[1].f + 2 + sizeof xid_plain, xid_comp, 14) == 0);
    CHECK(A.comp_tx && A.comp_rx && B.comp_tx && B.comp_rx);
    CHECK_EQ_I(A.comp_n2, 512); CHECK_EQ_I(B.comp_n7, 32);

    /* a command in the four-octet form of the 2002 text is answered in that
     * form */
    {
        static const uint8_t x4[] = { 0x03, 0xAF, 0x82, 0x80, 0x00, 0x14, 0x03, 0x04, 0x8A, 0x89, 0x00, 0x00,
                                      0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00, 0x07, 0x01, 0x0F, 0x08, 0x01, 0x0F };
        setup(); init_pair();
        v42lapm_rx_frame(&B, x4, sizeof x4, 0);
        CHECK(B.xid_resp_len >= 12);
        CHECK(memcmp(B.xid_resp, x4 + 2, 10) == 0);
    }
}

/* What a parser that takes the optional functions in three octets only
 * (as an Agere SV92PP's does) reads of an XID command: it drops the rest
 * of the frame after a mask of any other length. 1 when it finds the
 * V.42 bis set ("V42") after the parameter group; *v44 1 when the user
 * data subfield follows with the V.44 set at once (no group length,
 * V.42 12.2.1.3). */
static int strict_reads_comp(const uint8_t *f, int len, int *v44)
{
    int pos = 3, gl, end, comp = 0;
    *v44 = 0;
    if (len < 6 || f[1] != 0xAF || f[2] != 0x82 || f[3] != 0x80) return 0;
    gl = (f[4] << 8) | f[5];
    pos = 6; end = pos + gl;
    if (end > len) return 0;
    while (pos + 2 <= end) {
        if (f[pos] == 3 && f[pos + 1] != 3) return 0;          /* drops everything after */
        pos += 2 + f[pos + 1];
    }
    pos = end;
    while (pos < len) {
        if (f[pos] == 0xFF) {
            *v44 = pos + 5 < len && f[pos + 1] == 0x40 && f[pos + 2] == 3 && f[pos + 3] == 'V'
                   && f[pos + 4] == '4' && f[pos + 5] == '4';
            break;
        }
        if (pos + 3 > len) break;
        gl = (f[pos + 1] << 8) | f[pos + 2];
        if (f[pos] == 0xF0 && pos + 8 <= len && f[pos + 3] == 0 && f[pos + 4] == 3
            && f[pos + 5] == 'V' && f[pos + 6] == '4' && f[pos + 7] == '2') comp = 1;
        pos += 3 + gl;
    }
    return comp;
}

/* our XID command carries the optional functions in three octets, bit 16
 * set, as real modems send them */
static void test_xid_command_form(void)
{
    uint8_t f[V42HDLC_MAX_PAYLOAD]; int fcs, n, v44, fcs32;
    for (fcs32 = 0; fcs32 <= 1; fcs32++) {
        setup(); ca.comp_tx = ca.comp_rx = 1; ca.v44_tx = ca.v44_rx = 1; ca.fcs32 = fcs32;
        cb.v44_tx = cb.v44_rx = 1;
        init_pair(); v42lapm_start(&A);
        n = v42lapm_tx_frame(&A, f, &fcs);
        CHECK(n > 12 && f[1] == 0xAF);
        CHECK_EQ_I(f[6], 3); CHECK_EQ_I(f[7], 3);                  /* PI 3, PL 3 */
        CHECK_EQ_I(f[8], 0x8A);
        CHECK_EQ_I(f[9] & 0x80, 0x80);                             /* bit 16 */
        CHECK_EQ_I(f[10], fcs32 ? 0x01 : 0x00);                    /* bit 17 */
        CHECK_EQ_I(f[11], 0x05);                                   /* N401 next */
        CHECK_EQ_I(strict_reads_comp(f, n, &v44), 1);
        CHECK_EQ_I(v44, 1);
        /* and our own answerer takes it */
        v42lapm_rx_frame(&B, f, n, 0);
        CHECK_EQ_I(B.state, V42LAPM_IDLE);
        CHECK_EQ_I(B.v44_rx, 1); CHECK_EQ_I(B.fcs32, 0);
        CHECK(B.xid_resp_len > 0 && B.xid_resp[4] == 3 && B.xid_resp[5] == 3);
        CHECK(B.xid_resp_len <= (int)sizeof B.xid_resp);
    }
    /* V.42 bis alone: offered, and no user data subfield */
    setup(); ca.comp_tx = ca.comp_rx = 1; cb.comp_tx = cb.comp_rx = 1;
    init_pair(); v42lapm_start(&A);
    n = v42lapm_tx_frame(&A, f, &fcs);
    CHECK_EQ_I(strict_reads_comp(f, n, &v44), 1);
    CHECK_EQ_I(v44, 0);
}

/* 9.2.4: a responder that can take fewer frames, or shorter ones, than we
 * proposed for our sending direction is taken at its word */
static void test_xid_response_smaller(void)
{
    static const uint8_t rsp[] = { 0x03, 0xAF, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8A, 0x89, 0x00,
                                   0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x02, 0x00,   /* N401 rx 64 */
                                   0x07, 0x01, 0x0F, 0x08, 0x01, 0x04 };             /* k rx 4 */
    static const uint8_t big[] = { 0x03, 0xAF, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8A, 0x89, 0x00,
                                   0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x10, 0x00,   /* N401 rx 512 */
                                   0x07, 0x01, 0x0F, 0x08, 0x01, 0x1E };             /* k rx 30 */
    uint8_t f[V42HDLC_MAX_PAYLOAD]; int fcs;
    setup(); init_pair(); v42lapm_start(&A);
    v42lapm_tx_frame(&A, f, &fcs);
    v42lapm_rx_frame(&A, rsp, sizeof rsp, 0);
    CHECK_EQ_I(A.state, V42LAPM_ESTABLISHING);
    CHECK_EQ_I(A.k_tx, 4); CHECK_EQ_I(A.n401_tx, 64);
    CHECK_EQ_I(A.k_rx, 15); CHECK_EQ_I(A.n401_rx, 128);
    /* more than we proposed: what we proposed */
    setup(); init_pair(); v42lapm_start(&A);
    v42lapm_tx_frame(&A, f, &fcs);
    v42lapm_rx_frame(&A, big, sizeof big, 0);
    CHECK_EQ_I(A.state, V42LAPM_ESTABLISHING);
    CHECK_EQ_I(A.k_tx, 15); CHECK_EQ_I(A.n401_tx, 128);
}

/* XID values longer than they need to be (security pass M3, M4): zero
 * padding is read through, and a value past an int is out of range, not
 * absent */
static void test_xid_long_values(void)
{
    /* N401 rx 64 octets (512 bits) in 5 octets; k rx 2^31 in 4 */
    static const uint8_t rsp[] = { 0x03, 0xAF, 0x82, 0x80, 0x00, 0x11,
                                   0x06, 0x05, 0x00, 0x00, 0x00, 0x02, 0x00,
                                   0x08, 0x04, 0x80, 0x00, 0x00, 0x00,
                                   0x05, 0x02, 0x04, 0x00 };
    /* V.44 P1T 2048 in 4 octets (V.44 values are read in 3) */
    static const uint8_t v44[] = { 0x03, 0xAF, 0x82, 0xFF, 0x40, 3, 'V', '4', '4', 0x43, 4, 0x00, 0x00, 0x08, 0x00 };
    uint8_t f[V42HDLC_MAX_PAYLOAD]; int fcs;
    setup(); ca.k_tx = 30; init_pair(); v42lapm_start(&A);
    v42lapm_tx_frame(&A, f, &fcs);
    v42lapm_rx_frame(&A, rsp, sizeof rsp, 0);
    CHECK_EQ_I(A.state, V42LAPM_ESTABLISHING);
    CHECK_EQ_I(A.n401_tx, 64);
    CHECK_EQ_I(A.k_tx, 30);         /* more than we proposed: what we proposed */
    setup(); cb.comp_tx = cb.comp_rx = 1; cb.v44_tx = cb.v44_rx = 1; init_pair();
    v42lapm_rx_frame(&B, v44, sizeof v44, 0);
    CHECK_EQ_I(B.state, V42LAPM_IDLE);
    CHECK_EQ_I(B.v44_rx, 1);
}

static void neg(int akt, int akr, int ant, int anr, int bkt, int bkr, int bnt, int bnr,
                int k_ab, int k_ba, int n_ab, int n_ba)
{
    setup();
    ca.k_tx = akt; ca.k_rx = akr; ca.n401_tx = ant; ca.n401_rx = anr;
    cb.k_tx = bkt; cb.k_rx = bkr; cb.n401_tx = bnt; cb.n401_rx = bnr;
    CHECK(connect_pair());
    CHECK_EQ_I(A.k_tx, k_ab); CHECK_EQ_I(B.k_rx, k_ab);
    CHECK_EQ_I(A.k_rx, k_ba); CHECK_EQ_I(B.k_tx, k_ba);
    CHECK_EQ_I(A.n401_tx, n_ab); CHECK_EQ_I(B.n401_rx, n_ab);
    CHECK_EQ_I(A.n401_rx, n_ba); CHECK_EQ_I(B.n401_tx, n_ba);
}

static void comp(int at, int ar, int ap1, int ap2, int bt, int br, int bp1, int bp2,
                 int want_ab, int want_ba, int n2, int n7)
{
    setup();
    ca.comp_tx = at; ca.comp_rx = ar; ca.comp_p1 = ap1; ca.comp_p2 = ap2;
    cb.comp_tx = bt; cb.comp_rx = br; cb.comp_p1 = bp1; cb.comp_p2 = bp2;
    CHECK(connect_pair());
    CHECK_EQ_I(A.comp_tx, want_ab); CHECK_EQ_I(B.comp_rx, want_ab);
    CHECK_EQ_I(A.comp_rx, want_ba); CHECK_EQ_I(B.comp_tx, want_ba);
    if (want_ab || want_ba) {
        CHECK_EQ_I(A.comp_n2, n2); CHECK_EQ_I(B.comp_n2, n2);
        CHECK_EQ_I(A.comp_n7, n7); CHECK_EQ_I(B.comp_n7, n7);
    }
}

static int drop_first_from_a(int from_a, const uint8_t *f, int len)
{
    static int done;
    (void)f; (void)len;
    if (nlog == 1) done = 0;
    if (from_a && !done) { done = 1; return 0; }
    return 1;
}
static int drop_all(int from_a, const uint8_t *f, int len) { (void)from_a; (void)f; (void)len; return 0; }

static void test_negotiation(void)
{
    int maxn = V42_MAX_N401 < 256 ? V42_MAX_N401 : 256;
    int maxk = V42_MAX_K < 30 ? V42_MAX_K : 30;

    /* 9.2.3, 9.2.4: the responder picks between the initiator's value and
     * the default, inclusive */
    neg(15, 15, 128, 128, 15, 15, 128, 128, 15, 15, 128, 128);
    neg(maxk, maxk, maxn, maxn, 15, 15, 128, 128, 15, 15, 128, 128);   /* capped by B */
    neg(8, 9, 64, 72, 15, 15, 128, 128, 8, 9, 64, 72);                 /* below default */
    neg(maxk, 20, maxn, 200, maxk, maxk, maxn, maxn, maxk, 20, maxn, 200 < maxn ? 200 : maxn);
    neg(15, 15, 128, 128, maxk, maxk, maxn, maxn, 15, 15, 128, 128);   /* B may not exceed */
    neg(20, 20, 128, 128, 18, 17, 128, 128, 17, 18, 128, 128);

    /* V.42 bis clause 5.1: P0 direction rules, lower P1 and P2 win */
    comp(1, 1, 2048, 32, 1, 1, 1024, 250, 1, 1, 1024 < V42COMP_MAX_CODEWORDS ? 1024 : V42COMP_MAX_CODEWORDS, 32);
    comp(1, 1, 512, 250, 1, 1, 2048, 6, 1, 1, 512, 6);
    comp(1, 0, 512, 6, 1, 1, 512, 6, 1, 0, 512, 6);
    comp(0, 1, 512, 6, 1, 1, 512, 6, 0, 1, 512, 6);
    comp(1, 1, 512, 6, 1, 0, 512, 6, 0, 1, 512, 6);
    comp(1, 1, 512, 6, 0, 1, 512, 6, 1, 0, 512, 6);
    comp(1, 1, 512, 6, 0, 0, 512, 6, 0, 0, 0, 0);
    comp(0, 0, 512, 6, 1, 1, 512, 6, 0, 0, 0, 0);
    comp(1, 0, 512, 6, 1, 0, 512, 6, 0, 0, 0, 0);     /* disjoint wishes */

    /* no XID at all: defaults, no compression */
    setup(); ca.use_xid = 0; cb.comp_tx = cb.comp_rx = 1; cb.k_rx = V42_MAX_K;
    CHECK(connect_pair());
    CHECK_EQ_I(B.k_rx, 15); CHECK_EQ_I(B.comp_rx, 0); CHECK_EQ_I(B.comp_tx, 0);

    /* XID command lost: repeated after T401 (8.10.3) */
    setup(); filt = drop_first_from_a;
    init_pair(); v42lapm_start(&A); run(ca.t401_ms + 50);
    CHECK(A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED);
    CHECK_EQ_I(count_ctrl(1, 0xAF), 2);
    CHECK(flog[1].t >= ca.t401_ms && flog[1].t <= ca.t401_ms + 2);
    /* never answered: N400 retransmissions, then give up */
    setup(); filt = drop_all; ca.n400 = 3;
    init_pair(); v42lapm_start(&A); run(5 * ca.t401_ms);
    CHECK_EQ_I(count_ctrl(1, 0xAF), 4);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(A.reason, V42LAPM_R_N400);

    /* illegal V.42 bis values are a procedural error (V.42 bis 5.1) */
    {
        uint8_t x[] = { 0x03, 0xAF, 0x82, 0xF0, 0x00, 0x0C, 0x00, 0x03, 'V', '4', '2',
                        0x01, 0x01, 0x03, 0x02, 0x02, 0x00, 0x64 };       /* P1 = 100 */
        uint8_t y[] = { 0x03, 0xAF, 0x82, 0xF0, 0x00, 0x0B, 0x00, 0x03, 'V', '4', '2',
                        0x01, 0x01, 0x03, 0x03, 0x01, 0x05 };             /* P2 = 5 */
        uint8_t z[] = { 0x03, 0xAF, 0x82, 0xF0, 0x00, 0x08, 0x00, 0x03, 'V', '4', '2',
                        0x01, 0x01, 0x03 };                               /* P0 = both */
        uint8_t f[V42HDLC_MAX_PAYLOAD]; int fcs;
        setup(); cb.comp_tx = cb.comp_rx = 1; init_pair();
        v42lapm_rx_frame(&B, x, sizeof x, 0);
        CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(B.reason, V42LAPM_R_NEGOTIATION);
        init_pair();
        v42lapm_rx_frame(&B, y, sizeof y, 0);
        CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED);
        /* a response granting a direction that was not asked for */
        setup(); ca.comp_tx = 1; ca.comp_rx = 0; init_pair();
        v42lapm_start(&A); v42lapm_tx_frame(&A, f, &fcs);
        v42lapm_rx_frame(&A, z, sizeof z, 0);
        CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(A.reason, V42LAPM_R_NEGOTIATION);
    }
    /* The XID command of a Conexant CX93001 as recorded on the box
     * (2026-11-15). It gives the HDLC optional functions in three octets, as
     * the earlier editions of V.42 had it (the 2002 text says four). The
     * modem repeated this command 43 times and never took our four-octet
     * answer, so the answer now has the form the command had. */
    {
        static const uint8_t cx[] = {
            0x03, 0xaf, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00,
            0x07, 0x01, 0x0f, 0x08, 0x01, 0x0f, 0xf0, 0x00, 0x0f, 0x00, 0x03, 0x56, 0x34, 0x32, 0x01, 0x01, 0x03, 0x02, 0x02,
            0x02, 0x00, 0x03, 0x01, 0x20, 0xff, 0x40, 0x03, 0x56, 0x34, 0x34, 0x41, 0x01, 0x00, 0x42, 0x01, 0x03, 0x43, 0x02,
            0x02, 0x00, 0x44, 0x02, 0x02, 0x00, 0x45, 0x01, 0x20, 0x46, 0x01, 0x20, 0x47, 0x02, 0x04, 0x00, 0x48, 0x02, 0x04, 0x00 };
        static const uint8_t want[] = { 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00,
                                        0x07, 0x01, 0x0f, 0x08, 0x01, 0x0f, 0xf0, 0x00, 0x0f, 0x00, 0x03, 0x56, 0x34, 0x32 };
        setup(); cb.comp_tx = cb.comp_rx = 1; cb.k_rx = cb.k_tx = 15; init_pair();
        v42lapm_rx_frame(&B, cx, sizeof cx, 0);
        CHECK_EQ_I(B.state, V42LAPM_IDLE);
        CHECK_EQ_I(B.k_rx, 15); CHECK_EQ_I(B.n401_rx, 128);
        CHECK(B.xid_resp_len >= (int)sizeof want);
        CHECK(memcmp(B.xid_resp, want, sizeof want) == 0);
    }
    /* unknown parameters and a trailing user data subfield are ignored */
    {
        uint8_t x[] = { 0x03, 0xAF, 0x82, 0x80, 0x00, 0x0A, 0x63, 0x02, 0xAA, 0xBB,
                        0x07, 0x01, 0x0A, 0x08, 0x01, 0x0C,
                        0xFF, 0x41, 0x01, 0x00, 0xFF, 0x03, 0x00, 0x00, 0x00 };
        setup(); init_pair();
        v42lapm_rx_frame(&B, x, sizeof x, 0);
        CHECK_EQ_I(B.state, V42LAPM_IDLE);
        CHECK_EQ_I(B.k_rx, 10); CHECK_EQ_I(B.k_tx, 12);
        CHECK_EQ_I(B.n401_rx, 128);
        CHECK_EQ_I(B.xid_resp_len > 0, 1);
    }
}

/* ------------------------------------------------- V.44 in XID (V.44 7.3,
 * 7.4, Annex A; V.42 12.2.1.3, Table 11c) */
#define V44_SET(p0, p1t, p1r, p2t, p2r, p3t, p3r) \
    0xFF, 0x40, 0x03, 'V', '4', '4', 0x41, 0x01, 0x00, 0x42, 0x01, (p0), \
    0x43, 0x02, (uint8_t)((p1t) >> 8), (uint8_t)(p1t), 0x44, 0x02, (uint8_t)((p1r) >> 8), (uint8_t)(p1r), \
    0x45, 0x01, (p2t), 0x46, 0x01, (p2r), \
    0x47, 0x02, (uint8_t)((p3t) >> 8), (uint8_t)(p3t), 0x48, 0x02, (uint8_t)((p3r) >> 8), (uint8_t)(p3r)
#define XID_HEAD 0x03, 0xAF, 0x82, 0x80, 0x00, 0x14, 0x03, 0x04, 0x8A, 0x89, 0x00, 0x00, \
    0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00, 0x07, 0x01, 0x0F, 0x08, 0x01, 0x0F
/* the head of our own command: the optional functions in three octets */
#define XID_CMD_HEAD 0x03, 0xAF, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8A, 0x89, 0x00, \
    0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00, 0x07, 0x01, 0x0F, 0x08, 0x01, 0x0F
#define V42BIS_SET 0xF0, 0x00, 0x0F, 0x00, 0x03, 'V', '4', '2', 0x01, 0x01, 0x03, 0x02, 0x02, 0x02, 0x00, 0x03, 0x01, 0x20

typedef struct { int tx, rx, p1t, p1r, p2t, p2r, p3t, p3r; } v44want;

static void v44cfg(v42lapm_cfg_t *c, v44want w)
{
    c->v44_tx = w.tx; c->v44_rx = w.rx;
    c->v44_p1_tx = w.p1t; c->v44_p1_rx = w.p1r;
    c->v44_p2_tx = w.p2t; c->v44_p2_rx = w.p2r;
    c->v44_p3_tx = w.p3t; c->v44_p3_rx = w.p3r;
}

/* A proposes a, B is willing to do b; ab and ba say whether V.44 comes out
 * for A to B and B to A, with n2/n7/n8 for each direction */
static void v44neg(v44want a, v44want b, int ab_on, int n2ab, int n7ab, int n8ab,
                   int ba_on, int n2ba, int n7ba, int n8ba)
{
    setup(); v44cfg(&ca, a); v44cfg(&cb, b);
    CHECK(connect_pair());
    CHECK_EQ_I(A.v44_tx, ab_on); CHECK_EQ_I(B.v44_rx, ab_on);
    CHECK_EQ_I(A.v44_rx, ba_on); CHECK_EQ_I(B.v44_tx, ba_on);
    if (ab_on) {
        CHECK_EQ_I(A.v44_n2_tx, n2ab); CHECK_EQ_I(B.v44_n2_rx, n2ab);
        CHECK_EQ_I(A.v44_n7_tx, n7ab); CHECK_EQ_I(B.v44_n7_rx, n7ab);
        CHECK_EQ_I(A.v44_n8_tx, n8ab); CHECK_EQ_I(B.v44_n8_rx, n8ab);
    }
    if (ba_on) {
        CHECK_EQ_I(A.v44_n2_rx, n2ba); CHECK_EQ_I(B.v44_n2_tx, n2ba);
        CHECK_EQ_I(A.v44_n7_rx, n7ba); CHECK_EQ_I(B.v44_n7_tx, n7ba);
        CHECK_EQ_I(A.v44_n8_rx, n8ba); CHECK_EQ_I(B.v44_n8_tx, n8ba);
    }
    CHECK_EQ_I(A.comp_tx, 0); CHECK_EQ_I(A.comp_rx, 0);
    CHECK_EQ_I(B.comp_tx, 0); CHECK_EQ_I(B.comp_rx, 0);
}

/* B, willing to do V.44 and V.42 bis, is given this XID command */
static void v44cmd(const uint8_t *f, int len)
{
    setup();
    cb.comp_tx = cb.comp_rx = 1; cb.comp_p1 = 2048; cb.comp_p2 = 32;
    cb.v44_tx = cb.v44_rx = 1;
    init_pair();
    v42lapm_rx_frame(&B, f, len, 0);
}

static void test_v44_negotiation(void)
{
    static const v44want all = { 1, 1, 2048, 2048, 255, 255, 4096, 4096 };
    static const v44want none = { 0, 0, 2048, 2048, 255, 255, 4096, 4096 };
    v44want a, b;
    uint8_t f[V42HDLC_MAX_PAYLOAD]; int fcs, n;

    /* defaults: not asked for unless the layer above says so; 2048
     * codewords, strings of 255, history 4096 */
    setup();
    CHECK_EQ_I(ca.v44_tx, 0); CHECK_EQ_I(ca.v44_rx, 0);
    CHECK_EQ_I(ca.v44_p1_tx, 2048); CHECK_EQ_I(ca.v44_p1_rx, 2048);
    CHECK_EQ_I(ca.v44_p2_tx, 255); CHECK_EQ_I(ca.v44_p2_rx, 255);
    CHECK_EQ_I(ca.v44_p3_tx, 4096); CHECK_EQ_I(ca.v44_p3_rx, 4096);

    /* The XID command of the Conexant CX93001 (see test_negotiation): V.42
     * bis with 512 codewords and, in the user data subfield, V.44 both ways
     * with 512 codewords, strings of 32 and a history of 1024. V.44 is
     * taken, with those values; the answer carries the V.44 set and no
     * V.42 bis set (V.44 7.3 NOTE). */
    {
        static const uint8_t cx[] = {
            0x03, 0xaf, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00,
            0x07, 0x01, 0x0f, 0x08, 0x01, 0x0f, 0xf0, 0x00, 0x0f, 0x00, 0x03, 0x56, 0x34, 0x32, 0x01, 0x01, 0x03, 0x02, 0x02,
            0x02, 0x00, 0x03, 0x01, 0x20, 0xff, 0x40, 0x03, 0x56, 0x34, 0x34, 0x41, 0x01, 0x00, 0x42, 0x01, 0x03, 0x43, 0x02,
            0x02, 0x00, 0x44, 0x02, 0x02, 0x00, 0x45, 0x01, 0x20, 0x46, 0x01, 0x20, 0x47, 0x02, 0x04, 0x00, 0x48, 0x02, 0x04, 0x00 };
        static const uint8_t want[] = {
            0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00,
            0x07, 0x01, 0x0f, 0x08, 0x01, 0x0f,
            0xff, 0x40, 0x03, 0x56, 0x34, 0x34, 0x41, 0x01, 0x00, 0x42, 0x01, 0x03, 0x43, 0x02, 0x02, 0x00,
            0x44, 0x02, 0x02, 0x00, 0x45, 0x01, 0x20, 0x46, 0x01, 0x20, 0x47, 0x02, 0x04, 0x00, 0x48, 0x02, 0x04, 0x00 };
        static const uint8_t want_bis[] = {
            0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04, 0x00, 0x06, 0x02, 0x04, 0x00,
            0x07, 0x01, 0x0f, 0x08, 0x01, 0x0f,
            0xf0, 0x00, 0x0f, 0x00, 0x03, 0x56, 0x34, 0x32, 0x01, 0x01, 0x03, 0x02, 0x02, 0x02, 0x00, 0x03, 0x01, 0x20 };
        v44cmd(cx, sizeof cx);
        CHECK_EQ_I(B.state, V42LAPM_IDLE);
        CHECK_EQ_I(B.v44_tx, 1); CHECK_EQ_I(B.v44_rx, 1);
        CHECK_EQ_I(B.v44_n2_tx, 512); CHECK_EQ_I(B.v44_n2_rx, 512);
        CHECK_EQ_I(B.v44_n7_tx, 32); CHECK_EQ_I(B.v44_n7_rx, 32);
        CHECK_EQ_I(B.v44_n8_tx, 1024); CHECK_EQ_I(B.v44_n8_rx, 1024);
        CHECK_EQ_I(B.comp_tx, 0); CHECK_EQ_I(B.comp_rx, 0);
        CHECK_EQ_I(B.k_rx, 15); CHECK_EQ_I(B.n401_rx, 128);
        CHECK_EQ_I(B.xid_resp_len, (int)sizeof want);
        CHECK(memcmp(B.xid_resp, want, sizeof want) == 0);
        n = v42lapm_tx_frame(&B, f, &fcs);
        CHECK_EQ_I(n, 2 + (int)sizeof want);
        CHECK(f[0] == 0x03 && f[1] == 0xAF && memcmp(f + 2, want, sizeof want) == 0);

        /* the same caller without the user data subfield: V.42 bis, as
         * before V.44 was there */
        v44cmd(cx, 43);
        CHECK_EQ_I(B.state, V42LAPM_IDLE);
        CHECK_EQ_I(B.v44_tx, 0); CHECK_EQ_I(B.v44_rx, 0);
        CHECK_EQ_I(B.comp_tx, 1); CHECK_EQ_I(B.comp_rx, 1);
        CHECK_EQ_I(B.comp_n2, 512); CHECK_EQ_I(B.comp_n7, 32);
        CHECK_EQ_I(B.xid_resp_len, (int)sizeof want_bis);
        CHECK(memcmp(B.xid_resp, want_bis, sizeof want_bis) == 0);

        /* both offered, V.44 switched off here: V.42 bis */
        setup(); cb.comp_tx = cb.comp_rx = 1; cb.comp_p2 = 32; init_pair();
        v42lapm_rx_frame(&B, cx, sizeof cx, 0);
        CHECK_EQ_I(B.v44_tx, 0); CHECK_EQ_I(B.v44_rx, 0);
        CHECK_EQ_I(B.comp_tx, 1); CHECK_EQ_I(B.comp_rx, 1);
        CHECK_EQ_I(B.xid_resp_len, (int)sizeof want_bis);
        CHECK(memcmp(B.xid_resp, want_bis, sizeof want_bis) == 0);
        /* both offered, only V.44 wanted here */
        setup(); cb.v44_tx = cb.v44_rx = 1; init_pair();
        v42lapm_rx_frame(&B, cx, sizeof cx, 0);
        CHECK_EQ_I(B.v44_tx, 1); CHECK_EQ_I(B.v44_rx, 1); CHECK_EQ_I(B.comp_tx, 0);
        CHECK_EQ_I(B.xid_resp_len, (int)sizeof want);
        /* both offered, nothing wanted here: the V.42 bis set says "neither
         * direction" as it always did, and there is no V.44 set */
        setup(); init_pair();
        v42lapm_rx_frame(&B, cx, sizeof cx, 0);
        CHECK_EQ_I(B.v44_tx, 0); CHECK_EQ_I(B.comp_tx, 0); CHECK_EQ_I(B.comp_rx, 0);
        CHECK_EQ_I(B.xid_resp_len, (int)sizeof want_bis);
        CHECK_EQ_I(B.xid_resp[33], 0);
    }

    /* a caller that offers V.44 only */
    {
        static const uint8_t only[] = { XID_HEAD, V44_SET(3, 2048, 1024, 255, 100, 4096, 2000) };
        static const uint8_t want[] = { V44_SET(3, 1024, 2048, 100, 255, 2000, 4096) };
        v44cmd(only, sizeof only);
        CHECK_EQ_I(B.state, V42LAPM_IDLE);
        CHECK_EQ_I(B.v44_tx, 1); CHECK_EQ_I(B.v44_rx, 1);
        CHECK_EQ_I(B.comp_tx, 0); CHECK_EQ_I(B.comp_rx, 0);
        /* its transmit direction is our receive direction */
        CHECK_EQ_I(B.v44_n2_rx, 2048); CHECK_EQ_I(B.v44_n7_rx, 255); CHECK_EQ_I(B.v44_n8_rx, 4096);
        CHECK_EQ_I(B.v44_n2_tx, 1024); CHECK_EQ_I(B.v44_n7_tx, 100); CHECK_EQ_I(B.v44_n8_tx, 2000);
        CHECK_EQ_I(B.xid_resp_len, 24 + (int)sizeof want);
        CHECK(memcmp(B.xid_resp + 24, want, sizeof want) == 0);
        /* ... to an answerer that does not want V.44: no compression, and
         * the answer has no user data subfield */
        setup(); cb.comp_tx = cb.comp_rx = 1; init_pair();
        v42lapm_rx_frame(&B, only, sizeof only, 0);
        CHECK_EQ_I(B.v44_tx, 0); CHECK_EQ_I(B.v44_rx, 0);
        CHECK_EQ_I(B.comp_tx, 0); CHECK_EQ_I(B.comp_rx, 0);
        CHECK_EQ_I(B.xid_resp_len, 24);
    }

    /* parameters left out take the defaults of Table 10: both directions,
     * 1024 codewords, strings of 255, history three times the codewords;
     * "*44" is the set identifier as V.42 Table 11c NOTE 2 spells it */
    {
        static const uint8_t bare[] = { XID_HEAD, 0xFF, 0x40, 0x03, 0x2A, '4', '4', 0x41, 0x01, 0x00 };
        static const uint8_t some[] = { XID_HEAD, 0xFF, 0xFF, 0x02, 0x12, 0x34,     /* a manufacturer ID first */
                                        0x40, 0x03, 'V', '4', '4', 0x41, 0x01, 0xC0,
                                        0x43, 0x02, 0x01, 0x00, 0x46, 0x01, 0x40 };   /* P1T = 256, P2R = 64 */
        static const uint8_t want[] = { V44_SET(3, 1024, 256, 64, 255, 3072, 768) };
        v44cmd(bare, sizeof bare);
        CHECK_EQ_I(B.v44_tx, 1); CHECK_EQ_I(B.v44_rx, 1);
        CHECK_EQ_I(B.v44_n2_tx, 1024); CHECK_EQ_I(B.v44_n2_rx, 1024);
        CHECK_EQ_I(B.v44_n7_tx, 255); CHECK_EQ_I(B.v44_n7_rx, 255);
        CHECK_EQ_I(B.v44_n8_tx, 3072); CHECK_EQ_I(B.v44_n8_rx, 3072);
        v44cmd(some, sizeof some);
        CHECK_EQ_I(B.v44_tx, 1); CHECK_EQ_I(B.v44_rx, 1);
        CHECK_EQ_I(B.v44_n2_rx, 256); CHECK_EQ_I(B.v44_n8_rx, 768); CHECK_EQ_I(B.v44_n7_rx, 255);
        CHECK_EQ_I(B.v44_n2_tx, 1024); CHECK_EQ_I(B.v44_n8_tx, 3072); CHECK_EQ_I(B.v44_n7_tx, 64);
        CHECK_EQ_I(B.xid_resp_len, 24 + (int)sizeof want);
        CHECK(memcmp(B.xid_resp + 24, want, sizeof want) == 0);
    }
    /* C0 asking for negotiation after link establishment (7.4.2): not
     * supported; answered with the XID values and N = 0 */
    {
        static const uint8_t pm[] = { XID_HEAD, 0xFF, 0x40, 0x03, 'V', '4', '4', 0x41, 0x01, 0x01,
                                      0x42, 0x01, 0x03, 0x43, 0x02, 0x08, 0x00, 0x44, 0x02, 0x08, 0x00 };
        v44cmd(pm, sizeof pm);
        CHECK_EQ_I(B.v44_tx, 1); CHECK_EQ_I(B.v44_rx, 1);
        CHECK_EQ_I(B.v44_n2_tx, 2048); CHECK_EQ_I(B.v44_n8_rx, 4096);
        CHECK_EQ_I(B.xid_resp[24 + 8], 0x00);
    }
    /* V.44 parameters without the set identifier, or under another one,
     * are not V.44 parameters; a subfield cut short is used as far as it
     * goes */
    {
        static const uint8_t noid[] = { XID_HEAD, 0xFF, 0x41, 0x01, 0x00, 0x42, 0x01, 0x03 };
        static const uint8_t other[] = { XID_HEAD, 0xFF, 0x40, 0x03, 'V', '4', '5', 0x41, 0x01, 0x00, 0x42, 0x01, 0x03 };
        static const uint8_t cut[] = { XID_HEAD, V44_SET(3, 512, 512, 32, 32, 1024, 1024) };
        v44cmd(noid, sizeof noid);
        CHECK_EQ_I(B.state, V42LAPM_IDLE); CHECK_EQ_I(B.v44_tx, 0); CHECK_EQ_I(B.v44_rx, 0);
        CHECK_EQ_I(B.xid_resp_len, 24);
        v44cmd(other, sizeof other);
        CHECK_EQ_I(B.v44_tx, 0); CHECK_EQ_I(B.v44_rx, 0);
        v44cmd(cut, (int)sizeof cut - 3);       /* P3R lost its last octet */
        CHECK_EQ_I(B.v44_tx, 1);
        CHECK_EQ_I(B.v44_n8_rx, 1024); CHECK_EQ_I(B.v44_n8_tx, 1536);
        v44cmd(cut, 26 + 1);                    /* just the group identifier */
        CHECK_EQ_I(B.state, V42LAPM_IDLE); CHECK_EQ_I(B.v44_tx, 0);
    }
    /* 7.4: a value below the minimum is a procedural error and ends it */
    {
        static const uint8_t p1[] = { XID_HEAD, V44_SET(3, 255, 512, 32, 32, 1024, 1024) };
        static const uint8_t p2[] = { XID_HEAD, V44_SET(3, 512, 512, 32, 31, 1024, 1024) };
        static const uint8_t p3[] = { XID_HEAD, V44_SET(3, 512, 512, 32, 32, 511, 1024) };
        static const uint8_t ok[] = { XID_HEAD, V44_SET(3, 256, 256, 32, 32, 512, 512) };
        v44cmd(p1, sizeof p1);
        CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(B.reason, V42LAPM_R_NEGOTIATION);
        v44cmd(p2, sizeof p2);
        CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(B.reason, V42LAPM_R_NEGOTIATION);
        v44cmd(p3, sizeof p3);
        CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(B.reason, V42LAPM_R_NEGOTIATION);
        v44cmd(ok, sizeof ok);
        CHECK_EQ_I(B.state, V42LAPM_IDLE);
        CHECK_EQ_I(B.v44_n2_tx, 256); CHECK_EQ_I(B.v44_n7_rx, 32); CHECK_EQ_I(B.v44_n8_tx, 512);
    }

    /* the initiator's command: V.42 bis in the private parameter subfield,
     * V.44 in the user data subfield behind it (V.42 12.2.1.3). T and R
     * are the sender's transmit and receive directions (V.44 7.4). */
    {
        static const uint8_t want[] = { XID_CMD_HEAD, V42BIS_SET, V44_SET(3, 2048, 1024, 255, 64, 4096, 3000) };
        static const uint8_t want44[] = { XID_CMD_HEAD, V44_SET(1, 2048, 1024, 255, 64, 4096, 3000) };
        static const uint8_t both[] = { 0x03, 0xAF, 0x82, V42BIS_SET, V44_SET(3, 512, 512, 32, 32, 1024, 1024) };
        static const uint8_t wrong[] = { 0x03, 0xAF, 0x82, V44_SET(3, 512, 512, 32, 32, 1024, 1024) };
        static const uint8_t low[] = { 0x03, 0xAF, 0x82, V44_SET(2, 512, 100, 32, 32, 1024, 1024) };
        static const uint8_t fine[] = { 0x03, 0xAF, 0x82, V44_SET(2, 600, 700, 40, 50, 800, 900) };
        setup();
        ca.comp_tx = ca.comp_rx = 1; ca.comp_p1 = 512; ca.comp_p2 = 32;
        a = all; a.p1r = 1024; a.p2r = 64; a.p3r = 3000; v44cfg(&ca, a);
        init_pair(); v42lapm_start(&A);
        n = v42lapm_tx_frame(&A, f, &fcs);
        CHECK_EQ_I(n, (int)sizeof want);
        CHECK(memcmp(f, want, sizeof want) == 0);
        /* a response with both compression procedures in it */
        v42lapm_rx_frame(&A, both, sizeof both, 0);
        CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(A.reason, V42LAPM_R_NEGOTIATION);

        /* only our transmit direction asked for */
        setup(); a.rx = 0; v44cfg(&ca, a);
        init_pair(); v42lapm_start(&A);
        n = v42lapm_tx_frame(&A, f, &fcs);
        CHECK_EQ_I(n, (int)sizeof want44);
        CHECK(memcmp(f, want44, sizeof want44) == 0);
        /* the complementary answer is "receive only" (7.4): taken, with
         * the lesser of each pair of values */
        v42lapm_rx_frame(&A, fine, sizeof fine, 0);
        CHECK_EQ_I(A.state, V42LAPM_ESTABLISHING);
        CHECK_EQ_I(A.v44_tx, 1); CHECK_EQ_I(A.v44_rx, 0);
        CHECK_EQ_I(A.v44_n2_tx, 700); CHECK_EQ_I(A.v44_n7_tx, 50); CHECK_EQ_I(A.v44_n8_tx, 900);
        /* an answer granting a direction that was not asked for */
        init_pair(); v42lapm_start(&A); v42lapm_tx_frame(&A, f, &fcs);
        v42lapm_rx_frame(&A, wrong, sizeof wrong, 0);
        CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(A.reason, V42LAPM_R_NEGOTIATION);
        /* an answer with a value below the minimum */
        init_pair(); v42lapm_start(&A); v42lapm_tx_frame(&A, f, &fcs);
        v42lapm_rx_frame(&A, low, sizeof low, 0);
        CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(A.reason, V42LAPM_R_NEGOTIATION);
        /* an answer without V.44: none, and no V.42 bis either if that was
         * not in it */
        init_pair(); v42lapm_start(&A); v42lapm_tx_frame(&A, f, &fcs);
        v42lapm_rx_frame(&A, wrong, 3, 0);
        CHECK_EQ_I(A.state, V42LAPM_ESTABLISHING);
        CHECK_EQ_I(A.v44_tx, 0); CHECK_EQ_I(A.v44_rx, 0); CHECK_EQ_I(A.comp_tx, 0);
    }

    /* two of ours: every direction rule, the lesser value wins, each
     * direction on its own */
    v44neg(all, all, 1, 2048, 255, 4096, 1, 2048, 255, 4096);
    v44neg(all, none, 0, 0, 0, 0, 0, 0, 0, 0);
    v44neg(none, all, 0, 0, 0, 0, 0, 0, 0, 0);
    a = all; a.rx = 0; v44neg(a, all, 1, 2048, 255, 4096, 0, 0, 0, 0);
    a = all; a.tx = 0; v44neg(a, all, 0, 0, 0, 0, 1, 2048, 255, 4096);
    b = all; b.rx = 0; v44neg(all, b, 0, 0, 0, 0, 1, 2048, 255, 4096);
    b = all; b.tx = 0; v44neg(all, b, 1, 2048, 255, 4096, 0, 0, 0, 0);
    a = all; a.rx = 0; b = all; b.rx = 0; v44neg(a, b, 0, 0, 0, 0, 0, 0, 0, 0);     /* disjoint wishes */
    a = all; a.p1t = 512; a.p2t = 32; a.p3t = 1024;
    v44neg(a, all, 1, 512, 32, 1024, 1, 2048, 255, 4096);
    v44neg(all, a, 1, 2048, 255, 4096, 1, 512, 32, 1024);
    a = all; a.p1r = 256; a.p2r = 100; a.p3r = 512;
    v44neg(a, all, 1, 2048, 255, 4096, 1, 256, 100, 512);
    v44neg(all, a, 1, 256, 100, 512, 1, 2048, 255, 4096);
    a.tx = 1; a.rx = 1; a.p1t = 1000; a.p1r = 300; a.p2t = 200; a.p2r = 33; a.p3t = 3000; a.p3r = 600;
    b.tx = 1; b.rx = 1; b.p1t = 400; b.p1r = 2000; b.p2t = 40; b.p2r = 100; b.p3t = 700; b.p3r = 2500;
    v44neg(a, b, 1, 1000, 100, 2500, 1, 300, 33, 600);
    v44neg(b, a, 1, 300, 33, 600, 1, 1000, 100, 2500);
    /* values outside the ranges of Table 10 are brought inside them */
    a = all; a.p1t = 10; a.p2t = 1000; a.p3t = 0; a.p1r = 100000; a.p2r = 5; a.p3r = 100000;
    v44neg(a, all, 1, 256, 255, 512, 1, 2048, 32, 4096);

    /* V.44 is chosen over V.42 bis when both ends can do both; V.42 bis
     * where one end has no V.44 */
    setup();
    ca.comp_tx = ca.comp_rx = cb.comp_tx = cb.comp_rx = 1;
    v44cfg(&ca, all); v44cfg(&cb, all);
    CHECK(connect_pair());
    CHECK(A.v44_tx && A.v44_rx && B.v44_tx && B.v44_rx);
    CHECK(!A.comp_tx && !A.comp_rx && !B.comp_tx && !B.comp_rx);
    setup();
    ca.comp_tx = ca.comp_rx = cb.comp_tx = cb.comp_rx = 1;
    v44cfg(&ca, all);
    CHECK(connect_pair());
    CHECK(!A.v44_tx && !A.v44_rx && !B.v44_tx && !B.v44_rx);
    CHECK(A.comp_tx && A.comp_rx && B.comp_tx && B.comp_rx);
    setup();
    ca.comp_tx = ca.comp_rx = cb.comp_tx = cb.comp_rx = 1;
    v44cfg(&cb, all);
    CHECK(connect_pair());
    CHECK(!A.v44_tx && !B.v44_rx && A.comp_tx && B.comp_rx);
    /* no XID: no V.44 */
    setup(); ca.use_xid = 0; v44cfg(&ca, all); v44cfg(&cb, all);
    CHECK(connect_pair());
    CHECK(!A.v44_tx && !A.v44_rx && !B.v44_tx && !B.v44_rx);

    /* an XID command on the established connection changes nothing and is
     * answered with what is in force */
    {
        static const uint8_t again[] = { XID_HEAD, V42BIS_SET, V44_SET(3, 256, 256, 32, 32, 512, 512) };
        static const uint8_t want[] = { V44_SET(3, 2048, 2048, 255, 255, 4096, 4096) };
        setup(); v44cfg(&ca, all); v44cfg(&cb, all);
        CHECK(connect_pair());
        v42lapm_rx_frame(&B, again, sizeof again, 0);
        CHECK_EQ_I(B.state, V42LAPM_CONNECTED);
        CHECK_EQ_I(B.v44_n2_tx, 2048); CHECK_EQ_I(B.v44_n8_rx, 4096);
        CHECK_EQ_I(B.xid_resp_len, 24 + (int)sizeof want);
        CHECK(memcmp(B.xid_resp + 24, want, sizeof want) == 0);
    }
}

static void test_data(void)
{
    /* both directions at once, more than 128 frames each way */
    setup();
    CHECK(connect_pair());
    nlog = 0;
    flow_reset(&ab, 30000, 100); flow_reset(&ba, 21000, 128);
    run(1000);
    CHECK(flows_ok());
    CHECK_EQ_I(A.tx_i, 300); CHECK_EQ_I(A.tx_i_retx, 0);
    CHECK_EQ_I(B.rx_i, 300);
    CHECK_EQ_I(v42lapm_unacked(&A), 0); CHECK_EQ_I(v42lapm_unacked(&B), 0);
    /* first I frame: N(S)=0, N(R)=0, P=0, then the data (Table 8) */
    CHECK(flog[0].from_a == 1 && flog[0].len == 103);
    CHECK(flog[0].f[0] == 0x03 && flog[0].f[1] == 0x00 && flog[0].f[2] == 0x00);
    CHECK(memcmp(flog[0].f + 3, ab.src, 37) == 0);
    /* the answerer's commands carry C/R = 0 */
    CHECK(flog[1].from_a == 0 && flog[1].f[0] == 0x01 && flog[1].f[1] == 0x00);
    /* one direction only: acknowledged by RR responses, F=0 */
    setup();
    CHECK(connect_pair());
    nlog = 0;
    flow_reset(&ab, 5000, 50);
    run(300);
    CHECK(flows_ok());
    CHECK(count_ctrl(0, 0x01) >= 1);
    CHECK(flog[1].from_a == 0 && flog[1].len == 3 && flog[1].f[0] == 0x03 && flog[1].f[1] == 0x01 && flog[1].f[2] == 0x02);
    /* an information field of exactly N401 passes; nothing longer is built */
    CHECK_EQ_I(v42lapm_can_send(&A), 128);
    CHECK_EQ_I(v42lapm_send(&A, ab.src, 129), -1);
}

static int acks_blocked;
static int block_b(int from_a, const uint8_t *f, int len) { (void)f; (void)len; return from_a || !acks_blocked; }

static void test_window(void)
{
    /* 8.4.1: never more than k outstanding */
    setup(); ca.k_tx = 4; ca.k_rx = 4;
    CHECK(connect_pair());
    CHECK_EQ_I(A.k_tx, 4);
    nlog = 0; filt = block_b; acks_blocked = 1;
    flow_reset(&ab, 3000, 100);
    run(200);
    CHECK_EQ_I(A.tx_i, 4);
    CHECK_EQ_I(v42lapm_unacked(&A), 4);
    CHECK_EQ_I(v42lapm_can_send(&A), 0);
    CHECK_EQ_I(ab.ngot, 400);
    acks_blocked = 0;
    run(2000);
    CHECK(flows_ok());
    CHECK(A.state == V42LAPM_CONNECTED);
}

static int lose_ns, lose_left;
static int lose_i_from_a(int from_a, const uint8_t *f, int len)
{
    if (from_a && is_i(f, len) && (f[1] >> 1) == lose_ns && lose_left > 0) { lose_left--; return 0; }
    return 1;
}

static void test_rej_recovery(void)
{
    /* 8.5.1: an I frame is lost, the next one is out of sequence, the
     * receiver sends one REJ and the sender goes back */
    setup();
    CHECK(connect_pair());
    nlog = 0; filt = lose_i_from_a; lose_ns = 2; lose_left = 1;
    flow_reset(&ab, 2000, 100);
    run(100);
    CHECK(flows_ok());
    CHECK_EQ_I(B.tx_rej, 1); CHECK_EQ_I(A.rx_rej, 1);
    CHECK_EQ_I(COUNT(0, 0x03, 0x09, 0x04), 1);       /* REJ response, N(R)=2 */
    CHECK(A.tx_i_retx >= 1);
    CHECK_EQ_I(A.t401_expiries, 0);                  /* recovered without the timer */
    CHECK(now_ms < 200);
    /* the retransmitted frame is lost again: no second REJ may be sent for
     * the same condition (8.5.1), so T401 and the poll sort it out */
    setup();
    CHECK(connect_pair());
    nlog = 0; filt = lose_i_from_a; lose_ns = 5; lose_left = 2;
    flow_reset(&ab, 3000, 100);
    run(3000);
    CHECK(flows_ok());
    CHECK_EQ_I(B.tx_rej, 1);
    CHECK(A.t401_expiries >= 1);
}

static void test_t401_recovery(void)
{
    /* 8.5.3 / 8.4.8: the last frame of a burst is lost, so no REJ can
     * come; T401 expires, RR command P=1, RR response F=1, retransmission */
    setup();
    CHECK(connect_pair());
    nlog = 0; now_ms = 0; filt = lose_i_from_a; lose_ns = 0; lose_left = 1;
    flow_reset(&ab, 60, 100);
    run(ca.t401_ms - 10);
    CHECK_EQ_I(ab.ngot, 0);
    CHECK_EQ_I(nlog, 1);
    run(100);
    CHECK(flows_ok());
    CHECK_EQ_I(A.t401_expiries, 1);
    CHECK_EQ_I(nlog, 5);
    CHECK(flog[1].from_a == 1 && flog[1].len == 3 && flog[1].f[0] == 0x03 && flog[1].f[1] == 0x01 && flog[1].f[2] == 0x01);
    CHECK(flog[1].t >= ca.t401_ms - 1 && flog[1].t <= ca.t401_ms + 2);
    CHECK(flog[2].from_a == 0 && flog[2].len == 3 && flog[2].f[0] == 0x03 && flog[2].f[1] == 0x01 && flog[2].f[2] == 0x01);
    CHECK(flog[3].from_a == 1 && is_i(flog[3].f, flog[3].len) && flog[3].f[1] == 0x00);
    CHECK(flog[4].from_a == 0 && flog[4].f[1] == 0x01 && flog[4].f[2] == 0x02);
    CHECK_EQ_I(A.timer_rec, 0);
}

static void test_line_restored(void)
{
    /* The modem under the link stops carrying data for a while (a retrain
     * or a rate renegotiation) and says when it carries data again: the
     * link asks where the peer stands at once, not when T401 next runs out. */
    int t0, n0;
    setup();
    CHECK(connect_pair());
    flow_reset(&ab, 3000, 100);
    run(3);
    filt = drop_all;
    run(2500);                              /* 2.5 s with nothing either way */
    CHECK(!flows_ok());
    filt = NULL; nlog = 0; t0 = now_ms; n0 = (int)A.t401_expiries;
    v42lapm_line_restored(&A);
    run(60);
    /* the first frame out is the poll: RR command, P = 1 */
    CHECK(nlog >= 1 && flog[0].from_a == 1 && flog[0].len == 3 && flog[0].f[1] == 0x01 && (flog[0].f[2] & 1) == 1);
    CHECK(flog[0].t - t0 <= 5);
    CHECK_EQ_I((int)A.t401_expiries, n0);
    run(3000);
    CHECK(flows_ok());
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);

    /* nothing outstanding, no recovery under way: nothing is sent for it */
    setup();
    CHECK(connect_pair());
    run(200); nlog = 0;
    v42lapm_line_restored(&A);
    run(50);
    CHECK_EQ_I(nlog, 0);
    /* not connected: ignored */
    setup(); init_pair();
    v42lapm_line_restored(&A);
    CHECK_EQ_I(A.state, V42LAPM_IDLE);
}

static int after_connect_drop_a;
static int drop_a_later(int from_a, const uint8_t *f, int len) { (void)f; (void)len; return !(from_a && after_connect_drop_a); }

static void test_n400(void)
{
    /* SABME never answered: 1 + N400 attempts, then L-RELEASE (8.3.2.2) */
    setup(); ca.use_xid = 0; ca.n400 = 4; filt = drop_all;
    init_pair(); v42lapm_start(&A);
    run(4 * ca.t401_ms + 500);
    CHECK_EQ_I(A.state, V42LAPM_ESTABLISHING);
    run(ca.t401_ms);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(A.reason, V42LAPM_R_N400);
    CHECK_EQ_I(COUNT(1, 0x03, 0x7F), 5);
    run(5000);
    CHECK_EQ_I(nlog, 5);                             /* and then silence */

    /* peer vanishes with data outstanding: N400 polls, then termination
     * (8.4.8 b) */
    setup(); ca.n400 = 3;
    CHECK(connect_pair());
    nlog = 0; filt = drop_a_later; after_connect_drop_a = 1;
    flow_reset(&ab, 500, 100);
    run(3 * ca.t401_ms + 500);
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);
    run(ca.t401_ms);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(A.reason, V42LAPM_R_N400);
    CHECK_EQ_I(A.tx_polls, 3);
    CHECK_EQ_I(count_ctrl(1, 0x01), 3);
    CHECK_EQ_I(B.state, V42LAPM_CONNECTED);          /* B heard nothing */
    CHECK_EQ_I(v42lapm_can_send(&A), 0);
}

/* D9: a peer that answers every poll (RR F=1, N(R) = V(A)) and never
 * acknowledges an I frame: N400 recoveries without progress release the
 * link (V.42 restarts the count at each recovery; ours does not) */
static int drop_i_from_a(int from_a, const uint8_t *f, int len) { return !(from_a && is_i(f, len)); }
/* B never sees A's I frames and answers each with REJ, N(R) = A's V(A) */
static int rej_every_i(int from_a, const uint8_t *f, int len)
{
    if (from_a && is_i(f, len)) {
        uint8_t rej[3] = { 0x03, 0x09, 0 };
        rej[2] = (uint8_t)(A.va << 1);
        v42lapm_rx_frame(&A, rej, 3, 0);
        return 0;
    }
    return 1;
}
static int drop_i_some(int from_a, const uint8_t *f, int len) { return !(from_a && is_i(f, len) && now_ms % 400 < 150); }

static void test_no_progress(void)
{
    int t0;
    setup(); ca.t401_ms = 100; ca.n400 = 3;
    CHECK(connect_pair());
    filt = drop_i_from_a; nlog = 0; t0 = now_ms;
    flow_reset(&ab, 2000, 100);
    while (A.state == V42LAPM_CONNECTED && now_ms - t0 < 60000) run(1);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(A.reason, V42LAPM_R_N400);
    /* N400 whole recoveries, each polled and answered; released as the
     * next one begins */
    CHECK_EQ_I(A.t401_expiries, (unsigned long)ca.n400 + 1);
    CHECK_EQ_I(A.tx_polls, (unsigned long)ca.n400);
    CHECK(now_ms - t0 <= (ca.n400 + 1) * ca.t401_ms + 20);
    CHECK_EQ_I(count_ctrl(0, 0x01), ca.n400);        /* the polls were answered */
    CHECK_EQ_I(B.state, V42LAPM_CONNECTED);

    /* N400 = 1: one lost I frame is still polled for and recovered */
    setup(); ca.n400 = 1;
    CHECK(connect_pair());
    nlog = 0; filt = lose_i_from_a; lose_ns = 0; lose_left = 1;
    flow_reset(&ab, 60, 100);
    run(5000);
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);
    CHECK_EQ_I(A.tx_polls, 1);
    CHECK(flows_ok());
    /* the same with the line stopped and restored in the middle */
    setup(); ca.n400 = 1;
    CHECK(connect_pair());
    filt = drop_all; flow_reset(&ab, 300, 100);
    run(50);
    filt = NULL;
    v42lapm_line_restored(&A);
    run(3000);
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);
    CHECK(flows_ok());

    /* a peer that answers every I frame with REJ, N(R) = V(A): each such
     * go-back is a recovery without progress (T401 never runs out) */
    setup(); ca.t401_ms = 100; ca.n400 = 3;
    CHECK(connect_pair());
    filt = rej_every_i; t0 = now_ms;
    flow_reset(&ab, 2000, 100);
    while (A.state == V42LAPM_CONNECTED && now_ms - t0 < 60000) run(1);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(A.reason, V42LAPM_R_N400);
    CHECK_EQ_I(A.t401_expiries, 0);
    CHECK_EQ_I(A.rx_rej, (unsigned long)ca.n400 + 1);
    CHECK(now_ms - t0 <= 50);

    /* a destructive break (the data path reset) does not start the count
     * again: no data moved */
    setup(); ca.t401_ms = 100; ca.n400 = 3;
    CHECK(connect_pair());
    filt = drop_i_from_a; t0 = now_ms;
    flow_reset(&ab, 20000, 100);
    while (A.t401_expiries < 2 && now_ms - t0 < 5000) run(1);
    CHECK_EQ_I(v42lapm_send_break(&A, V42_BRK_DESTRUCTIVE_EXPEDITED, 0), 0);
    while (A.state == V42LAPM_CONNECTED && now_ms - t0 < 60000) run(1);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(A.reason, V42LAPM_R_N400);
    CHECK_EQ_I(A.t401_expiries, (unsigned long)ca.n400 + 1);
    CHECK_EQ_I(A.resets, 1);

    /* progress in between starts the count again: the I frames are lost
     * for 150 ms of every 400, two recoveries each time, more than N400 in
     * all; the transfer completes */
    setup(); ca.t401_ms = 100; ca.n400 = 3;
    CHECK(connect_pair());
    filt = drop_i_some; now_ms = 0;
    flow_reset(&ab, 60000, 100);
    run(10000);
    CHECK(flows_ok());
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);
    CHECK(A.t401_expiries > 2 * (unsigned long)ca.n400);
}

static int ua_drops;
static int drop_ua(int from_a, const uint8_t *f, int len)
{
    (void)len;
    if (!from_a && f[1] == 0x73 && ua_drops > 0) { ua_drops--; return 0; }
    return 1;
}

static void test_ua_lost(void)
{
    /* 8.4.9.1 first item: the answerer tolerates a repeated SABME while
     * the link is being set up */
    setup(); ca.use_xid = 0; filt = drop_ua; ua_drops = 1;
    init_pair(); v42lapm_start(&A);
    run(100);
    CHECK_EQ_I(A.state, V42LAPM_ESTABLISHING);
    CHECK_EQ_I(B.state, V42LAPM_CONNECTED);
    run(ca.t401_ms);
    CHECK(A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED);
    CHECK_EQ_I(COUNT(1, 0x03, 0x7F), 2);
    CHECK_EQ_I(COUNT(0, 0x03, 0x73), 2);
    flow_reset(&ab, 3000, 100); flow_reset(&ba, 3000, 100);
    run(300);
    CHECK(flows_ok());

    /* same, but the answerer already has data to send: the originator may
     * take the I frame as proof that UA was sent (8.3.2.1 last paragraph) */
    setup(); ca.use_xid = 0; filt = drop_ua; ua_drops = 1;
    init_pair(); v42lapm_start(&A);
    flow_reset(&ba, 2500, 100); flow_reset(&ab, 1200, 100);
    run(400);
    CHECK(A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED);
    CHECK_EQ_I(COUNT(1, 0x03, 0x7F), 1);
    CHECK(flows_ok());
}

static void test_busy(void)
{
    int tx_before;

    /* 8.4.7 / 8.4.6: RNR stops the sender; it enquires with RR P=1 every
     * T401 and is answered RNR F=1 for as long as the condition lasts */
    setup(); ca.n400 = 3;
    CHECK(connect_pair());
    flow_reset(&ab, 20000, 100);
    run(20);
    nlog = 0;
    v42lapm_set_busy(&B, 1);
    run(5);
    CHECK_EQ_I(count_ctrl(0, 0x05), 1);              /* RNR */
    CHECK_EQ_I(A.peer_busy, 1);
    tx_before = (int)A.tx_i;
    run(8 * ca.t401_ms);
    CHECK_EQ_I(A.tx_i, tx_before);
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);          /* longer than N400 x T401 */
    CHECK(A.tx_polls >= 6);
    CHECK(COUNT(0, 0x03, 0x05) >= 7);
    CHECK(ab.ngot < 20000);
    v42lapm_set_busy(&B, 0);
    run(2000);
    CHECK_EQ_I(A.peer_busy, 0);
    CHECK(flows_ok());
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);

    /* busy with frames in flight: they are discarded and sent again */
    setup();
    CHECK(connect_pair());
    flow_reset(&ab, 30000, 128); flow_reset(&ba, 9000, 60);
    {
        int i;
        for (i = 0; i < 40; i++) {
            run(3 + (int)(rnd() % 20));
            v42lapm_set_busy(&B, i & 1);
            if (rnd() % 3 == 0) v42lapm_set_busy(&A, (int)(rnd() & 1));
        }
        v42lapm_set_busy(&A, 0); v42lapm_set_busy(&B, 0);
    }
    run(4000);
    CHECK(flows_ok());
    CHECK(B.rx_i_dropped > 0);

    /* the user stops reading without declaring busy: the held frame fills
     * the receiver and it turns busy by itself; nothing is lost */
    setup();
    CHECK(connect_pair());
    flow_reset(&ab, 8000, 100);
    ab.reading = 0;
    run(3000);
    CHECK_EQ_I(ab.ngot, 0);
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);
    ab.reading = 1;
    run(3000);
    CHECK(flows_ok());
}

static int disc_drops;
static int drop_disc(int from_a, const uint8_t *f, int len)
{
    (void)len; (void)from_a;
    if (f[1] == 0x53 && disc_drops > 0) { disc_drops--; return 0; }
    return 1;
}

static void test_release(void)
{
    uint8_t f[V42HDLC_MAX_PAYLOAD]; int fcs;
    static const uint8_t disc_from_a[] = { 0x03, 0x53 };

    /* 8.7.2 from the originator: DISC P=1 is 03 53, UA F=1 is 03 73 */
    setup();
    CHECK(connect_pair());
    nlog = 0;
    v42lapm_release(&A);
    CHECK_EQ_I(A.state, V42LAPM_RELEASING);
    run(10);
    CHECK_EQ_I(nlog, 2);
    CHECK(flog[0].from_a == 1 && flog[0].f[0] == 0x03 && flog[0].f[1] == 0x53);
    CHECK(flog[1].from_a == 0 && flog[1].f[0] == 0x03 && flog[1].f[1] == 0x73);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(A.reason, V42LAPM_R_LOCAL);
    CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(B.reason, V42LAPM_R_PEER_DISC);
    /* 8.8: DISC in the disconnected state is answered DM with F = P */
    v42lapm_rx_frame(&B, disc_from_a, 2, 0);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 2);
    CHECK(f[0] == 0x03 && f[1] == 0x1F);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 0);

    /* from the answerer: its commands use C/R = 0 */
    setup();
    CHECK(connect_pair());
    nlog = 0;
    v42lapm_release(&B);
    run(10);
    CHECK(flog[0].from_a == 0 && flog[0].f[0] == 0x01 && flog[0].f[1] == 0x53);
    CHECK(flog[1].from_a == 1 && flog[1].f[0] == 0x01 && flog[1].f[1] == 0x73);
    CHECK_EQ_I(B.reason, V42LAPM_R_LOCAL); CHECK_EQ_I(A.reason, V42LAPM_R_PEER_DISC);

    /* release with data queued and unacknowledged: discarded (8.7.1) */
    setup();
    CHECK(connect_pair());
    flow_reset(&ab, 5000, 100);
    run(3);
    v42lapm_release(&A);
    run(20);
    CHECK(A.state == V42LAPM_DISCONNECTED && B.state == V42LAPM_DISCONNECTED);
    CHECK(ab.ngot < 5000);
    CHECK_EQ_I(v42lapm_can_send(&A), 0);

    /* 8.7.3: DISC lost twice, sent again each T401 */
    setup();
    CHECK(connect_pair());
    nlog = 0; filt = drop_disc; disc_drops = 2;
    v42lapm_release(&A);
    run(2 * ca.t401_ms + 50);
    CHECK_EQ_I(count_ctrl(1, 0x53), 3);
    CHECK(A.state == V42LAPM_DISCONNECTED && B.state == V42LAPM_DISCONNECTED);
    /* never answered: N400 retransmissions and out */
    setup(); ca.n400 = 2;
    CHECK(connect_pair());
    nlog = 0; filt = drop_all;
    v42lapm_release(&A);
    run(4 * ca.t401_ms);
    CHECK_EQ_I(count_ctrl(1, 0x53), 3);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED); CHECK_EQ_I(A.reason, V42LAPM_R_LOCAL);
    /* both release at once (8.9.1) */
    setup();
    CHECK(connect_pair());
    v42lapm_release(&A); v42lapm_release(&B);
    run(50);
    CHECK(A.state == V42LAPM_DISCONNECTED && B.state == V42LAPM_DISCONNECTED);
    /* releasing before any connection just ends quietly */
    setup(); init_pair();
    v42lapm_release(&B);
    CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 0);
}

/* inject one frame into a freshly connected B and report its state */
static int inject(const uint8_t *f, int len)
{
    setup();
    if (!connect_pair()) return -1;
    v42lapm_rx_frame(&B, f, len, 0);
    return B.state;
}
#define INJECT(...) inject((const uint8_t[]){__VA_ARGS__}, (int)sizeof((const uint8_t[]){__VA_ARGS__}))

static void test_protocol_errors(void)
{
    uint8_t big[3 + V42_MAX_N401 + 1];
    uint8_t f[V42HDLC_MAX_PAYLOAD]; int fcs;

    /* 8.5.2: N(R) outside V(A)..V(S) */
    CHECK_EQ_I(INJECT(0x01, 0x01, 0x0A), V42LAPM_DISCONNECTED);
    CHECK_EQ_I(B.reason, V42LAPM_R_PROTOCOL);
    CHECK_EQ_I(INJECT(0x03, 0x00, 0x02, 'x'), V42LAPM_DISCONNECTED);
    /* 8.4.5.1: SREJ when not agreed is an unrecognised control field */
    CHECK_EQ_I(INJECT(0x01, 0x0D, 0x00), V42LAPM_DISCONNECTED);
    /* 8.2.4.12 d): information field longer than N401 */
    memset(big, 0x55, sizeof big); big[0] = 0x03; big[1] = 0; big[2] = 0;
    CHECK_EQ_I(inject(big, 3 + 128), V42LAPM_CONNECTED);
    CHECK_EQ_I(B.rx_i, 1);
    CHECK_EQ_I(inject(big, 3 + 129), V42LAPM_DISCONNECTED);
    CHECK_EQ_I(B.reason, V42LAPM_R_PROTOCOL);
    /* 8.5.6: FRMR received */
    CHECK_EQ_I(INJECT(0x01, 0x87, 0x0D, 0x00, 0x00, 0x00, 0x01), V42LAPM_DISCONNECTED);
    /* Table 9: DM F=0 terminates; DM F=1 outside timer recovery is ignored */
    CHECK_EQ_I(INJECT(0x01, 0x0F), V42LAPM_DISCONNECTED);
    CHECK_EQ_I(B.reason, V42LAPM_R_PEER_DM);
    CHECK_EQ_I(INJECT(0x01, 0x1F), V42LAPM_CONNECTED);
    CHECK_EQ_I(B.violations, 1);
    /* undefined control fields: TEST was not negotiated; 0x2F is nothing */
    CHECK_EQ_I(INJECT(0x03, 0xE3, 'h', 'i'), V42LAPM_DISCONNECTED);
    CHECK_EQ_I(INJECT(0x03, 0x2F), V42LAPM_DISCONNECTED);
    /* 8.2.4.12 b): supervisory frame with an information field, SABME
     * with one */
    CHECK_EQ_I(INJECT(0x01, 0x01, 0x00, 0x00), V42LAPM_DISCONNECTED);
    CHECK_EQ_I(INJECT(0x03, 0x7F, 0x00), V42LAPM_DISCONNECTED);
    /* Table 9: unsolicited UA is ignored and noted */
    CHECK_EQ_I(INJECT(0x01, 0x73), V42LAPM_CONNECTED);
    CHECK_EQ_I(B.violations, 1);
    /* 8.1.3 e): other DLCI, or a two octet address: discarded silently */
    CHECK_EQ_I(INJECT(0x07, 0x53), V42LAPM_CONNECTED);
    CHECK_EQ_I(INJECT(0x02, 0x01, 0x53), V42LAPM_CONNECTED);
    CHECK_EQ_I(B.violations, 0);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 0);
    /* an I frame with the response address (our own echo, say) is not data */
    CHECK_EQ_I(INJECT(0x01, 0x00, 0x00, 'x'), V42LAPM_CONNECTED);
    CHECK_EQ_I(B.rx_i, 0);
    /* frames too short to hold their control field are invalid (8.1.3 b) */
    CHECK_EQ_I(INJECT(0x03), V42LAPM_CONNECTED);
    CHECK_EQ_I(INJECT(0x01, 0x01), V42LAPM_CONNECTED);
    CHECK_EQ_I(INJECT(0x03, 0x00), V42LAPM_CONNECTED);

    /* 8.4.9.1: SABME once data has moved means the peer restarted */
    setup();
    CHECK(connect_pair());
    flow_reset(&ab, 300, 100);
    run(20);
    v42lapm_rx_frame(&B, (const uint8_t[]){ 0x03, 0x7F }, 2, 0);
    CHECK_EQ_I(B.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(B.reason, V42LAPM_R_PROTOCOL);

    /* poll with nothing wrong: RR command P=1 gets RR response F=1 */
    setup();
    CHECK(connect_pair());
    v42lapm_rx_frame(&B, (const uint8_t[]){ 0x03, 0x01, 0x01 }, 3, 0);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 3);
    CHECK(f[0] == 0x03 && f[1] == 0x01 && f[2] == 0x01);
    /* I frame with P=1 is answered RR F=1 (8.4.2.1) */
    v42lapm_rx_frame(&B, (const uint8_t[]){ 0x03, 0x00, 0x01, 'q' }, 4, 0);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 3);
    CHECK(f[0] == 0x03 && f[1] == 0x01 && f[2] == 0x03);
    /* an out of sequence I frame with P=1 is answered REJ F=1 (8.4.6 NOTE 2) */
    v42lapm_rx_frame(&B, (const uint8_t[]){ 0x03, 0x06, 0x01, 'q' }, 4, 0);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 3);
    CHECK(f[0] == 0x03 && f[1] == 0x09 && f[2] == 0x03);
    CHECK_EQ_I(v42lapm_tx_frame(&B, f, &fcs), 0);
}

static int brkack_drops;
static int drop_brkack(int from_a, const uint8_t *f, int len)
{
    (void)from_a;
    if (len == 3 && f[1] == 0x03 && (f[2] & 0x7F) == 0x60 && brkack_drops > 0) { brkack_drops--; return 0; }
    return 1;
}

static void test_break(void)
{
    int opt = -1, blen = -1;

    /* 8.13, 12.3: BRK in a UI command, BRKACK in a UI response */
    setup();
    CHECK(connect_pair());
    nlog = 0;
    CHECK_EQ_I(v42lapm_get_break(&B, &opt, &blen), 0);
    CHECK_EQ_I(v42lapm_send_break(&A, V42_BRK_NONDESTRUCTIVE_EXPEDITED, 30), 0);
    CHECK_EQ_I(v42lapm_send_break(&A, V42_BRK_NONDESTRUCTIVE_EXPEDITED, 30), -1);
    run(10);
    CHECK_EQ_I(nlog, 2);
    CHECK(flog[0].len == 5 && flog[0].f[0] == 0x03 && flog[0].f[1] == 0x03 &&
          flog[0].f[2] == 0x40 && flog[0].f[3] == 0x40 && flog[0].f[4] == 30);
    CHECK(flog[1].len == 3 && flog[1].f[0] == 0x03 && flog[1].f[1] == 0x03 && flog[1].f[2] == 0xE0);
    CHECK_EQ_I(v42lapm_get_break(&B, &opt, &blen), 1);
    CHECK_EQ_I(opt, V42_BRK_NONDESTRUCTIVE_EXPEDITED); CHECK_EQ_I(blen, 30);
    CHECK_EQ_I(v42lapm_get_break(&B, &opt, &blen), 0);
    CHECK_EQ_I(A.brk_confirmed, 1);
    /* the next one carries N(SB) = 1, default length (no octet 3); its
     * acknowledgement is lost once so it is repeated but indicated once */
    nlog = 0; filt = drop_brkack; brkack_drops = 1;
    CHECK_EQ_I(v42lapm_send_break(&A, V42_BRK_NONDESTRUCTIVE_EXPEDITED, 0), 0);
    run(ca.t401_ms + 50);
    CHECK_EQ_I(count_ctrl(1, 0x03), 2);
    CHECK(flog[0].len == 4 && flog[0].f[2] == 0xC0 && flog[0].f[3] == 0x40);
    CHECK_EQ_I(COUNT(0, 0x03, 0x03, 0x60), 2);
    CHECK_EQ_I(A.brk_confirmed, 2);
    CHECK_EQ_I(v42lapm_get_break(&B, &opt, &blen), 1);
    CHECK_EQ_I(blen, 0);
    CHECK_EQ_I(v42lapm_get_break(&B, &opt, &blen), 0);

    /* in sequence break waits for outstanding I frames (Table 4), and data
     * keeps flowing afterwards; from the answerer this time */
    setup();
    CHECK(connect_pair());
    flow_reset(&ba, 1000, 100); flow_reset(&ab, 700, 100);
    run(2);
    nlog = 0;
    CHECK_EQ_I(v42lapm_send_break(&B, V42_BRK_NONDESTRUCTIVE_INSEQ, 5), 0);
    run(200);
    CHECK_EQ_I(v42lapm_get_break(&A, &opt, &blen), 1);
    CHECK_EQ_I(opt, V42_BRK_NONDESTRUCTIVE_INSEQ);
    CHECK(flows_ok());
    {
        /* every I frame B sent before the BRK was acknowledged before it */
        int i, brk_at = -1, last_i = -1;
        for (i = 0; i < nlog; i++) {
            if (!flog[i].from_a && flog[i].f[1] == 0x03 && brk_at < 0) brk_at = i;
            if (!flog[i].from_a && is_i(flog[i].f, flog[i].len) && brk_at < 0) last_i = i;
        }
        CHECK(brk_at > 0);
        CHECK(flog[brk_at].f[0] == 0x01 && flog[brk_at].f[2] == 0x40 && flog[brk_at].f[3] == 0x00);
        CHECK(last_i < brk_at);
    }

    /* destructive break: both ends reset their state variables and drop
     * queued data (Tables 4 and 5), then carry on from N(S) = 0 */
    setup();
    CHECK(connect_pair());
    flow_reset(&ab, 3000, 100);
    run(7);
    CHECK(A.vs != 0);
    CHECK_EQ_I(v42lapm_send_break(&A, V42_BRK_DESTRUCTIVE_EXPEDITED, 0), 0);
    CHECK_EQ_I(A.resets, 1);
    CHECK_EQ_I(A.vs, 0);
    ab.len = ab.pos;                     /* the layer above drops its data too */
    run(50);
    CHECK_EQ_I(B.resets, 1);
    CHECK_EQ_I(v42lapm_get_break(&B, &opt, &blen), 1);
    CHECK_EQ_I(opt, V42_BRK_DESTRUCTIVE_EXPEDITED);
    CHECK_EQ_I(A.brk_confirmed, 1);
    CHECK(A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED);
    CHECK(A.vs == 0 && A.va == 0 && B.vr == 0);
    flow_reset(&ab, 2000, 100); flow_reset(&ba, 2000, 100);
    run(200);
    CHECK(flows_ok());

    /* BRK never acknowledged: N400 repeats, failure reported, link stays */
    setup(); ca.n400 = 2;
    CHECK(connect_pair());
    nlog = 0; filt = drop_brkack; brkack_drops = 100;
    v42lapm_send_break(&A, V42_BRK_NONDESTRUCTIVE_EXPEDITED, 1);
    run(4 * ca.t401_ms);
    CHECK_EQ_I(count_ctrl(1, 0x03), 3);
    CHECK_EQ_I(A.brk_failed, 1);
    CHECK_EQ_I(A.state, V42LAPM_CONNECTED);
    /* unknown UI contents are noted and otherwise ignored */
    v42lapm_rx_frame(&B, (const uint8_t[]){ 0x03, 0x03, 0x11, 0x22 }, 4, 0);
    CHECK_EQ_I(B.state, V42LAPM_CONNECTED);
    CHECK(B.violations >= 1);
}

static void test_t403(void)
{
    /* 8.12: with T403 in use an idle link is polled and stays up */
    setup(); ca.t403_ms = 2000; cb.t403_ms = 0; ca.n400 = 5;
    CHECK(connect_pair());
    nlog = 0;
    run(1970);
    CHECK_EQ_I(nlog, 0);
    run(40);
    CHECK_EQ_I(nlog, 2);
    CHECK(flog[0].from_a == 1 && flog[0].f[0] == 0x03 && flog[0].f[1] == 0x01 && flog[0].f[2] == 0x01);
    CHECK(flog[1].from_a == 0 && flog[1].f[1] == 0x01 && flog[1].f[2] == 0x01);
    run(30000);
    CHECK(A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED);
    CHECK(nlog >= 2 * 14 && nlog <= 2 * 17);
    /* and a dead peer is noticed after T403 + (N400 + 1) x T401 */
    filt = drop_all;
    run(2000 + 12 * ca.t401_ms);
    CHECK_EQ_I(A.state, V42LAPM_DISCONNECTED);
    CHECK_EQ_I(A.reason, V42LAPM_R_N400);
    /* without T403 an idle link sends nothing at all */
    setup();
    CHECK(connect_pair());
    nlog = 0;
    run(60000);
    CHECK_EQ_I(nlog, 0);
    CHECK(A.state == V42LAPM_CONNECTED);
}

static void test_fcs32(void)
{
    /* 8.10.2: XID with the 16-bit FCS, then SABME and everything after it
     * with the 32-bit FCS when both sides agreed to bit 17 */
    setup(); ca.fcs32 = 1; cb.fcs32 = 1;
    init_pair(); v42lapm_start(&A);
    CHECK_EQ_I(B.rx_fcs_mode, V42HDLC_FCS16);
    run(1);
    CHECK_EQ_I(B.rx_fcs_mode, V42HDLC_FCS_EITHER);   /* until SABME shows which */
    run(20);
    CHECK(A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED);
    CHECK_EQ_I(nlog, 4);
    CHECK(flog[0].fcs32 == 0 && flog[1].fcs32 == 0 && flog[2].fcs32 == 1 && flog[3].fcs32 == 1);
    /* optional function bits: 17 asked for; in the response 16 is cleared */
    CHECK(flog[0].f[10] == 0x01 && flog[0].f[9] == 0x89 && flog[0].f[7] == 3);
    CHECK(flog[1].f[10] == 0x01 && flog[1].f[9] == 0x09);
    CHECK(A.tx_fcs32 && B.tx_fcs32);
    CHECK(A.rx_fcs_mode == V42HDLC_FCS32 && B.rx_fcs_mode == V42HDLC_FCS32);
    flow_reset(&ab, 1000, 100);
    run(50);
    CHECK(flows_ok());
    CHECK_EQ_I(flog[nlog - 1].fcs32, 1);
    /* not agreed when either side does not want it */
    setup(); ca.fcs32 = 1; cb.fcs32 = 0;
    CHECK(connect_pair());
    CHECK(!A.tx_fcs32 && !B.tx_fcs32 && flog[2].fcs32 == 0 && flog[1].f[10] == 0x00 && flog[1].f[9] == 0x89);
    CHECK(A.rx_fcs_mode == V42HDLC_FCS16 && B.rx_fcs_mode == V42HDLC_FCS16);
    setup(); ca.fcs32 = 0; cb.fcs32 = 1;
    CHECK(connect_pair());
    CHECK(!A.tx_fcs32 && !B.tx_fcs32);
    /* agreed in XID but the SABME comes with the 16-bit FCS: follow it */
    setup(); cb.fcs32 = 1; init_pair();
    v42lapm_rx_frame(&B, (const uint8_t[]){ 0x03, 0xAF, 0x82, 0x80, 0x00, 0x06, 0x03, 0x04, 0x8A, 0x89, 0x01, 0x00 }, 12, 0);
    CHECK_EQ_I(B.rx_fcs_mode, V42HDLC_FCS_EITHER);
    v42lapm_rx_frame(&B, (const uint8_t[]){ 0x03, 0x7F }, 2, 0);
    CHECK_EQ_I(B.state, V42LAPM_CONNECTED);
    CHECK(!B.tx_fcs32 && B.rx_fcs_mode == V42HDLC_FCS16);
}

static int loss_pct;
static int lossy(int from_a, const uint8_t *f, int len)
{
    (void)from_a; (void)f; (void)len;
    return (int)(rnd() % 100) >= loss_pct;
}

static void test_stress(void)
{
    int round;
    for (round = 0; round < 6; round++) {
        setup();
        ca.t401_ms = cb.t401_ms = 60; ca.n400 = cb.n400 = 60;
        ca.k_tx = ca.k_rx = round & 1 ? V42_MAX_K : 7;
        cb.k_tx = cb.k_rx = V42_MAX_K;
        ca.t403_ms = round == 2 ? 500 : 0;
        CHECK(connect_pair());
        {
            /* one rnd() per statement: argument evaluation order is not
             * defined, and the run must be the same on every target */
            size_t la = 40000 + rnd() % 15000, lb = 25000 + rnd() % 15000;
            int ka = 16 + (int)(rnd() % 113), kb = 16 + (int)(rnd() % 113);
            flow_reset(&ab, la, ka);
            flow_reset(&ba, lb, kb);
        }
        filt = lossy; loss_pct = 3 + 4 * round;
        {
            int i;
            for (i = 0; i < 300; i++) {
                run(50 + (int)(rnd() % 100));
                if (round >= 3) {
                    v42lapm_set_busy(&A, rnd() % 4 == 0);
                    v42lapm_set_busy(&B, rnd() % 4 == 0);
                }
            }
        }
        v42lapm_set_busy(&A, 0); v42lapm_set_busy(&B, 0);
        filt = NULL;
        run(20000);
        CHECK(A.state == V42LAPM_CONNECTED && B.state == V42LAPM_CONNECTED);
        CHECK(flows_ok());
        CHECK(A.tx_i_retx > 0 && B.tx_i_retx > 0);
        CHECK(A.tx_i >= 128);                        /* numbers wrapped */
    }
}

int main(void)
{
    test_establish_encodings();
    test_negotiation();
    test_xid_command_form();
    test_xid_response_smaller();
    test_xid_long_values();
    test_v44_negotiation();
    test_data();
    test_window();
    test_rej_recovery();
    test_t401_recovery();
    test_line_restored();
    test_n400();
    test_no_progress();
    test_ua_lost();
    test_busy();
    test_release();
    test_protocol_errors();
    test_break();
    test_t403();
    test_fcs32();
    test_stress();
    T_REPORT("test_lapm");
}
