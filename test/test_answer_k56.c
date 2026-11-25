/* The answering modem (modem/answer) with K56flex offered (ANS_MODE_K56):
 * Phase 1, V.8bis before V.8 (modem/v8bis), and what every caller gets
 * from it (docs/superpowers/plans/2026-11-21-k56flex-answering.md, Task 5;
 * Review Focus 1 and 2). K56flex, V.34 and V.90 offered unless said.
 *   - the simulated client (sim/k56call) over a digital link, the whole
 *     V.8bis exchange, its CM V.34 only: agreed, JM selects V.34, with x2
 *     offered too no JM flip and no x2 INFO0a bits; its INFO1c 0x001 at
 *     3200 baud: our INFO1a flagged, ANS_K56, and K56flex's start-up;
 *   - K56flex to data mode (Task 9; modem/k56d): the client asking r = 1,
 *     pad 0, no robbed bits: 32000 down, up the highest rate of its mask at
 *     3200 baud (with its mask to 28800, what sim/v34peer gets on the same
 *     line); every training segment as the facts give it, its Js, 16 MPs
 *     and 16 MP's, E; the data check (a self-synchronising PRBS 2^23 - 1
 *     both ways, DBITS bits and no error each way: reaching data mode is
 *     not a pass); max_down 32000 holds a client asking 13 to r = 1; we
 *     never retrain; our cleardown is silence at once; Review Focus 4 (the
 *     client silent at five points: the end within 10 s, K56flex's, in
 *     data mode the line's loss) and 5 (pad code 3, MPs with rate_ac 0,
 *     mask 0, no upstream rate: K56flex's end); a client that ignores our
 *     MP (64 states, non-linear, expanded up: B1 retried under x2's
 *     configuration, 10 s of data without error; TEST_SHORT: 3 s), and
 *     B1 wrong with and without it (Review Focus 3: K56D_ERR_B1);
 *   - every rate, the pads and robbed bits (Task 10): r = 1, 2, 7, 12 and
 *     13 at pad 0; pads 3 and 6 dB at r = 13 and 7; robbed 0x24, 0x12, 0x3f
 *     and 0x1f (5 taken as 6) at r = 13 and 3, pad 0 and 6 dB: the rate both
 *     sides, the J octet's pad and robbed symbols in k56d and k56tx, the
 *     training checked octet by octet on the pad's levels, the first data
 *     symbol on the grid of 24, the data check both ways; r = 1 with 0x24 at
 *     pad 0 and 6 dB (no bit to rob at 32000) (TEST_SHORT: those two, r = 13
 *     at pad 0 and 6 dB with and without 0x24, r = 1 at 6 dB);
 *   - the client's renegotiation (Task 11): 5 s into data mode from r = 13
 *     to 10, 5 s into that data to 12 (TEST_SHORT: 2 s), pad 0, robbed bits
 *     none and 0x24: our answer checked by the client octet by octet (R at a
 *     frame boundary, TRN2A 2048, MARK, TRN2B, MP and MP' at 1 bit a symbol,
 *     E 24, the grid of 24), the data check in each of the three data
 *     segments each way, tx_bps 56000, 50000, 54000, answer_moving 0 in
 *     between; a renegotiation MP with rate 0: the far end's cleardown; the
 *     client silent after its renegotiation's S: K56flex's end within 10 s;
 *     60 s of data mode (TEST_SHORT: 15 s) with no R of ours (the events
 *     and end reasons through the API: test/api/test_api.c); a retrain
 *     halfway through its renegotiation (R-3): followed, data again; a
 *     client ignoring our MP that renegotiates, its upstream x2's again or
 *     ours after it (the retry back to ours);
 *   - the client's retrain (Task 12): one 5 s into data mode (TEST_SHORT:
 *     2 s, robbed bits none only), r = 13, pad 0, robbed bits none and
 *     0x24: ANS_K56RT at once, Phase 2 again with INFO1a flagged, the whole
 *     training checked by the client as the first, the data check before and
 *     after each way, retraining from ANS_K56RT to data again, answer_retrain
 *     -1 in it; three in a row 300 ms into data mode each: the third ends
 *     the call as K56flex's; three 5 s of data apart: the call goes on; a
 *     retrain with INFO1c 0x200: K56flex's end (the events through the API:
 *     test/api/test_api.c);
 *   - Review Focus 3: the agreed client with INFO1c 0x200, or one that
 *     cannot transmit at 3200 baud: INFO1a unflagged, V.34 at the rates of
 *     the same client without K56flex;
 *   - the same client with CRd and no MS (a V.8bis caller without K56flex,
 *     Review Focus 1): our ANSam 1700 ms (+-20) after our CL ended (V.8bis
 *     done at 1500 ms, then v8ans' own 200 ms of silence), not agreed,
 *     V.34 as without K56flex; its MS with a bad FCS: NAK(1), not agreed;
 *   - the whole exchange with PCM in its CM (Review Focus 2): agreed, but
 *     JM selects V.90 and Phase 2 is V.90's;
 *   - fall-through, each as without K56flex (the same modulation and rates,
 *     100000 bits each way equal end to end): sim/v34peer (no V.8bis), the
 *     library's own calling modem in V.90 and in V.92 (test/v90link.h; there
 *     the reference answerer starts as late as V.8 does behind V.8bis, since
 *     V.92's rates move a step with the line's noise, and from there on it
 *     sends the same octets), the x2 client (sim/x2call) with x2 offered
 *     too;
 *   - an A-law line: not one transmit sample differs (no CRe).
 * A click in data mode (Task 20: 0.75 ms of full scale on the line to us,
 * 4 s into data mode): the upstream clean from 2 s after it.
 * The delays K56flex adds before our ANSam are printed (docs/limits.md). */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "test.h"
#include <inttypes.h>
#include "wctr.h"
#include "../modem/answer.h"
/* the old cleared flag, from the end reason: 1 the caller's cleardown, 2 ours */
static int cleared_of(const answer_t *a)
{
    int e = answer_end(a);
    return e == ANS_END_CLEARDOWN_FAR ? 1 : e == ANS_END_CLEARDOWN_OURS ? 2 : 0;
}
#include "../modem/k56d.h"
#include "../sim/k56call.h"
#include "../sim/x2call.h"
#include "../sim/g711.h"
#include "../sim/noise.h"
#include "line34.h"
#ifndef TEST_SHORT          /* on ARM under qemu (test.h): not the calling modem, not x2 */
#include "v90link.h"
#endif

#define FS 8000
#define MS(x) ((int64_t)(x) * 8)
#define BASE (ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21)
#define BITS 100000L

static answer_t A;

/* x^7 + x^6 + 1 sent; the receiver predicts each bit from the seven before */
typedef struct { unsigned tx, rx; long n, bad; } prbs_t;
static int p_get(void *u)
{
    prbs_t *p = (prbs_t *)u;
    int b = ((p->tx >> 6) ^ (p->tx >> 5)) & 1;
    p->tx = ((p->tx << 1) | (unsigned)b) & 127;
    return b;
}
static void p_put(void *u, int bit)
{
    prbs_t *p = (prbs_t *)u;
    int want = ((p->rx >> 6) ^ (p->rx >> 5)) & 1;
    if (p->n >= 7 && want != (bit & 1)) p->bad++;
    p->rx = ((p->rx << 1) | (unsigned)(bit & 1)) & 127;
    p->n++;
}
static prbs_t UA, UC;     /* what we receive, what the caller receives */

/* what our side sent, per run slot */
#define TXN (FS * 20)
static int16_t txs[2][TXN];
static long ntx;
static int slot;

/* Phase 1 as our answerer went through it: samples, -1 none */
typedef struct { long cl_end, ansam_at, p2_at; int prev; int flipped; } p1_t;
static p1_t P1;
static void p1_reset(void) { P1.cl_end = P1.ansam_at = P1.p2_at = -1; P1.prev = -1; P1.flipped = 0; }
/* after our sample n (the next sample is n + 1) */
static void p1_watch(long n)
{
    if (A.phase == ANS_V8BIS) {
        if (P1.prev == V8B_CL && A.u.v8b.state != V8B_CL) P1.cl_end = n + 1;
        P1.prev = A.u.v8b.state;
    }
    if (P1.ansam_at < 0 && A.phase == ANS_V8 && A.v8.state == V8A_ANSAM) P1.ansam_at = n + 1;
    if (P1.p2_at < 0 && A.phase == ANS_P2) P1.p2_at = n + 1;
    if (A.v8.jm_sign < 0) P1.flipped = 1;
}

/* ---- the K56flex client over a digital link (as test_answer_x2's x2 client) ---- */

#define XD 4096
static k56call_t K;

typedef struct {
    int ok, mod, down, up;
    int agreed, sel, x2, sr_diff, cme;
    long bad;
    long k56_at, flag_at, end_at;   /* our ANS_K56, the client's VP_K56 (it took our INFO1a
                                       flagged), ANS_FAILED: samples, -1 none */
    int info1a_off, sym;            /* the INFO1a offset the client received, the symbol rate */
} k_outcome;

static int V34PEER_UP;  /* the same client without K56flex (sim/v34peer): its upstream rate */
static int K_INFO1C = 1, K_UP3000 = 0;  /* the client's options (k56call_cfg), for k_call */

static void k_call(unsigned modes, int v8bis, int cm_pcm, int secs, k_outcome *o)
{
    static uint8_t down[XD]; static int16_t up[XD]; static double dl[XD];
    const int d_ac = 37, d_ca = 41;
    answer_cfg ac; k56call_cfg kc; rng_t rng;
    uint8_t arx = 0xff;
    long n, end = -1;
    memset(o, 0, sizeof *o);
    memset(down, 0xff, sizeof down); memset(up, 0, sizeof up); memset(dl, 0, sizeof dl);
    memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
    rng_seed(&rng, 4242);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, 0, 0);
    k56call_default(&kc, 3000);
    kc.v8bis = v8bis; kc.cm_pcm = cm_pcm;
    kc.info1c_k56 = K_INFO1C; kc.up_sr_3000 = K_UP3000;
    kc.getbit = p_get; kc.putbit = p_put; kc.user = &UC;
    k56call_init(&K, &kc);
    p1_reset();
    o->k56_at = o->flag_at = o->end_at = -1;
    for (n = 0; n < 8000L * secs; n++) {
        int i = (int)(n & (XD - 1));
        double v;
        uint8_t atx = answer_tick_pcm(&A, arx);
        p1_watch(n);
        if (A.phase == ANS_K56 && o->k56_at < 0) o->k56_at = n;
        if (A.phase == ANS_FAILED && o->end_at < 0) o->end_at = n;
        down[i] = atx;
        dl[i] = (double)ulaw_to_linear(atx);
        up[i] = k56call_tick(&K, down[(n - d_ac) & (XD - 1)]);
        /* what reaches us: the client, our own echo off the line's hybrid, noise */
        v = 0.5 * up[(n - d_ca) & (XD - 1)]
            + 0.12 * dl[(n - 2) & (XD - 1)] + 0.08 * dl[(n - 4) & (XD - 1)] - 0.05 * dl[(n - 9) & (XD - 1)]
            + 2.0 * rng_gauss(&rng);
        arx = linear_to_ulaw((int32_t)lround(v));
        if (K.state == K56C_K56 && o->flag_at < 0) o->flag_at = n;
        if (A.phase == ANS_FAILED || K.state == K56C_ENDED) break;
        if (A.phase == ANS_K56 && A.connected) break;       /* K56flex: kd_call's */
        if (cm_pcm && A.phase == ANS_P2) break;
        /* in data: BITS each way, from 1 s into it */
        if (end < 0 && A.connected && K.state == K56C_V34 && K.peer.state == VP_DATA) {
            end = n + 8000; UA.n = UA.bad = UC.n = UC.bad = 0;
        }
        if (end >= 0 && n == end) { UA.n = UA.bad = UC.n = UC.bad = 0; }
        if (end >= 0 && n > end && UA.n >= BITS && UC.n >= BITS) break;
    }
    o->agreed = A.k56_agreed; o->sel = A.v8.selected; o->x2 = v8ans_x2(&A.v8);
    o->sr_diff = K.peer.p2.info0a.max_sr_diff; o->cme = K.peer.p2.info0a.cme;
    o->ok = A.connected && A.modulation == ANS_MOD_V34 && K.state == K56C_V34 && K.peer.state == VP_DATA;
    o->mod = A.modulation; o->down = A.bitrate; o->up = A.rx_bitrate;
    o->bad = UA.bad + UC.bad;
    if (UA.n < BITS || UC.n < BITS) o->bad = -1;
    o->info1a_off = K.peer.p2.info1a.freq_off; o->sym = A.v34.p2.sym_rate;
}

