/* modem/v8orig: the calling side of V.8 (8.1) against the library's answer
 * side (modem/v8ans) over a 2-wire line (sim/v32line: echo at both hybrids,
 * noise, delay, carrier shift, the answerer on its own clock), and against
 * answerers that send ANS, ANSam without answering, or nothing. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/v8orig.h"
#include "../modem/v8ans.h"
#include "v32line.h"

#define FS 8000
#define TWO_PI 6.283185307179586
#define PI 3.141592653589793
#define MS(ms) ((long)(ms) * FS / 1000)

enum { P_V8ANS, P_ANS, P_ANSAM_ONLY, P_SILENT };
static int peer_kind;
static v8ans_t A;                      /* the far end, when it is the library's answerer */
static ansam_t G;                      /* the far end sending ANSam and nothing else */
static long peer_n;
static double peer_amp;
static long peer_lead;                 /* samples of silence before the far end's tone */
static long lead_ms = 300;             /* the far end starts this long after answering */
static long jm_at;                     /* peer sample at which the answerer began JM */
static long delay_set = -1;            /* the line's one-way delay, when a test sets it */
static int jm_pcm_analog;              /* the answerer's JM names "PCM analogue" (an analogue V.90 modem) */

static int16_t clip(double x) { long q = lrint(x); return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q); }

static double peer(void *u, double rx)
{
    int16_t tx = 0;
    (void)u;
    peer_n++;
    if (peer_n <= peer_lead) return 0;
    switch (peer_kind) {
    case P_V8ANS:
        if (v8ans_tick(&A, clip(rx), &tx) == V8A_JM && !jm_at) {
            jm_at = peer_n;
            if (jm_pcm_analog) {        /* the JM, before its first bit goes out */
                uint8_t oct[V8_MAX_OCTETS];
                int n;
                A.jm.has_pcm = 1; A.jm.pcm = V8_PCM_ANALOG;
                n = v8_encode(&A.jm, oct, V8_MAX_OCTETS);
                A.njm = v8_frame(V8_SYNC_CM_JM, oct, n, A.jmbits, V8A_MAXBITS);
            }
        }
        return tx;
    case P_ANSAM_ONLY: ansam_run(&G, &tx, 1); return tx;
    case P_ANS: {      /* V.25 ANS: 2100 Hz with reversals every 450 ms, no modulation */
        double t = (double)(peer_n - peer_lead) / FS, ph = TWO_PI * 2100 * t;
        if (((long)(t / 0.45)) % 2) ph += PI;
        return peer_amp * cos(ph);
    }
    default: return 0;
    }
}

static v32line_t L;
static v8orig_t O;
static long tx_energy_before_tone;     /* our transmit power before the tone was decided */

static void line(v32line_cfg *lc, int realistic)
{
    memset(lc, 0, sizeof *lc);
    lc->mulaw = 1; lc->seed = 7;
    lc->noise_a_dbm0 = lc->noise_c_dbm0 = -300; lc->near_db = lc->far_db = lc->c_near_db = lc->c_far_db = 300;
    lc->delay = 40;
    if (delay_set >= 0) lc->delay = (int)delay_set;
    if (realistic) {
        lc->loss_db = 10; lc->delay = 160;
        lc->near_db = 10;          /* our own CM comes back 10 dB down into our JM receiver */
        lc->far_db = 15; lc->c_near_db = 12; lc->c_far_db = 18;
        lc->noise_a_dbm0 = -45; lc->noise_c_dbm0 = -45;
        lc->freq_offset = 2; lc->ppm = 100;
    }
}

static void menu_cm(v8_menu_t *m, unsigned modes)
{
    memset(m, 0, sizeof *m);
    m->call_function = V8_CALLF_DATA; m->modes = (uint16_t)modes; m->n_mod_octets = 3;
    m->has_prot = 1; m->prot = V8_PROT_LAPM;
}

