/* libv90modem: the start-up diagnostics (v90m_get_diag, v90m_err_name;
 * api/v90modem_diag.h). Reads the modem, never changes it: the phase and
 * the modulation chosen after each sample, the cause of the end from the
 * engine that ran last, the deepest that has a number. */
#pragma GCC visibility push(default)
#include "../include/v90modem.h"
#pragma GCC visibility pop
#include "v90modem_diag.h"

#define C(fam, n) V90M_DIAG_CODE(fam, n)

/* ------------------------------------------------------------ the names */
#define N(e) [e] = #e
static const char *const v8_names[] = { N(V8_ERR_NO_TONE), N(V8_ERR_NO_JM), N(V8_ERR_NO_COMMON), N(V8_ERR_NO_CM) };
static const char *const v21_names[] = { N(V21_ERR_NO_CARRIER), N(V21_ERR_CARRIER) };
static const char *const v22_names[] = { N(V22_ERR_NO_ANSWER), N(V22_ERR_HANDSHAKE), N(V22_ERR_RETRAIN), N(V22_ERR_CARRIER) };
static const char *const v32_names[] = { N(V32_ERR_NO_TONE), N(V32_ERR_TRAIN), N(V32_ERR_NO_RATE), N(V32_ERR_CARRIER) };
static const char *const p2d_names[] = {
    N(P2D_ERR_INFO0), N(P2D_ERR_REV), N(P2D_ERR_TONE_A), N(P2D_ERR_INFO1A), N(P2D_ERR_PCM_UP), N(P2D_ERR_TIME)
};
static const char *const p34_names[] = {
    N(P34_ERR_NO_S), N(P34_ERR_JA), N(P34_ERR_UINFO), N(P34_ERR_NO_S2), N(P34_ERR_DIL_END), N(P34_ERR_CPT),
    N(P34_ERR_CP), N(P34_ERR_RATE), N(P34_ERR_B1), N(P34_ERR_CLEARDOWN), N(P34_ERR_CARRIER), N(P34_ERR_HANGUP)
};
static const char *const v92d_names[] = {
    N(V92D_ERR_NO_RU), N(V92D_ERR_JA), N(V92D_ERR_UINFO), N(V92D_ERR_NO_SU), N(V92D_ERR_CPT), N(V92D_ERR_DESIGN),
    N(V92D_ERR_CPU), N(V92D_ERR_B1), N(V92D_ERR_CLEARDOWN), N(V92D_ERR_HANGUP), N(V92D_ERR_CARRIER)
};
static const char *const p2n_names[] = {
    N(P2N_ERR_INFO0), N(P2N_ERR_REV), N(P2N_ERR_INFO1C), N(P2N_ERR_NO_RATE), N(P2N_ERR_TIME)
};
static const char *const dx_names[] = {
    N(DX_ERR_CFG), N(DX_ERR_NO_S), N(DX_ERR_RATE), N(DX_ERR_B1), N(DX_ERR_CLEARDOWN), N(DX_ERR_CARRIER), N(DX_ERR_HANGUP)
};
static const char *const v34a_names[] = {
    N(V34A_ERR_PHASE2), N(V34A_ERR_TRAINING), N(V34A_ERR_RETRAINS), N(V34A_ERR_CLEARDOWN), N(V34A_ERR_CARRIER),
    N(V34A_ERR_HANGUP)
};
static const char *const x2d_names[] = {
    N(X2D_ERR_CFG), N(X2D_ERR_NO_J), N(X2D_ERR_NO_S4), N(X2D_ERR_NO_MP), N(X2D_ERR_RATE), N(X2D_ERR_PAD),
    N(X2D_ERR_UP), N(X2D_ERR_NO_E), N(X2D_ERR_B1), N(X2D_ERR_CARRIER), N(X2D_ERR_RENEG), N(X2D_ERR_HANGUP),
    N(X2D_ERR_RETRAIN)
};
static const char *const k56d_names[] = {
    N(K56D_ERR_CFG), N(K56D_ERR_NO_S), N(K56D_ERR_NO_J), N(K56D_ERR_J), N(K56D_ERR_NO_MP), N(K56D_ERR_RATE),
    N(K56D_ERR_NO_E), N(K56D_ERR_B1), N(K56D_ERR_CARRIER), N(K56D_ERR_RENEG), N(K56D_ERR_CLEARDOWN),
    N(K56D_ERR_HANGUP)
};
static const char *const p2l_names[] = { N(P2L_ERR_INFO0), N(P2L_ERR_REV), N(P2L_ERR_INFO1A), N(P2L_ERR_NO_RATE) };
static const char *const v34o_names[] = {
    N(V34O_ERR_PHASE2), N(V34O_ERR_TRAINING), N(V34O_ERR_RETRAINS), N(V34O_ERR_CLEARDOWN), N(V34O_ERR_CARRIER),
    N(V34O_ERR_HANGUP)
};
static const char *const v90a_names[] = {
    N(V90A_ERR_PHASE2), N(V90A_ERR_SD), N(V90A_ERR_TRAIN), N(V90A_ERR_JD), N(V90A_ERR_JDP), N(V90A_ERR_DIL),
    N(V90A_ERR_RI), N(V90A_ERR_PHASE4), N(V90A_ERR_B1D), N(V90A_ERR_RATE), N(V90A_ERR_CLEARDOWN),
    N(V90A_ERR_HANGUP), N(V90A_ERR_CARRIER), N(V90A_ERR_JP), N(V90A_ERR_CPD), N(V90A_ERR_RETRAINS)
};
static const char *const ans_names[] = { N(ANS_ERR_TIMEOUT), N(ANS_ERR_RETRAINS), N(ANS_ERR_START), N(ANS_ERR_K56_RETRAIN) };
static const char *const orig_names[] = { N(ORIG_ERR_TIMEOUT) };
#undef N

