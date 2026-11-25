/* End-to-end tests of the V.42 endpoint: two of our own endpoints joined by
 * a simulated synchronous bit pipe (32 line bits per millisecond each way)
 * with optional delay, random bit errors, error bursts and line cut; plus
 * single-endpoint runs against scripted start-stop peers for the detection
 * phase fallbacks. Every transfer is checked octet for octet. */
#include <limits.h>
#include <string.h>
#include "test.h"
#include "v42.h"

#define BITS_PER_MS 32

static uint32_t rs = 0x1234567u;
static uint32_t rnd32(void)
{
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return rs;
}

/* ------------------------------------------------------------ bit pipe */
#define DLMAX 8192
typedef struct {
    uint8_t dl[DLMAX];
    int delay, pos;          /* delay in bits */
    uint32_t ber;            /* error probability times 2^32 */
    int corrupt;             /* invert every bit */
    int cut;                 /* receiver sees mark only */
    unsigned long errs;
} pipe_t;

static void pipe_init(pipe_t *p)
{
    memset(p, 0, sizeof *p);
    memset(p->dl, 1, sizeof p->dl);
}

static int pipe_bit(pipe_t *p, int bit)
{
    int out = bit;
    if (p->delay > 0) {
        out = p->dl[p->pos];
        p->dl[p->pos] = (uint8_t)bit;
        if (++p->pos >= p->delay) p->pos = 0;
    }
    if (p->cut) return 1;
    if (p->corrupt) { p->errs++; return !out; }
    if (p->ber && rnd32() < p->ber) { p->errs++; return !out; }
    return out;
}

/* ------------------------------------------- line tap: what was sent */
#define TAPN 8192
#define RAWN 6000
typedef struct {
    v42hdlc_rx_t h;
    struct { uint8_t a, c, c2; int len; } f[TAPN];
    int n;
    uint8_t raw[RAWN]; int nraw;
    long first_zero;         /* bit index of the first 0 sent, -1 = none */
    long bits;
} tap_t;

static void tap_init(tap_t *t)
{
    memset(t, 0, sizeof *t);
    v42hdlc_rx_init(&t->h);
    v42hdlc_rx_set_fcs(&t->h, V42HDLC_FCS_EITHER);
    t->first_zero = -1;
}

static void tap_bit(tap_t *t, int bit)
{
    int r = v42hdlc_rx_bit(&t->h, bit);
    if (t->nraw < RAWN) t->raw[t->nraw++] = (uint8_t)bit;
    if (!bit && t->first_zero < 0) t->first_zero = t->bits;
    t->bits++;
    if (r >= 2 && t->n < TAPN) {
        t->f[t->n].a = t->h.buf[0]; t->f[t->n].c = t->h.buf[1];
        t->f[t->n].c2 = r > 2 ? t->h.buf[2] : 0;
        t->f[t->n].len = r; t->n++;
    }
}

static int tap_count(const tap_t *t, int addr, int ctl)
{
    int i, c = 0;
    for (i = 0; i < t->n; i++) if (t->f[i].a == addr && t->f[i].c == ctl) c++;
    return c;
}

/* index of the first frame with this address and control, or -1 */
static int tap_find(const tap_t *t, int addr, int ctl)
{
    int i;
    for (i = 0; i < t->n; i++) if (t->f[i].a == addr && t->f[i].c == ctl) return i;
    return -1;
}

/* ------------------------------------------------------- user data flows */
#define DMAX 70000
typedef struct {
    uint8_t src[DMAX]; size_t len, pos; size_t chunk;
    uint8_t got[DMAX]; size_t ngot;
    int reading;
} flow_t;

enum { F_RANDOM, F_TEXT, F_RUNS, F_ESC };

static void flow_fill(flow_t *w, size_t len, int kind)
{
    static const char *words[16] = {
        "the", "modem", "carrier", "signal", "error", "frame", "window", "answer",
        "originate", "line", "data", "octet", "phase", "symbol", "rate", "retrain" };
    size_t i = 0;
    w->len = len; w->pos = 0; w->ngot = 0; w->chunk = 200; w->reading = 1;
    while (i < len) {
        if (kind == F_RANDOM) w->src[i++] = (uint8_t)(rnd32() >> 9);
        else if (kind == F_TEXT) {
            const char *s = words[(rnd32() >> 7) & 15];
            while (*s && i < len) w->src[i++] = (uint8_t)*s++;
            if (i < len) w->src[i++] = ((rnd32() >> 5) % 11) ? ' ' : '\n';
        } else if (kind == F_RUNS) {
            size_t n = 1 + (rnd32() >> 8) % 900;
            uint8_t c = (uint8_t)(rnd32() >> 11);
            while (n-- && i < len) w->src[i++] = c;
        } else {
            /* the escape character and its successors (V.42 bis 9.2) */
            static const uint8_t e[6] = { 0, 51, 102, 153, 204, 255 };
            w->src[i++] = e[(rnd32() >> 9) % 6];
        }
    }
}

static v42ec_t A, B;                    /* A originates, B answers */
static v42ec_cfg_t ca, cb;
static pipe_t pab, pba;
static tap_t ta, tb;
static flow_t ab, ba;
static int now_ms, max_un_a, max_un_b;
static int use_v44;                     /* 0: the tests run with V.42 bis alone */

static void setup(void)
{
    v42ec_cfg_default(&ca, 1); v42ec_cfg_default(&cb, 0);
    if (!use_v44) ca.lapm.v44_tx = ca.lapm.v44_rx = cb.lapm.v44_tx = cb.lapm.v44_rx = 0;
    pipe_init(&pab); pipe_init(&pba);
    tap_init(&ta); tap_init(&tb);
    flow_fill(&ab, 0, F_RANDOM); flow_fill(&ba, 0, F_RANDOM);
    now_ms = 0; max_un_a = max_un_b = 0;
}

static void start(void)
{
    v42ec_init(&A, &ca); v42ec_init(&B, &cb);
}

static void pump(v42ec_t *tx, flow_t *w, v42ec_t *rx)
{
    uint8_t tmp[512]; size_t n;
    if (w->pos < w->len) {
        size_t c = w->len - w->pos;
        if (c > w->chunk) c = w->chunk;
        w->pos += v42ec_write(tx, w->src + w->pos, c);
    }
    if (!w->reading) return;
    while ((n = v42ec_read(rx, tmp, sizeof tmp)) > 0) {
        if (w->ngot + n <= DMAX) memcpy(w->got + w->ngot, tmp, n);
        w->ngot += n;
    }
}

static void step_ms(void)
{
    int i, u;
    for (i = 0; i < BITS_PER_MS; i++) {
        int a = v42ec_tx_bit(&A), b = v42ec_tx_bit(&B);
        tap_bit(&ta, a); tap_bit(&tb, b);
        v42ec_rx_bit(&B, pipe_bit(&pab, a));
        v42ec_rx_bit(&A, pipe_bit(&pba, b));
    }
    pump(&A, &ab, &B); pump(&B, &ba, &A);
    v42ec_tick(&A, 1); v42ec_tick(&B, 1);
    u = v42lapm_unacked(&A.lapm); if (u > max_un_a) max_un_a = u;
    u = v42lapm_unacked(&B.lapm); if (u > max_un_b) max_un_b = u;
    now_ms++;
}

static void run(int ms) { while (ms-- > 0) step_ms(); }

static int both(int state) { return v42ec_state(&A) == state && v42ec_state(&B) == state; }

/* run until both ends are in the state; returns ms taken or -1 */
static int run_until_both(int state, int max_ms)
{
    int t0 = now_ms;
    while (now_ms - t0 < max_ms) {
        if (both(state)) return now_ms - t0;
        step_ms();
    }
    return both(state) ? max_ms : -1;
}

static int flows_done(void)
{
    return ab.ngot == ab.len && ba.ngot == ba.len && ab.pos == ab.len && ba.pos == ba.len
        && v42ec_tx_idle(&A) && v42ec_tx_idle(&B);
}

static int run_until_done(int max_ms)
{
    int t0 = now_ms;
    while (now_ms - t0 < max_ms) {
        if (flows_done()) return now_ms - t0;
        step_ms();
    }
    return flows_done() ? max_ms : -1;
}

static void check_flow(const flow_t *w)
{
    CHECK_EQ_I(w->ngot, w->len);
    CHECK(w->ngot == w->len && memcmp(w->got, w->src, w->len) == 0);
}

static void connect_pair(void)
{
    start();
    CHECK(run_until_both(V42EC_CONNECTED, 3000) >= 0);
}

