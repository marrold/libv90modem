/* The V.42 endpoint's field lists (api/v90modem_fields.h) against the code
 * they replaced, kept here as it was (old_*): the range check, cfg_in,
 * the defaults out and the status out give the same results for every
 * field at and around each bound, INT_MIN and INT_MAX, one field at a time
 * and many at once. The range check is also run through the library
 * (v90m_ec_init and v90m_ec_reset from libv90modem.a as installed). */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "v90modem.h"
#include "test.h"
#include "../../api/v90modem_fields.h"

/* ---- the code before the lists (api/v90modem.c, 0.6.0 Phase 1) ---- */
static int old_in(int v, int lo, int hi) { return v >= lo && v <= hi; }
static int old_flag(int v) { return v == 0 || v == 1; }

static int old_check_ec_cfg(const v90m_ec_cfg *c)
{
    if (!old_flag(c->originator) || !old_flag(c->detect) || !old_flag(c->fallback) || c->t400_ms < 0 || c->establish_ms < 0
        || !old_in(c->comp_policy, V90M_COMP_AUTO, V90M_COMP_NEVER))
        return V90M_EINVAL;
    if (!old_flag(c->lapm.use_xid) || !old_flag(c->lapm.fcs32) || !old_flag(c->lapm.v42bis_tx) || !old_flag(c->lapm.v42bis_rx)
        || !old_flag(c->lapm.v44_tx) || !old_flag(c->lapm.v44_rx))
        return V90M_EINVAL;
    if (!old_in(c->lapm.k_tx, 1, V42_MAX_K) || !old_in(c->lapm.k_rx, 1, 127)
        || !old_in(c->lapm.n401_tx, 1, V42_MAX_N401) || !old_in(c->lapm.n401_rx, 1, V42_MAX_N401))
        return V90M_EINVAL;
    if (!old_in(c->lapm.v42bis_p1, 512, V42COMP_MAX_CODEWORDS) || !old_in(c->lapm.v42bis_p2, 6, 250))
        return V90M_EINVAL;
    if (!old_in(c->lapm.v44_p1_tx, 256, V44COMP_MAX_CODEWORDS) || !old_in(c->lapm.v44_p1_rx, 256, V44COMP_MAX_CODEWORDS)
        || !old_in(c->lapm.v44_p2_tx, 32, 255) || !old_in(c->lapm.v44_p2_rx, 32, 255)
        || !old_in(c->lapm.v44_p3_tx, 512, V44COMP_MAX_HISTORY) || !old_in(c->lapm.v44_p3_rx, 512, V44COMP_MAX_HISTORY))
        return V90M_EINVAL;
    if (c->lapm.t401_ms < 1 || c->lapm.t403_ms < 0 || c->lapm.n400 < 1) return V90M_EINVAL;
    return V90M_OK;
}

static void old_cfg_in(v42ec_cfg_t *o, const v90m_ec_cfg *c)
{
    memset(o, 0, sizeof *o);
    o->originator = c->originator; o->detect = c->detect; o->fallback = c->fallback;
    o->t400_ms = c->t400_ms; o->establish_ms = c->establish_ms; o->comp_policy = c->comp_policy;
    o->lapm.use_xid = c->lapm.use_xid;
    o->lapm.k_tx = c->lapm.k_tx; o->lapm.k_rx = c->lapm.k_rx;
    o->lapm.n401_tx = c->lapm.n401_tx; o->lapm.n401_rx = c->lapm.n401_rx;
    o->lapm.fcs32 = c->lapm.fcs32;
    o->lapm.comp_tx = c->lapm.v42bis_tx; o->lapm.comp_rx = c->lapm.v42bis_rx;
    o->lapm.comp_p1 = c->lapm.v42bis_p1; o->lapm.comp_p2 = c->lapm.v42bis_p2;
    o->lapm.v44_tx = c->lapm.v44_tx; o->lapm.v44_rx = c->lapm.v44_rx;
    o->lapm.v44_p1_tx = c->lapm.v44_p1_tx; o->lapm.v44_p1_rx = c->lapm.v44_p1_rx;
    o->lapm.v44_p2_tx = c->lapm.v44_p2_tx; o->lapm.v44_p2_rx = c->lapm.v44_p2_rx;
    o->lapm.v44_p3_tx = c->lapm.v44_p3_tx; o->lapm.v44_p3_rx = c->lapm.v44_p3_rx;
    o->lapm.t401_ms = c->lapm.t401_ms; o->lapm.t403_ms = c->lapm.t403_ms; o->lapm.n400 = c->lapm.n400;
}

