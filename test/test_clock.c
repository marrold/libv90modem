/* Sample clocks past 2^31 samples (74.6 hours at 8 kHz): modem/clock.h, and
 * every modem's data mode with its clocks moved on to 8000 samples short of
 * 2^31, as if the call had been up that long: three more seconds of data
 * both ways with nothing lost (and, under check-ubsan, no signed overflow).
 * After the end, the answerer's and the caller's phase clocks stop at
 * INT32_MAX and the end stays as it was. */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "test.h"
#include "../modem/clock.h"
#include "../modem/answer.h"
#include "../modem/k56d.h"
#include "../sim/x2call.h"
#include "../sim/k56call.h"
#include "../sim/v34peer.h"
#include "../sim/callpeer.h"
#include "../sim/g711.h"
#include "../sim/noise.h"
#include "line34.h"
#include "v90link.h"

#define NEAR 2147475648L            /* 2^31 - 8000 */
#define RUN_S 3
#define MS(x) ((int64_t)(x) * 8)

/* n moved on by a multiple of step (> 0) to within step of NEAR: what the
 * clock would read that much later, the same modulo step */
static long long near_of(long long n, long step) { return n + (NEAR - n) / step * step; }

/* ---- the clocks moved on ---- */

static void jump_answer(answer_t *a) { a->t = (int32_t)NEAR; }
/* the V.34 receiver (qamrx): its T/3 output index by a multiple of 3 (it
 * is read modulo 3), its symbol count */
static void jump_qamrx(qamrx_t *r) { r->m = (int32_t)near_of(r->m, 3); r->nsym = (int32_t)NEAR; }
static void jump_v90a(v90a_t *a)
{
    v90arx_t *r = &a->rx;
    int32_t k = clk_diff(r->n, r->n0);
    a->now = (uint32_t)NEAR; a->t = (uint32_t)NEAR; a->un = (int32_t)NEAR;
    /* the receiver: its sample number n and k = n - n0 both 8000 short of
     * 2^31, k by a multiple of 768 (the data frame of 6, its rings of 256)
     * on from what it was */
    r->n0 = (uint32_t)(NEAR - near_of(k, 768));
    r->n = (uint32_t)NEAR;
    r->neq = (uint32_t)NEAR; r->nest = (int32_t)NEAR;
    a->mod.nsym = (int32_t)NEAR; a->dly_pos = (int)(0x7FFFFF00 + (a->dly_pos & 1));
}
static void jump_orig(orig_t *o) { o->t = (int32_t)NEAR; jump_v90a(&o->e.v90); }
static void jump_v92d(v92d_t *p)
{
    int32_t d = (int32_t)(near_of(p->trn.k, 768) - p->trn.k);
    p->now = (uint32_t)NEAR; p->t = (uint32_t)NEAR;
    p->trn.k += d; p->data_k += d;
}

/* ---- V.90 and V.92: our caller and our answerer (test/v90link.h) ---- */

static vl_t VL;

