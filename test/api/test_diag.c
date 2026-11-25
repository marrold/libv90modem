/* The start-up diagnostics (v90m_get_diag, v90m_err_name, v90m_phase_name,
 * v90m_conn_name), from the outside: the modems come from libv90modem.a as
 * it is installed; an API caller and an API answerer back to back (or a
 * simulated caller from the dev archive), the line cut where a start-up is
 * to fail. After the end, each side's error, phase, modulation chosen and
 * whether data mode was reached; and a name for every error code.
 *
 *   api_test_diag            the tests
 *   api_test_diag --names    every error name, one a line
 *                            (tools/check-diag-names.sh) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "v90modem.h"
#include "test.h"
/* the simulated callers (dev archive) */
#include "callpeer.h"
#include "x2call.h"
/* the families of error codes and their counts (never installed) */
#include "../../api/v90modem_diag.h"

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static void print_names(void)
{
    int f, k;
    for (f = 1; f < DG_NFAM; f++)
        for (k = 1; k <= dg_count(f); k++) printf("%s\n", v90m_err_name(V90M_DIAG_CODE(f, k)));
}

/* ---- every code has a name, and no two the same; the phase and
 * modulation names ---- */
static void test_names(void)
{
    static const char *seen[DG_NFAM * 100];
    static const char *const ph[] = { "none", "tone", "v8", "phase2", "phase3", "phase4", "train", "data" };
    static const char *const cn[] = { "none", "V.21", "V.22", "V.22bis", "V.90", "V.34", "V.32bis", "V.92",
                                      "Bell 103", "Bell 212A", "x2", "K56flex", "V.23" };
    int f, k, j, total = 0;
    CHECK(strcmp(v90m_err_name(0), "none") == 0);
    CHECK(strcmp(v90m_err_name(-1), "?") == 0);
    CHECK(strcmp(v90m_err_name(99), "?") == 0);
    CHECK(strcmp(v90m_err_name(V90M_DIAG_CODE(DG_NFAM, 1)), "?") == 0);
    CHECK(strcmp(v90m_err_name(V90M_DIAG_CODE(DG_P2D, P2D_ERR_INFO1A)), "P2D_ERR_INFO1A") == 0);
    CHECK(strcmp(v90m_err_name(V90M_DIAG_CODE(DG_V90A, V90A_ERR_RETRAINS)), "V90A_ERR_RETRAINS") == 0);
    for (f = 1; f < DG_NFAM; f++) {
        int n = dg_count(f);
        const char *first = v90m_err_name(V90M_DIAG_CODE(f, 1));
        size_t pre = strstr(first, "_ERR_") ? (size_t)(strstr(first, "_ERR_") - first) + 5 : 0;
        CHECK(n > 0 && n < 100);
        CHECK(pre > 0);
        CHECK(strcmp(v90m_err_name(V90M_DIAG_CODE(f, 0)), "?") == 0);
        CHECK(strcmp(v90m_err_name(V90M_DIAG_CODE(f, n + 1)), "?") == 0);
        for (k = 1; k <= n; k++) {
            const char *s = v90m_err_name(V90M_DIAG_CODE(f, k));
            CHECK(s != NULL);
            if (!s) continue;
            CHECK(strcmp(s, "?") != 0 && strcmp(s, "none") != 0);
            CHECK(strncmp(s, first, pre) == 0);         /* one prefix per family */
            for (j = 0; j < total; j++) CHECK(strcmp(seen[j], s) != 0);
            seen[total++] = s;
        }
    }
    printf("  names: %d codes in %d families, each named once\n", total, DG_NFAM - 1);
    for (k = 0; k < (int)(sizeof ph / sizeof ph[0]); k++) CHECK(strcmp(v90m_phase_name((v90m_phase)k), ph[k]) == 0);
    CHECK(strcmp(v90m_phase_name((v90m_phase)8), "?") == 0);
    CHECK(strcmp(v90m_phase_name((v90m_phase)-1), "?") == 0);
    for (k = 0; k < (int)(sizeof cn / sizeof cn[0]); k++) CHECK(strcmp(v90m_conn_name((v90m_conn)k), cn[k]) == 0);
    CHECK(strcmp(v90m_conn_name((v90m_conn)13), "?") == 0);
    CHECK(strcmp(v90m_conn_name((v90m_conn)-1), "?") == 0);
}