#define FAM(t) { t, sizeof t / sizeof t[0] }
static const struct { const char *const *name; unsigned n; } fam[DG_NFAM] = {
    [DG_V8] = FAM(v8_names),       [DG_V21] = FAM(v21_names),
    [DG_V22] = FAM(v22_names),    [DG_V32] = FAM(v32_names),
    [DG_P2D] = FAM(p2d_names),    [DG_P34] = FAM(p34_names),
    [DG_V92D] = FAM(v92d_names), [DG_P2N] = FAM(p2n_names),
    [DG_DX] = FAM(dx_names),       [DG_V34A] = FAM(v34a_names),
    [DG_X2D] = FAM(x2d_names),    [DG_K56D] = FAM(k56d_names),
    [DG_P2L] = FAM(p2l_names),    [DG_V34O] = FAM(v34o_names),
    [DG_V90A] = FAM(v90a_names), [DG_ANS] = FAM(ans_names),
    [DG_ORIG] = FAM(orig_names)
};
#undef FAM
/* each table runs from 1 to its family's count (index 0 unused): one
 * typedef per family, so that a count and its table cannot drift apart */
#define ROWS(t, count) typedef char dg_rows_##t[sizeof t / sizeof t[0] == (count) + 1 ? 1 : -1];
ROWS(v8_names, DG_COUNT_V8) ROWS(v21_names, DG_COUNT_V21) ROWS(v22_names, DG_COUNT_V22)
ROWS(v32_names, DG_COUNT_V32) ROWS(p2d_names, DG_COUNT_P2D) ROWS(p34_names, DG_COUNT_P34)
ROWS(v92d_names, DG_COUNT_V92D) ROWS(p2n_names, DG_COUNT_P2N) ROWS(dx_names, DG_COUNT_DX)
ROWS(v34a_names, DG_COUNT_V34A) ROWS(x2d_names, DG_COUNT_X2D) ROWS(k56d_names, DG_COUNT_K56D)
ROWS(p2l_names, DG_COUNT_P2L) ROWS(v34o_names, DG_COUNT_V34O) ROWS(v90a_names, DG_COUNT_V90A)
ROWS(ans_names, DG_COUNT_ANS) ROWS(orig_names, DG_COUNT_ORIG)
#undef ROWS
/* a family's numbers stay below the next family's base */
typedef char dg_fits[DG_COUNT_V8 < 100 && DG_COUNT_V21 < 100 && DG_COUNT_V22 < 100 && DG_COUNT_V32 < 100
                     && DG_COUNT_P2D < 100 && DG_COUNT_P34 < 100 && DG_COUNT_V92D < 100 && DG_COUNT_P2N < 100
                     && DG_COUNT_DX < 100 && DG_COUNT_V34A < 100 && DG_COUNT_X2D < 100 && DG_COUNT_K56D < 100
                     && DG_COUNT_P2L < 100 && DG_COUNT_V34O < 100 && DG_COUNT_V90A < 100 && DG_COUNT_ANS < 100
                     && DG_COUNT_ORIG < 100 ? 1 : -1];