static void pcm_call(int v92)
{
    vl_cfg c;
    vl_t *l = &VL;
    int64_t d0, u0, f0d, f0u, e;
    const char *what = v92 ? "V.92" : "V.90";
    memset(&c, 0, sizeof c);
    c.loop_km = 2; c.noise_dbm0 = -75; c.ppm = 30; c.echo_db = -16;
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.up_line = v92; c.full = 1; c.caller_orig = 1; c.seed = 7;
    c.v92 = v92; c.caller_v92 = v92;
    vl_init(l, &c);
    while (l->k < MS(40000) && !(orig_moving(&l->org) && answer_moving(&l->ans)) && l->org.phase != ORIG_FAILED) vl_tick(l);
    CHECK(orig_moving(&l->org) && answer_moving(&l->ans));
    CHECK_EQ_I(l->org.modulation, v92 ? ORIG_MOD_V92 : ORIG_MOD_V90);
    e = l->k + MS(300); while (l->k < e) vl_tick(l);
    jump_answer(&l->ans); jump_orig(&l->org);
    if (v92) jump_v92d(&l->ans.u.v92);
    else { l->ans.p34.now = (uint32_t)NEAR; l->ans.p34.t = (uint32_t)NEAR; jump_qamrx(&l->ans.p34.up.rx); }
    d0 = l->down.got; u0 = l->up.got; f0d = l->down.fails; f0u = l->up.fails;
    e = l->k + MS(RUN_S * 1000); while (l->k < e) vl_tick(l);
    printf("  %s: %d bit/s down, %d up; after the clocks' wrap %" PRId64 " / %" PRId64 " bits, %" PRId64 " / %" PRId64 " wrong\n",
           what, l->org.rx_bitrate, l->org.bitrate, l->down.got - d0, l->up.got - u0, l->down.fails - f0d, l->up.fails - f0u);
    CHECK(orig_moving(&l->org)); CHECK(answer_moving(&l->ans));
    CHECK(l->down.got - d0 > (int64_t)(RUN_S - 1) * l->org.rx_bitrate);
    CHECK(l->up.got - u0 > (int64_t)(RUN_S - 1) * l->org.bitrate);
    CHECK_EQ_I(l->down.fails - f0d, 0); CHECK_EQ_I(l->up.fails - f0u, 0);
    /* the clocks went past 2^31; the symbol numbers were brought back */
    CHECK(l->org.e.v90.now > 0x80000000u); CHECK(l->org.e.v90.rx.n > 0x80000000u);
    CHECK(clk_diff(l->org.e.v90.rx.n, l->org.e.v90.rx.n0) < CLK_REBASE_AT);
    CHECK_EQ_I(l->org.t, INT32_MAX); CHECK_EQ_I(l->ans.t, INT32_MAX);
    if (v92) { CHECK(l->ans.u.v92.now > 0x80000000u); CHECK(l->ans.u.v92.trn.k < 0x40000000); }
    else {
        CHECK(l->ans.p34.now > 0x80000000u);
        /* the symbol count went on past 2^31 - 1 from 0: its low bits keep
         * cycling (the centroid nudge every 1024 symbols, qamrx.c) */
        printf("  V.90: the answerer's symbol count %d\n", (int)l->ans.p34.up.rx.nsym);
        CHECK(l->ans.p34.up.rx.nsym >= 0 && l->ans.p34.up.rx.nsym < RUN_S * 3429 - 8000);
    }
    /* a rate renegotiation of ours on the symbol numbers brought back (V.90:
     * Rd, TRN2d on m_b1d's frames; V.92 also the digital modem's Ru and its
     * receiver's resume on trn.k): data again both ways, nothing wrong */
    CHECK_EQ_I(orig_renegotiate(&l->org, 40000), 0);
    e = l->k + MS(1000); while (l->k < e) vl_tick(l);
    e = l->k + MS(20000);
    while (l->k < e && !(orig_moving(&l->org) && answer_moving(&l->ans)) && l->org.phase != ORIG_FAILED) vl_tick(l);
    e = l->k + MS(300); while (l->k < e) vl_tick(l);
    d0 = l->down.got; u0 = l->up.got; f0d = l->down.fails; f0u = l->up.fails;
    e = l->k + MS(2000); while (l->k < e) vl_tick(l);
    printf("  %s renegotiated: %d bit/s down, %d up; %" PRId64 " / %" PRId64 " bits, %" PRId64 " / %" PRId64 " wrong; renegotiations %d, retrains %d\n",
           what, l->org.rx_bitrate, l->org.bitrate, l->down.got - d0, l->up.got - u0, l->down.fails - f0d, l->up.fails - f0u,
           (int)l->org.e.v90.renegs, orig_retrains(&l->org));
    CHECK(orig_moving(&l->org)); CHECK(answer_moving(&l->ans));
    CHECK(l->org.rx_bitrate <= 40000); CHECK_EQ_I(l->org.e.v90.renegs, 1); CHECK_EQ_I(orig_retrains(&l->org), 0);
    CHECK(l->down.got - d0 > (int64_t)l->org.rx_bitrate); CHECK(l->up.got - u0 > (int64_t)l->org.bitrate);
    CHECK_EQ_I(l->down.fails - f0d, 0); CHECK_EQ_I(l->up.fails - f0u, 0);
}

