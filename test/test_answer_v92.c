/* The answering modem with V.92 enabled (ANS_MODE_V92), at its G.711
 * interface, against whole simulated callers (sim/v92call: V.8, Phase 2,
 * then V.92 with PCM upstream through sim/v92a and sim/v92chan, or V.90
 * through sim/p34a):
 *   - a V.92 caller that selects PCM upstream: data both ways, mu-law and
 *     A-law, a rate renegotiation, retrains from either side, cleardown from
 *     either side, the status getters;
 *   - a V.92 start-up that fails: the retrain of V.92 9.5.1.2 with bit 70 of
 *     INFO1d cleared, and V.90 from there;
 *   - a V.92 caller with PCM upstream switched off, a V.90 caller, a V.34
 *     caller: served as before; what changes on the wire for them is
 *     measured here (INFO0d bit 27 and its CRC, nothing else for a V.90 caller);
 *   - V.92 not enabled: a V.92 caller gets V.90. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../modem/ans_scheme.h"   /* ans_enter: a phase put in place by hand */
#include "../modem/info.h"
#include "../sim/v92call.h"
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

#define V90  (ANS_MODE_V90 | V8_MOD_V22 | V8_MOD_V21)
#define V92  (V90 | ANS_MODE_V92)
#define RECN (30 * 8000)

static answer_t A; static v92call_t C; static prbs_t UA, UC;
static uint8_t atx;                 /* the octet we send */
static int deaf;                    /* the caller no longer hears us: it gets the idle code */
static long now;
static uint8_t rec[2][RECN]; static int rec_to = -1;    /* what we sent, for comparisons */

typedef struct {
    unsigned modes; int alaw, echo_delay, max_down, max_up;     /* the answerer; echo_delay as answer_v90_line */
    int c_v92, c_pcm, dil_n, mute_pcm, shape; double noise;     /* the caller */
} scen_t;

static void start(const scen_t *s)
{
    answer_cfg ac; v92call_cfg cc;
    memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = s->modes; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    answer_init(&A, &ac);
    if (s->alaw) { p2d_cfg pc = A.p2cfg; pc.alaw = 1; answer_v90_cfg(&A, &pc); }
    answer_v90_line(&A, s->echo_delay, s->max_down, s->max_up);
    memset(&cc, 0, sizeof cc);
    cc.alaw = s->alaw; cc.v92 = s->c_v92; cc.pcm_up = s->c_pcm; cc.lapm = 1;
    cc.echo_gain = 0.1; cc.echo_delay = 21;
    cc.shape = s->shape; cc.up_noise = s->noise; cc.dil_n = s->dil_n; cc.mute_pcm = s->mute_pcm;
    cc.getbit = p_get; cc.putbit = p_put; cc.user = &UC;
    v92call_init(&C, &cc);
    atx = v90_pcm_octet(s->alaw, 0, 1); now = 0;
}

/* the caller behind a packet network (an ATA's jitter buffers): far_d
 * samples each way, so the echo of its line's hybrid (v92call's, 20 dB down)
 * comes back a round trip late; 0: no such delay */
#define FARN 4096
static int far_d; static uint8_t far_dn[FARN], far_up[FARN];

static void run(long samples)
{
    while (samples-- > 0) {
        uint8_t to_c = deaf ? v90_pcm_octet(A.p2cfg.alaw, 0, 1) : atx, from_c;
        if (rec_to >= 0 && now < RECN) rec[rec_to][now] = atx;
        if (far_d) { far_dn[now & (FARN - 1)] = to_c; to_c = far_dn[(now - far_d) & (FARN - 1)]; }
        from_c = v92call_tick(&C, to_c);
        if (far_d) { far_up[now & (FARN - 1)] = from_c; from_c = far_up[(now - far_d) & (FARN - 1)]; }
        atx = answer_tick_pcm(&A, from_c);
        now++;
    }
}