/* ---------------------------------------------------------- handshake */
static void test_handshake(void)
{
    v42det_rx_t d; v42ec_status_t sa, sb;
    uint8_t buf[8];
    int i, t, res, lead;

    setup(); start();
    CHECK_EQ_I(v42ec_state(&A), V42EC_DETECT);
    CHECK_EQ_I(v42ec_state(&B), V42EC_DETECT);
    t = run_until_both(V42EC_CONNECTED, 3000);
    CHECK(t >= 0 && t < 400);
    CHECK_EQ_I(v42ec_read(&A, buf, sizeof buf), 0);     /* no pattern leaks to the user */
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 0);

    /* originator's line opens with the ODP (7.2.1.2) */
    v42det_rx_init(&d, 0); res = 0;
    for (i = 0; i < ta.nraw && !res; i++) res = v42det_rx_bit(&d, ta.raw[i]);
    CHECK_EQ_I(res, V42DET_ODP);
    CHECK(i < 150);
    /* answerer sends mark, then the ADP "EC" at least ten times (7.2.1.3) */
    for (lead = 0; lead < tb.nraw && tb.raw[lead]; lead++) ;
    CHECK(lead >= 60);                                  /* four DC1s had to arrive first */
    v42det_rx_init(&d, 1); res = 0;
    for (i = 0; i < tb.nraw && !res; i++) res = v42det_rx_bit(&d, tb.raw[i]);
    CHECK_EQ_I(res, V42DET_ADP_V42);
    CHECK(B.det_tx.patterns >= 10);

    /* frames: XID command and response, SABME, UA (clause 8.10, 8.3) */
    CHECK(ta.n >= 2 && tb.n >= 2);
    CHECK_EQ_I(ta.f[0].a, 0x03); CHECK_EQ_I(ta.f[0].c, 0xAF);
    CHECK_EQ_I(tap_count(&ta, 0x03, 0x7F), 1);
    CHECK_EQ_I(tb.f[0].a, 0x03); CHECK_EQ_I(tb.f[0].c, 0xAF);
    CHECK_EQ_I(tap_count(&tb, 0x03, 0x73), 1);
    CHECK(tap_find(&ta, 0x03, 0x7F) > tap_find(&ta, 0x03, 0xAF));

    v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
    CHECK_EQ_I(sa.state, V42EC_CONNECTED); CHECK_EQ_I(sa.reason, 0);
    CHECK_EQ_I(sa.k_tx, 15); CHECK_EQ_I(sa.k_rx, 15);
    CHECK_EQ_I(sa.n401_tx, 128); CHECK_EQ_I(sb.n401_rx, 128);
    CHECK_EQ_I(sa.comp_tx, 1); CHECK_EQ_I(sa.comp_rx, 1);
    CHECK_EQ_I(sb.comp_tx, 1); CHECK_EQ_I(sb.comp_rx, 1);
    CHECK_EQ_I(sa.comp_n2, V42COMP_MAX_CODEWORDS < 2048 ? V42COMP_MAX_CODEWORDS : 2048);
    CHECK_EQ_I(sb.comp_n2, sa.comp_n2);
    CHECK_EQ_I(sa.comp_n7, 32); CHECK_EQ_I(sb.comp_n7, 32);
    CHECK_EQ_I(sa.fcs32, 0);

    /* an idle link stays up and stays quiet: flags only */
    i = ta.n; run(3000);
    CHECK(both(V42EC_CONNECTED));
    CHECK_EQ_I(ta.n, i);
    CHECK(v42ec_tx_idle(&A) && v42ec_tx_idle(&B));
}

/* connection must also come up through a noisy start */
static void test_handshake_noisy(void)
{
    int seed;
    for (seed = 1; seed <= 6; seed++) {
        setup(); rs = 0x9E3779B9u * (uint32_t)seed;
        pab.ber = pba.ber = 4294967u;                   /* 1e-3 */
        ca.lapm.t401_ms = cb.lapm.t401_ms = 300;
        start();
        CHECK(run_until_both(V42EC_CONNECTED, 15000) >= 0);
        CHECK_EQ_I(A.lapm.k_tx, B.lapm.k_rx);
        CHECK_EQ_I(A.lapm.comp_tx, B.lapm.comp_rx);
        CHECK_EQ_I(A.lapm.comp_n2, B.lapm.comp_n2);
    }
}

/* ------------------------------------------------------------ bulk data */
static void bulk(int kind_ab, int kind_ba, size_t len, int comp, int fcs32, int k, int n401,
                 int delay, int max_ms)
{
    setup();
    ca.lapm.comp_tx = ca.lapm.comp_rx = cb.lapm.comp_tx = cb.lapm.comp_rx = comp;
    ca.lapm.v44_tx = ca.lapm.v44_rx = cb.lapm.v44_tx = cb.lapm.v44_rx = comp && use_v44;
    ca.lapm.fcs32 = cb.lapm.fcs32 = fcs32;
    ca.lapm.k_tx = ca.lapm.k_rx = cb.lapm.k_tx = cb.lapm.k_rx = k;
    ca.lapm.n401_tx = ca.lapm.n401_rx = cb.lapm.n401_tx = cb.lapm.n401_rx = n401;
    pab.delay = pba.delay = delay;
    connect_pair();
    CHECK_EQ_I(A.lapm.fcs32, fcs32);
    CHECK_EQ_I(A.lapm.k_tx, k); CHECK_EQ_I(B.lapm.k_tx, k);
    CHECK_EQ_I(A.lapm.n401_tx, n401);
    CHECK_EQ_I(A.comp_tx, comp); CHECK_EQ_I(B.comp_tx, comp);
    CHECK_EQ_I(A.comp_rx, comp); CHECK_EQ_I(B.comp_rx, comp);
    CHECK_EQ_I(A.comp_alg, !comp ? V42EC_COMP_NONE : use_v44 ? V42EC_COMP_V44 : V42EC_COMP_V42BIS);
    CHECK_EQ_I(B.comp_alg, A.comp_alg);
    flow_fill(&ab, len, kind_ab); flow_fill(&ba, len, kind_ba);
    ab.chunk = 1 + (rnd32() >> 8) % 400; ba.chunk = 1 + (rnd32() >> 8) % 400;
    CHECK(run_until_done(max_ms) >= 0);
    check_flow(&ab); check_flow(&ba);
    CHECK(max_un_a <= k && max_un_b <= k);
    CHECK(max_un_a >= 1);
    CHECK(both(V42EC_CONNECTED));
    CHECK_EQ_I(A.lapm.tx_i_retx, 0); CHECK_EQ_I(B.lapm.tx_i_retx, 0);
    CHECK_EQ_I(A.lapm.violations, 0); CHECK_EQ_I(B.lapm.violations, 0);
    if (fcs32) CHECK_EQ_I(A.hrx.last_fcs32, 1);
}

static void test_bulk(void)
{
    bulk(F_RANDOM, F_RANDOM, 30000, 0, 0, 15, 128, 0, 30000);
    bulk(F_RANDOM, F_TEXT,   30000, 1, 0, 15, 128, 0, 30000);
    bulk(F_TEXT,   F_RUNS,   60000, 1, 0, 15, 128, 0, 30000);
    bulk(F_ESC,    F_RANDOM, 20000, 1, 0, 15, 128, 0, 30000);
    bulk(F_TEXT,   F_RANDOM, 30000, 1, 1, 15, 128, 0, 30000);     /* 32-bit FCS */
    bulk(F_RANDOM, F_TEXT,   30000, 1, 0, V42_MAX_K, V42_MAX_N401, 0, 30000);
    bulk(F_RANDOM, F_RANDOM, 8000,  0, 0, 1, 32, 0, 60000);       /* window of one */
    bulk(F_TEXT,   F_RANDOM, 30000, 1, 0, 15, 128, 3200, 40000);  /* 100 ms each way */
    bulk(F_RANDOM, F_RUNS,   20000, 1, 0, 4, 64, 6400, 90000);    /* 200 ms each way */
}

/* compression has to pay on text, and may not cost much on random data */
static void test_compression_gain(void)
{
    v42ec_status_t s; int t;
    setup(); connect_pair();
    flow_fill(&ab, 60000, F_TEXT); flow_fill(&ba, 0, F_RANDOM);
    t = run_until_done(30000);
    check_flow(&ab);
    v42ec_get_status(&A, &s);
    CHECK_EQ_I(s.comp_in, 60000);
    CHECK(s.comp_out * 2 < s.comp_in);
    CHECK(t >= 0 && t < 60000 * 8 / BITS_PER_MS / 2);   /* twice the raw line rate */
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.decomp_out, 60000);

    setup(); connect_pair();
    flow_fill(&ab, 30000, F_RANDOM);
    CHECK(run_until_done(30000) >= 0);
    check_flow(&ab);
    v42ec_get_status(&A, &s);
    CHECK(s.comp_out < s.comp_in + s.comp_in / 20);
}

/* single characters are not held back waiting for more (C-FLUSH, 7.9) */
static void test_interactive(void)
{
    int i, policy;
    for (policy = V42COMP_AUTO; policy <= V42COMP_NEVER; policy++) {
        setup(); ca.comp_policy = cb.comp_policy = policy; connect_pair();
        for (i = 0; i < 40; i++) {
            uint8_t c = (uint8_t)("abcabcabd"[i % 9]), r = 0, buf[4];
            ab.reading = ba.reading = 0;
            CHECK_EQ_I(v42ec_write(&A, &c, 1), 1);
            run(60);
            CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 1);
            CHECK_EQ_I(buf[0], c);
            r = (uint8_t)(c ^ 0x20);
            CHECK_EQ_I(v42ec_write(&B, &r, 1), 1);
            run(60);
            CHECK_EQ_I(v42ec_read(&A, buf, sizeof buf), 1);
            CHECK_EQ_I(buf[0], r);
        }
        CHECK(v42ec_tx_idle(&A) && v42ec_tx_idle(&B));
    }
}

/* ------------------------------------------------------ error recovery */
static void test_bit_errors(void)
{
    v42ec_status_t sa, sb;
    int fcs32;
    for (fcs32 = 0; fcs32 <= 1; fcs32++) {
        setup(); rs = 777u + (uint32_t)fcs32;
        ca.lapm.fcs32 = cb.lapm.fcs32 = fcs32;
        connect_pair();
        pab.ber = pba.ber = 429497u;                    /* 1e-4: one frame in ten */
        flow_fill(&ab, 40000, F_RANDOM); flow_fill(&ba, 40000, F_TEXT);
        CHECK(run_until_done(240000) >= 0);
        check_flow(&ab); check_flow(&ba);
        v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
        CHECK(pab.errs > 50 && pba.errs > 20);
        CHECK(sa.rx_frames_bad >= 5 && sb.rx_frames_bad >= 5);
        CHECK(sa.rx_rej >= 1 && sb.rx_rej >= 1);        /* recovery by REJ (8.4.4) */
        CHECK(sa.tx_i_retx >= 1 && sb.tx_i_retx >= 1);
        CHECK(both(V42EC_CONNECTED));
        CHECK(max_un_a <= 15 && max_un_b <= 15);
    }
}