/* the caller's Sd counters (v90arx in V90ARX_WAIT): at the limit while Sd
 * is heard; the call goes on with no overflow (what it makes of a level
 * measured over 2^31 samples is not checked) */
static void sd_counters(void)
{
    vl_cfg c;
    vl_t *l = &VL;
    int64_t e;
    int hit = 0;
    memset(&c, 0, sizeof c);
    c.loop_km = 2; c.noise_dbm0 = -75; c.echo_db = -16;
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.full = 1; c.caller_orig = 1; c.seed = 7;
    vl_init(l, &c);
    while (l->k < MS(40000) && l->org.phase != ORIG_FAILED && !orig_moving(&l->org)) {
        vl_tick(l);
        if (!hit && l->org.phase == ORIG_V90 && l->org.e.v90.rx_on && l->org.e.v90.rx.state == V90ARX_WAIT && l->org.e.v90.rx.sd_cnt > 0) {
            l->org.e.v90.rx.sd_cnt = INT32_MAX - 2; l->org.e.v90.rx.sd_np = INT32_MAX - 2;
            hit = 1;
            e = l->k + MS(1000); while (l->k < e) vl_tick(l);
            break;
        }
    }
    CHECK_EQ_I(hit, 1);
}

/* ---- x2 and K56flex: our answerer against the simulated clients ---- */

#define XD 4096
typedef struct { uint32_t tx, rx; long n, bad; } rbits_t;
static int rnext(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return (int)(*s & 1); }
static rbits_t DN, UP;
static int  x_aget(void *u) { (void)u; return rnext(&DN.tx); }
static void x_aput(void *u, int bit) { (void)u; if (rnext(&UP.rx) != (bit & 1)) UP.bad++; UP.n++; }
static int  x_cget(void *u) { (void)u; return rnext(&UP.tx); }
static void x_cput(void *u, int bit) { (void)u; if (rnext(&DN.rx) != (bit & 1)) DN.bad++; DN.n++; }

static answer_t A;
static x2call_t X;
static k56call_t K;