static void k_report(const char *what, const k_outcome *o)
{
    printf("  %s: %s %d / %d bit/s, %ld bad; agreed %d, JM %d, x2 %d (flipped %d), CL %d (ok %d), MS %d, ACK %d, NAK %d\n",
           what, o->ok ? "V.34" : "NO DATA", o->down, o->up, o->bad, o->agreed, o->sel, o->x2, P1.flipped,
           K.got_cl, K.cl_ok, K.sent_ms, K.got_ack, K.got_nak);
}

static void k56_client_tests(void)
{
    k_outcome o0, o1, o2;
    long ansam0;

    /* without K56flex: the reference (the client never hears CRe) */
    k_call(BASE, K56C_V8BIS_FULL, 0, 40, &o0);
    ansam0 = P1.ansam_at;
    V34PEER_UP = o0.up;
    k_report("K56flex client, K56flex not offered", &o0);
    CHECK(o0.ok); CHECK_EQ_I(o0.bad, 0); CHECK_EQ_I(o0.agreed, 0);
    CHECK_EQ_I(K.got_cre, 0); CHECK_EQ_I(K.got_cl, 0);

    /* the whole exchange, x2 offered too: agreed, x2 not offered in the
     * call; INFO1c 0x001, 3200 baud: our INFO1a flagged, the point of no
     * return, and K56flex's start-up to data mode (its data: kd_call) */
    k_call(BASE | ANS_MODE_K56 | ANS_MODE_X2, K56C_V8BIS_FULL, 0, 40, &o1);
    k_report("K56flex client, K56flex and x2 offered", &o1);
    printf("     our ANSam %.3f s after answering (%.3f s without K56flex); INFO1a offset %d at %d baud,"
           " ANS_K56 at %.3f s, the client took the flag at %.3f s; K56flex %d / %d bit/s\n",
           P1.ansam_at / 8000.0, ansam0 / 8000.0, o1.info1a_off, o1.sym, o1.k56_at / 8000.0, o1.flag_at / 8000.0,
           A.bitrate, A.rx_bitrate);
    CHECK_EQ_I(A.k56_offered, 1);
    CHECK_EQ_I(K.got_cre, 1); CHECK_EQ_I(K.got_cl, 1); CHECK_EQ_I(K.cl_ok, 1);
    CHECK_EQ_I(K.sent_ms, 1); CHECK_EQ_I(K.got_ack, 1); CHECK_EQ_I(K.got_nak, 0);
    CHECK_EQ_I(o1.agreed, 1);
    CHECK_EQ_I(o1.sel, V8_SEL_V34); CHECK_EQ_I(K.v8_sel, V8_SEL_V34);
    CHECK_EQ_I(o1.x2, 0); CHECK_EQ_I(P1.flipped, 0);
    CHECK_EQ_I(o1.sr_diff, o0.sr_diff); CHECK_EQ_I(o1.cme, o0.cme);
    CHECK_EQ_I(A.v34.cfg.x2, 0); CHECK_EQ_I(A.v34.cfg.k56, 1);
    CHECK_EQ_I(A.v34.p2.k56, 1); CHECK_EQ_I(A.v34.state, V34A_K56);
    CHECK_EQ_I(o1.info1a_off, 1); CHECK_EQ_I(o1.sym, 3200);
    CHECK_EQ_I(K.peer.state, VP_K56); CHECK_EQ_I(K.state, K56C_K56);
    CHECK(o1.k56_at > 0); CHECK(o1.flag_at > 0); CHECK_EQ_I(o1.end_at, -1);
    CHECK(o1.k56_at - o1.flag_at <= MS(100) && o1.flag_at - o1.k56_at <= MS(100));
    CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.modulation, ANS_MOD_K56);
    CHECK_EQ_I(answer_end(&A), ANS_END_NONE); CHECK(!o1.ok);

    /* Review Focus 3: agreed, but its INFO1c is 0x200 (its codec check
     * failed), or it cannot transmit at 3200 baud: INFO1a unflagged, V.34
     * at the rates the same client gets without K56flex */
    {
        int j;
        k_outcome r3;
        for (j = 0; j < 2; j++) {
            K_INFO1C = j == 0 ? 0 : 1; K_UP3000 = j;
            k_call(BASE, K56C_V8BIS_FULL, 0, 40, &r3);
            k_call(BASE | ANS_MODE_K56, K56C_V8BIS_FULL, 0, 40, &o2);
            k_report(j ? "agreed K56flex client, 3000 baud up at most" : "agreed K56flex client, INFO1c 0x200", &o2);
            printf("     INFO1a offset %d at %d baud (without K56flex: %d / %d bit/s, offset %d at %d baud)\n",
                   o2.info1a_off, o2.sym, r3.down, r3.up, r3.info1a_off, r3.sym);
            CHECK(r3.ok); CHECK_EQ_I(r3.agreed, 0);
            CHECK_EQ_I(o2.agreed, 1); CHECK_EQ_I(K.got_ack, 1);
            CHECK_EQ_I(A.v34.p2.k56, 0); CHECK_EQ_I(o2.k56_at, -1); CHECK(answer_end(&A) != ANS_END_K56_FAILED);
            CHECK(o2.info1a_off != 1);
            CHECK_EQ_I(o2.info1a_off, p2ans_k56_nudge(r3.info1a_off));
            CHECK_EQ_I(o2.sym, j ? 3000 : 3200); CHECK_EQ_I(o2.sym, r3.sym);
            CHECK(o2.ok); CHECK_EQ_I(o2.mod, ANS_MOD_V34);
            CHECK_EQ_I(o2.down, r3.down); CHECK_EQ_I(o2.up, r3.up); CHECK_EQ_I(o2.bad, 0);
        }
        K_INFO1C = 1; K_UP3000 = 0;
    }

    /* Review Focus 1: CRd, then no MS (a V.92 modem) */
    k_call(BASE | ANS_MODE_K56, K56C_V8BIS_NO_MS, 0, 40, &o2);
    k_report("V.8bis caller without K56flex", &o2);
    printf("     our ANSam %.1f ms after our CL ended; %.3f s later than without K56flex\n",
           (P1.ansam_at - P1.cl_end) / 8.0, (P1.ansam_at - ansam0) / 8000.0);
    CHECK_EQ_I(K.got_cl, 1); CHECK_EQ_I(K.sent_ms, 0); CHECK_EQ_I(K.got_ack, 0); CHECK_EQ_I(K.got_nak, 0);
    CHECK_NEAR(P1.ansam_at - P1.cl_end, 8 * (V8B_MS_WAIT_MS + V8A_SILENCE_MS), 8 * 20);
    CHECK_EQ_I(o2.agreed, 0); CHECK_EQ_I(A.v34.cfg.k56, 0);
    CHECK(o2.ok); CHECK_EQ_I(o2.down, o0.down); CHECK_EQ_I(o2.up, o0.up); CHECK_EQ_I(o2.bad, 0);

    /* a caller without V.8bis over the same link */
    k_call(BASE | ANS_MODE_K56, K56C_V8BIS_NONE, 0, 40, &o2);
    k_report("caller without V.8bis", &o2);
    printf("     our ANSam %.3f s later than without K56flex\n", (P1.ansam_at - ansam0) / 8000.0);
    CHECK_EQ_I(P1.ansam_at - ansam0, 8L * (V8B_SIL_MS + V8B_CRE_DUAL_MS + V8B_CRE_TONE_MS + V8B_CRD_DEADLINE_MS));
    CHECK_EQ_I(o2.agreed, 0);
    CHECK(o2.ok); CHECK_EQ_I(o2.down, o0.down); CHECK_EQ_I(o2.up, o0.up); CHECK_EQ_I(o2.bad, 0);

#ifndef TEST_SHORT
    /* its MS with a bad FCS: NAK(1), not agreed, V.34 */
    k_call(BASE | ANS_MODE_K56, K56C_V8BIS_BAD_FCS, 0, 40, &o2);
    k_report("K56flex client, its MS with a bad FCS", &o2);
    CHECK_EQ_I(K.sent_ms, 1); CHECK_EQ_I(K.got_ack, 0); CHECK_EQ_I(K.got_nak, 1);
    CHECK_EQ_I(o2.agreed, 0);
    CHECK(o2.ok); CHECK_EQ_I(o2.down, o0.down); CHECK_EQ_I(o2.up, o0.up); CHECK_EQ_I(o2.bad, 0);
#endif

    /* Review Focus 2: agreed, but its CM offers PCM while we offer V.90 */
    k_call(BASE | ANS_MODE_K56, K56C_V8BIS_FULL, 1, 20, &o2);
    printf("  K56flex client with PCM in its CM: agreed %d, JM %d, V.90 Phase 2 %.3f s after answering\n",
           o2.agreed, o2.sel, P1.p2_at / 8000.0);
    CHECK_EQ_I(o2.agreed, 1); CHECK_EQ_I(K.got_ack, 1);
    CHECK_EQ_I(o2.sel, V8_SEL_V90); CHECK_EQ_I(K.v8_sel, V8_SEL_V90);
    CHECK(P1.p2_at > 0); CHECK_EQ_I(A.phase, ANS_P2);
    CHECK_EQ_I(o2.x2, 0); CHECK_EQ_I(P1.flipped, 0);
}

/* ---- K56flex to data mode (plan Task 9): the client over the same link ---- */

/* The data check: both directions carry a self-synchronising PRBS (2^23 -
 * 1, x^23 + x^18 + 1); each receiver predicts every bit from the 23 it
 * received before it, and from 64 good bits in a row on counts the bits
 * and the errors of the data segment. Reaching data mode is not a pass. */
#ifdef TEST_SHORT
#define DBITS 20000L
#else
#define DBITS 100000L
#endif
typedef struct { uint32_t tx, rx; int good; long n, bad, ones; } p23_t;
static int p23_get(void *u)
{
    p23_t *p = (p23_t *)u;
    int b = (int)(((p->tx >> 22) ^ (p->tx >> 17)) & 1);
    p->tx = ((p->tx << 1) | (uint32_t)b) & 0x7fffff;
    return b;
}
static void p23_put(void *u, int bit)
{
    p23_t *p = (p23_t *)u;
    int want = (int)(((p->rx >> 22) ^ (p->rx >> 17)) & 1);
    bit &= 1;
    if (p->good >= 64) { p->n++; p->ones += bit; if (bit != want) p->bad++; }
    else p->good = bit == want ? p->good + 1 : 0;
    p->rx = ((p->rx << 1) | (uint32_t)bit) & 0x7fffff;
}
static void p23_init(p23_t *p, uint32_t seed) { memset(p, 0, sizeof *p); p->tx = seed & 0x7fffff; }
/* an all-zero receiver predicts itself: the counted bits must be about half ones */
#define BALANCED(p) ((p).ones * 10 > (p).n * 4 && (p).ones * 10 < (p).n * 6)
static p23_t DA, DC;    /* up: the client sends, we check; down: we send, the client checks */
/* KW: the upstream is counter words (test/wctr.h) in KWU, DA counting them */
static int KW; static wctr_t KWU;
static int  kd_aget(void *u) { (void)u; return p23_get(&DC); }
static void kd_aput(void *u, int b) { (void)u; if (KW) { wctr_put(&KWU, b); DA.n++; } else p23_put(&DA, b); }
static int  kd_cget(void *u) { (void)u; return KW ? wctr_get(&KWU) : p23_get(&DA); }
static void kd_cput(void *u, int b) { (void)u; p23_put(&DC, b); }

typedef struct {
    int rate, pad, rbs, j_octet, silent, mp_bad; uint16_t up_mask;
    int max_down, max_up;
    int echo_delay;         /* answer_v90_line's: 0, or -1 (measured on our TRN1A) */
    int early_e, b1_bad;    /* k56call_cfg's */
    int cleardown;          /* answer_cleardown once the data check has passed */
    int reneg_ms, reneg_rate, reneg_ms2, reneg_rate2, reneg_rate0;    /* k56call_cfg's */
    int retrain_ms, retrains, retrain_200;                          /* k56call_cfg's */
    int heavy_up, retrain_in_reneg;                                 /* k56call_cfg's */
    int hold;               /* seconds of data mode to run (no end at the data check) */
    int click_ms;           /* a click on the line to us this long into data mode (Task 20):
                               six samples of full-scale mu-law, 0x80 and 0x00 alternated */
} kd_opts;