/* whole frames wiped out by periodic error bursts */
static void test_bursts(void)
{
    int t;
    setup(); rs = 99u; connect_pair();
    flow_fill(&ab, 40000, F_TEXT); flow_fill(&ba, 40000, F_RANDOM);
    for (t = 0; t < 200000 && !flows_done(); t++) {
        pab.corrupt = (t % 700) < 45;
        pba.corrupt = ((t + 350) % 900) < 60;
        step_ms();
    }
    pab.corrupt = pba.corrupt = 0;
    CHECK(run_until_done(20000) >= 0);
    check_flow(&ab); check_flow(&ba);
    CHECK(A.lapm.rx_rej + A.lapm.t401_expiries >= 3);
    CHECK(B.lapm.rx_rej + B.lapm.t401_expiries >= 3);
    CHECK(A.lapm.tx_i_retx >= 3 && B.lapm.tx_i_retx >= 3);
    CHECK(both(V42EC_CONNECTED));
}

/* a lone frame lost: nothing follows it, so only T401 can recover (8.4.8) */
static void test_t401_recovery(void)
{
    uint8_t buf[32];
    setup(); connect_pair();
    ab.reading = 0;
    pab.corrupt = 1;
    CHECK_EQ_I(v42ec_write(&A, (const uint8_t *)"lonely frame", 12), 12);
    run(300);
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 0);
    CHECK_EQ_I(A.lapm.t401_expiries, 0);
    pab.corrupt = 0;
    run(1500);
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 12);
    CHECK(memcmp(buf, "lonely frame", 12) == 0);
    CHECK(A.lapm.t401_expiries >= 1);
    CHECK_EQ_I(A.lapm.rx_rej, 0);
    CHECK(A.lapm.tx_polls >= 1);
    CHECK(v42ec_tx_idle(&A));
    CHECK(both(V42EC_CONNECTED));

    /* the acknowledgement lost instead: data must not be duplicated */
    pba.corrupt = 1;
    CHECK_EQ_I(v42ec_write(&A, (const uint8_t *)"once only", 9), 9);
    run(300);
    pba.corrupt = 0;
    run(2500);
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 9);
    CHECK(memcmp(buf, "once only", 9) == 0);
    CHECK(v42ec_tx_idle(&A));
    CHECK(A.lapm.t401_expiries >= 2);
}

/* ---------------------------------------------------------- flow control */
static void test_flow_control(int comp)
{
    size_t stalled, bound;
    setup();
    ca.lapm.comp_tx = ca.lapm.comp_rx = cb.lapm.comp_tx = cb.lapm.comp_rx = comp;
    ca.lapm.v44_tx = ca.lapm.v44_rx = cb.lapm.v44_tx = cb.lapm.v44_rx = comp && use_v44;
    connect_pair();
    flow_fill(&ab, 60000, comp ? F_TEXT : F_RANDOM);
    flow_fill(&ba, 5000, F_RANDOM);                     /* reverse direction keeps going */
    ab.reading = 0;
    run(15000);
    stalled = ab.pos;
    run(3000);
    CHECK_EQ_I(ab.pos, stalled);                        /* writer is held off */
    CHECK_EQ_I(v42ec_write_space(&A), 0);
    CHECK_EQ_I(v42ec_write(&A, (const uint8_t *)"x", 1), 0);
    CHECK(stalled < 60000);
    CHECK(v42ec_read_avail(&B) >= V42EC_RXBUF / 2);
    if (!comp) {
        /* what can be in flight: both buffers plus one window of frames */
        bound = V42EC_TXBUF + V42EC_RXBUF + 17 * 128;
        CHECK(stalled <= bound);
        CHECK(stalled >= V42EC_TXBUF + V42EC_RXBUF / 2);
        CHECK(B.lapm.rx_i_dropped <= 15);               /* at most the window in flight */
    }
    CHECK(tap_count(&tb, 0x03, 0x05) + tap_count(&tb, 0x01, 0x05) >= 1);    /* RNR seen */
    CHECK(both(V42EC_CONNECTED));                       /* busy is not a failure */
    check_flow(&ba);
    ab.reading = 1;
    CHECK(run_until_done(60000) >= 0);
    check_flow(&ab);
    CHECK_EQ_I(A.lapm.violations, 0); CHECK_EQ_I(B.lapm.violations, 0);
}

/* ------------------------------------------------- single-endpoint runs */
typedef struct {
    v42det_tx_t det; int det_left;      /* patterns still to send, -1 = for ever */
    v42async_tx_t atx; const uint8_t *s; int n, pos;
    long start_bit, bit;
    v42async_rx_t arx; uint8_t got[256]; int ngot;
} peer_t;

static void peer_init(peer_t *p, int pattern, int count, const char *script, int start_ms)
{
    memset(p, 0, sizeof *p);
    v42det_tx_init(&p->det, pattern, 10);
    p->det_left = count;
    v42async_tx_init(&p->atx);
    v42async_rx_init(&p->arx);
    p->s = (const uint8_t *)script; p->n = script ? (int)strlen(script) : 0;
    p->start_bit = (long)start_ms * BITS_PER_MS;
}

static int peer_bit(peer_t *p)
{
    p->bit++;
    if (p->det_left != 0) {
        int b = v42det_tx_bit(&p->det);
        if (p->det_left > 0 && v42det_tx_boundary(&p->det)) p->det_left--;
        return b;
    }
    if (p->bit >= p->start_bit && p->pos < p->n && v42async_tx_idle(&p->atx))
        v42async_tx_char(&p->atx, p->s[p->pos++]);
    return v42async_tx_bit(&p->atx);
}

static void solo(v42ec_t *e, peer_t *p, int ms)
{
    int i;
    while (ms-- > 0) {
        for (i = 0; i < BITS_PER_MS; i++) {
            int b = v42ec_tx_bit(e), c;
            tap_bit(&ta, b);
            c = v42async_rx_bit(&p->arx, b);
            if (c >= 0 && p->ngot < 256) p->got[p->ngot++] = (uint8_t)c;
            v42ec_rx_bit(e, peer_bit(p));
        }
        v42ec_tick(e, 1);
        now_ms++;
    }
}

/* answerer, caller has no V.42: T400 runs out, buffered start-stop mode */
static void test_fallback_answerer(void)
{
    peer_t p; uint8_t buf[64]; v42ec_status_t s;
    setup();
    v42ec_init(&B, &cb);
    peer_init(&p, V42DET_PAT_ODP, 0, "AT\rhello", 100);
    solo(&B, &p, 740);
    CHECK_EQ_I(v42ec_state(&B), V42EC_DETECT);
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 0);
    CHECK_EQ_I(v42ec_write(&B, (const uint8_t *)"wor", 3), 3);  /* held until the mode is known */
    solo(&B, &p, 20);
    CHECK_EQ_I(v42ec_state(&B), V42EC_ASYNC);
    CHECK_EQ_I(ta.first_zero / BITS_PER_MS, 750);       /* mark until T400, then our data */
    /* characters that arrived during the detection phase were kept (App. I.3) */
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 8);
    CHECK(memcmp(buf, "AT\rhello", 8) == 0);
    CHECK_EQ_I(v42ec_write(&B, (const uint8_t *)"ld", 2), 2);
    CHECK(!v42ec_tx_idle(&B));
    solo(&B, &p, 100);
    CHECK(v42ec_tx_idle(&B));
    CHECK_EQ_I(p.ngot, 5);
    CHECK(memcmp(p.got, "world", 5) == 0);
    CHECK_EQ_I(ta.n, 0);                                /* never an HDLC frame */
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.state, V42EC_ASYNC);
    CHECK_EQ_I(s.async_framing, 0); CHECK_EQ_I(s.async_overruns, 0);
    /* and it ends when the user says so */
    v42ec_release(&B);
    CHECK_EQ_I(v42ec_state(&B), V42EC_DISCONNECTED);
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.reason, V42LAPM_R_LOCAL);

    /* same caller, fallback not allowed */
    setup(); cb.fallback = 0;
    v42ec_init(&B, &cb);
    peer_init(&p, V42DET_PAT_ODP, 0, "AT\r", 100);
    solo(&B, &p, 760);
    CHECK_EQ_I(v42ec_state(&B), V42EC_DISCONNECTED);
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.reason, V42EC_R_NO_EC);
    CHECK_EQ_I(ta.first_zero, -1);
}

static void test_fallback_originator(void)
{
    peer_t p; uint8_t buf[64]; v42ec_status_t s;
    v42det_rx_t d; int i, res;

    /* answerer sends mark only: ODP for T400, then start-stop mode */
    setup();
    v42ec_init(&A, &ca);
    peer_init(&p, V42DET_PAT_ODP, 0, "hi", 900);
    solo(&A, &p, 740);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DETECT);
    solo(&A, &p, 20);
    CHECK_EQ_I(v42ec_state(&A), V42EC_ASYNC);
    v42det_rx_init(&d, 0); res = 0;
    for (i = 0; i < ta.nraw && !res; i++) res = v42det_rx_bit(&d, ta.raw[i]);
    CHECK_EQ_I(res, V42DET_ODP);
    CHECK(A.det_tx.patterns >= 500);                    /* ODP all through T400 */
    CHECK_EQ_I(ta.n, 0);
    p.ngot = 0;                                         /* forget the DC1s */
    CHECK_EQ_I(v42ec_write(&A, (const uint8_t *)"ok", 2), 2);
    solo(&A, &p, 300);
    CHECK_EQ_I(v42ec_read(&A, buf, sizeof buf), 2);
    CHECK(memcmp(buf, "hi", 2) == 0);
    CHECK_EQ_I(p.ngot, 2);
    CHECK(memcmp(p.got, "ok", 2) == 0);

    /* answerer says "no error correction" (Table 3, E NUL): fall back at once
     * and do not hand the rest of its ADP to the user */
    setup();
    v42ec_init(&A, &ca);
    peer_init(&p, V42DET_PAT_ADP_NOEC, 12, "Echo", 200);
    solo(&A, &p, 60);
    CHECK_EQ_I(v42ec_state(&A), V42EC_ASYNC);
    solo(&A, &p, 400);
    CHECK_EQ_I(v42ec_read(&A, buf, sizeof buf), 4);
    CHECK(memcmp(buf, "Echo", 4) == 0);
    CHECK_EQ_I(ta.n, 0);

    /* same, fallback not allowed */
    setup(); ca.fallback = 0;
    v42ec_init(&A, &ca);
    peer_init(&p, V42DET_PAT_ADP_NOEC, 12, NULL, 0);
    solo(&A, &p, 100);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    v42ec_get_status(&A, &s);
    CHECK_EQ_I(s.reason, V42EC_R_NO_EC);
}