static void old_cfg_out(v90m_ec_cfg *c, const v42ec_cfg_t *ip)
{
    v42ec_cfg_t i = *ip;
    memset(c, 0, sizeof *c);
    c->originator = i.originator; c->detect = i.detect; c->fallback = i.fallback;
    c->t400_ms = i.t400_ms; c->establish_ms = i.establish_ms; c->comp_policy = i.comp_policy;
    c->lapm.use_xid = i.lapm.use_xid;
    c->lapm.k_tx = i.lapm.k_tx; c->lapm.k_rx = i.lapm.k_rx;
    c->lapm.n401_tx = i.lapm.n401_tx; c->lapm.n401_rx = i.lapm.n401_rx;
    c->lapm.fcs32 = i.lapm.fcs32;
    c->lapm.v42bis_tx = i.lapm.comp_tx; c->lapm.v42bis_rx = i.lapm.comp_rx;
    c->lapm.v42bis_p1 = i.lapm.comp_p1; c->lapm.v42bis_p2 = i.lapm.comp_p2;
    c->lapm.v44_tx = i.lapm.v44_tx; c->lapm.v44_rx = i.lapm.v44_rx;
    c->lapm.v44_p1_tx = i.lapm.v44_p1_tx; c->lapm.v44_p1_rx = i.lapm.v44_p1_rx;
    c->lapm.v44_p2_tx = i.lapm.v44_p2_tx; c->lapm.v44_p2_rx = i.lapm.v44_p2_rx;
    c->lapm.v44_p3_tx = i.lapm.v44_p3_tx; c->lapm.v44_p3_rx = i.lapm.v44_p3_rx;
    c->lapm.t401_ms = i.lapm.t401_ms; c->lapm.t403_ms = i.lapm.t403_ms; c->lapm.n400 = i.lapm.n400;
}

static void old_status_out(v90m_ec_status *st, const v42ec_status_t *sp, const v42ec_t *e)
{
    v42ec_status_t s = *sp;
    memset(st, 0, sizeof *st);
    st->state = s.state; st->reason = s.reason;
    st->k_tx = s.k_tx; st->k_rx = s.k_rx; st->n401_tx = s.n401_tx; st->n401_rx = s.n401_rx;
    st->comp_alg = s.comp_alg; st->comp_tx = s.comp_tx; st->comp_rx = s.comp_rx;
    st->enc_on = e->comp_tx; st->dec_on = e->comp_rx; st->alg_on = e->comp_alg;
    st->comp_n2 = s.comp_n2; st->comp_n7 = s.comp_n7;
    st->v44_n2_tx = s.v44_n2_tx; st->v44_n7_tx = s.v44_n7_tx; st->v44_n8_tx = s.v44_n8_tx;
    st->v44_n2_rx = s.v44_n2_rx; st->v44_n7_rx = s.v44_n7_rx; st->v44_n8_rx = s.v44_n8_rx;
    st->fcs32 = s.fcs32;
    st->tx_queued = s.tx_queued; st->rx_avail = s.rx_avail; st->unacked = s.unacked;
    st->rx_frames = s.rx_frames; st->rx_frames_bad = s.rx_frames_bad;
    st->tx_i = s.tx_i; st->tx_i_retx = s.tx_i_retx; st->rx_i = s.rx_i; st->rx_i_dropped = s.rx_i_dropped;
    st->tx_rej = s.tx_rej; st->rx_rej = s.rx_rej; st->t401_expiries = s.t401_expiries; st->violations = s.violations;
    st->comp_in = s.comp_in; st->comp_out = s.comp_out; st->decomp_in = s.decomp_in; st->decomp_out = s.decomp_out;
    st->async_overruns = s.async_overruns; st->async_framing = s.async_framing;
}

