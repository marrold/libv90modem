/* The answering modem (modem/answer) with a V.34 caller: V.8 selects V.34
 * when it is offered, the call reaches data mode and carries data both
 * ways, the rate ceilings hold, our cleardown ends it; and a caller that
 * only has V.34 is turned away when V.34 is not offered; after a click on
 * the line in data mode (Task 20) the upstream is clean from 10 s on. The
 * caller is sim/v34peer over test/line34.h (mu-law codec at our side). */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../sim/v34peer.h"
#include "line34.h"

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

static answer_t A; static v34peer_t C; static line34_t L; static prbs_t UA, UC;

static int16_t a_tick(void *u, int16_t rx) { return answer_tick((answer_t *)u, rx); }
static int16_t c_tick(void *u, int16_t rx) { return v34peer_tick((v34peer_t *)u, rx); }

static void start(unsigned modes, int max_down, int max_up)
{
    answer_cfg ac; v34peer_cfg pc; line34_cfg lc;
    memset(&lc, 0, sizeof lc);
    lc.law = 1; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
    memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    v34peer_default(&pc, 3000);
    pc.dx.getbit = p_get; pc.dx.putbit = p_put; pc.dx.user = &UC;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, max_down, max_up);
    v34peer_init(&C, &pc); line34_init(&L, &lc);
}

static void run(long samples) { while (samples-- > 0) (void)line34_step(&L, a_tick, &A, c_tick, &C); }

static int to_data(int max_ms)
{
    long end = L.n + 8L * max_ms;
    while (L.n < end) {
        run(8);
        if (A.connected && C.state == VP_DATA) return 1;
        if (A.phase == ANS_FAILED || C.state == VP_ENDED) return 0;
    }
    return 0;
}

int main(void)
{
    long t;

    /* everything offered: a V.34-only caller gets V.34 */
    start(ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21, 0, 0);
    CHECK_EQ_I(to_data(25000), 1);
    CHECK_EQ_I(A.phase, ANS_V34);
    CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    CHECK(A.bitrate >= 24000 && A.bitrate <= 33600);
    CHECK(A.rx_bitrate >= 24000 && A.rx_bitrate <= 33600);
    CHECK_EQ_I(A.bitrate, C.dx.rx_rate); CHECK_EQ_I(A.rx_bitrate, C.dx.tx_rate);
    UA.n = UA.bad = UC.n = UC.bad = 0;
    run(8000L * 3);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 0);
    CHECK(UA.n > 2L * A.rx_bitrate); CHECK(UC.n > 2L * A.bitrate);
    CHECK_EQ_I(UA.bad, 0); CHECK_EQ_I(UC.bad, 0);
    printf("  V.34 caller: %d bit/s to it, %d bit/s from it; %ld / %ld bits, %ld / %ld bad\n",
           A.bitrate, A.rx_bitrate, UC.n, UA.n, UC.bad, UA.bad);
    /* a retrain of ours: no data while it runs, data again after it */
    CHECK_EQ_I(answer_retrain(&A), 0);
    run(8000);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 1); CHECK_EQ_I(A.retrains, 1);
    for (t = 0; t < 15000 && A.retraining; t += 10) run(80);
    CHECK_EQ_I(A.retraining, 0); CHECK_EQ_I(A.phase, ANS_V34);
    /* our cleardown */
    CHECK_EQ_I(answer_cleardown(&A), 0);
    for (t = 0; t < 2000 && A.phase != ANS_FAILED; t += 10) run(80);
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_CLEARDOWN_OURS); CHECK_EQ_I(A.connected, 0);

    /* a click in data mode (Task 20): 0.75 ms of full scale on the line
     * to us, 1 s into data mode. From 10 s after it the upstream is clean,
     * or the call has ended */
    start(ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21, 0, 0);
    CHECK_EQ_I(to_data(25000), 1);
    L.c.click_at = L.n + 8000;
    run(8000L * 11);
    {
        long bad10 = UA.bad, n10 = UA.n;
        run(8000L * 4);
        printf("  V.34 caller, a click 1 s into data mode: %ld bits %ld bad from it, %ld bits %ld bad from 10 s after the click;"
               " %d retrains; end %d\n", UA.n, UA.bad, UA.n - n10, UA.bad - bad10, A.retrains, answer_end(&A));
        CHECK((UA.bad == bad10 && UA.n - n10 >= 3L * A.rx_bitrate) || answer_end(&A) != ANS_END_NONE);
    }

    /* ceilings: max_down is what we send, max_up what we take */
    start(ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21, 14400, 9600);
    CHECK_EQ_I(to_data(25000), 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    CHECK_EQ_I(A.bitrate, 14400); CHECK_EQ_I(A.rx_bitrate, 9600);

    /* V.34 without V.90 */
    start(V8_MOD_V34 | V8_MOD_V21, 0, 0);
    CHECK_EQ_I(to_data(25000), 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V34);

    /* V.34 not offered: this caller has nothing else */
    start(ANS_MODE_V90 | V8_MOD_V22 | V8_MOD_V21, 0, 0);
    CHECK_EQ_I(to_data(25000), 0);
    CHECK(A.phase != ANS_V34); CHECK_EQ_I(A.connected, 0);
    T_REPORT("answer_v34");
}