/* the rule docs/api.md gives for "start-up failed" */
static int startup_failed(const v90m_state *s, const v90m_diag *d) { return s->end != V90M_END_NONE && !d->data_reached; }

/* ---- an API caller and an API answerer back to back ---- */
typedef struct { v90m_t *m; v90m_async_t *a; } end_t;
static int end_get(void *u) { return v90m_async_getbit(((end_t *)u)->a); }
static void end_put(void *u, int b) { v90m_async_putbit(((end_t *)u)->a, b); }

static void end_init(end_t *e, v90m_role role, unsigned mods)
{
    v90m_cfg c; int err;
    memset(e, 0, sizeof *e);
    e->a = v90m_async_create(&err);
    v90m_cfg_default(&c);
    c.role = role; c.modulations = mods;
    c.getbit = end_get; c.putbit = end_put; c.user = e;
    e->m = v90m_create(&c, &err);
    CHECK(e->m != NULL);
}
static void end_free(end_t *e) { v90m_destroy(e->m); v90m_async_destroy(e->a); }

typedef struct { v90m_diag d; v90m_state s; } side_t;

static void show_side(const char *who, const side_t *x)
{
    printf("    %s end %d: %s, %s, %s, data reached %d\n", who, (int)x->s.end, v90m_err_name(x->d.error),
           v90m_phase_name(x->d.phase), v90m_conn_name(x->d.selected), x->d.data_reached);
}
static void show(const char *what, const side_t *c, const side_t *a)
{
    printf("  %s:\n", what);
    show_side("answerer", a);
    if (c) show_side("caller", c);
}

#define CUT_NEVER V90M_PHASE_NONE
/* the call up to both ends (or secs): from the first sample the answerer
 * is in phase cut (CUT_NEVER: never), it hears silence. data: once data
 * moves both ways at both ends, the diagnostics are checked against
 * want_sel (DATA_CHECK), then the answerer clears down (DATA_CLEAR), or
 * asks for a retrain and from then on hears silence (DATA_RETRAIN_CUT). */
enum { DATA_NONE, DATA_CHECK, DATA_CLEAR, DATA_RETRAIN_CUT };
static void pair(unsigned c_mods, unsigned a_mods, v90m_phase cut, int data, v90m_conn want_sel, int secs,
                 side_t *c, side_t *a)
{
    static end_t cl, an;
    int16_t c_rx = 0, a_rx = 0;
    long k, cut_at = -1;
    int seen = 0;
    end_init(&cl, V90M_ROLE_ORIGINATE, c_mods);
    end_init(&an, V90M_ROLE_ANSWER, a_mods);
    memset(c, 0, sizeof *c); memset(a, 0, sizeof *a);
    if (cl.m && an.m) {
        for (k = 0; k < 8000L * secs; k++) {
            int16_t c_tx, a_tx, in = cut_at >= 0 ? 0 : a_rx;
            v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
            v90m_process_linear(an.m, &in, &a_tx, 1);
            a_rx = c_tx; c_rx = a_tx;
            v90m_get_diag(an.m, &a->d);
            if (cut_at < 0 && cut != CUT_NEVER && a->d.phase == cut) cut_at = k;
            if (data != DATA_NONE && !seen) {
                v90m_get_state(an.m, &a->s); v90m_get_state(cl.m, &c->s);
                if (a->s.moving && c->s.moving) {
                    v90m_get_diag(cl.m, &c->d);
                    printf("    in data mode: answerer %s %s, error %s, data reached %d; caller %s %s, error %s, data reached %d\n",
                           v90m_phase_name(a->d.phase), v90m_conn_name(a->d.selected), v90m_err_name(a->d.error),
                           a->d.data_reached, v90m_phase_name(c->d.phase), v90m_conn_name(c->d.selected),
                           v90m_err_name(c->d.error), c->d.data_reached);
                    CHECK_EQ_I(a->d.phase, V90M_PHASE_DATA); CHECK_EQ_I(a->d.selected, want_sel);
                    CHECK_EQ_I(a->d.error, 0); CHECK_EQ_I(a->d.data_reached, 1);
                    CHECK_EQ_I(c->d.phase, V90M_PHASE_DATA); CHECK_EQ_I(c->d.selected, want_sel);
                    CHECK_EQ_I(c->d.error, 0); CHECK_EQ_I(c->d.data_reached, 1);
                    if (data == DATA_CLEAR) CHECK_EQ_I(v90m_cleardown(an.m), V90M_OK);
                    if (data == DATA_RETRAIN_CUT) { CHECK_EQ_I(v90m_retrain(an.m), V90M_OK); cut_at = k; }
                    seen = 1;
                }
            }
            if (k % 80 == 0) {
                v90m_get_state(an.m, &a->s); v90m_get_state(cl.m, &c->s);
                if (a->s.end != V90M_END_NONE && c->s.end != V90M_END_NONE) break;
            }
        }
        v90m_get_diag(an.m, &a->d); v90m_get_state(an.m, &a->s);
        v90m_get_diag(cl.m, &c->d); v90m_get_state(cl.m, &c->s);
    }
    end_free(&cl); end_free(&an);
}