/* the data check of one data segment (from data mode, or from data mode
 * again after a renegotiation or a retrain, to the client's next S or
 * retrain, or the end), each way */
typedef struct { long dn, dbad, un, ubad; int balanced; } kd_seg;
#define KD_SEGS 4

typedef struct {
    int connected_once, mod, down, up, client_down, client_up;
    long k56_at, flag_at, quiet_at, end_at, data_at, e_at;
    int k56_failed, k56_lost, cleared, error, phase;
    int retrain_ret, cd_ret; uint8_t cd_octet; int cd_phase;
    v34_mp_t mp;            /* ours, as the client decoded it */
    /* renegotiations: the data segments, our tx_bps as each began, samples
     * with answer_moving during K56D_RENEG, the client's last S */
    int nseg, nmove; kd_seg seg[KD_SEGS]; int seg_bps[KD_SEGS];
    long moving_in_reneg, rn_at, reneg_seen;
    /* retrains: the client's last, our first ANS_K56RT, samples in ANS_K56RT,
     * samples from its retrain to data again without retraining set (and
     * not ended), answer_retrain in ANS_K56RT */
    long rt_at, k56rt_at, rt_in, rt_not_retraining, move_at;
    int rt_ret;
    long click_at, click_bad, click_n;  /* the click, the upstream errors and bits 2 s after it */
} kd_out;

/* the data check from here on: the counts cleared, the 64 good bits again */
static void p23_restart(p23_t *p) { p->good = 0; p->n = p->bad = p->ones = 0; }
static void kd_seg_close(kd_out *o)
{
    kd_seg *g;
    if (o->nseg >= KD_SEGS) return;
    g = &o->seg[o->nseg++];
    g->dn = DC.n; g->dbad = DC.bad; g->un = DA.n; g->ubad = DA.bad;
    g->balanced = BALANCED(DA) && BALANCED(DC);
}

static void kd_default(kd_opts *k)
{
    memset(k, 0, sizeof *k);
    k->rate = 13; k->j_octet = -1; k->up_mask = 0x3fff;
}

#ifdef TEST_SHORT
#define KH_S 3              /* data mode of the client ignoring our MP, s */
#else
#define KH_S 10
#endif

/* the client behind a packet network (an ATA's jitter buffers), for the
 * next kd_call (which clears it): d samples each way, and our echo off its
 * line's hybrid a round trip late, 20 dB down; 0: the line above */
static struct { int d; } KFAR;

static void kd_call(const kd_opts *ko, long secs, kd_out *o)
{
    static uint8_t down[XD]; static int16_t up[XD]; static double dl[XD];
    const int d_ac = KFAR.d ? KFAR.d : 37, d_ca = KFAR.d ? KFAR.d : 41;
    const int e = KFAR.d ? d_ac + d_ca : 0;
    const double g = KFAR.d ? 0.65 : 1.0;
    answer_cfg ac; k56call_cfg kc; rng_t rng;
    uint8_t arx = 0xff;
    long n;
    int passed = 0, rn = 0, was_mv = 0;
    memset(o, 0, sizeof *o);
    o->k56_at = o->flag_at = o->quiet_at = o->end_at = o->data_at = -1;
    memset(down, 0xff, sizeof down); memset(up, 0, sizeof up); memset(dl, 0, sizeof dl);
    p23_init(&DA, 0x1234567); p23_init(&DC, 0x7654321);
    rng_seed(&rng, 4242);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = BASE | ANS_MODE_K56; ac.getbit = kd_aget; ac.putbit = kd_aput; ac.user = NULL;
    answer_init(&A, &ac);
    answer_v90_line(&A, ko->echo_delay, ko->max_down, ko->max_up);
    k56call_default(&kc, 3000);
    kc.rate = ko->rate; kc.pad_mode = ko->pad; kc.rbs = ko->rbs; kc.j_octet = ko->j_octet;
    kc.up_mask = ko->up_mask; kc.silent = ko->silent; kc.mp_bad = ko->mp_bad;
    kc.early_e = ko->early_e; kc.b1_bad = ko->b1_bad;
    kc.reneg_ms = ko->reneg_ms; kc.reneg_rate = ko->reneg_rate; kc.reneg_ms2 = ko->reneg_ms2;
    kc.reneg_rate2 = ko->reneg_rate2; kc.reneg_rate0 = ko->reneg_rate0;
    kc.retrain_ms = ko->retrain_ms; kc.retrains = ko->retrains; kc.retrain_200 = ko->retrain_200;
    kc.heavy_up = ko->heavy_up; kc.retrain_in_reneg = ko->retrain_in_reneg;
    kc.getbit = kd_cget; kc.putbit = kd_cput; kc.user = NULL;
    k56call_init(&K, &kc);
    o->rn_at = o->reneg_seen = o->rt_at = o->k56rt_at = -1;
    o->rt_ret = 1;
    o->click_at = -1;
    for (n = 0; n < 8000L * secs; n++) {
        int i = (int)(n & (XD - 1));
        double v;
        uint8_t atx;
        if (ko->click_ms && o->data_at >= 0) {
            o->click_at = o->data_at + 8L * ko->click_ms;
            if (n >= o->click_at && n < o->click_at + 6) arx = (n - o->click_at) & 1 ? 0x00 : 0x80;
            if (n == o->click_at + 8000L * 2) { o->click_bad = DA.bad; o->click_n = DA.n; }
        }
        atx = answer_tick_pcm(&A, arx);
        if (A.phase == ANS_K56 && o->k56_at < 0) o->k56_at = n;
        if (A.connected) { o->connected_once = 1; o->mod = A.modulation; o->down = A.bitrate; o->up = A.rx_bitrate; }
        if (A.phase == ANS_K56 && A.connected && o->data_at < 0) o->data_at = n;
        down[i] = atx;
        /* the echo is of what the client's line card plays: after the
         * network's pad (its robbed bit, one level step, left out) */
        dl[i] = (double)ulaw_to_linear(x2call_pad(atx, ko->pad));
        up[i] = k56call_tick(&K, down[(n - d_ac) & (XD - 1)]);
        v = 0.5 * up[(n - d_ca) & (XD - 1)]
            + g * 0.12 * dl[(n - e - 2) & (XD - 1)] + g * 0.08 * dl[(n - e - 4) & (XD - 1)] - g * 0.05 * dl[(n - e - 9) & (XD - 1)]
            + 2.0 * rng_gauss(&rng);
        arx = linear_to_ulaw((int32_t)lround(v));
        if (K.state == K56C_K56 && o->flag_at < 0) o->flag_at = n;
        if (ko->retrain_ms || ko->retrain_in_reneg) {
            if (A.phase == ANS_K56RT) {
                if (o->k56rt_at < 0) { o->k56rt_at = n; o->rt_ret = answer_retrain(&A); }
                o->rt_in++;
            }
            if (K.retrains && A.phase != ANS_FAILED && !answer_moving(&A) && !A.retraining) o->rt_not_retraining++;
        }
        if (ko->reneg_ms || ko->retrain_ms) {
            /* a segment ends at the client's S or retrain (what is received
             * after it is not data); the next one is checked from data mode
             * again */
            int mv = answer_moving(&A);
            if (K.renegs + K.retrains > rn) { rn = K.renegs + K.retrains; o->rn_at = n; kd_seg_close(o); }
            if (K.retrains) o->rt_at = o->rn_at;
            if (A.phase == ANS_K56 && A.u.k56.state == K56D_RENEG) {
                if (o->reneg_seen < 0) o->reneg_seen = n;
                if (mv) o->moving_in_reneg++;
            }
            if (mv && !was_mv) {
                o->move_at = n;
                if (o->nmove < KD_SEGS) o->seg_bps[o->nmove] = A.bitrate;
                o->nmove++;
                p23_restart(&DA); p23_restart(&DC);
            }
            was_mv = mv;
            /* (a retrain in a renegotiation: its S, its retrain, data again once) */
            /* (its S before data first moved: data moves once) */
            if (rn == (ko->retrain_in_reneg ? 2 : ko->retrain_ms ? ko->retrains : ko->reneg_ms2 ? 2 : 1)
                && (o->nmove == (ko->retrain_in_reneg ? 2 : rn + 1) || (o->nmove == rn && o->move_at > o->rn_at))
                && DA.n >= DBITS && DC.n >= DBITS) {
                kd_seg_close(o);
                break;
            }
        }
        if (A.phase == ANS_FAILED) { o->end_at = n; break; }
        if (ko->hold && o->data_at >= 0 && n - o->data_at >= 8000L * ko->hold) break;
        if (!passed && !ko->reneg_ms && !ko->retrain_ms && DA.n >= DBITS && DC.n >= DBITS) {
            passed = 1;
            o->retrain_ret = answer_retrain(&A);
            if (ko->cleardown) {
                int16_t t;
                o->cd_ret = answer_cleardown(&A);
                t = answer_tick(&A, 0);
                o->cd_octet = linear_to_ulaw(t);
                o->cd_phase = A.phase;
                o->end_at = n;
                break;
            }
            if (!ko->silent && !ko->hold) break;
        }
    }
    KFAR.d = 0;
    o->quiet_at = K.quiet_at; o->e_at = K.e_at;
    o->k56_failed = answer_end(&A) == ANS_END_K56_FAILED; o->k56_lost = answer_end(&A) == ANS_END_LOST;
    o->cleared = cleared_of(&A); o->phase = A.phase;
    o->error = A.u.k56.error;
    o->client_down = k56call_down_rate(&K); o->client_up = K.up_rate;
    o->mp = K.our_mp;
    /* our MP's upstream (hypothesis A) unless the client ignored it, or its
     * B1 was wrong (B tried in vain) */
    if (o->k56_at >= 0 && !ko->heavy_up && !ko->b1_bad) CHECK_EQ_I(A.u.k56.up_hyp, 0);
}

/* the rate our V.34 receiver takes from a client's mask at 3200 baud (its
 * rate_ca 13): the highest of Table 8's (decision 10), as sim/v34peer's
 * own MP lists them for that symbol rate */
static int ref_up(uint16_t mask)
{
    int n;
    for (n = 13; n >= 1; n--) {
        v34_map_t m;
        if (((mask >> (n - 1)) & 1) && v34_map_params(3200, (unsigned)(2400 * n), 0, &m) == 0) return 2400 * n;
    }
    return 0;
}

static void kd_report(const char *what, const kd_out *o)
{
    int k;
    long sb = 0;
    for (k = 0; k < K56TX_DATA; k++) sb += K.seg_bad[k];
    printf("  %s: %s %d / %d bit/s (client %d / %d), data %ld bits %ld bad down, %ld bits %ld bad up;"
           " client: %ld bad training octets, Js %d, MP %d MP' %d (%d wrong), E %d, B1 bad %ld, frames %ld lost %ld;"
           " end: phase %d error %d k56_failed %d lost %d\n",
           what, o->connected_once && o->mod == ANS_MOD_K56 ? "K56flex" : "NO K56FLEX", o->down, o->up,
           o->client_down, o->client_up, DC.n, DC.bad, DA.n, DA.bad, sb, K.js_ok, K.mp_good, K.mpa_good, K.mp_wrong,
           K.e_seen, K.b1_bad, K.data_frames, K.data_lost, o->phase, o->error, o->k56_failed, o->k56_lost);
}

static long seg_bad_sum(void)
{
    long sb = 0; int k;
    for (k = 0; k < K56TX_DATA; k++) sb += K.seg_bad[k];
    return sb;
}

