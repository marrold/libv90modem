/* libv90modem: what is behind v90m_get_diag and v90m_err_name
 * (include/v90modem.h): the phase and the modulation chosen, kept after
 * every sample, and the cause of the end read from the engines.
 *
 * An error code is a family's base plus the engine's own number,
 * V90M_DIAG_CODE(DG_P2D, P2D_ERR_INFO1A); its name is the engine's own
 * (api/v90modem_diag.c). A family is an engine's enum of errors, or, for
 * the causes no engine numbers (V.8, V.21, V.22, the answering and calling
 * modems' own limits), one of the enums here. A new error in an engine
 * goes into its family: its name in v90modem_diag.c and its count here.
 * Never installed; not part of the API. */
#ifndef V90MODEM_DIAG_H
#define V90MODEM_DIAG_H
#include <stdint.h>
#include "../include/v90modem.h"
#include "../modem/answer.h"
#include "../modem/orig.h"

enum {
    DG_NONE,
    DG_V8,      /* V.8, either role (below) */
    DG_V21,     /* V.21, Bell 103 and V.23, either role (below) */
    DG_V22,     /* V.22bis, V.22 and Bell 212A, either role (below) */
    DG_V32,     /* V32_ERR_* (v32/v32.h), either role */
    DG_P2D,     /* P2D_ERR_*: answering V.90 / V.92 Phase 2 (modem/p2d.h) */
    DG_P34,     /* P34_ERR_*: answering V.90 Phases 3 and 4, data (modem/p34d.h) */
    DG_V92D,    /* V92D_ERR_*: answering V.92 with PCM upstream (v92/v92_p34d.h) */
    DG_P2N,     /* P2N_ERR_*: answering V.34 Phase 2 (modem/p2ans.h) */
    DG_DX,      /* DX_ERR_*: V.34 Phases 3 and 4, data, either role (modem/v34dx.h) */
    DG_V34A,    /* V34A_ERR_*: answering V.34 (modem/v34ans.h) */
    DG_X2D,     /* X2D_ERR_*: answering x2 (modem/x2d.h) */
    DG_K56D,    /* K56D_ERR_*: answering K56flex (modem/k56d.h) */
    DG_P2L,     /* P2L_ERR_*: calling V.34 / V.90 / V.92 Phase 2 (modem/p2call.h) */
    DG_V34O,    /* V34O_ERR_*: calling V.34 (modem/v34orig.h) */
    DG_V90A,    /* V90A_ERR_*: calling V.90 / V.92 (modem/v90a.h) */
    DG_ANS,     /* the answering modem's own (below) */
    DG_ORIG,    /* the calling modem's own (below) */
    DG_NFAM
};
#define V90M_DIAG_CODE(fam, n) ((fam) * 100 + (n))

/* the causes no engine has a number for */
enum {
    V8_ERR_NO_TONE = 1,     /* calling: no answer tone */
    V8_ERR_NO_JM,           /* calling: ANSam, but no JM in answer to CM */
    V8_ERR_NO_COMMON,       /* nothing in common (or, calling, a modulation not built) */
    V8_ERR_NO_CM            /* answering: no CM (a caller without V.8), and neither V.22 nor V.21 offered */
};
enum {
    V21_ERR_NO_CARRIER = 1, /* the far modem's carrier never held */
    V21_ERR_CARRIER         /* its carrier went in data mode */
};
enum {
    V22_ERR_NO_ANSWER = 1,  /* the start-up did not complete in time */
    V22_ERR_HANDSHAKE,      /* the start-up failed (the V.22bis engine's end) */
    V22_ERR_RETRAIN,        /* a retrain in data mode (V.22bis 6.4) never completed */
    V22_ERR_CARRIER         /* the carrier went in data mode */
};
enum {
    ANS_ERR_TIMEOUT = 1,    /* a phase ran past its limit (the backstop, a K56flex retrain's time) */
    ANS_ERR_RETRAINS,       /* one more retrain was due past the limit */
    ANS_ERR_START,          /* the next engine could not start with what was agreed */
    ANS_ERR_K56_RETRAIN     /* the K56flex client's retrain did not take K56flex up again */
};
enum {
    ORIG_ERR_TIMEOUT = 1    /* the start-up ran past the calling modem's limit for its phase */
};

/* the last value of the public enums the name helpers cover: a value added to
 * v90m_phase or v90m_conn needs its name in api/v90modem_diag.c and this
 * changed (tools/check-diag-names.sh checks that it is the enum's last) */
#define DG_LAST_PHASE V90M_PHASE_DATA
#define DG_LAST_CONN  V90M_CONN_V23

/* the number of codes in family f (1 .. that); 0 for none */
#define DG_COUNT_V8   V8_ERR_NO_CM
#define DG_COUNT_V21  V21_ERR_CARRIER
#define DG_COUNT_V22  V22_ERR_CARRIER
#define DG_COUNT_V32  V32_ERR_CARRIER
#define DG_COUNT_P2D  P2D_ERR_TIME
#define DG_COUNT_P34  P34_ERR_HANGUP
#define DG_COUNT_V92D V92D_ERR_CARRIER
#define DG_COUNT_P2N  P2N_ERR_TIME
#define DG_COUNT_DX   DX_ERR_HANGUP
#define DG_COUNT_V34A V34A_ERR_HANGUP
#define DG_COUNT_X2D  X2D_ERR_RETRAIN
#define DG_COUNT_K56D K56D_ERR_HANGUP
#define DG_COUNT_P2L  P2L_ERR_NO_RATE
#define DG_COUNT_V34O V34O_ERR_HANGUP
#define DG_COUNT_V90A V90A_ERR_RETRAINS
#define DG_COUNT_ANS  ANS_ERR_K56_RETRAIN
#define DG_COUNT_ORIG ORIG_ERR_TIMEOUT
static inline int dg_count(int fam)
{
    static const uint8_t n[DG_NFAM] = {
        0, DG_COUNT_V8, DG_COUNT_V21, DG_COUNT_V22, DG_COUNT_V32, DG_COUNT_P2D, DG_COUNT_P34, DG_COUNT_V92D,
        DG_COUNT_P2N, DG_COUNT_DX, DG_COUNT_V34A, DG_COUNT_X2D, DG_COUNT_K56D, DG_COUNT_P2L, DG_COUNT_V34O,
        DG_COUNT_V90A, DG_COUNT_ANS, DG_COUNT_ORIG
    };
    return fam >= 0 && fam < DG_NFAM ? n[fam] : 0;
}

/* what the API keeps of a call as it goes: the modem's own phase and the
 * modulation chosen (v90m_conn) of the last sample before the end, and
 * the furthest public phase (v90m_phase) reached; the phases are in the
 * order a call goes through them */
typedef struct { uint8_t iphase, phase, sel; } diag_track_t;

/* after a sample that did not end the call */
void diag_track_answer(diag_track_t *t, const answer_t *a);
void diag_track_orig(diag_track_t *t, const orig_t *o);
/* the code of why the call ended (the modem's phase is ANS_FAILED /
 * ORIG_FAILED; t as the last sample before left it) */
int  diag_err_answer(const answer_t *a, const diag_track_t *t);
int  diag_err_orig(const orig_t *o, const diag_track_t *t);

#endif