/* peer accepts the detection phase and then never speaks LAPM */
static void test_no_answer(void)
{
    peer_t p; v42ec_status_t s;
    int use_xid;
    for (use_xid = 0; use_xid <= 1; use_xid++) {
        setup(); ca.lapm.n400 = 3; ca.lapm.t401_ms = 500; ca.lapm.use_xid = use_xid;
        v42ec_init(&A, &ca);
        peer_init(&p, V42DET_PAT_ADP_V42, 12, NULL, 0);
        solo(&A, &p, 100);
        CHECK_EQ_I(v42ec_state(&A), V42EC_ESTABLISH);
        solo(&A, &p, 1850);
        CHECK_EQ_I(v42ec_state(&A), V42EC_ESTABLISH);   /* still trying */
        solo(&A, &p, 400);
        CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
        v42ec_get_status(&A, &s);
        CHECK_EQ_I(s.reason, V42LAPM_R_N400);
        /* 1 + N400 attempts, nothing else (8.3.2.2, 8.10.3) */
        CHECK_EQ_I(ta.n, 4);
        CHECK_EQ_I(tap_count(&ta, 0x03, use_xid ? 0xAF : 0x7F), 4);
        CHECK_EQ_I(v42ec_write(&A, (const uint8_t *)"x", 1), 0);
    }

    /* answerer: ODP arrives, ADP goes out, and then nothing; give up after
     * the establishment guard time, having repeated the ADP all the while */
    setup(); cb.establish_ms = 2000;
    v42ec_init(&B, &cb);
    peer_init(&p, V42DET_PAT_ODP, -1, NULL, 0);
    solo(&B, &p, 1900);
    CHECK(v42ec_state(&B) == V42EC_DETECT || v42ec_state(&B) == V42EC_ESTABLISH);
    CHECK(B.det_tx.patterns >= 500);
    solo(&B, &p, 200);
    CHECK_EQ_I(v42ec_state(&B), V42EC_DISCONNECTED);
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.reason, V42EC_R_NO_SABME);
    CHECK_EQ_I(ta.n, 0);
}

/* the line dies with data in flight: both ends notice and stop cleanly */
static void test_line_cut(void)
{
    v42ec_status_t s; uint8_t buf[256]; size_t got_before;
    setup();
    ca.lapm.n400 = cb.lapm.n400 = 3; ca.lapm.t401_ms = cb.lapm.t401_ms = 400;
    connect_pair();
    flow_fill(&ab, 30000, F_RANDOM); flow_fill(&ba, 30000, F_TEXT);
    run(1500);
    CHECK(ab.ngot > 1000 && ab.ngot < 30000);
    pab.cut = pba.cut = 1;
    got_before = ab.ngot;
    run(1400);
    CHECK(both(V42EC_CONNECTED));                       /* not before N400 polls */
    run(600);
    CHECK(both(V42EC_DISCONNECTED));
    v42ec_get_status(&A, &s); CHECK_EQ_I(s.reason, V42LAPM_R_N400);
    v42ec_get_status(&B, &s); CHECK_EQ_I(s.reason, V42LAPM_R_N400);
    CHECK_EQ_I(A.lapm.tx_polls, 3); CHECK_EQ_I(B.lapm.tx_polls, 3);    /* N400 */
    /* what did arrive was right, and nothing is invented afterwards */
    CHECK(ab.ngot - got_before < 400);
    CHECK(memcmp(ab.got, ab.src, ab.ngot) == 0);
    CHECK(memcmp(ba.got, ba.src, ba.ngot) == 0);
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 0);
    CHECK_EQ_I(v42ec_write(&A, buf, 10), 0);
    CHECK(!v42ec_tx_idle(&A));                          /* data was lost, not delivered */
}

/* ------------------------------------- a frame resumed without its flag */
/* A rate change as a U.S. Robotics x2 client makes one (live call,
 * 2026-11-21): its bits stop, and start again with some fifty ones and then
 * the frame it had begun, whose opening flag never comes. The answerer
 * takes the frame as it stands: nothing is rejected or sent twice, and the
 * data arrive well inside T401. */
#define RFIFO 4096
static void test_resume_without_flag(void)
{
    static uint8_t q[RFIFO];
    int head = 0, tail = 0, frames = 0, done = 0, wait = 0, t0, i, j;
    int was_in = 0; uint32_t hist = 0;

    setup();
    connect_pair();
    flow_fill(&ab, 600, F_TEXT);
    ab.chunk = 40;
    t0 = now_ms;
    while (now_ms - t0 < 4000 && !(ab.pos == ab.len && ab.ngot == ab.len && v42ec_tx_idle(&A))) {
        for (i = 0; i < BITS_PER_MS; i++) {
            int a = v42ec_tx_bit(&A), b = v42ec_tx_bit(&B), in = A.htx.in_frame;
            hist = (hist << 1) | (uint32_t)a;
            if (in && !was_in && (A.htx.buf[1] & 1) == 0 && ++frames == 3 && ((hist >> 1) & 0xFFFF) == 0x7E7E) {
                /* its third I frame, after idle flags: the flag that opened
                 * it (the last eight bits queued) is lost, fill comes instead */
                for (j = 1; j <= 8; j++) q[(tail - j + RFIFO) % RFIFO] = 1;
                for (j = 0; j < 41; j++) { q[tail] = 1; tail = (tail + 1) % RFIFO; }
                done = 1;
            }
            was_in = in;
            q[tail] = (uint8_t)a; tail = (tail + 1) % RFIFO;
            tap_bit(&ta, a); tap_bit(&tb, b);
            if ((tail - head + RFIFO) % RFIFO > 8) { v42ec_rx_bit(&B, q[head]); head = (head + 1) % RFIFO; }
            v42ec_rx_bit(&A, pipe_bit(&pba, b));
        }
        /* one chunk at a time, each after the line has been idle */
        if (ab.pos < ab.len && v42ec_tx_idle(&A) && ++wait >= 40) {
            wait = 0;
            ab.pos += v42ec_write(&A, ab.src + ab.pos, ab.chunk);
        }
        {
            uint8_t tmp[512]; size_t n;
            while ((n = v42ec_read(&B, tmp, sizeof tmp)) > 0) {
                if (ab.ngot + n <= DMAX) memcpy(ab.got + ab.ngot, tmp, n);
                ab.ngot += n;
            }
        }
        v42ec_tick(&A, 1); v42ec_tick(&B, 1);
        now_ms++;
    }
    CHECK_EQ_I(done, 1);
    check_flow(&ab);
    CHECK_EQ_I(B.lapm.tx_rej, 0);
    CHECK_EQ_I(B.lapm.rx_i_dropped, 0);
    CHECK_EQ_I(A.lapm.tx_i_retx, 0);
    CHECK_EQ_I(A.lapm.t401_expiries, 0);
    CHECK(both(V42EC_CONNECTED));
}

/* The line stops (v42ec_line_stopped, on V90M_EV_DATA_STOPPED) and is back
 * (v42ec_line_restored, on V90M_EV_DATA_MOVING). Seen on a live x2 call
 * (2026-11-21): our REJ after each of the client's rate changes went out
 * after the start segment's ones with no whole flag of its own, and the
 * client never took it. Now:
 *  - no frame is begun while the line is stopped, even if the modem takes
 *    our bits again before data moves both ways (it may, a round trip
 *    early); the first one after opens with whole flags;
 *  - the frame the stop cut is aborted if it is still going at the
 *    restore, and its data come again through LAPM; a frame begun after
 *    the restore is never aborted;
 *  - without v42ec_line_stopped nothing but LAPM's poll happens. */
static void pull_a(int n) { while (n-- > 0) { int a = v42ec_tx_bit(&A); tap_bit(&ta, a); v42ec_rx_bit(&B, pipe_bit(&pab, a)); } }
static void pull_b(int n) { while (n-- > 0) { int b = v42ec_tx_bit(&B); tap_bit(&tb, b); v42ec_rx_bit(&A, pipe_bit(&pba, b)); } }