#define V90_MODS (V90M_MOD_V90 | V90M_MOD_V34)
#define V92_MODS (V90M_MOD_V92 | V90M_MOD_V90 | V90M_MOD_V34)

/* a failed start-up, as the answerer saw it: the rule, the end, the phase,
 * the modulation chosen and the cause's family */
static void check_failed(const side_t *x, v90m_end end, v90m_phase phase, v90m_conn sel, const char *prefix)
{
    CHECK_EQ_I(x->s.end, end);
    CHECK(startup_failed(&x->s, &x->d));
    CHECK_EQ_I(x->d.data_reached, 0);
    CHECK(starts(v90m_err_name(x->d.error), prefix));
    CHECK_EQ_I(x->d.phase, phase); CHECK_EQ_I(x->d.selected, sel);
}

static void test_failures(void)
{
    side_t c, a;

    /* V.90: the caller goes silent once the answerer's Phase 2 begins */
    pair(V90_MODS, V90_MODS, V90M_PHASE_PHASE2, DATA_NONE, V90M_CONN_NONE, 60, &c, &a);
    show("V.90, the answerer hears nothing from its Phase 2 on", &c, &a);
    check_failed(&a, V90M_END_FAILED, V90M_PHASE_PHASE2, V90M_CONN_V90, "P2D_ERR_");
    CHECK(startup_failed(&c.s, &c.d));
    CHECK(c.d.error != 0 && strcmp(v90m_err_name(c.d.error), "?") != 0);
    CHECK_EQ_I(c.d.phase, V90M_PHASE_PHASE2); CHECK_EQ_I(c.d.selected, V90M_CONN_V90);

    /* ... from its Phase 3 on */
    pair(V90_MODS, V90_MODS, V90M_PHASE_PHASE3, DATA_NONE, V90M_CONN_NONE, 60, &c, &a);
    show("V.90, the answerer hears nothing from its Phase 3 on", &c, &a);
    check_failed(&a, V90M_END_FAILED, V90M_PHASE_PHASE3, V90M_CONN_V90, "P34_ERR_");
    CHECK(startup_failed(&c.s, &c.d));
    CHECK(starts(v90m_err_name(c.d.error), "V90A_ERR_"));
    CHECK(c.d.phase == V90M_PHASE_PHASE3 || c.d.phase == V90M_PHASE_PHASE4); CHECK_EQ_I(c.d.selected, V90M_CONN_V90);

    /* V.92 (the ISP's calls): PCM upstream agreed in Phase 2, then the
     * answerer hears nothing from its Phase 3 on. V.92's failed start-up
     * is a retrain (9.5.1.2), whose Phase 2 then fails: the phase reached
     * stays Phase 3, the error is the retrain's Phase 2's, V.92 chosen */
    pair(V92_MODS, V92_MODS, V90M_PHASE_PHASE3, DATA_NONE, V90M_CONN_NONE, 90, &c, &a);
    show("V.92, the answerer hears nothing from its Phase 3 on (a retrain, which fails)", &c, &a);
    check_failed(&a, V90M_END_FAILED, V90M_PHASE_PHASE3, V90M_CONN_V92, "P2D_ERR_");
    CHECK(startup_failed(&c.s, &c.d));
    CHECK(c.d.error != 0 && strcmp(v90m_err_name(c.d.error), "?") != 0);
    CHECK(c.d.selected == V90M_CONN_V92 || c.d.selected == V90M_CONN_V90);
    CHECK_EQ_I(c.d.phase, V90M_PHASE_PHASE3); CHECK_EQ_I(c.d.data_reached, 0);

    /* V.34 chosen in V.8, silence from the answerer's Phase 3 on: the
     * failed training is a retrain (V.34 11.5), whose Phase 2 fails */
    pair(V90M_MOD_V34, V90M_MOD_V34, V90M_PHASE_PHASE3, DATA_NONE, V90M_CONN_NONE, 60, &c, &a);
    show("V.34, the answerer hears nothing from its Phase 3 on (a retrain, which fails)", &c, &a);
    check_failed(&a, V90M_END_FAILED, V90M_PHASE_PHASE3, V90M_CONN_V34, "P2N_ERR_");
    CHECK(startup_failed(&c.s, &c.d));
    CHECK(c.d.error != 0 && strcmp(v90m_err_name(c.d.error), "?") != 0);
    CHECK_EQ_I(c.d.selected, V90M_CONN_V34);
    CHECK_EQ_I(c.d.phase, V90M_PHASE_PHASE3); CHECK_EQ_I(c.d.data_reached, 0);

    /* V.32bis chosen in V.8, and the caller never answers our AC */
    pair(V90M_MOD_V32BIS, V90M_MOD_V32BIS | V90M_MOD_V22, V90M_PHASE_TRAIN, DATA_NONE, V90M_CONN_NONE, 60, &c, &a);
    show("V.32bis, the caller silent after V.8", &c, &a);
    check_failed(&a, V90M_END_FAILED, V90M_PHASE_TRAIN, V90M_CONN_V32BIS, "V32_ERR_NO_TONE");
    CHECK(starts(v90m_err_name(c.d.error), "V32_ERR_"));
    CHECK_EQ_I(c.d.phase, V90M_PHASE_TRAIN); CHECK_EQ_I(c.d.selected, V90M_CONN_V32BIS);

    /* V.8 with nothing in common */
    pair(V90M_MOD_V21, V90M_MOD_V22, CUT_NEVER, DATA_NONE, V90M_CONN_NONE, 30, &c, &a);
    show("V.8, nothing in common", &c, &a);
    check_failed(&a, V90M_END_NO_COMMON, V90M_PHASE_V8, V90M_CONN_NONE, "V8_ERR_NO_COMMON");
    check_failed(&c, V90M_END_NO_COMMON, V90M_PHASE_V8, V90M_CONN_NONE, "V8_ERR_NO_COMMON");
}