const char *v90m_err_name(int code)
{
    int f, n;
    if (code == 0) return "none";
    if (code < 0) return "?";
    f = code / 100; n = code % 100;
    if (f >= DG_NFAM || n == 0 || (unsigned)n >= fam[f].n || !fam[f].name[n]) return "?";
    return fam[f].name[n];
}

static const char *const phase_names[] = { "none", "tone", "v8", "phase2", "phase3", "phase4", "train", "data" };
static const char *const conn_names[] = { "none", "V.21", "V.22", "V.22bis", "V.90", "V.34", "V.32bis", "V.92",
                                          "Bell 103", "Bell 212A", "x2", "K56flex", "V.23" };
/* one name per value of each enum: a table's size is its enum's last value + 1 */
typedef char dg_phase_rows[sizeof phase_names / sizeof phase_names[0] == (unsigned)DG_LAST_PHASE + 1 ? 1 : -1];
typedef char dg_conn_rows[sizeof conn_names / sizeof conn_names[0] == (unsigned)DG_LAST_CONN + 1 ? 1 : -1];

const char *v90m_phase_name(v90m_phase p)
{
    return (unsigned)p < sizeof phase_names / sizeof phase_names[0] ? phase_names[p] : "?";
}

const char *v90m_conn_name(v90m_conn c)
{
    return (unsigned)c < sizeof conn_names / sizeof conn_names[0] ? conn_names[c] : "?";
}

/* ------------------------------------------- the phase and the choice */
/* V.8's choice (JM), as the modems go on with it (answer.c tick_v8,
 * orig.c ORIG_V8): V90M_CONN_NONE where nothing follows. The V.22 item is
 * V.22 and V.22bis: V.22bis until the connection says which. */
static uint8_t v8_choice(int sel, unsigned modes)
{
    if (sel == V8_SEL_V90) return (modes & ANS_MODE_V90) ? V90M_CONN_V90 : V90M_CONN_NONE;
    switch (sel) {
    case 1:  return (modes & V8_MOD_V34) ? V90M_CONN_V34 : V90M_CONN_NONE;
    case 3:  return (modes & V8_MOD_V32) ? V90M_CONN_V32BIS : V90M_CONN_NONE;
    case 4:  return (modes & V8_MOD_V22) ? V90M_CONN_V22BIS : V90M_CONN_NONE;
    case 12: return (modes & V8_MOD_V21) ? V90M_CONN_V21 : V90M_CONN_NONE;
    default: return V90M_CONN_NONE;
    }
}

/* V.34's Phases 3 and 4 and data mode, either role */
static uint8_t dx_phase(const v34dx_t *dx)
{
    if (dx->state <= DX_P3_RX) return V90M_PHASE_PHASE3;
    return dx->state <= DX_P4_E ? V90M_PHASE_PHASE4 : V90M_PHASE_DATA;
}
static uint8_t legacy_phase(int connected) { return connected ? V90M_PHASE_DATA : V90M_PHASE_TRAIN; }