/* runs the call; returns the caller's result */
static int call(int kind, int realistic, const v8_menu_t *cm, const v8_menu_t *ans_local, int send_ci, long max_samples)
{
    v32line_cfg lc; v8orig_cfg oc; long n; int16_t rx = 0, tx;
    line(&lc, realistic);
    v32line_init(&L, &lc);
    memset(&oc, 0, sizeof oc);
    oc.amp = 4243; oc.local = *cm; oc.send_ci = send_ci;
    v8orig_init(&O, &oc);
    peer_kind = kind; peer_n = 0; peer_lead = MS(lead_ms); peer_amp = 4243;
    if (kind == P_V8ANS) v8ans_init(&A, FS, 4243, ans_local);
    if (kind == P_ANSAM_ONLY) ansam_init(&G, FS, 4243, 1);
    tx_energy_before_tone = 0; jm_at = 0;
    for (n = 0; n < max_samples; n++) {
        if (v8orig_tick(&O, rx, &tx) == V8O_DONE) break;
        if (O.answer_tone == ANST_NONE && !send_ci) tx_energy_before_tone += (long)tx * tx / 1000;
        rx = v32line_tick(&L, tx, peer, NULL);
    }
    /* let the far end hear the end of it */
    for (n = 0; n < MS(200); n++) rx = v32line_tick(&L, 0, peer, NULL);
    return O.result;
}

static void the_call(const char *what, int realistic, int send_ci)
{
    v8_menu_t cm, al;
    int r;
    menu_cm(&cm, V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21);
    menu_cm(&al, V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21);
    r = call(P_V8ANS, realistic, &cm, &al, send_ci, MS(12000));
    printf("  %-44s result %d, tone %s at %.3f s, CM from %.3f s, CJ at %.3f s, done %.3f s; selected %d; answerer: result %d, selected %d%s\n",
           what, r, O.answer_tone == ANST_ANSAM ? "ANSam" : O.answer_tone == ANST_ANS ? "ANS" : "none",
           O.at_tone / (double)FS, O.at_cm / (double)FS, O.at_cj / (double)FS, O.at_post / (double)FS,
           O.selected, A.result, A.selected, send_ci ? "" : "");
    CHECK_EQ_I(r, V8O_RES_OK);
    CHECK_EQ_I(O.answer_tone, ANST_ANSAM);
    CHECK_EQ_I(A.result, V8A_RES_OK);                 /* the answerer saw CJ */
    CHECK_EQ_I(O.selected, A.selected);
    CHECK_EQ_I(O.selected, 3);                        /* V.32bis: the first common mode of Table 4 here */
    /* what the answerer heard is what we offered */
    CHECK_EQ_I(A.cm.call_function, cm.call_function);
    CHECK_EQ_I(A.cm.modes, cm.modes);
    CHECK_EQ_I(A.cm.has_prot, 1);
    /* what we heard is what it sent */
    CHECK_EQ_I(O.jm.modes, A.jm.modes);
    CHECK_EQ_I(O.jm.call_function, A.jm.call_function);
    /* Te: 1 s of silence from the decision to CM (8.1.1) */
    CHECK(O.at_cm - O.at_tone >= MS(1000) && O.at_cm - O.at_tone <= MS(1000) + 2);
    /* 75 +- 5 ms of silence after CJ (8.1.2): CJ is 3 octets, 100 ms */
    CHECK(O.at_post - O.at_cj >= MS(100) - 30 && O.at_post - O.at_cj <= MS(100) + 30);
    if (!send_ci) CHECK_EQ_I(tx_energy_before_tone, 0);   /* no CI: silent until the tone */
    else CHECK(O.ci_sent >= 3);
}