/* a V.22bis caller without V.8 (sim/callpeer): chosen when it is heard,
 * then it goes silent */
static void test_no_v8(void)
{
    static callpeer_t p;
    side_t a;
    v90m_cfg c; v90m_t *m; int err;
    int16_t line = 0, rx, tx;
    long k, cut_at = -1;
    v90m_cfg_default(&c);
    c.modulations = V90M_MOD_V22;
    c.getbit = end_get; c.putbit = end_put;
    memset(&a, 0, sizeof a);
    {
        static end_t e;
        memset(&e, 0, sizeof e);
        e.a = v90m_async_create(&err);
        c.user = &e;
        m = v90m_create(&c, &err);
        CHECK(m != NULL);
        if (!m) return;
        callpeer_init(&p, 0, 0, 1, 3000);
        for (k = 0; k < 8000L * 60; k++) {
            rx = callpeer_tick(&p, line);
            if (cut_at >= 0) rx = 0;
            v90m_process_linear(m, &rx, &tx, 1);
            line = tx;
            v90m_get_diag(m, &a.d);
            if (cut_at < 0 && a.d.selected != V90M_CONN_NONE) cut_at = k;
            if (k % 80 == 0) { v90m_get_state(m, &a.s); if (a.s.end != V90M_END_NONE) break; }
        }
        v90m_get_state(m, &a.s); v90m_get_diag(m, &a.d);
        v90m_destroy(m); v90m_async_destroy(e.a);
    }
    printf("  a V.22bis caller without V.8, silent once heard: chosen after %ld ms\n", cut_at < 0 ? -1L : cut_at / 8);
    show("  ... its end", NULL, &a);
    CHECK(cut_at >= 0);
    check_failed(&a, V90M_END_FAILED, V90M_PHASE_TRAIN, V90M_CONN_V22BIS, "V22_ERR_");
}