static int moving(void)
{
    if (!A.connected || A.retraining || !v92call_data(&C)) return 0;
    if (A.phase == ANS_V92) return A.u.v92.state == V92D_DATA;
    return A.phase == ANS_V90 && A.p34.state == P34_DATA;
}

static int to_data(int max_ms)
{
    long end = now + 8L * max_ms;
    while (now < end) {
        run(8);
        if (moving()) return 1;
        if (A.phase == ANS_FAILED || C.stage == VCL_FAILED || C.hung) return 0;
    }
    return 0;
}

/* three seconds of data, both ways, without an error */
static void data_ok(const char *what, int mod)
{
    run(400);                           /* what was in the pipes when the pumps (re)started */
    UA.n = UA.bad = UC.n = UC.bad = 0;
    run(8000L * 3);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 0);
    CHECK_EQ_I(A.modulation, mod);
    CHECK_EQ_I(A.phase, mod == ANS_MOD_V92 ? ANS_V92 : ANS_V90);
    CHECK(UA.n > 2L * A.rx_bitrate); CHECK(UC.n > 2L * A.bitrate);
    CHECK(UA.bad <= 7); CHECK(UC.bad <= 7);     /* the predictor takes 7 bits to lock */
    printf("  %-44s %s: %d bit/s down, %d bit/s up; %ld / %ld bits, %ld / %ld bad; SNR %d.%d dB, rtd %d\n",
           what, mod == ANS_MOD_V92 ? "V.92" : "V.90", A.bitrate, A.rx_bitrate, UC.n, UA.n, UC.bad, UA.bad,
           answer_snr_db10(&A) / 10, answer_snr_db10(&A) % 10, answer_rtd(&A));
}

static void failed_why(void)
{
    if (A.phase == ANS_FAILED || !A.connected)
        printf("    not in data mode at %.2f s: phase %d, p2 state %d err %d mode %d, v92 state %d err %d, p34 state %d err %d, caller stage %d\n",
               now / 8000.0, (int)A.phase, (int)A.p2.state, A.p2.error, A.p2.mode, (int)A.u.v92.state, A.u.v92.error,
               (int)A.p34.state, A.p34.error, (int)C.stage);
}

static int16_t t8_a(void *u, int16_t rx) { return answer_tick((answer_t *)u, rx); }
static int16_t t8_c(void *u, int16_t rx) { return v34peer_tick((v34peer_t *)u, rx); }

static void scen(scen_t *s, unsigned modes, int alaw, int c_v92, int c_pcm)
{
    memset(s, 0, sizeof *s);
    s->modes = modes; s->alaw = alaw; s->c_v92 = c_v92; s->c_pcm = c_pcm; s->echo_delay = -1;
}