/* one digital call: kind 0 x2, 1 K56flex */
static void dig_call(int kind)
{
    static uint8_t down[XD]; static int16_t up[XD]; static double dl[XD];
    const int d_ac = 37, d_ca = 41;
    answer_cfg ac; rng_t rng;
    uint8_t arx = 0xff;
    long n, data_at = -1, jump_at = -1, dn0 = 0, up0 = 0, dbad0 = 0, ubad0 = 0;
    const char *what = kind ? "K56flex" : "x2";
    memset(down, kind ? 0xff : 0x7f, sizeof down); memset(up, 0, sizeof up); memset(dl, 0, sizeof dl);
    memset(&DN, 0, sizeof DN); memset(&UP, 0, sizeof UP);
    DN.tx = DN.rx = 0x2468ace1u; UP.tx = UP.rx = 0x13579bdfu;
    rng_seed(&rng, 4242);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21 | (kind ? ANS_MODE_K56 : ANS_MODE_X2);
    ac.getbit = x_aget; ac.putbit = x_aput; ac.user = NULL;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, 0, 0);
    if (kind) {
        k56call_cfg kc;
        k56call_default(&kc, 3000);
        kc.rate = 13; kc.j_octet = -1; kc.up_mask = 0x3fff;
        kc.getbit = x_cget; kc.putbit = x_cput; kc.user = NULL;
        k56call_init(&K, &kc);
    } else {
        x2call_cfg xc;
        memset(&xc, 0, sizeof xc);
        xc.level = 3000; xc.max_index = 1; xc.up_mask = 0x03FE; xc.silent = X2C_SIL_NONE;
        xc.getbit = x_cget; xc.putbit = x_cput; xc.user = NULL;
        x2call_init(&X, &xc);
    }
    for (n = 0; n < 8000L * 60; n++) {
        int i = (int)(n & (XD - 1));
        double v;
        uint8_t atx = answer_tick_pcm(&A, arx);
        down[i] = atx;
        dl[i] = (double)ulaw_to_linear(atx);
        up[i] = kind ? k56call_tick(&K, down[(n - d_ac) & (XD - 1)]) : x2call_tick(&X, down[(n - d_ac) & (XD - 1)]);
        v = 0.5 * up[(n - d_ca) & (XD - 1)]
            + 0.12 * dl[(n - 2) & (XD - 1)] + 0.08 * dl[(n - 4) & (XD - 1)] - 0.05 * dl[(n - 9) & (XD - 1)]
            + 2.0 * rng_gauss(&rng);
        arx = linear_to_ulaw((int32_t)lround(v));
        if (A.phase == ANS_FAILED) break;
        if (data_at < 0 && answer_moving(&A) && (kind || x2call_state(&X) == X2C_DATA)) data_at = n;
        if (data_at >= 0 && jump_at < 0 && n == data_at + 8000L) {
            jump_at = n;
            jump_answer(&A);
            if (kind) {
                A.u.k56.now = (uint32_t)NEAR; A.u.k56.t = (uint32_t)NEAR; jump_qamrx(&A.u.k56.up.rx);
                A.u.k56.tx.n = (int32_t)NEAR; A.u.k56.tx.alt = (int32_t)NEAR;
            } else {
                A.u.x2.now = (uint32_t)NEAR; A.u.x2.t = (uint32_t)NEAR; jump_qamrx(&A.u.x2.up.rx);
                A.u.x2.tx.n = (int32_t)NEAR;
            }
            dn0 = DN.n; up0 = UP.n; dbad0 = DN.bad; ubad0 = UP.bad;
        }
        if (jump_at >= 0 && n == jump_at + 8000L * RUN_S) break;
    }
    printf("  %s: %d bit/s down, %d up; after the clocks' wrap %ld / %ld bits, %ld / %ld wrong\n",
           what, A.bitrate, A.rx_bitrate, DN.n - dn0, UP.n - up0, DN.bad - dbad0, UP.bad - ubad0);
    CHECK(jump_at >= 0);
    CHECK(answer_moving(&A));
    CHECK(DN.n - dn0 > (long)(RUN_S - 1) * A.bitrate); CHECK(UP.n - up0 > (long)(RUN_S - 1) * A.rx_bitrate);
    CHECK_EQ_I(DN.bad - dbad0, 0); CHECK_EQ_I(UP.bad - ubad0, 0);
}

/* ---- V.34: our answerer against sim/v34peer ---- */

static v34peer_t C; static line34_t L;
static int16_t v34_a(void *u, int16_t rx) { return answer_tick((answer_t *)u, rx); }
static int16_t v34_c(void *u, int16_t rx) { return v34peer_tick((v34peer_t *)u, rx); }