/* ---- the values tried ---- */
#define NFIELDS 27
static int *field_ptr(v90m_ec_cfg *c, int k)
{
    int n = 0;
#define FIELD_AT(pub, in, lo, hi) if (n++ == k) return &c->pub;
    V90M_EC_CFG_FIELDS(FIELD_AT)
#undef FIELD_AT
    return NULL;
}
static const char *names[] = {
#define FIELD_NAME(pub, in, lo, hi) #pub,
    V90M_EC_CFG_FIELDS(FIELD_NAME)
#undef FIELD_NAME
};
typedef char nfields_check[sizeof names / sizeof *names == NFIELDS ? 1 : -1];
static const char *field_name(int k) { return names[k]; }

static int vals[512], nvals;
static void add(int v)
{
    int i;
    for (i = 0; i < nvals; i++) if (vals[i] == v) return;
    vals[nvals++] = v;
}
static void add_around(int b)
{
    if (b > INT_MIN) add(b - 1);
    add(b);
    if (b < INT_MAX) add(b + 1);
}
/* INT_MIN, INT_MAX, around 0, and each bound of every field, one below,
 * at and one above (where the int has room) */
static void make_values(void)
{
    add(INT_MIN); add(INT_MIN + 1); add(-2); add(-1); add(0); add(1); add(2); add(INT_MAX - 1); add(INT_MAX);
#define FIELD_BOUNDS(pub, in, lo, hi) add_around(lo); add_around(hi);
    V90M_EC_CFG_FIELDS(FIELD_BOUNDS)
#undef FIELD_BOUNDS
    /* the constants the old code names, in case a list bound is not one of them */
    add(V42_MAX_K); add(V42_MAX_K + 1); add(V42_MAX_N401); add(V42_MAX_N401 + 1);
    add(V42COMP_MAX_CODEWORDS); add(V42COMP_MAX_CODEWORDS + 1); add(V44COMP_MAX_CODEWORDS); add(V44COMP_MAX_CODEWORDS + 1);
    add(V44COMP_MAX_HISTORY); add(V44COMP_MAX_HISTORY + 1); add(127); add(128); add(250); add(251); add(255); add(256);
}

static union { v90m_align a; uint8_t b[V90M_EC_MEM_SIZE]; } mem;
static int cases, accepted, differ;

/* one configuration, old against new; through the library when lib */
static void compare(const v90m_ec_cfg *c, int lib, const char *what, int k, int v)
{
    v42ec_cfg_t o1, o2;
    int r1 = old_check_ec_cfg(c), r2 = check_ec_cfg(c), bad = 0;
    cases++;
    if (r1 == V90M_OK) accepted++;
    if (r1 != r2) bad = 1;
    memset(&o1, 0xA5, sizeof o1); memset(&o2, 0x5A, sizeof o2);
    old_cfg_in(&o1, c); cfg_in(&o2, c);
    if (memcmp(&o1, &o2, sizeof o1) != 0) bad = 1;
    if (lib) {
        int err = 99;
        v90m_ec_t *e = v90m_ec_init(&mem, sizeof mem, c, &err);
        if ((e != NULL) != (r1 == V90M_OK) || err != r1) bad = 1;
        if (!e) {
            v90m_ec_cfg d;
            v90m_ec_cfg_default(&d, 0);
            e = v90m_ec_init(&mem, sizeof mem, &d, &err);
        }
        if (!e || v90m_ec_reset(e, c) != r1) bad = 1;
    }
    if (bad) {
        if (differ < 10) printf("  differ: %s, %s = %d: old %d, new %d\n", what, k >= 0 ? field_name(k) : "-", v, r1, r2);
        differ++;
    }
}