void diag_track_answer(diag_track_t *t, const answer_t *a)
{
    uint8_t ph = t->phase, sel = t->sel;
    switch (a->phase) {
    case ANS_V8:
        ph = a->v8.state <= V8A_ANSAM ? V90M_PHASE_TONE : V90M_PHASE_V8;
        if (a->v8.selected) sel = v8_choice(a->v8.selected, a->cfg.modes);
        break;
    case ANS_V8BIS:
        ph = V90M_PHASE_TONE;
        break;
    case ANS_V22:
        /* a caller without V.8: chosen once it is heard (its signal, or
         * its reply to the Bell answer tone) */
        ph = legacy_phase(a->connected);
        if (a->connected) sel = (uint8_t)a->modulation;
        else if (a->via_bell) sel = V90M_CONN_BELL212A;
        else if (a->used_v8 || a->v22.state != V22S_WAIT) sel = V90M_CONN_V22BIS;
        break;
    case ANS_V21:
        /* V.21 after a silent V.22 wait is a guess; Bell 103 is heard */
        ph = legacy_phase(a->connected);
        if (a->connected || a->used_v8 || a->fsk_mod == ANS_MOD_BELL103) sel = (uint8_t)a->fsk_mod;
        break;
    case ANS_V23:
        ph = legacy_phase(a->connected); sel = V90M_CONN_V23;
        break;
    case ANS_V32_SIL: case ANS_V32:
        ph = legacy_phase(a->connected); sel = V90M_CONN_V32BIS;
        break;
    case ANS_P2: case ANS_RETRAIN:
        ph = V90M_PHASE_PHASE2;
        if (sel != V90M_CONN_V90 && sel != V90M_CONN_V92) sel = V90M_CONN_V90;
        break;
    case ANS_P3:
        ph = a->p34.state < P34_RI ? V90M_PHASE_PHASE3 : V90M_PHASE_PHASE4; sel = V90M_CONN_V90;
        break;
    case ANS_V90:
        ph = V90M_PHASE_DATA; sel = V90M_CONN_V90;
        break;
    case ANS_P3X:
        ph = a->u.v92.state < V92D_PH4 ? V90M_PHASE_PHASE3 : V90M_PHASE_PHASE4; sel = V90M_CONN_V92;
        break;
    case ANS_V92:
        ph = V90M_PHASE_DATA; sel = V90M_CONN_V92;
        break;
    case ANS_V34:
        if (a->v34.state == V34A_TRAINING) ph = dx_phase(&a->v34.dx);
        else if (a->v34.state == V34A_DATA || a->v34.state == V34A_RENEG) ph = V90M_PHASE_DATA;
        else ph = V90M_PHASE_PHASE2;
        sel = V90M_CONN_V34;
        break;
    case ANS_X2:
        ph = a->u.x2.state == X2D_WAIT_J ? V90M_PHASE_PHASE3 : a->u.x2.state < X2D_DATA ? V90M_PHASE_PHASE4 : V90M_PHASE_DATA;
        sel = V90M_CONN_X2;
        break;
    case ANS_K56:
        ph = a->u.k56.state <= K56D_WAIT_J ? V90M_PHASE_PHASE3 : a->u.k56.state < K56D_DATA ? V90M_PHASE_PHASE4
           : a->u.k56.state == K56D_RETRAIN ? V90M_PHASE_PHASE2 : V90M_PHASE_DATA;
        sel = V90M_CONN_K56FLEX;
        break;
    case ANS_K56RT:
        ph = V90M_PHASE_PHASE2; sel = V90M_CONN_K56FLEX;
        break;
    default:
        break;
    }
    t->iphase = (uint8_t)a->phase; t->sel = sel;
    if (ph > t->phase) t->phase = ph;           /* the furthest reached */
}