static void test_restore_flags(void)
{
    v42hdlc_rx_t h; uint8_t buf[60];
    int i, r, got;
    memset(buf, 'x', sizeof buf);

    /* stopped between frames, a frame queued, our bits taken again a while
     * before data moves: flags only; then the frame, with flags of its own */
    setup(); connect_pair();
    while (!(!A.htx.in_frame && A.htx.raw && A.htx.nbits == 4)) (void)v42ec_tx_bit(&A);
    v42ec_line_stopped(&A);
    v42ec_write(&A, buf, sizeof buf);
    v42hdlc_rx_init(&h); v42hdlc_rx_set_fcs(&h, V42HDLC_FCS_EITHER);
    for (i = 0; i < 49; i++) (void)v42hdlc_rx_bit(&h, 1);      /* the fill in between */
    for (i = 0; i < 400; i++) CHECK(v42hdlc_rx_bit(&h, v42ec_tx_bit(&A)) <= 0);
    CHECK_EQ_I(A.htx.in_frame, 0);
    v42ec_line_restored(&A);
    got = 0;
    for (i = 0; i < 4000; i++) { r = v42hdlc_rx_bit(&h, v42ec_tx_bit(&A)); if (r > 3) got++; }
    CHECK_EQ_I(got, 1); CHECK_EQ_I(h.resynced, 0); CHECK_EQ_I(A.htx.aborts, 0);

    /* stopped mid-frame, both ends running: the cut frame is aborted at the
     * restore, its data arrive all the same (one retransmission), and the
     * frames after it are left alone */
    setup(); connect_pair();
    flow_fill(&ab, 600, F_TEXT); ab.chunk = 600;
    ab.pos += v42ec_write(&A, ab.src, ab.len);
    while (!(A.htx.in_frame && A.htx.pos > 10)) pull_a(1), pull_b(1);
    v42ec_line_stopped(&A); v42ec_line_stopped(&B);
    for (i = 0; i < 800; i++) { v42ec_rx_bit(&B, 1); pull_b(1); }  /* our bits stop; fill to the far end */
    pull_a(300); pull_b(300);                                         /* ours taken again, data not yet moving */
    v42ec_line_restored(&A); v42ec_line_restored(&B);
    pull_a(16); pull_b(16);                     /* the abort goes at the next octet */
    CHECK_EQ_I(A.htx.aborts, 1);
    CHECK(run_until_done(3000) >= 0);
    check_flow(&ab);
    CHECK_EQ_I(A.lapm.tx_i_retx, 1);
    CHECK_EQ_I(A.htx.aborts, 1);                /* the frames after it: none aborted */
    CHECK_EQ_I(A.lapm.t401_expiries, 0);

    /* a frame begun after the restore is not the one the stop cut */
    setup(); connect_pair();
    while (!(!A.htx.in_frame && A.htx.raw && A.htx.nbits == 2)) (void)v42ec_tx_bit(&A);
    v42ec_line_stopped(&A);
    v42ec_line_restored(&A);
    v42ec_write(&A, buf, sizeof buf);
    while (!(A.htx.in_frame && A.htx.pos > 10)) (void)v42ec_tx_bit(&A);
    v42ec_line_stopped(&B); v42ec_line_restored(&B);                /* the other end's, unrelated */
    for (i = 0; i < 2000; i++) (void)v42ec_tx_bit(&A);
    CHECK_EQ_I(A.htx.aborts, 0);

    /* without v42ec_line_stopped: nothing but LAPM's poll, as ever */
    setup(); connect_pair();
    v42ec_write(&A, buf, sizeof buf);
    while (!(A.htx.in_frame && A.htx.pos > 10)) (void)v42ec_tx_bit(&A);
    v42ec_line_restored(&A);
    for (i = 0; i < 2000; i++) (void)v42ec_tx_bit(&A);
    CHECK_EQ_I(A.htx.aborts, 0); CHECK_EQ_I(A.htx.min_flags, 0);
}

/* In the detection phase any frame starts the protocol. One with no opening
 * flag after a run of ones counts only when LAPM could take it (address
 * 0x01 or 0x03): a caller without error control sends characters after
 * idle mark, and the start and stop bits of some make a flag. */
static int detect_case(uint8_t addr)
{
    v42hdlc_tx_t t; uint8_t f[12];
    int i;
    for (i = 0; i < 12; i++) f[i] = (uint8_t)(0x41 + i);
    f[0] = addr; f[1] = 0x3F;
    v42ec_cfg_default(&cb, 0);
    v42ec_init(&B, &cb);
    for (i = 0; i < 200; i++) v42ec_rx_bit(&B, 1);
    v42hdlc_tx_init(&t);
    for (i = 0; i < 8; i++) (void)v42hdlc_tx_bit(&t);
    v42hdlc_tx_frame(&t, f, 12, 0); v42hdlc_tx_flags(&t, 1);
    for (i = 0; i < 8; i++) (void)v42hdlc_tx_bit(&t);             /* its flag: lost */
    while (v42hdlc_tx_busy(&t)) v42ec_rx_bit(&B, v42hdlc_tx_bit(&t));
    for (i = 0; i < 16; i++) v42ec_rx_bit(&B, v42hdlc_tx_bit(&t));  /* its last bits, the closing flag */
    for (i = 0; i < 40; i++) v42ec_rx_bit(&B, 1);                  /* mark again */
    return B.want_hdlc;
}

static void test_detect_flagless(void)
{
    CHECK_EQ_I(detect_case(0x07), 0);
    CHECK_EQ_I(detect_case(0x0F), 0);
    CHECK_EQ_I(detect_case(0x03), 1);
    CHECK_EQ_I(detect_case(0x01), 1);
}

/* ----------------------------------------------------- orderly release */
static void test_release(int from_a)
{
    v42ec_t *x = from_a ? &A : &B, *y = from_a ? &B : &A;
    const tap_t *tx = from_a ? &ta : &tb, *ty = from_a ? &tb : &ta;
    int cmd = from_a ? 0x03 : 0x01;                     /* Table 6 */
    v42ec_status_t s; int t;
    setup(); connect_pair();
    flow_fill(&ab, 3000, F_TEXT); flow_fill(&ba, 3000, F_RANDOM);
    CHECK(run_until_done(10000) >= 0);
    v42ec_release(x);
    CHECK_EQ_I(v42ec_state(x), V42EC_RELEASING);
    t = run_until_both(V42EC_DISCONNECTED, 2000);
    CHECK(t >= 0 && t < 100);
    v42ec_get_status(x, &s); CHECK_EQ_I(s.reason, V42LAPM_R_LOCAL);
    v42ec_get_status(y, &s); CHECK_EQ_I(s.reason, V42LAPM_R_PEER_DISC);
    check_flow(&ab); check_flow(&ba);
    run(50);                                            /* let the UA finish on the line */
    CHECK_EQ_I(tap_count(tx, cmd, 0x53), 1);            /* DISC, P = 1 */
    CHECK_EQ_I(tap_count(ty, cmd, 0x73), 1 + from_a);   /* UA, F = 1 (B also sent the SABME one) */
    CHECK_EQ_I(v42ec_write(x, (const uint8_t *)"x", 1), 0);
    CHECK_EQ_I(v42ec_write(y, (const uint8_t *)"x", 1), 0);
}

/* release while the far end is gone: DISC repeated N400 times, then done */
static void test_release_unanswered(void)
{
    v42ec_status_t s;
    setup(); ca.lapm.n400 = 2; ca.lapm.t401_ms = 300;
    connect_pair();
    pab.cut = pba.cut = 1;
    v42ec_release(&A);
    run(700);
    CHECK_EQ_I(v42ec_state(&A), V42EC_RELEASING);
    run(400);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    v42ec_get_status(&A, &s);
    CHECK(s.reason == V42LAPM_R_N400 || s.reason == V42LAPM_R_LOCAL);
    CHECK_EQ_I(tap_count(&ta, 0x03, 0x53), 3);
}

/* -------------------------------------------- detection phase variants */
static void test_detection_off(void)
{
    v42ec_status_t s; peer_t p;
    /* both skip it (7.2.1: optional when the peer is known to do LAPM) */
    setup(); ca.detect = cb.detect = 0; start();
    CHECK_EQ_I(v42ec_state(&A), V42EC_ESTABLISH);
    CHECK(run_until_both(V42EC_CONNECTED, 500) >= 0);
    CHECK(ta.raw[0] == 0 && tb.raw[0] == 0);            /* flags from the first bit */
    CHECK_EQ_I(ta.f[0].c, 0xAF);
    flow_fill(&ab, 2000, F_TEXT); flow_fill(&ba, 2000, F_TEXT);
    CHECK(run_until_done(5000) >= 0);
    check_flow(&ab); check_flow(&ba);

    /* originator skips it, answerer does not: flags start the protocol phase */
    setup(); ca.detect = 0; start();
    CHECK(run_until_both(V42EC_CONNECTED, 500) >= 0);
    CHECK_EQ_I(B.det_tx.patterns, 0);                   /* no ADP was sent */
    flow_fill(&ab, 2000, F_TEXT); flow_fill(&ba, 2000, F_TEXT);
    CHECK(run_until_done(5000) >= 0);
    check_flow(&ab); check_flow(&ba);

    /* originator without XID: defaults apply, no compression */
    setup(); ca.lapm.use_xid = 0; start();
    CHECK(run_until_both(V42EC_CONNECTED, 1000) >= 0);
    CHECK_EQ_I(tap_count(&ta, 0x03, 0xAF), 0);
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.comp_tx, 0); CHECK_EQ_I(s.comp_rx, 0);
    CHECK_EQ_I(s.k_tx, 15); CHECK_EQ_I(s.n401_tx, 128);
    flow_fill(&ab, 2000, F_TEXT); flow_fill(&ba, 2000, F_TEXT);
    CHECK(run_until_done(5000) >= 0);
    check_flow(&ab); check_flow(&ba);

    /* answerer with detection off and a silent line: guard time ends it */
    setup(); cb.detect = 0; cb.establish_ms = 1000;
    v42ec_init(&B, &cb);
    peer_init(&p, V42DET_PAT_ODP, 0, NULL, 0);
    solo(&B, &p, 990);
    CHECK_EQ_I(v42ec_state(&B), V42EC_ESTABLISH);
    solo(&B, &p, 20);
    CHECK_EQ_I(v42ec_state(&B), V42EC_DISCONNECTED);
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.reason, V42EC_R_NO_SABME);
}

/* the originator detects, the answerer does not (7.2.1 allows it when the
 * peer is known to do LAPM): the answerer's flags end the originator's
 * detection phase, as the originator's end the answerer's */
static void test_detection_mirror(void)
{
    v42ec_status_t s;
    int t;
    setup(); cb.detect = 0; start();
    t = run_until_both(V42EC_CONNECTED, 3000);
    CHECK(t >= 0 && t < 750);                           /* before T400 */
    CHECK_EQ_I(v42ec_state(&A), V42EC_CONNECTED);
    CHECK_EQ_I(ba.ngot, 0);                             /* no flag debris read as characters */
    v42ec_get_status(&A, &s);
    CHECK_EQ_I(s.comp_tx, 1); CHECK_EQ_I(s.comp_rx, 1);    /* XID went through */
    flow_fill(&ab, 3000, F_TEXT); flow_fill(&ba, 3000, F_TEXT);
    CHECK(run_until_done(5000) >= 0);
    check_flow(&ab); check_flow(&ba);
}