/* x2: the simulated client (sim/x2call); the library hears silence from
 * its Phase 4 on */
static int ones_get(void *u) { (void)u; return 1; }
static void bits_put(void *u, int b) { (void)u; (void)b; }
static void test_x2(void)
{
    static x2call_t x;
    static uint8_t down[64];
    x2call_cfg xc; v90m_cfg c;
    v90m_t *m; int err;
    side_t a;
    long n, cut = -1;
    uint8_t rx = 0xff, tx;

    v90m_cfg_default(&c);
    c.modulations |= V90M_MOD_X2;
    c.getbit = ones_get; c.putbit = bits_put;
    memset(&a, 0, sizeof a);
    m = v90m_create(&c, &err);
    CHECK(m != NULL);
    if (!m) return;
    memset(&xc, 0, sizeof xc);
    xc.level = 3000; xc.max_index = 1; xc.up_mask = 0x03FE;
    xc.getbit = ones_get; xc.putbit = bits_put;
    x2call_init(&x, &xc);
    memset(down, 0x7f, sizeof down);
    for (n = 0; n < 8000L * 60; n++) {
        int16_t s;
        uint8_t in = cut >= 0 ? v90m_g711_encode(0, 0) : rx;
        v90m_process_g711(m, &in, &tx, 1);
        down[n & 63] = tx;
        s = x2call_tick(&x, down[(n - 30) & 63]);
        rx = v90m_g711_encode(0, (int16_t)(s / 2));
        v90m_get_diag(m, &a.d);
        if (cut < 0 && a.d.phase == V90M_PHASE_PHASE4) cut = n;
        if (n % 80 == 0) { v90m_get_state(m, &a.s); if (a.s.end != V90M_END_NONE) break; }
    }
    v90m_get_state(m, &a.s); v90m_get_diag(m, &a.d);
    v90m_destroy(m);
    show("x2, the server hears nothing from its Phase 4 on", NULL, &a);
    check_failed(&a, V90M_END_X2_FAILED, V90M_PHASE_PHASE4, V90M_CONN_X2, "X2D_ERR_");
}

/* calling into silence: no answer tone */
static void test_no_tone(void)
{
    end_t cl;
    side_t c;
    int16_t rx = 0, tx;
    long k;
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V32BIS);
    if (!cl.m) return;
    for (k = 0; k < 8000L * 60; k++) {
        v90m_process_linear(cl.m, &rx, &tx, 1);
        if (k % 80 == 0) { v90m_get_state(cl.m, &c.s); if (c.s.end != V90M_END_NONE) break; }
    }
    v90m_get_state(cl.m, &c.s); v90m_get_diag(cl.m, &c.d);
    printf("  V.32bis caller, no answer:\n");
    show_side("caller", &c);
    check_failed(&c, V90M_END_NO_ANSWER_TONE, V90M_PHASE_TONE, V90M_CONN_NONE, "V8_ERR_NO_TONE");
    end_free(&cl);
}