static void k56_data_tests(void)
{
    kd_opts ko; kd_out o;
    int j;

    /* pad 0, no robbed bits, the client asking r = 1: 32000 down, the
     * highest upstream rate of its mask at 3200 baud; the training as the
     * facts give it, segment by segment; the data check both ways */
    kd_default(&ko); ko.rate = 1;
    kd_call(&ko, 60, &o);
    kd_report("K56flex client, r = 1", &o);
    printf("     ANS_K56 at %.3f s, data mode at %.3f s (%.3f s after); silence 2 %d, TRN2A %d, TRN2B %d symbols;"
           " bad octets by segment:", o.k56_at / 8000.0, o.data_at / 8000.0, (o.data_at - o.k56_at) / 8000.0,
           (int)K.sil2_n, (int)K.trn2a_n, (int)K.trn2b_n);
    for (j = 0; j < K56TX_DATA; j++) printf(" %ld", K.seg_bad[j]);
    printf("\n");
    CHECK_EQ_I(o.connected_once, 1); CHECK_EQ_I(o.mod, ANS_MOD_K56);
    CHECK_EQ_I(o.down, 32000); CHECK_EQ_I(o.client_down, 32000);
    CHECK_EQ_I(o.up, ref_up(0x3fff)); CHECK_EQ_I(o.up, 31200); CHECK_EQ_I(o.client_up, o.up);
    CHECK_EQ_I(o.mp.max_ac, 1); CHECK_EQ_I(o.mp.max_ca * 2400, o.up);
    CHECK(DA.n >= DBITS); CHECK(DC.n >= DBITS); CHECK_EQ_I(DA.bad, 0); CHECK_EQ_I(DC.bad, 0);
    CHECK(BALANCED(DA)); CHECK(BALANCED(DC));
    CHECK_EQ_I(seg_bad_sum(), 0);
    CHECK_EQ_I(K.js_ok, 1); CHECK_EQ_I(K.mp_good, 16); CHECK_EQ_I(K.mpa_good, 16); CHECK_EQ_I(K.mp_wrong, 0);
    CHECK_EQ_I(K.e_seen, 1); CHECK_EQ_I(K.b1_bad, 0); CHECK_EQ_I(K.data_lost, 0);
    CHECK_EQ_I(K.sil2_n % 8, 0); CHECK(K.sil2_n >= K56TX_SIL2_MIN); CHECK_EQ_I(K.trn2a_n, K56TX_TRN2A_N);
    CHECK(K.trn2b_n >= K56TX_TRN2B_MIN);
    CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA);
    CHECK_EQ_I(k56d_pad(&A.u.k56), 0); CHECK_EQ_I(k56d_rbs(&A.u.k56), 0);
    /* we never retrain: answer_retrain refuses (the API: V90M_ESTATE) */
    CHECK_EQ_I(o.retrain_ret, -1);
    CHECK(answer_moving(&A)); CHECK(answer_snr_db10(&A) > 200); CHECK(answer_rtd(&A) > 0);

    /* the client's mask without the rates above 28800: what sim/v34peer
     * gets from the same line at 3200 baud (Ruling F9); and our echo delay
     * not given: measured on our TRN1A (the line's echo is 2 to 9 samples
     * late) */
    kd_default(&ko); ko.rate = 1; ko.up_mask = 0x0fff; ko.echo_delay = -1;
    kd_call(&ko, 60, &o);
    kd_report("K56flex client, mask 0FFF, echo delay measured", &o);
    printf("     echo delay measured: %d samples\n", A.u.k56.ecd.delay);
    CHECK(A.u.k56.ecd.delay >= 0 && A.u.k56.ecd.delay <= 2);
    CHECK_EQ_I(o.mod, ANS_MOD_K56); CHECK_EQ_I(o.up, ref_up(0x0fff)); CHECK_EQ_I(o.up, V34PEER_UP);
    printf("     (the same client without K56flex, sim/v34peer: %d bit/s up)\n", V34PEER_UP);
    CHECK(DA.n >= DBITS); CHECK(DC.n >= DBITS); CHECK_EQ_I(DA.bad, 0); CHECK_EQ_I(DC.bad, 0);

    /* max_down 32000, the client asking 13: r = 1 */
    kd_default(&ko); ko.max_down = 32000;
    kd_call(&ko, 60, &o);
    kd_report("K56flex client asking r = 13, max_down 32000", &o);
    CHECK_EQ_I(o.mod, ANS_MOD_K56); CHECK_EQ_I(o.down, 32000); CHECK_EQ_I(o.mp.max_ac, 1); CHECK_EQ_I(o.client_down, 32000);
    CHECK(DA.n >= DBITS); CHECK(DC.n >= DBITS); CHECK_EQ_I(DA.bad, 0); CHECK_EQ_I(DC.bad, 0);

    /* our cleardown in data: silence (0xff) at once, the call over as ours */
    kd_default(&ko); ko.rate = 1; ko.cleardown = 1;
    kd_call(&ko, 60, &o);
    printf("  our cleardown in K56flex data mode: %d, then octet %02X, phase %d, cleared %d\n",
           o.cd_ret, o.cd_octet, o.cd_phase, o.cleared);
    CHECK_EQ_I(o.cd_ret, 0); CHECK_EQ_I(o.cd_octet, 0xff); CHECK_EQ_I(o.cd_phase, ANS_FAILED);
    CHECK_EQ_I(o.cleared, 2); CHECK_EQ_I(o.k56_failed, 0); CHECK_EQ_I(o.error, K56D_ERR_HANGUP);

    /* a click in data mode (Task 20): 0.75 ms of full scale on the line
     * to us, 4 s into data mode, then 14 s more. From 2 s after it the
     * upstream is clean again, or the call has ended within 3 s of it as
     * lost: never up and deaf */
    kd_default(&ko); ko.rate = 1; ko.click_ms = 4000; ko.hold = 18;
    kd_call(&ko, 60, &o);
    kd_report("K56flex client, r = 1, a click 4 s into data mode", &o);
    printf("     from 2 s after the click: up %ld bits %ld bad\n", DA.n - o.click_n, DA.bad - o.click_bad);
    CHECK_EQ_I(o.connected_once, 1); CHECK(o.click_at > 0);
    CHECK((DA.bad == o.click_bad && DA.n - o.click_n >= 10L * o.up && answer_end(&A) == ANS_END_NONE)
          || (o.end_at >= 0 && answer_end(&A) == ANS_END_LOST && o.end_at <= o.click_at + 8000L * 3));

    /* Review Focus 4: the client silent after our flagged INFO1a, before
     * its S, before J24, before its MP, before its E, in data mode: the
     * call ends within 10 s, as K56flex's (in data mode: the line lost) */
    {
        static const struct { int sil, err; const char *what; } q[5] = {
            { K56C_SIL_BEFORE_S, K56D_ERR_NO_S, "before its S" }, { K56C_SIL_BEFORE_J, K56D_ERR_NO_J, "before its J24" },
            { K56C_SIL_BEFORE_MP, K56D_ERR_NO_MP, "before its MP" }, { K56C_SIL_BEFORE_E, K56D_ERR_NO_E, "before its E" },
            { K56C_SIL_DATA, K56D_ERR_CARRIER, "in data mode" } };
        for (j = 0; j < 5; j++) {
            kd_default(&ko); ko.rate = 1; ko.silent = q[j].sil;
            kd_call(&ko, 60, &o);
            printf("  K56flex client silent %s: the call ended %.2f s later, error %d, k56_failed %d, lost %d\n",
                   q[j].what, (o.end_at - o.quiet_at) / 8000.0, o.error, o.k56_failed, o.k56_lost);
            CHECK(o.quiet_at > 0); CHECK(o.end_at > o.quiet_at);
            CHECK(o.end_at - o.quiet_at <= 8000L * 10);
            CHECK_EQ_I(o.phase, ANS_FAILED); CHECK_EQ_I(o.error, q[j].err);
            CHECK_EQ_I(o.k56_failed, q[j].sil != K56C_SIL_DATA);
            CHECK_EQ_I(o.k56_lost, q[j].sil == K56C_SIL_DATA);
            CHECK_EQ_I(o.connected_once, q[j].sil == K56C_SIL_DATA);
        }
    }

    /* Review Focus 5: a J24 octet with pad code 3; an MP with rate_ac 0,
     * mask 0, or no upstream rate we receive: a clean clear-down */
    {
        static const struct { int j, bad, err; const char *what; } q[4] = {
            { 0xc0, K56C_MP_GOOD, K56D_ERR_J, "J24 octet C0 (pad code 3)" },
            { -1, K56C_MP_RATE0, K56D_ERR_RATE, "MP rate_ac 0" },
            { -1, K56C_MP_MASK0, K56D_ERR_RATE, "MP mask 0" },
            { -1, K56C_MP_UP_NONE, K56D_ERR_RATE, "MP mask 2000 (33600 only)" } };
        for (j = 0; j < 4; j++) {
            kd_default(&ko); ko.rate = 1; ko.j_octet = q[j].j; ko.mp_bad = q[j].bad;
            kd_call(&ko, 60, &o);
            printf("  K56flex client, %s: the call ended %.2f s after ANS_K56, error %d, k56_failed %d\n",
                   q[j].what, (o.end_at - o.k56_at) / 8000.0, o.error, o.k56_failed);
            CHECK(o.end_at > 0); CHECK_EQ_I(o.phase, ANS_FAILED); CHECK_EQ_I(o.error, q[j].err);
            CHECK_EQ_I(o.k56_failed, 1); CHECK_EQ_I(o.connected_once, 0);
        }
    }
    /* B1 wrong (the client's first 400 symbols of it damaged): the call
     * fails as K56D_ERR_B1, a K56flex failure, never connected */
    kd_default(&ko); ko.rate = 1; ko.b1_bad = 1;
    kd_call(&ko, 60, &o);
    printf("  K56flex client, B1 wrong: ended %.2f s after ANS_K56, error %d, k56_failed %d\n",
           (o.end_at - o.k56_at) / 8000.0, o.error, o.k56_failed);
    CHECK(o.end_at > 0); CHECK_EQ_I(o.phase, ANS_FAILED); CHECK_EQ_I(o.error, K56D_ERR_B1);
    CHECK_EQ_I(o.k56_failed, 1); CHECK_EQ_I(o.k56_lost, 0); CHECK_EQ_I(o.connected_once, 0);

    /* a client that ignores our MP (docs/answering.md, D12): its upstream
     * with 64 states, non-linear, expanded shaping (x2's configuration).
     * B1 fails under what we asked; the stored points replayed under x2's
     * pass, and the data goes on under it (hypothesis B): KH_S of data mode
     * without an error either way */
    kd_default(&ko); ko.heavy_up = K56C_HEAVY_ALWAYS; ko.hold = KH_S;
    kd_call(&ko, 30 + KH_S, &o);
    kd_report("K56flex client ignoring our MP (64 states, non-linear, expanded)", &o);
    printf("     upstream hypothesis %d, %d s of data mode\n", A.u.k56.up_hyp, KH_S);
    CHECK_EQ_I(o.connected_once, 1); CHECK_EQ_I(o.mod, ANS_MOD_K56);
    CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK_EQ_I(A.u.k56.up_hyp, 1);
    CHECK_EQ_I(o.up, 31200); CHECK_EQ_I(o.client_up, 31200); CHECK_EQ_I(o.down, 56000);
    CHECK(DA.n >= (long)KH_S * 31200 - 31200); CHECK(DC.n >= (long)KH_S * 56000 - 56000);
    CHECK_EQ_I(DA.bad, 0); CHECK_EQ_I(DC.bad, 0); CHECK(BALANCED(DA)); CHECK(BALANCED(DC));
    CHECK_EQ_I(K.b1_bad, 0); CHECK_EQ_I(K.data_lost, 0);

    /* Review Focus 3: B1 wrong from a client that keeps to our MP, and from
     * one that does not: neither hypothesis passes, K56D_ERR_B1 as before */
    for (j = 0; j < 2; j++) {
        kd_default(&ko); ko.rate = 1; ko.b1_bad = 1; ko.heavy_up = j ? K56C_HEAVY_ALWAYS : K56C_HEAVY_NONE;
        kd_call(&ko, 60, &o);
        printf("  K56flex client, B1 wrong, heavy upstream %d: ended %.2f s after ANS_K56, error %d, hypothesis %d\n",
               j, (o.end_at - o.k56_at) / 8000.0, o.error, A.u.k56.up_hyp);
        CHECK(o.end_at > 0); CHECK_EQ_I(o.phase, ANS_FAILED); CHECK_EQ_I(o.error, K56D_ERR_B1);
        CHECK_EQ_I(o.k56_failed, 1); CHECK_EQ_I(o.connected_once, 0);
    }

    /* its E before our MP state (not conforming; it comes 0.5 s before we
     * enter K56D_MP): the 1 s for B1 counts from its E, which the entry into
     * K56D_MP must not reset. Reset, the call failed as B1 at the entry;
     * kept, our B1 goes out in time and the call connects */
    kd_default(&ko); ko.rate = 1; ko.early_e = 200;
    kd_call(&ko, 60, &o);
    printf("  K56flex client, E early: E at %.3f s, data mode at %.3f s, ended %.3f s, error %d, connected %d, k56_failed %d\n",
           o.e_at / 8000.0, o.data_at / 8000.0, o.end_at / 8000.0, o.error, o.connected_once, o.k56_failed);
    CHECK(o.e_at > 0); CHECK(o.data_at > o.e_at); CHECK(o.data_at - o.e_at < MS(K56D_T_B1) + MS(1000));
    CHECK_EQ_I(o.connected_once, 1); CHECK_EQ_I(o.mod, ANS_MOD_K56); CHECK_EQ_I(o.k56_failed, 0);
    CHECK_EQ_I(o.error, 0); CHECK_EQ_I(o.phase, ANS_K56);
}