/* v42ec_abort: in every state the endpoint stops at once and says nothing */
static void test_abort_states(void)
{
    static v42ec_t E;
    v42ec_cfg_t c; v42ec_status_t s;
    uint8_t buf[8];
    int i, ones;

    /* detection phase: no start-stop mode when T400 runs out */
    v42ec_cfg_default(&c, 0);
    v42ec_init(&E, &c);
    v42ec_abort(&E);
    CHECK_EQ_I(v42ec_state(&E), V42EC_DISCONNECTED);
    for (i = 0; i < 1000; i++) v42ec_tick(&E, 1);
    CHECK_EQ_I(v42ec_state(&E), V42EC_DISCONNECTED);
    v42ec_get_status(&E, &s);
    CHECK_EQ_I(s.reason, V42LAPM_R_LOCAL);

    /* the originator's ODP stops too */
    v42ec_cfg_default(&c, 1);
    v42ec_init(&E, &c);
    for (i = 0; i < 50; i++) v42ec_tx_bit(&E);
    v42ec_abort(&E);
    CHECK_EQ_I(v42ec_state(&E), V42EC_DISCONNECTED);
    for (ones = 0, i = 0; i < 200; i++) ones += v42ec_tx_bit(&E);
    CHECK_EQ_I(ones, 200);

    /* start-stop mode: no more data either way */
    v42ec_cfg_default(&c, 0);
    v42ec_init(&E, &c);
    v42ec_tick(&E, 1000);
    CHECK_EQ_I(v42ec_state(&E), V42EC_ASYNC);
    CHECK_EQ_I(v42ec_write(&E, (const uint8_t *)"abc", 3), 3);
    v42ec_abort(&E);
    CHECK_EQ_I(v42ec_state(&E), V42EC_DISCONNECTED);
    CHECK_EQ_I(v42ec_write(&E, (const uint8_t *)"x", 1), 0);
    for (ones = 0, i = 0; i < 100; i++) ones += v42ec_tx_bit(&E);
    CHECK_EQ_I(ones, 100);                              /* "abc" is not sent */
    v42ec_get_status(&E, &s);
    CHECK_EQ_I(s.reason, V42LAPM_R_LOCAL);

    /* protocol establishment: the XID is never sent */
    setup(); ca.detect = cb.detect = 0; start();
    CHECK_EQ_I(v42ec_state(&A), V42EC_ESTABLISH);
    v42ec_abort(&A);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    run(500);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    CHECK_EQ_I(ta.n, 0);

    /* connected: visible at once, no DISC */
    setup(); connect_pair();
    v42ec_abort(&A);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    v42ec_get_status(&A, &s);
    CHECK_EQ_I(s.reason, V42LAPM_R_LOCAL);
    CHECK_EQ_I(v42ec_write(&A, buf, 1), 0);
    run(1000);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    CHECK_EQ_I(tap_count(&ta, 0x03, 0x53), 0);
    CHECK_EQ_I(v42ec_state(&B), V42EC_CONNECTED);       /* the far end was told nothing */

    /* releasing: the DISC is not repeated */
    setup(); connect_pair();
    pab.cut = pba.cut = 1;
    v42ec_release(&A);
    CHECK_EQ_I(v42ec_state(&A), V42EC_RELEASING);
    run(100);
    i = tap_count(&ta, 0x03, 0x53);
    CHECK_EQ_I(i, 1);
    v42ec_abort(&A);
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    run(3000);
    CHECK_EQ_I(tap_count(&ta, 0x03, 0x53), i);
    v42ec_get_status(&A, &s);
    CHECK_EQ_I(s.reason, V42LAPM_R_LOCAL);

    /* a UA already owed still goes: the far end's DISC was answered */
    setup(); connect_pair();
    v42ec_release(&B);
    while (v42ec_state(&A) == V42EC_CONNECTED && now_ms < 5000) {
        int a = v42ec_tx_bit(&A), b = v42ec_tx_bit(&B);
        tap_bit(&ta, a); tap_bit(&tb, b);
        v42ec_rx_bit(&B, a); v42ec_rx_bit(&A, b);
    }
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);    /* DISC taken: UA owed */
    v42ec_abort(&A);
    v42ec_get_status(&A, &s);
    CHECK_EQ_I(s.reason, V42LAPM_R_PEER_DISC);          /* over already: abort changes nothing */
    run(200);
    CHECK_EQ_I(tap_count(&ta, 0x01, 0x73), 1);
    CHECK_EQ_I(v42ec_state(&B), V42EC_DISCONNECTED);
}

/* compression asked by one side only, or in one direction only */
static void test_comp_negotiation(void)
{
    setup(); cb.lapm.comp_tx = cb.lapm.comp_rx = 0; connect_pair();
    CHECK_EQ_I(A.lapm.comp_tx, 0); CHECK_EQ_I(A.lapm.comp_rx, 0);
    flow_fill(&ab, 5000, F_TEXT); flow_fill(&ba, 5000, F_TEXT);
    CHECK(run_until_done(10000) >= 0);
    check_flow(&ab); check_flow(&ba);
    CHECK_EQ_I(A.enc.bis.in_chars, 0);

    setup(); ca.lapm.comp_rx = 0; connect_pair();       /* only A to B compressed */
    CHECK_EQ_I(A.lapm.comp_tx, 1); CHECK_EQ_I(A.lapm.comp_rx, 0);
    CHECK_EQ_I(B.lapm.comp_tx, 0); CHECK_EQ_I(B.lapm.comp_rx, 1);
    flow_fill(&ab, 20000, F_TEXT); flow_fill(&ba, 5000, F_TEXT);
    CHECK(run_until_done(10000) >= 0);
    check_flow(&ab); check_flow(&ba);
    CHECK_EQ_I(A.enc.bis.in_chars, 20000);
    CHECK(A.enc.bis.out_octets < 10000);

    /* smaller dictionary and string length on one side: the minimum rules */
    setup(); cb.lapm.comp_p1 = 512; cb.lapm.comp_p2 = 6; connect_pair();
    CHECK_EQ_I(A.lapm.comp_n2, 512); CHECK_EQ_I(B.lapm.comp_n2, 512);
    CHECK_EQ_I(A.lapm.comp_n7, 6);   CHECK_EQ_I(B.lapm.comp_n7, 6);
    flow_fill(&ab, 40000, F_TEXT); flow_fill(&ba, 40000, F_RUNS);   /* dictionary recycles */
    CHECK(run_until_done(20000) >= 0);
    check_flow(&ab); check_flow(&ba);
    CHECK(A.enc.bis.d.recovered > 100);
}

/* V.42 bis P1 above what this build holds: proposed at the ceiling, and
 * the encoder and decoder run (they were agreed and then left off) */
static void test_p1_ceiling(void)
{
    v42ec_status_t sa, sb;
    setup();
    ca.lapm.comp_p1 = cb.lapm.comp_p1 = 4096; ca.comp_policy = V42COMP_ALWAYS;
    connect_pair();
    v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
    CHECK_EQ_I(sa.comp_alg, V42EC_COMP_V42BIS);
    CHECK_EQ_I(sa.comp_n2, V42COMP_MAX_CODEWORDS); CHECK_EQ_I(sb.comp_n2, V42COMP_MAX_CODEWORDS);
    CHECK_EQ_I(A.comp_tx, 1); CHECK_EQ_I(A.comp_rx, 1);
    CHECK_EQ_I(B.comp_tx, 1); CHECK_EQ_I(B.comp_rx, 1);
    flow_fill(&ab, 20000, F_TEXT); flow_fill(&ba, 5000, F_TEXT);
    CHECK(run_until_done(10000) >= 0);
    check_flow(&ab); check_flow(&ba);
    CHECK_EQ_I(A.enc.bis.in_chars, 20000);
    CHECK(A.enc.bis.out_octets < 10000);
}

/* a C-INIT that fails (values LAPM should never have agreed) releases the
 * link instead of running uncompressed while the far end compresses */
static void test_cinit_fail(void)
{
    v42ec_status_t s;
    int i;
    setup(); start();
    for (i = 0; i < 3000 && A.lapm.state != V42LAPM_ESTABLISHING; i++) step_ms();
    CHECK_EQ_I(A.lapm.state, V42LAPM_ESTABLISHING);
    CHECK_EQ_I(A.lapm.comp_tx, 1);
    A.lapm.comp_n2 = 4096;                              /* past V42COMP_MAX_CODEWORDS */
    for (i = 0; i < 3000 && v42ec_state(&A) != V42EC_DISCONNECTED; i++) step_ms();
    CHECK_EQ_I(v42ec_state(&A), V42EC_DISCONNECTED);
    v42ec_get_status(&A, &s);
    CHECK_EQ_I(s.reason, V42EC_R_COMPRESSION);
    CHECK_EQ_I(tap_count(&ta, 0x03, 0x53), 1);          /* DISC */
    run(200);
    v42ec_get_status(&B, &s);
    CHECK_EQ_I(s.state, V42EC_DISCONNECTED);
    CHECK_EQ_I(s.reason, V42LAPM_R_PEER_DISC);
}

/* a corrupt compressed stream (cannot happen over LAPM unless one end is
 * broken) is a C-ERROR: the link is released, not left delivering rubbish */
static void test_comp_error(void)
{
    static const uint8_t bad[2] = { 0x00, 0x05 };       /* escape, reserved command */
    v42ec_status_t s;
    setup(); connect_pair();
    CHECK(v42lapm_can_send(&A.lapm) > 0);
    CHECK_EQ_I(v42lapm_send(&A.lapm, bad, 2), 0);
    CHECK(run_until_both(V42EC_DISCONNECTED, 2000) >= 0);
    v42ec_get_status(&B, &s); CHECK_EQ_I(s.reason, V42EC_R_COMPRESSION);
    v42ec_get_status(&A, &s); CHECK_EQ_I(s.reason, V42LAPM_R_PEER_DISC);
    CHECK_EQ_I(tap_count(&tb, 0x01, 0x53), 1);
}