static uint32_t rng = 12345;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_cfg(void)
{
    int orig, k, i, n;
    make_values();
    CHECK(nvals < (int)(sizeof vals / sizeof *vals));
    for (orig = 0; orig <= 1; orig++) {
        v90m_ec_cfg base;
        v90m_ec_cfg_default(&base, orig);
        compare(&base, 1, "default", -1, orig);
        /* one field at a time, the others at their defaults */
        for (k = 0; k < NFIELDS; k++)
            for (i = 0; i < nvals; i++) {
                v90m_ec_cfg c = base;
                *field_ptr(&c, k) = vals[i];
                compare(&c, 1, orig ? "one field, originator" : "one field, answerer", k, vals[i]);
            }
        /* many at once: each field a default (3 in 4) or one of the values */
        for (n = 0; n < 20000; n++) {
            v90m_ec_cfg c = base;
            for (k = 0; k < NFIELDS; k++)
                if (next() % 4 == 0) *field_ptr(&c, k) = vals[next() % (uint32_t)nvals];
            compare(&c, n % 16 == 0, "many fields", -1, n);
        }
        /* every field off by one at once, the other way too */
        {
            v90m_ec_cfg c = base;
            for (k = 0; k < NFIELDS; k++) *field_ptr(&c, k) = INT_MIN;
            compare(&c, 1, "all INT_MIN", -1, 0);
            for (k = 0; k < NFIELDS; k++) *field_ptr(&c, k) = INT_MAX;
            compare(&c, 1, "all INT_MAX", -1, 0);
        }
    }
    printf("ec cfg: %d values per field, %d fields, %d configurations (%d valid), %d differ\n",
           nvals, NFIELDS, cases, accepted, differ);
    CHECK_EQ_I(differ, 0);
    CHECK(accepted > 0 && accepted < cases);
}

static void test_defaults(void)
{
    int orig;
    for (orig = -1; orig <= 2; orig++) {
        v42ec_cfg_t i;
        v90m_ec_cfg c1, c2, c3;
        v42ec_cfg_default(&i, orig);
        memset(&c1, 0xA5, sizeof c1); memset(&c2, 0x5A, sizeof c2);
        old_cfg_out(&c1, &i); cfg_out(&c2, &i);
        CHECK(memcmp(&c1, &c2, sizeof c1) == 0);
        /* the library's */
        memset(&c3, 0x33, sizeof c3);
        v90m_ec_cfg_default(&c3, orig);
        CHECK(memcmp(&c1, &c3, sizeof c1) == 0);
    }
    printf("ec defaults: the same for originator -1, 0, 1, 2\n");
}

static void test_status(void)
{
    static v42ec_t e;
    v42ec_status_t s;
    v90m_ec_status s1, s2;
    int n = 0, round;
    for (round = 0; round < 2; round++) {
        /* each listed field its own octets, the rest (one left off the
         * list) 0x77, which the old code copies and the list would not */
        memset(&s, 0x77, sizeof s);
        n = 0;
#define FIELD_SET2(f) n++; memset(&s.f, round ? 0xC0 + n : n, sizeof s.f);
        V90M_EC_STATUS_FIELDS(FIELD_SET2)
#undef FIELD_SET2
        memset(&e, 0, sizeof e);
        e.comp_tx = round ? 3 : 1; e.comp_rx = round ? 4 : 0; e.comp_alg = round ? V42EC_COMP_V44 : V42EC_COMP_V42BIS;
        memset(&s1, 0xA5, sizeof s1); memset(&s2, 0x5A, sizeof s2);
        old_status_out(&s1, &s, &e); status_out(&s2, &s, &e);
        CHECK(memcmp(&s1, &s2, sizeof s1) == 0);
    }
    printf("ec status: %d fields listed and 3 of the endpoint's, the same\n", n);
    CHECK_EQ_I(n, 37);
}

int main(void)
{
    test_cfg();
    test_defaults();
    test_status();
    T_REPORT("ec fields");
}