void diag_track_orig(diag_track_t *t, const orig_t *o)
{
    uint8_t ph = t->phase, sel = t->sel;
    switch (o->phase) {
    case ORIG_V8:
        ph = o->v8.state == V8O_LISTEN ? V90M_PHASE_TONE : V90M_PHASE_V8;
        if (o->v8.selected) sel = v8_choice(o->v8.selected, o->cfg.modes);
        break;
    case ORIG_LEGACY: case ORIG_BELL:
        ph = V90M_PHASE_TRAIN;
        break;
    case ORIG_V22:
        ph = legacy_phase(o->connected);
        sel = o->connected ? (uint8_t)o->modulation : o->via_bell ? V90M_CONN_BELL212A : V90M_CONN_V22BIS;
        break;
    case ORIG_V21:
        ph = legacy_phase(o->connected); sel = (uint8_t)o->fsk_mod;
        break;
    case ORIG_V23:
        ph = legacy_phase(o->connected); sel = V90M_CONN_V23;
        break;
    case ORIG_V32:
        ph = legacy_phase(o->connected); sel = V90M_CONN_V32BIS;
        break;
    case ORIG_V34:
        if (o->e.v34.state == V34O_TRAINING) ph = dx_phase(&o->e.v34.dx);
        else if (o->e.v34.state == V34O_DATA || o->e.v34.state == V34O_RENEG) ph = V90M_PHASE_DATA;
        else ph = V90M_PHASE_PHASE2;
        sel = V90M_CONN_V34;
        break;
    case ORIG_V90:
        switch (o->e.v90.state) {
        case V90A_P3: ph = V90M_PHASE_PHASE3; break;
        case V90A_P4: ph = V90M_PHASE_PHASE4; break;
        case V90A_DATA: case V90A_RENEG: ph = V90M_PHASE_DATA; break;
        default: ph = V90M_PHASE_PHASE2; break;
        }
        sel = o->e.v90.pcm ? V90M_CONN_V92 : V90M_CONN_V90;
        break;
    default:
        break;
    }
    t->iphase = (uint8_t)o->phase; t->sel = sel;
    if (ph > t->phase) t->phase = ph;           /* the furthest reached */
}

/* ------------------------------------------------------ why it ended */
static int v22_err(int lost, int engine_failed)
{
    if (lost) return C(DG_V22, engine_failed ? V22_ERR_RETRAIN : V22_ERR_CARRIER);
    return C(DG_V22, engine_failed ? V22_ERR_HANDSHAKE : V22_ERR_NO_ANSWER);
}

/* V.34 answering: Phase 2's or Phases 3 and 4's own cause where there is one */
static int v34a_err(const v34ans_t *v)
{
    if (v->error == V34A_ERR_PHASE2 && v->p2.error) return C(DG_P2N, v->p2.error);
    if (v->error == V34A_ERR_TRAINING && v->dx.error) return C(DG_DX, v->dx.error);
    return v->error ? C(DG_V34A, v->error) : 0;
}

/* K56flex did not start after our flagged INFO1a (ans_start_k56): the
 * engine is read only when it was started (a symbol rate of 3200) */
static int k56_start_err(const answer_t *a)
{
    if (a->v34.p2.sym_rate == 3200 && a->u.k56.error) return C(DG_K56D, a->u.k56.error);
    return C(DG_ANS, ANS_ERR_START);
}

/* Phase 2 ended and the next engine did not start (ans_v90.c: start_p34,
 * start_v92, start_v34_call): its own cause where it gave one. V.92's
 * engine shares its storage, and is not read. */
static int after_p2_err(const answer_t *a)
{
    int e;
    if (a->p2.mode == P2D_MODE_V92) return C(DG_ANS, ANS_ERR_START);
    if (a->p2.info1a.v90) return a->p34.error ? C(DG_P34, a->p34.error) : C(DG_ANS, ANS_ERR_START);
    e = v34a_err(&a->v34);
    return e ? e : C(DG_ANS, ANS_ERR_START);
}