/* ---------------------------------------------------------------- V.44 */
static void test_v44(void)
{
    v42ec_status_t sa, sb;
    int t;

    /* the defaults offer V.44 both ways with the most this build holds, and
     * V.42 bis next to it; two such ends settle on V.44 */
    v42ec_cfg_default(&ca, 1);
    CHECK_EQ_I(ca.lapm.v44_tx, 1); CHECK_EQ_I(ca.lapm.v44_rx, 1);
    CHECK_EQ_I(ca.lapm.v44_p1_tx, V44COMP_MAX_CODEWORDS < 2048 ? V44COMP_MAX_CODEWORDS : 2048);
    CHECK_EQ_I(ca.lapm.v44_p1_rx, ca.lapm.v44_p1_tx);
    CHECK_EQ_I(ca.lapm.v44_p2_tx, 255); CHECK_EQ_I(ca.lapm.v44_p2_rx, 255);
    CHECK_EQ_I(ca.lapm.v44_p3_tx, V44COMP_MAX_HISTORY < 4096 ? V44COMP_MAX_HISTORY : 4096);
    CHECK_EQ_I(ca.lapm.v44_p3_rx, ca.lapm.v44_p3_tx);
    CHECK_EQ_I(ca.lapm.comp_tx, 1); CHECK_EQ_I(ca.lapm.comp_rx, 1);

    setup(); connect_pair();
    v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
    CHECK_EQ_I(sa.comp_alg, V42EC_COMP_V44); CHECK_EQ_I(sb.comp_alg, V42EC_COMP_V44);
    CHECK_EQ_I(sa.comp_tx, 1); CHECK_EQ_I(sa.comp_rx, 1);
    CHECK_EQ_I(sb.comp_tx, 1); CHECK_EQ_I(sb.comp_rx, 1);
    CHECK_EQ_I(sa.v44_n2_tx, ca.lapm.v44_p1_tx); CHECK_EQ_I(sa.v44_n2_rx, ca.lapm.v44_p1_tx);
    CHECK_EQ_I(sa.v44_n7_tx, 255); CHECK_EQ_I(sb.v44_n7_rx, 255);
    CHECK_EQ_I(sa.v44_n8_tx, ca.lapm.v44_p3_tx); CHECK_EQ_I(sb.v44_n8_tx, ca.lapm.v44_p3_tx);
    CHECK_EQ_I(A.lapm.comp_tx, 0); CHECK_EQ_I(B.lapm.comp_rx, 0);
    flow_fill(&ab, 60000, F_TEXT); flow_fill(&ba, 60000, F_RUNS);
    t = run_until_done(30000);
    CHECK(t >= 0);
    check_flow(&ab); check_flow(&ba);
    v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
    CHECK_EQ_I(sa.comp_in, 60000); CHECK_EQ_I(sb.decomp_out, 60000);
    CHECK_EQ_I(sb.comp_in, 60000); CHECK_EQ_I(sa.decomp_out, 60000);
    CHECK(sa.comp_out * 2 < sa.comp_in);
    CHECK_EQ_I(sb.decomp_in, sa.comp_out);
    CHECK(sb.comp_out * 8 < sb.comp_in);
    CHECK(A.enc.v44.reinits >= 10);                     /* the history filled, many times */
    CHECK_EQ_I(B.dec.v44.reinits, A.enc.v44.reinits);
    CHECK(both(V42EC_CONNECTED));

    /* one direction only, and each direction with its own values: here the
     * answerer's encoder is cut down to what a CX93001 proposes */
    setup(); ca.lapm.v44_tx = 0;
    cb.lapm.v44_p1_tx = 512; cb.lapm.v44_p2_tx = 32; cb.lapm.v44_p3_tx = 1024;
    connect_pair();
    v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
    CHECK_EQ_I(sa.comp_alg, V42EC_COMP_V44); CHECK_EQ_I(sb.comp_alg, V42EC_COMP_V44);
    CHECK_EQ_I(sa.comp_tx, 0); CHECK_EQ_I(sa.comp_rx, 1);
    CHECK_EQ_I(sb.comp_tx, 1); CHECK_EQ_I(sb.comp_rx, 0);
    CHECK_EQ_I(sa.v44_n2_rx, 512); CHECK_EQ_I(sa.v44_n7_rx, 32); CHECK_EQ_I(sa.v44_n8_rx, 1024);
    CHECK_EQ_I(sb.v44_n2_tx, 512); CHECK_EQ_I(sb.v44_n7_tx, 32); CHECK_EQ_I(sb.v44_n8_tx, 1024);
    flow_fill(&ab, 20000, F_TEXT); flow_fill(&ba, 40000, F_TEXT);
    CHECK(run_until_done(30000) >= 0);
    check_flow(&ab); check_flow(&ba);
    v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
    CHECK_EQ_I(sa.comp_in, 0); CHECK_EQ_I(sb.decomp_out, 0);
    CHECK_EQ_I(sb.comp_in, 40000); CHECK_EQ_I(sa.decomp_out, 40000);
    CHECK(sb.comp_out * 2 < sb.comp_in);
    CHECK_EQ_I(B.enc.v44.n2, 512); CHECK_EQ_I(A.dec.v44.n8, 1024);

    /* different values at the two ends: the lesser of each (V.44 7.4) */
    setup();
    ca.lapm.v44_p1_tx = 1024; ca.lapm.v44_p2_rx = 64; cb.lapm.v44_p3_rx = 2000; cb.lapm.v44_p1_tx = 300;
    connect_pair();
    CHECK_EQ_I(A.enc.v44.n2, 1024); CHECK_EQ_I(B.dec.v44.n2, 1024);
    CHECK_EQ_I(A.enc.v44.n8, 2000); CHECK_EQ_I(B.dec.v44.n8, 2000);
    CHECK_EQ_I(B.enc.v44.n2, 300); CHECK_EQ_I(A.dec.v44.n2, 300);
    CHECK_EQ_I(B.enc.v44.n7, 64); CHECK_EQ_I(A.dec.v44.n7, 64);
    flow_fill(&ab, 30000, F_TEXT); flow_fill(&ba, 30000, F_TEXT);
    CHECK(run_until_done(30000) >= 0);
    check_flow(&ab); check_flow(&ba);

    /* more asked for than this build holds: brought down before it is
     * proposed, so the far end cannot be told a size we do not have */
    setup();
    ca.lapm.v44_p1_tx = ca.lapm.v44_p1_rx = 60000; ca.lapm.v44_p3_tx = ca.lapm.v44_p3_rx = 60000;
    cb.lapm.v44_p1_tx = cb.lapm.v44_p1_rx = 60000; cb.lapm.v44_p3_tx = cb.lapm.v44_p3_rx = 60000;
    connect_pair();
    v42ec_get_status(&A, &sa);
    CHECK_EQ_I(sa.comp_alg, V42EC_COMP_V44);
    CHECK_EQ_I(sa.v44_n2_tx, V44COMP_MAX_CODEWORDS); CHECK_EQ_I(sa.v44_n2_rx, V44COMP_MAX_CODEWORDS);
    CHECK_EQ_I(sa.v44_n8_tx, V44COMP_MAX_HISTORY); CHECK_EQ_I(sa.v44_n8_rx, V44COMP_MAX_HISTORY);
    flow_fill(&ab, 30000, F_TEXT); flow_fill(&ba, 30000, F_ESC);
    CHECK(run_until_done(30000) >= 0);
    check_flow(&ab); check_flow(&ba);

    /* a far end without V.44: V.42 bis as before, either way round */
    for (t = 0; t < 2; t++) {
        setup();
        if (t) ca.lapm.v44_tx = ca.lapm.v44_rx = 0; else cb.lapm.v44_tx = cb.lapm.v44_rx = 0;
        connect_pair();
        v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
        CHECK_EQ_I(sa.comp_alg, V42EC_COMP_V42BIS); CHECK_EQ_I(sb.comp_alg, V42EC_COMP_V42BIS);
        CHECK_EQ_I(sa.comp_tx, 1); CHECK_EQ_I(sb.comp_rx, 1);
        CHECK_EQ_I(A.lapm.v44_tx, 0); CHECK_EQ_I(B.lapm.v44_tx, 0);
        flow_fill(&ab, 20000, F_TEXT); flow_fill(&ba, 20000, F_TEXT);
        CHECK(run_until_done(30000) >= 0);
        check_flow(&ab); check_flow(&ba);
        CHECK_EQ_I(A.enc.bis.in_chars, 20000);
        CHECK(A.enc.bis.out_octets < 10000);
    }
    /* V.44 alone at both ends; V.44 here and V.42 bis there: no compression */
    setup(); ca.lapm.comp_tx = ca.lapm.comp_rx = cb.lapm.comp_tx = cb.lapm.comp_rx = 0;
    connect_pair();
    CHECK_EQ_I(A.comp_alg, V42EC_COMP_V44); CHECK_EQ_I(B.comp_alg, V42EC_COMP_V44);
    setup(); ca.lapm.comp_tx = ca.lapm.comp_rx = 0; cb.lapm.v44_tx = cb.lapm.v44_rx = 0;
    connect_pair();
    v42ec_get_status(&A, &sa);
    CHECK_EQ_I(sa.comp_alg, V42EC_COMP_NONE); CHECK_EQ_I(sa.comp_tx, 0); CHECK_EQ_I(sa.comp_rx, 0);
    CHECK_EQ_I(B.comp_alg, V42EC_COMP_NONE);
    flow_fill(&ab, 5000, F_TEXT); flow_fill(&ba, 5000, F_TEXT);
    CHECK(run_until_done(10000) >= 0);
    check_flow(&ab); check_flow(&ba);
    v42ec_get_status(&A, &sa);
    CHECK_EQ_I(sa.comp_in, 0);

    /* a corrupt compressed stream is a C-ERROR (V.44 7.15) and ends the
     * connection: here a codeword far beyond any the decoder has */
    {
        static const uint8_t bad[2] = { 0x7F, 0x00 };
        setup(); connect_pair();
        CHECK(v42lapm_can_send(&A.lapm) > 0);
        CHECK_EQ_I(v42lapm_send(&A.lapm, bad, 2), 0);
        CHECK(run_until_both(V42EC_DISCONNECTED, 2000) >= 0);
        v42ec_get_status(&B, &sb); CHECK_EQ_I(sb.reason, V42EC_R_COMPRESSION);
        v42ec_get_status(&A, &sa); CHECK_EQ_I(sa.reason, V42LAPM_R_PEER_DISC);
    }

    /* every octet value one at a time, each waited for: the flush of 7.13
     * gets it across with nothing held back, whatever the policy */
    {
        int i, policy;
        for (policy = V44COMP_AUTO; policy <= V44COMP_NEVER; policy++) {
            setup(); ca.comp_policy = cb.comp_policy = policy; connect_pair();
            ab.reading = ba.reading = 0;
            for (i = 0; i < 300; i++) {
                uint8_t c = (uint8_t)(i < 256 ? i : 'a' + i % 3), buf[4];
                CHECK_EQ_I(v42ec_write(&A, &c, 1), 1);
                run(50);
                CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 1);
                CHECK_EQ_I(buf[0], c);
            }
            CHECK(v42ec_tx_idle(&A));
            CHECK_EQ_I(A.enc.v44.in_chars, 300);
        }
    }

    /* the line at 1e-4 with a small window and long frames, and bursts on
     * top: retransmission keeps the two dictionaries in step */
    setup(); rs = 4242u;
    ca.lapm.k_tx = ca.lapm.k_rx = cb.lapm.k_tx = cb.lapm.k_rx = 7;
    ca.lapm.n401_tx = ca.lapm.n401_rx = cb.lapm.n401_tx = cb.lapm.n401_rx = V42_MAX_N401;
    connect_pair();
    pab.ber = pba.ber = 429497u;
    flow_fill(&ab, 60000, F_TEXT); flow_fill(&ba, 60000, F_RUNS);
    for (t = 0; t < 300000 && !flows_done(); t++) {
        pab.corrupt = (t % 900) < 40;
        pba.corrupt = ((t + 450) % 1100) < 50;
        step_ms();
    }
    pab.corrupt = pba.corrupt = 0;
    CHECK(run_until_done(30000) >= 0);
    check_flow(&ab); check_flow(&ba);
    CHECK(A.lapm.tx_i_retx >= 3 && B.lapm.tx_i_retx >= 3);
    CHECK_EQ_I(B.dec.v44.out_chars, 60000); CHECK_EQ_I(A.dec.v44.out_chars, 60000);
    CHECK(both(V42EC_CONNECTED));
}