int main(void)
{
    scen_t s;
    long t;

    /* ---- 1. a V.92 caller, mu-law, echo delay not known (as on the box) ---- */
    scen(&s, V92, 0, 1, 1);
    start(&s);
    CHECK_EQ_I(answer_snr_db10(&A), -1); CHECK_EQ_I(answer_rtd(&A), -1);
    CHECK_EQ_I(to_data(30000), 1); failed_why();
    printf("  V.92 caller, mu-law: data mode at %.2f s\n", now / 8000.0);
    /* Phase 1 is V.90's (V.92 9.1): the same JM */
    CHECK_EQ_I(A.v8.selected, V8_SEL_V90);
    CHECK(A.v8.jm.has_pcm && (A.v8.jm.pcm & V8_PCM_DIGITAL));
    CHECK(A.v8.jm.has_access && (A.v8.jm.access & V8_ACC_DIGITAL));
    /* Phase 2 (9.3): both sides V.92, INFO1d of Table 17 with bit 70, INFO1a of Table 18 */
    CHECK_EQ_I(C.p2.info0d.v92, 1); CHECK_EQ_I(A.p2.info0a.v92, 1);
    CHECK_EQ_I(C.p2.info1d.sr[5].hi_carrier, 1);
    CHECK_EQ_I(A.p2.mode, P2D_MODE_V92); CHECK_EQ_I(A.v92_both, 1); CHECK_EQ_I(A.pcm_up, 1);
    CHECK_EQ_I(C.pcm_starts, 1); CHECK_EQ_I(C.v90_starts, 0);
    CHECK_EQ_I(A.phase, ANS_V92); CHECK_EQ_I(A.modulation, ANS_MOD_V92);
    CHECK_EQ_I(A.bitrate, 50666);
    CHECK(A.rx_bitrate > 33600 && A.rx_bitrate <= 48000);
    CHECK_EQ_I(A.rx_bitrate, C.va.up_rate);
    CHECK_EQ_I(A.retrains, 0);
    /* the echo delay was measured (the line has 21 samples) */
    CHECK(A.u.v92.ecd.delay >= 21 - 12 && A.u.v92.ecd.delay <= 21 + 2);
    data_ok("V.92 caller", ANS_MOD_V92);
    CHECK(answer_snr_db10(&A) >= 250 && answer_snr_db10(&A) <= 600);
    CHECK(answer_rtd(&A) >= 0 && answer_rtd(&A) < 80);
    CHECK_EQ_I(A.u.v92.dec.bad_frames, 0);
    /* the echo delay the stats give: the engine of the phase, not of the
     * modulation (after a retrain that withdrew PCM upstream, V.90's Phase 3
     * runs with the modulation still V.92) */
    CHECK_EQ_I(answer_echo_delay(&A), A.u.v92.ecd.delay);
    {
        static answer_t E;
        E = A;
        ans_enter(&E, ANS_P3); E.p34.ecd.delay = 123; E.u.v92.ecd.delay = 77;
        CHECK_EQ_I(answer_echo_delay(&E), 123);
        ans_enter(&E, ANS_RETRAIN);
        CHECK_EQ_I(answer_echo_delay(&E), 77);      /* between engines: the modulation's */
        ans_enter(&E, ANS_FAILED);
        CHECK_EQ_I(answer_echo_delay(&E), 77);
    }

    /* the caller renegotiates the rate (9.8): connected throughout, data again */
    CHECK_EQ_I(v92call_renegotiate(&C, 0), 0);
    run(800);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 0); CHECK_EQ_I(A.phase, ANS_V92);
    CHECK(!(A.u.v92.tx_ready && A.u.v92.rx_ready));
    CHECK_EQ_I(to_data(8000), 1);
    CHECK_EQ_I(A.u.v92.renegs, 1); CHECK_EQ_I(C.va.renegs, 1); CHECK_EQ_I(A.retrains, 0);
    CHECK_EQ_I(A.rx_bitrate, C.va.up_rate);
    data_ok("after the caller's rate renegotiation", ANS_MOD_V92);

    /* our retrain (9.7.1.1): silence, Tone B, Phase 2 of V.92 without INFO0, PCM upstream again */
    CHECK_EQ_I(answer_retrain(&A), 0);
    run(2000);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 1); CHECK_EQ_I(A.retrains, 1);
    CHECK_EQ_I(answer_retrain(&A), -1);                 /* not from here */
    CHECK_EQ_I(answer_cleardown(&A), -1);
    CHECK_EQ_I(to_data(20000), 1); failed_why();
    CHECK_EQ_I(C.retrains, 1); CHECK_EQ_I(C.pcm_starts, 2);
    CHECK_EQ_I(A.p2.n_info0d, 0);
    CHECK_EQ_I(C.p2.info1d.sr[5].hi_carrier, 1);        /* Table 17 again, bit 70 still set */
    data_ok("after our retrain", ANS_MOD_V92);

    /* the caller's retrain: its Tone A in data mode (9.7.1.2) */
    v92call_retrain(&C);
    run(2000);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 1); CHECK_EQ_I(A.retrains, 2);
    CHECK_EQ_I(to_data(20000), 1); failed_why();
    CHECK_EQ_I(C.pcm_starts, 3); CHECK_EQ_I(A.pcm_up, 1);
    data_ok("after the caller's retrain", ANS_MOD_V92);

    /* our cleardown (9.11) */
    CHECK_EQ_I(answer_cleardown(&A), 0);
    for (t = 0; t < 9000 && A.phase != ANS_FAILED; t += 10) run(80);     /* (the CPd design is paced) */
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_CLEARDOWN_OURS); CHECK_EQ_I(A.connected, 0);
    CHECK(C.va.clear_seen);
    CHECK_EQ_I(answer_snr_db10(&A), -1); CHECK_EQ_I(answer_rtd(&A), -1);

    /* ---- 2. A-law, a DIL, a long line, the echo delay given; the caller clears down ---- */
    scen(&s, V92, 1, 1, 1);
    s.dil_n = 40; s.shape = 1; s.echo_delay = 21;
    start(&s);
    CHECK_EQ_I(to_data(30000), 1); failed_why();
    CHECK_EQ_I(A.modulation, ANS_MOD_V92);
    CHECK_EQ_I(A.u.v92.ecd.delay, 21);
    CHECK_EQ_I(A.bitrate, 50666); CHECK(A.rx_bitrate >= 24000 && A.rx_bitrate <= 48000);
    data_ok("V.92 caller, A-law, DIL, long line", ANS_MOD_V92);
    CHECK_EQ_I(A.u.v92.dec.bad_frames, 0);
    CHECK_EQ_I(v92call_renegotiate(&C, 1), 0);
    for (t = 0; t < 9000 && A.phase != ANS_FAILED; t += 10) run(80);     /* (the CPd design is paced) */
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_CLEARDOWN_FAR); CHECK_EQ_I(A.connected, 0);

    /* ---- 3. rate ceilings hold: max_up applies to the PCM upstream ---- */
    scen(&s, V92, 0, 1, 1);
    s.max_down = 44000; s.max_up = 33600;
    start(&s);
    CHECK_EQ_I(to_data(30000), 1); failed_why();
    CHECK_EQ_I(A.modulation, ANS_MOD_V92);
    /* Jd (Table 21/V.92) names the downstream rates up to the ceiling: bits 0..12,
     * (21 + 12) * 8000 / 6 = 44000 (the simulated caller does not read them) */
    CHECK_EQ_I(A.u.v92.jd.rates, 0x1FFF);
    CHECK_EQ_I(A.rx_bitrate, 33333);
    data_ok("V.92 caller, ceilings 44000 / 33600", ANS_MOD_V92);
    /* ---- 3b. our cleardown when the caller no longer hears us (it goes on
     * sending data, but our Rd never reaches it, nor its Ru us): Rd,
     * TRN2d and SUVd go out all the same, no SUVu comes, and the call ends
     * as our cleardown at the cleardown's limit; no retrain ---- */
    deaf = 1;
    run(800);
    CHECK_EQ_I(answer_cleardown(&A), 0);
    {
        int retrain_seen = 0, retrains = A.retrains;
        for (t = 0; t < 8L * 20000 && A.phase != ANS_FAILED; t++) {
            run(1);
            if (A.phase == ANS_RETRAIN || A.phase == ANS_P2) retrain_seen = 1;
        }
        printf("  our cleardown, the caller deaf to us: over after %ld ms, end %d, retrain %d\n", t / 8, answer_end(&A), retrain_seen);
        CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_CLEARDOWN_OURS);
        CHECK_EQ_I(retrain_seen, 0); CHECK_EQ_I(A.retrains, retrains);
        CHECK(t < 8L * 4000);
    }
    deaf = 0;

    /* ---- 4. the V.92 start-up fails (the caller's Ru never arrives): the
     * recovery of 9.5.1.2 is a retrain; its INFO1d says "no PCM upstream"
     * (bit 70 = 0) and the call goes on as V.90 ---- */
    scen(&s, V92, 0, 1, 1);
    s.mute_pcm = 1;
    start(&s);
    for (t = 0; t < 30000 && A.phase != ANS_P3X && A.phase != ANS_FAILED; t += 10) run(80);
    CHECK_EQ_I(A.phase, ANS_P3X); CHECK_EQ_I(A.connected, 0);
    CHECK_EQ_I(to_data(40000), 1); failed_why();
    printf("  V.92 start-up without the caller's Ru: V.90 data mode at %.2f s, %d retrain(s)\n", now / 8000.0, A.retrains);
    CHECK_EQ_I(A.retrains, 1); CHECK_EQ_I(A.pcm_up, 0); CHECK_EQ_I(A.v92_both, 1);
    CHECK_EQ_I(C.pcm_starts, 1); CHECK_EQ_I(C.v90_starts, 1);
    CHECK_EQ_I(C.p2.info1d.sr[5].hi_carrier, 0);        /* bit 70 of Table 17 */
    CHECK_EQ_I(A.p2.mode, P2D_MODE_V90);
    CHECK_EQ_I(A.bitrate, 50666); CHECK(A.rx_bitrate >= 19200 && A.rx_bitrate <= 33600);
    data_ok("V.90 after the failed V.92 start-up", ANS_MOD_V90);
    /* and it stays that way through a later retrain */
    CHECK_EQ_I(answer_retrain(&A), 0);
    CHECK_EQ_I(to_data(20000), 1); failed_why();
    CHECK_EQ_I(C.pcm_starts, 1); CHECK_EQ_I(C.v90_starts, 2);
    data_ok("and after a retrain of that call", ANS_MOD_V90);

    /* ---- 5. a V.92 modem with PCM upstream switched off: V.90, with the
     * V.92 form of INFO1d (9.3) ---- */
    scen(&s, V92, 0, 1, 0);
    start(&s);
    CHECK_EQ_I(to_data(30000), 1); failed_why();
    CHECK_EQ_I(A.v92_both, 1); CHECK_EQ_I(A.p2.mode, P2D_MODE_V90);
    CHECK_EQ_I(C.p2.info1d.sr[5].hi_carrier, 1); CHECK_EQ_I(C.pcm_starts, 0);
    data_ok("V.92 caller, PCM upstream off", ANS_MOD_V90);
    CHECK(answer_snr_db10(&A) >= 200);

    /* ---- 6. a V.90 caller: with V.92 enabled here and without. The two
     * calls are the same simulation, so what we send can be compared octet
     * by octet: it differs while INFO0d is on the line and nowhere else ---- */
    {
        long k, ndiff = 0, first = -1, last = -1, p2_at[2], data_at[2];
        int up[2];
        for (rec_to = 0; rec_to < 2; rec_to++) {
            scen(&s, rec_to ? V92 : V90, 0, 0, 0);
            start(&s);
            for (t = 0; t < 30000 && A.phase != ANS_P2 && A.phase != ANS_FAILED; t += 1) run(8);
            p2_at[rec_to] = now;
            CHECK_EQ_I(to_data(30000), 1); failed_why();
            data_at[rec_to] = now; up[rec_to] = A.rx_bitrate;
            CHECK_EQ_I(C.p2.info0d.v92, rec_to);            /* INFO0d bit 27 as the caller received it */
            CHECK_EQ_I(A.v92_both, 0); CHECK_EQ_I(A.p2.mode, P2D_MODE_V90);
            CHECK_EQ_I(C.p2.info1d.sr[5].hi_carrier, 0);    /* INFO1d of V.90: our 3429 carrier bit */
            data_ok(rec_to ? "V.90 caller, V.92 enabled here" : "V.90 caller, V.92 not enabled", ANS_MOD_V90);
            run(RECN - now > 0 ? RECN - now : 0);
        }
        rec_to = -1;
        for (k = 0; k < RECN; k++)
            if (rec[0][k] != rec[1][k]) { ndiff++; if (first < 0) first = k; last = k; }
        printf("  V.90 caller, V.92 enabled against not enabled: %ld of %d octets sent differ, all between %.3f s and %.3f s "
               "(Phase 2 began at %.3f s; INFO0d is its first %d ms)\n",
               ndiff, RECN, first / 8000.0, last / 8000.0, p2_at[0] / 8000.0, INFO0D_BITS * 1000 / 600 + 1);
        CHECK_EQ_I(p2_at[0], p2_at[1]); CHECK_EQ_I(data_at[0], data_at[1]); CHECK_EQ_I(up[0], up[1]);
        CHECK(ndiff > 0);
        CHECK(first >= p2_at[0]);
        /* INFO0d: 62 bits at 600 bit/s, plus the tails of the transmit filter */
        CHECK(last < p2_at[0] + (INFO0D_BITS + 8) * 8000 / 600);
    }
    /* the bits: INFO0d with and without the V.92 bit differ in bit 27 and in the CRC (42:57) */
    {
        info0_t i; uint8_t b0[INFO_MAX_BITS], b1[INFO_MAX_BITS]; int k, n, other = 0, crc = 0;
        memset(&i, 0, sizeof i);
        i.lo3000 = i.hi3000 = i.lo3200 = i.hi3200 = 1; i.max_sr_diff = 5; i.tx_power = 6; i.max_tx_power = 23;
        n = info0d_build(&i, b0);
        i.v92 = 1;
        CHECK_EQ_I(info0d_build(&i, b1), n);
        for (k = 0; k < n; k++)
            if (b0[k] != b1[k]) { if (k >= 42 && k < 58) crc++; else if (k != 27) other++; }
        CHECK_EQ_I(b0[27], 0); CHECK_EQ_I(b1[27], 1); CHECK_EQ_I(other, 0); CHECK(crc > 0);
    }

    /* ---- 7. V.92 not enabled here: a V.92 caller that would like PCM
     * upstream gets V.90, and INFO0d does not carry the bit ---- */
    scen(&s, V90, 0, 1, 1);
    start(&s);
    CHECK_EQ_I(to_data(30000), 1); failed_why();
    CHECK_EQ_I(C.p2.info0d.v92, 0); CHECK_EQ_I(A.v92_both, 0);
    CHECK_EQ_I(C.p2.info1d.sr[5].hi_carrier, 0);
    CHECK_EQ_I(C.pcm_starts, 0); CHECK_EQ_I(C.v90_starts, 1);
    data_ok("V.92 caller, V.92 not enabled here", ANS_MOD_V90);

    /* ---- 8. a V.34 caller (no PCM in its CM): the JM and the call are what
     * they are without V.92 ---- */
    {
        static v34peer_t P; static line34_t L; static answer_t B;
        v8_menu_t jm[2]; int rate[2][2], k;
        for (k = 0; k < 2; k++) {
            answer_cfg ac; v34peer_cfg pc; line34_cfg lc;
            long end;
            memset(&lc, 0, sizeof lc);
            lc.law = 1; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
            memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
            memset(&ac, 0, sizeof ac);
            ac.level = 3000; ac.modes = (k ? V92 : V90) | V8_MOD_V34; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
            v34peer_default(&pc, 3000);
            pc.dx.getbit = p_get; pc.dx.putbit = p_put; pc.dx.user = &UC;
            answer_init(&B, &ac);
            answer_v90_line(&B, 0, 0, 0);
            v34peer_init(&P, &pc); line34_init(&L, &lc);
            end = 8000L * 30;
            while (L.n < end && !(B.connected && P.state == VP_DATA) && B.phase != ANS_FAILED && P.state != VP_ENDED) {
                (void)line34_step(&L, t8_a, &B, t8_c, &P);
            }
            CHECK_EQ_I(B.connected, 1); CHECK_EQ_I(B.phase, ANS_V34); CHECK_EQ_I(B.modulation, ANS_MOD_V34);
            jm[k] = B.v8.jm; rate[k][0] = B.bitrate; rate[k][1] = B.rx_bitrate;
            if (k) printf("  V.34 caller, V.92 enabled here: data mode at %.2f s, %d / %d bit/s\n", L.n / 8000.0, B.bitrate, B.rx_bitrate);
        }
        CHECK(memcmp(&jm[0], &jm[1], sizeof jm[0]) == 0);
        CHECK_EQ_I(rate[0][0], rate[1][0]); CHECK_EQ_I(rate[0][1], rate[1][1]);
    }

    /* ---- 9. the caller is gone in the middle of V.92 data (no cleardown):
     * the call must not stay "connected" on a dead line ---- */
    scen(&s, V92, 0, 1, 1);
    start(&s);
    CHECK_EQ_I(to_data(30000), 1); failed_why();
    run(8000);
    v92call_hangup(&C);
    for (t = 0; t < 60000 && A.phase != ANS_FAILED; t += 10) run(80);
    printf("  V.92 caller gone in data mode: over after %ld ms, %d retrain(s)\n", t, A.retrains);
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(A.connected, 0); CHECK(answer_end(&A) != ANS_END_CLEARDOWN_FAR && answer_end(&A) != ANS_END_CLEARDOWN_OURS);
    CHECK(t < 2000);

    /* ---- 10. the line goes dead in V.92 data (the caller hung up, the
     * network sends the idle code): what the canceller gives out is its own
     * estimate of our echo; the call ends as a carrier loss, in the half
     * second the carrier watch allows, with no rate renegotiation ---- */
    scen(&s, V92, 0, 1, 1);
    start(&s);
    CHECK_EQ_I(to_data(30000), 1); failed_why();
    run(8000);
    {
        int reneg = 0;
        for (t = 0; t < 8L * 60000 && A.phase != ANS_FAILED; t++) {
            atx = answer_tick_pcm(&A, 0xFF);
            if (A.phase == ANS_V92 && A.u.v92.state == V92D_RENEG) reneg = 1;
        }
        printf("  V.92 line dead in data mode: over after %ld ms, end %d, %d retrain(s), renegotiation %d\n",
               t / 8, answer_end(&A), A.retrains, reneg);
        CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_LOST);
        CHECK_EQ_I(reneg, 0); CHECK_EQ_I(A.retrains, 0);
        CHECK(t > 8L * 400 && t < 8L * 750);
    }

    /* ---- 11. the caller behind an ATA or a VoIP gateway (ISP calls of
     * 2026-11-23: a Cisco IOS FXS port's hybrid at 302 ms, 19.5 dB down):
     * the echo a round trip late, measured (as on the box). V.92 and V.90
     * find it near the round trip of Phase 2 and cancel it: the upstream as
     * clean as on a short path. And a round trip of 100 ms, inside the
     * search as it always was ---- */
    {
        static const int ms[3] = { 300, 400, 100 };
        int k, v;
        for (v = 0; v < 2; v++)
            for (k = 0; k < 3; k++) {
                char what[64];
                int e, d;
#ifdef TEST_SHORT
                if (k > 0) break;
#endif
                scen(&s, V92, 0, !v, !v);
                far_d = ms[k] * 4; e = 2 * far_d + 21;  /* (v92call's echo path: 21 samples) */
                memset(far_dn, v90_pcm_octet(0, 0, 1), sizeof far_dn); memset(far_up, v90_pcm_octet(0, 0, 1), sizeof far_up);
                start(&s);
                CHECK_EQ_I(to_data(40000), 1); failed_why();
                d = v ? A.p34.ecd.delay : A.u.v92.ecd.delay;
                snprintf(what, sizeof what, "far echo %d ms, 20 dB down (echo delay %d)", ms[k], d);
                data_ok(what, v ? ANS_MOD_V90 : ANS_MOD_V92);
                CHECK_EQ_I(A.retrains, 0);
                CHECK(d >= e - 12 && d <= e + 2);
                CHECK(answer_snr_db10(&A) > 300);      /* V.90: the upstream error below -30 dB */
                if (!v) CHECK_EQ_I(A.u.v92.dec.bad_frames, 0);
            }
        far_d = 0;
    }

    T_REPORT("answer_v92");
}