/* ---- every rate, the pads, robbed bits (plan Task 10) ---- */

/* one call of the client asking r behind pad and rbs, j_octet the truth:
 * tx_bps 30000 + 2000 r both sides; the J octet's pad and robbed symbols
 * reach k56d (k56d_pad, k56d_rbs) and k56tx (TRN2B, MP, E on that pad's
 * levels: the client checks every octet of them, seg_bad); the first data
 * symbol on the grid of 24 from TRN1B's first; the data check both ways */
static void kr_call(int rate, int pad, int rbs)
{
    kd_opts ko; kd_out o;
    char what[80];
    uint8_t oct = 0;
    int k, n_rob = 0, n_oct = 0;
    kd_default(&ko); ko.rate = rate; ko.pad = pad; ko.rbs = rbs;
    kd_call(&ko, 60, &o);
    for (k = 0; k < 8; k++) oct = (uint8_t)(oct | K.j24[16 + k] << k);
    for (k = 0; k < 6; k++) { n_rob += (rbs >> k) & 1; n_oct += (oct >> k) & 1; }
    snprintf(what, sizeof what, "r = %d, pad %d, robbed %02X (J octet %02X)", rate, pad, rbs, oct);
    kd_report(what, &o);
    if (seg_bad_sum()) {
        printf("     bad octets by segment:");
        for (k = 0; k < K56TX_DATA; k++) printf(" %ld", K.seg_bad[k]);
        printf("\n");
    }
    CHECK_EQ_I(o.connected_once, 1); CHECK_EQ_I(o.mod, ANS_MOD_K56);
    CHECK_EQ_I(o.down, 30000 + 2000 * rate); CHECK_EQ_I(o.client_down, o.down);
    CHECK_EQ_I(o.mp.max_ac, rate); CHECK_EQ_I(o.up, 31200); CHECK_EQ_I(o.client_up, o.up);
    CHECK_EQ_I(oct >> 6, pad); CHECK_EQ_I(n_oct, n_rob);
    CHECK_EQ_I(k56d_pad(&A.u.k56), pad); CHECK_EQ_I(k56d_rbs(&A.u.k56), oct & 0x3f);
    CHECK_EQ_I(A.u.k56.tx.pad, pad);
    CHECK_EQ_I(A.u.k56.tx.d_rate, rate); CHECK_EQ_I(A.u.k56.tx.d_pad, pad); CHECK_EQ_I(A.u.k56.tx.d_rbs, oct & 0x3f);
    CHECK(K.data_at > K.trn1b_at); CHECK_EQ_I((K.data_at - K.trn1b_at) % 24, 0);
    CHECK_EQ_I(seg_bad_sum(), 0); CHECK_EQ_I(K.mp_good, 16); CHECK_EQ_I(K.mpa_good, 16); CHECK_EQ_I(K.mp_wrong, 0);
    CHECK_EQ_I(K.e_seen, 1); CHECK_EQ_I(K.b1_bad, 0); CHECK_EQ_I(K.data_lost, 0);
    CHECK(DA.n >= DBITS); CHECK(DC.n >= DBITS); CHECK_EQ_I(DA.bad, 0); CHECK_EQ_I(DC.bad, 0);
    CHECK(BALANCED(DA)); CHECK(BALANCED(DC));
    CHECK_EQ_I(A.u.k56.state, K56D_DATA);
}

static void k56_rate_tests(void)
{
    /* r = 1 (32000, q = 0: no bit to rob) with robbed bits 0x24 on the
     * line, pad 0 and 6 dB: the server ignores them (both variants) */
    kr_call(1, 0, 0x24);
    kr_call(1, 2, 0x24);
#ifdef TEST_SHORT
    /* r = 1 and 13, pad 0 and 2, no robbed bits and 0x24 (r = 1, pad 0,
     * none: k56_data_tests) */
    kr_call(13, 0, 0);
    kr_call(1, 2, 0);
    kr_call(13, 2, 0);
    kr_call(13, 0, 0x24);
    kr_call(13, 2, 0x24);
#else
    static const int rates[5] = { 1, 2, 7, 12, 13 };
    static const int robbed[4] = { 0x24, 0x12, 0x3f, 0x1f };   /* 0x1f: 5, taken as 6 */
    int i, p;
    /* the rates, pad 0 (12: 54000, M = 16) */
    for (i = 0; i < 5; i++) kr_call(rates[i], 0, 0);
    /* the pads, 3 and 6 dB, at r = 13 and r = 7 */
    for (p = 1; p <= 2; p++) { kr_call(13, p, 0); kr_call(7, p, 0); }
    /* robbed bits at r = 13 and r = 3, pad 0 and pad 2 */
    for (i = 0; i < 4; i++)
        for (p = 0; p <= 2; p += 2) { kr_call(13, p, robbed[i]); kr_call(3, p, robbed[i]); }
#endif
}


/* ---- the client's renegotiation (plan Task 11) ---- */

#ifdef TEST_SHORT
#define RN_MS  2000         /* into data mode, and into the data after the first */
#define HOLD_S 15
#else
#define RN_MS  5000
#define HOLD_S 60
#endif