int diag_err_answer(const answer_t *a, const diag_track_t *t)
{
    int e;
    if (a->phase != ANS_FAILED) return 0;
    switch (t->iphase) {
    case ANS_V8:
        return C(DG_V8, a->end == ANS_END_NO_COMMON ? V8_ERR_NO_COMMON : V8_ERR_NO_CM);
    case ANS_V22:
        return v22_err(a->end == ANS_END_LOST, a->v22.state == V22S_FAILED);
    case ANS_V21: case ANS_V23:
        return C(DG_V21, a->end == ANS_END_LOST ? V21_ERR_CARRIER : V21_ERR_NO_CARRIER);
    case ANS_V32:
        if (a->u.v32.error) return C(DG_V32, a->u.v32.error);
        break;
    case ANS_P2: case ANS_RETRAIN:
        if (a->p2.error) return C(DG_P2D, a->p2.error);
        if (a->p2.state == P2D_DONE) return after_p2_err(a);
        break;
    case ANS_P3: case ANS_V90:
        if (a->p34.error) return C(DG_P34, a->p34.error);
        if (a->p34.state == P34_RETRAIN) return C(DG_ANS, ANS_ERR_RETRAINS);
        break;
    case ANS_P3X: case ANS_V92:
        if (a->u.v92.error) return C(DG_V92D, a->u.v92.error);
        if (a->u.v92.state == V92D_RETRAIN) return C(DG_ANS, ANS_ERR_RETRAINS);
        break;
    case ANS_V34:
        if (a->v34.state == V34A_X2) return a->u.x2.error ? C(DG_X2D, a->u.x2.error) : C(DG_ANS, ANS_ERR_START);
        if (a->v34.state == V34A_K56) return k56_start_err(a);
        if ((e = v34a_err(&a->v34)) != 0) return e;
        if (a->v34.state == V34A_RETRAIN) return C(DG_ANS, ANS_ERR_RETRAINS);
        break;
    case ANS_X2:
        if (a->u.x2.error) return C(DG_X2D, a->u.x2.error);
        break;
    case ANS_K56:
        if (a->u.k56.error) return C(DG_K56D, a->u.k56.error);
        if (a->u.k56.state == K56D_RETRAIN) return C(DG_ANS, ANS_ERR_RETRAINS);
        break;
    case ANS_K56RT:
        if (a->v34.p2.state == P2N_K56) return k56_start_err(a);
        if (a->v34.p2.error) return C(DG_P2N, a->v34.p2.error);
        if (a->v34.p2.state == P2N_DONE || a->v34.p2.state == P2N_X2) return C(DG_ANS, ANS_ERR_K56_RETRAIN);
        break;
    default:
        break;
    }
    /* no engine's cause: the phase ran past the modem's own limit for it */
    return C(DG_ANS, ANS_ERR_TIMEOUT);
}

int diag_err_orig(const orig_t *o, const diag_track_t *t)
{
    if (o->phase != ORIG_FAILED) return 0;
    switch (t->iphase) {
    case ORIG_V8:
        return C(DG_V8, o->end == ORIG_END_NO_TONE ? V8_ERR_NO_TONE : o->end == ORIG_END_NO_JM ? V8_ERR_NO_JM : V8_ERR_NO_COMMON);
    case ORIG_V22:
        return v22_err(o->end == ORIG_END_LOST, o->e.leg.v22.state == V22S_FAILED);
    case ORIG_V21: case ORIG_V23:
        return C(DG_V21, o->end == ORIG_END_LOST ? V21_ERR_CARRIER : V21_ERR_NO_CARRIER);
    case ORIG_V32:
        if (o->e.leg.v32.error) return C(DG_V32, o->e.leg.v32.error);
        break;
    case ORIG_V34: {
        const v34orig_t *v = &o->e.v34;
        if (v->error == V34O_ERR_PHASE2 && v->p2.error) return C(DG_P2L, v->p2.error);
        if (v->error == V34O_ERR_TRAINING && v->dx.error) return C(DG_DX, v->dx.error);
        if (v->error) return C(DG_V34O, v->error);
        break;
    }
    case ORIG_V90:
        if (o->e.v90.error == V90A_ERR_PHASE2 && o->e.v90.p2.error) return C(DG_P2L, o->e.v90.p2.error);
        if (o->e.v90.error) return C(DG_V90A, o->e.v90.error);
        break;
    default:
        break;
    }
    /* the start-up ran past the calling modem's own limit for its phase
     * (ANS heard and no modulation, Bell, V.32bis, V.34, V.90) */
    return C(DG_ORIG, ORIG_ERR_TIMEOUT);
}