static void v34_call(void)
{
    answer_cfg ac; v34peer_cfg pc; line34_cfg lc;
    long n, dn0, up0, dbad0, ubad0;
    memset(&lc, 0, sizeof lc);
    lc.law = 1; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
    memset(&DN, 0, sizeof DN); memset(&UP, 0, sizeof UP);
    DN.tx = DN.rx = 0x2468ace1u; UP.tx = UP.rx = 0x13579bdfu;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; ac.getbit = x_aget; ac.putbit = x_aput;
    v34peer_default(&pc, 3000);
    pc.dx.getbit = x_cget; pc.dx.putbit = x_cput;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, 0, 0);
    v34peer_init(&C, &pc); line34_init(&L, &lc);
    for (n = 0; n < 8000L * 25 && !(answer_moving(&A) && C.state == VP_DATA) && A.phase != ANS_FAILED; n++)
        (void)line34_step(&L, v34_a, &A, v34_c, &C);
    CHECK(answer_moving(&A)); CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    for (n = 0; n < 8000L; n++) (void)line34_step(&L, v34_a, &A, v34_c, &C);
    jump_answer(&A); A.v34.dx.now = (uint32_t)NEAR; A.v34.dx.t = (uint32_t)NEAR;
    jump_qamrx(&A.v34.dx.up.rx); A.v34.dx.mod.nsym = (int32_t)NEAR;
    dn0 = DN.n; up0 = UP.n; dbad0 = DN.bad; ubad0 = UP.bad;
    for (n = 0; n < 8000L * RUN_S; n++) (void)line34_step(&L, v34_a, &A, v34_c, &C);
    printf("  V.34: %d bit/s down, %d up; after the clocks' wrap %ld / %ld bits, %ld / %ld wrong\n",
           A.bitrate, A.rx_bitrate, DN.n - dn0, UP.n - up0, DN.bad - dbad0, UP.bad - ubad0);
    CHECK(answer_moving(&A));
    CHECK(DN.n - dn0 > (long)(RUN_S - 1) * A.bitrate); CHECK(UP.n - up0 > (long)(RUN_S - 1) * A.rx_bitrate);
    CHECK_EQ_I(DN.bad - dbad0, 0); CHECK_EQ_I(UP.bad - ubad0, 0);
}

/* ---- V.22bis: our answerer against sim/callpeer, both sides' v22bis ---- */

static callpeer_t P;
/* x^7 + x^6 + 1, checked by prediction from the seven bits before: no
 * alignment needed (V.22bis hands on bits from its training too) */
typedef struct { unsigned tx, rx; long n, bad; } prbs7_t;
static prbs7_t SA, SP;     /* what the answerer sends, what the caller sends */
static int p7_get(prbs7_t *p)
{
    int b = (int)(((p->tx >> 6) ^ (p->tx >> 5)) & 1);
    p->tx = ((p->tx << 1) | (unsigned)b) & 127;
    return b;
}
static void p7_put(prbs7_t *p, int bit)
{
    int want = (int)(((p->rx >> 6) ^ (p->rx >> 5)) & 1);
    if (p->n >= 7 && want != (bit & 1)) p->bad++;
    p->rx = ((p->rx << 1) | (unsigned)(bit & 1)) & 127;
    p->n++;
}
static int  v22_aget(void *u) { (void)u; return p7_get(&SA); }
static void v22_aput(void *u, int bit) { (void)u; p7_put(&SP, bit); }
static int  v22_cget(void *u) { (void)u; return p7_get(&SP); }
static void v22_cput(void *u, int bit) { (void)u; p7_put(&SA, bit); }