/* connected: no error, data mode, the modulation; then how the call ended */
static void test_connected(void)
{
    side_t c, a;

    /* an orderly cleardown is no error */
    pair(V90_MODS, V90_MODS, CUT_NEVER, DATA_CLEAR, V90M_CONN_V90, 60, &c, &a);
    show("V.90 to data mode, then our cleardown", &c, &a);
    CHECK_EQ_I(a.s.end, V90M_END_CLEARDOWN_OURS); CHECK_EQ_I(c.s.end, V90M_END_CLEARDOWN_FAR);
    CHECK_EQ_I(a.d.error, 0); CHECK_EQ_I(c.d.error, 0);
    CHECK_EQ_I(a.d.data_reached, 1); CHECK_EQ_I(c.d.data_reached, 1);
    CHECK(!startup_failed(&a.s, &a.d)); CHECK(!startup_failed(&c.s, &c.d));
    CHECK_EQ_I(a.d.phase, V90M_PHASE_DATA); CHECK_EQ_I(a.d.selected, V90M_CONN_V90);
    CHECK_EQ_I(c.d.phase, V90M_PHASE_DATA); CHECK_EQ_I(c.d.selected, V90M_CONN_V90);

    /* a retrain after data mode into a line gone silent: lost, not a
     * failed start-up */
    pair(V90_MODS, V90_MODS, CUT_NEVER, DATA_RETRAIN_CUT, V90M_CONN_V90, 90, &c, &a);
    show("V.90 to data mode, then our retrain into silence", &c, &a);
    CHECK_EQ_I(a.s.end, V90M_END_RETRAIN_FAILED);
    CHECK_EQ_I(a.d.data_reached, 1);
    CHECK(!startup_failed(&a.s, &a.d));
    CHECK_EQ_I(a.d.phase, V90M_PHASE_DATA);              /* the furthest reached */
    CHECK(a.d.error != 0 && strcmp(v90m_err_name(a.d.error), "?") != 0);
    CHECK_EQ_I(a.d.selected, V90M_CONN_V90);
    CHECK_EQ_I(c.s.end, V90M_END_RETRAIN_FAILED);        /* the caller's side of it */
    CHECK_EQ_I(c.d.data_reached, 1);
    CHECK(!startup_failed(&c.s, &c.d));
    CHECK_EQ_I(c.d.phase, V90M_PHASE_DATA);
    CHECK(starts(v90m_err_name(c.d.error), "V90A_ERR_"));
    CHECK_EQ_I(c.d.selected, V90M_CONN_V90);

    pair(V90M_MOD_V22, V90M_MOD_V22, CUT_NEVER, DATA_CHECK, V90M_CONN_V22BIS, 10, &c, &a);
    show("V.22bis to data mode (no cleardown signal: the call goes on)", &c, &a);
    CHECK_EQ_I(a.s.end, V90M_END_NONE); CHECK_EQ_I(c.s.end, V90M_END_NONE);
    CHECK_EQ_I(a.d.error, 0); CHECK_EQ_I(c.d.error, 0);
    CHECK_EQ_I(a.d.data_reached, 1); CHECK_EQ_I(c.d.data_reached, 1);
    CHECK_EQ_I(a.d.phase, V90M_PHASE_DATA); CHECK_EQ_I(a.d.selected, V90M_CONN_V22BIS);
    CHECK_EQ_I(c.d.phase, V90M_PHASE_DATA); CHECK_EQ_I(c.d.selected, V90M_CONN_V22BIS);
}

/* NULL, and v90m_reset clears them */
static void test_reset_null(void)
{
    end_t cl;
    v90m_cfg cfg;
    v90m_diag d;
    int16_t rx = 0, tx;
    long k;
    memset(&d, 0x5a, sizeof d);
    v90m_get_diag(NULL, &d);
    CHECK_EQ_I(d.error, 0x5a5a5a5a);                     /* untouched */
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V22);
    if (!cl.m) return;
    v90m_get_diag(cl.m, NULL);
    v90m_get_diag(cl.m, &d);
    CHECK_EQ_I(d.error, 0); CHECK_EQ_I(d.phase, V90M_PHASE_NONE); CHECK_EQ_I(d.selected, V90M_CONN_NONE);
    CHECK_EQ_I(d.data_reached, 0);
    for (k = 0; k < 8000L * 60; k++) {
        v90m_process_linear(cl.m, &rx, &tx, 1);
        v90m_get_diag(cl.m, &d);
        if (d.error) break;
    }
    CHECK(strcmp(v90m_err_name(d.error), "V8_ERR_NO_TONE") == 0);
    v90m_cfg_default(&cfg); cfg.role = V90M_ROLE_ORIGINATE; cfg.modulations = V90M_MOD_V22;
    CHECK_EQ_I(v90m_reset(cl.m, &cfg), V90M_OK);
    v90m_get_diag(cl.m, &d);
    CHECK_EQ_I(d.error, 0); CHECK_EQ_I(d.phase, V90M_PHASE_NONE); CHECK_EQ_I(d.selected, V90M_CONN_NONE);
    CHECK_EQ_I(d.data_reached, 0);
    end_free(&cl);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--names") == 0) { print_names(); return 0; }
    test_names();
    test_failures();
    test_no_v8();
    test_x2();
    test_no_tone();
    test_connected();
    test_reset_null();
    T_REPORT("diag");
}