static void k56_reneg_tests(void)
{
    kd_opts ko; kd_out o;
    char what[96];
    int j, k;

    /* r = 13, RN_MS into data mode down to 10, RN_MS into that data up to
     * 12; pad 0, no robbed bits and 0x24. The client checks our answer octet
     * by octet: R (at a frame boundary: a whole frame of R's) 288, TRN2A
     * 2048 (its first renegotiation's at 2 bits an even symbol, the second's
     * at 1), MARK 32, TRN2B until its MP, MP and MP' on the pad's 2
     * magnitudes (8 of each), E 24, B1 and the data on the grid of 24, the
     * training's scrambler and running sum going on from the last E. The
     * data check in each of the three segments, each way; answer_moving 0
     * from its S to data again */
    for (j = 0; j < 2; j++) {
        uint8_t oct = 0;
        kd_default(&ko); ko.rbs = j ? 0x24 : 0;
        ko.reneg_ms = RN_MS; ko.reneg_rate = 10; ko.reneg_ms2 = RN_MS; ko.reneg_rate2 = 12;
        kd_call(&ko, 60, &o);
        for (k = 0; k < 8; k++) oct = (uint8_t)(oct | K.j24[16 + k] << k);
        snprintf(what, sizeof what, "renegotiations 13 -> 10 -> 12, robbed %02X", ko.rbs);
        kd_report(what, &o);
        printf("     tx_bps %d, %d, %d; the last TRN2A %d, TRN2B %d symbols; bad octets by segment:",
               o.seg_bps[0], o.seg_bps[1], o.seg_bps[2], (int)K.trn2a_n, (int)K.trn2b_n);
        for (k = 0; k < K56TX_DATA; k++) printf(" %ld", K.seg_bad[k]);
        printf("\n");
        for (k = 0; k < o.nseg && k < KD_SEGS; k++)
            printf("     segment %d: down %ld bits %ld bad, up %ld bits %ld bad\n", k, o.seg[k].dn, o.seg[k].dbad,
                   o.seg[k].un, o.seg[k].ubad);
        CHECK_EQ_I(K.renegs, 2); CHECK_EQ_I(o.nseg, 3); CHECK_EQ_I(o.nmove, 3);
        CHECK_EQ_I(o.seg_bps[0], 56000); CHECK_EQ_I(o.seg_bps[1], 50000); CHECK_EQ_I(o.seg_bps[2], 54000);
        for (k = 0; k < 3; k++) {
            CHECK(o.seg[k].dn >= DBITS); CHECK(o.seg[k].un >= DBITS);
            CHECK_EQ_I(o.seg[k].dbad, 0); CHECK_EQ_I(o.seg[k].ubad, 0); CHECK(o.seg[k].balanced);
        }
        CHECK_EQ_I(o.moving_in_reneg, 0); CHECK(o.reneg_seen > 0);
        CHECK_EQ_I(o.down, 54000); CHECK_EQ_I(o.client_down, 54000); CHECK_EQ_I(o.mp.max_ac, 12);
        CHECK_EQ_I(o.up, 31200); CHECK_EQ_I(o.client_up, 31200);
        CHECK_EQ_I(seg_bad_sum(), 0);
        CHECK_EQ_I(K.mp_good, 16 + 2 * 8); CHECK_EQ_I(K.mpa_good, 16 + 2 * 8); CHECK_EQ_I(K.mp_wrong, 0);
        CHECK_EQ_I(K.e_seen, 1); CHECK_EQ_I(K.b1_bad, 0); CHECK_EQ_I(K.data_lost, 0); CHECK_EQ_I(K.r_unasked, 0);
        CHECK_EQ_I(K.trn2a_n, K56TX_RN_TRN2A_N); CHECK(K.trn2b_n >= K56TX_TRN2B_MIN);
        CHECK(K.data_at > K.trn1b_at); CHECK_EQ_I((K.data_at - K.trn1b_at) % 24, 0);
        CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK(answer_moving(&A));
        CHECK_EQ_I(A.u.k56.tx.d_rate, 12); CHECK_EQ_I(A.u.k56.tx.d_rbs, oct & 0x3f); CHECK_EQ_I(A.u.k56.tx.nb, 1);
        CHECK_EQ_I(o.k56_failed, 0); CHECK_EQ_I(o.error, 0);
    }

    /* its renegotiation's MP with rate 0 both ways (V.34's cleardown): the
     * call ends as the far end's cleardown (V90M_END_CLEARDOWN_FAR) */
    kd_default(&ko); ko.rate = 1; ko.reneg_ms = 1000; ko.reneg_rate = 1; ko.reneg_rate0 = 1;
    kd_call(&ko, 60, &o);
    printf("  K56flex client, a renegotiation MP with rate 0: ended %.2f s after its S, error %d, cleared %d,"
           " k56_failed %d, lost %d\n", (o.end_at - o.rn_at) / 8000.0, o.error, o.cleared, o.k56_failed, o.k56_lost);
    CHECK_EQ_I(K.renegs, 1); CHECK(o.rn_at > 0); CHECK(o.end_at > o.rn_at); CHECK(o.end_at - o.rn_at <= 8000L * 2);
    CHECK_EQ_I(o.phase, ANS_FAILED); CHECK_EQ_I(o.error, K56D_ERR_CLEARDOWN);
    CHECK_EQ_I(o.cleared, 1); CHECK_EQ_I(o.k56_failed, 0); CHECK_EQ_I(o.k56_lost, 0); CHECK_EQ_I(o.connected_once, 1);

    /* the client silent after its renegotiation's S: the call ends within
     * 10 s, as K56flex's (V90M_END_K56_FAILED) */
    kd_default(&ko); ko.rate = 1; ko.reneg_ms = 1000; ko.reneg_rate = 1; ko.silent = K56C_SIL_RENEG;
    kd_call(&ko, 60, &o);
    printf("  K56flex client silent after its renegotiation's S: ended %.2f s later, error %d, k56_failed %d, lost %d\n",
           (o.end_at - o.quiet_at) / 8000.0, o.error, o.k56_failed, o.k56_lost);
    CHECK_EQ_I(K.renegs, 1); CHECK(o.quiet_at > 0); CHECK(o.end_at > o.quiet_at);
    CHECK(o.end_at - o.quiet_at <= 8000L * 10);
    CHECK_EQ_I(o.phase, ANS_FAILED); CHECK_EQ_I(o.error, K56D_ERR_RENEG);
    CHECK_EQ_I(o.k56_failed, 1); CHECK_EQ_I(o.k56_lost, 0); CHECK_EQ_I(o.cleared, 0); CHECK_EQ_I(o.connected_once, 1);

    /* the upstream whole across its renegotiation: counter words. Its S
     * after its mapping frame (1000 ms into data mode: 3200 symbols) loses
     * exactly the one frame k56d keeps as a margin against an S found late
     * (v34_rx_flush_before); its S 3 symbols into a frame (1001 ms) loses
     * that one too. Nothing repeated or wrong either way. */
    for (j = 0; j < 2; j++) {
        int64_t frame = 31200 * 8 / 3200, gap;
        kd_default(&ko); ko.reneg_ms = j ? 1001 : 1000; ko.reneg_rate = 10;
        KW = 1; memset(&KWU, 0, sizeof KWU); KWU.gen = 0x2c00;
        kd_call(&ko, 60, &o);
        KW = 0;
        gap = KWU.gap_bits;
        printf("  K56flex client renegotiates, its S %s, counter words up: %ld words, lost %" PRId64 ", again %" PRId64
               "; breaks %" PRId64 " (%" PRId64 " bits missing, the mapping frame %" PRId64 ")\n",
               j ? "3 symbols into a mapping frame" : "after its mapping frame", (long)KWU.words, KWU.lost, KWU.again,
               KWU.breaks, gap, frame);
        CHECK_EQ_I(K.renegs, 1); CHECK_EQ_I(o.nmove, 2);
        CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA);
        CHECK(KWU.words > DBITS / 16);
        CHECK_EQ_I(KWU.again, 0);
        CHECK_EQ_I(KWU.breaks, 1);
        if (!j) CHECK(gap >= frame - 1 && gap <= frame);
        else CHECK(gap >= frame - 1 && gap <= 2 * frame);
        CHECK(16 * KWU.lost >= gap && 16 * KWU.lost < gap + 32);
        CHECK_EQ_I(KWU.locked, 1);
    }

    /* its S near the start of its data (Task 10d), counter words up, its S
     * after a mapping frame (ms * 3.2 symbols, a multiple of 8): no bit
     * delivered that it did not send as data (nothing inserted, the first
     * lock within a word of the start), nothing repeated, data again. The
     * plain client 20 .. 70 ms into its data (from its B1's first symbol: B1
     * is 30 ms; ours is not out until some 67 ms after its E): up to 50 ms
     * its S is heard before its B1 is decided (some 54 ms after its E) and
     * nothing before it is data; from 55 ms what it sent before S comes but
     * for the margin frame. The client ignoring our MP 60 .. 150 ms in,
     * while the stored points are caught up with (about 110 ms from the
     * retry): the margin frame lost, as without the catch-up */
    {
        static const struct { int ms, heavy, kept; } st[] = {
            { 20, 0, 0 }, { 40, 0, 0 }, { 50, 0, 0 }, { 55, 0, 1 }, { 60, 0, 1 }, { 70, 0, 1 },
            { 60, 1, 1 }, { 70, 1, 1 }, { 100, 1, 1 }, { 150, 1, 1 }
        };
        for (j = 0; j < (int)(sizeof st / sizeof st[0]); j++) {
            int64_t frame = 31200 * 8 / 3200, gap;
            kd_default(&ko); ko.reneg_ms = st[j].ms; ko.reneg_rate = 10;
            ko.heavy_up = st[j].heavy ? K56C_HEAVY_ALWAYS : K56C_HEAVY_NONE;
            KW = 1; memset(&KWU, 0, sizeof KWU); KWU.gen = 0x2c00;
            kd_call(&ko, 60, &o);
            KW = 0;
            gap = KWU.gap_bits;
            printf("  K56flex client%s, its S %d ms into its data, counter words up: %ld words, lost %" PRId64
                   ", again %" PRId64 "; breaks %" PRId64 " (%" PRId64 " bits missing), %" PRId64 " bits before the first\n",
                   st[j].heavy ? " ignoring our MP" : "", st[j].ms, (long)KWU.words, KWU.lost, KWU.again,
                   KWU.breaks, gap, KWU.lead);
            CHECK_EQ_I(K.renegs, 1); CHECK(o.nmove >= 1); CHECK(o.move_at > o.rn_at);
            CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK_EQ_I(o.error, 0);
            CHECK(KWU.words > DBITS / 16);
            CHECK_EQ_I(KWU.again, 0);
            CHECK(KWU.lead >= 0 && KWU.lead < 16); CHECK_EQ_I(KWU.lead_tail, 1);
            CHECK_EQ_I(KWU.locked, 1);
            if (st[j].kept) {
                CHECK_EQ_I(KWU.breaks, 1);
                CHECK(gap >= frame - 1 && gap <= frame);
                CHECK(16 * KWU.lost >= gap && 16 * KWU.lost < gap + 32);
            } else {
                CHECK_EQ_I(KWU.breaks, 0); CHECK_EQ_I(KWU.lost, 0);
            }
        }
    }

    /* the limit (docs/limits.md): the client ignoring our MP, its S 45 ms
     * into its data, before its B1 under our MP's configuration is decided:
     * followed, nothing before it is data, nothing inserted or repeated; the
     * first frames after it may have errors (the equaliser pulled by B1
     * under the wrong configuration): words damaged, printed */
    {
        kd_default(&ko); ko.reneg_ms = 45; ko.reneg_rate = 10; ko.heavy_up = K56C_HEAVY_ALWAYS;
        KW = 1; memset(&KWU, 0, sizeof KWU); KWU.gen = 0x2c00;
        kd_call(&ko, 60, &o);
        KW = 0;
        printf("  K56flex client ignoring our MP, its S 45 ms into its data: %ld words, %" PRId64 " damaged, again %" PRId64
               ", %" PRId64 " bits missing\n", (long)KWU.words, KWU.lost, KWU.again, KWU.gap_bits);
        CHECK_EQ_I(K.renegs, 1); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK_EQ_I(o.error, 0);
        CHECK_EQ_I(KWU.again, 0); CHECK_EQ_I(KWU.gap_bits, 0); CHECK_EQ_I(KWU.lead_tail, 1);
    }

    /* a client that ignores our MP renegotiates (13 -> 10): its data after
     * it with x2's upstream again (the receiver starts from the hypothesis
     * that passed: no retry), or with what our MP asks this time (x2's
     * fails B1, the retry goes back to ours). The data check each way in
     * both data segments */
    for (j = 0; j < 2; j++) {
        kd_default(&ko); ko.heavy_up = j ? K56C_HEAVY_FIRST : K56C_HEAVY_ALWAYS;
        ko.reneg_ms = RN_MS; ko.reneg_rate = 10;
        kd_call(&ko, 60, &o);
        snprintf(what, sizeof what, "client ignoring our MP %s, a renegotiation 13 -> 10",
                 j ? "until its renegotiation" : "throughout");
        kd_report(what, &o);
        printf("     upstream hypothesis %d after it, retried %d\n", A.u.k56.up_hyp, A.u.k56.up_retried);
        CHECK_EQ_I(K.renegs, 1); CHECK_EQ_I(o.nseg, 2); CHECK_EQ_I(o.nmove, 2);
        for (k = 0; k < 2; k++) {
            CHECK(o.seg[k].dn >= DBITS); CHECK(o.seg[k].un >= DBITS);
            CHECK_EQ_I(o.seg[k].dbad, 0); CHECK_EQ_I(o.seg[k].ubad, 0); CHECK(o.seg[k].balanced);
        }
        CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA);
        CHECK_EQ_I(A.u.k56.up_hyp, j ? 0 : 1); CHECK_EQ_I(A.u.k56.up_retried, j);
        CHECK_EQ_I(o.down, 50000); CHECK_EQ_I(o.up, 31200); CHECK_EQ_I(o.error, 0);
    }

    /* the client gives up its renegotiation halfway through its TRN and
     * retrains (R-3): its tone B in K56D_RENEG is followed as in data mode
     * (K56D_RETRAIN, ANS_K56RT), the training again, data again */
    kd_default(&ko); ko.reneg_ms = RN_MS; ko.reneg_rate = 10; ko.retrain_in_reneg = 1;
    kd_call(&ko, 60, &o);
    kd_report("K56flex client, a retrain in its renegotiation", &o);
    printf("     ANS_K56RT %.3f s after its retrain began, data again %.2f s after it\n",
           (o.k56rt_at - o.rt_at) / 8000.0, (o.move_at - o.rt_at) / 8000.0);
    CHECK_EQ_I(K.renegs, 1); CHECK_EQ_I(K.retrains, 1); CHECK_EQ_I(A.retrains, 1); CHECK_EQ_I(K.k56_starts, 2);
    CHECK(o.k56rt_at > o.rt_at); CHECK(o.k56rt_at - o.rt_at < 8000L / 4);
    CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK(answer_moving(&A));
    CHECK_EQ_I(o.nmove, 2); CHECK_EQ_I(o.nseg, 3);
    CHECK(o.seg[2].dn >= DBITS); CHECK(o.seg[2].un >= DBITS);
    CHECK_EQ_I(o.seg[2].dbad, 0); CHECK_EQ_I(o.seg[2].ubad, 0); CHECK(o.seg[2].balanced);
    CHECK_EQ_I(o.down, 56000); CHECK_EQ_I(o.k56_failed, 0); CHECK_EQ_I(o.error, 0);

    /* we never start one: HOLD_S of data mode without the client's, no R in
     * our data (the client finds none), data on all of it */
    kd_default(&ko); ko.hold = HOLD_S;
    kd_call(&ko, 30 + HOLD_S, &o);
    kd_report("K56flex client, no renegotiation", &o);
    printf("     %d s of data mode, R in our data %ld times\n", HOLD_S, K.r_unasked);
    CHECK_EQ_I(o.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK(o.data_at > 0);
    CHECK_EQ_I(K.renegs, 0); CHECK_EQ_I(K.r_unasked, 0); CHECK_EQ_I(A.u.k56.tx.reneg, 0);
    CHECK(K.data_frames >= (long)HOLD_S * 1000 - 100); CHECK_EQ_I(K.data_lost, 0);
    CHECK(DA.n >= (long)HOLD_S * 31200 - 31200); CHECK(DC.n >= (long)HOLD_S * 56000 - 56000);
    CHECK_EQ_I(DA.bad, 0); CHECK_EQ_I(DC.bad, 0);
}

/* ---- the client's retrain (plan Task 12) ---- */

/* one retrain RN_MS into data mode (r = 13, pad 0; robbed bits none and
 * 0x24): its silence and tone B, our ANS_K56RT at once, Phase 2 again with
 * our INFO1a flagged, the whole training (checked by the client octet by
 * octet as the first), data again; the data check before and after, each
 * way; retraining 1 from our ANS_K56RT to data again, 0 after; we never
 * retrain (answer_retrain -1 in ANS_K56RT) */