int main(void)
{
    v8_menu_t cm, al;
    int r;

    printf("V.8 calling side against the library's answering side\n");
    the_call("clean line", 0, 0);
    the_call("10 dB loss, echoes, noise, 2 Hz, 100 ppm", 1, 0);
    /* the far end's tone 2.5 s after answering (the longest V.25 silence),
     * so that CI goes out before it */
    lead_ms = 2500;
    the_call("the same with CI, the tone 2.5 s late", 1, 1);
    lead_ms = 300;

    /* JM comes straight after ANSam, and the V.21(H) receiver reads 2100 Hz
     * as characters: one still in progress when JM begins takes the ten ONEs
     * before JM's sync, and that JM goes unseen. Real callers send CJ after
     * the second JM (tools/v90m-fields on the recordings of docs/history/PROGRESS.md,
     * "realcheck": the Conexant CX93001 ends CJ 0.673 s after the start of
     * a 5-octet JM, 0.807 s after a 7-octet one), so the first one must
     * count, whatever the line's delay. */
    printf("CJ after the second JM, whatever the delay (8.1.2)\n");
    {
        long d, worst = 0, late = 0;
        menu_cm(&cm, V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21);
        menu_cm(&al, V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21);
        for (d = 0; d <= 240; d += 8) {
            long after;
            delay_set = d;
            r = call(P_V8ANS, 0, &cm, &al, 0, MS(12000));
            /* JM here: 5 octets, 70 bits, 233 ms. Two of them, the 8 ONEs
             * that end the second, the CM octet under way, a little more */
            after = O.at_cj - (jm_at + d);
            if (after > worst) worst = after;
            if (r != V8O_RES_OK || after > MS(2 * 233 + 27 + 34 + 15)) late++;
        }
        delay_set = -1;
        printf("  delays 0 to 30 ms: CJ at most %.3f s after JM reached us; %ld of 31 waited for a third JM\n", worst / (double)FS, late);
        CHECK_EQ_I(late, 0);
    }

    printf("PCM: a V.90 / V.92 analogue caller to a digital answerer\n");
    menu_cm(&cm, V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21);
    cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
    menu_cm(&al, V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21);
    al.has_pcm = 1; al.pcm = V8_PCM_DIGITAL; al.has_access = 1; al.access = V8_ACC_DIGITAL;
    r = call(P_V8ANS, 1, &cm, &al, 0, MS(12000));
    printf("  result %d, selected %d (V.90 / V.92 is %d), answerer selected %d\n", r, O.selected, V8_SEL_V90, A.selected);
    CHECK_EQ_I(r, V8O_RES_OK);
    CHECK_EQ_I(O.selected, V8_SEL_V90);
    CHECK_EQ_I(A.selected, V8_SEL_V90);

    printf("PCM: a JM naming PCM-analogue only (an analogue answerer): V.34, not V.90\n");
    {
        v8_menu_t jm_v34_only;
        jm_pcm_analog = 1;
        r = call(P_V8ANS, 1, &cm, &al, 0, MS(12000));
        jm_pcm_analog = 0;
        jm_v34_only = O.jm; jm_v34_only.has_pcm = 0; jm_v34_only.pcm = 0;
        printf("  result %d, JM pcm %d/0x%x, selected %d\n", r, O.jm.has_pcm, O.jm.pcm, O.selected);
        CHECK_EQ_I(r, V8O_RES_OK);
        CHECK(O.jm.has_pcm && O.jm.pcm == V8_PCM_ANALOG);
        CHECK_EQ_I(O.selected, v8_selected(&jm_v34_only));
        CHECK_EQ_I(O.selected, V8_SEL_V34);
    }

    printf("nothing in common: JM with no modulation mode (8.1.2)\n");
    menu_cm(&cm, V8_MOD_V22 | V8_MOD_V21);
    menu_cm(&al, V8_MOD_V34);
    r = call(P_V8ANS, 1, &cm, &al, 0, MS(12000));
    printf("  result %d, answerer result %d\n", r, A.result);
    CHECK_EQ_I(r, V8O_RES_NO_COMMON);
    CHECK_EQ_I(A.result, V8A_RES_OK);                 /* it got CJ all the same */

    printf("a pre-V.8 answerer: ANS (V.25), so no CM (V.8 7.2)\n");
    menu_cm(&cm, V8_MOD_V22 | V8_MOD_V21);
    r = call(P_ANS, 1, &cm, &al, 0, MS(12000));
    printf("  result %d, tone %s at %.3f s, our transmit before and at it %ld\n", r,
           O.answer_tone == ANST_ANS ? "ANS" : "?", O.at_tone / (double)FS, tx_energy_before_tone);
    CHECK_EQ_I(r, V8O_RES_ANS);
    CHECK_EQ_I(O.answer_tone, ANST_ANS);
    CHECK_EQ_I(O.at_cm, 0);
    CHECK_EQ_I(tx_energy_before_tone, 0);

    printf("ANSam but never a JM\n");
    r = call(P_ANSAM_ONLY, 1, &cm, &al, 0, MS(12000));
    printf("  result %d after %.3f s; CM from %.3f s\n", r, O.n / (double)FS, O.at_cm / (double)FS);
    CHECK_EQ_I(r, V8O_RES_NO_JM);
    CHECK(O.at_cm > 0);
    CHECK(O.n - O.at_cm >= MS(5000) && O.n - O.at_cm <= MS(5000) + 2);

    printf("no answer tone at all\n");
    r = call(P_SILENT, 1, &cm, &al, 0, MS(12000));
    printf("  result %d after %.3f s\n", r, O.n / (double)FS);
    CHECK_EQ_I(r, V8O_RES_NO_TONE);
    CHECK(O.n >= MS(10000) && O.n <= MS(10000) + 2);

    T_REPORT("v8orig");
}