/* -------------------------------------------------------------- break */
static void test_break(void)
{
    uint8_t buf[64]; int opt = -1, len = -1;
    setup(); connect_pair();
    ab.reading = ba.reading = 0;

    /* in sequence: arrives between the data written before and after it */
    CHECK_EQ_I(v42ec_write(&A, (const uint8_t *)"abc", 3), 3);
    CHECK_EQ_I(v42ec_send_break(&A, V42_BRK_NONDESTRUCTIVE_INSEQ, 5), 0);
    CHECK_EQ_I(v42ec_write(&A, (const uint8_t *)"def", 3), 3);
    run(500);
    CHECK_EQ_I(v42ec_get_break(&B, &opt, &len), 0);     /* "abc" comes first */
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 3);
    CHECK(memcmp(buf, "abc", 3) == 0);
    CHECK_EQ_I(v42ec_get_break(&B, &opt, &len), 1);
    CHECK_EQ_I(opt, V42_BRK_NONDESTRUCTIVE_INSEQ); CHECK_EQ_I(len, 5);
    CHECK_EQ_I(v42ec_get_break(&B, &opt, &len), 0);
    run(300);
    CHECK_EQ_I(v42ec_read(&B, buf, sizeof buf), 3);
    CHECK(memcmp(buf, "def", 3) == 0);
    CHECK(v42ec_tx_idle(&A));

    /* expedited: overtakes queued data, loses none */
    flow_fill(&ab, 6000, F_RANDOM); ab.reading = 0;
    run(200);
    CHECK_EQ_I(v42ec_send_break(&A, V42_BRK_NONDESTRUCTIVE_EXPEDITED, 0), 0);
    run(300);
    CHECK_EQ_I(v42ec_get_break(&B, &opt, &len), 1);
    CHECK_EQ_I(opt, V42_BRK_NONDESTRUCTIVE_EXPEDITED);
    CHECK(ab.pos < 6000 || !v42ec_tx_idle(&A));         /* data was still on its way */
    ab.reading = 1;
    CHECK(run_until_done(10000) >= 0);
    check_flow(&ab);

    /* destructive: data in transit both ways is thrown away, the link and
     * the compressors carry on in step afterwards (V.42 bis 5.6) */
    flow_fill(&ab, 6000, F_TEXT); flow_fill(&ba, 6000, F_TEXT);
    ab.reading = ba.reading = 0;
    run(400);
    CHECK(v42ec_read_avail(&B) > 0);
    ab.len = ab.pos; ba.len = ba.pos;                   /* stop the writers */
    CHECK_EQ_I(v42ec_send_break(&A, V42_BRK_DESTRUCTIVE_EXPEDITED, 0), 0);
    run(300);
    CHECK_EQ_I(v42ec_get_break(&B, &opt, &len), 1);
    CHECK_EQ_I(opt, V42_BRK_DESTRUCTIVE_EXPEDITED);
    CHECK_EQ_I(v42ec_read_avail(&B), 0);
    CHECK_EQ_I(v42ec_read_avail(&A), 0);
    CHECK(v42ec_tx_idle(&A) && v42ec_tx_idle(&B));
    CHECK(both(V42EC_CONNECTED));
    flow_fill(&ab, 8000, F_TEXT); flow_fill(&ba, 8000, F_TEXT);
    CHECK(run_until_done(10000) >= 0);
    check_flow(&ab); check_flow(&ba);

    /* not connected: refused */
    setup(); start();
    CHECK_EQ_I(v42ec_send_break(&A, V42_BRK_NONDESTRUCTIVE_INSEQ, 0), -1);
}

/* ------------------------------------------------- octet-wise line pump */
static void test_octet_pump(void)
{
    uint8_t la[4], lb[4];
    int t;
    setup(); start();
    flow_fill(&ab, 20000, F_TEXT); flow_fill(&ba, 20000, F_RANDOM);
    for (t = 0; t < 30000 && !(flows_done() && both(V42EC_CONNECTED)); t++) {
        v42ec_tx_octets(&A, la, 4); v42ec_tx_octets(&B, lb, 4);
        v42ec_rx_octets(&B, la, 4); v42ec_rx_octets(&A, lb, 4);
        pump(&A, &ab, &B); pump(&B, &ba, &A);
        v42ec_tick(&A, 1); v42ec_tick(&B, 1);
    }
    CHECK(both(V42EC_CONNECTED));
    check_flow(&ab); check_flow(&ba);
}

/* coarse ticks and bursty line service, as a real modem loop would do */
static void test_coarse_timing(void)
{
    int t, i;
    setup(); start();
    flow_fill(&ab, 20000, F_TEXT); flow_fill(&ba, 20000, F_RANDOM);
    pab.ber = pba.ber = 214748u;                        /* 5e-5 */
    for (t = 0; t < 20000 && !(flows_done() && both(V42EC_CONNECTED)); t++) {
        for (i = 0; i < 20 * BITS_PER_MS; i++) {
            int a = v42ec_tx_bit(&A), b = v42ec_tx_bit(&B);
            v42ec_rx_bit(&B, pipe_bit(&pab, a));
            v42ec_rx_bit(&A, pipe_bit(&pba, b));
            if (i % 64 == 0) { pump(&A, &ab, &B); pump(&B, &ba, &A); }
        }
        v42ec_tick(&A, 20); v42ec_tick(&B, 20);
    }
    CHECK(both(V42EC_CONNECTED));
    check_flow(&ab); check_flow(&ba);
}

/* v42ec_tick with any ms: past INT_MAX it expires the timers, not
 * extends them */
static void test_tick_huge(void)
{
    static v42ec_t E;
    v42ec_cfg_t c;
    v42ec_cfg_default(&c, 0);
    v42ec_init(&E, &c);
    CHECK_EQ_I(v42ec_state(&E), V42EC_DETECT);
    v42ec_tick(&E, UINT_MAX);
    CHECK_EQ_I(E.t400, -1);                             /* T400 expired: no V.42 peer */
    CHECK_EQ_I(v42ec_state(&E), V42EC_ASYNC);
    v42ec_tick(&E, 0x80000000u);
    CHECK_EQ_I(v42ec_state(&E), V42EC_ASYNC);
}

int main(void)
{
    test_handshake();
    test_handshake_noisy();
    test_bulk();
    test_compression_gain();
    test_interactive();
    test_bit_errors();
    test_bursts();
    test_t401_recovery();
    test_flow_control(0);
    test_flow_control(1);
    test_fallback_answerer();
    test_fallback_originator();
    test_no_answer();
    test_line_cut();
    test_resume_without_flag();
    test_restore_flags();
    test_detect_flagless();
    test_release(1);
    test_release(0);
    test_release_unanswered();
    test_detection_off();
    test_detection_mirror();
    test_abort_states();
    test_comp_negotiation();
    test_comp_error();
    test_p1_ceiling();
    test_cinit_fail();
    test_break();
    test_octet_pump();
    test_coarse_timing();
    test_tick_huge();

    /* V.44: its own cases, then the data tests above once more with V.44
     * doing the compression */
    use_v44 = 1;
    test_v44();
    test_bulk();
    test_compression_gain();
    test_interactive();
    test_bit_errors();
    test_bursts();
    test_flow_control(1);
    test_break();
    test_octet_pump();
    test_coarse_timing();
    T_REPORT("test_v42");
}