static void kt_one(int rbs)
{
    kd_opts ko; kd_out o;
    char what[96];
    int k;
    kd_default(&ko); ko.rbs = rbs; ko.retrain_ms = RN_MS; ko.retrains = 1;
    kd_call(&ko, 60, &o);
    snprintf(what, sizeof what, "K56flex client, a retrain %d ms into data mode, robbed %02X", RN_MS, rbs);
    kd_report(what, &o);
    printf("     ANS_K56RT %.3f s after its retrain began, %.2f s in it, data again %.2f s after it"
           " (the first training: %.2f s from ANS_K56); answer_retrain %d\n", (o.k56rt_at - o.rt_at) / 8000.0,
           o.rt_in / 8000.0, (o.move_at - o.rt_at) / 8000.0, (o.data_at - o.k56_at) / 8000.0, o.rt_ret);
    for (k = 0; k < o.nseg && k < KD_SEGS; k++)
        printf("     segment %d: down %ld bits %ld bad, up %ld bits %ld bad\n", k, o.seg[k].dn, o.seg[k].dbad,
               o.seg[k].un, o.seg[k].ubad);
    CHECK_EQ_I(K.retrains, 1); CHECK_EQ_I(A.retrains, 1); CHECK_EQ_I(K.renegs, 0);
    CHECK_EQ_I(K.k56_starts, 2); CHECK_EQ_I(A.v34.p2.k56, 1); CHECK_EQ_I(K.peer.p2.info1a.freq_off, 1);
    CHECK(o.rt_at > 0); CHECK(o.k56rt_at > o.rt_at); CHECK(o.k56rt_at - o.rt_at < 8000L / 4);
    CHECK(o.rt_in > 0); CHECK_EQ_I(o.rt_ret, -1); CHECK_EQ_I(o.rt_not_retraining, 0);
    /* Phase 2 of the retrain, then a training as long as the first */
    CHECK(o.move_at > o.rt_at); CHECK(o.move_at - o.rt_at - o.rt_in < o.data_at - o.k56_at + 8000L);
    CHECK_EQ_I(o.nseg, 2); CHECK_EQ_I(o.nmove, 2);
    CHECK_EQ_I(o.seg_bps[0], 56000); CHECK_EQ_I(o.seg_bps[1], 56000);
    for (k = 0; k < 2; k++) {
        CHECK(o.seg[k].dn >= DBITS); CHECK(o.seg[k].un >= DBITS);
        CHECK_EQ_I(o.seg[k].dbad, 0); CHECK_EQ_I(o.seg[k].ubad, 0); CHECK(o.seg[k].balanced);
    }
    CHECK_EQ_I(seg_bad_sum(), 0);
    CHECK_EQ_I(K.mp_good, 2 * 16); CHECK_EQ_I(K.mpa_good, 2 * 16); CHECK_EQ_I(K.mp_wrong, 0);
    CHECK_EQ_I(K.e_seen, 1); CHECK_EQ_I(K.b1_bad, 0); CHECK_EQ_I(K.data_lost, 0); CHECK_EQ_I(K.r_unasked, 0);
    CHECK(K.data_at > K.trn1b_at); CHECK_EQ_I((K.data_at - K.trn1b_at) % 24, 0);
    CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK(answer_moving(&A));
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 0);
    CHECK_EQ_I(o.down, 56000); CHECK_EQ_I(o.up, 31200); CHECK_EQ_I(o.client_down, 56000); CHECK_EQ_I(o.client_up, 31200);
    CHECK_EQ_I(A.u.k56.tx.d_rate, 13); CHECK_EQ_I(o.k56_failed, 0); CHECK_EQ_I(o.error, 0);
}

static void k56_retrain_tests(void)
{
#ifndef TEST_SHORT
    kd_opts ko; kd_out o;
    int k;
#endif
    kt_one(0);
#ifndef TEST_SHORT
    kt_one(0x24);

    /* three retrains in a row, 300 ms of data mode between them (no data to
     * speak of): the third ends the call as K56flex's (V90M_END_K56_FAILED) */
    kd_default(&ko); ko.retrain_ms = 300; ko.retrains = 3;
    kd_call(&ko, 90, &o);
    printf("  K56flex client, three retrains 300 ms into data mode each: %d begun (ours %d), data again %d times,"
           " ended %.3f s after the third, k56_failed %d\n", K.retrains, A.retrains, o.nmove,
           (o.end_at - o.rt_at) / 8000.0, o.k56_failed);
    CHECK_EQ_I(K.retrains, 3); CHECK_EQ_I(A.retrains, 3); CHECK_EQ_I(o.nmove, 3);
    CHECK_EQ_I(o.phase, ANS_FAILED); CHECK(o.end_at > o.rt_at); CHECK(o.end_at - o.rt_at < 8000L / 4);
    CHECK_EQ_I(o.k56_failed, 1); CHECK_EQ_I(o.k56_lost, 0); CHECK_EQ_I(o.cleared, 0); CHECK_EQ_I(o.connected_once, 1);

    /* three separated by data (RN_MS of it before each): the call goes on */
    kd_default(&ko); ko.retrain_ms = RN_MS; ko.retrains = 3;
    kd_call(&ko, 120, &o);
    kd_report("K56flex client, three retrains with data between", &o);
    for (k = 0; k < o.nseg && k < KD_SEGS; k++)
        printf("     segment %d: down %ld bits %ld bad, up %ld bits %ld bad\n", k, o.seg[k].dn, o.seg[k].dbad,
               o.seg[k].un, o.seg[k].ubad);
    CHECK_EQ_I(K.retrains, 3); CHECK_EQ_I(A.retrains, 3); CHECK_EQ_I(K.k56_starts, 4);
    CHECK_EQ_I(o.nseg, 4); CHECK_EQ_I(o.nmove, 4); CHECK_EQ_I(o.rt_not_retraining, 0);
    for (k = 0; k < KD_SEGS; k++) {
        CHECK(o.seg[k].dn >= DBITS); CHECK(o.seg[k].un >= DBITS);
        CHECK_EQ_I(o.seg[k].dbad, 0); CHECK_EQ_I(o.seg[k].ubad, 0); CHECK(o.seg[k].balanced);
    }
    CHECK_EQ_I(seg_bad_sum(), 0); CHECK_EQ_I(K.mp_good, 4 * 16); CHECK_EQ_I(K.mp_wrong, 0);
    CHECK_EQ_I(A.phase, ANS_K56); CHECK_EQ_I(A.u.k56.state, K56D_DATA); CHECK_EQ_I(A.retraining, 0);
    CHECK_EQ_I(o.k56_failed, 0);

    /* a retrain whose INFO1c comes back 0x200: our INFO1a unflagged, and no
     * way back to V.34 (V90M_END_K56_FAILED) */
    kd_default(&ko); ko.retrain_ms = 1000; ko.retrains = 1; ko.retrain_200 = 1;
    kd_call(&ko, 60, &o);
    printf("  K56flex client, a retrain with INFO1c 0x200: INFO1a offset %d, ended %.2f s after it began,"
           " k56_failed %d\n", (int)K.peer.p2.info1a.freq_off, (o.end_at - o.rt_at) / 8000.0, o.k56_failed);
    CHECK_EQ_I(K.retrains, 1); CHECK_EQ_I(A.retrains, 1); CHECK_EQ_I(A.v34.p2.k56, 0);
    CHECK(K.peer.p2.info1a.freq_off != 1);
    CHECK_EQ_I(o.phase, ANS_FAILED); CHECK(o.end_at > o.rt_at); CHECK(o.end_at - o.rt_at < 8000L * 10);
    CHECK_EQ_I(o.k56_failed, 1); CHECK_EQ_I(o.k56_lost, 0); CHECK_EQ_I(o.cleared, 0);

    /* the client silent after 200 ms of its retrain's tone B: our Phase 2
     * of the retrain hears nothing more; the call ends within 10 s of its
     * tone B, as K56flex's (V90M_END_K56_FAILED) */
    kd_default(&ko); ko.retrain_ms = 1000; ko.retrains = 1; ko.silent = K56C_SIL_RETRAIN;
    kd_call(&ko, 60, &o);
    printf("  K56flex client silent after its retrain's tone B: ended %.2f s after its tone B began,"
           " error %d, k56_failed %d, lost %d\n", (o.end_at - o.rt_at - 560) / 8000.0, o.error, o.k56_failed,
           o.k56_lost);
    CHECK_EQ_I(K.retrains, 1); CHECK_EQ_I(A.retrains, 1);
    CHECK(o.k56rt_at > o.rt_at); CHECK(o.quiet_at > 0);
    CHECK_EQ_I(o.phase, ANS_FAILED); CHECK(o.end_at > o.quiet_at);
    CHECK(o.end_at - (o.rt_at + 560) <= 8000L * 10);
    CHECK_EQ_I(o.k56_failed, 1); CHECK_EQ_I(o.k56_lost, 0); CHECK_EQ_I(o.cleared, 0); CHECK_EQ_I(o.connected_once, 1);
#endif
}

/* ---- a caller without V.8bis: sim/v34peer over test/line34.h ---- */

static v34peer_t C; static line34_t L;
static int16_t a_tick(void *u, int16_t rx)
{
    int16_t t = answer_tick((answer_t *)u, rx);
    p1_watch(L.n);
    if (ntx < TXN) txs[slot][ntx++] = t;
    return t;
}
static int16_t c_tick(void *u, int16_t rx) { return v34peer_tick((v34peer_t *)u, rx); }

typedef struct { int ok, tx, rx, sym; long bad, ansam_at; } v34_outcome;

static void v34_call(unsigned modes, int law, long data_bits, v34_outcome *o)
{
    answer_cfg ac; v34peer_cfg pc; line34_cfg lc;
    long end;
    memset(o, 0, sizeof *o);
    memset(&lc, 0, sizeof lc);
    lc.law = law; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
    memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    v34peer_default(&pc, 3000);
    pc.dx.getbit = p_get; pc.dx.putbit = p_put; pc.dx.user = &UC;
    answer_init(&A, &ac);
    if (law == 2) { p2d_cfg p = A.p2cfg; p.alaw = 1; answer_v90_cfg(&A, &p); }
    answer_v90_line(&A, 0, 0, 0);
    v34peer_init(&C, &pc); line34_init(&L, &lc);
    ntx = 0; p1_reset();
    end = 8000L * 25;
    while (L.n < end) {
        (void)line34_step(&L, a_tick, &A, c_tick, &C);
        if (A.connected && C.state == VP_DATA) break;
        if (A.phase == ANS_FAILED || C.state == VP_ENDED) break;
    }
    o->ansam_at = P1.ansam_at;
    o->ok = A.connected && C.state == VP_DATA;
    if (!o->ok || !data_bits) return;
    o->tx = A.bitrate; o->rx = A.rx_bitrate; o->sym = A.v34.dx.cfg.sym_rate;
    end = L.n + 8000;
    while (L.n < end) (void)line34_step(&L, a_tick, &A, c_tick, &C);
    UA.n = UA.bad = UC.n = UC.bad = 0;
    end = L.n + 8000L * 10;
    while (L.n < end && (UA.n < data_bits || UC.n < data_bits)) (void)line34_step(&L, a_tick, &A, c_tick, &C);
    o->bad = UA.bad + UC.bad;
    if (UA.n < data_bits || UC.n < data_bits) o->bad = -1;
}

static int same_tx(long n)
{
    long k;
    for (k = 0; k < n; k++) if (txs[0][k] != txs[1][k]) return 0;
    return 1;
}

static void v34peer_tests(void)
{
    v34_outcome o0, o1;
    long n0;
    slot = 0; v34_call(BASE, 1, BITS, &o0);
    slot = 1; v34_call(BASE | ANS_MODE_K56, 1, BITS, &o1);
    printf("  V.34 caller without V.8bis, K56flex offered: %d / %d bit/s at %d baud, %ld bad (without: %d / %d at %d, %ld bad);"
           " ANSam %.3f s later\n", o1.tx, o1.rx, o1.sym, o1.bad, o0.tx, o0.rx, o0.sym, o0.bad, (o1.ansam_at - o0.ansam_at) / 8000.0);
    CHECK(o0.ok); CHECK(o1.ok);
    CHECK_EQ_I(A.k56_offered, 1); CHECK_EQ_I(A.k56_agreed, 0);
    CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    CHECK_EQ_I(o1.tx, o0.tx); CHECK_EQ_I(o1.rx, o0.rx); CHECK_EQ_I(o1.sym, o0.sym);
    CHECK_EQ_I(o0.bad, 0); CHECK_EQ_I(o1.bad, 0);
    CHECK_EQ_I(o1.ansam_at - o0.ansam_at, 8L * (V8B_SIL_MS + V8B_CRE_DUAL_MS + V8B_CRE_TONE_MS + V8B_CRD_DEADLINE_MS));

    /* an A-law line: K56flex is not offered at all, not a sample differs */
    slot = 0; v34_call(BASE, 2, 0, &o0); n0 = ntx;
    slot = 1; v34_call(BASE | ANS_MODE_K56, 2, 0, &o1);
    printf("  A-law V.34 caller, K56flex offered: %ld samples sent, %s\n", ntx,
           ntx == n0 && same_tx(ntx) ? "all as without K56flex" : "SOME DIFFER");
    CHECK(o0.ok); CHECK(o1.ok);
    CHECK_EQ_I(A.k56_offered, 0);
    CHECK_EQ_I(ntx, n0);
    CHECK(same_tx(ntx < n0 ? ntx : n0));
}

#ifndef TEST_SHORT
/* ---- the library's calling modem in V.90 and V.92 (test/v90link.h) ---- */

static vl_t VL;

/* The reference is the answerer without K56flex started as late as V.8
 * starts when K56flex is offered to a caller without V.8bis (silent until
 * then), so that it meets the same line from there on: then what it sends
 * from there on is the same, octet for octet (hashed over 12 s). */