static void v22_call(void)
{
    answer_cfg ac;
    int16_t atx = 0, ctx = 0;
    long n, dn0, up0, dbad0, ubad0;
    memset(&SA, 0, sizeof SA); memset(&SP, 0, sizeof SP); SA.tx = 1; SP.tx = 77;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21; ac.getbit = v22_aget; ac.putbit = v22_aput;
    answer_init(&A, &ac);
    callpeer_init(&P, 1, V8_MOD_V22 | V8_MOD_V21, 1, 3000);
    callpeer_set_pump(&P, v22_cget, v22_cput, NULL);
    for (n = 0; n < 8000L * 20 && !(A.connected && P.connected) && A.phase != ANS_FAILED; n++) {
        int16_t t = answer_tick(&A, ctx);
        ctx = callpeer_tick(&P, atx); atx = t;
    }
    CHECK(A.connected && P.connected); CHECK_EQ_I(A.modulation, ANS_MOD_V22BIS);
    for (n = 0; n < 8000L; n++) { int16_t t = answer_tick(&A, ctx); ctx = callpeer_tick(&P, atx); atx = t; }
    jump_answer(&A);
    A.v22.t = (int32_t)NEAR; P.v22.t = (int32_t)NEAR;
    dn0 = SA.n; up0 = SP.n; dbad0 = SA.bad; ubad0 = SP.bad;
    for (n = 0; n < 8000L * RUN_S; n++) { int16_t t = answer_tick(&A, ctx); ctx = callpeer_tick(&P, atx); atx = t; }
    printf("  V.22bis: %d bit/s; after the clocks' wrap %ld / %ld bits, %ld / %ld wrong\n",
           A.bitrate, SA.n - dn0, SP.n - up0, SA.bad - dbad0, SP.bad - ubad0);
    CHECK(A.connected && P.connected);
    CHECK(SA.n - dn0 > (long)(RUN_S - 1) * A.bitrate); CHECK(SP.n - up0 > (long)(RUN_S - 1) * A.bitrate);
    CHECK_EQ_I(SA.bad - dbad0, 0); CHECK_EQ_I(SP.bad - ubad0, 0);
}

/* ---- after the end: ticks go on (the API allows it) ---- */

static void after_end(void)
{
    answer_cfg ac;
    int n, end;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21; ac.getbit = x_aget; ac.putbit = x_aput;
    answer_init(&A, &ac);
    for (n = 0; n < 8000 * 120 && A.phase != ANS_FAILED; n++) (void)answer_tick(&A, 0);
    CHECK_EQ_I(A.phase, ANS_FAILED);
    end = A.end;
    A.t = INT32_MAX - 10;
    for (n = 0; n < 100; n++) (void)answer_tick(&A, 0);
    CHECK_EQ_I(A.t, INT32_MAX); CHECK_EQ_I(A.end, end); CHECK_EQ_I(A.phase, ANS_FAILED);
    {
        orig_t *o = &VL.org;
        orig_cfg oc;
        memset(&oc, 0, sizeof oc);
        oc.level = 3000; oc.modes = V8_MOD_V22 | V8_MOD_V21; oc.getbit = x_aget; oc.putbit = x_aput;
        orig_init(o, &oc);
        for (n = 0; n < 8000 * 120 && o->phase != ORIG_FAILED; n++) (void)orig_tick(o, 0);
        CHECK_EQ_I(o->phase, ORIG_FAILED);
        end = o->end;
        o->t = INT32_MAX - 10;
        for (n = 0; n < 100; n++) (void)orig_tick(o, 0);
        CHECK_EQ_I(o->t, INT32_MAX); CHECK_EQ_I(o->end, end); CHECK_EQ_I(o->phase, ORIG_FAILED);
    }
}

int main(void)
{
    CHECK_EQ_I(clk_diff(5u, 0xFFFFFFFBu), 10); CHECK_EQ_I(clk_diff(0u, 1u), -1);
    CHECK_EQ_I(clk_diff(0x80000000u, 0u), INT32_MIN); CHECK_EQ_I(clk_diff(0x7FFFFFFFu, 0u), INT32_MAX);
    CHECK_EQ_I(clk_diff(0u, 0x80000000u), INT32_MIN); CHECK_EQ_I(clk_diff(7u, 7u), 0);
    CHECK_EQ_I(clk_sat_inc(INT32_MAX), INT32_MAX); CHECK_EQ_I(clk_sat_inc(INT32_MAX - 1), INT32_MAX);
    CHECK_EQ_I(clk_sat_inc(0), 1);

    pcm_call(0);
    pcm_call(1);
    sd_counters();
    dig_call(0);
    dig_call(1);
    v34_call();
    v22_call();
    after_end();
    T_REPORT("clock");
}