#define LATE (8L * (V8B_SIL_MS + V8B_CRE_DUAL_MS + V8B_CRE_TONE_MS + V8B_CRD_DEADLINE_MS))
static long dig_n; static int dig_late; static uint32_t dig_hash;
static uint8_t dig_answer(void *u, uint8_t in)
{
    vl_t *l = (vl_t *)u;
    long n = dig_n++;
    uint8_t out = dig_late && n < LATE ? 0xff : answer_tick_pcm(&l->ans, in);
    if (n >= LATE && n < LATE + 8000L * 12) dig_hash = (dig_hash ^ out) * 16777619u;
    return out;
}

typedef struct { int ok, mod, down, up; int64_t bd, bu, fd, fu; uint32_t hash; } vl_outcome;

static void orig_call(int v92, int k56, vl_outcome *o)
{
    vl_cfg c;
    vl_t *l = &VL;
    int64_t d0, u0, f0d, f0u, e;
    memset(o, 0, sizeof *o);
    memset(&c, 0, sizeof c);
    c.loop_km = 2; c.noise_dbm0 = -75; c.echo_db = -16;
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.up_line = 1; c.full = 1; c.caller_orig = 1; c.seed = 20;
    c.v92 = v92; c.caller_v92 = v92;
    vl_init(l, &c);
    if (k56) {
        /* vl_init's answerer, with K56flex offered too */
        answer_cfg ac;
        ac = l->ans.cfg;
        ac.modes |= ANS_MODE_K56;
        answer_init(&l->ans, &ac);
        answer_v90_line(&l->ans, -1, 0, 0);
        CHECK_EQ_I(l->ans.phase, ANS_V8BIS);
    }
    dig_n = 0; dig_late = !k56; dig_hash = 2166136261u;
    l->dig = dig_answer; l->dig_user = l;
    while (l->k < MS(40000) && !orig_moving(&l->org) && l->org.phase != ORIG_FAILED) vl_tick(l);
    if (!orig_moving(&l->org)) return;
    o->mod = l->org.modulation; o->down = l->org.rx_bitrate; o->up = l->org.bitrate;
    while (l->k < MS(42000) && !answer_moving(&l->ans)) vl_tick(l);
    e = l->k + MS(300); while (l->k < e) vl_tick(l);
    d0 = l->down.got; u0 = l->up.got; f0d = l->down.fails; f0u = l->up.fails;
    e = l->k + MS(10000);
    while (l->k < e && l->org.phase != ORIG_FAILED && (l->down.got - d0 < BITS || l->up.got - u0 < BITS)) vl_tick(l);
    o->bd = l->down.got - d0; o->bu = l->up.got - u0;
    o->fd = l->down.fails - f0d; o->fu = l->up.fails - f0u;
    o->ok = orig_moving(&l->org) && answer_moving(&l->ans);
    o->hash = dig_hash;
}

static void orig_tests(void)
{
    int v92;
    for (v92 = 0; v92 < 2; v92++) {
        vl_outcome o0, o1;
        orig_call(v92, 0, &o0);
        orig_call(v92, 1, &o1);
        printf("  our calling modem in %s, K56flex offered: mod %d, %d / %d bit/s, %" PRId64 " / %" PRId64 " bits,"
               " %" PRId64 " / %" PRId64 " fail (without: mod %d, %d / %d)\n", v92 ? "V.92" : "V.90", o1.mod, o1.down, o1.up,
               o1.bd, o1.bu, o1.fd, o1.fu, o0.mod, o0.down, o0.up);
        CHECK(o0.ok); CHECK(o1.ok);
        CHECK_EQ_I(VL.ans.k56_offered, 1); CHECK_EQ_I(VL.ans.k56_agreed, 0);
        CHECK_EQ_I(o1.mod, v92 ? ORIG_MOD_V92 : ORIG_MOD_V90); CHECK_EQ_I(o1.mod, o0.mod);
        CHECK_EQ_I(o1.down, o0.down); CHECK_EQ_I(o1.up, o0.up);
        CHECK_EQ_I(o1.hash, o0.hash);
        CHECK(o1.bd >= BITS); CHECK(o1.bu >= BITS);
        CHECK_EQ_I(o1.fd, 0); CHECK_EQ_I(o1.fu, 0);
    }
}

/* ---- the x2 client with x2 and K56flex offered ---- */

typedef struct { uint32_t tx, rx; long n, bad; } rbits_t;
static int rnext(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return (int)(*s & 1); }
static void r_check(rbits_t *r, int bit) { if (rnext(&r->rx) != (bit & 1)) r->bad++; r->n++; }
static x2call_t X; static rbits_t DN, UP;
static int s_get(void *u) { (void)u; return rnext(&DN.tx); }
static void s_put(void *u, int bit) { (void)u; r_check(&UP, bit); }
static int c_get(void *u) { (void)u; return rnext(&UP.tx); }
static void c_put(void *u, int bit) { (void)u; r_check(&DN, bit); }

typedef struct { int ok, mod, down, up; } x2_outcome;

static void x2_call(unsigned modes, x2_outcome *o)
{
    static uint8_t down[XD]; static int16_t up[XD]; static double dl[XD];
    const int d_ac = 37, d_ca = 41;
    answer_cfg ac; x2call_cfg xc; rng_t rng;
    uint8_t arx = 0xff;
    long n;
    memset(o, 0, sizeof *o);
    memset(down, 0x7f, sizeof down); memset(up, 0, sizeof up); memset(dl, 0, sizeof dl);
    memset(&DN, 0, sizeof DN); memset(&UP, 0, sizeof UP);
    DN.tx = DN.rx = 0x2468ace1u; UP.tx = UP.rx = 0x13579bdfu;
    rng_seed(&rng, 4242);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = s_get; ac.putbit = s_put; ac.user = NULL;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, 0, 0);
    memset(&xc, 0, sizeof xc);
    xc.level = 3000; xc.max_index = 1; xc.up_mask = 0x03FE;
    xc.getbit = c_get; xc.putbit = c_put; xc.user = NULL;
    x2call_init(&X, &xc);
    for (n = 0; n < 8000L * 45; n++) {
        int i = (int)(n & (XD - 1));
        double v;
        uint8_t atx = answer_tick_pcm(&A, arx);
        down[i] = atx;
        dl[i] = (double)ulaw_to_linear(atx);
        up[i] = x2call_tick(&X, down[(n - d_ac) & (XD - 1)]);
        v = 0.5 * up[(n - d_ca) & (XD - 1)]
            + 0.12 * dl[(n - 2) & (XD - 1)] + 0.08 * dl[(n - 4) & (XD - 1)] - 0.05 * dl[(n - 9) & (XD - 1)]
            + 2.0 * rng_gauss(&rng);
        arx = linear_to_ulaw((int32_t)lround(v));
        if (A.phase == ANS_FAILED) break;
        if (A.connected && x2call_state(&X) == X2C_DATA && DN.n >= BITS && UP.n >= BITS) break;
    }
    o->ok = A.connected && A.phase == ANS_X2 && x2call_state(&X) == X2C_DATA;
    o->mod = A.modulation; o->down = A.bitrate; o->up = A.rx_bitrate;
}

static void x2_tests(void)
{
    x2_outcome o0, o1;
    long b0;
    x2_call(BASE | ANS_MODE_X2, &o0);
    b0 = DN.bad + UP.bad;
    x2_call(BASE | ANS_MODE_X2 | ANS_MODE_K56, &o1);
    printf("  x2 client, x2 and K56flex offered: %s %d / %d bit/s, down %ld bits %ld bad, up %ld bits %ld bad"
           " (without K56flex: %d / %d)\n", o1.ok ? "x2" : "NO DATA", o1.down, o1.up, DN.n, DN.bad, UP.n, UP.bad, o0.down, o0.up);
    CHECK(o0.ok); CHECK(o1.ok); CHECK_EQ_I(b0, 0);
    CHECK_EQ_I(A.k56_offered, 1); CHECK_EQ_I(A.k56_agreed, 0);
    CHECK_EQ_I(o1.mod, ANS_MOD_X2); CHECK_EQ_I(o1.down, o0.down); CHECK_EQ_I(o1.up, o0.up);
    CHECK(DN.n >= BITS); CHECK(UP.n >= BITS); CHECK_EQ_I(DN.bad, 0); CHECK_EQ_I(UP.bad, 0);
}

#endif

/* answer_v90_cfg after the first sample changes nothing: neither what is
 * offered nor the phase (it re-ran the offers: K56flex's flag overwritten,
 * x2 withdrawn, and V.8bis begun again when called as V.8 follows it) */
static void cfg_after_start_tests(void)
{
    answer_cfg ac;
    p2d_cfg pc;
    long n;
    int k56, x2, ph;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = BASE | ANS_MODE_K56 | ANS_MODE_X2; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    /* before the first sample: the law decides the offers */
    answer_init(&A, &ac);
    CHECK_EQ_I(A.k56_offered, 1); CHECK_EQ_I(A.x2_offered, 1); CHECK_EQ_I(A.phase, ANS_V8BIS);
    pc = A.p2cfg; pc.alaw = 1; answer_v90_cfg(&A, &pc);
    CHECK_EQ_I(A.k56_offered, 0); CHECK_EQ_I(A.x2_offered, 0); CHECK_EQ_I(A.phase, ANS_V8);
    /* after it: nothing changes */
    answer_init(&A, &ac);
    (void)answer_tick(&A, 0);
    k56 = A.k56_offered; x2 = A.x2_offered; ph = A.phase;
    pc = A.p2cfg; pc.alaw = 1; answer_v90_cfg(&A, &pc);
    CHECK_EQ_I(A.k56_offered, k56); CHECK_EQ_I(A.x2_offered, x2); CHECK_EQ_I(A.phase, ph);
    CHECK_EQ_I(A.p2cfg.alaw, 0);
    /* nor at the sample V.8 follows V.8bis (its clock starts at 0 again) */
    answer_init(&A, &ac);
    for (n = 0; n < MS(20000) && A.phase == ANS_V8BIS; n++) (void)answer_tick(&A, 0);
    CHECK_EQ_I(A.phase, ANS_V8);
    pc = A.p2cfg; answer_v90_cfg(&A, &pc);
    CHECK_EQ_I(A.phase, ANS_V8);
    CHECK_EQ_I(A.k56_offered, 1);
    printf("  answer_v90_cfg after the first sample: offers and phase kept (V.8 after V.8bis at %.2f s)\n", n / 8000.0);
}

/* the client behind an ATA or a VoIP gateway (ISP calls of 2026-11-23: a
 * Cisco IOS FXS port's hybrid at 302 ms, 19.5 dB down): our echo a round
 * trip late, measured on our TRN1A. k56d finds it near the round trip of
 * Phase 2 and cancels it: the upstream as clean as on a short path. And a
 * round trip of 100 ms, inside the search as it always was. */
static void k56_far_echo_tests(void)
{
    static const int ms[3] = { 300, 400, 100 };
    kd_opts ko; kd_out o;
    int k;
    for (k = 0; k < 3; k++) {
        char what[64];
        int e;
#ifdef TEST_SHORT
        if (k > 0) break;
#endif
        kd_default(&ko); ko.rate = 1; ko.echo_delay = -1;
        KFAR.d = ms[k] * 4; e = 2 * KFAR.d + 2;    /* (the echo path's first tap) */
        kd_call(&ko, 60, &o);
        snprintf(what, sizeof what, "K56flex, far echo %d ms, 20 dB down", ms[k]);
        kd_report(what, &o);
        printf("     echo delay %d (the echo at %d samples), round trip of Phase 2 %d, upstream SNR %.1f dB\n",
               A.u.k56.ecd.delay, e, answer_rtd(&A), answer_snr_db10(&A) / 10.0);
        CHECK_EQ_I(o.connected_once, 1); CHECK_EQ_I(o.mod, ANS_MOD_K56);
        CHECK(A.u.k56.ecd.delay >= e - 12 && A.u.k56.ecd.delay <= e);
        CHECK(answer_snr_db10(&A) > 300);              /* upstream error below -30 dB */
        CHECK(DA.n >= DBITS); CHECK(DC.n >= DBITS); CHECK_EQ_I(DA.bad, 0); CHECK_EQ_I(DC.bad, 0);
    }
}

int main(void)
{
    cfg_after_start_tests();
    k56_client_tests();
    k56_data_tests();
    k56_rate_tests();
    k56_reneg_tests();
    k56_retrain_tests();
    v34peer_tests();
    k56_far_echo_tests();
#ifndef TEST_SHORT          /* on ARM under qemu (test.h): the K56flex client and v34peer only */
    orig_tests();
    x2_tests();
#endif
    T_REPORT("answer_k56");
}
